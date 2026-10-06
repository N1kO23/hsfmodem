/* SPDX-License-Identifier: MIT */
/*
 * Internal interface of the userspace OS layer.
 *
 * The Conexant blobs call the Os* functions declared in their own headers
 * (osservices.h and friends); docs/OS-LAYER.md specifies their behaviour.
 * This header adds what the rest of the daemon and the tests need on top:
 * initialisation, logging and tracing, the execution-context model, time
 * helpers and the hooks other subsystems register with.
 */
#ifndef HSF_OS_H
#define HSF_OS_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include "hsf_blob_abi.h"

/* ---- lifecycle -------------------------------------------------------- */

struct hsf_os_config {
	int trace;		/* 0 off, 1 calls, 2 also frequent calls */
	bool realtime;		/* try SCHED_FIFO for high-priority threads */
	int rt_priority;	/* SCHED_FIFO priority of the modem thread */
	const char *bios_source; /* "auto", "devmem" or "zeros" */
};

#define HSF_OS_CONFIG_DEFAULT { .trace = 0, .realtime = false, .rt_priority = 40, .bios_source = "auto" }

int hsf_os_init(const struct hsf_os_config *cfg);
void hsf_os_shutdown(void);

/* ---- logging ------------------------------------------------------------ */

enum hsf_log_level { HSF_LOG_ERR, HSF_LOG_WARN, HSF_LOG_INFO, HSF_LOG_DEBUG };

void hsf_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void hsf_vlog(int level, const char *prefix, const char *fmt, va_list ap);
void hsf_log_set_level(int level);
/* Route log lines elsewhere (tests capture them); NULL restores stderr. */
void hsf_log_set_sink(void (*sink)(int level, const char *line));

/* An invariant the blob relies on was violated: counted and logged. */
void hsf_problem(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int hsf_problem_count(void);
void hsf_problem_reset(void);

/* ---- call tracing and inventory -------------------------------------------- */

extern int hsf_trace_level;
extern __thread const char *hsf_last_call;

void hsf_count_call(const char *name, int *slot);
unsigned long hsf_call_count(const char *name);
void hsf_inventory_dump(void);

#define HSF_COUNT(name)                                                         \
	do {                                                                    \
		static int hsf_slot_;                                           \
		hsf_count_call((name), &hsf_slot_);                             \
		hsf_last_call = (name);                                         \
	} while (0)
/* TRACE logs at trace level 1, TRACE_HOT (frequent calls) at level 2. */
#define HSF_TRACE(name, fmt, ...)                                               \
	do {                                                                    \
		HSF_COUNT(name);                                                \
		if (hsf_trace_level > 0)                                        \
			hsf_log(HSF_LOG_DEBUG, "%s(" fmt ")", (name), ##__VA_ARGS__); \
	} while (0)
#define HSF_TRACE_HOT(name, fmt, ...)                                           \
	do {                                                                    \
		HSF_COUNT(name);                                                \
		if (hsf_trace_level > 1)                                        \
			hsf_log(HSF_LOG_DEBUG, "%s(" fmt ")", (name), ##__VA_ARGS__); \
	} while (0)

/* ---- execution context ---------------------------------------------------
 *
 * The kernel glue refused to sleep in atomic context: inside a critical
 * section (interrupts disabled) or in a timer softirq. The blobs may depend
 * on that, so the depth is tracked per thread.
 */
extern __thread int hsf_atomic_depth;
extern __thread bool hsf_in_softirq;

static inline bool hsf_may_sleep(void)
{
	return hsf_atomic_depth == 0;
}

pid_t hsf_gettid(void);

/* ---- time -------------------------------------------------------------- */

uint64_t hsf_now_ns(void);
void hsf_cond_init(pthread_cond_t *c);	/* CLOCK_MONOTONIC */
/* pthread_cond_timedwait with an absolute hsf_now_ns() deadline. */
int hsf_cond_wait_until(pthread_cond_t *c, pthread_mutex_t *m, uint64_t deadline_ns);

/* ---- threads ------------------------------------------------------------ */

int hsf_thread_start(pthread_t *th, const char *name, void *(*fn)(void *), void *arg);
/* Called first thing in every thread we create (alternate signal stack). */
void hsf_thread_enter(const char *name);
void hsf_thread_set_rt(pthread_t th, int priority_offset, const char *name);
void hsf_threads_configure_rt(bool enabled, int base_priority);
pid_t hsf_thread_tid(POSTHRD t);
/* The shared "modem" worker (OsMdmThread), created on first use. */
POSTHRD hsf_modem_thread(void);
int hsf_periodic_timer_count(void);

/* Install the crash reporter (fatal signals -> symbol, cause, last Os call). */
void hsf_fault_install(void);

/* ---- memory --------------------------------------------------------------- */

long hsf_live_allocations(void);

/* ---- physical memory window (see docs/BLOB-INTEGRATION.md) ----------------- */

int hsf_physmem_init(const char *source);
void hsf_physmem_shutdown(void);

/* ---- PCI configuration space reachable through OsPciReadConfig* ------------ */

void hsf_pci_register(void *handle, const uint8_t config[256]);
void hsf_pci_unregister(void *handle);
/* Copy out a registered snapshot; false if the handle is unknown. */
bool hsf_pci_lookup(void *handle, uint8_t config[256]);

/* ---- NVM ------------------------------------------------------------------- */

void hsf_nvm_set_dirs(const char *static_dir, const char *dynamic_dir);

/* ---- internal (os_timer.c / os_thread.c) ----------------------------------- */

int hsf_timers_init(void);
void hsf_timers_shutdown(void);
void hsf_threads_shutdown(void);
DWORD hsf_cpu_mhz(void);
void hsf_time_init(void);

#endif /* HSF_OS_H */
