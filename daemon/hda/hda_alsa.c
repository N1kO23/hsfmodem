/* SPDX-License-Identifier: MIT */
/*
 * The real HD-audio modem, through snd-hda-codec-hsfmodem.
 *
 * Codec verbs, stream tags, DMA positions, the wall clock and unsolicited
 * responses go through the driver's hwdep node (include/uapi/sound/hsfmodem.h).
 * The sample streams are the driver's ALSA modem PCM, used through the raw
 * ALSA ioctls the way the old in-kernel driver used the PCM: both directions
 * mmap'd, linked so they start together, and free-running. start_threshold
 * and stop_threshold are the ring boundary and silence filling is off, so
 * ALSA never starts, stops or overwrites anything on its own; the blob reads
 * and writes the rings directly, guided by the link positions (LPIB).
 */
#include "hda_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <sound/asound.h>
#include <sound/hsfmodem.h>

#define RATE 16000
#define BYTES_PER_FRAME 2	/* S16_LE mono */

struct alsa_stream {
	int dir;		/* SNDRV_PCM_STREAM_* */
	int fd;
	void *area;
	size_t area_len;
	unsigned int bytes;
	unsigned char tag;
};

struct alsa_be {
	struct hsf_hda_backend b;
	int card, hwdep_dev, pcm_dev;
	int hwdep_fd;
	struct alsa_stream play, cap;
	bool streams_open, need_prepare;
	pthread_t event_thread;
	bool event_thread_running;
	int stop_fd;
	int (*pm_hook)(void *ctx, unsigned int event);
	void *pm_ctx;
	uint8_t pci_config[256];
};

/* ---- discovery ------------------------------------------------------------ */

static int find_hwdep(int want_card, int *card_out, int *dev_out)
{
	for (int card = 0; card < 32; card++) {
		char path[32];
		int fd, dev = -1;

		if (want_card >= 0 && card != want_card)
			continue;
		snprintf(path, sizeof(path), "/dev/snd/controlC%d", card);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		while (ioctl(fd, SNDRV_CTL_IOCTL_HWDEP_NEXT_DEVICE, &dev) == 0 && dev >= 0) {
			struct snd_hwdep_info info = { .device = (unsigned int)dev };

			if (ioctl(fd, SNDRV_CTL_IOCTL_HWDEP_INFO, &info) == 0 &&
			    !strcmp((const char *)info.id, HSFMODEM_HWDEP_ID)) {
				close(fd);
				*card_out = card;
				*dev_out = dev;
				return 0;
			}
		}
		close(fd);
	}
	return -ENODEV;
}

static void read_pci_config(struct alsa_be *a, const struct hsfmodem_info *info)
{
	char path[96];
	ssize_t n = -1;
	int fd;

	if (info->pci_domain == 0xffffffffu)
		return;
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%04x:%02x:%02x.%x/config",
		 info->pci_domain, info->pci_bus, info->pci_devfn >> 3, info->pci_devfn & 7);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		/* unprivileged readers only get the first 64 bytes */
		n = read(fd, a->pci_config, sizeof(a->pci_config));
		close(fd);
	}
	if (n < 64)
		hsf_log(HSF_LOG_WARN, "cannot read %s", path);
	hsf_pci_register(a->b.pci_handle, a->pci_config);
}

/* ---- hwdep ------------------------------------------------------------------ */

static unsigned int alsa_read(struct hsf_hda_backend *b, unsigned int nid, int direct,
			      unsigned int verb, unsigned int param)
{
	struct alsa_be *a = (struct alsa_be *)b;
	struct hsfmodem_verb v = { .nid = nid, .verb = verb, .param = param };

	(void)direct;
	if (ioctl(a->hwdep_fd, HSFMODEM_IOCTL_VERB, &v) < 0) {
		hsf_log(HSF_LOG_ERR, "codec verb nid=0x%02x verb=0x%03x param=0x%02x failed: %s",
			nid, verb, param, strerror(errno));
		return 0xffffffffu;
	}
	return v.response;
}

static int sample_pos(struct alsa_be *a, struct hsfmodem_pos *pos)
{
	if (ioctl(a->hwdep_fd, HSFMODEM_IOCTL_POS, pos) < 0) {
		hsf_log(HSF_LOG_ERR, "HSFMODEM_IOCTL_POS failed: %s", strerror(errno));
		memset(pos, 0, sizeof(*pos));
		return -errno;
	}
	return 0;
}

static unsigned int alsa_wallclock(struct hsf_hda_backend *b)
{
	struct hsfmodem_pos pos;

	sample_pos((struct alsa_be *)b, &pos);
	return pos.wallclk;
}

static unsigned long alsa_get_dma_pos(struct hsf_hda_backend *b, void *stream)
{
	struct alsa_be *a = (struct alsa_be *)b;
	struct alsa_stream *s = stream;
	struct hsfmodem_pos pos;
	unsigned int bit = s->dir == SNDRV_PCM_STREAM_PLAYBACK ? HSFMODEM_POS_PLAYBACK : HSFMODEM_POS_CAPTURE;

	if (sample_pos(a, &pos) || !(pos.flags & bit))
		return 0;
	return pos.stream[s->dir].lpib % s->bytes;
}

/* ---- PCM -------------------------------------------------------------------- */

static void hw_any(struct snd_pcm_hw_params *p)
{
	memset(p, 0, sizeof(*p));
	for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
		memset(p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits, 0xff,
		       sizeof(p->masks[0].bits));
	for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
		struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];

		i->min = 0;
		i->max = ~0u;
	}
	p->rmask = ~0u;
	p->info = ~0u;
}

static void hw_mask(struct snd_pcm_hw_params *p, int n, unsigned int bit)
{
	struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];

	memset(m->bits, 0, sizeof(m->bits));
	m->bits[bit >> 5] = 1u << (bit & 31);
}

static void hw_int(struct snd_pcm_hw_params *p, int n, unsigned int val)
{
	struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];

	i->min = val;
	i->max = val;
	i->integer = 1;
}

static unsigned int hw_get(const struct snd_pcm_hw_params *p, int n)
{
	return p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
}

static int stream_open(struct alsa_be *a, struct alsa_stream *s, int dir, unsigned int bytes)
{
	struct snd_pcm_hw_params hw;
	struct snd_pcm_sw_params sw;
	unsigned int frames = bytes / BYTES_PER_FRAME, period = bytes / 4;
	char path[48];
	long page = sysconf(_SC_PAGESIZE);
	int ver;

	s->dir = dir;
	s->bytes = bytes;
	snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%d%c", a->card, a->pcm_dev,
		 dir == SNDRV_PCM_STREAM_PLAYBACK ? 'p' : 'c');
	s->fd = open(path, O_RDWR | O_CLOEXEC);
	if (s->fd < 0) {
		hsf_log(HSF_LOG_ERR, "cannot open %s: %s", path, strerror(errno));
		return -errno;
	}
	if (ioctl(s->fd, SNDRV_PCM_IOCTL_PVERSION, &ver) < 0) {
		hsf_log(HSF_LOG_ERR, "%s: not an ALSA PCM", path);
		return -ENODEV;
	}

	hw_any(&hw);
	hw_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_MMAP_INTERLEAVED);
	hw_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	hw_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	hw_int(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 1);
	hw_int(&hw, SNDRV_PCM_HW_PARAM_RATE, RATE);
	hw_int(&hw, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, bytes);
	if (period >= 128 && period % 128 == 0)	/* snd-hda-intel wants 128-byte steps */
		hw_int(&hw, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, period);
	/* The legacy driver ignored a failure here; a ring of the wrong size
	 * would silently corrupt the modem's timing, so fail instead. */
	if (ioctl(s->fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
		hsf_log(HSF_LOG_ERR, "%s: cannot set 16 kHz mono S16_LE mmap with %u-byte buffer: %s",
			path, bytes, strerror(errno));
		return -errno;
	}
	if (hw_get(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE) != frames) {
		hsf_log(HSF_LOG_ERR, "%s: got a %u-frame buffer, wanted %u", path,
			hw_get(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE), frames);
		return -EINVAL;
	}

	/* First pass learns the boundary, second sets the free-running thresholds. */
	memset(&sw, 0, sizeof(sw));
	sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
	sw.period_step = 1;
	sw.avail_min = hw_get(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	sw.xfer_align = 1;
	sw.start_threshold = 1;
	sw.stop_threshold = frames;
	if (ioctl(s->fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0 || !sw.boundary) {
		hsf_log(HSF_LOG_ERR, "%s: SW_PARAMS failed: %s", path, strerror(errno));
		return -errno;
	}
	sw.start_threshold = sw.boundary;
	sw.stop_threshold = sw.boundary;
	sw.silence_threshold = 0;
	sw.silence_size = 0;
	if (ioctl(s->fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
		hsf_log(HSF_LOG_ERR, "%s: cannot make the stream free-running: %s", path, strerror(errno));
		return -errno;
	}

	s->area_len = ((size_t)bytes + (size_t)page - 1) & ~((size_t)page - 1);
	s->area = mmap(NULL, s->area_len, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd,
		       SNDRV_PCM_MMAP_OFFSET_DATA);
	if (s->area == MAP_FAILED) {
		s->area = NULL;
		hsf_log(HSF_LOG_ERR, "%s: cannot map the DMA buffer: %s", path, strerror(errno));
		return -errno;
	}
	memset(s->area, 0, bytes);
	return 0;
}

static void stream_close(struct alsa_stream *s)
{
	if (s->area)
		munmap(s->area, s->area_len);
	if (s->fd >= 0)
		close(s->fd);
	s->area = NULL;
	s->fd = -1;
	s->tag = 0;
}

static int fetch_tags(struct alsa_be *a)
{
	struct hsfmodem_stream_tags t;

	for (int tries = 0; tries < 10; tries++) {
		if (ioctl(a->hwdep_fd, HSFMODEM_IOCTL_STREAM_TAGS, &t) == 0) {
			if ((a->play.tag && a->play.tag != t.playback) || (a->cap.tag && a->cap.tag != t.capture))
				hsf_log(HSF_LOG_WARN, "stream tags changed (%u/%u -> %u/%u)",
					a->play.tag, a->cap.tag, t.playback, t.capture);
			a->play.tag = t.playback;
			a->cap.tag = t.capture;
			return 0;
		}
		if (errno != EAGAIN)
			break;
		usleep(1000);
	}
	hsf_log(HSF_LOG_ERR, "no stream tags from the driver: %s", strerror(errno));
	return -EIO;
}

static int prepare_streams(struct alsa_be *a)
{
	/* the streams are linked, so one PREPARE prepares both */
	if (ioctl(a->play.fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
		hsf_log(HSF_LOG_ERR, "PREPARE failed: %s", strerror(errno));
		return -errno;
	}
	a->need_prepare = false;
	return fetch_tags(a);
}

static int alsa_open_dma(struct hsf_hda_backend *b, int bytes, void **play, void **cap)
{
	struct alsa_be *a = (struct alsa_be *)b;
	int err;

	if (a->streams_open || bytes <= 0 || bytes % (2 * BYTES_PER_FRAME))
		return -EINVAL;
	a->play.fd = a->cap.fd = -1;
	err = stream_open(a, &a->play, SNDRV_PCM_STREAM_PLAYBACK, (unsigned int)bytes);
	if (!err)
		err = stream_open(a, &a->cap, SNDRV_PCM_STREAM_CAPTURE, (unsigned int)bytes);
	if (!err && ioctl(a->play.fd, SNDRV_PCM_IOCTL_LINK, a->cap.fd) < 0) {
		hsf_log(HSF_LOG_ERR, "cannot link playback and capture: %s", strerror(errno));
		err = -errno;
	}
	if (!err)
		err = prepare_streams(a);
	if (err) {
		stream_close(&a->play);
		stream_close(&a->cap);
		return err;
	}
	a->streams_open = true;
	*play = &a->play;
	*cap = &a->cap;
	hsf_log(HSF_LOG_INFO, "modem streams open: %d bytes each, tags %u/%u", bytes, a->play.tag, a->cap.tag);
	return 0;
}

static void alsa_close_dma(struct hsf_hda_backend *b, void *play, void *cap)
{
	struct alsa_be *a = (struct alsa_be *)b;

	(void)play;
	(void)cap;
	if (!a->streams_open)
		return;
	ioctl(a->play.fd, SNDRV_PCM_IOCTL_DROP);
	ioctl(a->play.fd, SNDRV_PCM_IOCTL_UNLINK);
	stream_close(&a->play);
	stream_close(&a->cap);
	a->streams_open = false;
}

static void alsa_dma_info(struct hsf_hda_backend *b, void *stream, unsigned char *tag,
			  unsigned long *fifo, unsigned short **buf)
{
	struct alsa_stream *s = stream;

	(void)b;
	*tag = s->tag;
	*fifo = 0;	/* what the legacy driver reported (runtime->hw.fifo_size) */
	*buf = s->area;
}

static int alsa_set_dma_state(struct hsf_hda_backend *b, OSHDA_STREAM_STATE st, void *play, void *cap)
{
	struct alsa_be *a = (struct alsa_be *)b;
	int err;

	(void)play;
	(void)cap;
	if (!a->streams_open)
		return -EINVAL;
	switch (st) {
	case OsHdaStreamStateRun:
		if (a->need_prepare) {
			err = prepare_streams(a);
			if (err)
				return err;
		}
		if (ioctl(a->play.fd, SNDRV_PCM_IOCTL_START) < 0) {
			hsf_log(HSF_LOG_ERR, "START failed: %s", strerror(errno));
			return -errno;
		}
		return 0;
	case OsHdaStreamStateStop:
	case OsHdaStreamStateReset:
		ioctl(a->play.fd, SNDRV_PCM_IOCTL_DROP);
		a->need_prepare = true;
		return 0;
	}
	return -ENOSYS;
}

/* ---- events -------------------------------------------------------------------- */

static void *event_main(void *arg)
{
	struct alsa_be *a = arg;
	struct pollfd pfd[2] = {
		{ .fd = a->hwdep_fd, .events = POLLIN },
		{ .fd = a->stop_fd, .events = POLLIN },
	};

	hsf_thread_enter("hda-events");
	for (;;) {
		struct hsfmodem_event ev[16];
		ssize_t n;

		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pfd[1].revents)
			break;
		if (!(pfd[0].revents & POLLIN))
			continue;
		n = read(a->hwdep_fd, ev, sizeof(ev));
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			hsf_log(HSF_LOG_ERR, "reading driver events: %s", strerror(errno));
			break;
		}
		for (ssize_t i = 0; i < n / (ssize_t)sizeof(ev[0]); i++) {
			switch (ev[i].type) {
			case HSFMODEM_EVENT_UNSOL:
				hsf_hda_deliver_unsol(ev[i].data);
				break;
			case HSFMODEM_EVENT_SUSPEND: {
				__u32 veto = a->pm_hook ? (__u32)a->pm_hook(a->pm_ctx, ev[i].type) : 0;

				if (ioctl(a->hwdep_fd, HSFMODEM_IOCTL_PM_ACK, &veto) < 0)
					hsf_log(HSF_LOG_WARN, "PM_ACK: %s", strerror(errno));
				break;
			}
			case HSFMODEM_EVENT_RESUME:
				a->need_prepare = true;	/* streams come back SUSPENDED */
				if (a->pm_hook)
					a->pm_hook(a->pm_ctx, ev[i].type);
				break;
			case HSFMODEM_EVENT_OVERFLOW:
				hsf_log(HSF_LOG_WARN, "driver dropped %u events", ev[i].data);
				break;
			}
		}
	}
	return NULL;
}

static int alsa_start_events(struct hsf_hda_backend *b)
{
	struct alsa_be *a = (struct alsa_be *)b;

	if (a->event_thread_running)
		return 0;
	if (hsf_thread_start(&a->event_thread, "hda-events", event_main, a))
		return -1;
	a->event_thread_running = true;
	return 0;
}

static void alsa_stop_events(struct hsf_hda_backend *b)
{
	struct alsa_be *a = (struct alsa_be *)b;
	uint64_t one = 1;

	if (!a->event_thread_running)
		return;
	(void)!write(a->stop_fd, &one, sizeof(one));
	pthread_join(a->event_thread, NULL);
	a->event_thread_running = false;
}

static void alsa_destroy(struct hsf_hda_backend *b)
{
	struct alsa_be *a = (struct alsa_be *)b;

	alsa_stop_events(b);
	alsa_close_dma(b, NULL, NULL);
	hsf_pci_unregister(a->b.pci_handle);
	if (a->hwdep_fd >= 0) {
		/* a deliberate exit: the driver need not reset the modem */
		ioctl(a->hwdep_fd, HSFMODEM_IOCTL_CLEAN_SHUTDOWN);
		close(a->hwdep_fd);
	}
	if (a->stop_fd >= 0)
		close(a->stop_fd);
	free(a);
}

void hsf_hda_alsa_set_pm_hook(struct hsf_hda_backend *b, int (*hook)(void *ctx, unsigned int event),
			      void *ctx)
{
	struct alsa_be *a = (struct alsa_be *)b;

	a->pm_hook = hook;
	a->pm_ctx = ctx;
}

static const struct hsf_hda_backend_ops alsa_ops = {
	.read = alsa_read,
	.wallclock = alsa_wallclock,
	.open_dma = alsa_open_dma,
	.close_dma = alsa_close_dma,
	.dma_info = alsa_dma_info,
	.set_dma_state = alsa_set_dma_state,
	.get_dma_pos = alsa_get_dma_pos,
	.start_events = alsa_start_events,
	.stop_events = alsa_stop_events,
	.destroy = alsa_destroy,
};

struct hsf_hda_backend *hsf_hda_alsa_new(int want_card)
{
	struct alsa_be *a = calloc(1, sizeof(*a));
	struct hsfmodem_info info;
	char path[32];
	__u32 version = 0;

	if (!a)
		return NULL;
	a->hwdep_fd = a->stop_fd = -1;
	a->play.fd = a->cap.fd = -1;
	if (find_hwdep(want_card, &a->card, &a->hwdep_dev)) {
		hsf_log(HSF_LOG_ERR, "no HSF modem found: is snd-hda-codec-hsfmodem loaded, and on a "
			"ThinkPad X/T/R60, snd_hda_intel.probe_mask=3 set?");
		goto fail;
	}
	snprintf(path, sizeof(path), "/dev/snd/hwC%dD%d", a->card, a->hwdep_dev);
	a->hwdep_fd = open(path, O_RDWR | O_CLOEXEC);
	if (a->hwdep_fd < 0) {
		hsf_log(HSF_LOG_ERR, "cannot open %s: %s", path, strerror(errno));
		goto fail;
	}
	if (ioctl(a->hwdep_fd, HSFMODEM_IOCTL_VERSION, &version) < 0 || version != HSFMODEM_UAPI_VERSION ||
	    ioctl(a->hwdep_fd, HSFMODEM_IOCTL_INFO, &info) < 0) {
		hsf_log(HSF_LOG_ERR, "%s: driver interface version %u, expected %u", path, version,
			HSFMODEM_UAPI_VERSION);
		goto fail;
	}
	if (info.pcm_device < 0) {
		hsf_log(HSF_LOG_ERR, "the driver has no modem PCM");
		goto fail;
	}
	a->stop_fd = eventfd(0, EFD_CLOEXEC);
	a->pcm_dev = info.pcm_device;
	a->b.ops = &alsa_ops;
	a->b.name = "alsa";
	a->b.vendor_id = info.vendor_id;
	a->b.subsystem_id = info.subsystem_id;
	a->b.revision_id = info.revision_id;
	a->b.codec_addr = info.codec_addr;
	a->b.mfg_nid = info.mfg_nid;
	a->b.pci_handle = &a->pci_config;
	read_pci_config(a, &info);
	hsf_log(HSF_LOG_INFO, "modem codec %08x (subsystem %08x) on card %d, codec %u, PCM device %d",
		info.vendor_id, info.subsystem_id, a->card, info.codec_addr, a->pcm_dev);
	return &a->b;
fail:
	if (a->hwdep_fd >= 0) {
		ioctl(a->hwdep_fd, HSFMODEM_IOCTL_CLEAN_SHUTDOWN);
		close(a->hwdep_fd);
	}
	free(a);
	return NULL;
}
