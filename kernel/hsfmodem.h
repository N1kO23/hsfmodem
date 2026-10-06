/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * snd-hda-codec-hsfmodem: internal declarations.
 */
#ifndef HSFMODEM_H
#define HSFMODEM_H

#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <sound/hda_codec.h>
#include <sound/hwdep.h>
#include <sound/pcm.h>

#include <sound/hsfmodem.h>

#define HSFMODEM_EVENTS		64	/* queued events (power of two) */

struct hsfmodem {
	struct hda_codec *codec;
	struct hda_pcm *pcm;
	struct snd_hwdep *hwdep;

	/* stream tags handed out by the controller at prepare time */
	spinlock_t tag_lock;
	u8 tag[2];		/* indexed by SNDRV_PCM_STREAM_* */
	u32 generation;

	/* hwdep state, protected by 'lock' */
	struct mutex lock;
	bool open;
	bool clean_shutdown;
	bool powered;

	/* events for the daemon */
	spinlock_t ev_lock;
	wait_queue_head_t ev_wait;
	struct hsfmodem_event ev[HSFMODEM_EVENTS];
	unsigned int ev_head, ev_tail;	/* free-running indices */
	u32 ev_dropped;

	/* suspend handshake */
	struct notifier_block pm_nb;
	wait_queue_head_t pm_wait;
	bool pm_waiting;
	bool pm_acked;
	u32 pm_veto;
};

int hsfmodem_hwdep_create(struct hsfmodem *hm);
void hsfmodem_hwdep_free(struct hsfmodem *hm);
void hsfmodem_queue_event(struct hsfmodem *hm, u32 type, u32 data);
bool hsfmodem_daemon_attached(struct hsfmodem *hm);

#endif /* HSFMODEM_H */
