/* SPDX-License-Identifier: MIT */
#include "privs.h"
#include "hsf_os.h"

#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <stdio.h>
#include <pwd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

/* A real-time thread that runs this long without sleeping is stuck. */
#define RTTIME_SOFT_US 2000000
#define RTTIME_HARD_US 3000000

static void set_limit(int resource, rlim_t soft, rlim_t hard, const char *name)
{
	struct rlimit rl = { .rlim_cur = soft, .rlim_max = hard };

	if (setrlimit(resource, &rl))
		hsf_log(HSF_LOG_WARN, "cannot set %s: %s", name, strerror(errno));
}

int hsf_privs_prepare(struct hsf_privs *p, const char *user, bool realtime, int rt_priority)
{
	*p = (struct hsf_privs){ .root = geteuid() == 0, .user = user };

	if (user && *user) {
		struct passwd *pw = getpwnam(user);

		if (!pw) {
			hsf_log(HSF_LOG_ERR, "unknown user '%s'", user);
			return 1;
		}
		p->uid = pw->pw_uid;
		p->gid = pw->pw_gid;
		if (p->root && pw->pw_uid != 0) {
			p->drop = true;
		} else if (!p->root && pw->pw_uid != geteuid()) {
			hsf_log(HSF_LOG_ERR, "--user=%s needs hsfmodemd to start as root", user);
			return 1;
		}
	}

	if (realtime) {
		if (p->root) {
			/* what the threads may still do after the switch */
			set_limit(RLIMIT_RTPRIO, (rlim_t)rt_priority + 10, (rlim_t)rt_priority + 10, "RLIMIT_RTPRIO");
			set_limit(RLIMIT_MEMLOCK, RLIM_INFINITY, RLIM_INFINITY, "RLIMIT_MEMLOCK");
		}
		set_limit(RLIMIT_RTTIME, RTTIME_SOFT_US, RTTIME_HARD_US, "RLIMIT_RTTIME");
		/* page faults are latency the modem cannot absorb */
		if (mlockall(MCL_CURRENT | MCL_FUTURE))
			hsf_log(HSF_LOG_WARN, "cannot lock memory (%s): expect latency spikes", strerror(errno));
	}
	return 0;
}

int hsf_privs_mkdir(const struct hsf_privs *p, const char *path, mode_t mode)
{
	char parent[PATH_MAX];

	snprintf(parent, sizeof(parent), "%s", path);
	for (char *s = parent + 1; (s = strchr(s, '/')); s++) {
		*s = '\0';
		if (mkdir(parent, 0755) && errno != EEXIST) {
			hsf_log(HSF_LOG_ERR, "cannot create %s: %s", parent, strerror(errno));
			return 1;
		}
		*s = '/';
	}
	if (mkdir(path, mode) && errno != EEXIST) {
		hsf_log(HSF_LOG_ERR, "cannot create %s: %s", path, strerror(errno));
		return 1;
	}
	if (p->drop && chown(path, p->uid, p->gid)) {
		hsf_log(HSF_LOG_ERR, "cannot give %s to %s: %s", path, p->user, strerror(errno));
		return 1;
	}
	return 0;
}

int hsf_privs_drop(const struct hsf_privs *p)
{
	if (!p->drop)
		return 0;
	if (initgroups(p->user, p->gid) || setgid(p->gid) || setuid(p->uid)) {
		hsf_log(HSF_LOG_ERR, "cannot switch to user %s: %s", p->user, strerror(errno));
		return 1;
	}
	if (setuid(0) == 0) {
		hsf_log(HSF_LOG_ERR, "still able to regain root after switching to %s", p->user);
		return 1;
	}
	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	hsf_log(HSF_LOG_INFO, "running as %s", p->user);
	return 0;
}
