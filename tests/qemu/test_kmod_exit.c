/*
 * test_kmod_exit.c - Executive $EXIT / $STATUS + CLI invocation context (vms-f60d)
 *
 * WHAT THIS PROVES
 *
 * This is the executive half of IMGACT's VMS-standard image return path
 * (src/imgact/imgact.c, src/imgact/include/ovmx_activation.h). When a port
 * image's crt0 (__main -> decc$main -> main) returns a VMS condition value,
 * IMGACT routes it through the executive $EXIT so the value becomes the
 * process's REAL completion $STATUS -- not a userspace-fabricated exit code.
 * Symmetrically, the cliflag and invoking command line IMGACT presents to
 * that crt0 come FROM the executive process context, not a Linux env var.
 * Both facilities are exercised here against a real /dev/vms with raw
 * ioctl(2) -- the same mechanism imgact_acp.c uses.
 *
 * The tests are built around the property a per-process userspace fake could
 * never satisfy -- the recorded status and command line are visible to OTHER
 * processes and survive the recording image:
 *
 *   1. $EXIT/$STATUS, self: a process records a condition value and reads it
 *      back as its $STATUS, with bit<0> (STS$M_SUCCESS) and bits<2:0>
 *      (STS$V_SEVERITY) decoded exactly as $STATUS/$SEVERITY report them.
 *      Recording the value 0 is distinguished from "no image has exited" by
 *      the has_exited flag -- a reader never guesses that from a zero.
 *
 *   2. $EXIT/$STATUS, cross-process: PROCESS A records a condition value;
 *      PROCESS B reads A's completion status back by VMS PID. A fake living
 *      in A's own address space could not do this.
 *
 *   3. CLI context round-trip: a process records a cliflag + command line and
 *      reads exactly them back.
 *
 *   4. CLI context INHERITANCE: a process records a command line, then a
 *      child that REGISTER_CONTINUEs the parent's identity reads the SAME
 *      command line back -- the mechanism by which DCL sets the command line
 *      once and every image it activates sources its own invoking command
 *      line from the executive (INV-6, no env-var shim).
 *
 * DEVICE-ABSENT CONTRACT (ci.yml kernel-executive-negative-control): a
 * test_kmod_* other than test_kmod_vmsfs* must exit NONZERO when /dev/vms is
 * absent -- it needs the executive and fails honestly without it, never a
 * fabricated pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "vms_ioctl.h"

#define SS_NORMAL   1

/*
 * VMS condition values used as $STATUS test vectors. ORACLE-GROUNDED where a
 * real code is available: SS$_NORMAL (1) and SS$_ABORT (44) are the ssdef.h
 * values (odd = success bit<0> set; even = failure). The decode this test
 * asserts -- bit<0> success, bits<2:0> severity -- is the public $STATUS /
 * $SEVERITY definition (OpenVMS DCL Dictionary), not an OVMX invention.
 */
#define COND_OK     1u          /* SS$_NORMAL:  success, severity 1 (SUCCESS)   */
#define COND_ABORT  44u         /* SS$_ABORT:   failure, severity 4 (SEVERE)    */
#define COND_WARN   0u          /* severity 0 (WARNING), bit<0> clear           */
#define COND_INFO   0x0000012Bu /* bit<0> set, severity 3 (INFO): 0x12B & 7 == 3 */
#define COND_CHILD  0x00038090u /* cross-process vector: bit<0> clear, sev 0    */

#define STS_SUCCESS(c) ((unsigned)((c) & 0x1u))
#define STS_SEVERITY(c) ((unsigned)((c) & 0x7u))

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* Register the calling task and return its assigned VMS PID, or 0 on error. */
static uint32_t do_register(int fd, int continue_identity)
{
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    unsigned long cmd = continue_identity ? VMS_IOCTL_REGISTER_CONTINUE
                                          : VMS_IOCTL_REGISTER;
    if (ioctl(fd, cmd, &reg) != 0 || reg.status != SS_NORMAL)
        return 0;
    return reg.vms_pid;
}

/* SETEXIT(cond) on the calling process; fills *out with the reply. */
static int do_setexit(int fd, uint32_t cond, struct vms_exit_args *out)
{
    struct vms_exit_args a;
    memset(&a, 0, sizeof(a));
    a.condition = cond;
    if (ioctl(fd, VMS_IOCTL_SETEXIT, &a) != 0)
        return -1;
    *out = a;
    return 0;
}

/* GETEXIT for self (pid==0) or another process by VMS PID. */
static int do_getexit(int fd, uint32_t select, uint32_t vms_pid,
                      struct vms_getexit_args *out)
{
    struct vms_getexit_args a;
    memset(&a, 0, sizeof(a));
    a.select = select;
    a.vms_pid = vms_pid;
    if (ioctl(fd, VMS_IOCTL_GETEXIT, &a) != 0)
        return -1;
    *out = a;
    return 0;
}

/* One $EXIT/$STATUS self round-trip: record `cond`, read it back, and assert
 * the recorded longword and its decoded success/severity/exit_code. */
static void check_status_roundtrip(int fd, uint32_t cond, const char *label)
{
    char msg[160];
    struct vms_exit_args se;
    struct vms_getexit_args ge;

    if (do_setexit(fd, cond, &se) != 0) {
        snprintf(msg, sizeof msg, "%s: SETEXIT ioctl succeeded", label);
        CHECK(0, msg);
        return;
    }
    snprintf(msg, sizeof msg, "%s: SETEXIT status is SS$_NORMAL", label);
    CHECK(se.status == SS_NORMAL, msg);

    snprintf(msg, sizeof msg, "%s: SETEXIT decodes success bit<0> = %u",
             label, STS_SUCCESS(cond));
    CHECK(se.success == STS_SUCCESS(cond), msg);
    snprintf(msg, sizeof msg, "%s: SETEXIT decodes severity<2:0> = %u",
             label, STS_SEVERITY(cond));
    CHECK(se.severity == STS_SEVERITY(cond), msg);
    snprintf(msg, sizeof msg, "%s: SETEXIT maps to POSIX exit_code %u",
             label, STS_SUCCESS(cond) ? 0u : 1u);
    CHECK(se.exit_code == (STS_SUCCESS(cond) ? 0u : 1u), msg);

    if (do_getexit(fd, VMS_JPI_SEL_SELF, 0, &ge) != 0) {
        snprintf(msg, sizeof msg, "%s: GETEXIT(self) ioctl succeeded", label);
        CHECK(0, msg);
        return;
    }
    snprintf(msg, sizeof msg, "%s: GETEXIT(self) status is SS$_NORMAL", label);
    CHECK(ge.status == SS_NORMAL, msg);
    snprintf(msg, sizeof msg,
             "%s: $STATUS reads back the EXACT condition value 0x%08x",
             label, cond);
    CHECK(ge.condition == cond, msg);
    snprintf(msg, sizeof msg, "%s: GETEXIT reports has_exited = 1", label);
    CHECK(ge.has_exited == 1, msg);
    snprintf(msg, sizeof msg, "%s: GETEXIT success bit matches (%u)",
             label, STS_SUCCESS(cond));
    CHECK(ge.success == STS_SUCCESS(cond), msg);
    snprintf(msg, sizeof msg, "%s: GETEXIT severity matches (%u)",
             label, STS_SEVERITY(cond));
    CHECK(ge.severity == STS_SEVERITY(cond), msg);
}

/* --- Part B/C child helpers ------------------------------------------------
 * A child either records its OWN completion status (for the parent to read
 * back by PID) or REGISTER_CONTINUEs and reports the CLI context it inherited.
 * Communication is over two pipes: child->parent (results) and parent->child
 * (a go byte the child waits on so its PCB stays alive until the parent has
 * finished reading it). */

struct child_exit_report {
    uint32_t vms_pid;       /* the VMS PID the executive assigned the child */
    uint32_t ok;            /* 1 iff the child registered + recorded cleanly */
};

struct child_cli_report {
    uint32_t ok;            /* 1 iff REGISTER_CONTINUE + GETCLI succeeded */
    uint8_t  cliflag;       /* the inherited cliflag */
    uint16_t length;        /* the inherited command-line length */
    char     command[VMS_CLI_CMDLINE_SIZE];
};

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_kmod_exit ===\n");

    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) {
        /* Fail honest -- this suite needs the executive (INV-6). */
        printf("  FAIL: cannot open /dev/vms (executive absent)\n");
        return 1;
    }

    uint32_t self_pid = do_register(fd, 0);
    CHECK(self_pid != 0, "the calling process registers with the executive");
    if (self_pid == 0) {
        printf("=== test_kmod_exit: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    /* --- Part 1: $EXIT/$STATUS self round-trips ------------------------- */
    printf("--- $EXIT / $STATUS (self) ---\n");
    check_status_roundtrip(fd, COND_OK,    "SS$_NORMAL");
    check_status_roundtrip(fd, COND_ABORT, "SS$_ABORT");
    check_status_roundtrip(fd, COND_INFO,  "INFO-severity");
    /* Recording 0 must still read back has_exited==1 -- the flag, not a zero
     * condition, is what says "an image completed". */
    check_status_roundtrip(fd, COND_WARN,  "WARNING-severity(0)");

    /* --- Part 2: $EXIT/$STATUS across processes ------------------------- */
    printf("--- $EXIT / $STATUS (cross-process A-writes / B-reads) ---\n");
    {
        int r2c[2], c2r[2];
        if (pipe(r2c) != 0 || pipe(c2r) != 0) {
            CHECK(0, "pipe() for cross-process exit test");
        } else {
            pid_t kid = fork();
            if (kid == 0) {
                /* CHILD (process A): own PCB, record COND_CHILD, then block
                 * until the parent has read it so the PCB is not reaped. */
                close(r2c[1]); close(c2r[0]);
                int cfd = open("/dev/vms", O_RDWR);
                struct child_exit_report rep; memset(&rep, 0, sizeof rep);
                if (cfd >= 0) {
                    uint32_t cpid = do_register(cfd, 0);
                    struct vms_exit_args se;
                    if (cpid != 0 && do_setexit(cfd, COND_CHILD, &se) == 0 &&
                        se.status == SS_NORMAL) {
                        rep.vms_pid = cpid;
                        rep.ok = 1;
                    }
                }
                (void)!write(c2r[1], &rep, sizeof rep);
                char go;
                (void)!read(r2c[0], &go, 1);   /* wait for parent's go-ahead */
                if (cfd >= 0) close(cfd);
                _exit(0);
            }
            /* PARENT (process B). */
            close(r2c[0]); close(c2r[1]);
            struct child_exit_report rep; memset(&rep, 0, sizeof rep);
            ssize_t n = read(c2r[0], &rep, sizeof rep);
            CHECK(n == (ssize_t)sizeof rep && rep.ok,
                  "process A registered and recorded its completion status");
            if (n == (ssize_t)sizeof rep && rep.ok) {
                struct vms_getexit_args ge;
                int rc = do_getexit(fd, VMS_JPI_SEL_PID, rep.vms_pid, &ge);
                CHECK(rc == 0 && ge.status == SS_NORMAL,
                      "process B reads A's completion status by VMS PID");
                CHECK(rc == 0 && ge.condition == COND_CHILD,
                      "B reads back the EXACT condition value A recorded");
                CHECK(rc == 0 && ge.has_exited == 1,
                      "B sees A's has_exited == 1");
                CHECK(rc == 0 && ge.success == STS_SUCCESS(COND_CHILD) &&
                      ge.severity == STS_SEVERITY(COND_CHILD),
                      "B decodes A's success/severity correctly");
            }
            (void)!write(r2c[1], "g", 1);      /* release the child */
            waitpid(kid, NULL, 0);
            close(r2c[1]); close(c2r[0]);
        }
    }

    /* --- Part 3: CLI context round-trip (self) ------------------------- */
    printf("--- CLI invocation context (round-trip) ---\n");
    {
        const char *cmdline = "RUN FOO/BAR=\"baz\"";
        struct vms_setcli_args sc; memset(&sc, 0, sizeof sc);
        sc.cliflag = 1;
        sc.length = (uint16_t)strlen(cmdline);
        memcpy(sc.command, cmdline, strlen(cmdline));
        CHECK(ioctl(fd, VMS_IOCTL_SETCLI, &sc) == 0 && sc.status == SS_NORMAL,
              "SETCLI records the invoking command line");

        struct vms_getcli_args gc; memset(&gc, 0, sizeof gc);
        int rc = ioctl(fd, VMS_IOCTL_GETCLI, &gc);
        CHECK(rc == 0 && gc.status == SS_NORMAL, "GETCLI succeeds");
        CHECK(rc == 0 && gc.cliflag == 1, "GETCLI reports cliflag == 1");
        CHECK(rc == 0 && gc.length == strlen(cmdline),
              "GETCLI reports the exact command-line length");
        CHECK(rc == 0 && strcmp(gc.command, cmdline) == 0,
              "GETCLI reads back the EXACT invoking command line");

        /* The no-CLI state is honest: cliflag 0, zero-length line. */
        struct vms_setcli_args sc0; memset(&sc0, 0, sizeof sc0);
        sc0.cliflag = 0; sc0.length = 0;
        CHECK(ioctl(fd, VMS_IOCTL_SETCLI, &sc0) == 0 && sc0.status == SS_NORMAL,
              "SETCLI can record the no-CLI state");
        struct vms_getcli_args gc0; memset(&gc0, 0, sizeof gc0);
        rc = ioctl(fd, VMS_IOCTL_GETCLI, &gc0);
        CHECK(rc == 0 && gc0.cliflag == 0 && gc0.length == 0,
              "GETCLI reports the no-CLI state (cliflag 0, length 0)");
    }

    /* --- Part 4: CLI context inherited across REGISTER_CONTINUE --------- */
    printf("--- CLI invocation context (inherited by activated image) ---\n");
    {
        const char *cmdline = "MCR FOO -x 42";
        struct vms_setcli_args sc; memset(&sc, 0, sizeof sc);
        sc.cliflag = 1;
        sc.length = (uint16_t)strlen(cmdline);
        memcpy(sc.command, cmdline, strlen(cmdline));
        int set_ok = (ioctl(fd, VMS_IOCTL_SETCLI, &sc) == 0 &&
                      sc.status == SS_NORMAL);
        CHECK(set_ok, "parent (the CLI) records a command line to be inherited");

        int c2r[2];
        if (pipe(c2r) != 0) {
            CHECK(0, "pipe() for CLI-inheritance test");
        } else {
            pid_t kid = fork();
            if (kid == 0) {
                /* CHILD: the activated image. Continue the parent's identity
                 * and read the command line it inherited. */
                close(c2r[0]);
                int cfd = open("/dev/vms", O_RDWR);
                struct child_cli_report rep; memset(&rep, 0, sizeof rep);
                if (cfd >= 0 && do_register(cfd, 1) != 0) {
                    struct vms_getcli_args gc; memset(&gc, 0, sizeof gc);
                    if (ioctl(cfd, VMS_IOCTL_GETCLI, &gc) == 0 &&
                        gc.status == SS_NORMAL) {
                        rep.ok = 1;
                        rep.cliflag = gc.cliflag;
                        rep.length = gc.length;
                        memcpy(rep.command, gc.command, VMS_CLI_CMDLINE_SIZE);
                    }
                }
                (void)!write(c2r[1], &rep, sizeof rep);
                if (cfd >= 0) close(cfd);
                _exit(0);
            }
            close(c2r[1]);
            struct child_cli_report rep; memset(&rep, 0, sizeof rep);
            ssize_t n = read(c2r[0], &rep, sizeof rep);
            CHECK(n == (ssize_t)sizeof rep && rep.ok,
                  "activated image REGISTER_CONTINUEd and read its CLI context");
            CHECK(rep.ok && rep.cliflag == 1,
                  "activated image inherited cliflag == 1 from the CLI");
            CHECK(rep.ok && rep.length == strlen(cmdline),
                  "activated image inherited the exact command-line length");
            CHECK(rep.ok && strcmp(rep.command, cmdline) == 0,
                  "activated image inherited the EXACT invoking command line");
            waitpid(kid, NULL, 0);
            close(c2r[0]);
        }
    }

    close(fd);
    printf("=== test_kmod_exit: %d passed, %d failed ===\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
