/* SPDX-License-Identifier: MIT */
/*
 * A simulated modem behind the port interface, for testing the serial
 * bridge and the daemon without the engine or hardware.
 *
 * Commands (case-insensitive, several per line after AT):
 *   E0/E1 echo   Q, V, X, L, M, &C, &K, +...  accepted and ignored
 *   Z, &F        reset (hangs up)
 *   &Dn          DTR drop: 0 ignore, 1 to command state, 2 hang up (default)
 *   Sn=v, Sn?    S-registers; S12 is the escape guard time in 1/50 s
 *   I, I0..I3    identification;  I9  the simulator's own state
 *   D<string>    dial: CONNECT 14400 after 200 ms, then data mode
 *                (B in the string: BUSY; * in it: carrier drops 300 ms
 *                after connecting, as when the far end hangs up)
 *   H            hang up        O  back to data mode
 * In data mode the "far end" echoes everything; +++ surrounded by the
 * guard time returns to command state.
 */
#include "port.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_SIZE 4096
#define MAX_WORK 8

struct work {
	OSSCHED *storage;
	void (*fn)(void *);
	void *arg;
};

struct sim {
	struct hsf_port port;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool stop;
	struct work work[MAX_WORK];
	unsigned int nwork;

	/* the modem: touched only on the sim thread */
	uint8_t out[OUT_SIZE];
	unsigned int out_len;
	char cmd[128];
	unsigned int cmd_len;
	bool echo, dtr, rts, online, data_mode, drop_after_connect;
	int dtr_mode;
	unsigned int sreg[32];
	unsigned int speed, dtr_drops, port_configs;
	bool crtscts;
	uint64_t connect_at, busy_at, drop_at, escape_at, last_data_ns;
	int plus_count;
};

static uint64_t guard_ns(struct sim *s)
{
	return (uint64_t)s->sreg[12] * 20000000u;	/* 1/50 s units */
}

static void emit(struct sim *s, const char *text)
{
	size_t n = strlen(text);

	if (n > OUT_SIZE - s->out_len)
		n = OUT_SIZE - s->out_len;
	memcpy(s->out + s->out_len, text, n);
	s->out_len += (unsigned int)n;
	hsf_port_event(&s->port, COMCTRL_EVT_RXCHAR);
}

static void result(struct sim *s, const char *code)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "\r\n%s\r\n", code);
	emit(s, buf);
}

static void set_carrier(struct sim *s, bool on)
{
	s->online = on;
	hsf_port_event(&s->port, COMCTRL_EVT_RLSD | (on ? COMCTRL_EVT_RLSDS : 0));
}

static void hang_up(struct sim *s, bool report)
{
	bool was_online = s->online;

	s->connect_at = s->busy_at = s->drop_at = s->escape_at = 0;
	s->data_mode = false;
	if (was_online)
		set_carrier(s, false);
	if (report && was_online)
		result(s, "NO CARRIER");
}

static void reset(struct sim *s)
{
	hang_up(s, false);
	s->echo = true;
	s->dtr_mode = 2;
	memset(s->sreg, 0, sizeof(s->sreg));
	s->sreg[12] = 50;
}

static unsigned int number(const char **p)
{
	unsigned int v = 0;

	while (isdigit((unsigned char)**p))
		v = v * 10 + (unsigned int)(*(*p)++ - '0');
	return v;
}

/* Returns false on a command error. */
static bool execute(struct sim *s, const char *p, bool *quiet)
{
	char buf[128];
	unsigned int n;

	while (*p) {
		char c = (char)toupper((unsigned char)*p++);

		switch (c) {
		case ' ':
			break;
		case 'E':
			s->echo = number(&p) != 0;
			break;
		case 'Q': case 'V': case 'X': case 'L': case 'M':
			number(&p);
			break;
		case 'Z':
			number(&p);
			reset(s);
			break;
		case 'H':
			number(&p);
			hang_up(s, false);
			break;
		case 'O':
			if (!s->online)
				return false;
			s->data_mode = true;
			s->last_data_ns = hsf_now_ns();
			result(s, "CONNECT 14400");
			*quiet = true;
			return true;
		case 'I':
			n = number(&p);
			if (n == 9) {
				snprintf(buf, sizeof(buf), "\r\ndtr=%d rts=%d speed=%u crtscts=%d dtr_drops=%u configs=%u",
					 s->dtr, s->rts, s->speed, s->crtscts, s->dtr_drops, s->port_configs);
				emit(s, buf);
			} else if (n <= 3) {
				emit(s, n == 3 ? "\r\nhsfmodemd simulated modem" : "\r\nHSF SIMULATOR");
			} else {
				return false;
			}
			emit(s, "\r\n");
			break;
		case 'S':
			n = number(&p);
			if (n >= 32)
				return false;
			if (*p == '=') {
				p++;
				s->sreg[n] = number(&p);
			} else if (*p == '?') {
				p++;
				snprintf(buf, sizeof(buf), "\r\n%03u\r\n", s->sreg[n]);
				emit(s, buf);
			} else {
				return false;
			}
			break;
		case 'D':
			if (s->online)
				return false;
			if (strchr(p, 'B') || strchr(p, 'b'))
				s->busy_at = hsf_now_ns() + 100000000u;
			else
				s->connect_at = hsf_now_ns() + 200000000u;
			s->drop_after_connect = strchr(p, '*') != NULL;
			*quiet = true;	/* the result comes later */
			return true;
		case '&':
			c = (char)toupper((unsigned char)*p++);
			n = number(&p);
			if (c == 'D')
				s->dtr_mode = (int)n;
			else if (c == 'F')
				reset(s);
			else if (c != 'C' && c != 'K')
				return false;
			break;
		case '+':
			return true;	/* extended commands: accepted wholesale */
		default:
			return false;
		}
	}
	return true;
}

static void command_line(struct sim *s)
{
	const char *p = s->cmd;
	bool quiet = false, good;

	s->cmd[s->cmd_len] = '\0';
	s->cmd_len = 0;
	while (*p == ' ')
		p++;
	if (!*p)
		return;
	if (toupper((unsigned char)p[0]) != 'A' || toupper((unsigned char)p[1]) != 'T') {
		result(s, "ERROR");
		return;
	}
	good = execute(s, p + 2, &quiet);
	if (!good)
		result(s, "ERROR");
	else if (!quiet)
		result(s, "OK");
}

static void command_char(struct sim *s, uint8_t c)
{
	if (s->connect_at || s->busy_at) {	/* any key aborts dialling */
		s->connect_at = s->busy_at = 0;
		result(s, "NO CARRIER");
		return;
	}
	if (s->echo) {
		char e[2] = { (char)c, 0 };

		emit(s, e);
	}
	if (c == '\r') {
		command_line(s);
	} else if (c == '\b' || c == 0x7f) {
		if (s->cmd_len)
			s->cmd_len--;
	} else if (c != '\n' && s->cmd_len < sizeof(s->cmd) - 1) {
		s->cmd[s->cmd_len++] = (char)c;
	}
}

static void data_char(struct sim *s, uint8_t c)
{
	uint64_t now = hsf_now_ns();

	if (c == '+' && (s->plus_count || now - s->last_data_ns >= guard_ns(s)))
		s->plus_count++;
	else
		s->plus_count = 0;
	s->escape_at = s->plus_count == 3 ? now + guard_ns(s) : 0;
	if (s->plus_count == 3)
		s->plus_count = 0;
	s->last_data_ns = now;
}

/* ---- port operations (sim thread) ------------------------------------------ */

static struct sim *sim_of(struct hsf_port *p)
{
	return (struct sim *)p;
}

static unsigned int sim_read(struct hsf_port *p, void *buf, unsigned int n)
{
	struct sim *s = sim_of(p);

	if (n > s->out_len)
		n = s->out_len;
	memcpy(buf, s->out, n);
	memmove(s->out, s->out + n, s->out_len - n);
	s->out_len -= n;
	return n;
}

static unsigned int sim_write(struct hsf_port *p, const void *buf, unsigned int n)
{
	struct sim *s = sim_of(p);
	const uint8_t *b = buf;
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (s->data_mode) {
			if (s->out_len == OUT_SIZE)
				break;	/* the far end is busy: flow control */
			s->out[s->out_len++] = b[i];
			data_char(s, b[i]);
		} else {
			command_char(s, b[i]);
		}
	}
	if (s->data_mode && i)
		hsf_port_event(&s->port, COMCTRL_EVT_RXCHAR);
	return i;
}

static int sim_control(struct hsf_port *p, COMCTRL_CONTROL_CODE code, void *arg)
{
	struct sim *s = sim_of(p);
	PORT_CONFIG *cfg = arg;

	switch (code) {
	case COMCTRL_CONTROL_SETDTR:
		s->dtr = true;
		break;
	case COMCTRL_CONTROL_CLRDTR:
		if (s->dtr)
			s->dtr_drops++;
		s->dtr = false;
		if (s->dtr_mode == 2)
			hang_up(s, true);
		else if (s->dtr_mode == 1 && s->data_mode)
			s->data_mode = false;
		break;
	case COMCTRL_CONTROL_SETRTS:
		s->rts = true;
		break;
	case COMCTRL_CONTROL_CLRRTS:
		s->rts = false;
		break;
	case COMCTRL_CONTROL_PORTCONFIG:
		s->port_configs++;
		if (cfg->dwValidFileds & PC_DTE_SPEED)
			s->speed = cfg->dwDteSpeed;
		if (cfg->dwValidFileds & PC_CTS)
			s->crtscts = cfg->fCTS;
		break;
	default:
		break;
	}
	return 0;
}

static bool sim_run(struct hsf_port *p, OSSCHED *storage, void (*fn)(void *), void *arg)
{
	struct sim *s = sim_of(p);
	bool ok = true;

	pthread_mutex_lock(&s->lock);
	for (unsigned int i = 0; i < s->nwork; i++)
		if (s->work[i].storage == storage)
			ok = false;
	if (ok && s->nwork == MAX_WORK) {
		hsf_problem("sim: work queue full");
		ok = false;
	}
	if (ok) {
		s->work[s->nwork++] = (struct work){ storage, fn, arg };
		pthread_cond_signal(&s->cond);
	}
	pthread_mutex_unlock(&s->lock);
	return ok;
}

/* ---- the sim thread ------------------------------------------------------------ */

static void timers(struct sim *s)
{
	uint64_t now = hsf_now_ns();

	if (s->busy_at && now >= s->busy_at) {
		s->busy_at = 0;
		result(s, "BUSY");
	}
	if (s->connect_at && now >= s->connect_at) {
		s->connect_at = 0;
		set_carrier(s, true);
		s->data_mode = true;
		s->last_data_ns = now;
		result(s, "CONNECT 14400");
		if (s->drop_after_connect)
			s->drop_at = now + 300000000u;
	}
	if (s->drop_at && now >= s->drop_at) {
		s->drop_at = 0;
		hang_up(s, true);
	}
	if (s->escape_at && now >= s->escape_at && now - s->last_data_ns >= guard_ns(s)) {
		s->escape_at = 0;
		s->data_mode = false;
		result(s, "OK");
	}
}

static uint64_t next_deadline(struct sim *s)
{
	uint64_t d = UINT64_MAX;
	const uint64_t t[] = { s->busy_at, s->connect_at, s->drop_at, s->escape_at };

	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (t[i] && t[i] < d)
			d = t[i];
	return d;
}

static void *sim_main(void *arg)
{
	struct sim *s = arg;

	hsf_thread_enter("sim");
	pthread_mutex_lock(&s->lock);
	while (!s->stop) {
		if (s->nwork) {
			struct work w = s->work[0];

			memmove(s->work, s->work + 1, --s->nwork * sizeof(s->work[0]));
			pthread_mutex_unlock(&s->lock);
			w.fn(w.arg);
			pthread_mutex_lock(&s->lock);
			continue;
		}
		pthread_mutex_unlock(&s->lock);
		timers(s);
		pthread_mutex_lock(&s->lock);
		if (s->nwork || s->stop)
			continue;
		uint64_t d = next_deadline(s);

		if (d == UINT64_MAX)
			pthread_cond_wait(&s->cond, &s->lock);
		else
			hsf_cond_wait_until(&s->cond, &s->lock, d);
	}
	pthread_mutex_unlock(&s->lock);
	return NULL;
}

struct hsf_port *hsf_sim_port_new(void)
{
	struct sim *s = calloc(1, sizeof(*s));

	if (!s)
		return NULL;
	s->port.read = sim_read;
	s->port.write = sim_write;
	s->port.control = sim_control;
	s->port.run = sim_run;
	s->port.lines = COMCTRL_EVT_DSRS | COMCTRL_EVT_CTSS;
	reset(s);
	pthread_mutex_init(&s->lock, NULL);
	hsf_cond_init(&s->cond);
	if (hsf_thread_start(&s->thread, "sim", sim_main, s)) {
		free(s);
		return NULL;
	}
	return &s->port;
}

void hsf_sim_port_destroy(struct hsf_port *p)
{
	struct sim *s = sim_of(p);

	if (!p)
		return;
	pthread_mutex_lock(&s->lock);
	s->stop = true;
	pthread_cond_signal(&s->cond);
	pthread_mutex_unlock(&s->lock);
	pthread_join(s->thread, NULL);
	pthread_mutex_destroy(&s->lock);
	pthread_cond_destroy(&s->cond);
	free(s);
}
