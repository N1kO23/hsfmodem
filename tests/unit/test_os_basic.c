/* SPDX-License-Identifier: MIT */
/* Memory, string, atomic and time services of the OS layer. */
#include "hsf_os.h"
#include "../tap.h"

#include <unistd.h>

static int captured_problems;

static void quiet_sink(int level, const char *line)
{
	(void)line;
	if (level == HSF_LOG_ERR)
		captured_problems++;
}

static void test_memory(void)
{
	long base = hsf_live_allocations();
	unsigned char *p = OsAllocate(100);
	int zero = 1;

	ok(p != NULL, "OsAllocate(100) succeeds");
	for (int i = 0; i < 100; i++)
		zero &= p[i] == 0;
	ok(zero, "OsAllocate returns zeroed memory");
	ok(((uintptr_t)p & 127) == 0, "a 100-byte block is aligned to 128 like kmalloc");
	is_int(hsf_live_allocations() - base, 1, "one live block while allocated");
	OsFree(p);
	is_int(hsf_live_allocations() - base, 0, "no live blocks after OsFree");

	void *big = OsAllocate(70000);
	ok(big && ((uintptr_t)big & 4095) == 0, "large blocks are page aligned");
	OsFree(big);

	void *tiny = OsAllocate(1);
	ok(tiny && ((uintptr_t)tiny & 15) == 0, "small blocks are 16-byte aligned");
	OsFree(tiny);

	hsf_problem_reset();
	unsigned char *q = OsAllocate(8);
	q[8] = 0xAA;	/* overrun into the canary */
	OsFree(q);
	is_int(hsf_problem_count(), 1, "writing past the end of a block is reported on OsFree");
	unsigned char *foreign = calloc(1, 64);
	OsFree(foreign + 48);	/* header area holds no OsAllocate magic */
	free(foreign);
	is_int(hsf_problem_count(), 2, "freeing a pointer OsAllocate never returned is reported, not executed");

	PUINT16 phys, buf;
	HANDLE handle;
	ok(OsMemDMAAllocate(1, &phys, &handle, &buf), "OsMemDMAAllocate(order 1) succeeds");
	ok(((uintptr_t)buf & 0xFFFF) == 0, "DMA buffers are 64 KiB aligned like the legacy allocator");
	ok(OsMemDMAFree(handle, 1), "OsMemDMAFree succeeds");
	ok(!OsMemDMAFree(NULL, 1), "OsMemDMAFree(NULL) fails");
}

static void test_strings(void)
{
	char buf[64];

	is_int(OsAtoi("10"), 16, "OsAtoi parses hexadecimal");
	is_int(OsAtoi("ff"), 255, "OsAtoi accepts lower-case hex");
	is_int(OsAtoi("1z"), 16, "OsAtoi shifts in 0 for non-hex characters");
	is_int(OsToupper('a'), 'A', "OsToupper");
	is_int(OsToupper('1'), '1', "OsToupper leaves non-letters");
	is_int(OsTolower('Q'), 'q', "OsTolower");
	ok(OsIsDigit('7') && !OsIsDigit('a'), "OsIsDigit");
	is_int(OsSprintf(buf, "%s-%02X", "ab", 10), 5, "OsSprintf returns the length");
	is_str(buf, "ab-0A", "OsSprintf formats like printf");
	OsStrCpy(buf, "abc");
	OsStrCat(buf, "def");
	is_str(buf, "abcdef", "OsStrCpy/OsStrCat");
	is_int(OsStrLen(buf), 6, "OsStrLen");
	ok(OsStrCmp("a", "b") < 0 && OsMemCmp("ab", "ab", 2) == 0, "OsStrCmp/OsMemCmp");
}

static void test_atomics(void)
{
	INT32 v = 5;
	uint8_t b = 1;
	uint16_t w = 2;
	uint32_t d = 3;
	uint64_t q = 4;

	is_int(OsAtomicAdd(3, &v), 8, "OsAtomicAdd returns the new value");
	is_int(OsAtomicIncrement(&v), 9, "OsAtomicIncrement returns the new value");
	is_int(OsAtomicDecrement(&v), 8, "OsAtomicDecrement returns the new value");
	ok(OsAtomicCompareAndSwapEx((PVOID)1, (PVOID)7, (PVOID *)&b, 1) && b == 7, "CAS on 1 byte");
	ok(!OsAtomicCompareAndSwapEx((PVOID)1, (PVOID)9, (PVOID *)&b, 1) && b == 7, "failing CAS leaves the value");
	ok(OsAtomicCompareAndSwapEx((PVOID)2, (PVOID)0x1234, (PVOID *)&w, 2) && w == 0x1234, "CAS on 2 bytes");
	ok(OsAtomicCompareAndSwapEx((PVOID)3, (PVOID)0x12345678, (PVOID *)&d, 4) && d == 0x12345678, "CAS on 4 bytes");
	ok(OsAtomicCompareAndSwapEx((PVOID)(uintptr_t)4, (PVOID)(uintptr_t)5, (PVOID *)&q, 8) && q == 5, "CAS on 8 bytes");
}

static void test_time(void)
{
	UINT32 t0 = OsGetSystemTime(), c0 = OsReadCpuCnt();

	OsSleep(30);
	UINT32 dt = OsGetSystemTime() - t0;
	ok(dt >= 30 && dt < 200, "OsSleep(30) sleeps about 30 ms (%u ms)", dt);
	ok(OsReadCpuCnt() != c0, "OsReadCpuCnt advances");
	ok(OsGetProcessorFreq() > 100, "OsGetProcessorFreq reports MHz (%u)", OsGetProcessorFreq());
	is_int((uintptr_t)OsGetCurrentThread(), hsf_gettid(), "OsGetCurrentThread is the thread id outside interrupts");
	ok(!OsKernelUsesRegParm(), "OsKernelUsesRegParm is FALSE in userspace");
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
	test_memory();
	test_strings();
	test_atomics();
	test_time();
	hsf_os_shutdown();
	return tap_done();
}
