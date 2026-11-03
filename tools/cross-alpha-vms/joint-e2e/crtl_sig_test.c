/*
 * crtl_sig_test.c - DEC C signals on OVMX/Alpha are VMS conditions and ASTs
 * (rd vms-931).
 *
 * An ordinary DEC C program against the RMS-backed DECC$SHR, whose decc$main
 * turns on src/vmsrms/crtl_signal.c. Run by SYSTARTUP on the real executive, it
 * prints one CRTLSIG line per property:
 *   raise    raise(SIGSEGV) is LIB$SIGNAL SS$_ACCVIO: the frame handler this
 *            program established saw the condition, then the SIGSEGV action ran
 *   cond     a LIB$SIGNAL of SS$_HPARITH ran the SIGFPE action
 *   mask     a raised SIGUSR2 waited while blocked and ran on unblock
 *   alarm    alarm(1) + pause(): the SIGALRM action ran from the $SETIMR AST
 *            (the frame handler saw SS$_ASTFLT), pause() returned EINTR
 *   kill     kill(this process's VMS PID, SIGUSR1) ran the SIGUSR1 action
 * then "OVMX CRTL-SIG test: OK" only when every check held. With the signal
 * calls on the Linux/Alpha kernel instead, no condition is signalled (raise,
 * cond and alarm print seen=0) and kill() of a VMS PID is ESRCH.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

unsigned int lib$signal(unsigned int, ...);
void *lib$establish(void *);
unsigned int lib$revert(void);
unsigned int sys$getjpiw(unsigned int, unsigned int *, void *, void *, void *,
                         void *, unsigned int);

#define SS_RESIGNAL 2328u
#define SS_ACCVIO   12u
#define SS_ASTFLT   1036u
#define SS_HPARITH  1284u

static unsigned int seen[16];
static int nseen;
static unsigned int frame_handler(unsigned int *sa, void *ma)
{
    (void)ma;
    if (nseen < 16)
        seen[nseen++] = sa[1];
    return SS_RESIGNAL;
}
static int saw(unsigned int c)
{
    for (int i = 0; i < nseen; i++)
        if ((seen[i] & 0x0FFFFFF8u) == (c & 0x0FFFFFF8u))
            return 1;
    return 0;
}

static volatile int got[32];
static void on_sig(int s) { if (s > 0 && s < 32) got[s]++; }

static unsigned int my_pid(void)
{
    unsigned int pid = 0;
    unsigned short rl = 0, iosb[4] = { 0 };
    struct { unsigned short len, code; void *buf; unsigned short *ret; } il[2];
    memset(il, 0, sizeof il);
    il[0].len = 4;
    il[0].code = 0x0319;                /* JPI$_PID */
    il[0].buf = &pid;
    il[0].ret = &rl;
    return (sys$getjpiw(128, 0, 0, il, iosb, 0, 0) & 1) ? pid : 0;
}

int main(void)
{
    int ok = 1, r, e, pend;
    sigset_t b, p;

    lib$establish((void *)frame_handler);

    signal(SIGSEGV, on_sig);
    nseen = 0;
    raise(SIGSEGV);
    printf("CRTLSIG: raise SIGSEGV action=%d seen-ACCVIO=%d\n", got[SIGSEGV], saw(SS_ACCVIO));
    ok &= got[SIGSEGV] == 1 && saw(SS_ACCVIO);

    signal(SIGFPE, on_sig);
    lib$signal(SS_HPARITH, 0);
    printf("CRTLSIG: cond HPARITH SIGFPE-action=%d\n", got[SIGFPE]);
    ok &= got[SIGFPE] == 1;

    signal(SIGUSR2, on_sig);
    sigemptyset(&b);
    sigaddset(&b, SIGUSR2);
    sigprocmask(SIG_BLOCK, &b, 0);
    raise(SIGUSR2);
    sigpending(&p);
    pend = sigismember(&p, SIGUSR2) && got[SIGUSR2] == 0;
    sigprocmask(SIG_UNBLOCK, &b, 0);
    printf("CRTLSIG: mask held=%d then action=%d\n", pend, got[SIGUSR2]);
    ok &= pend && got[SIGUSR2] == 1;

    signal(SIGALRM, on_sig);
    nseen = 0;
    alarm(1);
    errno = 0;
    r = pause();
    e = errno;
    printf("CRTLSIG: alarm action=%d pause=%d errno=%s seen-ASTFLT=%d\n", got[SIGALRM], r,
           e == EINTR ? "EINTR" : "other", saw(SS_ASTFLT));
    ok &= got[SIGALRM] == 1 && r == -1 && e == EINTR && saw(SS_ASTFLT);

    signal(SIGUSR1, on_sig);
    r = kill((int)my_pid(), SIGUSR1);
    printf("CRTLSIG: kill own-VMS-PID rc=%d action=%d\n", r, got[SIGUSR1]);
    ok &= r == 0 && got[SIGUSR1] == 1;

    lib$revert();
    printf("OVMX CRTL-SIG test: %s\n", ok ? "OK (signals are VMS conditions and ASTs)" : "FAILED");
    printf("CRTLSIG-END\n");
    return ok ? 1 : 44;
}
