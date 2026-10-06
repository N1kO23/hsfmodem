/* SPDX-License-Identifier: MIT */
/*
 * The OsHdaCodec* function pointers the hsfhda blob imports, routed to the
 * active backend.
 *
 * The legacy kernel glue filled these pointers from the codec driver; the
 * blob reaches the codec through an HDAOSHAL whose hda_codec field we own.
 * Unsolicited responses reach the blob through the callback it registers;
 * the tag is the modem function group node, and only responses carrying it
 * (bits 31..26) are forwarded, as before.
 */
#include "hda_backend.h"

#include <stdlib.h>

static struct hsf_hda_backend *backend;
static HDAOSHAL hal;
static unsigned long verbs;

static pthread_mutex_t cb_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*unsol_observer)(void *ctx, uint32_t res);
static void *unsol_observer_ctx;
static void (*event_cb)(void *ctx, unsigned int res);
static void *event_ctx;
static unsigned char event_tag;

static struct hsf_hda_backend *be(PHDAOSHAL h)
{
	return h ? (struct hsf_hda_backend *)h->hda_codec : backend;
}

HSF_EXPORT static unsigned int glue_get_addr(PHDAOSHAL h)
{
	HSF_TRACE("OsHdaCodecGetAddr", "");
	return be(h)->codec_addr;
}

HSF_EXPORT static unsigned int glue_get_vendor(PHDAOSHAL h)
{
	return be(h)->vendor_id;
}

HSF_EXPORT static unsigned int glue_get_subsys(PHDAOSHAL h)
{
	return be(h)->subsystem_id;
}

HSF_EXPORT static unsigned int glue_get_rev(PHDAOSHAL h)
{
	return be(h)->revision_id;
}

HSF_EXPORT static unsigned int glue_read(PHDAOSHAL h, unsigned short nid, int direct, unsigned int verb,
					 unsigned int para)
{
	unsigned int res = be(h)->ops->read(be(h), nid, direct, verb, para);

	__atomic_add_fetch(&verbs, 1, __ATOMIC_RELAXED);
	HSF_TRACE_HOT("OsHdaCodecRead", "0x%02x, 0x%03x, 0x%02x) = (0x%08x", nid, verb, para, res);
	if (hsf_trace_level == 1)	/* verbs are the interesting part of a level-1 trace */
		hsf_log(HSF_LOG_DEBUG, "verb nid=0x%02x verb=0x%03x param=0x%02x -> 0x%08x", nid, verb, para, res);
	return res;
}

HSF_EXPORT static unsigned int glue_wallclock(PHDAOSHAL h)
{
	HSF_TRACE_HOT("OsHdaCodecWallclock", "");
	return be(h)->ops->wallclock(be(h));
}

HSF_EXPORT static void glue_set_event_cb(PHDAOSHAL h, void (*cb)(void *, unsigned int), void *ctx,
					 unsigned char *tag)
{
	pthread_mutex_lock(&cb_lock);
	event_cb = cb;
	event_ctx = ctx;
	event_tag = (unsigned char)be(h)->mfg_nid;
	*tag = event_tag;
	pthread_mutex_unlock(&cb_lock);
	HSF_TRACE("OsHdaCodecSetEventCallback", "%p, %p) = (tag %u", (void *)cb, ctx, *tag);
}

HSF_EXPORT static void glue_clear_event_cb(PHDAOSHAL h, unsigned char tag)
{
	(void)h;
	HSF_TRACE("OsHdaCodecClearEventCallback", "%u", tag);
	pthread_mutex_lock(&cb_lock);
	event_cb = NULL;
	event_ctx = NULL;
	event_tag = 0;
	pthread_mutex_unlock(&cb_lock);
}

HSF_EXPORT static int glue_open_dma(PHDAOSHAL h, int bytes, void **play, void **cap)
{
	int r = be(h)->ops->open_dma(be(h), bytes, play, cap);

	HSF_TRACE("OsHdaCodecOpenDMA", "%d) = (%d", bytes, r);
	return r;
}

HSF_EXPORT static void glue_close_dma(PHDAOSHAL h, void *play, void *cap)
{
	HSF_TRACE("OsHdaCodecCloseDMA", "%p, %p", play, cap);
	be(h)->ops->close_dma(be(h), play, cap);
}

HSF_EXPORT static void glue_dma_info(PHDAOSHAL h, void *stream, unsigned char *tag, unsigned long *fifo,
				     short unsigned int **buf)
{
	be(h)->ops->dma_info(be(h), stream, tag, fifo, buf);
	HSF_TRACE("OsHdaCodecDMAInfo", "%p) = (tag %u, fifo %lu, buffer %p", stream, *tag, *fifo, (void *)*buf);
}

HSF_EXPORT static int glue_set_dma_state(PHDAOSHAL h, OSHDA_STREAM_STATE st, void *play, void *cap)
{
	int r = be(h)->ops->set_dma_state(be(h), st, play, cap);

	HSF_TRACE("OsHdaCodecSetDMAState", "%s) = (%d",
		  st == OsHdaStreamStateRun ? "run" : st == OsHdaStreamStateStop ? "stop" : "reset", r);
	return r;
}

HSF_EXPORT static unsigned long glue_get_dma_pos(PHDAOSHAL h, void *stream)
{
	HSF_TRACE_HOT("OsHdaCodecGetDMAPos", "%p", stream);
	return be(h)->ops->get_dma_pos(be(h), stream);
}

/* The pointer variables imported by the blob (declared in oshda.h). */
__shimcall__ unsigned int (*OsHdaCodecGetAddr)(PHDAOSHAL);
__shimcall__ unsigned int (*OsHdaCodecGetVendorId)(PHDAOSHAL);
__shimcall__ unsigned int (*OsHdaCodecGetSubsystemId)(PHDAOSHAL);
__shimcall__ unsigned int (*OsHdaCodecGetRevisionId)(PHDAOSHAL);
__shimcall__ unsigned int (*OsHdaCodecRead)(PHDAOSHAL, unsigned short, int, unsigned int, unsigned int);
__shimcall__ unsigned int (*OsHdaCodecWallclock)(PHDAOSHAL);
__shimcall__ void (*OsHdaCodecSetEventCallback)(PHDAOSHAL, void (*)(void *, unsigned int), void *,
						unsigned char *);
__shimcall__ void (*OsHdaCodecClearEventCallback)(PHDAOSHAL, unsigned char);
__shimcall__ int (*OsHdaCodecOpenDMA)(PHDAOSHAL, int, void **, void **);
__shimcall__ void (*OsHdaCodecCloseDMA)(PHDAOSHAL, void *, void *);
__shimcall__ void (*OsHdaCodecDMAInfo)(PHDAOSHAL, void *, unsigned char *, unsigned long *, short unsigned int **);
__shimcall__ int (*OsHdaCodecSetDMAState)(PHDAOSHAL, OSHDA_STREAM_STATE, void *, void *);
__shimcall__ unsigned long (*OsHdaCodecGetDMAPos)(PHDAOSHAL, void *);

void hsf_hda_bind(struct hsf_hda_backend *b)
{
	backend = b;
	hal.hda_codec = b;
	hal.bInSuspendResume = 0;
	verbs = 0;
	OsHdaCodecGetAddr = glue_get_addr;
	OsHdaCodecGetVendorId = glue_get_vendor;
	OsHdaCodecGetSubsystemId = glue_get_subsys;
	OsHdaCodecGetRevisionId = glue_get_rev;
	OsHdaCodecRead = glue_read;
	OsHdaCodecWallclock = glue_wallclock;
	OsHdaCodecSetEventCallback = glue_set_event_cb;
	OsHdaCodecClearEventCallback = glue_clear_event_cb;
	OsHdaCodecOpenDMA = glue_open_dma;
	OsHdaCodecCloseDMA = glue_close_dma;
	OsHdaCodecDMAInfo = glue_dma_info;
	OsHdaCodecSetDMAState = glue_set_dma_state;
	OsHdaCodecGetDMAPos = glue_get_dma_pos;
}

void hsf_hda_unbind(void)
{
	backend = NULL;
	hal.hda_codec = NULL;
}

HDAOSHAL *hsf_hda_hal(void)
{
	return &hal;
}

unsigned long hsf_hda_verb_count(void)
{
	return __atomic_load_n(&verbs, __ATOMIC_RELAXED);
}

void hsf_hda_deliver_unsol(uint32_t res)
{
	void (*cb)(void *, unsigned int);
	void *ctx;
	unsigned char tag;

	pthread_mutex_lock(&cb_lock);
	cb = event_cb;
	ctx = event_ctx;
	tag = event_tag;
	if (unsol_observer)
		unsol_observer(unsol_observer_ctx, res);
	pthread_mutex_unlock(&cb_lock);
	if (!cb)
		return;
	if (((res >> 26) & 0x3f) != tag) {
		hsf_log(HSF_LOG_DEBUG, "ignoring unsolicited response 0x%08x (tag %u)", res, tag);
		return;
	}
	HSF_TRACE("hsf_hda_deliver_unsol", "0x%08x", res);
	cb(ctx, res);
}

void hsf_hda_set_unsol_observer(void (*fn)(void *ctx, uint32_t res), void *ctx)
{
	pthread_mutex_lock(&cb_lock);
	unsol_observer = fn;
	unsol_observer_ctx = ctx;
	pthread_mutex_unlock(&cb_lock);
}
