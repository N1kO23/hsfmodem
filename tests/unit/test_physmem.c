/* SPDX-License-Identifier: MIT */
/* The BIOS window and the DMI tables its entry points refer to. */
#include "hsf_os.h"
#include "../tap.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define BIOS_BASE 0xE0000ul
#define BIOS_SIZE 0x20000ul

static int warnings;

static void quiet_sink(int level, const char *line)
{
	(void)line;
	if (level == HSF_LOG_WARN)
		warnings++;
}

/* A BIOS image with one legacy DMI entry point at offset 0x10. */
static int write_image(const char *path, uint32_t table, uint16_t len)
{
	static unsigned char img[BIOS_SIZE];
	int fd;
	ssize_t n;

	memset(img, 0, sizeof(img));
	memcpy(img + 0x10, "_DMI_", 5);
	memcpy(img + 0x16, &len, sizeof(len));
	memcpy(img + 0x18, &table, sizeof(table));
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	n = write(fd, img, sizeof(img));
	close(fd);
	return n == (ssize_t)sizeof(img) ? 0 : -1;
}

static bool mapped(uintptr_t addr)
{
	return msync((void *)(addr & ~(uintptr_t)4095), 4096, MS_ASYNC) == 0 || errno != ENOMEM;
}

static void test_table_inside(const char *path, const char *source)
{
	const unsigned char *ep = (const unsigned char *)BIOS_BASE + 0x10;

	ok(!write_image(path, BIOS_BASE + 0x1000, 0x80), "image with a table inside the window written");
	warnings = 0;
	is_int(hsf_physmem_init(source), 0, "init with a table inside the window");
	ok(!memcmp(ep, "_DMI_", 5), "its entry point is left alone");
	is_int(warnings, 0, "no warnings");
	hsf_physmem_shutdown();
}

static void test_table_outside(const char *path, const char *source)
{
	const unsigned char *ep = (const unsigned char *)BIOS_BASE + 0x10;
	const uint32_t table = 0x10000000;
	uint32_t still;

	ok(!write_image(path, table, 0x1234), "image with a table outside the window written");
	warnings = 0;
	is_int(hsf_physmem_init(source), 0, "init with a table outside the window");
	ok(memcmp(ep, "_DMI_", 5), "the entry point of an unreadable table is hidden");
	memcpy(&still, ep + 8, sizeof(still));
	is_int(still, table, "the rest of the window is unchanged");
	is_int(warnings, 1, "one warning about the hidden table");
	ok(!mapped(table), "nothing is mapped at the table address");
	hsf_physmem_shutdown();
}

int main(void)
{
	char dir[] = "/tmp/hsf-physmem-XXXXXX", path[4096], source[4200];

	hsf_log_set_sink(quiet_sink);
	if (!mkdtemp(dir)) {
		printf("Bail out! mkdtemp: %s\n", strerror(errno));
		return 1;
	}
	snprintf(path, sizeof(path), "%s/bios.img", dir);
	snprintf(source, sizeof(source), "file:%s", path);

	test_table_inside(path, source);
	test_table_outside(path, source);
	is_int(hsf_physmem_init("zeros"), 0, "the window maps again after shutdown");
	hsf_physmem_shutdown();
	is_int(hsf_problem_count(), 0, "no invariant violations");

	unlink(path);
	rmdir(dir);
	return tap_done();
}
