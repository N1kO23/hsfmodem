/* SPDX-License-Identifier: MIT */
/*
 * Starting as root and running as an ordinary user.
 *
 * Root is needed only at start-up: the codec device, /dev/mem for the BIOS
 * image, /run and /dev links, real-time limits. Everything after that runs
 * as the configured user, which owns the NVM state and the serial port.
 */
#ifndef HSF_PRIVS_H
#define HSF_PRIVS_H

#include <stdbool.h>
#include <sys/types.h>

struct hsf_privs {
	bool root;		/* started as root */
	bool drop;		/* switch to uid/gid */
	const char *user;
	uid_t uid;
	gid_t gid;
};

/* Resolve the user; as root, raise the limits real-time threads need after
 * the switch. Returns 0 or 1 (reported). */
int hsf_privs_prepare(struct hsf_privs *p, const char *user, bool realtime, int rt_priority);
/* mkdir -p; the last component gets mode and the target user. */
int hsf_privs_mkdir(const struct hsf_privs *p, const char *path, mode_t mode);
/* Switch to the user for good. Returns 0 or 1 (reported). */
int hsf_privs_drop(const struct hsf_privs *p);

#endif /* HSF_PRIVS_H */
