/* SPDX-License-Identifier: MIT */
/*
 * Time services: monotonic clock helpers, the TSC, sleeping and thread ids.
 */
#include "hsf_os.h"

#include <errno.h>
#include <time.h>
#include <x86intrin.h>

static uint64_t epoch_ns;
static DWORD cpu_mhz;

uint64_t hsf_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void hsf_cond_init(pthread_cond_t *c)
{
	pthread_condattr_t a;

	pthread_condattr_init(&a);
	pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
	pthread_cond_init(c, &a);
	pthread_condattr_destroy(&a);
}

int hsf_cond_wait_until(pthread_cond_t *c, pthread_mutex_t *m, uint64_t deadline_ns)
{
	struct timespec ts = {
		.tv_sec = (time_t)(deadline_ns / 1000000000u),
		.tv_nsec = (long)(deadline_ns % 1000000000u),
	};

	return pthread_cond_timedwait(c, m, &ts);
}

static DWORD calibrate_tsc_mhz(void)
{
	struct timespec a, b;
	uint64_t t0, t1;
	double ns;

	clock_gettime(CLOCK_MONOTONIC_RAW, &a);
	t0 = __rdtsc();
	do {
		clock_gettime(CLOCK_MONOTONIC_RAW, &b);
		ns = (double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec);
	} while (ns < 50e6);
	t1 = __rdtsc();
	return (DWORD)((double)(t1 - t0) / (ns / 1e3) + 0.5);
}

void hsf_time_init(void)
{
	epoch_ns = hsf_now_ns();
	cpu_mhz = calibrate_tsc_mhz();
}

DWORD hsf_cpu_mhz(void)
{
	return cpu_mhz;
}

HSF_EXPORT UINT32 OsGetSystemTime(void)
{
	HSF_TRACE_HOT("OsGetSystemTime", "");
	return (UINT32)((hsf_now_ns() - epoch_ns) / 1000000u);
}

HSF_EXPORT UINT32 OsReadCpuCnt(void)
{
	HSF_TRACE_HOT("OsReadCpuCnt", "");
	return (UINT32)__rdtsc();
}

HSF_EXPORT DWORD OsGetProcessorFreq(void)
{
	HSF_TRACE_HOT("OsGetProcessorFreq", "");
	return cpu_mhz;
}

HSF_EXPORT HTHREAD OsGetCurrentThread(void)
{
	/* The kernel returned 0 in interrupt context. */
	HSF_TRACE_HOT("OsGetCurrentThread", "");
	return (HTHREAD)(uintptr_t)(hsf_in_softirq ? 0 : hsf_gettid());
}

HSF_EXPORT void OsSleep(UINT32 ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000 };

	HSF_TRACE("OsSleep", "%u", ms);
	if (!hsf_may_sleep()) {
		/* The kernel busy-waited (mdelay) in atomic context. */
		uint64_t end = hsf_now_ns() + (uint64_t)ms * 1000000u;

		if (ms >= 100)
			hsf_problem("OsSleep(%u) in atomic context", ms);
		while (hsf_now_ns() < end)
			__builtin_ia32_pause();
		return;
	}
	while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR)
		;
}

HSF_EXPORT BOOL OsKernelUsesRegParm(void)
{
	/* i386 userspace uses plain cdecl, the convention the blobs were built
	 * with; this makes them hand us cdecl callbacks too. */
	HSF_TRACE("OsKernelUsesRegParm", "");
	return FALSE;
}
