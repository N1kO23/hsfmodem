/* SPDX-License-Identifier: MIT */
/*
 * Worker threads and work items.
 *
 * OsThreadCreate made a kthread_worker in the kernel; OsThreadSchedule queued
 * a work item that lives inside blob-owned OSSCHED storage. Items run in FIFO
 * order, one at a time per thread. An item is marked unqueued just before it
 * runs, so it may queue itself again.
 */
#include "hsf_os.h"

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define THREAD_STACK (256 * 1024)
#define ALT_STACK (64 * 1024)

static bool rt_enabled;
static int rt_base = 40;
static bool rt_warned;

void hsf_threads_configure_rt(bool enabled, int base_priority)
{
	rt_enabled = enabled;
	rt_base = base_priority;
}

void hsf_thread_set_rt(pthread_t th, int priority_offset, const char *name)
{
	struct sched_param sp = { .sched_priority = rt_base + priority_offset };
	int err;

	if (!rt_enabled)
		return;
	err = pthread_setschedparam(th, SCHED_FIFO, &sp);
	if (err && !rt_warned) {
		rt_warned = true;
		hsf_log(HSF_LOG_WARN,
			"SCHED_FIFO priority %d for %s failed (%s); running without real-time "
			"scheduling (see docs: RLIMIT_RTPRIO, CAP_SYS_NICE, rt_group_sched)",
			sp.sched_priority, name, strerror(err));
	}
}

void hsf_thread_enter(const char *name)
{
	char tname[16];

#if !defined(__SANITIZE_ADDRESS__)
	/* An alternate stack lets the fault handler run even after a stack
	 * overflow in blob code. AddressSanitizer installs and later unmaps
	 * its own, so leave that in place under ASan. */
	stack_t ss = { .ss_size = ALT_STACK };

	ss.ss_sp = mmap(NULL, ALT_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ss.ss_sp != MAP_FAILED)
		sigaltstack(&ss, NULL);	/* reclaimed at process exit */
#endif
	if (name) {
		snprintf(tname, sizeof(tname), "hsf/%.11s", name);
		pthread_setname_np(pthread_self(), tname);
	}
}

int hsf_thread_start(pthread_t *th, const char *name, void *(*fn)(void *), void *arg)
{
	pthread_attr_t a;
	int err;

	(void)name;
	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, THREAD_STACK);
	err = pthread_create(th, &a, fn, arg);
	pthread_attr_destroy(&a);
	return err;
}

/* ---- worker threads ------------------------------------------------------- */

struct work {
	struct work *next;
	void (*func)(void *);
	void *data;
	int queued;
};

_Static_assert(sizeof(struct work) <= sizeof(OSSCHED), "work item must fit in the blob's OSSCHED storage");

struct _OSTHRD {
	pthread_t th;
	pid_t tid;
	pthread_mutex_t m;
	pthread_cond_t c;
	struct work *head, *tail;
	int stop, started;
	char name[32];
};

POSTHRD OsMdmThread;

static void *worker_main(void *arg)
{
	OSTHRD *t = arg;

	hsf_thread_enter(t->name);
	pthread_mutex_lock(&t->m);
	t->tid = hsf_gettid();
	t->started = 1;
	pthread_cond_broadcast(&t->c);
	while (!t->stop) {
		struct work *w = t->head;

		if (!w) {
			pthread_cond_wait(&t->c, &t->m);
			continue;
		}
		t->head = w->next;
		if (!t->head)
			t->tail = NULL;
		w->next = NULL;
		w->queued = 0;
		pthread_mutex_unlock(&t->m);
		w->func(w->data);
		if (hsf_atomic_depth) {
			hsf_problem("work item %p on \"%s\" returned in atomic context (depth %d)",
				    (void *)w, t->name, hsf_atomic_depth);
			hsf_atomic_depth = 0;
		}
		pthread_mutex_lock(&t->m);
	}
	pthread_mutex_unlock(&t->m);
	return NULL;
}

HSF_EXPORT POSTHRD OsThreadCreate(const char *name, BOOL highestprio, int *pid)
{
	OSTHRD *t = calloc(1, sizeof(*t));

	if (!t)
		return NULL;
	snprintf(t->name, sizeof(t->name), "%s", name ? name : "worker");
	pthread_mutex_init(&t->m, NULL);
	pthread_cond_init(&t->c, NULL);
	if (hsf_thread_start(&t->th, t->name, worker_main, t)) {
		hsf_problem("OsThreadCreate(%s): cannot start thread", t->name);
		free(t);
		return NULL;
	}
	pthread_mutex_lock(&t->m);
	while (!t->started)
		pthread_cond_wait(&t->c, &t->m);
	pthread_mutex_unlock(&t->m);
	if (highestprio)
		hsf_thread_set_rt(t->th, 0, t->name);
	if (pid)
		*pid = t->tid;
	HSF_TRACE("OsThreadCreate", "\"%s\", %d) = (%p, tid %d", t->name, highestprio, (void *)t, t->tid);
	return t;
}

HSF_EXPORT void OsThreadDestroy(POSTHRD t)
{
	int dropped = 0;

	HSF_TRACE("OsThreadDestroy", "%p \"%s\"", (void *)t, t ? t->name : "");
	if (!t)
		return;
	if (t->tid == hsf_gettid()) {
		hsf_problem("OsThreadDestroy(\"%s\") called on that thread", t->name);
		return;
	}
	pthread_mutex_lock(&t->m);
	t->stop = 1;
	for (struct work *w = t->head; w; w = w->next, dropped++)
		w->queued = 0;
	pthread_cond_broadcast(&t->c);
	pthread_mutex_unlock(&t->m);
	pthread_join(t->th, NULL);
	if (dropped)
		hsf_log(HSF_LOG_DEBUG, "OsThreadDestroy(\"%s\"): %d queued items dropped", t->name, dropped);
	pthread_mutex_destroy(&t->m);
	pthread_cond_destroy(&t->c);
	free(t);
}

HSF_EXPORT void OsThreadScheduleInit(HOSSCHED hWorkStorage, void (*func)(void *), void *data)
{
	struct work *w = (struct work *)hWorkStorage;

	HSF_TRACE_HOT("OsThreadScheduleInit", "%p, %p, %p", (void *)w, (void *)func, data);
	memset(w, 0, sizeof(*w));
	w->func = func;
	w->data = data;
}

HSF_EXPORT int OsThreadSchedule(POSTHRD t, HOSSCHED hWorkStorage)
{
	struct work *w = (struct work *)hWorkStorage;

	HSF_TRACE_HOT("OsThreadSchedule", "%p, %p", (void *)t, (void *)w);
	if (!t) {
		hsf_problem("OsThreadSchedule on a NULL thread");
		return 0;
	}
	pthread_mutex_lock(&t->m);
	if (w->queued || t->stop) {
		pthread_mutex_unlock(&t->m);
		return 0;
	}
	w->queued = 1;
	w->next = NULL;
	if (t->tail)
		t->tail->next = w;
	else
		t->head = w;
	t->tail = w;
	pthread_cond_signal(&t->c);
	pthread_mutex_unlock(&t->m);
	return 1;
}

HSF_EXPORT void OsThreadScheduleDone(void)
{
	/* The kernel version dropped a module reference here. */
	HSF_TRACE_HOT("OsThreadScheduleDone", "");
}

pid_t hsf_thread_tid(POSTHRD t)
{
	return t ? t->tid : 0;
}

void hsf_threads_shutdown(void)
{
	if (OsMdmThread) {
		OsThreadDestroy(OsMdmThread);
		OsMdmThread = NULL;
	}
}
