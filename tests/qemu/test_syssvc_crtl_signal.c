/*
 * test_syssvc_crtl_signal.c - the C RTL's signals are VMS conditions, ASTs and
 * process services (vms-931).
 *
 * On OpenVMS (HP C RTL Reference Manual, ch. 4, Table 4-5) raise() signals the
 * signal's OpenVMS exception through LIB$SIGNAL and the C RTL's own condition
 * handler maps it back to the program's action; alarm() is a timer AST whose
 * exception is SS$_ASTFLT; kill() names a VMS process. OVMX's C RTL serves all
 * of musl's signal system calls in src/vmsrms/crtl_signal.c. This suite proves,
 * against the real executive:
 *
 *   - raise(SIGSEGV) is LIB$SIGNAL SS$_ACCVIO: a frame handler the program
 *     established sees the condition before the C action runs;
 *   - a program's own LIB$SIGNAL of SS$_ACCVIO / SS$_HPARITH reaches its
 *     SIGBUS / SIGFPE action (the condition-to-signal direction);
 *   - alarm() fires the SIGALRM action from a $SETIMR AST: the frame handler
 *     sees SS$_ASTFLT, and pause() returns after it ($HIBER/$WAKE);
 *   - sigprocmask() defers delivery (a raised and an alarm-fired signal both
 *     wait, show in sigpending(), and run on unblock);
 *   - kill() of another process's VMS PID is the executive's: 0 finds it,
 *     SIGTERM ends it ($FORCEX: $GETJPI then says SS$_NONEXPR), and a gone
 *     PID is ESRCH; kill() of this process's own VMS PID is raise().
 *
 * Under the negctl crtl-signal-falls-to-linux every call goes to the substrate
 * kernel instead: no condition is signalled (the frame handler sees nothing),
 * and kill() takes the VMS PID for a Linux pid (ESRCH).
 *
 * Registered only where the layer is linked (OVMX_CRTL_FD_STATIC; tests/qemu/
 * CMakeLists.txt). No /dev/vms: SKIP.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "chfdef.h"
#include "prcdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

#if defined(OVMX_CRTLFD_STATIC)
void ovmx_crtl_fd_install(void);
extern uint32_t lib$signal(uint32_t, ...);
extern void *lib$establish(void *);
extern uint32_t lib$revert(void);

#define HOLD_SCRIPT "/tmp/ovmx931_hold.sh"

/* What the program's frame handler saw: every condition, resignalled. */
static uint32_t frame_seen[16];
static int frame_n;
static uint32_t frame_handler(struct chf$signal_array *sa, struct chf$mech_array *ma)
{
    (void)ma;
    if (frame_n < 16)
        frame_seen[frame_n++] = sa->chf$is_sig_name;
    return SS$_RESIGNAL;
}
static int frame_saw(uint32_t cond)
{
    for (int i = 0; i < frame_n; i++)
        if ((frame_seen[i] & 0x0FFFFFF8u) == (cond & 0x0FFFFFF8u))
            return 1;
    return 0;
}

/* The last-chance vector: claims what nobody else did, so a condition the C
 * RTL failed to map is a FAIL line here rather than an image exit. */
static int lastchance_n;
static uint32_t lastchance(struct chf$signal_array *sa, struct chf$mech_array *ma)
{
    (void)sa; (void)ma;
    lastchance_n++;
    return SS$_CONTINUE;
}

static volatile int got[32];
static volatile int got_order[8], got_on;
static void on_sig(int s)
{
    if (s > 0 && s < 32) got[s]++;
    if (got_on < 8) got_order[got_on++] = s;
}
static volatile int info_sig;
static volatile uintptr_t info_addr;
static void on_info(int s, siginfo_t *si, void *uc)
{
    (void)uc;
    info_sig = si ? si->si_signo : -1;
    info_addr = si ? (uintptr_t)si->si_addr : 0;
    got[s]++;
}

static void catch_sig(int s)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigemptyset(&sa.sa_mask);
    sigaction(s, &sa, NULL);
}

static struct dsc$descriptor_s str_dsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length = (uint16_t)strlen(s);
    d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static uint32_t my_vms_pid(void)
{
    struct vms_procinfo me;
    return (vms_kif_getjpi_self(&me) & 1) ? me.vms_pid : 0;
}

/* Wait (bounded, on the observed condition) for the VMS clock to pass t+ms. */
static void spin_ms(unsigned ms)
{
    uint64_t t0 = 0, t = 0;
    sys$gettim(&t0);
    do { sys$gettim(&t); } while (t - t0 < (uint64_t)ms * 10000u);
}
#endif

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_crtl_signal: the C RTL's signals are VMS conditions, ASTs and process services ===\n");
#if !defined(OVMX_CRTLFD_STATIC)
#error "test_syssvc_crtl_signal is registered only where the C RTL layer is linked (OVMX_CRTL_FD_STATIC, tests/qemu/CMakeLists.txt)"
#else
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL) || vms_kif_open() < 0) {
        printf("=== test_syssvc_crtl_signal: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    /* The kill() target: a long-lived subprocess, made before the file layer is
     * on (its $CREPRC hands the script to the child as a substrate file). */
    uint32_t tgt_pid = 0, tgt_lpid = 0;
    {
        FILE *hs = fopen(HOLD_SCRIPT, "w");
        if (hs) { fprintf(hs, "sleep 600\n"); fclose(hs); chmod(HOLD_SCRIPT, 0644); }
        struct dsc$descriptor_s img = str_dsc("/bin/sh"), in = str_dsc(HOLD_SCRIPT),
                                nd = str_dsc("OVMX931TGT");
        struct vms_procinfo info;
        if (sys$creprc(&tgt_pid, &img, &in, NULL, NULL, NULL, NULL, &nd, 0, 0, 0, 0) & 1)
            tgt_lpid = (vms_kif_getjpi_pid(tgt_pid, &info) & 1) ? info.linux_pid : 0;
    }
    ovmx_crtl_fd_install();
    (void)sys$setexv(CHF$K_LAST_CHANCE_VECTOR, (void *)lastchance, 3, NULL);
    (void)lib$establish((void *)frame_handler);

    /* -- the action table -- */
    {
        struct sigaction sa, old;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_sig;
        check(sigaction(SIGUSR1, &sa, NULL) == 0 && sigaction(SIGUSR1, NULL, &old) == 0 &&
                  old.sa_handler == on_sig,
              "sigaction(SIGUSR1) records the action and reads it back");
        check(signal(SIGKILL, on_sig) == SIG_ERR && errno == EINVAL,
              "SIGKILL cannot be caught (EINVAL)");
    }

    /* -- raise(): a signal with no SS$ condition runs its action -- */
    catch_sig(SIGUSR1);
    check(raise(SIGUSR1) == 0 && got[SIGUSR1] == 1, "raise(SIGUSR1) runs its action once");

    /* -- raise(SIGSEGV) is LIB$SIGNAL SS$_ACCVIO -- */
    catch_sig(SIGSEGV);
    frame_n = 0; lastchance_n = 0;
    raise(SIGSEGV);
    check(got[SIGSEGV] == 1, "raise(SIGSEGV) runs the SIGSEGV action");
    /* negctl: crtl-signal-falls-to-linux */
    check(frame_saw(SS$_ACCVIO), "raise(SIGSEGV) is LIB$SIGNAL SS$_ACCVIO: the frame handler saw it first");

    /* -- the program's own conditions reach its signal actions -- */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_info;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGBUS, &sa, NULL);
        catch_sig(SIGFPE);
        lastchance_n = 0;
        lib$signal(SS$_ACCVIO, 4, 0u /* access, not length */, 0x7000u, 0u, 0u);
        /* negctl: crtl-signal-falls-to-linux */
        check(got[SIGBUS] == 1 && info_sig == SIGBUS && info_addr == 0x7000u && lastchance_n == 0,
              "LIB$SIGNAL SS$_ACCVIO (access) reaches the SA_SIGINFO SIGBUS action, si_addr = the VA");
        lib$signal(SS$_HPARITH, 0);
        /* negctl: crtl-signal-falls-to-linux */
        check(got[SIGFPE] == 1 && lastchance_n == 0, "LIB$SIGNAL SS$_HPARITH reaches the SIGFPE action");
        lastchance_n = 0;
        lib$signal(SS$_BADPARAM, 0);
        check(lastchance_n == 1, "a condition with the default action is resignalled past the C RTL");
    }

    /* -- sigprocmask() defers a raised signal -- */
    {
        sigset_t b, p;
        catch_sig(SIGUSR2);
        sigemptyset(&b);
        sigaddset(&b, SIGUSR2);
        sigprocmask(SIG_BLOCK, &b, NULL);
        raise(SIGUSR2);
        sigpending(&p);
        check(got[SIGUSR2] == 0 && sigismember(&p, SIGUSR2), "a blocked SIGUSR2 waits, pending");
        sigprocmask(SIG_UNBLOCK, &b, NULL);
        check(got[SIGUSR2] == 1, "unblocking SIGUSR2 runs its action");
    }

    /* -- alarm(): a $SETIMR AST signals SS$_ASTFLT -> SIGALRM; pause() returns -- */
    {
        struct itimerval it;
        catch_sig(SIGALRM);
        frame_n = 0;
        got[SIGALRM] = 0;
        check(alarm(1) == 0, "alarm(1) arms the timer");
        check(getitimer(ITIMER_REAL, &it) == 0 && it.it_value.tv_sec <= 1 &&
                  (it.it_value.tv_sec || it.it_value.tv_usec),
              "getitimer() shows the time left");
        errno = 0;
        int r = pause();
        check(r == -1 && errno == EINTR && got[SIGALRM] == 1,
              "pause() returns EINTR after the SIGALRM action ran");
        /* negctl: crtl-signal-falls-to-linux */
        check(frame_saw(SS$_ASTFLT), "the alarm is the $SETIMR AST's SS$_ASTFLT condition");
        check(alarm(5) == 0 && alarm(0) >= 4, "alarm(0) cancels and returns the seconds left");
    }

    /* -- sigprocmask() defers the AST-fired SIGALRM -- */
    {
        sigset_t b, p;
        got[SIGALRM] = 0;
        sigemptyset(&b);
        sigaddset(&b, SIGALRM);
        sigprocmask(SIG_BLOCK, &b, NULL);
        struct itimerval it = { { 0, 0 }, { 0, 200000 } };
        setitimer(ITIMER_REAL, &it, NULL);
        int pend = 0;
        for (int i = 0; i < 100 && !pend; i++) {       /* <= ~5 s */
            spin_ms(50);
            sigpending(&p);
            pend = sigismember(&p, SIGALRM);
        }
        check(pend && got[SIGALRM] == 0, "a blocked SIGALRM from the timer AST waits, pending");
        sigprocmask(SIG_UNBLOCK, &b, NULL);
        check(got[SIGALRM] == 1, "unblocking it runs the SIGALRM action");
    }

    /* -- setitimer() interval: re-armed from the AST -- */
    {
        struct itimerval it = { { 0, 100000 }, { 0, 100000 } }, z = { { 0, 0 }, { 0, 0 } };
        got[SIGALRM] = 0;
        setitimer(ITIMER_REAL, &it, NULL);
        for (int i = 0; i < 3; i++)
            pause();
        setitimer(ITIMER_REAL, &z, NULL);
        check(got[SIGALRM] == 3, "an interval timer fires every interval (3 pauses, 3 actions)");
        int vr = setitimer(ITIMER_VIRTUAL, &it, NULL);
        int ve = errno;
        if (vr == 0)                   /* never leave a CPU timer armed */
            setitimer(ITIMER_VIRTUAL, &z, NULL);
        /* negctl: crtl-signal-falls-to-linux */
        check(vr == -1 && ve == EINVAL, "ITIMER_VIRTUAL is refused (EINVAL), not faked");
    }

    /* -- kill(): this process's VMS PID is raise() -- */
    {
        uint32_t me = my_vms_pid();
        got[SIGUSR1] = 0;
        /* negctl: crtl-signal-falls-to-linux */
        check(me != 0 && kill((pid_t)me, SIGUSR1) == 0 && got[SIGUSR1] == 1,
              "kill(own VMS PID, SIGUSR1) runs the action");
    }

    /* -- kill(): another VMS process, through the executive -- */
    {
        uint32_t pid = tgt_pid, lpid = tgt_lpid;
        struct vms_procinfo info;
        check(pid != 0 && lpid != 0, "$CREPRC made the kill() target");
        int alive = 0;
        spin_ms(1000);                 /* it is still there a second later: */
        alive = pid && (vms_kif_getjpi_pid(pid, &info) & 1);
        check(alive, "the target lives on its own (sleep 600)");
        /* negctl: crtl-signal-falls-to-linux */
        check(pid && kill((pid_t)pid, 0) == 0, "kill(VMS PID, 0) finds the process ($GETJPI)");
        /* negctl: crtl-signal-falls-to-linux */
        check(pid && kill((pid_t)pid, SIGTERM) == 0, "kill(VMS PID, SIGTERM) is accepted ($FORCEX)");
        int gone = 0;
        for (int i = 0; i < 400 && !gone; i++) {        /* <= ~20 s */
            int st;
            if (lpid) (void)waitpid((pid_t)lpid, &st, WNOHANG);
            gone = vms_kif_getjpi_pid(pid, &info) == SS$_NONEXPR;
            if (!gone) spin_ms(50);
        }
        /* negctl: crtl-signal-falls-to-linux */
        check(gone, "the target is gone: $GETJPI says SS$_NONEXPR");
        errno = 0;
        check(kill((pid_t)pid, 0) == -1 && errno == ESRCH, "kill() of a gone VMS PID is ESRCH");
        if (!gone && lpid) {                   /* never leave it behind */
            uint32_t p2 = pid;
            (void)sys$delprc(&p2, NULL);
            for (int i = 0; i < 200; i++) {
                int st;
                if (waitpid((pid_t)lpid, &st, WNOHANG) == (pid_t)lpid) break;
                spin_ms(10);
            }
        }
    }

    lib$revert();
    printf("=== test_syssvc_crtl_signal: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
#endif
}
