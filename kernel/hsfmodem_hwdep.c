// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * snd-hda-codec-hsfmodem: the hwdep node used by hsfmodemd.
 *
 * One opener at a time. While it is open, the codec stays powered,
 * unsolicited responses and power events are queued for read(), and the
 * ioctls of include/uapi/sound/hsfmodem.h are available. If the node is
 * closed without HSFMODEM_IOCTL_CLEAN_SHUTDOWN (the daemon died), the modem
 * function is reset so it cannot keep the phone line off-hook.
 */

#include <linux/compat.h>
#include <linux/poll.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <sound/core.h>
#include <sound/hda_register.h>
#include <sound/hdaudio.h>

#include "hsfmodem.h"

static_assert(sizeof(struct hsfmodem_info) == 64);
static_assert(sizeof(struct hsfmodem_verb) == 24);
static_assert(sizeof(struct hsfmodem_stream_tags) == 16);
static_assert(sizeof(struct hsfmodem_pos) == 32);
static_assert(sizeof(struct hsfmodem_event) == 16);

bool hsfmodem_daemon_attached(struct hsfmodem *hm)
{
	return READ_ONCE(hm->open);
}

void hsfmodem_queue_event(struct hsfmodem *hm, u32 type, u32 data)
{
	struct hsfmodem_event *ev;
	unsigned long flags;

	spin_lock_irqsave(&hm->ev_lock, flags);
	if (hm->ev_head - hm->ev_tail >= HSFMODEM_EVENTS) {
		hm->ev_tail++;		/* drop the oldest */
		hm->ev_dropped++;
	}
	ev = &hm->ev[hm->ev_head++ % HSFMODEM_EVENTS];
	ev->type = type;
	ev->data = data;
	ev->timestamp_ns = ktime_get_ns();
	spin_unlock_irqrestore(&hm->ev_lock, flags);
	wake_up_interruptible(&hm->ev_wait);
}

static bool events_pending(struct hsfmodem *hm)
{
	unsigned long flags;
	bool pending;

	spin_lock_irqsave(&hm->ev_lock, flags);
	pending = hm->ev_head != hm->ev_tail || hm->ev_dropped;
	spin_unlock_irqrestore(&hm->ev_lock, flags);
	return pending;
}

/* Take one event; an overflow report comes first when events were dropped. */
static bool take_event(struct hsfmodem *hm, struct hsfmodem_event *out)
{
	unsigned long flags;
	bool got = true;

	spin_lock_irqsave(&hm->ev_lock, flags);
	if (hm->ev_dropped) {
		out->type = HSFMODEM_EVENT_OVERFLOW;
		out->data = hm->ev_dropped;
		out->timestamp_ns = ktime_get_ns();
		hm->ev_dropped = 0;
	} else if (hm->ev_head != hm->ev_tail) {
		*out = hm->ev[hm->ev_tail++ % HSFMODEM_EVENTS];
	} else {
		got = false;
	}
	spin_unlock_irqrestore(&hm->ev_lock, flags);
	return got;
}

static int hsfmodem_open(struct snd_hwdep *hw, struct file *file)
{
	struct hsfmodem *hm = hw->private_data;
	unsigned long flags;
	int err;

	guard(mutex)(&hm->lock);
	err = snd_hda_power_up(hm->codec);
	if (err < 0) {
		snd_hda_power_down(hm->codec);
		return err;
	}
	hm->powered = true;
	spin_lock_irqsave(&hm->ev_lock, flags);
	hm->ev_head = 0;
	hm->ev_tail = 0;
	hm->ev_dropped = 0;
	spin_unlock_irqrestore(&hm->ev_lock, flags);
	hm->clean_shutdown = false;
	WRITE_ONCE(hm->open, true);
	return 0;
}

static int hsfmodem_release(struct snd_hwdep *hw, struct file *file)
{
	struct hsfmodem *hm = hw->private_data;
	struct hda_codec *codec = hm->codec;

	guard(mutex)(&hm->lock);
	WRITE_ONCE(hm->open, false);
	if (!hm->clean_shutdown) {
		dev_warn(hda_codec_dev(codec),
			 "hsfmodemd exited without shutting down; resetting the modem function\n");
		snd_hda_codec_write(codec, codec->core.mfg, 0, AC_VERB_SET_CODEC_RESET, 0);
		snd_hda_codec_write(codec, codec->core.mfg, 0, AC_VERB_SET_POWER_STATE,
				    AC_PWRST_D3);
	}
	/* let a pending suspend proceed */
	WRITE_ONCE(hm->pm_veto, 0);
	WRITE_ONCE(hm->pm_acked, true);
	wake_up(&hm->pm_wait);
	if (hm->powered) {
		snd_hda_power_down(codec);
		hm->powered = false;
	}
	return 0;
}

static long hsfmodem_read(struct snd_hwdep *hw, char __user *buf, long count, loff_t *offset)
{
	struct hsfmodem *hm = hw->private_data;
	struct hsfmodem_event ev;
	long done = 0;

	if (count < (long)sizeof(ev))
		return -EINVAL;
	while (!events_pending(hm)) {
		/* snd_hwdep_read() does not pass the file; poll() first to avoid blocking */
		if (wait_event_interruptible(hm->ev_wait, events_pending(hm)))
			return -ERESTARTSYS;
	}
	while (count - done >= (long)sizeof(ev) && take_event(hm, &ev)) {
		if (copy_to_user(buf + done, &ev, sizeof(ev)))
			return done ? done : -EFAULT;
		done += sizeof(ev);
	}
	return done;
}

static __poll_t hsfmodem_poll(struct snd_hwdep *hw, struct file *file, poll_table *wait)
{
	struct hsfmodem *hm = hw->private_data;

	poll_wait(file, &hm->ev_wait, wait);
	return events_pending(hm) ? EPOLLIN | EPOLLRDNORM : 0;
}

static void fill_info(struct hsfmodem *hm, struct hsfmodem_info *info)
{
	struct hda_codec *codec = hm->codec;
	struct pci_dev *pci = codec->bus->pci;

	memset(info, 0, sizeof(*info));
	info->version = HSFMODEM_UAPI_VERSION;
	info->vendor_id = codec->core.vendor_id;
	info->subsystem_id = codec->core.subsystem_id;
	info->revision_id = codec->core.revision_id;
	info->codec_addr = codec->addr;
	info->mfg_nid = codec->core.mfg;
	info->card = codec->card->number;
	info->pcm_device = hm->pcm && hm->pcm->pcm ? hm->pcm->device : -1;
	if (pci) {
		info->pci_domain = pci_domain_nr(pci->bus);
		info->pci_bus = pci->bus->number;
		info->pci_devfn = pci->devfn;
	} else {
		info->pci_domain = 0xffffffff;
	}
}

static void sample_positions(struct hsfmodem *hm, struct hsfmodem_pos *pos)
{
	struct hdac_bus *bus = &hm->codec->bus->core;
	unsigned long flags;
	u8 tag[2];

	spin_lock_irqsave(&hm->tag_lock, flags);
	tag[0] = hm->tag[SNDRV_PCM_STREAM_PLAYBACK];
	tag[1] = hm->tag[SNDRV_PCM_STREAM_CAPTURE];
	spin_unlock_irqrestore(&hm->tag_lock, flags);

	memset(pos, 0, sizeof(*pos));
	local_irq_save(flags);
	pos->wallclk = snd_hdac_chip_readl(bus, WALLCLK);
	pos->ktime_ns = ktime_get_ns();
	for (int dir = 0; dir < 2; dir++) {
		struct hdac_stream *s;

		if (!tag[dir])
			continue;
		s = snd_hdac_get_stream(bus, dir, tag[dir]);
		/* only report streams that carry our modem PCM */
		if (!s || !s->substream || !hm->pcm || s->substream->pcm != hm->pcm->pcm)
			continue;
		pos->stream[dir].lpib = snd_hdac_stream_get_pos_lpib(s);
		pos->stream[dir].posbuf = s->posbuf ? snd_hdac_stream_get_pos_posbuf(s) : 0;
		pos->flags |= dir == SNDRV_PCM_STREAM_PLAYBACK ?
			      HSFMODEM_POS_PLAYBACK : HSFMODEM_POS_CAPTURE;
	}
	local_irq_restore(flags);
}

static int hsfmodem_ioctl(struct snd_hwdep *hw, struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct hsfmodem *hm = hw->private_data;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case HSFMODEM_IOCTL_VERSION:
		return put_user((u32)HSFMODEM_UAPI_VERSION, (u32 __user *)argp);

	case HSFMODEM_IOCTL_INFO: {
		struct hsfmodem_info info;

		fill_info(hm, &info);
		return copy_to_user(argp, &info, sizeof(info)) ? -EFAULT : 0;
	}

	case HSFMODEM_IOCTL_VERB: {
		struct hsfmodem_verb v;

		if (copy_from_user(&v, argp, sizeof(v)))
			return -EFAULT;
		if (v.flags || v.nid > 0x7f || v.verb > 0xfff || v.param > 0xff)
			return -EINVAL;
		v.response = snd_hda_codec_read(hm->codec, v.nid, 0, v.verb, v.param);
		return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
	}

	case HSFMODEM_IOCTL_STREAM_TAGS: {
		struct hsfmodem_stream_tags t = {};
		unsigned long flags;

		spin_lock_irqsave(&hm->tag_lock, flags);
		t.generation = hm->generation;
		t.playback = hm->tag[SNDRV_PCM_STREAM_PLAYBACK];
		t.capture = hm->tag[SNDRV_PCM_STREAM_CAPTURE];
		spin_unlock_irqrestore(&hm->tag_lock, flags);
		if (!t.playback || !t.capture)
			return -EAGAIN;
		return copy_to_user(argp, &t, sizeof(t)) ? -EFAULT : 0;
	}

	case HSFMODEM_IOCTL_POS: {
		struct hsfmodem_pos pos;

		sample_positions(hm, &pos);
		return copy_to_user(argp, &pos, sizeof(pos)) ? -EFAULT : 0;
	}

	case HSFMODEM_IOCTL_PM_ACK: {
		u32 veto;

		if (get_user(veto, (u32 __user *)argp))
			return -EFAULT;
		if (!READ_ONCE(hm->pm_waiting))
			return -EINVAL;
		WRITE_ONCE(hm->pm_veto, veto);
		WRITE_ONCE(hm->pm_acked, true);
		wake_up(&hm->pm_wait);
		return 0;
	}

	case HSFMODEM_IOCTL_CLEAN_SHUTDOWN:
		scoped_guard(mutex, &hm->lock)
			hm->clean_shutdown = true;
		return 0;
	}
	return -ENOIOCTLCMD;
}

#ifdef CONFIG_COMPAT
static int hsfmodem_ioctl_compat(struct snd_hwdep *hw, struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	/* every structure has the same layout for 32-bit callers */
	return hsfmodem_ioctl(hw, file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

int hsfmodem_hwdep_create(struct hsfmodem *hm)
{
	struct hda_codec *codec = hm->codec;
	struct snd_hwdep *hw;
	int device, err;

	/* the HDA core already uses device <codec address> for its own hwdep */
#ifdef CONFIG_SND_DYNAMIC_MINORS
	device = 16 + codec->addr;
#else
	device = codec->addr == 3 ? 2 : 3;
#endif
	err = snd_hwdep_new(codec->card, HSFMODEM_HWDEP_ID, device, &hw);
	if (err < 0)
		return err;
	strscpy(hw->name, "HSF Modem", sizeof(hw->name));
	hw->private_data = hm;
	hw->exclusive = 1;
	hw->ops.open = hsfmodem_open;
	hw->ops.release = hsfmodem_release;
	hw->ops.read = hsfmodem_read;
	hw->ops.poll = hsfmodem_poll;
	hw->ops.ioctl = hsfmodem_ioctl;
#ifdef CONFIG_COMPAT
	hw->ops.ioctl_compat = hsfmodem_ioctl_compat;
#endif
	hm->hwdep = hw;
	return 0;
}

void hsfmodem_hwdep_free(struct hsfmodem *hm)
{
	if (hm->hwdep) {
		snd_device_free(hm->codec->card, hm->hwdep);
		hm->hwdep = NULL;
	}
}
