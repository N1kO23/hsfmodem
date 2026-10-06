/* SPDX-License-Identifier: MIT */
/*
 * Physical-memory window.
 *
 * The engine reads physical memory as OS_DEVNODE.osPageOffset + phys, which
 * was the kernel's direct map. It scans the BIOS area 0xE0000-0xFFFFF for a
 * DMI entry point; the result feeds the HARDWARE_ID that Linuxant license keys
 * are bound to. With osPageOffset = 0, the daemon maps that window read-only
 * at its identity address. Sources:
 *   "devmem"     the real BIOS area from /dev/mem (root, no kernel lockdown)
 *   "zeros"      an empty window (tests, or when nothing else is readable)
 *   "file:PATH"  a 128 KiB image, e.g. captured on the target machine
 *   "auto"       devmem if readable, otherwise zeros
 */
#include "hsf_os.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define BIOS_BASE 0xE0000ul
#define BIOS_SIZE 0x20000ul

static void *window;

static int fill_from(const char *path, off_t offset, void *dst)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -errno;
	n = pread(fd, dst, BIOS_SIZE, offset);
	close(fd);
	if (n != (ssize_t)BIOS_SIZE)
		return n < 0 ? -errno : -EIO;
	return 0;
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
		err = fill_from("/dev/mem", (off_t)BIOS_BASE, window);
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
		err = fill_from(source + 5, 0, window);
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
	mprotect(window, BIOS_SIZE, PROT_READ);
	hsf_log(HSF_LOG_INFO, "BIOS window %#lx-%#lx from %s", BIOS_BASE, BIOS_BASE + BIOS_SIZE - 1, used);
	return 0;
}

void hsf_physmem_shutdown(void)
{
	if (window) {
		munmap(window, BIOS_SIZE);
		window = NULL;
	}
}
