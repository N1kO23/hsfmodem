/* SPDX-License-Identifier: MIT */
/*
 * Crash reporting.
 *
 * A fault inside blob code gives gdb nothing but an address. This handler
 * maps the faulting instruction to the nearest symbol of our own (non-PIE)
 * executable, explains the signal code, and names the last Os* call the
 * thread made. It runs on the alternate signal stack, so it also works after
 * a stack overflow. Symbols are read from /proc/self/exe at install time;
 * only symbol tables are read, never code.
 */
#include "hsf_os.h"

#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__x86_64__)
typedef Elf64_Ehdr Ehdr;
typedef Elf64_Shdr Shdr;
typedef Elf64_Sym Sym;
#define SYM_TYPE ELF64_ST_TYPE
#else
typedef Elf32_Ehdr Ehdr;
typedef Elf32_Shdr Shdr;
typedef Elf32_Sym Sym;
#define SYM_TYPE ELF32_ST_TYPE
#endif

struct fsym {
	uintptr_t addr;
	const char *name;
};

static struct fsym *syms;
static size_t nsyms;
static void *image;	/* mapped executable; symbol names point into it */

static int cmp_sym(const void *a, const void *b)
{
	const struct fsym *x = a, *y = b;

	return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static void load_symbols(void)
{
	int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
	struct stat st;
	const Ehdr *eh;
	const Shdr *sh;

	if (fd < 0)
		return;
	if (fstat(fd, &st) || (size_t)st.st_size < sizeof(Ehdr)) {
		close(fd);
		return;
	}
	image = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (image == MAP_FAILED) {
		image = NULL;
		return;
	}
	eh = image;
	sh = (const Shdr *)((const char *)image + eh->e_shoff);
	for (unsigned i = 0; i < eh->e_shnum; i++) {
		const Sym *s;
		const char *strtab;
		size_t n;

		if (sh[i].sh_type != SHT_SYMTAB)
			continue;
		s = (const Sym *)((const char *)image + sh[i].sh_offset);
		strtab = (const char *)image + sh[sh[i].sh_link].sh_offset;
		n = sh[i].sh_size / sizeof(Sym);
		syms = calloc(n, sizeof(*syms));
		if (!syms)
			return;
		for (size_t k = 0; k < n; k++) {
			int type = SYM_TYPE(s[k].st_info);

			if (s[k].st_shndx == SHN_UNDEF || !s[k].st_value || !s[k].st_name)
				continue;
			if (type != STT_FUNC && type != STT_NOTYPE && type != STT_OBJECT)
				continue;
			syms[nsyms].addr = (uintptr_t)s[k].st_value;
			syms[nsyms].name = strtab + s[k].st_name;
			nsyms++;
		}
		qsort(syms, nsyms, sizeof(*syms), cmp_sym);
		return;
	}
}

static const struct fsym *nearest(uintptr_t ip)
{
	size_t lo = 0, hi = nsyms;

	if (!nsyms || ip < syms[0].addr)
		return NULL;
	while (hi - lo > 1) {
		size_t mid = (lo + hi) / 2;

		if (syms[mid].addr <= ip)
			lo = mid;
		else
			hi = mid;
	}
	return &syms[lo];
}

static const char *explain(int sig, int code)
{
	if (code == SI_KERNEL)
		return "general protection fault: a privileged instruction or non-canonical address";
	if (sig == SIGSEGV && code == SEGV_MAPERR)
		return "access to unmapped memory (a high address suggests PAGE_OFFSET arithmetic)";
	if (sig == SIGSEGV && code == SEGV_ACCERR)
		return "access not permitted (write to read-only data, or executing data)";
	if (sig == SIGILL && code == ILL_PRVOPC)
		return "privileged instruction";
	if (sig == SIGILL)
		return "illegal instruction";
	if (sig == SIGFPE)
		return "arithmetic exception";
	if (sig == SIGBUS)
		return "bus error";
	return "";
}

static void fault_handler(int sig, siginfo_t *si, void *ucv)
{
	ucontext_t *uc = ucv;
#ifdef __x86_64__
	uintptr_t ip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#else
	uintptr_t ip = (uintptr_t)uc->uc_mcontext.gregs[REG_EIP];
#endif
	const struct fsym *s = nearest(ip);
	char buf[640];
	int n;

	n = snprintf(buf, sizeof(buf),
		     "hsfmodem: fatal signal %d (%s), si_code %d, at %#lx = %s+%#lx, address %p\n"
		     "hsfmodem: %s\n"
		     "hsfmodem: thread %d, last Os call: %s, atomic depth %d\n",
		     sig, strsignal(sig), si->si_code, (unsigned long)ip,
		     s ? s->name : "?", s ? (unsigned long)(ip - s->addr) : 0ul, si->si_addr,
		     explain(sig, si->si_code), (int)hsf_gettid(), hsf_last_call, hsf_atomic_depth);
	if (n > 0)
		(void)!write(STDERR_FILENO, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
	signal(sig, SIG_DFL);	/* re-raise for a core dump */
	raise(sig);
}

void hsf_fault_install(void)
{
	struct sigaction sa;
	static const int sigs[] = { SIGSEGV, SIGILL, SIGBUS, SIGFPE };

	load_symbols();
	hsf_thread_enter(NULL);	/* alternate stack for the calling thread */
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = fault_handler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
		sigaction(sigs[i], &sa, NULL);
}
