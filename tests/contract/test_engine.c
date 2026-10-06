/* SPDX-License-Identifier: MIT */
/*
 * Contract test: the real Conexant engine and HDA blobs against our OS layer
 * and the fake, record and replay HD-audio backends.
 *
 * Without recorded responses from real hardware, ComCtrl_Open cannot succeed
 * (the HAL polls codec registers only a real modem answers). The test checks
 * everything up to that point: the engine starts and stops cleanly and
 * repeatedly, the device bring-up reaches the HAL, the engine writes its NVM
 * state, and teardown leaves no threads, timers, memory or invariant
 * violations behind. A recording of that dialogue must then replay without
 * divergence and produce the same engine state.
 */
#include "hda_backend.h"
#include "../tap.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static FILE *log_copy;

static void log_sink(int level, const char *line)
{
	if (log_copy)
		fprintf(log_copy, "%s\n", line);
	if (level <= HSF_LOG_WARN)
		printf("# %s\n", line);
}

static unsigned events_seen;

HSF_EXPORT static void port_event(PVOID ref, UINT32 mask)
{
	(void)ref;
	(void)mask;
	__atomic_add_fetch(&events_seen, 1, __ATOMIC_RELAXED);
}

HSF_EXPORT static COM_STATUS pm_control(struct tagOS_DEVNODE *dev,
					__shimcall__ COM_STATUS (*fn)(HANDLE, ePmeState),
					HANDLE hDevMgr, ePmeState state, ePmeState old)
{
	(void)dev;
	(void)old;
	return fn(hDevMgr, state);
}

static bool read_file(const char *dir, const char *name, char *buf, size_t n)
{
	char path[4096];
	FILE *f;
	size_t got;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "r");
	if (!f)
		return false;
	got = fread(buf, 1, n - 1, f);
	buf[got] = '\0';
	fclose(f);
	return true;
}

static void test_lifecycle(void)
{
	long base = hsf_live_allocations();
	int failures = 0;

	for (int i = 0; i < 20; i++)
		if (HsfEngineInit() != 0)
			failures++;
		else
			HsfEngineExit();
	is_int(failures, 0, "HsfEngineInit succeeds 20 times in a row");
	is_int(hsf_live_allocations() - base, 0, "init/exit cycles leak no OsAllocate blocks");
}

struct bringup {
	unsigned long verbs;
	char hardware_id[64];
};

/* The device bring-up of the legacy HDA glue, against backend b. */
static void device_bringup(struct hsf_hda_backend *b, const char *nvm_static, const char *dyn_dir,
			   struct bringup *out)
{
	static OS_DEVNODE dev;
	PORT_EVENT_HANDLER evh = { .pRef = NULL, .pfnCallback = port_event };
	HDAOSHAL *hal;
	long base = hsf_live_allocations();
	char inst_dir[4096], buf[256];
	HANDLE h;

	tap_diag("bring-up with the %s backend", b->name);
	mkdir(dyn_dir, 0700);
	hsf_nvm_set_dirs(nvm_static, dyn_dir);
	hsf_hda_bind(b);
	hal = hsf_hda_hal();
	ok(b->ops->start_events(b) == 0, "the backend starts its event source");

	ok(HsfEngineInit() == 0, "HsfEngineInit");
	memset(&dev, 0, sizeof(dev));
	dev.hwDev = hal;
	dev.hwDevLink = b->pci_handle;
	snprintf(dev.hwInstName, sizeof(dev.hwInstName), "HDA-%08x:%08x-%u",
		 OsHdaCodecGetVendorId(hal), OsHdaCodecGetSubsystemId(hal), OsHdaCodecGetAddr(hal));
	snprintf(dev.hwProfile, sizeof(dev.hwProfile), "hsfhda");
	dev.hwType = HW_TYPE_HDA;
	dev.hwIf = GetHwFuncs();
	dev.pmControl = pm_control;
	ok(dev.hwIf != NULL, "GetHwFuncs returns the HDA hardware interface");

	h = ComCtrl_Create();
	ok(h != NULL, "ComCtrl_Create");
	dev.hcomctrl = h;
	is_int(ComCtrl_Configure(h, COMCTRL_CONFIG_DEVICE_ID, &dev), COM_STATUS_SUCCESS, "configure the device node");
	is_int(ComCtrl_Configure(h, COMCTRL_CONFIG_EVENT_HANDLER, &evh), COM_STATUS_SUCCESS, "configure the event handler");

	unsigned long dma_opens = hsf_call_count("OsHdaCodecOpenDMA");
	COM_STATUS r = ComCtrl_Open(h);

	out->verbs = hsf_hda_verb_count();
	tap_diag("ComCtrl_Open returned %d after %lu codec verbs", (int)r, out->verbs);
	ok(r != COM_STATUS_SUCCESS, "without real codec responses the open fails cleanly");
	ok(out->verbs > 100, "the HAL drove the codec through OsHdaCodecRead (%lu verbs)", out->verbs);
	ok(hsf_call_count("OsHdaCodecOpenDMA") > dma_opens, "the HAL opened the DMA streams");
	/* the shared modem worker is created on first use and lives on */
	is_int(hsf_call_count("OsThreadCreate") - hsf_call_count("OsThreadDestroy") - (OsMdmThread != NULL), 0,
	       "the HAL thread was stopped again (only the modem worker remains)");
	is_int(hsf_periodic_timer_count(), 0, "every periodic timer was destroyed");

	if (r == COM_STATUS_SUCCESS)
		ComCtrl_Close(h);
	is_int(ComCtrl_Destroy(h), COM_STATUS_SUCCESS, "ComCtrl_Destroy");
	HsfEngineExit();
	b->ops->stop_events(b);
	hsf_hda_unbind();
	is_int(hsf_live_allocations() - base, 0, "the whole bring-up leaks no OsAllocate blocks");

	snprintf(inst_dir, sizeof(inst_dir), "%s/0-%s", dyn_dir, dev.hwInstName);
	ok(read_file(inst_dir, "HARDWARE_ID", out->hardware_id, sizeof(out->hardware_id)),
	   "the engine stored HARDWARE_ID");
	ok(read_file(inst_dir, "LICENSE_STATUS", buf, sizeof(buf)), "the engine stored LICENSE_STATUS");
	is_str(buf, "\"FREE (max 14.4kbps data only)\"\n", "without a license key the engine runs in FREE mode");
	ok(read_file(inst_dir, "COUNTRY_CODE", buf, sizeof(buf)) && !strcmp(buf, "00B5\n"),
	   "the default country (USA, 0xB5) was stored");
}

static unsigned count_lines(const char *path, const char *prefix)
{
	FILE *f = fopen(path, "r");
	char line[1024];
	unsigned n = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, prefix, strlen(prefix)))
			n++;
	fclose(f);
	return n;
}

static void test_backends(const char *nvm_static, const char *dyn_dir)
{
	struct bringup fake, rec, rep;
	struct hsf_hda_backend *b;
	char dir[4096], recording[4096];

	b = hsf_hda_fake_new();
	snprintf(dir, sizeof(dir), "%s/fake", dyn_dir);
	device_bringup(b, nvm_static, dir, &fake);
	/* deterministic for an empty BIOS window and the fake codec's identity */
	is_str(fake.hardware_id, "21CC3BB2\n", "HARDWARE_ID matches the reference value");
	b->ops->destroy(b);

	snprintf(recording, sizeof(recording), "%s/codec.rec", dyn_dir);
	b = hsf_hda_record_new(hsf_hda_fake_new(), recording, false);
	ok(b != NULL, "the record backend wraps the fake");
	if (!b)
		return;
	snprintf(dir, sizeof(dir), "%s/record", dyn_dir);
	device_bringup(b, nvm_static, dir, &rec);
	b->ops->destroy(b);
	is_int(count_lines(recording, "verb "), rec.verbs, "every verb was recorded");
	is_int(count_lines(recording, "pci "), 1, "the controller's PCI config was recorded");
	ok(count_lines(recording, "open ") > 0, "the DMA open was recorded");
	is_str(rec.hardware_id, fake.hardware_id, "recording does not change what the engine sees");

	b = hsf_hda_replay_new(recording);
	ok(b != NULL, "the replay backend loads the recording");
	if (!b)
		return;
	snprintf(dir, sizeof(dir), "%s/replay", dyn_dir);
	device_bringup(b, nvm_static, dir, &rep);
	is_int(hsf_hda_replay_divergences(b), 0, "the replay follows the recording verb for verb");
	is_int(rep.verbs, rec.verbs, "the replay issued as many verbs as the recording");
	is_str(rep.hardware_id, rec.hardware_id, "the replayed engine derives the same HARDWARE_ID");
	b->ops->destroy(b);
}

int main(void)
{
	struct hsf_os_config cfg = HSF_OS_CONFIG_DEFAULT;
	const char *nvm_static = getenv("HSF_TEST_NVM_STATIC");
	char dyn_dir[] = "/tmp/hsf-contract-XXXXXX", logpath[4096];

	if (!nvm_static) {
		printf("Bail out! HSF_TEST_NVM_STATIC is not set\n");
		return 1;
	}
	if (!mkdtemp(dyn_dir)) {
		printf("Bail out! mkdtemp: %s\n", strerror(errno));
		return 1;
	}
	snprintf(logpath, sizeof(logpath), "%s.log", dyn_dir);
	log_copy = fopen(logpath, "w");
	hsf_log_set_sink(log_sink);
	cfg.bios_source = "zeros";
	cfg.trace = getenv("HSF_TRACE") ? atoi(getenv("HSF_TRACE")) : 0;
	if (hsf_os_init(&cfg)) {
		printf("Bail out! OS layer init failed\n");
		return 1;
	}
	hsf_nvm_set_dirs(nvm_static, dyn_dir);
	hsf_fault_install();

	test_lifecycle();
	test_backends(nvm_static, dyn_dir);
	is_int(hsf_problem_count(), 0, "no invariant violations (atomic context, locks, FPU, memory)");

	hsf_os_shutdown();
	tap_diag("log: %s", logpath);
	if (log_copy)
		fclose(log_copy);
	return tap_done();
}
