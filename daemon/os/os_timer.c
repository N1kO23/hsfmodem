/* SPDX-License-Identifier: MIT */
/*
 * Timers.
 *
 * Every kernel timer expired in softirq context. A periodic timer only queued
 * work on the shared "modem" worker (OsMdmThread), which ran the blob
 * callback and re-armed the timer after it returned (fixed delay; the first
 * expiry is immediate). A one-shot timer (OsCreateTimer) ran the blob
 * callback directly in softirq context. A dedicated dispatcher thread plays
 * the softirq here; its callbacks run in atomic context and outrank the
 * modem thread when real-time scheduling is enabled.
 */
#include "hsf_os.h"

#include <errno.h>
#include <sched.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct ktimer {
	struct ktimer *next;
	uint64_t expires;
	bool armed;
	bool running;
	void (*fn)(struct ktimer *);
};

static pthread_mutex_t tl = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tcond, tdone;
static struct ktimer *armed_list;
static pthread_t softirq_th;
static pid_t softirq_tid;
static bool softirq_running, softirq_stop;

static void kt_unlink(struct ktimer *t)
{
	for (struct ktimer **pp = &armed_list; *pp; pp = &(*pp)->next) {
		if (*pp == t) {
			*pp = t->next;
			break;
		}
	}
	t->next = NULL;
	t->armed = false;
}

static void kt_mod(struct ktimer *t, uint64_t expires)
{
	struct ktimer **pp;

	pthread_mutex_lock(&tl);
	if (t->armed)
		kt_unlink(t);
	t->expires = expires;
	for (pp = &armed_list; *pp && (*pp)->expires <= expires; pp = &(*pp)->next)
		;
	t->next = *pp;
	*pp = t;
	t->armed = true;
	pthread_cond_signal(&tcond);
	pthread_mutex_unlock(&tl);
}

static void kt_del(struct ktimer *t, bool sync)
{
	pthread_mutex_lock(&tl);
	if (t->armed)
		kt_unlink(t);
	while (sync && t->running && hsf_gettid() != softirq_tid)
		pthread_cond_wait(&tdone, &tl);
	pthread_mutex_unlock(&tl);
}

static void *softirq_main(void *arg)
{
	(void)arg;
	hsf_thread_enter("softirq");
	softirq_tid = hsf_gettid();
	pthread_mutex_lock(&tl);
	while (!softirq_stop) {
		struct ktimer *t = armed_list;

		if (!t) {
			pthread_cond_wait(&tcond, &tl);
			continue;
		}
		if (t->expires > hsf_now_ns()) {
			hsf_cond_wait_until(&tcond, &tl, t->expires);
			continue;
		}
		kt_unlink(t);
		t->running = true;
		pthread_mutex_unlock(&tl);

		hsf_in_softirq = true;
		hsf_atomic_depth++;
		t->fn(t);
		hsf_atomic_depth--;
		hsf_in_softirq = false;
		if (hsf_atomic_depth) {
			hsf_problem("timer callback returned with atomic depth %d", hsf_atomic_depth);
			hsf_atomic_depth = 0;
		}

		pthread_mutex_lock(&tl);
		t->running = false;
		pthread_cond_broadcast(&tdone);
	}
	pthread_mutex_unlock(&tl);
	return NULL;
}

int hsf_timers_init(void)
{
	hsf_cond_init(&tcond);
	pthread_cond_init(&tdone, NULL);
	softirq_stop = false;
	if (hsf_thread_start(&softirq_th, "softirq", softirq_main, NULL))
		return -1;
	softirq_running = true;
	hsf_thread_set_rt(softirq_th, 5, "softirq");
	return 0;
}

void hsf_timers_shutdown(void)
{
	if (!softirq_running)
		return;
	pthread_mutex_lock(&tl);
	softirq_stop = true;
	if (armed_list)
		hsf_problem("timers still armed at shutdown");
	pthread_cond_broadcast(&tcond);
	pthread_mutex_unlock(&tl);
	pthread_join(softirq_th, NULL);
	softirq_running = false;
	armed_list = NULL;
}

/* ---- periodic timers ------------------------------------------------------ */

#define PT_DELETE 1u
#define PT_QUEUED 2u

struct ptimer {
	OSSCHED task;
	struct ktimer kt;
	UINT32 msec;
	unsigned flags;
	bool in_callback;
	bool destroy_pending;
	PCBFUNC cb;
	PFREE_FUNC free_fn;
	PVOID ref;
};

static INT32 nperiodic;

static void ptimer_free(struct ptimer *p)
{
	__atomic_sub_fetch(&nperiodic, 1, __ATOMIC_SEQ_CST);
	if (p->free_fn)
		p->free_fn(p, p->ref);
	else
		OsFree(p);
}

static void ptimer_run(void *arg)
{
	struct ptimer *p = arg;

	if (__atomic_load_n(&p->flags, __ATOMIC_SEQ_CST) & PT_DELETE) {
		__atomic_and_fetch(&p->flags, ~PT_QUEUED, __ATOMIC_SEQ_CST);
		return;
	}
	p->in_callback = true;
	p->cb(p->ref);
	p->in_callback = false;
	__atomic_and_fetch(&p->flags, ~PT_QUEUED, __ATOMIC_SEQ_CST);
	if (p->destroy_pending) {
		ptimer_free(p);
		return;
	}
	if (!(__atomic_load_n(&p->flags, __ATOMIC_SEQ_CST) & PT_DELETE))
		kt_mod(&p->kt, hsf_now_ns() + (uint64_t)p->msec * 1000000u);
}

static void ptimer_fire(struct ktimer *kt)
{
	struct ptimer *p = (struct ptimer *)((char *)kt - offsetof(struct ptimer, kt));

	if (__atomic_load_n(&p->flags, __ATOMIC_SEQ_CST) & PT_DELETE)
		return;
	if (!(__atomic_fetch_or(&p->flags, PT_QUEUED, __ATOMIC_SEQ_CST) & PT_QUEUED))
		if (OsThreadSchedule(OsMdmThread, &p->task) <= 0)
			__atomic_and_fetch(&p->flags, ~PT_QUEUED, __ATOMIC_SEQ_CST);
}

HSF_EXPORT HOSTIMER OsCreatePeriodicTimer(UINT32 InitialTimeOut, PCBFUNC pTimeOutCallBack,
					  PALLOC_FUNC pFuncAlloc, PFREE_FUNC pFuncFree,
					  PVOID pRefData, HTHREAD *pThreadId)
{
	struct ptimer *p;

	p = pFuncAlloc ? pFuncAlloc(sizeof(*p), pRefData) : OsAllocate(sizeof(*p));
	HSF_TRACE("OsCreatePeriodicTimer", "%u ms, cb=%p, alloc=%p) = (%p", InitialTimeOut,
		  (void *)pTimeOutCallBack, (void *)pFuncAlloc, (void *)p);
	if (!p)
		return NULL;
	memset(p, 0, sizeof(*p));
	p->msec = InitialTimeOut;
	p->cb = pTimeOutCallBack;
	p->free_fn = pFuncFree;
	p->ref = pRefData;
	p->kt.fn = ptimer_fire;
	OsThreadScheduleInit(&p->task, ptimer_run, p);

	/* The modem worker lives until shutdown, so the thread id handed out
	 * below stays valid. */
	if (!hsf_modem_thread()) {
		if (pFuncFree)
			pFuncFree(p, pRefData);
		else
			OsFree(p);
		return NULL;
	}
	__atomic_add_fetch(&nperiodic, 1, __ATOMIC_SEQ_CST);
	if (InitialTimeOut)
		kt_mod(&p->kt, hsf_now_ns());
	if (pThreadId)
		*pThreadId = (HTHREAD)(uintptr_t)hsf_thread_tid(OsMdmThread);
	return (HOSTIMER)p;
}

POSTHRD hsf_modem_thread(void)
{
	static pthread_mutex_t create_lock = PTHREAD_MUTEX_INITIALIZER;
	POSTHRD t = __atomic_load_n(&OsMdmThread, __ATOMIC_ACQUIRE);
	int tid;

	if (t)
		return t;
	pthread_mutex_lock(&create_lock);
	if (!OsMdmThread)
		__atomic_store_n(&OsMdmThread, OsThreadCreate("modem", TRUE, &tid), __ATOMIC_RELEASE);
	t = OsMdmThread;
	pthread_mutex_unlock(&create_lock);
	return t;
}

HSF_EXPORT void OsDestroyPeriodicTimer(HOSTIMER hTimeOut)
{
	struct ptimer *p = (struct ptimer *)hTimeOut;
	uint64_t deadline;

	HSF_TRACE("OsDestroyPeriodicTimer", "%p", (void *)p);
	if (!p)
		return;
	__atomic_or_fetch(&p->flags, PT_DELETE, __ATOMIC_SEQ_CST);
	kt_del(&p->kt, true);
	if (p->in_callback && hsf_thread_tid(OsMdmThread) == hsf_gettid()) {
		p->destroy_pending = true;	/* freed when the callback returns */
		return;
	}
	deadline = hsf_now_ns() + 2000000000u;
	while (__atomic_load_n(&p->flags, __ATOMIC_SEQ_CST) & PT_QUEUED) {
		if (hsf_now_ns() > deadline) {
			hsf_problem("OsDestroyPeriodicTimer(%p): its work item never ran", (void *)p);
			break;
		}
		sched_yield();
	}
	ptimer_free(p);
}

HSF_EXPORT BOOL OsSetPeriodicTimer(HOSTIMER hTimeOut, UINT32 NewTimeOut)
{
	struct ptimer *p = (struct ptimer *)hTimeOut;

	HSF_TRACE("OsSetPeriodicTimer", "%p, %u", (void *)p, NewTimeOut);
	if (!p)
		return FALSE;
	if (NewTimeOut == p->msec)
		return TRUE;
	p->msec = NewTimeOut;
	if (NewTimeOut) {
		__atomic_and_fetch(&p->flags, ~PT_DELETE, __ATOMIC_SEQ_CST);
		kt_mod(&p->kt, hsf_now_ns() + (uint64_t)NewTimeOut * 1000000u);
		return TRUE;
	}
	__atomic_or_fetch(&p->flags, PT_DELETE, __ATOMIC_SEQ_CST);
	kt_del(&p->kt, false);
	return FALSE;
}

HSF_EXPORT void OsImmediateTimeOut(HOSTIMER hTimeOut)
{
	struct ptimer *p = (struct ptimer *)hTimeOut;

	HSF_TRACE("OsImmediateTimeOut", "%p", (void *)p);
	if (p)
		kt_mod(&p->kt, hsf_now_ns());
}

/* ---- one-shot timers ------------------------------------------------------- */

struct otimer {
	struct ktimer kt;
	UINT32 msec;
	void (*cb)(void *);
	void *ref;
};

static void otimer_fire(struct ktimer *kt)
{
	struct otimer *o = (struct otimer *)kt;

	o->cb(o->ref);
}

HSF_EXPORT HANDLE OsCreateTimer(UINT32 msec, PVOID pCBFunc, PVOID pRefData)
{
	struct otimer *o;

	if (!pCBFunc) {
		hsf_problem("OsCreateTimer with a NULL callback");
		return NULL;
	}
	o = calloc(1, sizeof(*o));
	HSF_TRACE("OsCreateTimer", "%u ms, cb=%p) = (%p", msec, pCBFunc, (void *)o);
	if (!o)
		return NULL;
	o->kt.fn = otimer_fire;
	o->msec = msec;
	o->cb = (void (*)(void *))pCBFunc;
	o->ref = pRefData;
	return o;
}

HSF_EXPORT void OsSetTimer(PVOID pTimer)
{
	struct otimer *o = pTimer;

	HSF_TRACE_HOT("OsSetTimer", "%p", pTimer);
	if (o)
		kt_mod(&o->kt, hsf_now_ns() + (uint64_t)o->msec * 1000000u);
}

HSF_EXPORT void OsCancelTimer(PVOID pTimer)
{
	HSF_TRACE_HOT("OsCancelTimer", "%p", pTimer);
	if (pTimer)
		kt_del(&((struct otimer *)pTimer)->kt, false);
}

HSF_EXPORT void OsChangeTimerTimeOut(PVOID pTimer, UINT32 msec)
{
	HSF_TRACE_HOT("OsChangeTimerTimeOut", "%p, %u", pTimer, msec);
	if (pTimer)
		((struct otimer *)pTimer)->msec = msec;
}

HSF_EXPORT void OsDestroyTimer(PVOID pTimer)
{
	HSF_TRACE("OsDestroyTimer", "%p", pTimer);
	if (!pTimer)
		return;
	kt_del(&((struct otimer *)pTimer)->kt, true);
	free(pTimer);
}

int hsf_periodic_timer_count(void)
{
	return __atomic_load_n(&nperiodic, __ATOMIC_SEQ_CST);
}
