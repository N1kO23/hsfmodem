/* SPDX-License-Identifier: MIT */
/*
 * Recording and replaying the codec dialogue.
 *
 * record wraps another backend (normally the real modem) and writes a text
 * file, one record per line:
 *
 *   # hsfmodem codec recording v1
 *   info vendor=14f12bfa subsys=17aa2150 rev=00100000 addr=1 mfg=2
 *   pci <256 bytes of the controller's config space, in hex>
 *   verb t=<us> nid=02 verb=708 param=81 res=00000000
 *   unsol t=<us> after=<verbs so far> res=08000000
 *   open t=<us> bytes=4096 tags=1/2
 *   state t=<us> run|stop|reset
 *   pos t=<us> dir=play|cap pos=<bytes>   (only with positions enabled)
 *   close t=<us>
 *
 * replay answers from such a file. Each (nid, verb, param) has its own queue
 * of recorded responses, served in order; once a queue is exhausted its last
 * response repeats. Polling loops therefore see the same sequence of values
 * however fast they run. Unsolicited responses are delivered when the verb
 * count reaches the recorded point. DMA rings and the wall clock are
 * simulated, as in the fake backend.
 *
 * A divergence is a verb that was never recorded, or one that does not
 * follow the recorded order. Polling a register more or fewer times than the
 * recording did is not a divergence; after one, the replay resynchronises
 * with the recorded order where it can.
 */
#include "hda_backend.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* ---- record ----------------------------------------------------------------- */

struct record {
	struct hsf_hda_backend b;
	struct hsf_hda_backend *inner;
	FILE *out;
	bool positions;
	void *play;
	uint64_t t0;
	unsigned long verbs;
	pthread_mutex_t lock;
};

static uint64_t rec_us(struct record *r)
{
	return (hsf_now_ns() - r->t0) / 1000u;
}

static void rec_line(struct record *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void rec_line(struct record *r, const char *fmt, ...)
{
	va_list ap;

	pthread_mutex_lock(&r->lock);
	va_start(ap, fmt);
	vfprintf(r->out, fmt, ap);
	va_end(ap);
	fflush(r->out);
	pthread_mutex_unlock(&r->lock);
}

static unsigned int rec_read(struct hsf_hda_backend *b, unsigned int nid, int direct,
			     unsigned int verb, unsigned int param)
{
	struct record *r = (struct record *)b;
	unsigned int res = r->inner->ops->read(r->inner, nid, direct, verb, param);

	__atomic_add_fetch(&r->verbs, 1, __ATOMIC_RELAXED);
	rec_line(r, "verb t=%" PRIu64 " nid=%02x verb=%03x param=%02x res=%08x\n",
		 rec_us(r), nid, verb, param, res);
	return res;
}

static void rec_unsol(void *ctx, uint32_t res)
{
	struct record *r = ctx;

	rec_line(r, "unsol t=%" PRIu64 " after=%lu res=%08x\n", rec_us(r),
		 __atomic_load_n(&r->verbs, __ATOMIC_RELAXED), res);
}

static unsigned int rec_wallclock(struct hsf_hda_backend *b)
{
	struct record *r = (struct record *)b;

	return r->inner->ops->wallclock(r->inner);
}

static int rec_open_dma(struct hsf_hda_backend *b, int bytes, void **play, void **cap)
{
	struct record *r = (struct record *)b;
	int err = r->inner->ops->open_dma(r->inner, bytes, play, cap);
	unsigned char tp = 0, tc = 0;
	unsigned long fifo;
	unsigned short *buf;

	if (!err) {
		r->play = *play;
		r->inner->ops->dma_info(r->inner, *play, &tp, &fifo, &buf);
		r->inner->ops->dma_info(r->inner, *cap, &tc, &fifo, &buf);
	}
	rec_line(r, "open t=%" PRIu64 " bytes=%d tags=%u/%u err=%d\n", rec_us(r), bytes, tp, tc, err);
	return err;
}

static void rec_close_dma(struct hsf_hda_backend *b, void *play, void *cap)
{
	struct record *r = (struct record *)b;

	r->inner->ops->close_dma(r->inner, play, cap);
	rec_line(r, "close t=%" PRIu64 "\n", rec_us(r));
}

static void rec_dma_info(struct hsf_hda_backend *b, void *stream, unsigned char *tag,
			 unsigned long *fifo, unsigned short **buf)
{
	struct record *r = (struct record *)b;

	r->inner->ops->dma_info(r->inner, stream, tag, fifo, buf);
}

static int rec_set_dma_state(struct hsf_hda_backend *b, OSHDA_STREAM_STATE st, void *play, void *cap)
{
	struct record *r = (struct record *)b;
	int err = r->inner->ops->set_dma_state(r->inner, st, play, cap);

	rec_line(r, "state t=%" PRIu64 " %s\n", rec_us(r),
		 st == OsHdaStreamStateRun ? "run" : st == OsHdaStreamStateStop ? "stop" : "reset");
	return err;
}

static unsigned long rec_get_dma_pos(struct hsf_hda_backend *b, void *stream)
{
	struct record *r = (struct record *)b;
	unsigned long pos = r->inner->ops->get_dma_pos(r->inner, stream);

	if (r->positions)
		rec_line(r, "pos t=%" PRIu64 " dir=%s pos=%lu\n", rec_us(r),
			 stream == r->play ? "play" : "cap", pos);
	return pos;
}

static int rec_start_events(struct hsf_hda_backend *b)
{
	struct record *r = (struct record *)b;

	return r->inner->ops->start_events(r->inner);
}

static void rec_stop_events(struct hsf_hda_backend *b)
{
	struct record *r = (struct record *)b;

	r->inner->ops->stop_events(r->inner);
}

static void rec_destroy(struct hsf_hda_backend *b)
{
	struct record *r = (struct record *)b;

	hsf_hda_set_unsol_observer(NULL, NULL);
	r->inner->ops->destroy(r->inner);
	fclose(r->out);
	pthread_mutex_destroy(&r->lock);
	free(r);
}

static const struct hsf_hda_backend_ops record_ops = {
	.read = rec_read,
	.wallclock = rec_wallclock,
	.open_dma = rec_open_dma,
	.close_dma = rec_close_dma,
	.dma_info = rec_dma_info,
	.set_dma_state = rec_set_dma_state,
	.get_dma_pos = rec_get_dma_pos,
	.start_events = rec_start_events,
	.stop_events = rec_stop_events,
	.destroy = rec_destroy,
};

struct hsf_hda_backend *hsf_hda_record_new(struct hsf_hda_backend *inner, const char *path,
					   bool positions)
{
	struct record *r;
	uint8_t config[256];

	if (!inner)
		return NULL;
	r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->out = fopen(path, "w");
	if (!r->out) {
		hsf_log(HSF_LOG_ERR, "cannot create recording %s: %s", path, strerror(errno));
		free(r);
		return NULL;
	}
	r->b = *inner;	/* identity and PCI handle */
	r->b.ops = &record_ops;
	r->b.name = "record";
	r->inner = inner;
	r->positions = positions;
	r->t0 = hsf_now_ns();
	pthread_mutex_init(&r->lock, NULL);
	fprintf(r->out, "# hsfmodem codec recording v1\n");
	fprintf(r->out, "info vendor=%08x subsys=%08x rev=%08x addr=%u mfg=%u\n", inner->vendor_id,
		inner->subsystem_id, inner->revision_id, inner->codec_addr, inner->mfg_nid);
	if (hsf_pci_lookup(inner->pci_handle, config)) {
		fprintf(r->out, "pci ");
		for (int i = 0; i < 256; i++)
			fprintf(r->out, "%02x", config[i]);
		fprintf(r->out, "\n");
	}
	hsf_hda_set_unsol_observer(rec_unsol, r);
	return &r->b;
}

/* ---- replay ----------------------------------------------------------------- */

struct verb_queue {
	uint32_t key;		/* nid << 20 | verb << 8 | param */
	uint32_t *res;
	unsigned int n, next;
};

struct unsol_rec {
	unsigned long after;
	uint32_t res;
};

struct replay_stream {
	unsigned char tag;
	int bytes;
	unsigned char *buf;
};

struct replay {
	struct hsf_hda_backend b;
	struct verb_queue *q;
	unsigned int nq;
	uint32_t *order;	/* recorded key sequence, for divergence counting */
	unsigned long norder, cursor;
	uint32_t last_key;
	struct unsol_rec *ev;
	unsigned int nev, next_ev;
	unsigned long verbs, divergences;
	unsigned char tags[2];
	struct replay_stream play, cap;
	bool running;
	uint64_t run_start_ns, base_bytes;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_t thread;
	bool thread_running, stop;
};

static struct verb_queue *find_queue(struct replay *r, uint32_t key, bool create)
{
	for (unsigned int i = 0; i < r->nq; i++)
		if (r->q[i].key == key)
			return &r->q[i];
	if (!create)
		return NULL;
	struct verb_queue *nq = realloc(r->q, (r->nq + 1) * sizeof(*nq));

	if (!nq)
		return NULL;
	r->q = nq;
	memset(&r->q[r->nq], 0, sizeof(r->q[0]));
	r->q[r->nq].key = key;
	return &r->q[r->nq++];
}

static bool append_u32(uint32_t **v, unsigned long *n, uint32_t x)
{
	uint32_t *nv = realloc(*v, (*n + 1) * sizeof(**v));

	if (!nv)
		return false;
	nv[(*n)++] = x;
	*v = nv;
	return true;
}

#define RESYNC_WINDOW 256

/* Called with the lock held. */
static void follow_order(struct replay *r, uint32_t key, bool recorded)
{
	unsigned long end = r->cursor + RESYNC_WINDOW < r->norder ? r->cursor + RESYNC_WINDOW : r->norder;
	unsigned long i;

	/* the recording polled the previous register more often than we did */
	while (r->cursor < r->norder && r->order[r->cursor] != key && r->order[r->cursor] == r->last_key)
		r->cursor++;
	if (r->cursor < r->norder && r->order[r->cursor] == key) {
		r->cursor++;
	} else if (key != r->last_key || !recorded) {
		/* not a repeated poll: out of order (skip ahead if it shows up soon) */
		r->divergences++;
		for (i = r->cursor; i < end; i++)
			if (r->order[i] == key) {
				r->cursor = i + 1;
				break;
			}
	}
	r->last_key = key;
}

static unsigned int rep_read(struct hsf_hda_backend *b, unsigned int nid, int direct,
			     unsigned int verb, unsigned int param)
{
	struct replay *r = (struct replay *)b;
	uint32_t key = (nid << 20) | (verb << 8) | param;
	struct verb_queue *q;
	unsigned int res = 0;

	(void)direct;
	pthread_mutex_lock(&r->lock);
	q = find_queue(r, key, false);
	follow_order(r, key, q != NULL);
	r->verbs++;
	if (q) {
		res = q->res[q->next < q->n ? q->next : q->n - 1];
		if (q->next < q->n)
			q->next++;
	} else {
		hsf_log(HSF_LOG_DEBUG, "replay: verb nid=%02x verb=%03x param=%02x not recorded", nid, verb, param);
	}
	pthread_cond_broadcast(&r->cond);
	pthread_mutex_unlock(&r->lock);
	return res;
}

static void *rep_event_main(void *arg)
{
	struct replay *r = arg;

	hsf_thread_enter("replay-events");
	pthread_mutex_lock(&r->lock);
	while (!r->stop) {
		if (r->next_ev < r->nev && r->verbs >= r->ev[r->next_ev].after) {
			uint32_t res = r->ev[r->next_ev++].res;

			pthread_mutex_unlock(&r->lock);
			hsf_hda_deliver_unsol(res);
			pthread_mutex_lock(&r->lock);
			continue;
		}
		pthread_cond_wait(&r->cond, &r->lock);
	}
	pthread_mutex_unlock(&r->lock);
	return NULL;
}

static unsigned int rep_wallclock(struct hsf_hda_backend *b)
{
	(void)b;
	return (unsigned int)(hsf_now_ns() * 3 / 125);
}

static void rep_close_dma(struct hsf_hda_backend *b, void *play, void *cap)
{
	struct replay *r = (struct replay *)b;

	(void)play;
	(void)cap;
	free(r->play.buf);
	free(r->cap.buf);
	r->play.buf = r->cap.buf = NULL;
	r->running = false;
}

static int rep_open_dma(struct hsf_hda_backend *b, int bytes, void **play, void **cap)
{
	struct replay *r = (struct replay *)b;

	if (bytes <= 0 || r->play.buf)
		return -EINVAL;
	r->play.buf = calloc(1, (size_t)bytes);
	r->cap.buf = calloc(1, (size_t)bytes);
	if (!r->play.buf || !r->cap.buf) {
		rep_close_dma(b, NULL, NULL);
		return -ENOMEM;
	}
	r->play.bytes = r->cap.bytes = bytes;
	r->play.tag = r->tags[0] ? r->tags[0] : 1;
	r->cap.tag = r->tags[1] ? r->tags[1] : 2;
	r->running = false;
	r->base_bytes = 0;
	*play = &r->play;
	*cap = &r->cap;
	return 0;
}

static void rep_dma_info(struct hsf_hda_backend *b, void *stream, unsigned char *tag,
			 unsigned long *fifo, unsigned short **buf)
{
	struct replay_stream *s = stream;

	(void)b;
	*tag = s->tag;
	*fifo = 0;
	*buf = (unsigned short *)s->buf;
}

static uint64_t rep_bytes(struct replay *r)
{
	if (!r->running)
		return r->base_bytes;
	return r->base_bytes + (hsf_now_ns() - r->run_start_ns) * 32000u / 1000000000u;
}

static int rep_set_dma_state(struct hsf_hda_backend *b, OSHDA_STREAM_STATE st, void *play, void *cap)
{
	struct replay *r = (struct replay *)b;

	(void)play;
	(void)cap;
	if (st == OsHdaStreamStateRun && !r->running) {
		r->run_start_ns = hsf_now_ns();
		r->running = true;
	} else if (st == OsHdaStreamStateStop) {
		r->base_bytes = rep_bytes(r);
		r->running = false;
	} else if (st == OsHdaStreamStateReset) {
		r->base_bytes = 0;
		r->running = false;
	}
	return 0;
}

static unsigned long rep_get_dma_pos(struct hsf_hda_backend *b, void *stream)
{
	struct replay_stream *s = stream;

	return s->bytes ? (unsigned long)(rep_bytes((struct replay *)b) % (unsigned)s->bytes) & ~1ul : 0;
}

static int rep_start_events(struct hsf_hda_backend *b)
{
	struct replay *r = (struct replay *)b;

	if (r->thread_running)
		return 0;
	r->stop = false;
	if (hsf_thread_start(&r->thread, "replay-events", rep_event_main, r))
		return -1;
	r->thread_running = true;
	return 0;
}

static void rep_stop_events(struct hsf_hda_backend *b)
{
	struct replay *r = (struct replay *)b;

	if (!r->thread_running)
		return;
	pthread_mutex_lock(&r->lock);
	r->stop = true;
	pthread_cond_broadcast(&r->cond);
	pthread_mutex_unlock(&r->lock);
	pthread_join(r->thread, NULL);
	r->thread_running = false;
}

static void rep_destroy(struct hsf_hda_backend *b)
{
	struct replay *r = (struct replay *)b;

	rep_stop_events(b);
	rep_close_dma(b, NULL, NULL);
	hsf_pci_unregister(r->b.pci_handle);
	for (unsigned int i = 0; i < r->nq; i++)
		free(r->q[i].res);
	free(r->q);
	free(r->order);
	free(r->ev);
	pthread_mutex_destroy(&r->lock);
	pthread_cond_destroy(&r->cond);
	free(r);
}

unsigned long hsf_hda_replay_divergences(struct hsf_hda_backend *b)
{
	struct replay *r = (struct replay *)b;
	unsigned long d;

	pthread_mutex_lock(&r->lock);
	d = r->divergences;
	pthread_mutex_unlock(&r->lock);
	return d;
}

static const struct hsf_hda_backend_ops replay_ops = {
	.read = rep_read,
	.wallclock = rep_wallclock,
	.open_dma = rep_open_dma,
	.close_dma = rep_close_dma,
	.dma_info = rep_dma_info,
	.set_dma_state = rep_set_dma_state,
	.get_dma_pos = rep_get_dma_pos,
	.start_events = rep_start_events,
	.stop_events = rep_stop_events,
	.destroy = rep_destroy,
};

struct hsf_hda_backend *hsf_hda_replay_new(const char *path)
{
	uint8_t config[256] = { 0 };
	struct replay *r;
	FILE *in;
	char line[1024];
	unsigned long lineno = 0;

	in = fopen(path, "r");
	if (!in) {
		hsf_log(HSF_LOG_ERR, "cannot open recording %s: %s", path, strerror(errno));
		return NULL;
	}
	r = calloc(1, sizeof(*r));
	if (!r) {
		fclose(in);
		return NULL;
	}
	pthread_mutex_init(&r->lock, NULL);
	pthread_cond_init(&r->cond, NULL);
	r->b.ops = &replay_ops;
	r->b.name = "replay";
	r->b.pci_handle = &r->b;

	while (fgets(line, sizeof(line), in)) {
		unsigned int a, v, p, res, addr, mfg, tp, tc;
		unsigned long after;
		uint64_t t;
		char pci[513];

		lineno++;
		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (sscanf(line, "pci %512[0-9a-f]", pci) == 1 && strlen(pci) == 512) {
			for (int i = 0; i < 256; i++)
				sscanf(&pci[2 * i], "%2hhx", &config[i]);
			continue;
		}
		if (sscanf(line, "info vendor=%x subsys=%x rev=%x addr=%u mfg=%u", &r->b.vendor_id,
			   &r->b.subsystem_id, &r->b.revision_id, &addr, &mfg) == 5) {
			r->b.codec_addr = addr;
			r->b.mfg_nid = mfg;
		} else if (sscanf(line, "verb t=%" SCNu64 " nid=%x verb=%x param=%x res=%x", &t, &a, &v, &p,
				  &res) == 5) {
			uint32_t key = (a << 20) | (v << 8) | p;
			struct verb_queue *q = find_queue(r, key, true);
			unsigned long n;

			if (!q)
				goto oom;
			n = q->n;
			if (!append_u32(&q->res, &n, res) || !append_u32(&r->order, &r->norder, key))
				goto oom;
			q->n = (unsigned int)n;
		} else if (sscanf(line, "unsol t=%" SCNu64 " after=%lu res=%x", &t, &after, &res) == 3) {
			struct unsol_rec *ne = realloc(r->ev, (r->nev + 1) * sizeof(*ne));

			if (!ne)
				goto oom;
			r->ev = ne;
			r->ev[r->nev].after = after;
			r->ev[r->nev++].res = res;
		} else if (sscanf(line, "open t=%" SCNu64 " bytes=%u tags=%u/%u", &t, &a, &tp, &tc) == 4) {
			r->tags[0] = (unsigned char)tp;
			r->tags[1] = (unsigned char)tc;
		} else if (strncmp(line, "state ", 6) && strncmp(line, "pos ", 4) && strncmp(line, "close ", 6)) {
			hsf_log(HSF_LOG_WARN, "%s:%lu: unrecognised record", path, lineno);
		}
	}
	fclose(in);
	hsf_pci_register(r->b.pci_handle, config);
	hsf_log(HSF_LOG_INFO, "replaying %s: %lu verbs (%u distinct), %u unsolicited responses",
		path, r->norder, r->nq, r->nev);
	return &r->b;
oom:
	fclose(in);
	rep_destroy(&r->b);
	return NULL;
}
