/* SPDX-License-Identifier: MIT */
/*
 * A stand-in for the Conexant HDA modem codec, for tests and development.
 *
 * It answers the identity parameters of a 14f1:2bfa codec, returns 0 for
 * every other verb, and simulates two free-running 16 kHz mono DMA rings.
 * That is enough for the engine and HAL to start, but not to open the
 * modem: the HAL polls vendor registers only a real modem answers. Use a
 * replay of a hardware recording for that.
 */
#include "hda_backend.h"

#include <stdlib.h>
#include <string.h>

#define BYTES_PER_SEC 32000u	/* 16 kHz, mono, S16_LE */

struct fake_stream {
	unsigned char tag;
	int bytes;
	unsigned char *buf;
};

struct fake {
	struct hsf_hda_backend b;
	struct fake_stream play, cap;
	bool running;
	uint64_t run_start_ns, base_bytes;
};

static unsigned int fake_read(struct hsf_hda_backend *b, unsigned int nid, int direct,
			      unsigned int verb, unsigned int param)
{
	(void)direct;
	if (verb == 0xF00) {		/* GET_PARAMETER */
		if (nid == 0) {
			switch (param) {
			case 0x00: return b->vendor_id;
			case 0x01: return b->subsystem_id;
			case 0x02: return b->revision_id;
			case 0x04: return (b->mfg_nid << 16) | 1;	/* one function group */
			}
		} else if (nid == b->mfg_nid && param == 0x05) {
			return 0x102;	/* modem function group, unsolicited capable */
		}
	} else if (verb == 0xF20) {	/* GET_SUBSYSTEM_ID */
		return b->subsystem_id;
	}
	return 0;
}

static unsigned int fake_wallclock(struct hsf_hda_backend *b)
{
	(void)b;
	return (unsigned int)(hsf_now_ns() * 3 / 125);	/* 24 MHz */
}

static int fake_open_dma(struct hsf_hda_backend *b, int bytes, void **play, void **cap)
{
	struct fake *f = (struct fake *)b;

	if (bytes <= 0 || f->play.buf)
		return -22;
	f->play.bytes = f->cap.bytes = bytes;
	f->play.buf = calloc(1, (size_t)bytes);
	f->cap.buf = calloc(1, (size_t)bytes);
	if (!f->play.buf || !f->cap.buf) {
		free(f->play.buf);
		free(f->cap.buf);
		f->play.buf = f->cap.buf = NULL;
		return -12;
	}
	f->running = false;
	f->base_bytes = 0;
	*play = &f->play;
	*cap = &f->cap;
	return 0;
}

static void fake_close_dma(struct hsf_hda_backend *b, void *play, void *cap)
{
	struct fake *f = (struct fake *)b;

	(void)play;
	(void)cap;
	free(f->play.buf);
	free(f->cap.buf);
	f->play.buf = f->cap.buf = NULL;
	f->running = false;
}

static void fake_dma_info(struct hsf_hda_backend *b, void *stream, unsigned char *tag,
			  unsigned long *fifo, unsigned short **buf)
{
	struct fake_stream *s = stream;

	(void)b;
	*tag = s->tag;
	*fifo = 0;		/* snd-hda-intel reports no FIFO size either */
	*buf = (unsigned short *)s->buf;
}

static uint64_t fake_bytes(struct fake *f)
{
	if (!f->running)
		return f->base_bytes;
	return f->base_bytes + (hsf_now_ns() - f->run_start_ns) * BYTES_PER_SEC / 1000000000u;
}

static int fake_set_dma_state(struct hsf_hda_backend *b, OSHDA_STREAM_STATE st, void *play, void *cap)
{
	struct fake *f = (struct fake *)b;

	(void)play;
	(void)cap;
	switch (st) {
	case OsHdaStreamStateRun:
		if (!f->running) {
			f->run_start_ns = hsf_now_ns();
			f->running = true;
		}
		break;
	case OsHdaStreamStateStop:
		f->base_bytes = fake_bytes(f);
		f->running = false;
		break;
	case OsHdaStreamStateReset:
		f->base_bytes = 0;
		f->running = false;
		break;
	}
	return 0;
}

static unsigned long fake_get_dma_pos(struct hsf_hda_backend *b, void *stream)
{
	struct fake_stream *s = stream;

	if (!s->bytes)
		return 0;
	return (unsigned long)(fake_bytes((struct fake *)b) % (unsigned)s->bytes) & ~1ul;
}

static int fake_start_events(struct hsf_hda_backend *b)
{
	(void)b;
	return 0;
}

static void fake_stop_events(struct hsf_hda_backend *b)
{
	(void)b;
}

static void fake_destroy(struct hsf_hda_backend *b)
{
	struct fake *f = (struct fake *)b;

	hsf_pci_unregister(f->b.pci_handle);
	free(f->play.buf);
	free(f->cap.buf);
	free(f);
}

static const struct hsf_hda_backend_ops fake_ops = {
	.read = fake_read,
	.wallclock = fake_wallclock,
	.open_dma = fake_open_dma,
	.close_dma = fake_close_dma,
	.dma_info = fake_dma_info,
	.set_dma_state = fake_set_dma_state,
	.get_dma_pos = fake_get_dma_pos,
	.start_events = fake_start_events,
	.stop_events = fake_stop_events,
	.destroy = fake_destroy,
};

struct hsf_hda_backend *hsf_hda_fake_new(void)
{
	static const uint8_t ich7_config[256] = {
		/* ICH7 HD-audio controller in a ThinkPad X/T/R60 */
		[0x00] = 0x86, [0x01] = 0x80, [0x02] = 0xd8, [0x03] = 0x27,
		[0x0a] = 0x03, [0x0b] = 0x04,
		[0x2c] = 0xaa, [0x2d] = 0x17, [0x2e] = 0x10, [0x2f] = 0x20,
	};
	struct fake *f = calloc(1, sizeof(*f));

	if (!f)
		return NULL;
	f->b.ops = &fake_ops;
	f->b.name = "fake";
	f->b.vendor_id = 0x14f12bfa;	/* Conexant HDA D330 MDC */
	f->b.subsystem_id = 0x17aa2150;
	f->b.revision_id = 0x00100000;
	f->b.codec_addr = 1;
	f->b.mfg_nid = 2;
	f->b.pci_handle = &f->b;
	f->play.tag = 1;
	f->cap.tag = 2;
	hsf_pci_register(f->b.pci_handle, ich7_config);
	return &f->b;
}
