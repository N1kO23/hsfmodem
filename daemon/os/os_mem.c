/* SPDX-License-Identifier: MIT */
/*
 * Memory for the blobs.
 *
 * kmalloc handed out power-of-two-sized objects at their natural alignment,
 * and blob code may depend on that, so OsAllocate aligns to the next power of
 * two (16 bytes to one page). Every block carries a header and a trailing
 * canary, so bad frees and overruns are reported, and live blocks are
 * counted for leak checks.
 */
#include "hsf_os.h"

#include <stdlib.h>
#include <string.h>

#define MEM_MAGIC 0x48534641u	/* "HSFA" */
#define MEM_FREED 0xdeadbeefu
#define CANARY 0x5a5a5a5a5a5a5a5aull

struct memhdr {
	uint32_t magic;
	uint32_t align;
	size_t size;
	size_t offset;
	size_t pad;
};

static long live_blocks;

static size_t natural_align(size_t size)
{
	size_t a = 16;

	while (a < size && a < 4096)
		a <<= 1;
	return a;
}

static void *mem_alloc(size_t size, size_t align)
{
	size_t total = sizeof(struct memhdr) + align + size + sizeof(uint64_t);
	unsigned char *raw = malloc(total), *p;
	struct memhdr *h;
	uint64_t canary = CANARY;

	if (!raw)
		return NULL;
	p = (unsigned char *)(((uintptr_t)raw + sizeof(*h) + align - 1) & ~(uintptr_t)(align - 1));
	h = (struct memhdr *)p - 1;
	h->magic = MEM_MAGIC;
	h->align = (uint32_t)align;
	h->size = size;
	h->offset = (size_t)(p - raw);
	memset(p, 0, size);
	memcpy(p + size, &canary, sizeof(canary));
	__atomic_add_fetch(&live_blocks, 1, __ATOMIC_RELAXED);
	return p;
}

static void mem_free(void *ptr, const char *who)
{
	struct memhdr *h;
	uint64_t canary;

	if (!ptr) {
		hsf_problem("%s(NULL)", who);
		return;
	}
	h = (struct memhdr *)ptr - 1;
	if (h->magic != MEM_MAGIC) {
		hsf_problem("%s(%p): not an OsAllocate block or freed twice (magic %08x)", who, ptr, h->magic);
		return;
	}
	memcpy(&canary, (unsigned char *)ptr + h->size, sizeof(canary));
	if (canary != CANARY)
		hsf_problem("%s(%p): write past the end of a %zu-byte block", who, ptr, h->size);
	h->magic = MEM_FREED;
	__atomic_sub_fetch(&live_blocks, 1, __ATOMIC_RELAXED);
	free((unsigned char *)ptr - h->offset);
}

long hsf_live_allocations(void)
{
	return __atomic_load_n(&live_blocks, __ATOMIC_RELAXED);
}

HSF_EXPORT void *OsAllocate(unsigned size)
{
	void *p = mem_alloc(size, natural_align(size));

	HSF_TRACE_HOT("OsAllocate", "%u) = (%p", size, p);
	if (!p)
		hsf_problem("OsAllocate(%u) failed", size);
	return p;
}

HSF_EXPORT void OsFree(void *ptr)
{
	HSF_TRACE_HOT("OsFree", "%p", ptr);
	mem_free(ptr, "OsFree");
}

HSF_EXPORT BOOL OsMemDMAAllocate(UINT32 nPages, PUINT16 *ppPhysAddr, HANDLE *pMemHandle, PUINT16 *ppBuffer)
{
	/* nPages is a page order (as for __get_dma_pages) and the legacy code
	 * looked for a 64 KiB-aligned block. The engine uses these buffers
	 * internally, even on HDA hardware, so no device DMA is involved. */
	size_t size = (size_t)4096 << nPages;
	void *p;

	HSF_TRACE("OsMemDMAAllocate", "order %u", nPages);
	if (nPages > 8)
		return FALSE;
	p = mem_alloc(size, size > 65536 ? size : 65536);
	if (!p)
		return FALSE;
	*ppBuffer = p;
	*ppPhysAddr = p;
	if (pMemHandle)
		*pMemHandle = p;
	return TRUE;
}

HSF_EXPORT BOOL OsMemDMAFree(HANDLE MemHandle, UINT32 nPages)
{
	HSF_TRACE("OsMemDMAFree", "%p, %u", MemHandle, nPages);
	if (!MemHandle)
		return FALSE;
	mem_free(MemHandle, "OsMemDMAFree");
	return TRUE;
}
