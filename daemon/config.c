/* SPDX-License-Identifier: MIT */
#include "config.h"
#include "hsf_os.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_CONFIG "/etc/hsfmodem/hsfmodemd.conf"

enum {
	OPT_LINK = 0x100, OPT_DEV_LINK, OPT_CONTROL, OPT_NVM_STATIC, OPT_NVM_DYNAMIC, OPT_BIOS,
	OPT_REALTIME, OPT_NO_REALTIME, OPT_RECORD_POSITIONS, OPT_SYSLOG,
};

static const struct option options[] = {
	{ "config", required_argument, NULL, 'c' },
	{ "backend", required_argument, NULL, 'b' },
	{ "record-positions", no_argument, NULL, OPT_RECORD_POSITIONS },
	{ "link", required_argument, NULL, OPT_LINK },
	{ "dev-link", required_argument, NULL, OPT_DEV_LINK },
	{ "control", required_argument, NULL, OPT_CONTROL },
	{ "group", required_argument, NULL, 'g' },
	{ "user", required_argument, NULL, 'u' },
	{ "nvm-static", required_argument, NULL, OPT_NVM_STATIC },
	{ "nvm-dynamic", required_argument, NULL, OPT_NVM_DYNAMIC },
	{ "bios", required_argument, NULL, OPT_BIOS },
	{ "realtime", optional_argument, NULL, OPT_REALTIME },
	{ "no-realtime", no_argument, NULL, OPT_NO_REALTIME },
	{ "trace", optional_argument, NULL, 't' },
	{ "verbose", no_argument, NULL, 'v' },
	{ "quiet", no_argument, NULL, 'q' },
	{ "syslog", no_argument, NULL, OPT_SYSLOG },
	{ "help", no_argument, NULL, 'h' },
	{ "version", no_argument, NULL, 'V' },
	{ NULL, 0, NULL, 0 },
};

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: hsfmodemd [OPTION]...\n"
		"Run a Conexant HSF (HD-audio) softmodem as a serial port.\n"
		"\n"
		"  -c, --config=FILE        settings file (default " DEFAULT_CONFIG ")\n"
		"  -b, --backend=SPEC       alsa[:CARD] (default), record:FILE[:CARD],\n"
		"                           replay:FILE, fake, or sim (no engine)\n"
		"      --record-positions   also record DMA positions\n"
		"      --link=PATH          serial port link (default /run/hsfmodem/ttySHSF0)\n"
		"      --dev-link=PATH      static link to it, made as root (default /dev/ttySHSF0)\n"
		"      --control=PATH       status socket (default /run/hsfmodem/control)\n"
		"  -g, --group=NAME         group of the serial port (default dialout)\n"
		"  -u, --user=NAME          drop to this user after start-up\n"
		"      --nvm-static=DIR     modem configuration data\n"
		"      --nvm-dynamic=DIR    per-modem state written by the engine\n"
		"      --bios=SRC           BIOS image for the engine: auto, devmem, zeros, file:PATH\n"
		"      --realtime[=PRIO]    run modem threads SCHED_FIFO (default priority 40)\n"
		"      --no-realtime\n"
		"  -t, --trace[=LEVEL]      trace OS-layer calls (1, or 2 for frequent calls)\n"
		"  -v, --verbose            -q, --quiet\n"
		"      --syslog             log to syslog instead of standard error\n"
		"  -h, --help               -V, --version\n"
		"\n"
		"Every long option can also be set in the settings file as\n"
		"\"name = value\" (\"name = yes\" for options without a value).\n");
}

const char *hsf_backend_name(enum hsf_backend_kind k)
{
	static const char *const names[] = {
		[BACKEND_ALSA] = "alsa", [BACKEND_FAKE] = "fake", [BACKEND_REPLAY] = "replay",
		[BACKEND_RECORD] = "record", [BACKEND_SIM] = "sim",
	};

	return names[k];
}

/* Settings live as long as the process; keep their strings reachable. */
static char *keep(const char *s)
{
	static char *kept[128];
	static unsigned int nkept;
	char *copy;

	if (nkept == sizeof(kept) / sizeof(kept[0]))
		return NULL;
	copy = strdup(s);
	if (copy)
		kept[nkept++] = copy;
	return copy;
}

static bool parse_int(const char *s, int lo, int hi, int *out)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end || v < lo || v > hi)
		return false;
	*out = (int)v;
	return true;
}

static bool parse_backend(struct hsf_config *cfg, const char *spec)
{
	const char *colon = strchr(spec, ':');
	size_t n = colon ? (size_t)(colon - spec) : strlen(spec);
	char *file, *card;

	cfg->card = -1;
	cfg->backend_file = NULL;
	if (!strncmp(spec, "alsa", n) && n == 4) {
		cfg->backend = BACKEND_ALSA;
		return !colon || parse_int(colon + 1, 0, 31, &cfg->card);
	}
	if (!strcmp(spec, "fake")) {
		cfg->backend = BACKEND_FAKE;
		return true;
	}
	if (!strcmp(spec, "sim")) {
		cfg->backend = BACKEND_SIM;
		return true;
	}
	if (!colon || !colon[1])
		return false;
	if (n == 6 && !strncmp(spec, "replay", n)) {
		cfg->backend = BACKEND_REPLAY;
		cfg->backend_file = keep(colon + 1);
		return cfg->backend_file != NULL;
	}
	if (n == 6 && !strncmp(spec, "record", n)) {
		cfg->backend = BACKEND_RECORD;
		file = keep(colon + 1);
		if (!file)
			return false;
		card = strrchr(file, ':');	/* record:FILE:CARD */
		if (card && card[1] && strspn(card + 1, "0123456789") == strlen(card + 1)) {
			*card = '\0';
			parse_int(card + 1, 0, 31, &cfg->card);
		}
		cfg->backend_file = file;
		return true;
	}
	return false;
}

/* Apply one option; arg may be NULL for options without a value. */
static int apply(struct hsf_config *cfg, int opt, const char *arg, const char *where)
{
	switch (opt) {
	case 'c':
		break;	/* handled before everything else */
	case 'b':
		if (!parse_backend(cfg, arg)) {
			fprintf(stderr, "hsfmodemd: %sinvalid backend '%s'\n", where, arg);
			return 1;
		}
		break;
	case OPT_RECORD_POSITIONS:
		cfg->record_positions = true;
		break;
	case OPT_LINK:
		cfg->link = arg;
		break;
	case OPT_DEV_LINK:
		cfg->dev_link = arg;
		break;
	case OPT_CONTROL:
		cfg->control = arg;
		break;
	case 'g':
		cfg->group = arg;
		break;
	case 'u':
		cfg->user = arg;
		break;
	case OPT_NVM_STATIC:
		cfg->nvm_static = arg;
		break;
	case OPT_NVM_DYNAMIC:
		cfg->nvm_dynamic = arg;
		break;
	case OPT_BIOS:
		if (strcmp(arg, "auto") && strcmp(arg, "devmem") && strcmp(arg, "zeros") && strncmp(arg, "file:", 5)) {
			fprintf(stderr, "hsfmodemd: %sinvalid BIOS source '%s'\n", where, arg);
			return 1;
		}
		cfg->bios = arg;
		break;
	case OPT_REALTIME:
		cfg->realtime = true;
		if (arg && !parse_int(arg, 1, 98, &cfg->rt_priority)) {
			fprintf(stderr, "hsfmodemd: %sinvalid real-time priority '%s'\n", where, arg);
			return 1;
		}
		break;
	case OPT_NO_REALTIME:
		cfg->realtime = false;
		break;
	case 't':
		cfg->trace = 1;
		if (arg && !parse_int(arg, 0, 2, &cfg->trace)) {
			fprintf(stderr, "hsfmodemd: %sinvalid trace level '%s'\n", where, arg);
			return 1;
		}
		break;
	case 'v':
		cfg->log_level = HSF_LOG_DEBUG;
		break;
	case 'q':
		cfg->log_level = HSF_LOG_WARN;
		break;
	case OPT_SYSLOG:
		cfg->syslog = true;
		break;
	case 'h':
		usage(stdout);
		return -1;
	case 'V':
		printf("hsfmodemd %s\n", HSF_VERSION);
		return -1;
	default:
		usage(stderr);
		return 1;
	}
	return 0;
}

static char *trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

static bool yes(const char *v)
{
	return !strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "on");
}

static int load_file(struct hsf_config *cfg, const char *path, bool must_exist)
{
	char line[1024], where[1100];
	unsigned int lineno = 0;
	FILE *f = fopen(path, "r");
	int err = 0;

	if (!f) {
		if (!must_exist && errno == ENOENT)
			return 0;
		fprintf(stderr, "hsfmodemd: %s: %s\n", path, strerror(errno));
		return 1;
	}
	while (!err && fgets(line, sizeof(line), f)) {
		char *hash = strchr(line, '#'), *eq, *key, *val;
		const struct option *o;

		lineno++;
		if (hash)
			*hash = '\0';
		key = trim(line);
		if (!*key)
			continue;
		snprintf(where, sizeof(where), "%s:%u: ", path, lineno);
		eq = strchr(key, '=');
		if (!eq) {
			fprintf(stderr, "hsfmodemd: %sexpected name = value\n", where);
			err = 1;
			break;
		}
		*eq = '\0';
		key = trim(key);
		val = trim(eq + 1);
		for (o = options; o->name && strcmp(o->name, key); o++)
			;
		if (!o->name || o->val == 'c' || o->val == 'h' || o->val == 'V') {
			fprintf(stderr, "hsfmodemd: %sunknown setting '%s'\n", where, key);
			err = 1;
			break;
		}
		if (o->has_arg == no_argument) {
			if (yes(val))
				err = apply(cfg, o->val, NULL, where);	/* "syslog = no" sets nothing */
		} else {
			char *copy = keep(val);

			if (!copy) {
				fprintf(stderr, "hsfmodemd: %stoo many settings\n", where);
				err = 1;
				break;
			}
			if (o->val == OPT_REALTIME && (yes(val) || !strcmp(val, "no")))
				cfg->realtime = yes(val);
			else
				err = apply(cfg, o->val, copy, where);
		}
	}
	fclose(f);
	return err;
}

int hsf_config_load(struct hsf_config *cfg, int argc, char **argv)
{
	bool explicit_config = false;
	int opt, err;

	*cfg = (struct hsf_config){
		.config_file = DEFAULT_CONFIG,
		.backend = BACKEND_ALSA,
		.card = -1,
		.link = "/run/hsfmodem/ttySHSF0",
		.dev_link = "/dev/ttySHSF0",
		.control = "/run/hsfmodem/control",
		.group = "dialout",
		.user = "",
		.nvm_static = HSF_NVM_STATIC_DIR,
		.nvm_dynamic = HSF_NVM_DYNAMIC_DIR,
		.bios = "auto",
		.rt_priority = 40,
		.log_level = HSF_LOG_INFO,
	};

	/* the settings file first, so the command line overrides it */
	opterr = 0;
	optind = 1;
	while ((opt = getopt_long(argc, argv, "c:b:g:u:t::vqhV", options, NULL)) != -1) {
		if (opt == 'c') {
			cfg->config_file = optarg;
			explicit_config = true;
		}
	}
	err = load_file(cfg, cfg->config_file, explicit_config);
	if (err)
		return err;

	opterr = 1;
	optind = 1;
	while ((opt = getopt_long(argc, argv, "c:b:g:u:t::vqhV", options, NULL)) != -1) {
		err = apply(cfg, opt, optarg, "");
		if (err)
			return err;
	}
	if (optind < argc) {
		fprintf(stderr, "hsfmodemd: unexpected argument '%s'\n", argv[optind]);
		return 1;
	}
	return 0;
}
