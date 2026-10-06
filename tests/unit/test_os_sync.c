/* SPDX-License-Identifier: MIT */
/* Critical sections, OsLock and events: the legacy semantics the blobs expect. */
#include "hsf_os.h"
#include "../tap.h"

#include <unistd.h>

static void quiet_sink(int level, const char *line)
{
	(void)level;
	(void)line;
}

struct shared {
	HCRIT cs;
	HLOCK lock;
	HOSEVENT ev;
	volatile int stage;
	OSEVENT_WAIT_RESULT wait_result;
	uint64_t waited_ns;
};

static void *crit_contender(void *arg)
{
	struct shared *s = arg;

	OsCriticalSectionAcquire(s->cs);
	s->stage = 2;	/* only possible once the main thread released it */
	OsCriticalSectionRelease(s->cs);
	return NULL;
}

static void test_critical_sections(void)
{
	struct shared s = { .cs = OsCriticalSectionCreate() };
	pthread_t th;

	ok(s.cs != NULL, "OsCriticalSectionCreate");
	OsCriticalSectionAcquire(s.cs);
	ok(!hsf_may_sleep(), "holding a critical section is atomic context");
	OsCriticalSectionAcquire(s.cs);
	is_int(hsf_atomic_depth, 2, "critical sections nest on the owning thread");
	s.stage = 1;
	pthread_create(&th, NULL, crit_contender, &s);
	usleep(20000);
	is_int(s.stage, 1, "another thread cannot enter while it is held");
	OsCriticalSectionRelease(s.cs);
	usleep(20000);
	is_int(s.stage, 1, "still held after releasing one nesting level");
	OsCriticalSectionRelease(s.cs);
	pthread_join(th, NULL);
	is_int(s.stage, 2, "the other thread enters after the final release");
	ok(hsf_may_sleep(), "atomic context ends with the last release");

	hsf_problem_reset();
	HOSEVENT ev = OsEventCreate("poll");
	OsCriticalSectionAcquire(s.cs);
	is_int(OsEventWaitTime(ev, 0), OSEVENT_WAIT_TIMEOUT, "a zero-timeout poll is fine in atomic context");
	OsCriticalSectionRelease(s.cs);
	OsEventDestroy(ev);
	OsCriticalSectionDestroy(s.cs);
	OsCriticalSectionAcquire(NULL);	/* tolerated, as in the legacy code */
	OsCriticalSectionRelease(NULL);
	is_int(hsf_problem_count(), 0, "NULL critical sections are ignored");
}

static void *lock_releaser(void *arg)
{
	struct shared *s = arg;

	OsLockUnlock(s->lock);	/* releasing from a non-owner thread is legal */
	return NULL;
}

static void *lock_tryer(void *arg)
{
	struct shared *s = arg;

	s->stage = OsLockTry(s->lock) ? 10 : 20;
	return NULL;
}

static void test_locks(void)
{
	struct shared s = { .lock = OsLockCreate() };
	pthread_t th;

	ok(s.lock != NULL, "OsLockCreate");
	OsLockLock(s.lock);
	OsLockLock(s.lock);
	ok(OsLockTry(s.lock), "the owner may take its lock again (nesting)");
	pthread_create(&th, NULL, lock_tryer, &s);
	pthread_join(th, NULL);
	is_int(s.stage, 20, "OsLockTry fails for other threads while held");
	OsLockUnlock(s.lock);
	OsLockTryUnlock(s.lock);
	OsLockUnlock(s.lock);
	pthread_create(&th, NULL, lock_tryer, &s);
	pthread_join(th, NULL);
	is_int(s.stage, 10, "the lock is free after balanced unlocks");

	hsf_problem_reset();
	pthread_create(&th, NULL, lock_releaser, &s);
	pthread_join(th, NULL);
	ok(OsLockTry(s.lock), "a lock released by another thread can be taken again");
	OsLockUnlock(s.lock);
	is_int(hsf_problem_count(), 0, "non-owner release is not a problem");
	OsLockDestroy(s.lock);
}

static void *event_setter(void *arg)
{
	struct shared *s = arg;

	usleep(30000);
	OsEventSet(s->ev);
	return NULL;
}

static void *event_waiter(void *arg)
{
	struct shared *s = arg;
	uint64_t t0 = hsf_now_ns();

	s->wait_result = OsEventWaitTime(s->ev, 2000);
	s->waited_ns = hsf_now_ns() - t0;
	return NULL;
}

static void test_events(void)
{
	struct shared s = { .ev = OsEventCreate("test") };
	pthread_t th, th2;
	uint64_t t0;

	ok(s.ev != NULL, "OsEventCreate");
	is_int(OsEventState(s.ev), 0, "events start clear");
	is_int(OsEventSet(s.ev), 0, "OsEventSet returns the previous state (clear)");
	is_int(OsEventSet(s.ev), 1, "OsEventSet returns the previous state (set)");
	is_int(OsEventWaitTime(s.ev, 0), OSEVENT_WAIT_OK, "a zero-timeout poll consumes a set event");
	is_int(OsEventState(s.ev), 0, "a successful wait clears the event");
	is_int(OsEventWaitTime(s.ev, 0), OSEVENT_WAIT_TIMEOUT, "polling a clear event times out at once");
	OsEventSet(s.ev);
	is_int(OsEventClear(s.ev), 1, "OsEventClear returns the previous state");

	t0 = hsf_now_ns();
	is_int(OsEventWaitTime(s.ev, 50), OSEVENT_WAIT_TIMEOUT, "waiting on a clear event times out");
	ok(hsf_now_ns() - t0 >= 50000000u, "the timeout lasts at least the requested 50 ms");

	pthread_create(&th, NULL, event_waiter, &s);
	pthread_create(&th2, NULL, event_setter, &s);
	pthread_join(th, NULL);
	pthread_join(th2, NULL);
	is_int(s.wait_result, OSEVENT_WAIT_OK, "a waiter wakes when another thread sets the event");
	ok(s.waited_ns < 1000000000u, "and does not wait for the full timeout");

	hsf_problem_reset();
	hsf_atomic_depth++;
	is_int(OsEventWaitTime(s.ev, 10), OSEVENT_WAIT_ERROR, "a timed wait in atomic context returns ERROR");
	hsf_atomic_depth--;
	is_int(hsf_problem_count(), 1, "and is reported");
	OsEventDestroy(s.ev);
}

int main(void)
{
	struct hsf_os_config cfg = HSF_OS_CONFIG_DEFAULT;

	cfg.bios_source = "zeros";
	hsf_log_set_sink(quiet_sink);
	if (hsf_os_init(&cfg)) {
		printf("Bail out! OS layer init failed\n");
		return 1;
	}
	test_critical_sections();
	test_locks();
	test_events();
	hsf_os_shutdown();
	return tap_done();
}
