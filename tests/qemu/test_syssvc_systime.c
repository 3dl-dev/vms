/*
 * test_syssvc_systime.c - the system time is the EXECUTIVE's: $GETTIM /
 * $GETUTC / $SETIME and DCL SET TIME / SHOW TIME / F$TIME (rd vms-642),
 * against a real /dev/vms.
 *
 * VMS keeps one system time for the whole system; $SETIME (DCL SET TIME)
 * changes it for every process, and needs OPER and LOG_IO (OpenVMS System
 * Services Reference Manual, $SETIME; OpenVMS DCL Dictionary, SET TIME). What is
 * asserted, and why only an executive-held time can pass:
 *   1. Before any $SETIME the system time is the time-of-year clock (the
 *      substrate's realtime clock); $GETTIM_PREC and $GETUTC agree with it.
 *   2. $SETIME without OPER and LOG_IO -- or with OPER alone -- is SS$_NOPRIV
 *      and moves nothing; a process with no SETPRV cannot get them at all; a
 *      delta time is SS$_IVTIME.
 *   3. After $SETIME to 1-JAN-2031 in THIS process, a SECOND process reads
 *      1-JAN-2031 through $GETTIM, $GETUTC, $ASCTIM(now), $CREATE_UID's
 *      timestamp and the SYSUAF password-lifetime check. A per-process clock
 *      (the substrate's, or anything this image keeps) cannot show another
 *      process the time this one set.
 *   4. The substrate's own clock did not move.
 *   5. DCL: SHOW TIME and F$TIME in a DCL process show the set time (SHOW
 *      TIME's display as VAX V7.3 / Alpha V8.4 print it: two blanks, no
 *      hundredths, $STATUS %X00030001); SET TIME without the privileges is
 *      %SYSTEM-F-NOPRIV; with them SET TIME=dd-mmm-yyyy:hh:mm:ss sets the time
 *      this process then reads; SET TIME with no time recalibrates.
 *   6. $SETIME with no timadr recalibrates: the system time is the
 *      time-of-year clock again (so the suites after this one see real time).
 *
 * No /dev/vms -> the services are SS$_NOSUCHDEV (asserted), then SKIP (77).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "starlet.h"
#include "ssdef.h"
#include "descrip.h"
#include "gen64def.h"
#include "sysuaf.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP 77
#define DCL_PATH "/bin/DCL.EXE"

#define TICKS_PER_SEC   10000000ULL
#define VMS_UNIX_EPOCH  0x007C95674BEB4000ULL   /* 17-NOV-1858 -> 1-JAN-1970 */
#define UTC_OFFSET      0x0135886AC7960000ULL   /* 15-OCT-1582 -> 17-NOV-1858 */
#define UID_EPOCH       0x0135886AC7960000ULL

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

static uint64_t substrate_now(void)        /* the test's view of the host clock */
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * TICKS_PER_SEC + (uint64_t)ts.tv_nsec / 100 + VMS_UNIX_EPOCH;
}

static uint64_t absdiff(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

static uint64_t bintim(const char *s)
{
    struct dsc$descriptor_s d = { (uint16_t)strlen(s), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)s };
    GENERIC_64 q;
    q.gen64$q_quadword = 0;
    return (sys$bintim(&d, &q) & 1) ? q.gen64$q_quadword : 0;
}

static uint32_t setprv(int enable, uint64_t mask)
{
    return sys$setprv(enable, &mask, 0, NULL);
}

static void le64w(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* What the second process saw. */
struct report {
    uint32_t gettim_st, getutc_st, asctim_st, uid_st;
    uint64_t gettim;
    uint64_t utc_abs;
    uint8_t  utc_tdf_hi;
    uint64_t uid_time;
    int      pwd_expired;
    char     asctim[24];
};

/* A password changed 2 days before `target` with a 1-day lifetime: expired at
 * `target`, not yet at the real time (years earlier). */
static int pwd_expired_around(uint64_t target)
{
    sysuaf_record_t r;
    memset(&r, 0, sizeof r);
    strncpy(r.username, "TIMEUSR", sizeof(r.username) - 1);
    le64w(r.raw.uaf$q_pwd_date, target - 2 * 86400ULL * TICKS_PER_SEC);
    le64w(r.raw.uaf$q_pwd_lifetime, 86400ULL * TICKS_PER_SEC);
    return sysuaf_password_expired(&r);
}

static void second_process(int wfd, uint64_t target)
{
    struct report rep;
    uint8_t utc[16], uid[16];
    char buf[24];
    uint16_t len = 0;
    struct dsc$descriptor_s bd = { 23, DSC$K_DTYPE_T, DSC$K_CLASS_S, buf };

    memset(&rep, 0, sizeof rep);
    rep.gettim_st = sys$gettim(&rep.gettim);
    rep.getutc_st = sys$getutc(utc);
    for (int i = 0; i < 8; i++) rep.utc_abs |= (uint64_t)utc[i] << (8 * i);
    rep.utc_tdf_hi = utc[15];
    memset(buf, 0, sizeof buf);
    rep.asctim_st = sys$asctim(&len, &bd, NULL, 0);
    memcpy(rep.asctim, buf, len < 23 ? len : 23);
    rep.uid_st = sys$create_uid(uid);
    for (int i = 0; i < 7; i++) rep.uid_time |= (uint64_t)uid[i] << (8 * i);
    rep.uid_time |= (uint64_t)(uid[7] & 0x0F) << 56;
    rep.pwd_expired = pwd_expired_around(target);
    (void)!write(wfd, &rep, sizeof rep);
}

/* The second process: a subprocess of this one, reporting through a pipe. */
static int run_second(uint64_t target, struct report *rep)
{
    int pfd[2], ws = 0;
    pid_t c;
    ssize_t r;

    if (pipe(pfd) != 0)
        return 0;
    fflush(NULL);
    c = ovmx_fork_subprocess();
    if (c == 0) {
        close(pfd[0]);
        second_process(pfd[1], target);
        _exit(0);
    }
    close(pfd[1]);
    if (c < 0) { close(pfd[0]); return 0; }
    r = read(pfd[0], rep, sizeof *rep);
    close(pfd[0]);
    while (waitpid(c, &ws, 0) < 0 && errno == EINTR)
        ;
    return r == (ssize_t)sizeof *rep;
}

/* An unprivileged process (no SETPRV) tries $SETIME after asking for the
 * privileges it is not authorized to hold. */
static int run_unprivileged_setime(uint64_t target, uint32_t *st)
{
    int pfd[2], ws = 0;
    pid_t c;
    ssize_t r;

    if (pipe(pfd) != 0)
        return 0;
    fflush(NULL);
    c = ovmx_fork_unprivileged(402, 1005);
    if (c == 0) {
        uint32_t s;
        close(pfd[0]);
        if (setgid(402) != 0 || setuid(1005) != 0)
            _exit(2);
        (void)setprv(1, VMS_PRV_M_OPER | VMS_PRV_M_LOG_IO);
        s = sys$setime(&target);
        (void)!write(pfd[1], &s, sizeof s);
        _exit(0);
    }
    close(pfd[1]);
    if (c < 0) { close(pfd[0]); return 0; }
    r = read(pfd[0], st, sizeof *st);
    close(pfd[0]);
    while (waitpid(c, &ws, 0) < 0 && errno == EINTR)
        ;
    return r == (ssize_t)sizeof *st;
}

static char *const dcl_env[] = { (char *)"PATH=/bin", NULL };

static int run_dcl(const char *script, char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2], st;
    size_t used = 0;
    pid_t pid;

    out[0] = '\0';
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0)
        return -1;
    fflush(NULL);
    pid = ovmx_fork_login();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execle(DCL_PATH, "DCL.EXE", (char *)NULL, dcl_env);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (write(in_pipe[1], script, strlen(script)) < 0) { /* the read below shows it */ }
    close(in_pipe[1]);
    for (;;) {
        ssize_t n = read(out_pipe[0], out + used, outsz - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(out_pipe[0]);
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

int main(void)
{
    static char out[16384];
    uint64_t t0 = 0, tp = 0, t1 = 0, now = 0, target, h0;
    uint8_t utc[16];
    uint32_t st;
    struct report rep;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_systime: the system time is the executive's -- $GETTIM/$SETIME, SET/SHOW TIME (rd vms-642) ===\n");
    if (vms_kif_open() < 0) {
        check(sys$gettim(&t0) == SS$_NOSUCHDEV, "no executive: $GETTIM is SS$_NOSUCHDEV, not the host clock");
        check(sys$setime(NULL) == SS$_NOSUCHDEV, "no executive: $SETIME is SS$_NOSUCHDEV");
        printf("=== test_syssvc_systime: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n", pass, fail);
        return fail ? 1 : EXIT_SKIP;
    }

    /* ---- 1. the system time is the time-of-year clock, read three ways ---- */
    h0 = substrate_now();
    check(sys$gettim(&t0) == SS$_NORMAL && absdiff(t0, h0) < 5 * TICKS_PER_SEC,
          "before any $SETIME the system time is the time-of-year (substrate) clock");
    check(sys$gettim_prec(&tp) == SS$_NORMAL && tp >= t0 && tp - t0 < 10 * TICKS_PER_SEC,
          "$GETTIM_PREC agrees with $GETTIM");
    check(sys$getutc(utc) == SS$_NORMAL && sys$gettim(&t1) == SS$_NORMAL, "$GETUTC reads the system time");
    {
        uint64_t a = 0;
        for (int i = 0; i < 8; i++) a |= (uint64_t)utc[i] << (8 * i);
        check(a >= t0 + UTC_OFFSET && a <= t1 + UTC_OFFSET && (utc[15] >> 4) == 1,
              "$GETUTC abstime is the system time + the 1582->1858 offset, structure version 1");
    }

    target = bintim("1-JAN-2031 00:00:00.00");
    check(target > t0, "1-JAN-2031 converts to an absolute time after today");

    /* ---- 2. privileges and the delta time ---- */
    setprv(0, VMS_PRV_M_OPER | VMS_PRV_M_LOG_IO);
    st = sys$setime(&target);
    /* negctl: systime-setime-no-privcheck */
    check(st == SS$_NOPRIV, "$SETIME without OPER and LOG_IO is SS$_NOPRIV");
    check(sys$gettim(&now) == SS$_NORMAL && absdiff(now, substrate_now()) < 5 * TICKS_PER_SEC,
          "... and the system time did not move");
    check(setprv(1, VMS_PRV_M_OPER) & 1, "this process (it holds SETPRV) enables OPER");
    check(sys$setime(&target) == SS$_NOPRIV, "$SETIME with OPER but not LOG_IO is SS$_NOPRIV");
    {
        uint32_t ust = 0;
        check(run_unprivileged_setime(target, &ust) && ust == SS$_NOPRIV,
              "a process with no SETPRV cannot get OPER/LOG_IO: its $SETIME is SS$_NOPRIV");
    }
    check(sys$gettim(&now) == SS$_NORMAL && absdiff(now, substrate_now()) < 5 * TICKS_PER_SEC,
          "... the system time is still the time-of-year clock");
    check(setprv(1, VMS_PRV_M_OPER | VMS_PRV_M_LOG_IO) & 1, "this process enables OPER and LOG_IO");
    {
        uint64_t delta = (uint64_t)-(int64_t)(10 * TICKS_PER_SEC);
        check(sys$setime(&delta) == SS$_IVTIME, "$SETIME of a delta time is SS$_IVTIME");
    }

    /* ---- 3. set it here, read it in a second process ---- */
    check(sys$setime(&target) == SS$_NORMAL, "$SETIME to 1-JAN-2031 00:00:00 with OPER and LOG_IO");
    /* negctl: systime-setime-not-stored */
    check(sys$gettim(&now) == SS$_NORMAL && now >= target && now - target < 60 * TICKS_PER_SEC,
          "this process's $GETTIM now reads 1-JAN-2031");
    memset(&rep, 0, sizeof rep);
    check(run_second(target, &rep), "a second process reported what it read");
    /* negctl: systime-gettim-substrate-clock */
    check(rep.gettim_st == SS$_NORMAL && rep.gettim >= target && rep.gettim - target < 60 * TICKS_PER_SEC,
          "the second process's $GETTIM reads the time this one set");
    check(rep.getutc_st == SS$_NORMAL && rep.utc_abs >= target + UTC_OFFSET &&
          rep.utc_abs - (target + UTC_OFFSET) < 60 * TICKS_PER_SEC && (rep.utc_tdf_hi >> 4) == 1,
          "... its $GETUTC too (abstime = the set time + the 1582 offset)");
    printf("  second process $ASCTIM(now): '%s'\n", rep.asctim);
    check(rep.asctim_st == SS$_NORMAL && strncmp(rep.asctim, " 1-JAN-2031 00:0", 16) == 0,
          "... its $ASCTIM of the current time formats 1-JAN-2031");
    check(rep.uid_st == SS$_NORMAL && rep.uid_time >= target + UID_EPOCH &&
          rep.uid_time - (target + UID_EPOCH) < 60 * TICKS_PER_SEC,
          "... its $CREATE_UID stamps the set time");
    check(rep.pwd_expired == 1,
          "... and SYSUAF ages a password against it: a lifetime ending 31-DEC-2030 has expired");
    check(absdiff(substrate_now(), h0) < 300 * TICKS_PER_SEC,
          "the substrate's own clock did not move (SET TIME is not the host clock)");

    /* ---- 4. recalibrate ---- */
    st = sys$setime(NULL);
    check(st == SS$_NORMAL && sys$gettim(&now) == SS$_NORMAL &&
          absdiff(now, substrate_now()) < 5 * TICKS_PER_SEC,
          "$SETIME with no timadr recalibrates: the system time is the time-of-year clock again");
    check(pwd_expired_around(target) == 0, "... and the same password is not yet expired");

    /* ---- 5. DCL ---- */
    if (access(DCL_PATH, X_OK) != 0) {
        check(0, "/bin/DCL.EXE is present in the initramfs");
    } else {
        /* Without OPER/LOG_IO enabled (a fresh process holds neither). */
        run_dcl("SET PROCESS/PRIVILEGES=(NOOPER,NOLOG_IO)\n"
                "SET TIME=1-JAN-2032:12:00:00\n"
                "SHOW SYMBOL $STATUS\n", out, sizeof out);
        printf("  ---- DCL SET TIME, no privilege ----\n%s  ---- end ----\n", out);
        check(strstr(out, "%SYSTEM-F-NOPRIV") != NULL && strstr(out, "%X00000024") != NULL,
              "DCL SET TIME without OPER and LOG_IO is %SYSTEM-F-NOPRIV");
        check(sys$gettim(&now) == SS$_NORMAL && absdiff(now, substrate_now()) < 5 * TICKS_PER_SEC,
              "... and the system time did not move");

        check(sys$setime(&target) == SS$_NORMAL, "this process sets 1-JAN-2031 again");
        run_dcl("WRITE SYS$OUTPUT \"FTIME=\" + F$TIME()\n"
                "SHOW TIME\n"
                "SHOW SYMBOL $STATUS\n", out, sizeof out);
        printf("  ---- DCL SHOW TIME / F$TIME ----\n%s  ---- end ----\n", out);
        check(strstr(out, "FTIME= 1-JAN-2031 00:0") != NULL,
              "F$TIME in a DCL process is the time this process set");
        /* "  dd-MMM-yyyy hh:mm:ss": two blanks, the day right-justified in 2, no
         * hundredths (tests/qemu/golden/oracle-{vax-v7.3,alpha-v8.4}.console.txt). */
        {
            const char *l = strstr(out, "\n   1-JAN-2031 00:0");
            check(l != NULL && l[20] == ':' && l[23] == '\n',
                  "SHOW TIME in a DCL process shows it as VMS does: '   1-JAN-2031 00:0m:ss', no hundredths");
        }
        check(strstr(out, "$STATUS == \"%X00030001\"") != NULL,
              "... and $STATUS after SHOW TIME is DCL's success %X00030001");

        run_dcl("SET PROCESS/PRIVILEGES=(OPER,LOG_IO)\n"
                "SET TIME=1-JAN-2032:12:00:00\n"
                "SHOW SYMBOL $STATUS\n", out, sizeof out);
        printf("  ---- DCL SET TIME=1-JAN-2032:12:00:00 ----\n%s  ---- end ----\n", out);
        {
            uint64_t want = bintim("1-JAN-2032 12:00:00.00");
            check(want && sys$gettim(&now) == SS$_NORMAL && now >= want && now - want < 60 * TICKS_PER_SEC,
                  "DCL SET TIME=1-JAN-2032:12:00:00 with OPER and LOG_IO: this process reads 1-JAN-2032 12:00");
        }
        run_dcl("SET TIME=1-XYZ-2032:00:00:00\n"
                "SHOW SYMBOL $STATUS\n", out, sizeof out);
        check(strstr(out, "%CLI-W-IVATIME") != NULL && strstr(out, "%X00038290") != NULL,
              "DCL SET TIME of a non-time is %CLI-W-IVATIME");
        run_dcl("SET PROCESS/PRIVILEGES=(OPER,LOG_IO)\n"
                "SET TIME\n"
                "SHOW SYMBOL $STATUS\n", out, sizeof out);
        printf("  ---- DCL SET TIME (recalibrate) ----\n%s  ---- end ----\n", out);
        check(sys$gettim(&now) == SS$_NORMAL && absdiff(now, substrate_now()) < 5 * TICKS_PER_SEC,
              "DCL SET TIME with no time recalibrates from the time-of-year clock");
    }

    /* ---- 6. leave the system time as it was found ---- */
    (void)setprv(1, VMS_PRV_M_OPER | VMS_PRV_M_LOG_IO);
    check(sys$setime(NULL) == SS$_NORMAL && sys$gettim(&now) == SS$_NORMAL &&
          absdiff(now, substrate_now()) < 5 * TICKS_PER_SEC,
          "the system time is recalibrated for the suites after this one");

    printf("=== test_syssvc_systime: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
