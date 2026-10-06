/* SPDX-License-Identifier: MIT */
/*
 * hsfmodemd: the Conexant HSF softmodem engine as a serial port.
 *
 * Start-up order matters: everything that needs root (the codec device, the
 * BIOS image, links in /run and /dev, real-time limits) happens before the
 * switch to the service user; the engine and the serial port run after it.
 */
#include "bridge.h"
#include "config.h"
#include "control.h"
#include "modem.h"
#include "privs.h"

#include <sound/hsfmodem.h>

#include <errno.h>
#include <grp.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <syslog.h>
#include <unistd.h>

#define LAST_CALL_SIZE 4096

struct daemon {
	struct hsf_config cfg;
	struct hsf_privs privs;
	bool os_started;
	gid_t group;
	struct hsf_hda_backend *backend, *alsa;
	struct hsf_modem modem;
	bool modem_open;
	struct hsf_port *sim;
	struct hsf_bridge *bridge;
	struct hsf_control *control;
};

static void syslog_sink(int level, const char *line)
{
	static const int prio[] = { LOG_ERR, LOG_WARNING, LOG_INFO, LOG_DEBUG };

	syslog(prio[level < 0 ? 0 : level > 3 ? 3 : level], "%s", line);
}

/* ---- the status socket ------------------------------------------------------- */

static void flush_nvm(void *arg)
{
	(void)arg;
	NVM_WriteFlushList(TRUE);
}

static void handle_request(void *ctx, const char *req, FILE *out)
{
	struct daemon *d = ctx;
	struct hsf_bridge_status st;

	if (!strcmp(req, "status")) {
		hsf_bridge_status(d->bridge, &st);
		fprintf(out, "backend=%s\n", hsf_backend_name(d->cfg.backend));
		if (d->modem_open) {
			const OS_DEVNODE *dev = &d->modem.dev;
			const struct hsf_hda_backend *b = d->backend;

			fprintf(out, "instance=%d\nname=%s\nprofile=%s\nrevision=%s\n", dev->hwInstNum,
				dev->hwInstName, dev->hwProfile, dev->hwRevision);
			fprintf(out, "codec=%08x:%08x rev %08x addr %u\n", b->vendor_id, b->subsystem_id,
				b->revision_id, b->codec_addr);
		}
		fprintf(out, "link=%s\ntty=%s\nopen=%d\ndtr=%d\n", d->cfg.link, st.tty, st.open, st.dtr);
		fprintf(out, "carrier=%d\ndsr=%d\ncts=%d\nring=%d\n", !!(st.lines & COMCTRL_EVT_RLSDS),
			!!(st.lines & COMCTRL_EVT_DSRS), !!(st.lines & COMCTRL_EVT_CTSS),
			!!(st.lines & COMCTRL_EVT_RINGS));
		fprintf(out, "rx_bytes=%lu\ntx_bytes=%lu\nhangups=%lu\nbreaks=%lu\noverruns=%lu\nproblems=%d\n",
			st.rx_bytes, st.tx_bytes, st.hangups, st.breaks, st.overruns, hsf_problem_count());
	} else if (!strcmp(req, "lastcall")) {
		char *buf = malloc(LAST_CALL_SIZE);

		if (d->modem_open && buf && hsf_modem_last_call(&d->modem, buf, LAST_CALL_SIZE)) {
			fputs(buf, out);
			if (*buf && buf[strlen(buf) - 1] != '\n')
				fputc('\n', out);
		} else {
			fputs("error not available\n", out);
		}
		free(buf);
	} else if (!strcmp(req, "flush-nvm")) {
		if (d->modem_open) {
			hsf_modem_call(&d->modem, flush_nvm, NULL);
			fputs("ok\n", out);
		} else {
			fputs("error not available\n", out);
		}
	} else {
		fputs("error unknown command\n", out);
	}
}

/* ---- power management (alsa backend) ------------------------------------------- */

static int pm_hook(void *ctx, unsigned int event)
{
	struct daemon *d = ctx;

	if (event == HSFMODEM_EVENT_SUSPEND)
		return hsf_modem_suspend(&d->modem) ? 1 : 0;
	if (event == HSFMODEM_EVENT_RESUME)
		hsf_modem_resume(&d->modem);
	return 0;
}

/* ---- start-up and shutdown -------------------------------------------------------- */

static int create_backend(struct daemon *d)
{
	const struct hsf_config *c = &d->cfg;

	switch (c->backend) {
	case BACKEND_ALSA:
		d->backend = d->alsa = hsf_hda_alsa_new(c->card);
		break;
	case BACKEND_RECORD:
		d->alsa = hsf_hda_alsa_new(c->card);
		d->backend = d->alsa ? hsf_hda_record_new(d->alsa, c->backend_file, c->record_positions) : NULL;
		if (d->alsa && !d->backend)
			d->alsa->ops->destroy(d->alsa);
		break;
	case BACKEND_REPLAY:
		d->backend = hsf_hda_replay_new(c->backend_file);
		break;
	case BACKEND_FAKE:
		d->backend = hsf_hda_fake_new();
		break;
	case BACKEND_SIM:
		return 0;
	}
	if (!d->backend) {
		if (c->backend == BACKEND_ALSA || c->backend == BACKEND_RECORD)
			hsf_log(HSF_LOG_ERR, "no modem found: is snd-hda-codec-hsfmodem loaded, and "
				"snd-hda-intel probing the modem slot (probe_mask)?");
		return 1;
	}
	hsf_log(HSF_LOG_INFO, "codec %08x:%08x rev %08x at address %u (%s backend)", d->backend->vendor_id,
		d->backend->subsystem_id, d->backend->revision_id, d->backend->codec_addr, d->backend->name);
	return 0;
}

/* Directories the service user writes in, made while still root. */
static int prepare_dirs(struct daemon *d)
{
	const char *paths[] = { d->cfg.link, d->cfg.control };
	char buf[PATH_MAX];

	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		if (!*paths[i])
			continue;
		snprintf(buf, sizeof(buf), "%s", paths[i]);
		if (hsf_privs_mkdir(&d->privs, dirname(buf), 0755))
			return 1;
	}
	if (hsf_privs_mkdir(&d->privs, d->cfg.nvm_dynamic, 0700))
		return 1;
	return 0;
}

/* After the switch, the serial port can only be given to our own groups. */
static void check_group(struct daemon *d)
{
	gid_t groups[256];
	int n;

	if (d->group == (gid_t)-1 || (d->privs.root && !d->privs.drop) || getegid() == d->group)
		return;
	n = getgroups(256, groups);
	for (int i = 0; i < n; i++)
		if (groups[i] == d->group)
			return;
	hsf_log(HSF_LOG_WARN, "not a member of group '%s': the serial port stays in its default group "
		"(add the service user to '%s')", d->cfg.group, d->cfg.group);
	d->group = (gid_t)-1;
}

/* /dev/ttySHSF0 -> /run/hsfmodem/ttySHSF0; only root may write /dev. */
static void make_dev_link(struct daemon *d)
{
	char tmp[PATH_MAX];

	if (!*d->cfg.dev_link || !d->privs.root)
		return;
	snprintf(tmp, sizeof(tmp), "%s.new", d->cfg.dev_link);
	unlink(tmp);
	if (symlink(d->cfg.link, tmp) || rename(tmp, d->cfg.dev_link)) {
		hsf_log(HSF_LOG_WARN, "cannot create %s: %s", d->cfg.dev_link, strerror(errno));
		unlink(tmp);
	}
}

static int start(struct daemon *d)
{
	struct hsf_os_config os = HSF_OS_CONFIG_DEFAULT;
	struct hsf_bridge_config bc = { .link = d->cfg.link, .mode = 0660 };
	int err;

	if (hsf_privs_prepare(&d->privs, d->cfg.user, d->cfg.realtime, d->cfg.rt_priority))
		return 1;
	os.trace = d->cfg.trace;
	os.realtime = d->cfg.realtime;
	os.rt_priority = d->cfg.rt_priority;
	os.bios_source = d->cfg.bios;
	if (hsf_os_init(&os))
		return 1;
	d->os_started = true;
	hsf_fault_install();
	if (*d->cfg.group) {
		struct group *gr = getgrnam(d->cfg.group);

		if (gr)
			d->group = gr->gr_gid;
		else
			hsf_log(HSF_LOG_WARN, "no group '%s': the serial port keeps its default group", d->cfg.group);
	}
	if (create_backend(d) || prepare_dirs(d))
		return 1;
	make_dev_link(d);
	hsf_nvm_set_dirs(d->cfg.nvm_static, d->cfg.nvm_dynamic);
	if (hsf_privs_drop(&d->privs))
		return 1;
	check_group(d);

	if (d->cfg.backend == BACKEND_SIM) {
		d->sim = hsf_sim_port_new();
		if (!d->sim)
			return 1;
		hsf_log(HSF_LOG_INFO, "simulated modem: no engine, no hardware");
	} else {
		hsf_hda_bind(d->backend);
		err = hsf_modem_open(&d->modem, d->backend, 0);
		if (err) {
			if (d->cfg.backend == BACKEND_FAKE)
				hsf_log(HSF_LOG_INFO, "the fake codec cannot answer the modem's vendor registers; "
					"use --backend=sim, or replay a hardware recording");
			return 1;
		}
		d->modem_open = true;
		if (d->alsa)
			hsf_hda_alsa_set_pm_hook(d->alsa, pm_hook, d);
	}

	bc.group = d->group;	/* resolved and checked above */
	d->bridge = hsf_bridge_start(d->sim ? d->sim : &d->modem.port, &bc);
	if (!d->bridge)
		return 1;
	if (*d->cfg.control) {
		d->control = hsf_control_start(d->cfg.control, d->group, handle_request, d);
		if (!d->control)
			return 1;
	}
	return 0;
}

static void stop(struct daemon *d)
{
	hsf_control_stop(d->control);
	d->control = NULL;
	if (d->bridge)
		hsf_bridge_stop(d->bridge);	/* drops DTR while the engine still runs */
	if (d->alsa)
		hsf_hda_alsa_set_pm_hook(d->alsa, NULL, NULL);
	if (d->modem_open || d->modem.engine_started) {
		hsf_modem_close(&d->modem);
		d->modem_open = false;
	}
	hsf_sim_port_destroy(d->sim);
	d->sim = NULL;
	hsf_bridge_free(d->bridge);
	d->bridge = NULL;
	if (d->backend) {
		hsf_hda_unbind();
		d->backend->ops->destroy(d->backend);	/* alsa: tells the driver we left cleanly */
		d->backend = d->alsa = NULL;
	}
	if (d->os_started)
		hsf_os_shutdown();
	d->os_started = false;
}

int main(int argc, char **argv)
{
	static struct daemon d = { .group = (gid_t)-1 };
	struct signalfd_siginfo si;
	sigset_t sigs;
	int r, sfd;

	r = hsf_config_load(&d.cfg, argc, argv);
	if (r)
		return r < 0 ? 0 : 1;
	hsf_log_set_level(d.cfg.log_level);
	if (d.cfg.syslog) {
		openlog("hsfmodemd", LOG_PID, LOG_DAEMON);
		hsf_log_set_sink(syslog_sink);
	}

	/* before any thread exists, so all of them inherit the mask */
	signal(SIGPIPE, SIG_IGN);
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGTERM);
	sigaddset(&sigs, SIGHUP);
	pthread_sigmask(SIG_BLOCK, &sigs, NULL);
	sfd = signalfd(-1, &sigs, SFD_CLOEXEC);
	if (sfd < 0) {
		hsf_log(HSF_LOG_ERR, "signalfd: %s", strerror(errno));
		return 1;
	}

	hsf_log(HSF_LOG_INFO, "hsfmodemd %s starting", HSF_VERSION);
	if (start(&d)) {
		hsf_log(HSF_LOG_ERR, "start-up failed");
		stop(&d);
		return 1;
	}
	hsf_log(HSF_LOG_INFO, "ready: %s", d.cfg.link);

	while (read(sfd, &si, sizeof(si)) == sizeof(si)) {
		if (si.ssi_signo == SIGHUP) {
			hsf_log(HSF_LOG_INFO, "SIGHUP: nothing to reload; restart to apply new settings");
			continue;
		}
		hsf_log(HSF_LOG_INFO, "%s: shutting down", strsignal((int)si.ssi_signo));
		break;
	}
	r = hsf_problem_count();
	stop(&d);
	if (r)
		hsf_log(HSF_LOG_WARN, "%d invariant violations during this run (see the log)", r);
	close(sfd);
	return 0;
}
