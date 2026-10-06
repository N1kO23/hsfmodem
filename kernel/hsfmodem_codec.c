// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HD-audio codec driver for Conexant HSF ("HDA D330/D110 MDC") modems.
 *
 * The modem's signal processing runs in the hsfmodemd userspace daemon, as
 * slmodemd does for Si3054 modems. This driver only binds the modem codec,
 * exposes its sample stream as an ALSA modem PCM (16 kHz, mono, S16_LE) and
 * provides a hwdep node for the rest (see include/uapi/sound/hsfmodem.h).
 *
 * The daemon programs the codec's converters itself, using the stream tags
 * from the hwdep node, so the PCM callbacks deliberately leave the codec
 * alone.
 *
 * Based on the si3054 modem codec driver and the Linuxant HSF modem codec
 * driver (Copyright (c) 2005-2008 Linuxant inc., GPL).
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <sound/core.h>
#include <sound/pcm.h>

#include "hsfmodem.h"

/* ---- modem PCM --------------------------------------------------------- */

static int hsfmodem_pcm_open(struct hda_pcm_stream *info, struct hda_codec *codec,
			     struct snd_pcm_substream *substream)
{
	return snd_pcm_hw_constraint_single(substream->runtime, SNDRV_PCM_HW_PARAM_RATE, 16000);
}

static int hsfmodem_pcm_prepare(struct hda_pcm_stream *info, struct hda_codec *codec,
				unsigned int stream_tag, unsigned int format,
				struct snd_pcm_substream *substream)
{
	struct hsfmodem *hm = codec->spec;
	unsigned long flags;

	/* No snd_hda_codec_setup_stream(): the daemon programs the converter. */
	spin_lock_irqsave(&hm->tag_lock, flags);
	hm->tag[substream->stream] = stream_tag;
	hm->generation++;
	spin_unlock_irqrestore(&hm->tag_lock, flags);
	return 0;
}

static int hsfmodem_pcm_cleanup(struct hda_pcm_stream *info, struct hda_codec *codec,
				struct snd_pcm_substream *substream)
{
	struct hsfmodem *hm = codec->spec;
	unsigned long flags;

	spin_lock_irqsave(&hm->tag_lock, flags);
	hm->tag[substream->stream] = 0;
	hm->generation++;
	spin_unlock_irqrestore(&hm->tag_lock, flags);
	return 0;
}

static const struct hda_pcm_stream hsfmodem_pcm_stream = {
	.substreams = 1,
	.channels_min = 1,
	.channels_max = 1,
	.rates = SNDRV_PCM_RATE_16000,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.maxbps = 16,
	.ops = {
		.open = hsfmodem_pcm_open,
		.prepare = hsfmodem_pcm_prepare,
		.cleanup = hsfmodem_pcm_cleanup,
	},
};

static int hsfmodem_build_pcms(struct hda_codec *codec)
{
	struct hsfmodem *hm = codec->spec;
	struct hda_pcm *info;

	info = snd_hda_codec_pcm_new(codec, "HSF Modem");
	if (!info)
		return -ENOMEM;
	info->stream[SNDRV_PCM_STREAM_PLAYBACK] = hsfmodem_pcm_stream;
	info->stream[SNDRV_PCM_STREAM_CAPTURE] = hsfmodem_pcm_stream;
	info->stream[SNDRV_PCM_STREAM_PLAYBACK].nid = codec->core.mfg;
	info->stream[SNDRV_PCM_STREAM_CAPTURE].nid = codec->core.mfg;
	info->pcm_type = HDA_PCM_TYPE_MODEM;
	hm->pcm = info;
	return 0;
}

/* ---- unsolicited responses --------------------------------------------- */

static void hsfmodem_unsol_event(struct hda_codec *codec, unsigned int res)
{
	struct hsfmodem *hm = codec->spec;

	if (hsfmodem_daemon_attached(hm))
		hsfmodem_queue_event(hm, HSFMODEM_EVENT_UNSOL, res);
}

/* ---- system suspend handshake -----------------------------------------
 *
 * Userspace is frozen before devices suspend, so the daemon must hear about
 * a suspend from a PM notifier, while it can still put the modem to sleep
 * or refuse because a call is up.
 */
#define HSFMODEM_PM_TIMEOUT (5 * HZ)

static int hsfmodem_pm_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct hsfmodem *hm = container_of(nb, struct hsfmodem, pm_nb);
	long left;

	switch (action) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
		if (!hsfmodem_daemon_attached(hm))
			return NOTIFY_DONE;
		WRITE_ONCE(hm->pm_acked, false);
		WRITE_ONCE(hm->pm_veto, 0);
		WRITE_ONCE(hm->pm_waiting, true);
		hsfmodem_queue_event(hm, HSFMODEM_EVENT_SUSPEND, 0);
		left = wait_event_timeout(hm->pm_wait, READ_ONCE(hm->pm_acked),
					  HSFMODEM_PM_TIMEOUT);
		WRITE_ONCE(hm->pm_waiting, false);
		if (!left) {
			dev_warn(hda_codec_dev(hm->codec), "hsfmodemd did not answer the suspend request\n");
			return NOTIFY_DONE;
		}
		if (READ_ONCE(hm->pm_veto)) {
			dev_info(hda_codec_dev(hm->codec), "suspend refused: modem in use\n");
			return notifier_from_errno(-EBUSY);
		}
		return NOTIFY_OK;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		if (hsfmodem_daemon_attached(hm))
			hsfmodem_queue_event(hm, HSFMODEM_EVENT_RESUME, 0);
		return NOTIFY_OK;
	}
	return NOTIFY_DONE;
}

/* ---- driver ------------------------------------------------------------- */

static int hsfmodem_probe(struct hda_codec *codec, const struct hda_device_id *id)
{
	struct hsfmodem *hm;
	int err;

	if (!codec->core.mfg) {
		dev_err(hda_codec_dev(codec), "no modem function group\n");
		return -ENODEV;
	}
	hm = kzalloc(sizeof(*hm), GFP_KERNEL);
	if (!hm)
		return -ENOMEM;
	hm->codec = codec;
	spin_lock_init(&hm->tag_lock);
	spin_lock_init(&hm->ev_lock);
	mutex_init(&hm->lock);
	init_waitqueue_head(&hm->ev_wait);
	init_waitqueue_head(&hm->pm_wait);
	codec->spec = hm;

	err = hsfmodem_hwdep_create(hm);
	if (err < 0) {
		codec->spec = NULL;
		kfree(hm);
		return err;
	}
	hm->pm_nb.notifier_call = hsfmodem_pm_notify;
	register_pm_notifier(&hm->pm_nb);
	return 0;
}

static void hsfmodem_remove(struct hda_codec *codec)
{
	struct hsfmodem *hm = codec->spec;

	if (!hm)
		return;
	unregister_pm_notifier(&hm->pm_nb);
	hsfmodem_hwdep_free(hm);
	codec->spec = NULL;
	kfree(hm);
}

static const struct hda_codec_ops hsfmodem_codec_ops = {
	.probe = hsfmodem_probe,
	.remove = hsfmodem_remove,
	.build_pcms = hsfmodem_build_pcms,
	.unsol_event = hsfmodem_unsol_event,
};

static const struct hda_device_id hsfmodem_ids[] = {
	HDA_CODEC_ID(0x14f12bfa, "HSF HDA D330 MDC V.92 Modem"),
	HDA_CODEC_ID(0x14f12c06, "HSF HDA D110 MDC V.92 Modem"),
	{}
};
MODULE_DEVICE_TABLE(hdaudio, hsfmodem_ids);

static struct hda_codec_driver hsfmodem_driver = {
	.id = hsfmodem_ids,
	.ops = &hsfmodem_codec_ops,
};

module_hda_codec_driver(hsfmodem_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Conexant HSF HD-audio modem codec (userspace engine interface)");
