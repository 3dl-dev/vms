/*
 * signal.h - Alpha signal numbers, sigaction flags, and machine context.
 * OVMX alpha-dec-vms musl port (vms-960).
 *
 * Alpha diverges from the generic Linux layout in BOTH the signal numbers
 * (SIGEMT=7, SIGURG=16, SIGSTOP=17, ... SIGUSR1=30, SIGUSR2=31) and the
 * sigaction flag bits (SA_ONSTACK=1, SA_RESTART=2, ...), matching the classic
 * Alpha/OSF (and SPARC-family) conventions. Values from the public Alpha/Linux
 * ABI (Linux arch/alpha uapi/asm/signal.h) and the Alpha calling standard.
 *
 * RUNG-1 note: signal delivery needs the executive backend (GAP3); these are
 * compiled but not yet exercised against a real kernel.
 */

#if defined(_POSIX_SOURCE) || defined(_POSIX_C_SOURCE) \
 || defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) || defined(_BSD_SOURCE)

#define MINSIGSTKSZ 4096
#define SIGSTKSZ 16384

#if defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
typedef unsigned long greg_t;
typedef unsigned long gregset_t[33];
typedef struct {
	unsigned long fpregs[32];
	unsigned long fpcr;
} fpregset_t;

typedef struct sigcontext {
	long sc_onstack;
	long sc_mask;
	long sc_pc;
	long sc_ps;
	long sc_regs[32];
	long sc_ownedfp;
	long sc_fpregs[32];
	unsigned long sc_fpcr;
	unsigned long sc_fp_control;
	unsigned long sc_reserved1, sc_reserved2;
	unsigned long sc_ssize;
	char *sc_sbase;
	unsigned long sc_traparg_a0;
	unsigned long sc_traparg_a1;
	unsigned long sc_traparg_a2;
	unsigned long sc_fp_trap_pc;
	unsigned long sc_fp_trigger_sum;
	unsigned long sc_fp_trigger_inst;
} mcontext_t;
#else
typedef struct {
	unsigned long __regs[67];
} mcontext_t;
#endif

struct sigaltstack {
	void *ss_sp;
	int ss_flags;
	size_t ss_size;
};

typedef struct __ucontext {
	unsigned long uc_flags;
	struct __ucontext *uc_link;
	stack_t uc_stack;
	sigset_t uc_sigmask;
	mcontext_t uc_mcontext;
} ucontext_t;

/* Alpha/OSF sigaction flag bits (distinct from the generic layout). */
#define SA_ONSTACK    0x00000001
#define SA_RESTART    0x00000002
#define SA_NOCLDSTOP  0x00000004
#define SA_NODEFER    0x00000008
#define SA_RESETHAND  0x00000010
#define SA_NOCLDWAIT  0x00000020
#define SA_SIGINFO    0x00000040

#endif

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGIOT    SIGABRT
#define SIGEMT    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGBUS   10
#define SIGSEGV  11
#define SIGSYS   12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGURG   16
#define SIGSTOP  17
#define SIGTSTP  18
#define SIGCONT  19
#define SIGCHLD  20
#define SIGCLD   SIGCHLD
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGIO    23
#define SIGPOLL  SIGIO
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define SIGPWR   29
#define SIGINFO  SIGPWR
#define SIGUSR1  30
#define SIGUSR2  31
#define SIGUNUSED SIGSYS

#define _NSIG 65
