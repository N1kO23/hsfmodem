/* SPDX-License-Identifier: MIT */
/*
 * The serial bridge: a pseudo-terminal in front of a modem port
 * (docs/SERIAL.md).
 */
#ifndef HSF_BRIDGE_H
#define HSF_BRIDGE_H

#include "port.h"

#include <sys/types.h>

struct hsf_bridge_config {
	const char *link;	/* symlink to the slave, replaced atomically */
	gid_t group;		/* slave group, or (gid_t)-1 to leave it */
	mode_t mode;		/* slave mode */
};

struct hsf_bridge_status {
	char tty[64];		/* current slave, /dev/pts/N */
	bool open;		/* some process holds the slave open */
	bool dtr;
	UINT32 lines;		/* COMCTRL_EVT_*S */
	unsigned long rx_bytes, tx_bytes, breaks, overruns, hangups;
};

struct hsf_bridge;

struct hsf_bridge *hsf_bridge_start(struct hsf_port *port, const struct hsf_bridge_config *cfg);
/* Stops the bridge: drops DTR if it is up, removes the link, closes the
 * terminal. The port's thread must still be running. */
void hsf_bridge_stop(struct hsf_bridge *b);
/* Frees a stopped bridge, once the port delivers no more events. */
void hsf_bridge_free(struct hsf_bridge *b);
void hsf_bridge_status(struct hsf_bridge *b, struct hsf_bridge_status *st);

#endif /* HSF_BRIDGE_H */
