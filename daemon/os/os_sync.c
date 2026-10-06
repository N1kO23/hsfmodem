/* SPDX-License-Identifier: MIT */
/*
 * Critical sections, locks and events, following the legacy kernel semantics
 * rather than pthread conventions:
 *   - a critical section disabled interrupts around a spinlock: it nests on
 *     the owning thread, puts the thread in atomic context while held, and
 *     must not be held across a sleep;
 *   - an OsLock is a binary semaphore with an owner and a nesting count, so
 *     a thread other than the owner may release it;
 *   - an event is a sticky flag that a successful wait consumes.
 */
#include "hsf_os.h"

#include <errno.h>
#include <stdlib.h>

/* ---- critical sections ----------------------------------------------------- */

struct crit {
	pthread_mutex_t m;
	pthread_cond_t c;
	pid_t owner;
	unsigned nest;
};

HSF_EXPORT HCRIT OsCriticalSectionCreate(void)
{
	struct crit *cs = calloc(1, sizeof(*cs));

	HSF_TRACE("OsCriticalSectionCreate", ") = (%p", (void *)cs);
	if (!cs)
		return NULL;
	pthread_mutex_init(&cs->m, NULL);
	pthread_cond_init(&cs->c, NULL);
	return (HCRIT)cs;
}

HSF_EXPORT VOID OsCriticalSectionDestroy(HCRIT hMutex)
{
	struct crit *cs = (struct crit *)hMutex;

	HSF_TRACE("OsCriticalSectionDestroy", "%p", (void *)cs);
	if (!cs)
		return;
	if (cs->owner)
		hsf_problem("OsCriticalSectionDestroy(%p) while held by %d", (void *)cs, cs->owner);
	pthread_mutex_destroy(&cs->m);
	pthread_cond_destroy(&cs->c);
	free(cs);
}

HSF_EXPORT VOID OsCriticalSectionAcquire(HCRIT hMutex)
{
	struct crit *cs = (struct crit *)hMutex;
	pid_t me = hsf_gettid();

	HSF_TRACE_HOT("OsCriticalSectionAcquire", "%p", (void *)cs);
	if (!cs)	/* tolerated by the legacy code */
		return;
	hsf_atomic_depth++;
	pthread_mutex_lock(&cs->m);
	if (cs->owner == me) {
		cs->nest++;
	} else {
		while (cs->owner)
			pthread_cond_wait(&cs->c, &cs->m);
		cs->owner = me;
		cs->nest = 0;
	}
	pthread_mutex_unlock(&cs->m);
}

HSF_EXPORT VOID OsCriticalSectionRelease(HCRIT hMutex)
{
	struct crit *cs = (struct crit *)hMutex;

	HSF_TRACE_HOT("OsCriticalSectionRelease", "%p", (void *)cs);
	if (!cs)
		return;
	pthread_mutex_lock(&cs->m);
	if (cs->nest) {
		cs->nest--;
	} else {
		if (cs->owner != hsf_gettid())
			hsf_problem("critical section %p released by %d, owner %d",
				    (void *)cs, hsf_gettid(), cs->owner);
		cs->owner = 0;
		pthread_cond_signal(&cs->c);
	}
	pthread_mutex_unlock(&cs->m);
	if (--hsf_atomic_depth < 0) {
		hsf_problem("OsCriticalSectionRelease without matching acquire");
		hsf_atomic_depth = 0;
	}
}

/* ---- OsLock ---------------------------------------------------------------- */

struct oslock {
	pthread_mutex_t m;
	pthread_cond_t c;
	int free;		/* binary semaphore: 1 = available */
	pid_t owner;
	unsigned nest;
};

HSF_EXPORT HLOCK OsLockCreate(void)
{
	struct oslock *l = calloc(1, sizeof(*l));

	HSF_TRACE("OsLockCreate", ") = (%p", (void *)l);
	if (!l)
		return NULL;
	pthread_mutex_init(&l->m, NULL);
	pthread_cond_init(&l->c, NULL);
	l->free = 1;
	return (HLOCK)l;
}

HSF_EXPORT void OsLockDestroy(HLOCK hLock)
{
	struct oslock *l = (struct oslock *)hLock;

	HSF_TRACE("OsLockDestroy", "%p", (void *)l);
	if (!l)
		return;
	pthread_mutex_destroy(&l->m);
	pthread_cond_destroy(&l->c);
	free(l);
}

HSF_EXPORT void OsLockLock(HLOCK hLock)
{
	struct oslock *l = (struct oslock *)hLock;
	pid_t me = hsf_gettid();

	HSF_TRACE_HOT("OsLockLock", "%p", (void *)l);
	if (!hsf_may_sleep())
		hsf_problem("OsLockLock(%p) in atomic context", (void *)l);
	pthread_mutex_lock(&l->m);
	if (!l->free && l->owner == me) {
		l->nest++;
	} else {
		while (!l->free)
			pthread_cond_wait(&l->c, &l->m);
		l->free = 0;
		l->owner = me;
	}
	pthread_mutex_unlock(&l->m);
}

HSF_EXPORT void OsLockUnlock(HLOCK hLock)
{
	struct oslock *l = (struct oslock *)hLock;

	HSF_TRACE_HOT("OsLockUnlock", "%p", (void *)l);
	pthread_mutex_lock(&l->m);
	if (l->nest) {
		l->nest--;
	} else {
		if (l->free)
			hsf_problem("OsLockUnlock(%p) on an unlocked lock", (void *)l);
		else if (l->owner != hsf_gettid())
			hsf_log(HSF_LOG_DEBUG, "OsLock %p released by %d, owner %d",
				(void *)l, hsf_gettid(), l->owner);
		l->owner = 0;
		l->free = 1;
		pthread_cond_signal(&l->c);
	}
	pthread_mutex_unlock(&l->m);
}

HSF_EXPORT BOOL OsLockTry(HLOCK hLock)
{
	struct oslock *l = (struct oslock *)hLock;
	BOOL ok = TRUE;

	HSF_TRACE_HOT("OsLockTry", "%p", (void *)l);
	pthread_mutex_lock(&l->m);
	if (!l->free) {
		if (l->owner == hsf_gettid())
			l->nest++;
		else
			ok = FALSE;
	} else {
		l->free = 0;
		l->owner = hsf_gettid();
	}
	pthread_mutex_unlock(&l->m);
	return ok;
}

HSF_EXPORT void OsLockTryUnlock(HLOCK hLock)
{
	OsLockUnlock(hLock);
}

/* ---- events -------------------------------------------------------------------- */

struct osevent {
	pthread_mutex_t m;
	pthread_cond_t c;
	int flag;
	const char *name;
};

HSF_EXPORT HOSEVENT OsEventCreate(const char *name)
{
	struct osevent *e = calloc(1, sizeof(*e));

	HSF_TRACE("OsEventCreate", "\"%s\") = (%p", name ? name : "", (void *)e);
	if (!e)
		return NULL;
	pthread_mutex_init(&e->m, NULL);
	hsf_cond_init(&e->c);
	e->name = name;
	return (HOSEVENT)e;
}

HSF_EXPORT void OsEventDestroy(HOSEVENT hEvent)
{
	struct osevent *e = (struct osevent *)hEvent;

	HSF_TRACE("OsEventDestroy", "%p", (void *)e);
	if (!e)
		return;
	pthread_mutex_destroy(&e->m);
	pthread_cond_destroy(&e->c);
	free(e);
}

HSF_EXPORT long OsEventSet(HOSEVENT hEvent)
{
	struct osevent *e = (struct osevent *)hEvent;
	int prev;

	HSF_TRACE_HOT("OsEventSet", "%p", (void *)e);
	pthread_mutex_lock(&e->m);
	prev = e->flag;
	e->flag = 1;
	pthread_cond_broadcast(&e->c);
	pthread_mutex_unlock(&e->m);
	return prev;
}

HSF_EXPORT long OsEventClear(HOSEVENT hEvent)
{
	struct osevent *e = (struct osevent *)hEvent;
	int prev;

	HSF_TRACE_HOT("OsEventClear", "%p", (void *)e);
	pthread_mutex_lock(&e->m);
	prev = e->flag;
	e->flag = 0;
	pthread_mutex_unlock(&e->m);
	return prev;
}

HSF_EXPORT long OsEventState(HOSEVENT hEvent)
{
	struct osevent *e = (struct osevent *)hEvent;
	int state;

	HSF_TRACE_HOT("OsEventState", "%p", (void *)e);
	pthread_mutex_lock(&e->m);
	state = e->flag;
	pthread_mutex_unlock(&e->m);
	return state;
}

HSF_EXPORT OSEVENT_WAIT_RESULT OsEventWaitTime(HOSEVENT hEvent, UINT32 timeout)
{
	struct osevent *e = (struct osevent *)hEvent;
	OSEVENT_WAIT_RESULT res = OSEVENT_WAIT_TIMEOUT;
	uint64_t deadline;

	HSF_TRACE_HOT("OsEventWaitTime", "%p \"%s\", %u", (void *)e, e && e->name ? e->name : "", timeout);
	if (timeout && !hsf_may_sleep()) {
		hsf_problem("OsEventWaitTime(%u) in atomic context", timeout);
		return OSEVENT_WAIT_ERROR;
	}
	deadline = hsf_now_ns() + (uint64_t)timeout * 1000000u;
	pthread_mutex_lock(&e->m);
	for (;;) {
		if (e->flag) {
			e->flag = 0;
			res = OSEVENT_WAIT_OK;
			break;
		}
		if (!timeout || hsf_cond_wait_until(&e->c, &e->m, deadline) == ETIMEDOUT) {
			if (e->flag) {
				e->flag = 0;
				res = OSEVENT_WAIT_OK;
			}
			break;
		}
	}
	pthread_mutex_unlock(&e->m);
	return res;
}

HSF_EXPORT void OsEventWait(HOSEVENT hEvent)
{
	struct osevent *e = (struct osevent *)hEvent;

	HSF_TRACE("OsEventWait", "%p", (void *)e);
	if (!hsf_may_sleep()) {
		hsf_problem("OsEventWait in atomic context");
		return;
	}
	pthread_mutex_lock(&e->m);
	while (!e->flag)
		pthread_cond_wait(&e->c, &e->m);
	e->flag = 0;
	pthread_mutex_unlock(&e->m);
}
