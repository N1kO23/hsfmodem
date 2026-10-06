/* SPDX-License-Identifier: MIT */
/*
 * The kernel/userspace structures must have one layout on i386 and x86_64,
 * so a 32-bit daemon works on a 64-bit kernel (the kernel checks the same
 * sizes in kernel/hsfmodem_hwdep.c).
 */
#include <stddef.h>
#include <sound/hsfmodem.h>
#include "../tap.h"

_Static_assert(sizeof(struct hsfmodem_info) == 64, "hsfmodem_info");
_Static_assert(sizeof(struct hsfmodem_verb) == 24, "hsfmodem_verb");
_Static_assert(sizeof(struct hsfmodem_stream_tags) == 16, "hsfmodem_stream_tags");
_Static_assert(sizeof(struct hsfmodem_pos) == 32, "hsfmodem_pos");
_Static_assert(offsetof(struct hsfmodem_pos, ktime_ns) == 8, "hsfmodem_pos.ktime_ns");
_Static_assert(offsetof(struct hsfmodem_pos, stream) == 16, "hsfmodem_pos.stream");
_Static_assert(sizeof(struct hsfmodem_event) == 16, "hsfmodem_event");
_Static_assert(offsetof(struct hsfmodem_event, timestamp_ns) == 8, "hsfmodem_event.timestamp_ns");

int main(void)
{
	ok(1, "uapi structures have the same layout on this architecture as on the other");
	is_int(HSFMODEM_IOCTL_VERB & 0xff, 0x82, "ioctl numbers are stable");
	return tap_done();
}
