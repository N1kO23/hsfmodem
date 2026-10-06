/* SPDX-License-Identifier: MIT */
/*
 * A modem port as the serial bridge sees it: a byte stream with modem
 * control, served on one thread. The real implementation is the Conexant
 * engine's ComCtrl interface (engine/modem.c); the simulator (sim_port.c)
 * stands in for it in tests.
 */
#ifndef HSF_PORT_H
#define HSF_PORT_H

#include "hsf_os.h"

struct hsf_port {
	/* Called only from work run with run(). They never block. */
	unsigned int (*read)(struct hsf_port *p, void *buf, unsigned int n);
	unsigned int (*write)(struct hsf_port *p, const void *buf, unsigned int n);
	int (*control)(struct hsf_port *p, COMCTRL_CONTROL_CODE code, void *arg);

	/* Queue fn(arg) on the port's thread; storage stays valid until it ran.
	 * Returns false if storage is already queued. */
	bool (*run)(struct hsf_port *p, OSSCHED *storage, void (*fn)(void *), void *arg);

	/* Line states: COMCTRL_EVT_CTSS, _DSRS, _RLSDS and _RINGS. The port
	 * updates them before calling on_event; read them atomically. */
	UINT32 lines;

	/* Installed by the bridge. Called with each COMCTRL_EVT_* mask, from
	 * any thread and possibly in atomic context: it must not block. */
	void (*on_event)(void *ctx, UINT32 mask);
	void *on_event_ctx;
};

/* Apply an engine event mask to p->lines and forward it to on_event. */
void hsf_port_event(struct hsf_port *p, UINT32 mask);

/* The simulated modem (sim_port.c): AT commands, dialling, data loopback. */
struct hsf_port *hsf_sim_port_new(void);
void hsf_sim_port_destroy(struct hsf_port *p);

#endif /* HSF_PORT_H */
