/* SPDX-License-Identifier: MIT */
/* Worker threads, work items, timers and FPU isolation. */
#include "hsf_os.h"
#include "../tap.h"

#include <unistd.h>

static void quiet_sink(int level, const char *line)
{
	(void)level;
	(void)line;
}

/* ---- work items --------------------------------------------------------------- */

static int order[8], norder;
static pid_t work_tid;

static void record(void *arg)
{
	order[norder++] = (int)(intptr_t)arg;
	work_tid = hsf_gettid();
	usleep(5000);
}

static OSSCHED requeue_item;
static int requeue_runs;
static POSTHRD requeue_thread;

static void requeue(void *arg)
{
	(void)arg;
	if (++requeue_runs < 3)
		OsThreadSchedule(requeue_thread, &requeue_item);
}

static void test_work_items(void)
{
	OSSCHED w[3];
	int pid = 0;
	POSTHRD t = OsThreadCreate("test", FALSE, &pid);

	ok(t != NULL && pid > 0, "OsThreadCreate returns a thread and its id");
	for (int i = 0; i < 3; i++)
		OsThreadScheduleInit(&w[i], record, (void *)(intptr_t)(i + 1));
	is_int(OsThreadSchedule(t, &w[0]), 1, "scheduling an idle item returns 1");
	is_int(OsThreadSchedule(t, &w[1]), 1, "scheduling a second item returns 1");
	is_int(OsThreadSchedule(t, &w[1]), 0, "scheduling an already queued item returns 0");
	OsThreadSchedule(t, &w[2]);
	usleep(100000);
	is_int(norder, 3, "each queued item ran once");
	ok(order[0] == 1 && order[1] == 2 && order[2] == 3, "items run in FIFO order");
	is_int(work_tid, pid, "items run on the worker thread");

	requeue_thread = t;
	OsThreadScheduleInit(&requeue_item, requeue, NULL);
	OsThreadSchedule(t, &requeue_item);
	usleep(50000);
	is_int(requeue_runs, 3, "an item may queue itself again while running");
	OsThreadDestroy(t);
	ok(1, "OsThreadDestroy joins the worker");
}

/* ---- periodic timers ---------------------------------------------------------- */

struct tick {
	int count;
	uint64_t first_ns, last_ns;
	pid_t tid;
	HOSTIMER self;
	int destroy_at;
	bool atomic_seen;
};

HSF_EXPORT static void tick_cb(PVOID arg)
{
	struct tick *t = arg;

	if (!t->count)
		t->first_ns = hsf_now_ns();
	t->last_ns = hsf_now_ns();
	t->tid = (pid_t)(uintptr_t)OsGetCurrentThread();
	t->atomic_seen |= !hsf_may_sleep();
	if (++t->count == t->destroy_at)
		OsDestroyPeriodicTimer(t->self);
}

static void test_periodic_timers(void)
{
	struct tick t = { 0 };
	HTHREAD thread_id = NULL;
	uint64_t created;
	HOSTIMER h;

	created = hsf_now_ns();
	h = OsCreatePeriodicTimer(20, tick_cb, NULL, NULL, &t, &thread_id);
	ok(h != NULL, "OsCreatePeriodicTimer");
	usleep(210000);
	ok(t.count > 0 && t.first_ns - created < 15000000u, "the first expiry is immediate");
	ok(t.count >= 8 && t.count <= 12, "a 20 ms timer fires about 10 times in 210 ms (%d)", t.count);
	is_int(t.tid, (pid_t)(uintptr_t)thread_id, "callbacks run on the thread whose id was returned");
	ok(!t.atomic_seen, "periodic callbacks may sleep (they run on the modem thread)");

	OsSetPeriodicTimer(h, 0);
	int frozen = t.count;
	usleep(60000);
	ok(t.count <= frozen + 1, "a zero period stops the timer");
	ok(OsSetPeriodicTimer(h, 10), "setting a new period restarts it");
	usleep(60000);
	ok(t.count > frozen + 2, "and it fires again");
	OsDestroyPeriodicTimer(h);
	int after = t.count;
	usleep(50000);
	is_int(t.count, after, "no callbacks after OsDestroyPeriodicTimer returns");

	struct tick self = { .destroy_at = 3 };
	self.self = OsCreatePeriodicTimer(5, tick_cb, NULL, NULL, &self, NULL);
	usleep(100000);
	is_int(self.count, 3, "a timer may destroy itself from its own callback");
	is_int(hsf_periodic_timer_count(), 0, "every periodic timer has been freed");
}

static int custom_allocs, custom_frees;

HSF_EXPORT static PVOID custom_alloc(unsigned size, PVOID ref)
{
	(void)ref;
	custom_allocs++;
	return calloc(1, size);
}

HSF_EXPORT static void custom_free(PVOID p, PVOID ref)
{
	(void)ref;
	custom_frees++;
	free(p);
}

static void test_timer_allocator(void)
{
	struct tick t = { 0 };
	HOSTIMER h = OsCreatePeriodicTimer(10, tick_cb, custom_alloc, custom_free, &t, NULL);

	usleep(30000);
	OsDestroyPeriodicTimer(h);
	ok(custom_allocs == 1 && custom_frees == 1, "a caller-supplied allocator is used for the timer");
}

/* ---- one-shot timers ------------------------------------------------------------ */

struct shot {
	int fired;
	bool softirq_context;
	pid_t reported_tid;
};

static void shot_cb(void *arg)
{
	struct shot *s = arg;

	s->fired++;
	s->softirq_context = hsf_in_softirq && !hsf_may_sleep();
	s->reported_tid = (pid_t)(uintptr_t)OsGetCurrentThread();
}

static void test_one_shot_timers(void)
{
	struct shot s = { 0 };
	HANDLE h = OsCreateTimer(20, (PVOID)shot_cb, &s);

	ok(h != NULL, "OsCreateTimer");
	usleep(40000);
	is_int(s.fired, 0, "a one-shot timer does not start until OsSetTimer");
	OsSetTimer(h);
	usleep(10000);
	is_int(s.fired, 0, "it waits for its timeout");
	usleep(30000);
	is_int(s.fired, 1, "and then fires once");
	ok(s.softirq_context, "the callback runs in softirq (atomic) context");
	is_int(s.reported_tid, 0, "OsGetCurrentThread is 0 in that context, as in an interrupt");

	OsSetTimer(h);
	OsCancelTimer(h);
	usleep(40000);
	is_int(s.fired, 1, "OsCancelTimer prevents a pending expiry");
	OsChangeTimerTimeOut(h, 5);
	OsSetTimer(h);
	usleep(20000);
	is_int(s.fired, 2, "OsChangeTimerTimeOut sets the timeout for the next OsSetTimer");
	OsDestroyTimer(h);
}

/* ---- FPU isolation ------------------------------------------------------------------- */

static unsigned short fpu_cw(void)
{
	unsigned short cw;

	__asm__ volatile("fnstcw %0" : "=m"(cw));
	return cw;
}

static void set_fpu_cw(unsigned short cw)
{
	__asm__ volatile("fldcw %0" : : "m"(cw));
}

static void test_fpu(void)
{
	unsigned short custom = 0x0C7F;	/* round toward zero, 24-bit precision */
	int id, inner;

	set_fpu_cw(custom);
	id = OsFloatPrefix();
	is_int(fpu_cw(), 0x037F, "OsFloatPrefix gives the blob a freshly initialised x87");
	inner = OsFloatPrefix();
	is_int(inner, id + 1, "sections nest");
	ok(OsFloatSuffix(inner), "inner OsFloatSuffix");
	ok(OsFloatSuffix(id), "outer OsFloatSuffix");
	is_int(fpu_cw(), custom, "the caller's control word is restored afterwards");
	set_fpu_cw(0x037F);

	hsf_problem_reset();
	ok(!OsFloatSuffix(5), "an unmatched OsFloatSuffix is refused");
	is_int(hsf_problem_count(), 1, "and reported");
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
	test_work_items();
	test_periodic_timers();
	test_timer_allocator();
	test_one_shot_timers();
	test_fpu();
	hsf_os_shutdown();
	return tap_done();
}
