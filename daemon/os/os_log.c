/* SPDX-License-Identifier: MIT */
/*
 * Logging, invariant ("problem") reporting and Os* call tracing.
 */
#include "hsf_os.h"

#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int hsf_trace_level;
__thread const char *hsf_last_call = "(none)";
__thread int hsf_atomic_depth;
__thread bool hsf_in_softirq;

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int log_level = HSF_LOG_INFO;
static void (*log_sink)(int level, const char *line);
static int problems;
static uint64_t log_epoch_ns;

pid_t hsf_gettid(void)
{
	static __thread pid_t tid;

	if (!tid)
		tid = (pid_t)syscall(SYS_gettid);
	return tid;
}

void hsf_log_set_level(int level)
{
	log_level = level;
}

void hsf_log_set_sink(void (*sink)(int level, const char *line))
{
	pthread_mutex_lock(&log_lock);
	log_sink = sink;
	pthread_mutex_unlock(&log_lock);
}

void hsf_vlog(int level, const char *prefix, const char *fmt, va_list ap)
{
	static const char *const tags[] = { "E", "W", "I", "D" };
	char msg[1024], line[1200];
	size_t len;

	if (level > log_level && !(level == HSF_LOG_DEBUG && hsf_trace_level))
		return;
	if (!log_epoch_ns)
		log_epoch_ns = hsf_now_ns();
	vsnprintf(msg, sizeof(msg), fmt, ap);
	len = strlen(msg);
	while (len && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
		msg[--len] = '\0';
	snprintf(line, sizeof(line), "[%10.3f %6d%c] %s %s%s",
		 (hsf_now_ns() - log_epoch_ns) / 1e6, (int)hsf_gettid(),
		 hsf_in_softirq ? 's' : hsf_atomic_depth ? 'a' : ' ',
		 tags[level < 0 ? 0 : level > 3 ? 3 : level], prefix, msg);

	pthread_mutex_lock(&log_lock);
	if (log_sink)
		log_sink(level, line);
	else
		fprintf(stderr, "%s\n", line);
	pthread_mutex_unlock(&log_lock);
}

void hsf_log(int level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	hsf_vlog(level, "", fmt, ap);
	va_end(ap);
}

void hsf_problem(const char *fmt, ...)
{
	va_list ap;

	__atomic_add_fetch(&problems, 1, __ATOMIC_RELAXED);
	va_start(ap, fmt);
	hsf_vlog(HSF_LOG_ERR, "PROBLEM: ", fmt, ap);
	va_end(ap);
}

int hsf_problem_count(void)
{
	return __atomic_load_n(&problems, __ATOMIC_RELAXED);
}

void hsf_problem_reset(void)
{
	__atomic_store_n(&problems, 0, __ATOMIC_RELAXED);
}

/* ---- call inventory ------------------------------------------------------ */

#define MAX_COUNTED 192

static struct {
	const char *name;
	unsigned long calls;
} counted[MAX_COUNTED];
static int ncounted;
static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;

void hsf_count_call(const char *name, int *slot)
{
	int i = __atomic_load_n(slot, __ATOMIC_ACQUIRE);

	if (!i) {
		pthread_mutex_lock(&count_lock);
		i = *slot;
		if (!i && ncounted < MAX_COUNTED) {
			counted[ncounted].name = name;
			i = ++ncounted;
			__atomic_store_n(slot, i, __ATOMIC_RELEASE);
		}
		pthread_mutex_unlock(&count_lock);
		if (!i)
			return;
	}
	__atomic_add_fetch(&counted[i - 1].calls, 1, __ATOMIC_RELAXED);
}

unsigned long hsf_call_count(const char *name)
{
	unsigned long n = 0;

	pthread_mutex_lock(&count_lock);
	for (int i = 0; i < ncounted; i++)
		if (!strcmp(counted[i].name, name))
			n += __atomic_load_n(&counted[i].calls, __ATOMIC_RELAXED);
	pthread_mutex_unlock(&count_lock);
	return n;
}

void hsf_inventory_dump(void)
{
	pthread_mutex_lock(&count_lock);
	hsf_log(HSF_LOG_INFO, "Os call inventory (%d functions called):", ncounted);
	for (int i = 0; i < ncounted; i++)
		hsf_log(HSF_LOG_INFO, "  %-30s %10lu", counted[i].name, counted[i].calls);
	pthread_mutex_unlock(&count_lock);
}

/* ---- blob-facing printf family -------------------------------------------- */

HSF_EXPORT void OsErrorPrintf(LPCSTR szFormat, ...)
{
	va_list ap;

	HSF_COUNT("OsErrorPrintf");
	va_start(ap, szFormat);
	hsf_vlog(HSF_LOG_WARN, "engine: ", szFormat, ap);
	va_end(ap);
}

HSF_EXPORT void OsDebugPrintf(LPCSTR szFormat, ...)
{
	va_list ap;

	HSF_COUNT("OsDebugPrintf");
	va_start(ap, szFormat);
	hsf_vlog(HSF_LOG_DEBUG, "engine: ", szFormat, ap);
	va_end(ap);
}

HSF_EXPORT void OsDebugBreakpoint(LPCSTR szMsg)
{
	HSF_COUNT("OsDebugBreakpoint");
	hsf_problem("OsDebugBreakpoint: %s", szMsg ? szMsg : "(null)");
}
