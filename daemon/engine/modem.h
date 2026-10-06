/* SPDX-License-Identifier: MIT */
/*
 * The modem: the Conexant engine bound to one HD-audio backend.
 */
#ifndef HSF_MODEM_H
#define HSF_MODEM_H

#include "hda_backend.h"
#include "port.h"

struct hsf_modem {
	struct hsf_port port;
	OS_DEVNODE dev;
	HANDLE comctrl;
	struct hsf_hda_backend *backend;
	bool engine_started, open;
};

/* Start the engine and open the modem on the backend (already bound with
 * hsf_hda_bind). Returns 0 or the failing COM_STATUS / -errno. */
int hsf_modem_open(struct hsf_modem *m, struct hsf_hda_backend *b, int instance);
void hsf_modem_close(struct hsf_modem *m);
bool hsf_modem_carrier(const struct hsf_modem *m);

/* Run fn synchronously on the modem thread and wait for it. */
void hsf_modem_call(struct hsf_modem *m, void (*fn)(void *), void *arg);

/* Power management (docs/SERIAL.md): returns 0 or -EBUSY to veto. */
int hsf_modem_suspend(struct hsf_modem *m);
void hsf_modem_resume(struct hsf_modem *m);

/* The engine's last-call report (#UG) into buf; returns false on failure. */
bool hsf_modem_last_call(struct hsf_modem *m, char *buf, size_t n);

#endif /* HSF_MODEM_H */
