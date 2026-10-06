/* SPDX-License-Identifier: MIT */
/*
 * Minimal TAP (Test Anything Protocol) helpers; meson runs the test
 * programs with protocol: 'tap'.
 */
#ifndef HSF_TAP_H
#define HSF_TAP_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tap_count, tap_failed;

static void tap_ok(int pass, const char *file, int line, const char *fmt, ...)
	__attribute__((format(printf, 4, 5), unused));
static void tap_ok(int pass, const char *file, int line, const char *fmt, ...)
{
	va_list ap;

	printf("%sok %d - ", pass ? "" : "not ", ++tap_count);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	if (!pass) {
		tap_failed++;
		printf("#   failed at %s:%d\n", file, line);
	}
	fflush(stdout);
}

#define ok(cond, ...) tap_ok(!!(cond), __FILE__, __LINE__, __VA_ARGS__)
#define is_int(got, want, ...)                                                   \
	do {                                                                     \
		long long g_ = (long long)(got), w_ = (long long)(want);         \
		tap_ok(g_ == w_, __FILE__, __LINE__, __VA_ARGS__);               \
		if (g_ != w_)                                                    \
			printf("#   got %lld, want %lld\n", g_, w_);             \
	} while (0)
#define is_str(got, want, ...)                                                   \
	do {                                                                     \
		const char *g_ = (got), *w_ = (want);                            \
		int p_ = g_ && w_ && !strcmp(g_, w_);                            \
		tap_ok(p_, __FILE__, __LINE__, __VA_ARGS__);                     \
		if (!p_)                                                         \
			printf("#   got \"%s\", want \"%s\"\n", g_ ? g_ : "(null)", w_ ? w_ : "(null)"); \
	} while (0)

static void tap_diag(const char *fmt, ...) __attribute__((format(printf, 1, 2), unused));
static void tap_diag(const char *fmt, ...)
{
	va_list ap;

	printf("# ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

static int tap_done(void) __attribute__((unused));
static int tap_done(void)
{
	printf("1..%d\n", tap_count);
	return tap_failed ? 1 : 0;
}

#endif /* HSF_TAP_H */
