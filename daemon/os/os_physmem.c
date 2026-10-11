/* SPDX-License-Identifier: MIT */
/*
 * Physical-memory window.
 *
 * The engine reads physical memory as OS_DEVNODE.osPageOffset + phys, which
 * was the kernel's direct map. It scans the BIOS area 0xE0000-0xFFFFF for a
 * DMI entry point and walks the structure table it points to; the result
 * feeds the HARDWARE_ID that Linuxant license keys are bound to. With
 * osPageOffset = 0, the daemon maps that window read-only at its identity
 * address. Sources:
 *   "devmem"     the real BIOS area from /dev/mem (root, no kernel lockdown)
 *   "zeros"      an empty window (tests, or when nothing else is readable)
 *   "file:PATH"  a 128 KiB image, e.g. captured on the target machine
 *   "auto"       devmem if readable, otherwise zeros
 * A DMI table outside the window (coreboot, and many later BIOSes, keep it
 * near the top of RAM) is mapped at its identity address as well, read from
 * /dev/mem or the kernel's copy in sysfs.
 */
#include "hsf_os.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define BIOS_BASE 0xE0000ul
#define BIOS_SIZE 0x20000ul
#define MAX_DMI_TABLES 4

static void *window;
static struct {
	void *addr;
	size_t size;
} tables[MAX_DMI_TABLES];
static int ntables;

static int fill_from(const char *path, off_t offset, void *dst, size_t size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;
	int err;

	if (fd < 0)
		return -errno;
	n = pread(fd, dst, size, offset);
	err = n < 0 ? -errno : n != (ssize_t)size ? -EIO : 0;
	close(fd);
	return err;
}

static uint32_t get32(const unsigned char *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));
	return v;
}

/* The kernel's copy of the DMI table, if its entry point names the same table. */
static int fill_from_sysfs(uint32_t addr, void *dst, size_t size)
{
	int fd = open("/sys/firmware/dmi/tables/smbios_entry_point", O_RDONLY | O_CLOEXEC);
	unsigned char ep[32];
	uint64_t kaddr;
	ssize_t n;

	if (fd < 0)
		return -errno;
	n = read(fd, ep, sizeof(ep));
	close(fd);
	if (n >= 0x1c && !memcmp(ep, "_SM_", 4))
		kaddr = get32(ep + 0x18);
	else if (n >= 0x18 && !memcmp(ep, "_SM3_", 5))
		memcpy(&kaddr, ep + 0x10, sizeof(kaddr));
	else if (n >= 0x0c && !memcmp(ep, "_DMI_", 5))
		kaddr = get32(ep + 8);
	else
		return -ENOENT;
	if (kaddr != addr)
		return -ENOENT;
	return fill_from("/sys/firmware/dmi/tables/DMI", 0, dst, size);
}

static bool table_mapped(uint64_t lo, uint64_t hi)
{
	for (int i = 0; i < ntables; i++)
		if (lo >= (uintptr_t)tables[i].addr && hi <= (uintptr_t)tables[i].addr + tables[i].size)
			return true;
	return false;
}

/* Map one DMI table at its identity address; 0 on success. */
static int map_table(uint32_t addr, size_t len, bool devmem, const char **used)
{
	uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
	uint64_t lo = addr & ~(page - 1);
	uint64_t hi = ((uint64_t)addr + (len ? len : 1) + page - 1) & ~(page - 1);
	void *map;
	int err;

	if (table_mapped(lo, hi))
		return 0;
	if (ntables == MAX_DMI_TABLES)
		return -ENOSPC;
	if ((uintptr_t)(hi - 1) != hi - 1)	/* above 4 GiB on i386 */
		return -EFAULT;
	map = mmap((void *)(uintptr_t)lo, (size_t)(hi - lo), PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (map == MAP_FAILED)
		return -errno;
	if (map != (void *)(uintptr_t)lo) {
		munmap(map, (size_t)(hi - lo));
		return -EEXIST;
	}
	if (!devmem) {
		err = -ENOENT;	/* an image file holds the BIOS area only */
	} else if (!(err = fill_from("/dev/mem", (off_t)addr, (char *)map + (addr - lo), len))) {
		*used = "/dev/mem";
	} else if (!(err = fill_from_sysfs(addr, (char *)map + (addr - lo), len))) {
		*used = "/sys/firmware/dmi/tables/DMI";
	}
	if (err) {
		munmap(map, (size_t)(hi - lo));
		return err;
	}
	mprotect(map, (size_t)(hi - lo), PROT_READ);
	tables[ntables].addr = map;
	tables[ntables].size = (size_t)(hi - lo);
	ntables++;
	return 0;
}

/*
 * Find each DMI entry point as the engine does (16-byte steps) and make the
 * table it points to readable. An entry point whose table cannot be mapped is
 * hidden, since the engine would fault on it.
 */
static void map_dmi_tables(bool devmem)
{
	unsigned char *w = window;

	for (size_t off = 0; off + 16 <= BIOS_SIZE; off += 16) {
		unsigned char *ep = w + off;
		uint32_t addr = get32(ep + 8);
		uint16_t len;
		const char *used = NULL;
		int err;

		if (memcmp(ep, "_DMI_", 5))
			continue;
		memcpy(&len, ep + 6, sizeof(len));
		if (addr >= BIOS_BASE && (uint64_t)addr + len <= BIOS_BASE + BIOS_SIZE)
			continue;
		err = map_table(addr, len, devmem, &used);
		if (!err) {
			if (used)
				hsf_log(HSF_LOG_INFO, "DMI table %#x-%#x from %s", addr,
					addr + (len ? len - 1u : 0u), used);
			continue;
		}
		memset(ep, 0, 5);
		hsf_log(HSF_LOG_WARN, "DMI table at %#x (%u bytes) lies outside the BIOS window and "
			"cannot be mapped (%s); entry point hidden, so HARDWARE_ID will differ "
			"from the kernel driver's", addr, len,
			err == -ENOENT && !devmem ? "not in the image" : strerror(-err));
	}
}

int hsf_physmem_init(const char *source)
{
	const char *used = "zeros";
	int err = 0;

	if (window)
		return 0;
	window = mmap((void *)BIOS_BASE, BIOS_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (window == MAP_FAILED || window != (void *)BIOS_BASE) {
		err = -errno;
		hsf_log(HSF_LOG_ERR, "cannot map the BIOS window at %#lx: %s (vm.mmap_min_addr?)",
			BIOS_BASE, strerror(errno));
		window = NULL;
		return err ? err : -ENOMEM;
	}
	if (!source || !strcmp(source, "auto") || !strcmp(source, "devmem")) {
		err = fill_from("/dev/mem", (off_t)BIOS_BASE, window, BIOS_SIZE);
		if (!err) {
			used = "/dev/mem";
		} else if (source && !strcmp(source, "devmem")) {
			hsf_log(HSF_LOG_ERR, "cannot read the BIOS area from /dev/mem: %s", strerror(-err));
			hsf_physmem_shutdown();
			return err;
		} else {
			memset(window, 0, BIOS_SIZE);
			hsf_log(HSF_LOG_INFO, "/dev/mem not readable (%s); BIOS window left empty, "
				"so HARDWARE_ID will differ from the kernel driver's", strerror(-err));
		}
	} else if (!strncmp(source, "file:", 5)) {
		err = fill_from(source + 5, 0, window, BIOS_SIZE);
		if (err) {
			hsf_log(HSF_LOG_ERR, "cannot read BIOS image %s: %s", source + 5, strerror(-err));
			hsf_physmem_shutdown();
			return err;
		}
		used = source + 5;
	} else if (strcmp(source, "zeros")) {
		hsf_log(HSF_LOG_ERR, "unknown BIOS window source \"%s\"", source);
		hsf_physmem_shutdown();
		return -EINVAL;
	}
	hsf_log(HSF_LOG_INFO, "BIOS window %#lx-%#lx from %s", BIOS_BASE, BIOS_BASE + BIOS_SIZE - 1, used);
	map_dmi_tables(!strcmp(used, "/dev/mem"));
	mprotect(window, BIOS_SIZE, PROT_READ);
	return 0;
}

void hsf_physmem_shutdown(void)
{
	while (ntables > 0) {
		ntables--;
		munmap(tables[ntables].addr, tables[ntables].size);
	}
	if (window) {
		munmap(window, BIOS_SIZE);
		window = NULL;
	}
}
