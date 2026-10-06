/* SPDX-License-Identifier: MIT */
#include "control.h"
#include "hsf_os.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_REQUEST 256

struct hsf_control {
	char *path;
	int listen_fd, stop_fd;
	pthread_t thread;
	hsf_control_handler handler;
	void *ctx;
};

static void serve(struct hsf_control *c, int fd)
{
	struct timeval tv = { .tv_sec = 1 };
	char req[MAX_REQUEST + 1];
	size_t len = 0;
	FILE *reply;

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	while (len < MAX_REQUEST) {
		ssize_t n = read(fd, req + len, MAX_REQUEST - len);

		if (n <= 0)
			break;
		len += (size_t)n;
		if (memchr(req, '\n', len))
			break;
	}
	req[len] = '\0';
	req[strcspn(req, "\r\n")] = '\0';

	reply = fdopen(fd, "w");
	if (!reply) {
		close(fd);
		return;
	}
	c->handler(c->ctx, req, reply);
	fputc('\n', reply);
	fclose(reply);
}

static void *control_main(void *arg)
{
	struct hsf_control *c = arg;

	hsf_thread_enter("control");
	for (;;) {
		struct pollfd fds[2] = {
			{ .fd = c->listen_fd, .events = POLLIN },
			{ .fd = c->stop_fd, .events = POLLIN },
		};

		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			hsf_log(HSF_LOG_ERR, "control: poll: %s", strerror(errno));
			break;
		}
		if (fds[1].revents)
			break;
		if (fds[0].revents & POLLIN) {
			int fd = accept4(c->listen_fd, NULL, NULL, SOCK_CLOEXEC);

			if (fd >= 0)
				serve(c, fd);
		}
	}
	return NULL;
}

struct hsf_control *hsf_control_start(const char *path, gid_t group, hsf_control_handler handler, void *ctx)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct hsf_control *c;

	if (strlen(path) >= sizeof(addr.sun_path)) {
		hsf_log(HSF_LOG_ERR, "control socket path too long: %s", path);
		return NULL;
	}
	c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->handler = handler;
	c->ctx = ctx;
	c->path = strdup(path);
	c->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	c->stop_fd = eventfd(0, EFD_CLOEXEC);
	strcpy(addr.sun_path, path);
	unlink(path);
	if (!c->path || c->listen_fd < 0 || c->stop_fd < 0 ||
	    bind(c->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(c->listen_fd, 8)) {
		hsf_log(HSF_LOG_ERR, "control socket %s: %s", path, strerror(errno));
		goto fail;
	}
	if (group != (gid_t)-1 && chown(path, (uid_t)-1, group))
		hsf_log(HSF_LOG_WARN, "cannot give %s to group %u: %s", path, (unsigned)group, strerror(errno));
	chmod(path, 0660);
	if (hsf_thread_start(&c->thread, "control", control_main, c)) {
		unlink(path);
		goto fail;
	}
	return c;

fail:
	if (c->listen_fd >= 0)
		close(c->listen_fd);
	if (c->stop_fd >= 0)
		close(c->stop_fd);
	free(c->path);
	free(c);
	return NULL;
}

void hsf_control_stop(struct hsf_control *c)
{
	uint64_t one = 1;

	if (!c)
		return;
	if (write(c->stop_fd, &one, sizeof(one)) < 0)
		hsf_log(HSF_LOG_WARN, "control: %s", strerror(errno));
	pthread_join(c->thread, NULL);
	close(c->listen_fd);
	close(c->stop_fd);
	unlink(c->path);
	free(c->path);
	free(c);
}
