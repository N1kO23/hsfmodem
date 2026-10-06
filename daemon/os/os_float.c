/* SPDX-License-Identifier: MIT */
/*
 * FPU isolation for blob floating-point sections.
 *
 * The blobs use x87 arithmetic and bracket it with OsFloatPrefix/Suffix.
 * As the kernel did, save the caller's FPU/SSE state, give the blob a freshly
 * initialised FPU (default control word, MXCSR 0x1f80), and restore the
 * caller's state afterwards. Sections may nest; the returned id identifies
 * the level.
 */
#include "hsf_os.h"

#define FPU_NEST_MAX 8

struct fxarea {
	unsigned char b[512];
} __attribute__((aligned(16)));

static __thread struct fxarea fpu_save[FPU_NEST_MAX];
static __thread int fpu_depth;

HSF_EXPORT int OsFloatPrefix(void)
{
	int id = fpu_depth;
	unsigned int mxcsr = 0x1f80;

	HSF_TRACE_HOT("OsFloatPrefix", "");
	if (id >= FPU_NEST_MAX) {
		hsf_problem("OsFloatPrefix nested more than %d deep", FPU_NEST_MAX);
		return id;
	}
#ifdef __x86_64__
	__asm__ volatile("fxsave64 %0" : "=m"(fpu_save[id]));
#else
	__asm__ volatile("fxsave %0" : "=m"(fpu_save[id]));
#endif
	/* abridged x87 tag word: a non-empty register stack means someone
	 * left values behind */
	if (fpu_save[id].b[4])
		hsf_problem("OsFloatPrefix: x87 stack not empty (tags %02x)", fpu_save[id].b[4]);
	__asm__ volatile("fninit");
	__asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
	fpu_depth++;
	return id;
}

HSF_EXPORT BOOL OsFloatSuffix(int Id)
{
	HSF_TRACE_HOT("OsFloatSuffix", "%d", Id);
	if (Id < 0 || Id >= FPU_NEST_MAX || Id != fpu_depth - 1) {
		hsf_problem("OsFloatSuffix(%d) does not match OsFloatPrefix (depth %d)", Id, fpu_depth);
		return FALSE;
	}
	fpu_depth--;
#ifdef __x86_64__
	__asm__ volatile("fxrstor64 %0" : : "m"(fpu_save[Id]));
#else
	__asm__ volatile("fxrstor %0" : : "m"(fpu_save[Id]));
#endif
	return TRUE;
}
