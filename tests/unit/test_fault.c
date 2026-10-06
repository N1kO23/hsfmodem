/* SPDX-License-Identifier: MIT */
/* The crash reporter names the faulting function and the last Os call. */
#include "hsf_os.h"
#include "../tap.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

__attribute__((noinline, noclone)) void hsf_test_crash_here(volatile int *p)
{
	*p = 42;
}

int main(void)
{
	int fds[2];
	pid_t pid;
	char out[2048] = "";
	ssize_t n, got = 0;
	int status;

	if (pipe(fds)) {
		printf("Bail out! pipe\n");
		return 1;
	}
	pid = fork();
	if (pid == 0) {
		dup2(fds[1], STDERR_FILENO);
		hsf_fault_install();
		hsf_last_call = "OsTestMarker";
		hsf_test_crash_here((volatile int *)16);
		_exit(0);
	}
	close(fds[1]);
	while ((n = read(fds[0], out + got, sizeof(out) - 1 - (size_t)got)) > 0)
		got += n;
	out[got] = '\0';
	waitpid(pid, &status, 0);

	ok(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "the signal is re-raised after reporting");
	ok(strstr(out, "hsf_test_crash_here+") != NULL, "the report names the faulting function");
	ok(strstr(out, "unmapped memory") != NULL, "the report explains the fault");
	ok(strstr(out, "last Os call: OsTestMarker") != NULL, "the report names the last Os call");
	if (!strstr(out, "hsf_test_crash_here"))
		tap_diag("output: %s", out);
	return tap_done();
}
