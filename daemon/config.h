/* SPDX-License-Identifier: MIT */
/*
 * hsfmodemd settings: built-in defaults, then the configuration file, then
 * the command line.
 */
#ifndef HSF_CONFIG_H
#define HSF_CONFIG_H

#include <stdbool.h>

enum hsf_backend_kind { BACKEND_ALSA, BACKEND_FAKE, BACKEND_REPLAY, BACKEND_RECORD, BACKEND_SIM };

struct hsf_config {
	const char *config_file;
	enum hsf_backend_kind backend;
	int card;			/* alsa: -1 searches */
	const char *backend_file;	/* replay / record */
	bool record_positions;
	const char *link;		/* /run/hsfmodem/ttySHSF0 */
	const char *dev_link;		/* /dev/ttySHSF0, or "" */
	const char *control;		/* status socket, or "" */
	const char *group;		/* slave group, or "" */
	const char *user;		/* drop to this user, or "" */
	const char *nvm_static, *nvm_dynamic;
	const char *bios;
	bool realtime;
	int rt_priority;
	int trace;
	int log_level;
	bool syslog;
};

/* Fills cfg from defaults, the configuration file and argv. Returns 0, 1
 * for an error (already reported), or -1 when the program should exit
 * successfully (--help, --version). */
int hsf_config_load(struct hsf_config *cfg, int argc, char **argv);
const char *hsf_backend_name(enum hsf_backend_kind k);

#endif /* HSF_CONFIG_H */
