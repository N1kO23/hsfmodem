/* SPDX-License-Identifier: MIT */
/*
 * The serial bridge (docs/SERIAL.md).
 *
 * Two threads share the work:
 *
 *   serial  polls the pseudo-terminal master, inotify (slave opens) and an
 *           eventfd; turns opens, last closes, termios changes and carrier
 *           loss into modem-control operations; recreates the terminal to
 *           hang it up.
 *   port    (the engine's modem thread) runs the pump: modem-control
 *           operations in order, then bytes in both directions.
 *
 * The master fd is used only under the lock, so the serial thread can swap
 * it on hangup. Engine events only touch atomics and queue the pump.
 */
#include "bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define BUF_SIZE 256
#define PASS_LIMIT 256
#define TX_RETRY_MS 10
#define TERMIOS_CHECK_MS 500
#define MAX_OPS 16

enum op_type { OP_DTR_ON, OP_DTR_OFF, OP_PORTCONFIG, OP_FLUSH_RX };

struct op {
	enum op_type type;
	PORT_CONFIG cfg;
};

struct hsf_bridge {
	struct hsf_port *port;
	char *link;
	gid_t group;
	mode_t mode;
	int inotify, wd, evfd;
	pthread_t thread;
	bool thread_running;

	pthread_mutex_t lock;
	pthread_cond_t idle;
	/* under lock */
	int master;
	unsigned int gen;	/* a new session: after each close, and each hangup */
	char slave[64];
	bool open, dtr, stopping;
	struct op ops[MAX_OPS];
	unsigned int nops;

	/* serial thread only */
	struct termios sent_tios;

	/* atomics */
	bool pump_queued, want_out, tx_blocked;
	int pending;		/* queued pump runs not yet finished */
	unsigned long rx_bytes, tx_bytes, breaks, overruns, hangups;

	/* pump only */
	OSSCHED work;
	unsigned int pump_gen;
	uint8_t rxbuf[BUF_SIZE], txbuf[BUF_SIZE];
	unsigned int rx_len, rx_off, tx_len, tx_off;
};

static void wake(struct hsf_bridge *b)
{
	uint64_t one = 1;

	if (write(b->evfd, &one, sizeof(one)) < 0 && errno != EAGAIN)
		hsf_log(HSF_LOG_WARN, "bridge: eventfd: %s", strerror(errno));
}

static void pump(void *arg);

static void kick(struct hsf_bridge *b)
{
	if (__atomic_exchange_n(&b->pump_queued, true, __ATOMIC_SEQ_CST))
		return;
	__atomic_add_fetch(&b->pending, 1, __ATOMIC_SEQ_CST);
	if (!b->port->run(b->port, &b->work, pump, b)) {
		hsf_problem("bridge: cannot queue the pump on the port's thread");
		__atomic_sub_fetch(&b->pending, 1, __ATOMIC_SEQ_CST);
	}
}

/* ---- the pump (port thread) ------------------------------------------------ */

static void control(struct hsf_bridge *b, COMCTRL_CONTROL_CODE code, void *arg)
{
	int r = b->port->control(b->port, code, arg);

	if (r)
		hsf_log(HSF_LOG_WARN, "bridge: modem control %d failed (%d)", (int)code, r);
}

static void apply_op(struct hsf_bridge *b, struct op *op)
{
	switch (op->type) {
	case OP_DTR_ON:
		control(b, COMCTRL_CONTROL_SETDTR, NULL);
		control(b, COMCTRL_CONTROL_SETRTS, NULL);
		break;
	case OP_DTR_OFF:
		control(b, COMCTRL_CONTROL_CLRDTR, NULL);
		control(b, COMCTRL_CONTROL_CLRRTS, NULL);
		break;
	case OP_PORTCONFIG:
		control(b, COMCTRL_CONTROL_PORTCONFIG, &op->cfg);
		break;
	case OP_FLUSH_RX:
		/* what arrived while the port was closed (legacy start-up) */
		for (int i = 0; i < 64 && b->port->read(b->port, b->rxbuf, BUF_SIZE); i++)
			;
		b->rx_len = b->rx_off = 0;
		break;
	}
}

/* Move bytes in both directions; returns true if anything moved. */
static bool pump_pass(struct hsf_bridge *b, bool *out_full)
{
	bool progress = false;
	ssize_t n;

	/* modem -> terminal */
	if (b->rx_off == b->rx_len && !*out_full) {
		b->rx_len = b->port->read(b->port, b->rxbuf, BUF_SIZE);
		b->rx_off = 0;
	}
	if (b->rx_off < b->rx_len) {
		pthread_mutex_lock(&b->lock);
		n = b->gen == b->pump_gen ? write(b->master, b->rxbuf + b->rx_off, b->rx_len - b->rx_off) : -2;
		if (n < 0 && n != -2 && errno == EAGAIN)
			*out_full = true;
		pthread_mutex_unlock(&b->lock);
		if (n > 0) {
			b->rx_off += (unsigned int)n;
			__atomic_add_fetch(&b->rx_bytes, (unsigned long)n, __ATOMIC_RELAXED);
			progress = true;
		} else if (!*out_full) {
			b->rx_len = b->rx_off = 0;	/* terminal gone: drop */
		}
	}

	/* terminal -> modem */
	if (b->tx_off == b->tx_len) {
		pthread_mutex_lock(&b->lock);
		n = b->gen == b->pump_gen ? read(b->master, b->txbuf, BUF_SIZE) : 0;
		pthread_mutex_unlock(&b->lock);
		b->tx_len = n > 0 ? (unsigned int)n : 0;
		b->tx_off = 0;
	}
	if (b->tx_off < b->tx_len) {
		unsigned int put = b->port->write(b->port, b->txbuf + b->tx_off, b->tx_len - b->tx_off);

		if (put > b->tx_len - b->tx_off)
			put = b->tx_len - b->tx_off;
		b->tx_off += put;
		__atomic_add_fetch(&b->tx_bytes, put, __ATOMIC_RELAXED);
		if (put)
			progress = true;
	}
	return progress;
}

static void pump(void *arg)
{
	struct hsf_bridge *b = arg;
	struct op ops[MAX_OPS];
	unsigned int nops;
	bool open, out_full = false, more = false;

	__atomic_store_n(&b->pump_queued, false, __ATOMIC_SEQ_CST);

	pthread_mutex_lock(&b->lock);
	nops = b->nops;
	memcpy(ops, b->ops, nops * sizeof(ops[0]));
	b->nops = 0;
	if (b->pump_gen != b->gen) {
		b->pump_gen = b->gen;	/* new terminal: the old session's bytes go */
		b->rx_len = b->rx_off = b->tx_len = b->tx_off = 0;
	}
	open = b->open && !b->stopping;
	pthread_mutex_unlock(&b->lock);

	for (unsigned int i = 0; i < nops; i++)
		apply_op(b, &ops[i]);

	if (open) {
		int pass;

		for (pass = 0; pass < PASS_LIMIT; pass++)
			if (!pump_pass(b, &out_full))
				break;
		more = pass == PASS_LIMIT;
	}
	__atomic_store_n(&b->want_out, b->rx_off < b->rx_len, __ATOMIC_SEQ_CST);
	__atomic_store_n(&b->tx_blocked, b->tx_off < b->tx_len, __ATOMIC_SEQ_CST);
	if (more)
		kick(b);
	wake(b);

	pthread_mutex_lock(&b->lock);
	__atomic_sub_fetch(&b->pending, 1, __ATOMIC_SEQ_CST);
	pthread_cond_broadcast(&b->idle);
	pthread_mutex_unlock(&b->lock);
}

/* Called with the lock held. */
static void queue_op(struct hsf_bridge *b, enum op_type type, const PORT_CONFIG *cfg)
{
	if (b->nops == MAX_OPS) {
		hsf_problem("bridge: modem-control queue overflow");
		return;
	}
	b->ops[b->nops].type = type;
	if (cfg)
		b->ops[b->nops].cfg = *cfg;
	b->nops++;
}

static void on_event(void *ctx, UINT32 mask)
{
	struct hsf_bridge *b = ctx;

	if (mask & COMCTRL_EVT_BREAK)
		__atomic_add_fetch(&b->breaks, 1, __ATOMIC_RELAXED);
	if (mask & COMCTRL_EVT_RXOVRN)
		__atomic_add_fetch(&b->overruns, 1, __ATOMIC_RELAXED);
	if (mask & (COMCTRL_EVT_RXCHAR | COMCTRL_EVT_BREAK | COMCTRL_EVT_RXOVRN | COMCTRL_EVT_TXEMPTY |
		    COMCTRL_EVT_TXCHAR))
		kick(b);
	if (mask & (COMCTRL_EVT_CTS | COMCTRL_EVT_DSR | COMCTRL_EVT_RLSD | COMCTRL_EVT_RING))
		wake(b);
}

/* ---- the terminal (serial thread) ------------------------------------------ */

static unsigned int baud(speed_t s)
{
	static const struct { speed_t s; unsigned int baud; } map[] = {
		{ B50, 50 }, { B75, 75 }, { B110, 110 }, { B134, 134 }, { B150, 150 },
		{ B200, 200 }, { B300, 300 }, { B600, 600 }, { B1200, 1200 }, { B1800, 1800 },
		{ B2400, 2400 }, { B4800, 4800 }, { B9600, 9600 }, { B19200, 19200 },
		{ B38400, 38400 }, { B57600, 57600 }, { B115200, 115200 }, { B230400, 230400 },
		{ B460800, 460800 }, { B921600, 921600 },
	};

	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (map[i].s == s)
			return map[i].baud;
	return s > 1000 ? (unsigned int)s : 0;	/* a numeric speed_t */
}

static void port_config(const struct termios *t, PORT_CONFIG *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->dwValidFileds = PC_DTE_SPEED | PC_PARITY | PC_DATA_BITS | PC_CTS | PC_RTS;
	cfg->dwDteSpeed = baud(cfgetospeed(t));
	if (t->c_cflag & PARENB)
		cfg->eParity = (t->c_cflag & PARODD) ? PC_PARITY_ODD : PC_PARITY_EVEN;
	else
		cfg->eParity = PC_PARITY_NONE;
	cfg->eDataBits = (t->c_cflag & CSIZE) == CS7 ? PC_DATABITS_7 : PC_DATABITS_8;
	cfg->fCTS = cfg->fRTS = (t->c_cflag & CRTSCTS) ? TRUE : FALSE;
}

static bool config_differs(const struct termios *a, const struct termios *b)
{
	const tcflag_t mask = CSIZE | PARENB | PARODD | CRTSCTS;

	return (a->c_cflag & mask) != (b->c_cflag & mask) || cfgetospeed(a) != cfgetospeed(b);
}

static int create_terminal(struct hsf_bridge *b, char *slave, size_t n)
{
	struct termios t;
	char tmp[PATH_MAX];
	int m, wd;

	m = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (m < 0 || grantpt(m) || unlockpt(m) || ptsname_r(m, slave, n)) {
		hsf_log(HSF_LOG_ERR, "cannot create a pseudo-terminal: %s", strerror(errno));
		if (m >= 0)
			close(m);
		return -1;
	}
	/* serial ports hang up on last close by default; pseudo-terminals don't */
	if (!tcgetattr(m, &t)) {
		t.c_cflag |= HUPCL;
		tcsetattr(m, TCSANOW, &t);
	}
	if (b->group != (gid_t)-1 && chown(slave, (uid_t)-1, b->group))
		hsf_log(HSF_LOG_WARN, "cannot give %s to group %u: %s", slave, (unsigned)b->group, strerror(errno));
	if (chmod(slave, b->mode))
		hsf_log(HSF_LOG_WARN, "cannot chmod %s: %s", slave, strerror(errno));

	/* watch before publishing, so no open is missed */
	wd = inotify_add_watch(b->inotify, slave, IN_OPEN);
	if (wd < 0) {
		hsf_log(HSF_LOG_ERR, "inotify on %s: %s", slave, strerror(errno));
		close(m);
		return -1;
	}
	if (b->wd >= 0 && b->wd != wd)
		inotify_rm_watch(b->inotify, b->wd);
	b->wd = wd;

	if (snprintf(tmp, sizeof(tmp), "%s.new", b->link) >= (int)sizeof(tmp))
		goto link_failed;
	unlink(tmp);
	if (symlink(slave, tmp) || rename(tmp, b->link))
		goto link_failed;
	return m;

link_failed:
	hsf_log(HSF_LOG_ERR, "cannot link %s to %s: %s", b->link, slave, strerror(errno));
	unlink(tmp);
	close(m);
	return -1;
}

static void terminal_opened(struct hsf_bridge *b)
{
	struct termios t;
	PORT_CONFIG cfg;
	bool have_tios;

	pthread_mutex_lock(&b->lock);
	/* what got queued after the last close was seen belongs to no session */
	tcflush(b->master, TCIOFLUSH);
	have_tios = !tcgetattr(b->master, &t);
	b->open = true;
	queue_op(b, OP_FLUSH_RX, NULL);
	if (!b->dtr) {
		queue_op(b, OP_DTR_ON, NULL);
		b->dtr = true;
	}
	if (have_tios) {
		port_config(&t, &cfg);
		queue_op(b, OP_PORTCONFIG, &cfg);
		b->sent_tios = t;
	}
	pthread_mutex_unlock(&b->lock);
	hsf_log(HSF_LOG_INFO, "%s opened", b->slave);
	kick(b);
}

/*
 * The slave's line discipline outlives its last close while the master is
 * open, so bytes nobody read would greet the next opener. A serial port
 * drops them at close; flushing the master does not reach them, so open the
 * slave briefly and flush it there. Its IN_OPEN is harmless: opens are
 * confirmed on the master, which reports the hangup again once we close.
 * Serial thread, lock held.
 */
static void flush_slave(struct hsf_bridge *b)
{
	int fd = open(b->slave, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0)
		return;
	tcflush(fd, TCIFLUSH);
	close(fd);
}

/* Last close (or hangup): DTR follows HUPCL. Called with the lock held. */
static void terminal_closed_locked(struct hsf_bridge *b, const struct termios *t, bool flush)
{
	tcflush(b->master, TCIOFLUSH);
	if (flush)
		flush_slave(b);
	b->gen++;	/* a pump in flight must not write into the next session */
	b->open = false;
	if (b->dtr && (t->c_cflag & HUPCL)) {
		queue_op(b, OP_DTR_OFF, NULL);
		b->dtr = false;
	}
}

static void terminal_closed(struct hsf_bridge *b)
{
	struct termios t;

	pthread_mutex_lock(&b->lock);
	if (tcgetattr(b->master, &t))
		t.c_cflag = HUPCL;
	terminal_closed_locked(b, &t, true);
	pthread_mutex_unlock(&b->lock);
	hsf_log(HSF_LOG_INFO, "%s closed%s", b->slave, (t.c_cflag & HUPCL) ? ", dropping DTR" : "");
	kick(b);
}

/* Carrier lost without CLOCAL: hang the terminal up, as a serial port would. */
static void hang_up(struct hsf_bridge *b)
{
	char slave[sizeof(b->slave)];
	struct termios t;
	int m, old;

	m = create_terminal(b, slave, sizeof(slave));
	if (m < 0)
		return;
	pthread_mutex_lock(&b->lock);
	if (tcgetattr(b->master, &t))
		t.c_cflag = HUPCL;
	old = b->master;
	b->master = m;
	terminal_closed_locked(b, &t, false);	/* the old slave is hung up anyway */
	hsf_log(HSF_LOG_INFO, "carrier lost: hanging up %s (now %s)", b->slave, slave);
	snprintf(b->slave, sizeof(b->slave), "%s", slave);
	pthread_mutex_unlock(&b->lock);
	close(old);	/* the slave sees end-of-file and POLLHUP */
	__atomic_add_fetch(&b->hangups, 1, __ATOMIC_RELAXED);
	kick(b);
}

static bool read_inotify(struct hsf_bridge *b)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	bool opened = false;
	ssize_t n;

	while ((n = read(b->inotify, buf, sizeof(buf))) > 0) {
		for (char *p = buf; p < buf + n;) {
			struct inotify_event *ev = (struct inotify_event *)p;

			/* identical events coalesce: this means "one or more opens" */
			if (ev->wd == b->wd && (ev->mask & IN_OPEN))
				opened = true;
			p += sizeof(*ev) + ev->len;
		}
	}
	return opened;
}

static bool master_hung_up(struct hsf_bridge *b)
{
	struct pollfd p = { .fd = b->master, .events = 0 };

	return poll(&p, 1, 0) == 1 && (p.revents & POLLHUP);
}

static void *serial_main(void *arg)
{
	struct hsf_bridge *b = arg;
	UINT32 prev_lines = __atomic_load_n(&b->port->lines, __ATOMIC_SEQ_CST);

	hsf_thread_enter("serial");
	for (;;) {
		struct pollfd fds[3];
		bool open, tx_blocked, opened;
		int master, n, timeout;
		short events = 0;
		UINT32 lines;

		pthread_mutex_lock(&b->lock);
		if (b->stopping) {
			pthread_mutex_unlock(&b->lock);
			break;
		}
		master = b->master;
		open = b->open;
		pthread_mutex_unlock(&b->lock);

		tx_blocked = __atomic_load_n(&b->tx_blocked, __ATOMIC_SEQ_CST);
		if (open && !__atomic_load_n(&b->pump_queued, __ATOMIC_SEQ_CST)) {
			if (!tx_blocked)
				events |= POLLIN;
			if (__atomic_load_n(&b->want_out, __ATOMIC_SEQ_CST))
				events |= POLLOUT;
		}
		/* closed: POLLHUP is level-triggered, so only inotify tells of opens */
		fds[0] = (struct pollfd){ .fd = open ? master : -1, .events = events };
		fds[1] = (struct pollfd){ .fd = b->inotify, .events = POLLIN };
		fds[2] = (struct pollfd){ .fd = b->evfd, .events = POLLIN };
		timeout = !open ? -1 : tx_blocked ? TX_RETRY_MS : TERMIOS_CHECK_MS;
		n = poll(fds, 3, timeout);
		if (n < 0) {
			if (errno != EINTR)
				hsf_log(HSF_LOG_ERR, "bridge: poll: %s", strerror(errno));
			continue;
		}
		if (fds[2].revents & POLLIN) {
			uint64_t v;

			if (read(b->evfd, &v, sizeof(v)) < 0 && errno != EAGAIN)
				hsf_log(HSF_LOG_WARN, "bridge: eventfd: %s", strerror(errno));
		}
		opened = (fds[1].revents & POLLIN) && read_inotify(b);

		if (open) {
			if (fds[0].revents & (POLLHUP | POLLERR)) {
				terminal_closed(b);
			} else {
				struct termios t;

				if ((fds[0].revents & (POLLIN | POLLOUT)) || (tx_blocked && n == 0))
					kick(b);
				if (!tcgetattr(master, &t) && config_differs(&t, &b->sent_tios)) {
					PORT_CONFIG cfg;

					port_config(&t, &cfg);
					pthread_mutex_lock(&b->lock);
					queue_op(b, OP_PORTCONFIG, &cfg);
					pthread_mutex_unlock(&b->lock);
					b->sent_tios = t;
					kick(b);
				}
			}
		} else if (opened && !master_hung_up(b)) {
			terminal_opened(b);
		}

		lines = __atomic_load_n(&b->port->lines, __ATOMIC_SEQ_CST);
		if ((prev_lines & COMCTRL_EVT_RLSDS) && !(lines & COMCTRL_EVT_RLSDS)) {
			struct termios t;
			bool hup;

			pthread_mutex_lock(&b->lock);
			hup = b->open && !tcgetattr(b->master, &t) && !(t.c_cflag & CLOCAL);
			pthread_mutex_unlock(&b->lock);
			if (hup)
				hang_up(b);
		}
		prev_lines = lines;
	}
	return NULL;
}

/* ---- lifecycle -------------------------------------------------------------- */

struct hsf_bridge *hsf_bridge_start(struct hsf_port *port, const struct hsf_bridge_config *cfg)
{
	struct hsf_bridge *b = calloc(1, sizeof(*b));

	if (!b)
		return NULL;
	b->port = port;
	b->group = cfg->group;
	b->mode = cfg->mode;
	b->wd = -1;
	b->master = -1;
	b->link = strdup(cfg->link);
	pthread_mutex_init(&b->lock, NULL);
	hsf_cond_init(&b->idle);
	b->inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	b->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (!b->link || b->inotify < 0 || b->evfd < 0) {
		hsf_log(HSF_LOG_ERR, "bridge: %s", strerror(errno));
		goto fail;
	}
	b->master = create_terminal(b, b->slave, sizeof(b->slave));
	if (b->master < 0)
		goto fail;

	__atomic_store_n(&port->on_event_ctx, b, __ATOMIC_SEQ_CST);
	__atomic_store_n(&port->on_event, on_event, __ATOMIC_SEQ_CST);
	if (hsf_thread_start(&b->thread, "serial", serial_main, b)) {
		port->on_event = NULL;
		unlink(b->link);
		goto fail;
	}
	b->thread_running = true;
	hsf_log(HSF_LOG_INFO, "serial port %s -> %s", b->link, b->slave);
	return b;

fail:
	if (b->master >= 0)
		close(b->master);
	if (b->inotify >= 0)
		close(b->inotify);
	if (b->evfd >= 0)
		close(b->evfd);
	free(b->link);
	free(b);
	return NULL;
}

/* Wait until queued operations ran and no pump is pending. */
static void wait_idle(struct hsf_bridge *b)
{
	uint64_t deadline = hsf_now_ns() + 5000000000ull;

	pthread_mutex_lock(&b->lock);
	while (b->nops || __atomic_load_n(&b->pending, __ATOMIC_SEQ_CST)) {
		if (hsf_cond_wait_until(&b->idle, &b->lock, deadline) == ETIMEDOUT) {
			hsf_problem("bridge: the port thread did not run the pump");
			break;
		}
	}
	pthread_mutex_unlock(&b->lock);
}

void hsf_bridge_stop(struct hsf_bridge *b)
{
	pthread_mutex_lock(&b->lock);
	b->stopping = true;
	pthread_mutex_unlock(&b->lock);
	wake(b);
	if (b->thread_running)
		pthread_join(b->thread, NULL);

	/* the daemon going away is the cable being pulled */
	pthread_mutex_lock(&b->lock);
	if (b->dtr) {
		queue_op(b, OP_DTR_OFF, NULL);
		b->dtr = false;
	}
	b->open = false;
	pthread_mutex_unlock(&b->lock);
	kick(b);
	wait_idle(b);

	__atomic_store_n(&b->port->on_event, NULL, __ATOMIC_SEQ_CST);
	unlink(b->link);
	pthread_mutex_lock(&b->lock);
	close(b->master);
	b->master = -1;
	pthread_mutex_unlock(&b->lock);
	close(b->inotify);
}

void hsf_bridge_free(struct hsf_bridge *b)
{
	if (!b)
		return;
	wait_idle(b);	/* a pump queued by a late engine event */
	close(b->evfd);
	pthread_mutex_destroy(&b->lock);
	pthread_cond_destroy(&b->idle);
	free(b->link);
	free(b);
}

void hsf_bridge_status(struct hsf_bridge *b, struct hsf_bridge_status *st)
{
	memset(st, 0, sizeof(*st));
	pthread_mutex_lock(&b->lock);
	snprintf(st->tty, sizeof(st->tty), "%s", b->slave);
	st->open = b->open;
	st->dtr = b->dtr;
	pthread_mutex_unlock(&b->lock);
	st->lines = __atomic_load_n(&b->port->lines, __ATOMIC_SEQ_CST);
	st->rx_bytes = __atomic_load_n(&b->rx_bytes, __ATOMIC_RELAXED);
	st->tx_bytes = __atomic_load_n(&b->tx_bytes, __ATOMIC_RELAXED);
	st->breaks = __atomic_load_n(&b->breaks, __ATOMIC_RELAXED);
	st->overruns = __atomic_load_n(&b->overruns, __ATOMIC_RELAXED);
	st->hangups = __atomic_load_n(&b->hangups, __ATOMIC_RELAXED);
}
