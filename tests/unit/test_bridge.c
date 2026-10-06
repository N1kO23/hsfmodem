/* SPDX-License-Identifier: MIT */
/*
 * The serial bridge against the simulated modem, through a real
 * pseudo-terminal: what minicom and pppd would see (docs/SERIAL.md).
 */
#include "bridge.h"
#include "../tap.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static char link_path[4096];
static struct hsf_bridge *bridge;

static void quiet_sink(int level, const char *line)
{
	if (level <= HSF_LOG_WARN)
		printf("# %s\n", line);
}

static void msleep(int ms)
{
	usleep((useconds_t)ms * 1000);
}

/* Open the port as a terminal program would: raw, HUPCL as configured. */
static int open_port(void)
{
	struct termios t;
	int fd = open(link_path, O_RDWR | O_NOCTTY | O_NONBLOCK);

	if (fd < 0)
		return -1;
	tcgetattr(fd, &t);
	cfmakeraw(&t);
	tcsetattr(fd, TCSANOW, &t);
	msleep(30);	/* let the bridge see the open */
	return fd;
}

static void set_cflag(int fd, tcflag_t set, tcflag_t clear)
{
	struct termios t;

	tcgetattr(fd, &t);
	t.c_cflag = (t.c_cflag | set) & ~clear;
	tcsetattr(fd, TCSANOW, &t);
}

/* Read until want appears (true) or timeout; got collects the text. */
static bool expect(int fd, const char *want, int timeout_ms, char *got, size_t n)
{
	size_t len = 0;
	uint64_t end = hsf_now_ns() + (uint64_t)timeout_ms * 1000000u;

	got[0] = '\0';
	while (hsf_now_ns() < end) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		ssize_t r;

		if (poll(&p, 1, 10) <= 0)
			continue;
		r = read(fd, got + len, n - 1 - len);
		if (r <= 0)
			return false;
		len += (size_t)r;
		got[len] = '\0';
		if (strstr(got, want))
			return true;
	}
	return false;
}

static bool command(int fd, const char *cmd, const char *want, char *got, size_t n)
{
	char line[128];

	snprintf(line, sizeof(line), "%s\r", cmd);
	if (write(fd, line, strlen(line)) != (ssize_t)strlen(line))
		return false;
	if (expect(fd, want, 1000, got, n))
		return true;
	for (char *c = got; *c; c++)
		if (*c == '\r' || *c == '\n')
			*c = '|';
	tap_diag("%s: got \"%s\"", cmd, got);
	return false;
}

static void status(struct hsf_bridge_status *st)
{
	hsf_bridge_status(bridge, st);
}

static void test_link(void)
{
	char target[256];
	struct hsf_bridge_status st;
	struct stat sb;
	ssize_t n = readlink(link_path, target, sizeof(target) - 1);

	ok(n > 0, "the link exists");
	target[n > 0 ? n : 0] = '\0';
	ok(!strncmp(target, "/dev/pts/", 9), "and points at a pseudo-terminal (%s)", target);
	status(&st);
	is_str(st.tty, target, "the status names the same terminal");
	ok(!st.open && !st.dtr, "nobody has it open: DTR is down");
	ok(!stat(target, &sb) && (sb.st_mode & 0777) == 0660, "the terminal is mode 0660");
}

static void test_commands(void)
{
	char got[512];
	struct hsf_bridge_status st;
	int fd = open_port();

	ok(fd >= 0, "the port opens");
	status(&st);
	ok(st.open && st.dtr, "an open raises DTR");
	ok(command(fd, "AT", "OK\r\n", got, sizeof(got)), "AT answers OK");
	ok(!strncmp(got, "AT\r", 3), "with the command echoed");
	ok(command(fd, "ATI3", "OK", got, sizeof(got)) && strstr(got, "simulated modem"),
	   "ATI3 identifies the modem");
	ok(command(fd, "AT&Q", "ERROR", got, sizeof(got)), "an unknown command gives ERROR");
	ok(command(fd, "ATI9", "OK", got, sizeof(got)) && strstr(got, "dtr=1 rts=1"),
	   "the modem sees DTR and RTS raised");
	ok(strstr(got, "configs=1") != NULL, "and one port configuration at open");

	set_cflag(fd, CRTSCTS, 0);
	{
		struct termios t;

		tcgetattr(fd, &t);
		cfsetspeed(&t, B115200);
		tcsetattr(fd, TCSANOW, &t);
	}
	msleep(700);
	ok(command(fd, "ATI9", "OK", got, sizeof(got)) && strstr(got, "speed=115200 crtscts=1"),
	   "termios changes reach the modem as a port configuration");
	close(fd);
}

static void test_dtr(void)
{
	char got[512];
	struct hsf_bridge_status st;
	int fd;

	msleep(50);
	status(&st);
	ok(!st.open && !st.dtr, "the last close drops DTR (HUPCL is the default)");
	fd = open_port();
	ok(command(fd, "ATI9", "OK", got, sizeof(got)) && strstr(got, "dtr=1") && strstr(got, "dtr_drops=1"),
	   "the modem saw DTR drop once and rise again");
	ok(strstr(got, "speed=115200") != NULL, "termios settings persist across opens");

	set_cflag(fd, 0, HUPCL);
	close(fd);
	msleep(50);
	status(&st);
	ok(!st.open && st.dtr, "without HUPCL, DTR stays up after the last close");
	fd = open_port();
	ok(command(fd, "ATI9", "OK", got, sizeof(got)) && strstr(got, "dtr_drops=1"), "and the modem saw no drop");
	set_cflag(fd, HUPCL, 0);
	close(fd);
}

static void test_data(void)
{
	char got[512];
	struct hsf_bridge_status st;
	int fd = open_port();

	set_cflag(fd, CLOCAL, 0);	/* as minicom does: ATH must not hang the terminal up */
	ok(command(fd, "ATS12=5", "OK", got, sizeof(got)), "set a 100 ms escape guard time");
	ok(command(fd, "ATD5551234", "CONNECT 14400", got, sizeof(got)), "dialling connects");
	msleep(20);
	status(&st);
	ok(st.lines & COMCTRL_EVT_RLSDS, "the bridge sees carrier");
	ok(write(fd, "hello", 5) == 5 && expect(fd, "hello", 1000, got, sizeof(got)), "data reaches the far end and back");

	/* bulk data in both directions at once, through every buffer */
	enum { TOTAL = 64 * 1024 };
	static uint8_t sent[TOTAL], back[TOTAL];
	size_t wr = 0, rd = 0;
	uint64_t end = hsf_now_ns() + 10000000000ull;

	for (size_t i = 0; i < TOTAL; i++)
		sent[i] = (uint8_t)(i * 7 + (i >> 8));
	for (size_t i = 0; i < TOTAL; i++)	/* no accidental escapes */
		if (sent[i] == '+')
			sent[i] = '-';
	while (rd < TOTAL && hsf_now_ns() < end) {
		struct pollfd p = { .fd = fd, .events = POLLIN | (wr < TOTAL ? POLLOUT : 0) };
		ssize_t r;

		if (poll(&p, 1, 100) <= 0)
			continue;
		if ((p.revents & POLLOUT) && wr < TOTAL) {
			r = write(fd, sent + wr, TOTAL - wr > 1000 ? 1000 : TOTAL - wr);
			if (r > 0)
				wr += (size_t)r;
		}
		if (p.revents & POLLIN) {
			r = read(fd, back + rd, TOTAL - rd);
			if (r > 0)
				rd += (size_t)r;
		}
	}
	is_int(rd, TOTAL, "64 KiB looped back");
	ok(rd == TOTAL && !memcmp(sent, back, TOTAL), "unchanged and in order");

	msleep(150);
	ok(write(fd, "+++", 3) == 3 && expect(fd, "OK", 1000, got, sizeof(got)), "+++ escapes to command state");
	ok(command(fd, "ATH", "OK", got, sizeof(got)), "ATH hangs up");
	msleep(20);
	status(&st);
	ok(!(st.lines & COMCTRL_EVT_RLSDS), "carrier is gone");
	ok(st.rx_bytes >= TOTAL && st.tx_bytes >= TOTAL, "the counters saw the traffic");

	/* DTR drop while online hangs up (&D2) */
	ok(command(fd, "ATD1", "CONNECT", got, sizeof(got)), "connect again");
	close(fd);
	msleep(50);
	status(&st);
	ok(!(st.lines & COMCTRL_EVT_RLSDS), "closing the port (DTR drop) hung up the call");
}

static void test_carrier_loss(void)
{
	char got[512], before[256], after[256];
	struct hsf_bridge_status st;
	struct pollfd p;
	ssize_t n;
	int fd = open_port();

	n = readlink(link_path, before, sizeof(before) - 1);
	before[n > 0 ? n : 0] = '\0';
	set_cflag(fd, 0, CLOCAL);
	ok(command(fd, "ATD*1", "CONNECT", got, sizeof(got)), "connect to a far end that hangs up");
	p = (struct pollfd){ .fd = fd, .events = POLLIN };
	expect(fd, "NO CARRIER", 1000, got, sizeof(got));
	poll(&p, 1, 1000);
	ok(p.revents & POLLHUP, "carrier loss without CLOCAL hangs the terminal up");
	n = readlink(link_path, after, sizeof(after) - 1);
	after[n > 0 ? n : 0] = '\0';
	ok(strcmp(before, after) != 0, "the link moved to a new terminal (%s -> %s)", before, after);
	close(fd);
	msleep(50);
	status(&st);
	is_int(st.hangups, 1, "one hangup counted");
	ok(!st.open, "the port counts as closed");

	fd = open_port();
	ok(fd >= 0 && command(fd, "AT", "OK", got, sizeof(got)), "the new terminal works");
	set_cflag(fd, CLOCAL, 0);
	ok(command(fd, "ATD*2", "CONNECT", got, sizeof(got)), "with CLOCAL, connect again");
	ok(expect(fd, "NO CARRIER", 1000, got, sizeof(got)), "the modem reports the lost carrier");
	ok(command(fd, "AT", "OK", got, sizeof(got)), "but the terminal stays up");
	status(&st);
	is_int(st.hangups, 1, "no second hangup");

	/* bytes the modem sends while the port is closed are discarded at open */
	set_cflag(fd, 0, HUPCL);
	ok(write(fd, "ATI3\r", 5) == 5, "send a command and close at once");
	close(fd);
	msleep(100);
	fd = open_port();
	ok(!expect(fd, "simulated", 200, got, sizeof(got)), "the reply sent while closed is not delivered");
	set_cflag(fd, HUPCL, 0);
	close(fd);
}

int main(void)
{
	struct hsf_os_config cfg = HSF_OS_CONFIG_DEFAULT;
	struct hsf_bridge_config bc = { .group = (gid_t)-1, .mode = 0660 };
	char dir[] = "/tmp/hsf-bridge-XXXXXX";
	struct hsf_port *port;

	hsf_log_set_sink(quiet_sink);
	if (hsf_os_init(&cfg) || !mkdtemp(dir)) {
		printf("Bail out! setup failed\n");
		return 1;
	}
	snprintf(link_path, sizeof(link_path), "%s/ttySHSF0", dir);
	bc.link = link_path;
	port = hsf_sim_port_new();
	bridge = port ? hsf_bridge_start(port, &bc) : NULL;
	if (!bridge) {
		printf("Bail out! cannot start the bridge\n");
		return 1;
	}

	test_link();
	test_commands();
	test_dtr();
	test_data();
	test_carrier_loss();

	hsf_bridge_stop(bridge);
	ok(access(link_path, F_OK) != 0, "stopping removes the link");
	hsf_sim_port_destroy(port);
	hsf_bridge_free(bridge);
	is_int(hsf_problem_count(), 0, "no invariant violations");
	hsf_os_shutdown();
	rmdir(dir);
	return tap_done();
}
