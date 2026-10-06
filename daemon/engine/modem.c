/* SPDX-License-Identifier: MIT */
/*
 * Engine glue: the OS_DEVNODE, the ComCtrl lifecycle and engine events.
 *
 * Bring-up follows the legacy kernel glue (HDA probe + serial port add):
 * HsfEngineInit, an OS_DEVNODE describing the codec with the blob's hardware
 * interface (GetHwFuncs), then ComCtrl_Create / Configure(DEVICE_ID) /
 * Configure(EVENT_HANDLER) / Open. As before, ComCtrl data transfers run as
 * work items on the shared modem thread (OsMdmThread); see docs/SERIAL.md.
 */
#include "modem.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

static struct hsf_modem *from_port(struct hsf_port *p)
{
	return (struct hsf_modem *)((char *)p - offsetof(struct hsf_modem, port));
}

/* May run in atomic context: no blocking. */
HSF_EXPORT static void engine_event(PVOID ref, UINT32 mask)
{
	struct hsf_modem *m = ref;

	if (mask & COMCTRL_EVT_RLSD)
		hsf_log(HSF_LOG_INFO, "carrier %s", (mask & COMCTRL_EVT_RLSDS) ? "detected" : "lost");
	hsf_port_event(&m->port, mask);
}

static bool powered(ePmeState s)
{
	return s == DEVMGR_D0 || s == DEVMGR_QUICK_D0;
}

/* hwInUse counts the engine's own D3 -> D0 transitions (legacy semantics). */
HSF_EXPORT static COM_STATUS pm_control(struct tagOS_DEVNODE *dev,
					__shimcall__ COM_STATUS (*fn)(HANDLE, ePmeState),
					HANDLE hDevMgr, ePmeState state, ePmeState old)
{
	COM_STATUS r;

	if (!powered(old) && powered(state)) {
		dev->hwInUse++;
		r = fn(hDevMgr, state);
		if (r != COM_STATUS_SUCCESS)
			dev->hwInUse--;
	} else if (dev->hwInUse && powered(old) && !powered(state)) {
		r = fn(hDevMgr, state);
		if (r == COM_STATUS_SUCCESS)
			dev->hwInUse--;
	} else {
		r = fn(hDevMgr, state);
	}
	HSF_TRACE("pmControl", "%d -> %d) = (%d, in use %d", (int)old, (int)state, (int)r, (int)dev->hwInUse);
	return r;
}

/* ---- port operations on the engine ---------------------------------------- */

static unsigned int port_read(struct hsf_port *p, void *buf, unsigned int n)
{
	UINT32 got = ComCtrl_Read(from_port(p)->comctrl, buf, n);

	if (got > INT32_MAX) {
		hsf_log(HSF_LOG_WARN, "ComCtrl_Read returned %d", (int)got);
		return 0;
	}
	return got;
}

static unsigned int port_write(struct hsf_port *p, const void *buf, unsigned int n)
{
	UINT32 put = ComCtrl_Write(from_port(p)->comctrl, (PVOID)buf, n);

	if (put > INT32_MAX) {
		hsf_log(HSF_LOG_WARN, "ComCtrl_Write returned %d", (int)put);
		return 0;
	}
	return put;
}

static int port_control(struct hsf_port *p, COMCTRL_CONTROL_CODE code, void *arg)
{
	return (int)ComCtrl_Control(from_port(p)->comctrl, code, arg);
}

static bool port_run(struct hsf_port *p, OSSCHED *storage, void (*fn)(void *), void *arg)
{
	POSTHRD t = hsf_modem_thread();

	(void)p;
	if (!t)
		return false;
	OsThreadScheduleInit(storage, fn, arg);
	return OsThreadSchedule(t, storage) > 0;
}

/* ---- synchronous calls on the modem thread ---------------------------------- */

struct sync_call {
	OSSCHED work;
	void (*fn)(void *);
	void *arg;
	pthread_mutex_t m;
	pthread_cond_t c;
	bool done;
};

static void sync_call_run(void *p)
{
	struct sync_call *sc = p;

	sc->fn(sc->arg);
	pthread_mutex_lock(&sc->m);
	sc->done = true;
	pthread_cond_signal(&sc->c);
	pthread_mutex_unlock(&sc->m);
}

void hsf_modem_call(struct hsf_modem *m, void (*fn)(void *), void *arg)
{
	struct sync_call sc = { .fn = fn, .arg = arg };
	POSTHRD t = hsf_modem_thread();

	(void)m;
	if (!t || hsf_thread_tid(t) == hsf_gettid()) {
		fn(arg);
		return;
	}
	pthread_mutex_init(&sc.m, NULL);
	pthread_cond_init(&sc.c, NULL);
	OsThreadScheduleInit(&sc.work, sync_call_run, &sc);
	OsThreadSchedule(t, &sc.work);
	pthread_mutex_lock(&sc.m);
	while (!sc.done)
		pthread_cond_wait(&sc.c, &sc.m);
	pthread_mutex_unlock(&sc.m);
	pthread_mutex_destroy(&sc.m);
	pthread_cond_destroy(&sc.c);
}

/* ---- lifecycle ---------------------------------------------------------------- */

int hsf_modem_open(struct hsf_modem *m, struct hsf_hda_backend *b, int instance)
{
	PORT_EVENT_HANDLER evh = { .pRef = m, .pfnCallback = engine_event };
	COM_STATUS r;
	int err;

	memset(m, 0, sizeof(*m));
	m->backend = b;
	/* DSR until the engine reports otherwise (legacy) */
	m->port.lines = COMCTRL_EVT_DSRS;
	m->port.read = port_read;
	m->port.write = port_write;
	m->port.control = port_control;
	m->port.run = port_run;

	err = HsfEngineInit();
	if (err) {
		hsf_log(HSF_LOG_ERR, "HsfEngineInit failed: %d", err);
		return err;
	}
	m->engine_started = true;

	m->dev.hwDev = hsf_hda_hal();
	m->dev.hwDevLink = b->pci_handle;
	m->dev.hwInstNum = instance;
	snprintf(m->dev.hwInstName, sizeof(m->dev.hwInstName), "HDA-%08x:%08x-%u",
		 b->vendor_id, b->subsystem_id, b->codec_addr);
	snprintf(m->dev.hwProfile, sizeof(m->dev.hwProfile), "hsfhda");
	m->dev.hwType = HW_TYPE_HDA;
	m->dev.hwIf = GetHwFuncs();
	m->dev.pmControl = pm_control;
	m->dev.osPageOffset = 0;	/* physical memory is identity-mapped */

	if (b->ops->start_events(b))
		hsf_log(HSF_LOG_WARN, "cannot start the unsolicited-event thread");

	m->comctrl = ComCtrl_Create();
	if (!m->comctrl) {
		hsf_log(HSF_LOG_ERR, "ComCtrl_Create failed");
		hsf_modem_close(m);
		return -ENOMEM;
	}
	m->dev.hcomctrl = m->comctrl;
	r = ComCtrl_Configure(m->comctrl, COMCTRL_CONFIG_DEVICE_ID, &m->dev);
	if (r == COM_STATUS_SUCCESS)
		r = ComCtrl_Configure(m->comctrl, COMCTRL_CONFIG_EVENT_HANDLER, &evh);
	if (r == COM_STATUS_SUCCESS)
		r = ComCtrl_Open(m->comctrl);
	if (r != COM_STATUS_SUCCESS) {
		hsf_log(HSF_LOG_ERR, "opening the modem failed (ComCtrl status %d, %lu codec verbs)",
			(int)r, hsf_hda_verb_count());
		hsf_modem_close(m);
		return (int)r;
	}
	m->open = true;
	hsf_log(HSF_LOG_INFO, "modem %s ready", m->dev.hwInstName);
	return 0;
}

void hsf_modem_close(struct hsf_modem *m)
{
	if (m->open) {
		ComCtrl_Close(m->comctrl);
		m->open = false;
	}
	if (m->comctrl) {
		ComCtrl_Destroy(m->comctrl);
		m->comctrl = NULL;
	}
	if (m->backend)
		m->backend->ops->stop_events(m->backend);
	if (m->engine_started) {
		HsfEngineExit();
		m->engine_started = false;
	}
}

bool hsf_modem_carrier(const struct hsf_modem *m)
{
	return __atomic_load_n(&m->port.lines, __ATOMIC_SEQ_CST) & COMCTRL_EVT_RLSDS;
}

/* ---- power management and monitoring ------------------------------------------ */

struct control_call {
	struct hsf_modem *m;
	COMCTRL_CONTROL_CODE code;
	COM_STATUS r;
};

static void control_on_modem_thread(void *arg)
{
	struct control_call *c = arg;

	c->r = ComCtrl_Control(c->m->comctrl, c->code, NULL);
}

int hsf_modem_suspend(struct hsf_modem *m)
{
	struct control_call c = { .m = m, .code = COMCTRL_CONTROL_SLEEP };

	if (!m->open)
		return 0;
	if (m->dev.hwInUse) {
		hsf_log(HSF_LOG_WARN, "refusing to suspend: the modem is in use");
		return -EBUSY;
	}
	hsf_modem_call(m, control_on_modem_thread, &c);
	if (c.r != COM_STATUS_SUCCESS)
		hsf_log(HSF_LOG_ERR, "ComCtrl SLEEP failed (%d)", (int)c.r);
	return 0;
}

void hsf_modem_resume(struct hsf_modem *m)
{
	struct control_call c = { .m = m, .code = COMCTRL_CONTROL_WAKEUP };

	if (!m->open)
		return;
	hsf_modem_call(m, control_on_modem_thread, &c);
	if (c.r != COM_STATUS_SUCCESS)
		hsf_log(HSF_LOG_ERR, "ComCtrl WAKEUP failed (%d)", (int)c.r);
}

struct monitor_call {
	struct hsf_modem *m;
	PORT_MONITOR_DATA data;
	COM_STATUS r;
};

static void monitor_on_modem_thread(void *arg)
{
	struct monitor_call *c = arg;

	c->r = ComCtrl_Monitor(c->m->comctrl, COMCTRL_MONITOR_POUND_UG, &c->data);
}

bool hsf_modem_last_call(struct hsf_modem *m, char *buf, size_t n)
{
	struct monitor_call c = { .m = m };

	if (!m->open || n == 0)
		return false;
	memset(buf, 0, n);
	c.data.dwSize = (UINT32)n;
	c.data.pBuf = buf;
	hsf_modem_call(m, monitor_on_modem_thread, &c);
	buf[n - 1] = '\0';
	return c.r == COM_STATUS_SUCCESS;
}
