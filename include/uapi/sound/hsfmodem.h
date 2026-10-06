/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Interface between snd-hda-codec-hsfmodem and the hsfmodemd daemon.
 *
 * The codec driver exposes the Conexant HDA modem as an ALSA modem PCM
 * (device 6) plus one hwdep node, identified by HSFMODEM_HWDEP_ID, which
 * carries everything a PCM cannot: codec verbs, stream tags, raw DMA
 * positions, the wall clock, unsolicited responses and power-management
 * handshakes. All fields are fixed width and laid out identically on i386 and
 * x86_64, so a 32-bit daemon works on a 64-bit kernel.
 */
#ifndef _UAPI_SOUND_HSFMODEM_H
#define _UAPI_SOUND_HSFMODEM_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define HSFMODEM_HWDEP_ID	"HSFMODEM"
#define HSFMODEM_UAPI_VERSION	1

struct hsfmodem_info {
	__u32 version;		/* HSFMODEM_UAPI_VERSION */
	__u32 vendor_id;	/* e.g. 0x14f12bfa */
	__u32 subsystem_id;
	__u32 revision_id;
	__u32 codec_addr;
	__u32 mfg_nid;		/* modem function group node */
	__s32 card;
	__s32 pcm_device;	/* ALSA PCM device of the modem stream, -1 if none */
	__u32 pci_domain;	/* HD-audio controller; 0xffffffff when not PCI */
	__u32 pci_bus;
	__u32 pci_devfn;
	__u32 reserved[5];
};

/*
 * One codec verb, as snd_hda_codec_read(): for 4-bit verbs (stream format,
 * amp gain) the high payload byte is in the low byte of 'verb'.
 */
struct hsfmodem_verb {
	__u32 nid;
	__u32 verb;
	__u32 param;
	__u32 flags;		/* must be 0 */
	__u32 response;		/* out */
	__u32 reserved;
};

/*
 * Stream tags of the prepared modem PCM. A tag is 0 while that direction is
 * not prepared; 'generation' changes on every prepare.
 */
struct hsfmodem_stream_tags {
	__u32 generation;
	__u8 playback;
	__u8 capture;
	__u8 pad[2];
	__u32 reserved[2];
};

#define HSFMODEM_POS_PLAYBACK	(1u << 0)	/* stream[0] is valid */
#define HSFMODEM_POS_CAPTURE	(1u << 1)	/* stream[1] is valid */

/*
 * DMA positions sampled together with the wall clock. lpib is the link
 * position (what the old driver used); posbuf is the DMA position buffer.
 */
struct hsfmodem_pos {
	__u32 wallclk;		/* 24 MHz controller wall clock */
	__u32 flags;		/* HSFMODEM_POS_* */
	__u64 ktime_ns;		/* CLOCK_MONOTONIC when sampled */
	struct {
		__u32 lpib;
		__u32 posbuf;
	} stream[2];		/* [0] playback, [1] capture, in bytes */
};

enum hsfmodem_event_type {
	HSFMODEM_EVENT_UNSOL = 1,	/* data: unsolicited response word */
	HSFMODEM_EVENT_SUSPEND = 2,	/* answer with HSFMODEM_IOCTL_PM_ACK */
	HSFMODEM_EVENT_RESUME = 3,
	HSFMODEM_EVENT_OVERFLOW = 4,	/* data: number of events dropped */
};

/* read() returns whole events. */
struct hsfmodem_event {
	__u32 type;
	__u32 data;
	__u64 timestamp_ns;	/* CLOCK_MONOTONIC */
};

#define HSFMODEM_IOCTL_VERSION		_IOR('H', 0x80, __u32)
#define HSFMODEM_IOCTL_INFO		_IOR('H', 0x81, struct hsfmodem_info)
#define HSFMODEM_IOCTL_VERB		_IOWR('H', 0x82, struct hsfmodem_verb)
#define HSFMODEM_IOCTL_STREAM_TAGS	_IOR('H', 0x83, struct hsfmodem_stream_tags)
#define HSFMODEM_IOCTL_POS		_IOR('H', 0x84, struct hsfmodem_pos)
/*
 * Reply to HSFMODEM_EVENT_SUSPEND: 0 allows the suspend, non-zero vetoes it
 * (for example while a call is up).
 */
#define HSFMODEM_IOCTL_PM_ACK		_IOW('H', 0x85, __u32)
/*
 * The daemon is closing on purpose. Without this, closing the node resets
 * the modem function so a crashed daemon cannot leave the line off-hook.
 */
#define HSFMODEM_IOCTL_CLEAN_SHUTDOWN	_IO('H', 0x86)

#endif /* _UAPI_SOUND_HSFMODEM_H */
