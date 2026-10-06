/* SPDX-License-Identifier: MIT */
#include "port.h"

void hsf_port_event(struct hsf_port *p, UINT32 mask)
{
	static const struct { UINT32 change, state; } line[] = {
		{ COMCTRL_EVT_CTS, COMCTRL_EVT_CTSS },
		{ COMCTRL_EVT_DSR, COMCTRL_EVT_DSRS },
		{ COMCTRL_EVT_RLSD, COMCTRL_EVT_RLSDS },
		{ COMCTRL_EVT_RING, COMCTRL_EVT_RINGS },
	};
	UINT32 st = __atomic_load_n(&p->lines, __ATOMIC_SEQ_CST);
	void (*cb)(void *, UINT32);

	/* each state bit is meaningful only alongside its change bit */
	for (size_t i = 0; i < sizeof(line) / sizeof(line[0]); i++)
		if (mask & line[i].change)
			st = (st & ~line[i].state) | (mask & line[i].state);
	__atomic_store_n(&p->lines, st, __ATOMIC_SEQ_CST);

	cb = __atomic_load_n(&p->on_event, __ATOMIC_SEQ_CST);
	if (cb)
		cb(__atomic_load_n(&p->on_event_ctx, __ATOMIC_SEQ_CST), mask);
}
