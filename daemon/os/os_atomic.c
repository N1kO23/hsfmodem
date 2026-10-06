/* SPDX-License-Identifier: MIT */
/*
 * Atomic operations. Add/increment/decrement return the new value;
 * compare-and-swap supports 1, 2, 4 and 8-byte targets and returns TRUE when
 * the new value was stored.
 */
#include "hsf_os.h"

HSF_EXPORT INT32 OsAtomicAdd(INT32 amount, INT32 *address)
{
	HSF_TRACE_HOT("OsAtomicAdd", "%d, %p", amount, (void *)address);
	return __atomic_add_fetch(address, amount, __ATOMIC_SEQ_CST);
}

HSF_EXPORT INT32 OsAtomicIncrement(INT32 *address)
{
	HSF_TRACE_HOT("OsAtomicIncrement", "%p", (void *)address);
	return __atomic_add_fetch(address, 1, __ATOMIC_SEQ_CST);
}

HSF_EXPORT INT32 OsAtomicDecrement(INT32 *address)
{
	HSF_TRACE_HOT("OsAtomicDecrement", "%p", (void *)address);
	return __atomic_sub_fetch(address, 1, __ATOMIC_SEQ_CST);
}

#define CAS(type)                                                                \
	do {                                                                     \
		type expected = (type)o;                                         \
		return __atomic_compare_exchange_n((type *)address, &expected, (type)n, \
						   0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
	} while (0)

HSF_EXPORT BOOL OsAtomicCompareAndSwapEx(PVOID oldValue, PVOID newValue, PVOID *address, INT size)
{
	uintptr_t o = (uintptr_t)oldValue, n = (uintptr_t)newValue;

	HSF_TRACE_HOT("OsAtomicCompareAndSwapEx", "%p, %p, %p, %d", oldValue, newValue, (void *)address, size);
	switch (size) {
	case 1:
		CAS(uint8_t);
	case 2:
		CAS(uint16_t);
	case 4:
		CAS(uint32_t);
	case 8:
		CAS(uint64_t);
	default:
		/* the legacy code logged and reported success */
		hsf_problem("OsAtomicCompareAndSwapEx: unsupported size %d", size);
		return TRUE;
	}
}
