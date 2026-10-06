/* SPDX-License-Identifier: MIT */
/*
 * The HD-audio backends and the OsHdaCodec* glue, without the blobs: the
 * calls below are the ones the hsfhda blob makes.
 */
#include "hda_backend.h"
#include "../tap.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

static void quiet_sink(int level, const char *line)
{
	(void)level;
	(void)line;
}

static unsigned unsol_calls;
static unsigned int unsol_last;
static void *unsol_ctx_seen;

/* the blob's callback is a plain C function, not __shimcall__ (oshda.h) */
static void unsol_cb(void *ctx, unsigned int res)
{
	unsol_ctx_seen = ctx;
	unsol_last = res;
	__atomic_add_fetch(&unsol_calls, 1, __ATOMIC_RELEASE);
}

static bool wait_unsol(unsigned want)
{
	for (int i = 0; i < 200; i++) {
		if (__atomic_load_n(&unsol_calls, __ATOMIC_ACQUIRE) >= want)
			return true;
		usleep(5000);
	}
	return false;
}

static unsigned int verb(unsigned int nid, unsigned int v, unsigned int param)
{
	return OsHdaCodecRead(hsf_hda_hal(), (unsigned short)nid, 0, v, param);
}

/* ---- glue + fake -------------------------------------------------------- */

static void test_glue_fake(void)
{
	struct hsf_hda_backend *b = hsf_hda_fake_new();
	HDAOSHAL *hal;
	unsigned char tag = 0;
	int ctx;

	ok(b != NULL, "the fake backend is created");
	hsf_hda_bind(b);
	hal = hsf_hda_hal();
	is_int(OsHdaCodecGetVendorId(hal), 0x14f12bfa, "the vendor id is the Conexant modem codec");
	is_int(OsHdaCodecGetAddr(hal), 1, "the codec sits at address 1");
	is_int(verb(0, 0xF00, 0x00), 0x14f12bfa, "GET_PARAMETER(VENDOR_ID) on the root node");
	is_int(verb(0, 0xF00, 0x04), (2 << 16) | 1, "the root node has one function group, node 2");
	is_int(verb(2, 0xF00, 0x05) & 0xff, 0x02, "node 2 is a modem function group");
	is_int(hsf_hda_verb_count(), 3, "the glue counts verbs");

	OsHdaCodecSetEventCallback(hal, unsol_cb, &ctx, &tag);
	is_int(tag, 2, "the unsolicited-response tag is the function group node");
	hsf_hda_deliver_unsol((2u << 26) | 0x55);
	is_int(unsol_calls, 1, "a response carrying our tag reaches the callback");
	ok(unsol_last == ((2u << 26) | 0x55) && unsol_ctx_seen == &ctx,
	   "with the response and the registered context");
	hsf_hda_deliver_unsol((3u << 26) | 0x55);
	is_int(unsol_calls, 1, "a response carrying another tag is dropped");
	OsHdaCodecClearEventCallback(hal, tag);
	hsf_hda_deliver_unsol((2u << 26) | 0x55);
	is_int(unsol_calls, 1, "nothing is delivered after the callback is cleared");

	void *play = NULL, *cap = NULL;
	unsigned char tp = 0, tc = 0;
	unsigned long fifo = 1, p1, p2;
	unsigned short *bp = NULL, *bc = NULL;

	is_int(OsHdaCodecOpenDMA(hal, 4096, &play, &cap), 0, "OpenDMA succeeds");
	is_int(OsHdaCodecOpenDMA(hal, 4096, &play, &cap), -22, "a second OpenDMA is refused");
	OsHdaCodecDMAInfo(hal, play, &tp, &fifo, &bp);
	OsHdaCodecDMAInfo(hal, cap, &tc, &fifo, &bc);
	ok(tp == 1 && tc == 2, "the streams have tags 1 and 2");
	ok(bp && bc && bp != bc && fifo == 0, "each stream has its own buffer and no FIFO");
	is_int(OsHdaCodecGetDMAPos(hal, play), 0, "a stopped stream sits at 0");
	OsHdaCodecSetDMAState(hal, OsHdaStreamStateRun, play, cap);
	usleep(40000);
	p1 = OsHdaCodecGetDMAPos(hal, play);
	ok(p1 > 0 && p1 < 4096 && !(p1 & 1), "a running stream advances by whole samples (%lu)", p1);
	OsHdaCodecSetDMAState(hal, OsHdaStreamStateStop, play, cap);
	p1 = OsHdaCodecGetDMAPos(hal, play);
	usleep(10000);
	p2 = OsHdaCodecGetDMAPos(hal, play);
	is_int(p2, p1, "a stopped stream holds its position");
	OsHdaCodecSetDMAState(hal, OsHdaStreamStateReset, play, cap);
	is_int(OsHdaCodecGetDMAPos(hal, cap), 0, "a reset stream returns to 0");
	OsHdaCodecCloseDMA(hal, play, cap);

	hsf_hda_unbind();
	b->ops->destroy(b);
}

/* ---- record ---------------------------------------------------------------- */

static unsigned count_prefix(const char *path, const char *prefix, char *last, size_t n)
{
	FILE *f = fopen(path, "r");
	char line[1024];
	unsigned cnt = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, prefix, strlen(prefix)))
			continue;
		cnt++;
		if (last)
			snprintf(last, n, "%s", line);
	}
	fclose(f);
	return cnt;
}

static void test_record(const char *path)
{
	struct hsf_hda_backend *b = hsf_hda_record_new(hsf_hda_fake_new(), path, true);
	HDAOSHAL *hal;
	void *play, *cap;
	char line[1024];
	unsigned char tag;

	ok(b != NULL, "the record backend wraps the fake");
	if (!b)
		return;
	hsf_hda_bind(b);
	hal = hsf_hda_hal();
	is_int(OsHdaCodecGetSubsystemId(hal), 0x17aa2150, "the wrapper keeps the identity of the codec");
	OsHdaCodecSetEventCallback(hal, unsol_cb, NULL, &tag);
	verb(0, 0xF00, 0x00);
	verb(2, 0xF00, 0x05);
	verb(2, 0xF00, 0x05);
	hsf_hda_deliver_unsol((2u << 26) | 0x1);	/* the inner backend's event thread */
	hsf_hda_deliver_unsol((7u << 26) | 0x1);	/* recorded although not ours */
	verb(0x70, 0x7a0, 0x12);
	OsHdaCodecOpenDMA(hal, 2048, &play, &cap);
	OsHdaCodecSetDMAState(hal, OsHdaStreamStateRun, play, cap);
	OsHdaCodecGetDMAPos(hal, cap);
	OsHdaCodecSetDMAState(hal, OsHdaStreamStateStop, play, cap);
	OsHdaCodecCloseDMA(hal, play, cap);
	OsHdaCodecClearEventCallback(hal, tag);
	hsf_hda_unbind();
	b->ops->destroy(b);

	is_int(count_prefix(path, "info ", line, sizeof(line)), 1, "one info record");
	is_str(line, "info vendor=14f12bfa subsys=17aa2150 rev=00100000 addr=1 mfg=2\n",
	       "with the codec's identity");
	is_int(count_prefix(path, "pci 8680d827", NULL, 0), 1, "the controller's PCI config space");
	is_int(count_prefix(path, "verb ", line, sizeof(line)), 4, "every verb");
	ok(strstr(line, " nid=70 verb=7a0 param=12 res=00000000\n") != NULL, "with node, verb, parameter and response");
	is_int(count_prefix(path, "unsol ", line, sizeof(line)), 2, "every unsolicited response, ours or not");
	ok(strstr(line, " after=3 res=1c000001\n") != NULL, "with the verb count at its arrival");
	is_int(count_prefix(path, "open ", line, sizeof(line)), 1, "the DMA open");
	ok(strstr(line, " bytes=2048 tags=1/2 err=0\n") != NULL, "with size and stream tags");
	is_int(count_prefix(path, "state ", NULL, 0), 2, "stream state changes");
	is_int(count_prefix(path, "pos ", line, sizeof(line)), 1, "positions, when asked for");
	ok(strstr(line, " dir=cap ") != NULL, "by direction");
	is_int(count_prefix(path, "close ", NULL, 0), 1, "the DMA close");
}

/* ---- replay ---------------------------------------------------------------- */

static void test_replay_roundtrip(const char *path)
{
	struct hsf_hda_backend *b = hsf_hda_replay_new(path);
	HDAOSHAL *hal;
	void *play, *cap;
	unsigned char tag, tp, tc;
	unsigned long fifo;
	unsigned short *buf;
	unsigned int v;

	ok(b != NULL, "the recording loads");
	if (!b)
		return;
	hsf_hda_bind(b);
	hal = hsf_hda_hal();
	ok(OsHdaCodecGetVendorId(hal) == 0x14f12bfa && OsHdaCodecGetSubsystemId(hal) == 0x17aa2150 &&
	   OsHdaCodecGetRevisionId(hal) == 0x00100000 && OsHdaCodecGetAddr(hal) == 1,
	   "the replayed codec has the recorded identity");
	unsigned short did = 0;

	OsPciReadConfigw(b->pci_handle, 0x02, &did);
	is_int(did, 0x27d8, "the controller's PCI config is replayed");

	unsol_calls = 0;
	OsHdaCodecSetEventCallback(hal, unsol_cb, NULL, &tag);
	is_int(tag, 2, "the replayed function group provides the tag");
	ok(b->ops->start_events(b) == 0, "the replay event thread starts");
	is_int(verb(0, 0xF00, 0x00), 0x14f12bfa, "recorded responses are served");
	verb(2, 0xF00, 0x05);
	usleep(20000);
	is_int(unsol_calls, 0, "no unsolicited response before its recorded point");
	verb(2, 0xF00, 0x05);
	ok(wait_unsol(1), "the unsolicited response arrives after the third verb");
	usleep(20000);
	is_int(unsol_calls, 1, "responses carrying a foreign tag are still filtered");
	verb(0x70, 0x7a0, 0x12);
	is_int(hsf_hda_replay_divergences(b), 0, "the same dialogue does not diverge");

	is_int(OsHdaCodecOpenDMA(hal, 2048, &play, &cap), 0, "DMA opens");
	OsHdaCodecDMAInfo(hal, play, &tp, &fifo, &buf);
	OsHdaCodecDMAInfo(hal, cap, &tc, &fifo, &buf);
	ok(tp == 1 && tc == 2, "with the recorded stream tags");
	OsHdaCodecCloseDMA(hal, play, cap);

	v = verb(5, 0xF00, 0x09);
	is_int(v, 0, "an unrecorded verb answers 0");
	is_int(hsf_hda_replay_divergences(b), 1, "and counts as a divergence");
	OsHdaCodecClearEventCallback(hal, tag);
	b->ops->stop_events(b);
	hsf_hda_unbind();
	b->ops->destroy(b);
}

static void test_replay_polling(const char *dir)
{
	char path[4096];
	struct hsf_hda_backend *b;
	FILE *f;
	unsigned int got[5];

	snprintf(path, sizeof(path), "%s/poll.rec", dir);
	f = fopen(path, "w");
	fputs("# hsfmodem codec recording v1\n"
	      "info vendor=14f12c06 subsys=17aa2151 rev=00100100 addr=1 mfg=2\n"
	      "verb t=1 nid=70 verb=f80 param=00 res=00000000\n"
	      "verb t=2 nid=70 verb=f80 param=00 res=00000000\n"
	      "verb t=3 nid=70 verb=f80 param=00 res=00000001\n"
	      "state t=4 run\n"
	      "unknown t=5\n"
	      "verb t=6 nid=71 verb=f00 param=09 res=00000011\n"
	      "verb t=7 nid=71 verb=f00 param=0a res=00000022\n"
	      "verb t=8 nid=72 verb=f00 param=09 res=00000033\n"
	      "verb t=9 nid=72 verb=f00 param=0a res=00000044\n", f);
	fclose(f);
	b = hsf_hda_replay_new(path);
	ok(b != NULL, "a recording with an unknown record still loads");
	if (!b)
		return;
	hsf_hda_bind(b);
	is_int(OsHdaCodecGetVendorId(hsf_hda_hal()), 0x14f12c06, "the identity comes from the info record");
	for (int i = 0; i < 5; i++)
		got[i] = verb(0x70, 0xF80, 0);
	ok(got[0] == 0 && got[1] == 0 && got[2] == 1 && got[3] == 1 && got[4] == 1,
	   "a polled register replays its recorded sequence, then holds the last value");
	is_int(hsf_hda_replay_divergences(b), 0, "polling longer than recorded is not a divergence");
	verb(0x70, 0xF81, 0);
	is_int(hsf_hda_replay_divergences(b), 1, "an unrecorded verb is");
	is_int(verb(0x71, 0xF00, 0x0a), 0x22, "verbs are answered whatever their order");
	is_int(hsf_hda_replay_divergences(b), 2, "skipping a recorded verb is a divergence");
	verb(0x72, 0xF00, 0x09);
	verb(0x72, 0xF00, 0x0a);
	is_int(hsf_hda_replay_divergences(b), 2, "after which the replay follows the recording again");
	hsf_hda_unbind();
	b->ops->destroy(b);

	/* fewer polls than recorded */
	b = hsf_hda_replay_new(path);
	hsf_hda_bind(b);
	verb(0x70, 0xF80, 0);
	verb(0x71, 0xF00, 0x09);
	is_int(hsf_hda_replay_divergences(b), 0, "polling less than recorded is not a divergence");
	hsf_hda_unbind();
	b->ops->destroy(b);

	ok(hsf_hda_replay_new("/nonexistent/recording") == NULL, "a missing recording is an error");
}

int main(void)
{
	struct hsf_os_config cfg = HSF_OS_CONFIG_DEFAULT;
	char dir[] = "/tmp/hsf-hda-XXXXXX", path[4096];

	hsf_log_set_sink(quiet_sink);
	if (hsf_os_init(&cfg)) {
		printf("Bail out! OS layer init failed\n");
		return 1;
	}
	if (!mkdtemp(dir)) {
		printf("Bail out! mkdtemp: %s\n", strerror(errno));
		return 1;
	}
	snprintf(path, sizeof(path), "%s/codec.rec", dir);

	test_glue_fake();
	test_record(path);
	test_replay_roundtrip(path);
	test_replay_polling(dir);
	is_int(hsf_problem_count(), 0, "no invariant violations");

	hsf_os_shutdown();
	unlink(path);
	snprintf(path, sizeof(path), "%s/poll.rec", dir);
	unlink(path);
	rmdir(dir);
	return tap_done();
}
