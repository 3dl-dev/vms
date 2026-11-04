/*
 * test_syssvc_crtl_shr.c - a C program activated against the x86_64 DECC$SHR,
 * RUN from DCL, has VMS terminals, pipes, names, clock and signals (rd vms-569).
 *
 * On OpenVMS a C program's stdout on a terminal is SYS$OUTPUT ($QIO through
 * the terminal driver), pipe() is a mailbox, "/proc/self/status" is an RMS
 * name RMS cannot find, time() is the system time and alarm() is a $SETIMR
 * AST. OVMX's C RTL does each in src/vmsrms/crtl_{rms_fd,devfd,time,signal}.c.
 * The Alpha DECC$SHR turns that layer on in decc$main; the x86_64 DECC$SHR
 * (src/vmslink/mk_decc_shr.sh's RMS-backed pass) exports ovmx_crtl_fd_install
 * and the image activator calls it before main() (src/imgact/imgact.c,
 * crtl_image_init). Until then an activated x86_64 program sent all of it to
 * Linux.
 *
 * THE SHAPE. The suite runs the shipped DCL.EXE with its standard output on a
 * DECOY terminal line of the substrate's (what a process run from a terminal
 * inherits underneath), mints an RTAn: unit of the executive's terminal
 * driver, and feeds DCL
 *
 *     DEFINE SYS$OUTPUT <RTAn:>
 *     RUN VDA300:[SYS0.SYSCOMMON.SYSEXE]CRTLSHR.EXE
 *
 * CRTLSHR (tests/qemu/crtlshr/CRTLSHR.C, linked --use DECC$SHR) reports what
 * it sees through printf(). Asserted, all from the suite's side:
 *
 *   1. its report arrives on the RTAn: line -- SYS$OUTPUT's terminal, through
 *      the terminal driver -- and nothing of it on the decoy underneath;
 *      getname(1) names that terminal;
 *   2. getname() of its pipe() is a mailbox; DCL's SHOW DEVICE (a second DCL)
 *      shows that mailbox while the program waits on it; and a message THIS
 *      process writes into the mailbox with $QIO is what the program's read()
 *      of its pipe returns;
 *   3. open("/proc/self/status") fails ENOENT;
 *   4. with the executive's system time set to 1-JAN-2031 ($SETIME), the
 *      program's time() is 2031 -- far from the substrate clock;
 *   5. alarm(1) runs its SIGALRM action from a timer AST: the frame handler it
 *      established saw SS$_ASTFLT.
 *   6. fork() makes the child a VMS subprocess that holds the pipe(): what the
 *      child writes into the pipe is what the parent reads, and the child's
 *      $GETJPI JPI$_PID succeeds with a PID that is not its creator's.
 *
 * NEGATIVE CONTROLS: tests/qemu/defects.d/crtl-shr-layer-not-started.defect
 * removes the activator's call; every assertion above goes red.
 * crtl-shr-fork-not-subprocess.defect makes fork() a bare substrate fork: 6
 * goes red.
 *
 * Registered only where the RMS-backed DECC$SHR is built (OVMX_CRTL_FD_STATIC:
 * the musl system-call funnel, tests/qemu/CMakeLists.txt). No /dev/vms: SKIP.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "starlet.h"
#include "ssdef.h"
#include "descrip.h"
#include "iodef.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "vmsfs/ods2.h"
#include "ovmx_vterm.h"
#include "ovmx_test_fork.h"
#include "tcg_deadline.h"
#include "sysvol_stage.h"

#define EXIT_SKIP 77
#define DCL_PATH "/bin/DCL.EXE"
#define SUBJECT_HOST "/vms/SYS0/SYSCOMMON/SYSEXE/CRTLSHR.EXE"
#define SUBJECT_VMS SYSVOL_UNIT "[SYS0.SYSCOMMON.SYSEXE]CRTLSHR.EXE"
#define RUN_MS 60000
#define TICKS_PER_SEC  10000000ULL
#define VMS_UNIX_EPOCH 0x007C95674BEB4000ULL   /* 17-NOV-1858 -> 1-JAN-1970 */
#define T2031 1924992000LL                      /* 1-JAN-2031 00:00:00 UTC */
#define PIPE_MSG "GO-569"

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

/* Read what is ready on fd (a pty master) into buf, appending. */
static void slurp(int fd, char *buf, size_t cap, size_t *len, int ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    while (*len < cap - 1 && poll(&p, 1, ms) > 0) {
        ssize_t r = read(fd, buf + *len, cap - 1 - *len);
        if (r <= 0)
            break;
        *len += (size_t)r;
        ms = 0;
    }
    buf[*len] = '\0';
}

/* The value of the report line "CRTLSHR <key> <a> <b> <c>": its three fields. */
static int report(const char *buf, const char *key, char *a, size_t asz, long *b, long *c)
{
    char want[64];
    snprintf(want, sizeof want, "CRTLSHR %s ", key);
    const char *p = strstr(buf, want);
    if (!p)
        return 0;
    p += strlen(want);
    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\r' && p[n] != '\n')
        n++;
    if (n >= asz)
        n = asz - 1;
    memcpy(a, p, n);
    a[n] = '\0';
    p += n;
    char *e;
    long v1 = strtol(p, &e, 10);
    long v2 = strtol(e, &e, 10);
    if (*e != '\r' && *e != '\n')
        return 0;                       /* the line is not complete yet */
    if (b) *b = v1;
    if (c) *c = v2;
    return 1;
}

/* Feed a DCL a script on its standard input; its stdout/stderr go to outfd.
 * The DCL leads its own process group so it and its image can be stopped. */
static pid_t start_dcl(const char *script, int outfd)
{
    char path[] = "/tmp/crtlshr_dcl.XXXXXX";
    int sfd = mkstemp(path);
    if (sfd < 0)
        return -1;
    size_t n = strlen(script);
    if (write(sfd, script, n) != (ssize_t)n) {
        close(sfd);
        unlink(path);
        return -1;
    }
    lseek(sfd, 0, SEEK_SET);
    unlink(path);
    fflush(NULL);
    pid_t pid = ovmx_fork_subprocess();
    if (pid == 0) {
        setpgid(0, 0);
        dup2(sfd, 0);
        dup2(outfd, 1);
        dup2(outfd, 2);
        execl(DCL_PATH, "DCL.EXE", (char *)NULL);
        _exit(127);
    }
    close(sfd);
    if (pid > 0)
        setpgid(pid, pid);
    return pid;
}

static int wait_bounded(pid_t pid, int ms)
{
    int st = 0;
    for (long t = 0; t < ovmx_tcg_ms(ms); t += 20) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid)
            return 1;
        if (r < 0)
            return 0;
        usleep(20000);
    }
    kill(-pid, SIGKILL);
    waitpid(pid, &st, 0);
    return 0;
}

/* One DCL command, its output captured whole. */
static int dcl_capture(const char *cmd, char *out, size_t cap)
{
    char cpath[] = "/tmp/crtlshr_out.XXXXXX";
    int cfd = mkstemp(cpath);
    out[0] = '\0';
    if (cfd < 0)
        return 0;
    unlink(cpath);
    char script[256];
    snprintf(script, sizeof script, "%s\n", cmd);
    pid_t pid = start_dcl(script, cfd);
    int ok = pid > 0 && wait_bounded(pid, 30000);
    lseek(cfd, 0, SEEK_SET);
    ssize_t r = read(cfd, out, cap - 1);
    out[r > 0 ? r : 0] = '\0';
    close(cfd);
    return ok;
}

/* $QIO one message into a mailbox, from this process. */
static uint32_t mbx_send(const char *mbx, const char *msg)
{
    struct dsc$descriptor_s d = { (uint16_t)strlen(mbx), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)mbx };
    uint16_t chan = 0, iosb[4] = { 0, 0, 0, 0 };
    uint32_t st = sys$assign(&d, &chan, 0, NULL, 0);
    if (!(st & 1))
        return st;
    st = sys$qiow(0, chan, IO$_WRITEVBLK | IO$M_NOW, iosb, NULL, 0,
                  (void *)msg, (uint32_t)strlen(msg), 0, 0, 0, 0);
    if ((st & 1) && !(iosb[0] & 1))
        st = iosb[0];
    (void)sys$dassgn(chan);
    return st;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_crtl_shr: an activated DECC$SHR program run from DCL has VMS terminals, pipes, names, clock and signals ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL) || vms_kif_open() < 0) {
        printf("=== test_syssvc_crtl_shr: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    check(sysvol_stage_subject(SUBJECT_HOST, "CRTLSHR.EXE") == 0,
          "CRTLSHR.EXE staged on " SYSVOL_UNIT " (IMGACT reads a main image off the volume)");
    check(sysvol_stage_shareables_from("/vms/SYS0/SYSCOMMON/SYSLIB") >= 0,
          "the shareables it --uses staged in " SYSVOL_UNIT " SYS$LIBRARY");
    /* DCL stages a RUN image for the kernel under the boot staging root, which
     * PID 1 makes on a booted node; this harness's init does not. */
    (void)mkdir("/run", 0755);
    (void)mkdir("/run/ovmx-boot", 0755);

    /* the substrate's line underneath: DCL's (and so the image's) descriptor 1 */
    int dm = posix_openpt(O_RDWR | O_NOCTTY), ds = -1;
    if (dm >= 0 && grantpt(dm) == 0 && unlockpt(dm) == 0)
        ds = open(ptsname(dm), O_RDWR | O_NOCTTY);
    check(ds >= 0, "a decoy terminal line of the substrate's for DCL's standard output");

    /* the terminal SYS$OUTPUT will name */
    char dev[32] = "";
    int tm = -1;
    uint32_t vst = ovmx_vterm_create(dev, sizeof dev, &tm);
    check((vst & 1) && tm >= 0 && dev[0], "an RTAn: terminal unit of the executive's terminal driver");
    char want_tt[32];
    snprintf(want_tt, sizeof want_tt, "%s", dev + (dev[0] == '_'));
    if (want_tt[0] && want_tt[strlen(want_tt) - 1] == ':')
        want_tt[strlen(want_tt) - 1] = '\0';

    /* the system time: 1-JAN-2031, which the substrate clock is not */
    uint64_t privs = VMS_PRV_M_OPER | VMS_PRV_M_LOG_IO;
    uint64_t t2031 = (uint64_t)T2031 * TICKS_PER_SEC + VMS_UNIX_EPOCH;
    (void)sys$setprv(1, &privs, 0, NULL);
    uint32_t tst = sys$setime(&t2031);
    check(tst == SS$_NORMAL, "$SETIME: the executive's system time is 1-JAN-2031");

    char tt[16384] = "", decoy[16384] = "", show[8192] = "";
    size_t ttn = 0, dn = 0;
    char mbx[64] = "", a[128];
    long b = 0, c = 0;
    int sent = 0, showed = 0;
    uint32_t sst = 0;

    char script[512];
    snprintf(script, sizeof script, "DEFINE SYS$OUTPUT %s\nRUN %s\n", dev, SUBJECT_VMS);
    pid_t dcl = (ds >= 0 && tm >= 0) ? start_dcl(script, ds) : -1;
    check(dcl > 0, "DCL started: DEFINE SYS$OUTPUT <the RTAn:>, RUN CRTLSHR.EXE");

    long deadline = ovmx_tcg_mono_ms() + ovmx_tcg_ms(RUN_MS);
    while (dcl > 0 && ovmx_tcg_mono_ms() < deadline) {
        slurp(tm, tt, sizeof tt, &ttn, 100);
        slurp(dm, decoy, sizeof decoy, &dn, 0);
        if (!sent && report(tt, "PIPE", mbx, sizeof mbx, &b, &c)) {
            sent = 1;
            if (strncmp(mbx + (mbx[0] == '_'), "MBA", 3) == 0) {
                char cmd[96];
                snprintf(cmd, sizeof cmd, "SHOW DEVICE %s", mbx);
                showed = dcl_capture(cmd, show, sizeof show);
                sst = mbx_send(mbx, PIPE_MSG);
            }
        }
        if (strstr(tt, "CRTLSHR DONE") && report(tt, "DONE", a, sizeof a, NULL, NULL))
            break;
        if (waitpid(dcl, NULL, WNOHANG) == dcl) {
            dcl = 0;
            slurp(tm, tt, sizeof tt, &ttn, 500);
            break;
        }
    }
    if (dcl > 0 && !wait_bounded(dcl, 10000))
        printf("  INFO: DCL did not end by itself; its process group was stopped\n");
    slurp(dm, decoy, sizeof decoy, &dn, 300);
    (void)sys$setime(NULL);             /* recalibrate: the suites after see real time */

    printf("  ---- on %s ----\n%s\n  ---- on the decoy line ----\n%s\n  ---- end ----\n", dev, tt, decoy);

    /* 1. stdout is SYS$OUTPUT's terminal */
    /* negctl: crtl-shr-layer-not-started */
    check(strstr(tt, "CRTLSHR GETNAME1") != NULL && strstr(tt, "CRTLSHR DONE") != NULL,
          "the program's printf() report arrived on SYS$OUTPUT's RTAn: line, through the terminal driver");
    /* negctl: crtl-shr-layer-not-started */
    check(strstr(decoy, "CRTLSHR GETNAME1") == NULL && strstr(decoy, "CRTLSHR PROC") == NULL &&
              strstr(decoy, "CRTLSHR DONE") == NULL,
          "...and none of it on the substrate's line underneath descriptor 1");
    /* negctl: crtl-shr-layer-not-started */
    check(report(tt, "GETNAME1", a, sizeof a, NULL, NULL) && want_tt[0] && strstr(a, want_tt) != NULL,
          "getname(1) is SYS$OUTPUT's terminal device");

    /* 2. pipe() is a mailbox */
    /* negctl: crtl-shr-layer-not-started */
    check(report(tt, "PIPE", mbx, sizeof mbx, &b, &c) && b == 0 &&
              strncmp(mbx + (mbx[0] == '_'), "MBA", 3) == 0,
          "getname() of the program's pipe() is a mailbox (MBAn:)");
    {
        char unit[64];
        snprintf(unit, sizeof unit, "%s", mbx + (mbx[0] == '_'));
        if (unit[0] && unit[strlen(unit) - 1] == ':')
            unit[strlen(unit) - 1] = '\0';
        if (showed)
            printf("  ---- SHOW DEVICE %s ----\n%s  ---- end ----\n", mbx, show);
        /* negctl: crtl-shr-layer-not-started */
        check(showed && unit[0] && strstr(show, unit) != NULL && strstr(show, "%SYSTEM-") == NULL,
              "DCL SHOW DEVICE shows that mailbox while the program holds its pipe");
    }
    /* negctl: crtl-shr-layer-not-started */
    check((sst & 1) && report(tt, "PIPE-READ", a, sizeof a, &b, NULL) && strcmp(a, PIPE_MSG) == 0,
          "a message this process $QIOs into the mailbox is what the program's read() of its pipe returns");

    /* 3. no kernel namespace */
    /* negctl: crtl-shr-layer-not-started */
    check(report(tt, "PROC", a, sizeof a, &b, &c) && strcmp(a, "refused") == 0 && c == ENOENT,
          "open(\"/proc/self/status\") fails ENOENT");

    /* 4. the clock */
    /* negctl: crtl-shr-layer-not-started */
    check(report(tt, "TIME", a, sizeof a, &b, NULL) && b >= T2031 - 86400 && b < T2031 + 86400 + 600 &&
              (long)time(NULL) < T2031 - 86400,
          "time() is the executive's system time (2031), not the substrate clock");

    /* 5. alarm() is a timer AST */
    check(report(tt, "ALARM", a, sizeof a, &b, &c) && b == 1, "alarm(1) ran the SIGALRM action");
    /* negctl: crtl-shr-layer-not-started */
    check(report(tt, "ALARM", a, sizeof a, &b, &c) && c == 1,
          "...from a $SETIMR AST: the program's frame handler saw SS$_ASTFLT");

    /* 6. fork(): a subprocess that holds the pipe */
    {
        char f[64];
        long me = 0, mst = 0;
        unsigned long cpid = 0, cst = 0;
        int got = report(tt, "FORK", f, sizeof f, &me, &mst) &&
                  sscanf(f, "%lx,%lu", &cpid, &cst) == 2;
        /* negctl: crtl-shr-fork-not-subprocess */
        check(got, "fork(): what the child writes into the pipe() it inherited is what the parent reads");
        /* negctl: crtl-shr-fork-not-subprocess */
        check(got && (mst & 1) && (cst & 1) && cpid != 0 && cpid != (unsigned long)me,
              "...and the child is a VMS process of its own: its $GETJPI JPI$_PID succeeds, not its creator's PID");
    }

    if (tm >= 0)
        (void)ovmx_vterm_delete(dev, tm);
    printf("=== test_syssvc_crtl_shr: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
