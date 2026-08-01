/*
 * test_syssvc_ident.c - Identity is reported by the COMMAND from the
 *                       EXECUTIVE, not from the process (vms-2b8)
 *
 * WHAT THIS PROVES, AND WHY IT HAS TO RUN A REAL DCL COMMAND.
 *
 * The executive half of vms-2b8 already landed and is proven by
 * tests/qemu/test_kmod_ident.c: vms.ko derives a process's UIC and
 * authorized privilege mask from credentials it cannot be lied to about,
 * and VMS_IOCTL_SETIDENT refuses any caller without SETPRV an identity
 * that is not a weakening of its own. None of that was reaching the user.
 * DCL still read getenv("VMS_USERNAME"), getenv("VMS_UIC_GROUP"),
 * getenv("VMS_UIC_MEMBER") and getenv("VMS_PRIVILEGES"), so SHOW PROCESS
 * and SHOW PROCESS/PRIVILEGES reported what the PROCESS had announced
 * about itself while the executive enforced something else entirely.
 *
 * So the subject here is the user-visible command, run for real:
 * /bin/DCL.EXE is staged into this initramfs by tests/qemu/Dockerfile
 * (absence is a FATAL image-build error, not a skip), and every assertion
 * below is made against the bytes SHOW PROCESS actually printed.
 *
 * THE ENVIRONMENT IS POISONED ON PURPOSE. Every DCL process this test
 * starts is exec'd with
 *     VMS_USERNAME=SYSTEM VMS_PRIVILEGES=ALL
 *     VMS_UIC_GROUP=1 VMS_UIC_MEMBER=4
 * in its environment -- the most privileged identity on the system, in
 * exactly the four variables the deleted code read. If any of those reads
 * comes back, DCL reports SYSTEM with every privilege and this suite goes
 * red. A test that merely checked "SHOW PROCESS prints a plausible user
 * name" would pass against the facade it exists to delete.
 *
 * A-WRITES / B-READS (CLAUDE.md Rule 11). The identity is established by
 * one program (this test's child, calling the executive) and reported by
 * a DIFFERENT program (DCL.EXE, after execve replaced the image). Nothing
 * survives that boundary except the executive's own row: the writer's
 * memory is gone, its PCB is gone, and the only thing the environment
 * carries is a contradicting claim that must LOSE. Two such processes are
 * started with DIFFERENT identities and the SAME poisoned environment and
 * the SAME binary, and must report differently -- which no property of
 * the image or of the environment can explain.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. In the CI negative-control
 * rig (deliberately booted with no executive, never the product -- vms-0ff:
 * PID 1 refuses to boot without one) it asserts the property that survives
 * that ruling: with nothing to read, DCL reports NO identity at all rather
 * than falling back to the environment. It then exits EXIT_SKIP (77),
 * never a fake pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <grp.h>
#include <pwd.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "ssdef.h"
#include "prvdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

#define SS_NORMAL  SS$_NORMAL

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

#define DCL_PATH "/bin/DCL.EXE"

/*
 * The variables the deleted readers took identity from, set to the most
 * privileged account on the system. Handed to every DCL process this test
 * starts. PATH is present only so exec'd utilities behave normally; DCL
 * itself does not consult it for the commands used here.
 *
 * VMS_TERMINAL ADDED (vms-cb5). It belongs to the same family and was the
 * only member never planted here -- vms-fb9 deleted its reader and its
 * writers, and tests/integration/test_terminal_identity.sh keeps them
 * deleted by SOURCE SCAN. A source scan cannot see a reader written in a
 * spelling it has never met (that file's own header says so), so the
 * variable is planted here too and the behavioural checks below cover it
 * the same way they cover the other four: whatever any future reader does
 * with it, no identity OVMX reports may come from it.
 */
static char *const poison_env[] = {
    (char *)"VMS_USERNAME=SYSTEM",
    (char *)"VMS_PRIVILEGES=ALL",
    (char *)"VMS_UIC_GROUP=1",
    (char *)"VMS_UIC_MEMBER=4",
    (char *)"VMS_TERMINAL=_OPA0:",
    (char *)"PATH=/bin",
    NULL
};

/* Identity A: an ordinary account, in a group that is not the system's. */
#define A_NAME  "FIELD"
#define A_GRP   200u
#define A_MEM   10u
#define A_PRIVS (PRV$M_TMPMBX | PRV$M_NETMBX | PRV$M_OPER)

/* Identity B: a different account, deliberately in a DIFFERENT group and
 * with a DIFFERENT mask, so B's report cannot be A's by coincidence.
 * PRV$M_WORLD added (vms-2b8, operator ruling 2026-07-31): B is also the
 * positive half of the ENFORCED-privileges display test -- WORLD is one of
 * the four bits SHOW PROCESS/PRIVILEGES is now allowed to show
 * (VMS_PRV_M_ENFORCED, src/kernel/vms_ioctl.h), so B's own mask has to
 * hold one to prove the filter shows what it enforces, not merely hides
 * everything. SYSPRV stays in the mask specifically so its continued
 * ABSENCE from the display is the ruling's own worked example, not an
 * assumption. */
#define B_NAME  "OPERATOR"
#define B_GRP   1u
#define B_MEM   6u
#define B_PRIVS (PRV$M_TMPMBX | PRV$M_NETMBX | PRV$M_SYSPRV | PRV$M_WORLD)

/* Identity C: no identity at all. A real credential change, not a flag --
 * setgid() before setuid(), because setuid() away from root is what drops
 * the capabilities SETPRV is derived from. */
#define C_GID   300u
#define C_UID   1001u

/*
 * run_dcl - establish an identity, exec the real DCL, capture its output.
 *
 * username == NULL means "make no attempt to establish an identity".
 * drop != 0 means "become an unprivileged user first", which is what makes
 * the executive's refusal below a real refusal rather than a policy choice.
 *
 * The child prints "SETIDENT_STATUS=<n>" on the captured stream BEFORE
 * exec'ing, so the writer's own verdict is part of the evidence rather
 * than something this file asserts about from the outside.
 */
static int run_dcl(const char *username, uint32_t uic, uint64_t privs,
                   int drop, const char *script, char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2];

    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        if (drop) {
            if (setgid((gid_t)C_GID) != 0) { printf("SETGID_FAILED\n"); fflush(stdout); _exit(126); }
            if (setuid((uid_t)C_UID) != 0) { printf("SETUID_FAILED\n"); fflush(stdout); _exit(126); }
        }

        if (username) {
            uint32_t st = vms_kif_setident(username, uic, privs);
            printf("SETIDENT_STATUS=%u\n", (unsigned)st);
        } else {
            printf("SETIDENT_STATUS=none\n");
        }
        fflush(stdout);

        execle(DCL_PATH, "DCL.EXE", (char *)NULL, poison_env);
        printf("EXEC_FAILED=%d\n", errno);
        fflush(stdout);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    ssize_t w = write(in_pipe[1], script, strlen(script));
    (void)w;
    close(in_pipe[1]);

    size_t used = 0;
    for (;;) {
        ssize_t n = read(out_pipe[0], out + used, outsz - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(out_pipe[0]);

    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

static void dump(const char *label, const char *text)
{
    printf("  ---- %s ----\n%s  ---- end %s ----\n", label, text, label);
}

/*
 * run_session_fork - reproduce a real session's SUBPROCESS, exactly.
 *
 * THE DEFECT THIS EXISTS FOR (vms-2b8 round 6, found by the veracity
 * adversary and reproduced here before it was fixed). Scenario A above
 * proves the executive refuses to widen an identity, but the refusal is
 * per thread group: vms_proc_register() derives a NEW task's authorized
 * mask from capable(CAP_SYS_ADMIN) and inherits nothing from its
 * parent's row. So while LOGINOUT left the session running as Linux
 * root, a FIELD/[200,10] session could fork a child, the child
 * registered holding CMKRNL|CMEXEC|SETPRV|WORLD before executing an
 * instruction, and SETPRV let it stamp itself SYSTEM [1,4] with SYSUAF's
 * privilege ALL. Measured, not argued: the child's DCL printed
 * "User: SYSTEM ... [001,004] ... SETPRV ... WORLD".
 *
 * So the shape below is LOGINOUT's, step for step, and it must be:
 *   1. establish the authenticated identity through the executive while
 *      still privileged enough to be allowed to (VMS_IOCTL_SETIDENT
 *      requires SETPRV),
 *   2. DROP the Linux credentials to the authenticated UIC
 *      (tools/vms_login.c, same three calls in the same order),
 *   3. fork the subprocess the user would get from any spawn.
 *
 * If step 2 is ever removed from LOGINOUT, this test goes red, because
 * step 3's child gets SETPRV back and its claim succeeds. That is the
 * point: the executive's refusal and the credential drop are only
 * load-bearing together.
 *
 * The child prints its own verdict on the captured stream and then
 * execs the real DCL with the poisoned environment, so what is asserted
 * is what a user would actually see.
 */
static int run_session_fork(char *out, size_t outsz)
{
    int out_pipe[2], in_pipe[2];

    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    pid_t sess = fork();
    if (sess < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }

    if (sess == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        /* 1. LOGINOUT establishes the authenticated identity. */
        uint32_t st = vms_kif_setident(A_NAME, (A_GRP << 16) | A_MEM, A_PRIVS);
        printf("SESSION_SETIDENT=%u\n", (unsigned)st);

        /* 2. LOGINOUT becomes the user. Same calls, same order, same
         *    fatality as tools/vms_login.c. */
        if (setgroups(0, NULL) != 0 || setgid((gid_t)A_GRP) != 0 ||
            setuid((uid_t)A_MEM) != 0) {
            printf("SESSION_DROP_FAILED=%d\n", errno);
            fflush(stdout);
            _exit(126);
        }
        printf("SESSION_UID=%u SESSION_GID=%u\n",
               (unsigned)getuid(), (unsigned)getgid());
        fflush(stdout);

        /* 3. The session spawns a subprocess, which claims everything. */
        pid_t sub = fork();
        if (sub == 0) {
            uint32_t vpid = 0;
            uint32_t rst = vms_kif_register(&vpid);
            printf("SUB_REGISTER=%u\n", (unsigned)rst);
            uint32_t cst = vms_kif_setident("SYSTEM", (1u << 16) | 4u, ~0ULL);
            printf("SUB_SETIDENT=%u\n", (unsigned)cst);
            fflush(stdout);
            execle(DCL_PATH, "DCL.EXE", (char *)NULL, poison_env);
            printf("SUB_EXEC_FAILED=%d\n", errno);
            fflush(stdout);
            _exit(127);
        }
        int sst;
        while (sub > 0 && waitpid(sub, &sst, 0) < 0 && errno == EINTR)
            ;
        fflush(stdout);
        _exit(0);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    ssize_t w = write(in_pipe[1], "SHOW PROCESS\nSHOW PROCESS/PRIVILEGES\n",
                      strlen("SHOW PROCESS\nSHOW PROCESS/PRIVILEGES\n"));
    (void)w;
    close(in_pipe[1]);

    size_t used = 0;
    for (;;) {
        ssize_t n = read(out_pipe[0], out + used, outsz - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(out_pipe[0]);

    int st;
    while (waitpid(sess, &st, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

/*
 * A-WRITES / B-READS, IN ITS TRUE FORM (CLAUDE.md Rule 11, vms-2b8
 * round 6). Scenarios A-D above cross execve, which proves the row is
 * not in the image and not in the environment -- but it is still ONE
 * task's row, read by whatever that task became. It therefore cannot
 * detect a per-task derivation error, which is exactly the defect class
 * scenario D exists for.
 *
 * So here process B reads process A's row WHILE A IS STILL ALIVE and
 * blocked. Neither process ever runs the other's code and neither has
 * any channel to the other except the executive's table: A's identity
 * reaches B only because vms.ko holds it. If the process table were
 * per-process memory -- the facade shape Rule 11 names -- B would find
 * nothing, and a facade that fabricated a plausible answer would get
 * A's user name, UIC and privilege mask all wrong at once.
 *
 * Sequenced by blocking pipe reads in both directions, never by sleeps
 * (a fixed sleep against an emulated guest is a flaky test): the parent
 * does not release A until B has reported, so "A was alive" is a
 * property of the ordering rather than of timing.
 *
 * A and B are in the SAME UIC group on purpose. Per the oracle
 * (docs/oracle/vax73-privileges.md §5.2) a same-group $GETJPI needs no
 * privilege at all, so B reads A holding only TMPMBX|NETMBX -- the read
 * cannot be explained by B being privileged.
 */
#define E_A_NAME  "PAYROLL"
#define E_A_GRP   7u
#define E_A_MEM   3u
#define E_A_PRIVS (PRV$M_TMPMBX | PRV$M_NETMBX | PRV$M_SYSPRV)

#define E_B_NAME  "AUDITOR"
#define E_B_GRP   7u
#define E_B_MEM   9u
#define E_B_PRIVS (PRV$M_TMPMBX | PRV$M_NETMBX)

static void scenario_e_a_writes_b_reads(void)
{
    int a_out[2], a_hold[2];
    pid_t a_pid, b_pid;
    uint32_t a_vms_pid = 0;

    printf("  ---- E: process B reads process A's live row ----\n");

    if (pipe(a_out) < 0 || pipe(a_hold) < 0) {
        CHECK(0, "E: could not create the pipes that sequence A and B");
        return;
    }

    a_pid = fork();
    if (a_pid < 0) {
        CHECK(0, "E: could not fork process A");
        return;
    }
    if (a_pid == 0) {
        uint32_t vpid = 0;
        char go;
        close(a_out[0]); close(a_hold[1]);
        if (vms_kif_register(&vpid) != SS_NORMAL) _exit(2);
        if (!(vms_kif_setident(E_A_NAME, (E_A_GRP << 16) | E_A_MEM,
                               E_A_PRIVS) & 1)) _exit(3);
        /* Publish nothing but the ID. The name, UIC and mask B checks
         * are never sent over this pipe -- B has to get them from the
         * executive or not at all. */
        if (write(a_out[1], &vpid, sizeof(vpid)) != (ssize_t)sizeof(vpid))
            _exit(4);
        close(a_out[1]);
        /* Stay alive until the parent says B is done. */
        if (read(a_hold[0], &go, 1) != 1) _exit(5);
        _exit(0);
    }
    close(a_out[1]); close(a_hold[0]);

    if (read(a_out[0], &a_vms_pid, sizeof(a_vms_pid)) != (ssize_t)sizeof(a_vms_pid)) {
        CHECK(0, "E: process A never published its executive-assigned VMS PID");
        close(a_hold[1]);
        waitpid(a_pid, NULL, 0);
        return;
    }
    close(a_out[0]);

    int b_out[2];
    if (pipe(b_out) < 0) {
        CHECK(0, "E: could not create process B's pipe");
        close(a_hold[1]);
        waitpid(a_pid, NULL, 0);
        return;
    }

    b_pid = fork();
    if (b_pid < 0) {
        CHECK(0, "E: could not fork process B");
        close(a_hold[1]);
        waitpid(a_pid, NULL, 0);
        return;
    }
    if (b_pid == 0) {
        struct vms_procinfo self, seen;
        uint32_t vpid = 0;
        close(b_out[0]);
        dup2(b_out[1], STDOUT_FILENO);
        close(b_out[1]);
        if (vms_kif_register(&vpid) != SS_NORMAL) { printf("B_REGISTER_FAILED\n"); fflush(stdout); _exit(2); }
        /* B takes a DIFFERENT identity, and one WITHOUT SETPRV or
         * WORLD, so nothing below can be B looking at itself and
         * nothing below is explained by B being privileged. */
        if (!(vms_kif_setident(E_B_NAME, (E_B_GRP << 16) | E_B_MEM,
                               E_B_PRIVS) & 1)) { printf("B_SETIDENT_FAILED\n"); fflush(stdout); _exit(3); }

        memset(&self, 0, sizeof(self));
        if (vms_kif_getjpi_self(&self) & 1)
            printf("B_SELF=%s/%08X/%016llX\n", self.username,
                   self.uic, (unsigned long long)self.cur_privs);

        memset(&seen, 0, sizeof(seen));
        uint32_t st = vms_kif_getjpi_pid(a_vms_pid, &seen);
        printf("B_READ_STATUS=%u\n", (unsigned)st);
        if (st & 1)
            printf("B_READ=%s/%08X/%016llX\n", seen.username, seen.uic,
                   (unsigned long long)seen.perm_privs);
        fflush(stdout);
        _exit(0);
    }
    close(b_out[1]);

    static char bout[4096];
    size_t used = 0;
    for (;;) {
        ssize_t n = read(b_out[0], bout + used, sizeof(bout) - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= sizeof(bout) - 1) break;
    }
    bout[used] = '\0';
    close(b_out[0]);
    waitpid(b_pid, NULL, 0);

    /* A has still not been released -- it is blocked in read(). */
    printf("%s", bout);

    {
        char want_self[128], want_read[128];
        snprintf(want_self, sizeof(want_self), "B_SELF=%s/%08X/%016llX\n",
                 E_B_NAME, (E_B_GRP << 16) | E_B_MEM,
                 (unsigned long long)(uint64_t)E_B_PRIVS);
        snprintf(want_read, sizeof(want_read), "B_READ=%s/%08X/%016llX\n",
                 E_A_NAME, (E_A_GRP << 16) | E_A_MEM,
                 (unsigned long long)(uint64_t)E_A_PRIVS);

        CHECK(strstr(bout, want_self) != NULL,
              "E: B holds its OWN identity (so what it reads below is not itself)");
        CHECK(strstr(bout, "B_READ_STATUS=1\n") != NULL,
              "E: B's read of A's row succeeded with no privilege -- same UIC "
              "group, as the oracle requires");
        CHECK(strstr(bout, want_read) != NULL,
              "E: B read A's user name, UIC and authorized mask EXACTLY, from a "
              "LIVE process it shares nothing with but the executive's table");
        CHECK(strstr(bout, E_B_NAME "/00070003") == NULL,
              "E: B did not simply report its own identity under A's PID");
    }

    /* Release A only now, so "A was alive during the read" is ordering,
     * not timing. */
    {
        ssize_t w = write(a_hold[1], "x", 1);
        (void)w;
    }
    close(a_hold[1]);
    {
        int ast = 0;
        while (waitpid(a_pid, &ast, 0) < 0 && errno == EINTR)
            ;
        CHECK(WIFEXITED(ast) && WEXITSTATUS(ast) == 0,
              "E: process A exited normally, i.e. it was still blocked and "
              "alive while B read its row");
    }
}

/* ====================================================================
 * G. THE COMMANDS THAT NAME THE USER, ASKED BY A SUBPROCESS THE
 *    EXECUTIVE HAS NOT NAMED (vms-f39, vms-f42d, CLAUDE.md Rule 10).
 *
 * WHAT THIS EXISTS FOR. Scenario C proved ONE reader of the user name
 * (F$GETJPI) stopped fabricating. It said nothing about the others, and
 * there were five more, all the same one-liner:
 *
 *     const char *user = ctx->username[0] ? ctx->username : "SYSTEM";
 *
 * in cmd_submit / cmd_print / cmd_logout (src/vmsdcl/dcl_cmd_process.c)
 * and cmd_reply / cmd_accounting (src/vmsdcl/dcl_cmd_misc.c), plus
 * F$USER()'s getpwuid(getuid()) branch in dcl_lexical.c which answered
 * with the HOST Linux login name, upcased. So a process the executive
 * held no name for submitted print and batch jobs owned by SYSTEM, wrote
 * SYSTEM into OPERATOR.LOG through REPLY, was logged out as SYSTEM, and
 * reported SYSTEM's login history.
 *
 * THE STATE IS REACHABLE WITHOUT PRIVILEGE, and that is why this is a
 * defect rather than a curiosity: vms_proc_register() (src/kernel/
 * vms_module.c) zeroes the username of every newly registered task and
 * inherits nothing from the parent's row (src/kernel/vms_proctab.c), so
 * any SPAWNed subprocess of an ordinary login is in it. The repo already
 * asserts that blank out loud at tests/uat/vms_session_qemu.sh
 * ('User: +Process ID:'), where it is filed against $CREPRC identity
 * propagation (vms-afd).
 *
 * CROSS-PROCESS BY CONSTRUCTION. The shape is LOGINOUT's, as scenario D:
 * a session establishes an authenticated identity through the executive,
 * DROPS its Linux credentials to that UIC, and only then forks the
 * subprocess. The subprocess registers on its own -- so its row is the
 * unnamed one -- execs the shipped DCL.EXE with the poisoned environment,
 * and every assertion below is made by THIS process, which is neither the
 * session nor the subprocess, over bytes the subprocess printed.
 *
 * WHAT THE ASSERTIONS ARE MADE ON. Each site gets a POSITIVE check that
 * the command ran and rendered the name field EMPTY, in that command's own
 * printf format, plus a NEGATIVE check naming the literal the fallback
 * used to produce. "SYSTEM is absent" alone would be satisfied by the
 * command failing outright, and by the getpwuid() fabrication that the
 * same fallback produced on any system that has an /etc/passwd.
 * ==================================================================== */
#define G_NAME  "SHIPPING"
#define G_GRP   210u
#define G_MEM   11u
#define G_PRIVS (PRV$M_TMPMBX | PRV$M_NETMBX)

/* The queue manager's database. ensure_queue_init() defaults it to
 * /tmp/QMAN_MASTER.DAT, which the initramfs's root-owned /tmp does not let
 * an unprivileged subprocess create -- and a SUBMIT/PRINT that fails
 * QMANERR would satisfy every "SYSTEM is absent" check while proving
 * nothing. So the directory is made world-writable below and named here.
 * This is the ONLY variable added to the poisoned set; the five identity
 * variables are planted exactly as everywhere else in this file. */
#define G_QDIR  "/tmp/ovmx_cb5_q"

static char *const g_env[] = {
    (char *)"VMS_USERNAME=SYSTEM",
    (char *)"VMS_PRIVILEGES=ALL",
    (char *)"VMS_UIC_GROUP=1",
    (char *)"VMS_UIC_MEMBER=4",
    (char *)"VMS_TERMINAL=_OPA0:",
    (char *)"PATH=/bin",
    (char *)"VMSQ_DB_PATH=" G_QDIR "/QMAN.DAT",
    NULL
};

/* The files SUBMIT and PRINT queue, named as VMS filespecs:
 * dcl_resolve_path() maps DKA0: to SYSDISK_MOUNT, so DKA0:[OVMXCB5] is
 * /vms/OVMXCB5 in the guest. A Linux path was tried first and did not
 * survive DCL's parser, which reads '/' as the start of a qualifier --
 * measured on the host build, 'PRINT ./x.txt' answers
 * '%RMS-E-FNF, file not found - .'. */
#define G_VMSDIR "DKA0:[OVMXCB5]"
#define G_LNXDIR "/vms/OVMXCB5"
/*
 * Where vmsfs_to_linux_path() lands SYS$SYSTEM:SYSUAF.DAT and
 * SYS$MANAGER:OPERATOR.LOG under SYSDISK_MOUNT=/vms (ovmx_layout.h).
 *
 * DERIVED, NOT GUESSED: these two strings are what vmsfs_to_linux_path()
 * returns for those filespecs once lnm_setup_defaults() has run, printed
 * by a probe rather than read off the directory layout. Note the LOWERCASE
 * basenames -- that is the translation's own output. (Both cases happen to
 * open on the host, so a guess of "SYSUAF.DAT" would have worked here and
 * would have been a guess. Use what the function returns.)
 */
#define G_SYSUAF "/vms/SYS0/SYSCOMMON/SYSEXE/sysuaf.dat"
#define G_OPLOG  "/vms/SYS0/SYSCOMMON/SYSMGR/operator.log"

/*
 * THE /etc/passwd THIS SCENARIO STAGES, and why it is not decoration.
 *
 * F$USER()'s deleted fallback had TWO branches -- getpwuid(getuid()) first,
 * the literal "SYSTEM" only if that returned NULL. This initramfs ships no
 * /etc/passwd, so getpwuid() finds nothing here and the getpwuid branch is
 * not taken: an assertion run without this file could not tell the two
 * branches apart. MEASURED, not assumed -- with only the getpwuid half
 * restored (tests/qemu/facility_defects.sh dcl-fuser-host-login-name),
 * scenario C, which runs before this file is staged, stays GREEN, and the
 * whole red set is this scenario's.
 *
 * So the file is staged for the length of this scenario and removed after,
 * giving uid G_MEM a Linux account name -- the state a system with a passwd
 * database is in, and the state under which the defect was originally
 * measured (F$USER() answering "BARON", the host login name upcased). The
 * subprocess prints getpwuid(getuid())->pw_name on the captured stream, so
 * "the Linux name is absent from the output" is backed by evidence that the
 * Linux name EXISTED and was resolvable at the moment the question was
 * asked.
 */
#define G_PWNAME "shipuser"

/*
 * THE SYSUAF THIS SCENARIO STAGES, and why the F$IDENTIFIER checks below
 * would be vacuous without it.
 *
 * F$IDENTIFIER used to answer out of the HOST's passwd database:
 * getpwnam() for NAME_TO_NUMBER, getpwuid() for NUMBER_TO_NAME. It now
 * answers out of OVMX's authorization file. This initramfs ships no
 * SYSUAF, so with no file staged EVERY lookup misses and every answer is
 * the null string or 0 -- which is also what a correct implementation
 * returns for an unknown identifier, so "the Linux name is absent" would
 * be satisfied by the file simply not being there. Staging a SYSUAF is
 * what makes the two distinguishable: the POSITIVE checks require a real
 * account to resolve, in the same run in which the NEGATIVE checks
 * require the Linux account -- which /etc/passwd above makes resolvable
 * at the same moment -- not to.
 *
 * THE VALUES ARE THE ORACLE'S. SYSTEM = [1,4] = 65540 was measured on the
 * reference lab (real OpenVMS VAX V7.3, ~/vax/cluster VAX1) on
 * 2026-08-01: F$IDENTIFIER("SYSTEM","NAME_TO_NUMBER") -> 65540 and
 * F$IDENTIFIER(65540,"NUMBER_TO_NAME") -> "SYSTEM". OVMX's shipped
 * SYSUAF.DAT already carries SYSTEM|...|1|4, so this row is that file's
 * row, not a value chosen here.
 *
 * The second row is G_NAME at G_GRP/G_MEM -- the same identity the
 * session stamps into the executive -- so the NUMBER_TO_NAME direction
 * has an account to find that is NOT SYSTEM. A single SYSTEM hit could
 * be explained by SYSTEM having been special-cased, which is exactly
 * what the deleted code did.
 */

static int g_stage_files(void)
{
    FILE *fp;

    mkdir("/etc", 0777);
    fp = fopen("/etc/passwd", "w");
    if (!fp) return -1;
    fprintf(fp, "%s:x:%u:%u:OVMX cb5 probe:/tmp:/bin/sh\n",
            G_PWNAME, (unsigned)G_MEM, (unsigned)G_GRP);
    fclose(fp);
    chmod("/etc/passwd", 0644);

    mkdir("/vms", 0777);
    mkdir("/vms/SYS0", 0777);
    mkdir("/vms/SYS0/SYSCOMMON", 0777);
    mkdir("/vms/SYS0/SYSCOMMON/SYSEXE", 0777);
    mkdir("/vms/SYS0/SYSCOMMON/SYSMGR", 0777);
    chmod("/vms/SYS0", 0777);
    chmod("/vms/SYS0/SYSCOMMON", 0777);
    chmod("/vms/SYS0/SYSCOMMON/SYSEXE", 0777);
    chmod("/vms/SYS0/SYSCOMMON/SYSMGR", 0777);
    fp = fopen(G_SYSUAF, "w");
    if (!fp) return -1;
    fprintf(fp, "SYSTEM|x|1|4|SYS$SYSDEVICE:[SYSMGR]||ALL\n");
    fprintf(fp, "%s|x|%u|%u|SYS$SYSDEVICE:[USERS.%s]||TMPMBX\n",
            G_NAME, (unsigned)G_GRP, (unsigned)G_MEM, G_NAME);
    fclose(fp);
    chmod(G_SYSUAF, 0644);

    /* The operator log the OPCOM assertions read back. Truncated here so
     * what the run finds in it is this run's, not an earlier suite's. */
    fp = fopen(G_OPLOG, "w");
    if (fp) fclose(fp);
    chmod(G_OPLOG, 0666);

    mkdir("/vms", 0777);
    mkdir(G_LNXDIR, 0777);
    chmod("/vms", 0777);
    chmod(G_LNXDIR, 0777);
    mkdir(G_QDIR, 0777);
    chmod(G_QDIR, 0777);

    fp = fopen(G_LNXDIR "/JOB.TXT", "w");
    if (!fp) return -1;
    fputs("ovmx cb5 print job\n", fp);
    fclose(fp);
    chmod(G_LNXDIR "/JOB.TXT", 0666);

    fp = fopen(G_LNXDIR "/JOB.COM", "w");
    if (!fp) return -1;
    fputs("$ EXIT\n", fp);
    fclose(fp);
    chmod(G_LNXDIR "/JOB.COM", 0666);

    return 0;
}

static int run_g_subprocess(const char *script, char *out, size_t outsz)
{
    int out_pipe[2], in_pipe[2];

    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    pid_t sess = fork();
    if (sess < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }

    if (sess == 0) {
        struct vms_procinfo self;
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        /* 1. LOGINOUT stamps the authenticated identity. */
        printf("G_SESSION_SETIDENT=%u\n",
               (unsigned)vms_kif_setident(G_NAME, (G_GRP << 16) | G_MEM,
                                          G_PRIVS));
        /* Read it back out of the executive, so the run itself shows the
         * table CAN hold a name -- otherwise the subprocess's blank could
         * be explained by the executive naming nobody in this boot. */
        memset(&self, 0, sizeof(self));
        if (vms_kif_getjpi_self(&self) & 1)
            printf("G_SESSION_SELF=%s\n", self.username);

        /* 2. LOGINOUT becomes the user. */
        if (setgroups(0, NULL) != 0 || setgid((gid_t)G_GRP) != 0 ||
            setuid((uid_t)G_MEM) != 0) {
            printf("G_SESSION_DROP_FAILED=%d\n", errno);
            fflush(stdout);
            _exit(126);
        }
        printf("G_SESSION_UID=%u\n", (unsigned)getuid());
        fflush(stdout);

        /* 3. The session SPAWNs. The child registers itself, which is
         *    where the unnamed row comes from, and claims nothing. */
        pid_t sub = fork();
        if (sub == 0) {
            uint32_t vpid = 0;
            struct passwd *pw;
            printf("G_SUB_REGISTER=%u\n",
                   (unsigned)vms_kif_register(&vpid));
            /* The Linux account name the deleted getpwuid() branch would
             * have answered with, resolved in THIS process at THIS moment.
             * Without it, "the Linux name is absent" is a claim about a
             * name that may never have existed. */
            pw = getpwuid(getuid());
            printf("G_SUB_PWNAM=%s\n", pw && pw->pw_name ? pw->pw_name : "(none)");
            fflush(stdout);
            execle(DCL_PATH, "DCL.EXE", (char *)NULL, g_env);
            printf("G_SUB_EXEC_FAILED=%d\n", errno);
            fflush(stdout);
            _exit(127);
        }
        int sst;
        while (sub > 0 && waitpid(sub, &sst, 0) < 0 && errno == EINTR)
            ;
        fflush(stdout);
        _exit(0);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    {
        ssize_t w = write(in_pipe[1], script, strlen(script));
        (void)w;
    }
    close(in_pipe[1]);

    size_t used = 0;
    for (;;) {
        ssize_t n = read(out_pipe[0], out + used, outsz - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(out_pipe[0]);

    int st;
    while (waitpid(sess, &st, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

static void scenario_g_unnamed_row_reports_nothing(void)
{
    static char outg[65536];
    char want[160], nope[160];

    printf("  ---- G: the name-printing commands, in an unnamed subprocess ----\n");

    if (g_stage_files() != 0) {
        CHECK(0, "G: could not stage the files SUBMIT and PRINT queue");
        return;
    }

    /*
     * The UIC values in the F$IDENTIFIER lines are COMPUTED from G_GRP /
     * G_MEM rather than written out, so they cannot drift away from the
     * identity the session actually stamps and the SYSUAF row actually
     * carries. A hand-written 13762571 here would keep compiling and stop
     * meaning anything the day G_GRP changed.
     */
    static char script_g[1024];
    snprintf(script_g, sizeof(script_g),
        "PRINT " G_VMSDIR "JOB.TXT\n"
        "SUBMIT " G_VMSDIR "JOB.COM\n"
        "SHOW QUEUE/ALL\n"
        "ACCOUNTING\n"
        "REPLY/ENABLE\n"
        "IDENT_U = F$USER()\n"
        "SHOW SYMBOL IDENT_U\n"
        /* F$IDENTIFIER, both directions (vms-f39).
         *
         * POSITIVE half first -- accounts that ARE in the staged SYSUAF
         * must resolve. Without these, every negative below is equally
         * well explained by the lookup being dead, which is the exact
         * vacuity this suite exists to avoid. Two positives, not one,
         * because the deleted code SPECIAL-CASED SYSTEM: a run that only
         * checked SYSTEM would pass on a reimplementation that hardcoded
         * it again. SHIPPING is a second account at a different UIC.
         *
         * NEGATIVE half: the uid and the login name of the Linux account
         * /etc/passwd gives this process -- the defect verbatim.
         */
        "IDENT_N = F$IDENTIFIER(\"SYSTEM\",\"NAME_TO_NUMBER\")\n"
        "SHOW SYMBOL IDENT_N\n"
        "IDENT_R = F$IDENTIFIER(65540,\"NUMBER_TO_NAME\")\n"
        "SHOW SYMBOL IDENT_R\n"
        "IDENT_S = F$IDENTIFIER(%u,\"NUMBER_TO_NAME\")\n"
        "SHOW SYMBOL IDENT_S\n"
        "IDENT_H = F$IDENTIFIER(\"" G_PWNAME "\",\"NAME_TO_NUMBER\")\n"
        "SHOW SYMBOL IDENT_H\n"
        "IDENT_P = F$IDENTIFIER(%u,\"NUMBER_TO_NAME\")\n"
        "SHOW SYMBOL IDENT_P\n"
        "SHOW PROCESS\n"
        "LOGOUT\n",
        (unsigned)((G_GRP << 16) | G_MEM),   /* SHIPPING's UIC */
        (unsigned)G_MEM);                    /* [0,11] -- no account; the
                                              * Linux uid getpwuid() used */

    int rc = run_g_subprocess(script_g, outg, sizeof(outg));
    /* Staged for this scenario only -- every suite after this one runs in
     * the same initramfs and none of them should meet a passwd database
     * this scenario invented. */
    unlink("/etc/passwd");
    if (rc != 0) {
        CHECK(0, "G: could not run the session/subprocess scenario");
        return;
    }
    dump("G: unnamed subprocess", outg);

    /* --- the run is what it claims to be ------------------------------ */
    CHECK(strstr(outg, "G_SESSION_SETIDENT=1\n") != NULL,
          "G: the session established an authenticated identity");
    CHECK(strstr(outg, "G_SESSION_SELF=" G_NAME "\n") != NULL,
          "G: the executive HOLDS that name and reads it back -- so the "
          "subprocess's blank below is not the executive naming nobody");
    {
        char uid_want[64];
        snprintf(uid_want, sizeof(uid_want), "G_SESSION_UID=%u\n",
                 (unsigned)G_MEM);
        CHECK(strstr(outg, uid_want) != NULL,
              "G: the session dropped its Linux credentials, so the "
              "subprocess is genuinely unprivileged");
    }
    CHECK(strstr(outg, "G_SUB_REGISTER=1\n") != NULL,
          "G: the subprocess reached the executive and got a row of its own "
          "(so its blank name is a real blank, not an absent executive)");
    CHECK(strstr(outg, "G_SUB_EXEC_FAILED") == NULL,
          "G: the subprocess actually exec'd the shipped DCL.EXE");
    /* --- the unnamed row itself -------------------------------------- */
    /* cmd_show_process's own field: "User: %-17sProcess ID:". */
    snprintf(want, sizeof(want), "User: %-17sProcess ID:", "");
    CHECK(strstr(outg, want) != NULL,
          "G: SHOW PROCESS in the subprocess reports an EMPTY user name -- "
          "the known state tests/uat/vms_session_qemu.sh already pins, filed "
          "as vms-afd");
    CHECK(strstr(outg, "User: " G_NAME) == NULL,
          "G: the subprocess does NOT report the SESSION's name -- OVMX has "
          "no $CREPRC identity propagation yet (vms-afd); when it lands this "
          "line goes red and whoever lands it deletes it");

    /* --- F$USER (vms-f39) -------------------------------------------- */
    CHECK(strstr(outg, "G_SUB_PWNAM=" G_PWNAME "\n") != NULL,
          "G/F$USER: getpwuid(getuid()) DOES resolve to a Linux account name "
          "in this process -- so the next check is about a name that existed");
    CHECK(strstr(outg, "IDENT_U = \"\"\n") != NULL,
          "G/F$USER: reports NO name for a process the executive has not "
          "named -- not the host Linux login name, not SYSTEM");
    CHECK(strstr(outg, "IDENT_U = \"SYSTEM\"") == NULL,
          "G/F$USER: does not answer with the literal SYSTEM");
    /* Searched over DCL's OUTPUT ONLY -- everything after the harness's own
     * G_SUB_PWNAM line, which necessarily contains the name and would make a
     * whole-buffer search unfalsifiable. */
    {
        const char *dclout = strstr(outg, "G_SUB_PWNAM=");
        if (dclout) dclout = strchr(dclout, '\n');
        if (dclout) dclout++; else dclout = outg;
        CHECK(strstr(dclout, G_PWNAME) == NULL &&
              strstr(dclout, "SHIPUSER") == NULL,
              "G/F$USER: DCL does NOT answer with the Linux account name, "
              "upcased or otherwise -- the vms-f39 defect exactly");
    }

    /* --- PRINT (vms-f42d) -------------------------------------------- */
    CHECK(strstr(outg, "%PRINT-S-QUEUED, job JOB.TXT") != NULL,
          "G/PRINT: the job really was queued (so the owner assertions below "
          "are about a real queue entry)");
    snprintf(want, sizeof(want), " %-20s %-12s %-10s\n", "JOB.TXT", "", "Pending");
    snprintf(nope, sizeof(nope), " %-20s %-12s %-10s\n", "JOB.TXT", "SYSTEM", "Pending");
    CHECK(strstr(outg, want) != NULL,
          "G/PRINT: SHOW QUEUE shows the print job with an EMPTY owner, in "
          "cmd_show_queue's own column format");
    CHECK(strstr(outg, nope) == NULL,
          "G/PRINT: the print job is NOT owned by SYSTEM");

    /* --- SUBMIT (vms-f42d) ------------------------------------------- */
    CHECK(strstr(outg, "%SUBMIT-S-SUBMITTED, job JOB ") != NULL,
          "G/SUBMIT: the batch job really was queued");
    snprintf(want, sizeof(want), " %-20s %-12s %-10s\n", "JOB", "", "Pending");
    snprintf(nope, sizeof(nope), " %-20s %-12s %-10s\n", "JOB", "SYSTEM", "Pending");
    CHECK(strstr(outg, want) != NULL,
          "G/SUBMIT: SHOW QUEUE shows the batch job with an EMPTY owner");
    CHECK(strstr(outg, nope) == NULL,
          "G/SUBMIT: the batch job is NOT owned by SYSTEM");

    /* --- ACCOUNTING (vms-f42d) --------------------------------------- */
    CHECK(strstr(outg, "OVMX Accounting for user \n") != NULL,
          "G/ACCOUNTING: names no account for an unnamed process");
    CHECK(strstr(outg, "OVMX Accounting for user SYSTEM") == NULL,
          "G/ACCOUNTING: does not report SYSTEM's login history to an "
          "unnamed process");

    /* --- REPLY (vms-f42d) -------------------------------------------- */
    CHECK(strstr(outg, "%OPCOM-I-OPRENA, operator  enabled for CENTRAL class "
                       "messages\n") != NULL,
          "G/REPLY: the OPCOM enable message names no operator");
    /* Anchored on what is CAPTURED. cmd_reply also sends an OPC record to
     * OPERATOR.LOG carrying the same name, and an earlier wording of this
     * assertion claimed to be about that record -- it is not, it is about
     * the console message, and a check that names evidence it never looked
     * at is the defect class this suite exists for. */
    CHECK(strstr(outg, "operator SYSTEM enabled") == NULL,
          "G/REPLY: the console message does not name SYSTEM as the "
          "operator");

    /* --- LOGOUT (vms-f42d) ------------------------------------------- */
    CHECK(strstr(outg, "\n        logged out at ") != NULL,
          "G/LOGOUT: the logout line names no user, in cmd_logout's own "
          "\"  %s      logged out at\" format");
    CHECK(strstr(outg, "SYSTEM      logged out at") == NULL,
          "G/LOGOUT: the session is not logged out as SYSTEM");
}

int main(void)
{
    static char outa[65536], outb[65536], outc[65536];
    uint32_t selfpid = 0;

    printf("=== test_syssvc_ident: identity is read from the executive ===\n");

    /*
     * The SUBJECT must exist. A missing DCL.EXE would turn every
     * assertion below into a comparison against an empty string, i.e.
     * into a silent no-op, so it is fatal here as well as in the image
     * build that stages it.
     */
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        printf("=== test_syssvc_ident: 0 passed, 1 failed ===\n");
        return 1;
    }

    int fd = vms_kif_open();
    if (fd < 0) {
        /*
         * NEGATIVE-CONTROL RIG ONLY. What is asserted is not a status
         * value (vms-0ff ruled OVMX has no "executive absent" state to
         * define one for) but the property this item is about: with no
         * executive to read, the command reports NOTHING -- it does not
         * fall back to the identity sitting in its environment.
         */
        printf("  INFO: /dev/vms could not be opened (errno %d)\n", errno);

        if (run_dcl(NULL, 0, 0, 0, "SHOW PROCESS\n", outa, sizeof(outa)) != 0) {
            printf("  FAIL: could not run DCL at all\n");
            printf("=== test_syssvc_ident: %d passed, %d failed ===\n", pass, fail + 1);
            return 1;
        }
        dump("SHOW PROCESS, no executive", outa);
        /*
         * FIXED (vms-2b8 round 12): this was a bare strstr(outa, "SYSTEM"),
         * which is not a check for the planted identity -- it is a check
         * for the SUBSTRING "SYSTEM", and VMS's own message format
         * contains it in the facility name of unrelated status codes.
         * MEASURED against a real negative-control run: with kif_bind()
         * unable to reach the executive, vms_kif_kerr_to_ss() reports the
         * honest SS$_BUGCHECK failure vms_kif.c documents for the case
         * (the executive lost the PCB it is required to hold), and
         * cmd_show_process prints that status verbatim:
         *   %SYSTEM-F-BUGCHECK, internal consistency failure
         * "%SYSTEM-F-BUGCHECK" is VMS's own facility-name prefix, not the
         * planted VMS_USERNAME=SYSTEM claim -- but the old bare substring
         * check could not tell them apart, so a HONEST failure status
         * reddened an assertion meant to catch a FABRICATED success. An
         * assertion satisfiable by something other than the behaviour
         * under test is exactly as broken when it false-fails on honest
         * output as when it false-passes on fabricated output.
         *
         * The real property -- "SHOW PROCESS does not report the user
         * name planted in its environment" -- is that no "User: SYSTEM"
         * line is ever printed. That is what the check now asks, tied to
         * the exact field format cmd_show_process prints ("User: %-17s").
         * The next assertion (no "User:" label at all) already proves the
         * stronger fact that no identity block was printed in this
         * failure path; this one stays as the direct, named check for the
         * specific claim that was planted.
         */
        CHECK(strstr(outa, "User: SYSTEM") == NULL,
              "with no executive, SHOW PROCESS does NOT report the user name "
              "planted in its environment");
        CHECK(strstr(outa, "User:") == NULL,
              "with no executive, SHOW PROCESS reports no identity at all");
        CHECK(strstr(outa, "TMPMBX") == NULL,
              "with no executive, SHOW PROCESS invents no default privilege list");

        printf("=== test_syssvc_ident: %d passed, %d failed (SKIPPED: no /dev/vms -- the executive-backed scenario was not exercised, but the no-fabrication checks above WERE) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    if (vms_kif_register(&selfpid) != SS_NORMAL) {
        printf("  FAIL: VMS_IOCTL_REGISTER rejected\n");
        printf("=== test_syssvc_ident: 0 passed, 1 failed ===\n");
        return 1;
    }

    /*
     * F$GETJPI ADDED (vms-cb5). SHOW PROCESS and SHOW PROCESS/PRIVILEGES are
     * two DISPLAY paths through cmd_show_process; a caller that wants to know
     * who it is programmatically uses $GETJPI, and until this round nothing
     * proved THAT path reads the executive rather than the environment. It is
     * a separate reader of the same fact, so it needs its own evidence: a
     * display that has been fixed says nothing about a lexical function that
     * has not.
     */
    const char *script =
        "SHOW PROCESS\n"
        "SHOW PROCESS/PRIVILEGES\n"
        "IDENT_U = F$GETJPI(\"\",\"USERNAME\")\n"
        "SHOW SYMBOL IDENT_U\n";

    /* ----------------------------------------------------------------
     * A. A privileged writer establishes an ordinary identity, then
     *    execs DCL with the SYSTEM/ALL claim in its environment.
     * ---------------------------------------------------------------- */
    if (run_dcl(A_NAME, (A_GRP << 16) | A_MEM, A_PRIVS, 0,
                script, outa, sizeof(outa)) != 0) {
        printf("  FAIL: could not run DCL for identity A\n");
        printf("=== test_syssvc_ident: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    dump("identity A", outa);

    CHECK(strstr(outa, "SETIDENT_STATUS=1") != NULL,
          "A: the executive accepted the identity a privileged writer established");
    CHECK(strstr(outa, "User: " A_NAME) != NULL,
          "A: SHOW PROCESS reports the user name the EXECUTIVE holds");
    CHECK(strstr(outa, "SYSTEM") == NULL,
          "A: SHOW PROCESS does NOT report the user name planted in VMS_USERNAME");
    /* [200,10] octal, in SHOW PROCESS's own "[%03o,%03o]" format. */
    CHECK(strstr(outa, "[310,012]") != NULL,
          "A: SHOW PROCESS reports the UIC the EXECUTIVE holds");
    CHECK(strstr(outa, "[001,004]") == NULL,
          "A: SHOW PROCESS does NOT report the UIC planted in VMS_UIC_GROUP/MEMBER");
    /*
     * The POSITIVE half of the F$GETJPI check, and it has to be positive.
     * "SYSTEM is absent from the output" is already asserted above and is
     * satisfied by F$GETJPI returning NOTHING -- an unimplemented item, an
     * empty string, an error. That would be a green check over a lexical
     * function that answers no question at all. Requiring the executive's
     * OWN name to be printed is the assertion that cannot be satisfied by
     * silence. SHOW SYMBOL's format is dcl_cmd_show.c's own: `  %s = "%s"`.
     */
    CHECK(strstr(outa, "IDENT_U = \"" A_NAME "\"") != NULL,
          "A: F$GETJPI(\"\",\"USERNAME\") returns the name the EXECUTIVE holds "
          "-- the programmatic path reads the same source the display does");
    /*
     * The privilege assertions match SHOW PROCESS's one-line summary
     * EXACTLY, and the /PRIVILEGES list by each privilege's oracle
     * description rather than its NAME -- "OPER" is a substring of the
     * user name "OPERATOR" used for identity B, and a name-substring
     * check would quietly become unfalsifiable.
     */
    /*
     * PINNED TO THE ORACLE, NOT TO OVMX (vms-2b8 round 6). Round 5
     * asserted the exact bytes of a "Privileges:" summary line printed
     * by plain SHOW PROCESS. That line does not exist on OpenVMS --
     * measured this round, docs/oracle/vax73-privileges.md §6 -- so the
     * assertion was turning an OVMX invention into a contract. The line
     * is deleted from dcl_cmd_show.c and the whole-mask assertion moves
     * to the "Authorized privileges:" grid, whose format IS the oracle's
     * (8 columns, 10-character cells, trailing padding trimmed).
     *
     * NARROWED TO "EMPTY" (vms-2b8, operator ruling 2026-07-31, Rule 10
     * applied to the reporting side a second time). The grid used to show
     * the whole authorized mask (NETMBX/OPER/TMPMBX here); it now shows
     * only the intersection with VMS_PRV_M_ENFORCED
     * (src/kernel/vms_ioctl.h: CMKRNL|CMEXEC|SETPRV|WORLD) -- the
     * privileges some vms.ko code path will actually refuse an operation
     * over. A's mask (TMPMBX|NETMBX|OPER) shares none of those bits, so
     * the grid is correctly EMPTY: nothing in it is enforced, so nothing
     * in it is shown. The positive case -- an enforced privilege DOES
     * appear -- is proved by B below, which adds WORLD to its mask for
     * exactly this reason.
     */
    CHECK(strstr(outa, "\nAuthorized privileges:\n \nProcess privileges:\n") != NULL,
          "A: the authorized-privileges AND process-privileges blocks are both "
          "EMPTY -- none of A's granted mask (TMPMBX|NETMBX|OPER) is in "
          "VMS_PRV_M_ENFORCED");
    CHECK(strstr(outa, "may perform operator functions") == NULL,
          "A: SHOW PROCESS/PRIVILEGES does NOT list OPER, though A's SYSUAF-style "
          "mask holds it -- OPER is stored and reported by the executive but "
          "enforced nowhere in OVMX, so displaying it would be the illegal "
          "third answer (Rule 10)");
    /*
     * CORRECTED IN ROUND 6. This assertion used to end "...and the drop
     * is one-way", which was FALSE as a product property and was
     * disproved by execution: the writer's reduction held for THIS
     * thread group only, and a child it forked re-derived SETPRV from
     * the CAP_SYS_ADMIN it still held and stamped itself SYSTEM. What
     * this check actually proves is the narrow, true thing -- the
     * established identity does not carry a privilege the writer held.
     * The one-way property across fork is proved separately, and only
     * because of the credential drop, in scenario D below.
     */
    CHECK(strstr(outa, "may set any privilege bit") == NULL,
          "A: the privilege display does NOT carry SETPRV -- the writer held it "
          "and the identity it established did not");
    CHECK(strstr(outa, "may change mode to kernel") == NULL &&
          strstr(outa, "may bypass all object access controls") == NULL,
          "A: VMS_PRIVILEGES=ALL did not add a single privilege to the display");

    /* ----------------------------------------------------------------
     * B. A SECOND process, SAME binary, SAME poisoned environment, a
     *    DIFFERENT executive identity. If the answer came from the image
     *    or from the environment, A and B would report the same thing.
     * ---------------------------------------------------------------- */
    if (run_dcl(B_NAME, (B_GRP << 16) | B_MEM, B_PRIVS, 0,
                script, outb, sizeof(outb)) != 0) {
        printf("  FAIL: could not run DCL for identity B\n");
        printf("=== test_syssvc_ident: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    dump("identity B", outb);

    CHECK(strstr(outb, "User: " B_NAME) != NULL,
          "B: SHOW PROCESS reports B's user name");
    CHECK(strstr(outb, A_NAME) == NULL,
          "B: B does not report A's user name");
    CHECK(strstr(outa, B_NAME) == NULL,
          "A: A does not report B's user name");
    CHECK(strstr(outb, "[001,006]") != NULL,
          "B: SHOW PROCESS reports B's UIC");
    /*
     * A SECOND PROCESS, SAME IMAGE, SAME poison_env, DIFFERENT answer. This
     * is the check that makes A's F$GETJPI result mean something: a function
     * that returned a constant, or that read the environment both processes
     * share, could not print two different names here.
     */
    CHECK(strstr(outb, "IDENT_U = \"" B_NAME "\"") != NULL,
          "B: F$GETJPI returns B's name -- two processes with an IDENTICAL "
          "environment get DIFFERENT answers, so the answer is not the "
          "environment");
    /*
     * B is the POSITIVE half of the ENFORCED-privileges test (vms-2b8,
     * operator ruling 2026-07-31): B_PRIVS holds WORLD, one of the four
     * bits VMS_PRV_M_ENFORCED names, alongside TMPMBX/NETMBX/SYSPRV,
     * none of which are enforced. The grid must show WORLD and ONLY
     * WORLD -- proving the filter is a filter (shows what is enforced)
     * and not merely a blanket suppression (hides everything, which A's
     * empty grid alone could not distinguish from this).
     */
    CHECK(strstr(outb, "\nAuthorized privileges:\n WORLD\n \nProcess privileges:\n") != NULL,
          "B: the authorized-privileges grid shows EXACTLY WORLD -- the one "
          "bit of B's mask that is in VMS_PRV_M_ENFORCED -- not the whole "
          "mask and not nothing");
    CHECK(strstr(outb, "may affect other processes in the world") != NULL,
          "B: SHOW PROCESS/PRIVILEGES lists WORLD's description in the "
          "process-privileges block too");
    CHECK(strstr(outb, "may access objects via system protection") == NULL,
          "B: SHOW PROCESS/PRIVILEGES does NOT list SYSPRV, though B's "
          "SYSUAF-style mask holds it -- this is the ruling's own worked "
          "example: SYSPRV is stored and reported but enforced nowhere in "
          "OVMX (the override belongs in vmsfs.ko, tracked separately as "
          "vms-f15/vms-36d), so showing it would be the illegal third "
          "answer (Rule 10)");
    CHECK(strstr(outb, "may perform operator functions") == NULL,
          "B: the privilege display is B's mask, not A's -- two processes running "
          "the same image with the same environment report differently, and "
          "neither shows OPER (A's unenforced privilege)");

    /* ----------------------------------------------------------------
     * C. An UNPRIVILEGED process claims to be SYSTEM with every
     *    privilege, in the only two ways it has: the ioctl, and the
     *    environment. Both must fail.
     * ---------------------------------------------------------------- */
    if (run_dcl("SYSTEM", (1u << 16) | 4u, ~0ULL, 1,
                script, outc, sizeof(outc)) != 0) {
        printf("  FAIL: could not run DCL for the unprivileged claimant\n");
        printf("=== test_syssvc_ident: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    dump("unprivileged claimant", outc);

    {
        char expect[64];
        snprintf(expect, sizeof(expect), "SETIDENT_STATUS=%u", (unsigned)SS$_NOPRIV);
        CHECK(strstr(outc, expect) != NULL,
              "C: the executive refused an unprivileged process's attempt to "
              "become SYSTEM (SS$_NOPRIV)");
    }
    CHECK(strstr(outc, "SYSTEM") == NULL,
          "C: SHOW PROCESS does NOT report SYSTEM for a process that only "
          "claimed it -- through the ioctl AND through VMS_USERNAME");
    /*
     * THE CHECK THAT FOUND THE DEFECT, kept as the direct, named assertion
     * for it (vms-cb5). The blanket "SYSTEM is absent" check above already
     * goes red for this, but it goes red for a dozen unrelated reasons too,
     * and a defect worth finding once is worth naming.
     *
     * MEASURED before the fix in src/vmsdcl/dcl_lexical.c (lex_user): this
     * process -- refused SS$_NOPRIV by the executive two lines above, and so
     * holding NO name in the process table -- answered F$GETJPI USERNAME
     * with "SYSTEM", the most privileged account on the system, out of a
     * hardcoded fallback. SHOW PROCESS, reading the same field of the same
     * row, correctly printed nothing.
     *
     * The assertion is on the EXACT empty rendering, not on "SYSTEM is
     * absent": absence is satisfied by any other invented name, including
     * the getpwuid() one the same fallback would have produced on a system
     * with an /etc/passwd -- which is every system except the initramfs this
     * ran in. An assertion satisfiable by a different fabrication does not
     * catch the fabrication.
     */
    CHECK(strstr(outc, "IDENT_U = \"\"") != NULL,
          "C: F$GETJPI(\"\",\"USERNAME\") reports NO name for a process the "
          "executive refused to name -- it does not fall back to a Linux "
          "account name or to SYSTEM");
    /* [300,1001] octal: the UIC the executive derived from the credentials
     * the process really has, which is the one thing it could not forge. */
    CHECK(strstr(outc, "[454,1751]") != NULL,
          "C: SHOW PROCESS reports the UIC the executive derived from real "
          "credentials");
    /*
     * NARROWED TO "EMPTY" (vms-2b8, operator ruling 2026-07-31): the
     * unprivileged default mask is VMS_DEFAULT_PRIVS = TMPMBX|NETMBX
     * (src/kernel/vms_internal.h), and neither bit is in
     * VMS_PRV_M_ENFORCED, so the grid is correctly empty -- same
     * reasoning as identity A above.
     */
    CHECK(strstr(outc, "\nAuthorized privileges:\n \nProcess privileges:\n") != NULL,
          "C: the privilege display is EMPTY -- the two privileges the "
          "executive granted an unprivileged process (TMPMBX, NETMBX) are "
          "both outside VMS_PRV_M_ENFORCED");
    CHECK(strstr(outc, "may perform operator functions") == NULL &&
          strstr(outc, "may set any privilege bit") == NULL &&
          strstr(outc, "may bypass all object access controls") == NULL &&
          strstr(outc, "may access objects via system protection") == NULL,
          "C: VMS_PRIVILEGES=ALL bought the unprivileged process nothing");

    /* ----------------------------------------------------------------
     * D. A REAL SESSION'S SUBPROCESS. LOGINOUT's exact sequence:
     *    stamp the authenticated identity, drop the Linux credentials
     *    to that UIC, then spawn. The subprocess claims SYSTEM/ALL.
     *
     *    This is the scenario the veracity adversary used to break
     *    round 5: without the credential drop the subprocess got SETPRV
     *    back from CAP_SYS_ADMIN and its claim SUCCEEDED, so an
     *    ordinary session's child became SYSTEM with SYSUAF's privilege ALL.
     * ---------------------------------------------------------------- */
    {
        static char outd[65536];
        if (run_session_fork(outd, sizeof(outd)) != 0) {
            printf("  FAIL: could not run the session/subprocess scenario\n");
            printf("=== test_syssvc_ident: %d passed, %d failed ===\n", pass, fail + 1);
            return 1;
        }
        dump("session subprocess", outd);

        CHECK(strstr(outd, "SESSION_SETIDENT=1") != NULL,
              "D: the session established its authenticated identity");
        {
            char want[64];
            snprintf(want, sizeof(want), "SESSION_UID=%u SESSION_GID=%u",
                     (unsigned)A_MEM, (unsigned)A_GRP);
            CHECK(strstr(outd, want) != NULL,
                  "D: the session then BECAME that UIC at the Linux level, as "
                  "tools/vms_login.c does");
        }
        CHECK(strstr(outd, "SUB_REGISTER=1") != NULL,
              "D: the subprocess reached the executive at all (so its refusal "
              "below is a refusal, not an absent executive)");
        {
            char want[64];
            snprintf(want, sizeof(want), "SUB_SETIDENT=%u", (unsigned)SS$_NOPRIV);
            CHECK(strstr(outd, want) != NULL,
                  "D: the executive REFUSED the subprocess's claim to be SYSTEM "
                  "(SS$_NOPRIV) -- the reduction survives the fork");
        }
        CHECK(strstr(outd, "SUB_SETIDENT=1") == NULL,
              "D: the subprocess's claim did not succeed by any route");
        CHECK(strstr(outd, "User: SYSTEM") == NULL &&
              strstr(outd, "[001,004]") == NULL,
              "D: the subprocess's own DCL does not report SYSTEM or SYSTEM's UIC");
        CHECK(strstr(outd, "[310,012]") != NULL,
              "D: the subprocess's UIC is the one it inherited from the session's "
              "real credentials, [200,10]");
        /* NARROWED TO "EMPTY", same reasoning as identity C above
         * (vms-2b8, operator ruling 2026-07-31): TMPMBX|NETMBX are both
         * outside VMS_PRV_M_ENFORCED. */
        CHECK(strstr(outd, "\nAuthorized privileges:\n \nProcess privileges:\n") != NULL,
              "D: the subprocess holds exactly the unprivileged default mask, "
              "which is empty in the display because neither of its bits "
              "(TMPMBX, NETMBX) is in VMS_PRV_M_ENFORCED");
        CHECK(strstr(outd, "may set any privilege bit") == NULL &&
              strstr(outd, "may change mode to kernel") == NULL &&
              strstr(outd, "may affect other processes in the world") == NULL,
              "D: the subprocess has neither SETPRV, CMKRNL nor WORLD -- the three "
              "the CAP_SYS_ADMIN derivation used to hand it for free");
    }

    /* ----------------------------------------------------------------
     * E. A-writes / B-reads in its true form (Rule 11).
     * ---------------------------------------------------------------- */
    scenario_e_a_writes_b_reads();

    /* ----------------------------------------------------------------
     * F. F$GETJPI CURPRIV renders the executive's ENFORCED privilege
     *    names, not merely completes (vms-2b8 round 6 derivation; round
     *    9 rewrite). dcl_lexical.c's CURPRIV renderer walks
     *    VMS_PRV_M_ENFORCED (src/kernel/vms_ioctl.h) bit by bit and
     *    looks each set bit up in vms_priv_names[] (dcl_cmd_show.c).
     *    Whether every enforced bit has a row is now a COMPILE-TIME
     *    fact, pinned by a _Static_assert in src/libvms/prv_agreement.c
     *    with its own negative control -- rounds 7-8's RUNTIME guard
     *    for that same fact (walk the mask at F$GETJPI time, abort() if
     *    a bit had no row) was deleted round 9: a runtime handler for a
     *    condition already settled at compile time is Rule 10's
     *    forbidden third answer, not its HIDE answer.
     *
     *    THE VACUITY THIS SCENARIO USED TO HAVE, AND WHY IT MATTERED
     *    (round 9, found by the round-8 adversary review): the OLD
     *    version of this scenario only checked that a marker printed
     *    AFTER the F$GETJPI call, never that the call actually rendered
     *    anything. A run where CURPRIV silently rendered "" -- which is
     *    exactly what an unregistered process, or a build where the
     *    identity never took, produces -- satisfied that assertion just
     *    as well as a run where CURPRIV rendered real privilege names:
     *    the marker prints either way. That is not testing the feature.
     *    Fixed: the script now SHOWs the symbol CURPRIV was assigned
     *    to, so its rendered value is in the captured output, and the
     *    assertion requires the literal enforced-privilege string for
     *    SYSTEM/SYSUAF-ALL (cur_privs = ~0ULL, so VMS_PRV_M_ENFORCED's
     *    four bits -- CMKRNL, CMEXEC, SETPRV, WORLD, in that ascending
     *    bit-position order -- are all set), not merely its presence.
     *
     *    PROVEN BY MUTATION (vms-2b8 round 9), real bootable image, real
     *    QEMU: temporarily changing dcl_lexical.c's
     *    `uint64_t enforced = raw & VMS_PRV_M_ENFORCED;` to
     *    `uint64_t enforced = 0;` -- the one-line edit that makes
     *    CURPRIV/AUTHPRIV always render "" regardless of identity, with
     *    no abort and no other observable change -- and rebuilding the
     *    static tree + bootable image reddened test_syssvc_ident alone
     *    (rc=1, "37 passed, 1 failed") with the ONE failure being this
     *    assertion, "F: F\$GETJPI CURPRIV renders...". Every other suite
     *    in the run (test_kmod_*, the rest of test_syssvc_*) stayed
     *    rc=0. Reverted after confirming.
     *
     *    ISOLATED ON PURPOSE: its own run_dcl() call, its own script,
     *    its own buffer -- not appended to script/outa/outb/outc above,
     *    so nothing here can knock on scenarios A-E's assertions.
     * ---------------------------------------------------------------- */
    {
        static char outf[65536];
        const char *script_f =
            "IDENT_CURPRIV = F$GETJPI(\"\",\"CURPRIV\")\n"
            "SHOW SYMBOL IDENT_CURPRIV\n"
            "WRITE SYS$OUTPUT \"CURPRIV_DONE\"\n";
        if (run_dcl("SYSTEM", (1u << 16) | 4u, ~0ULL, 0,
                    script_f, outf, sizeof(outf)) != 0) {
            printf("  FAIL: could not run DCL for scenario F\n");
            printf("=== test_syssvc_ident: %d passed, %d failed ===\n", pass, fail + 1);
            return 1;
        }
        dump("scenario F, CURPRIV content", outf);
        CHECK(strstr(outf, "SETIDENT_STATUS=1") != NULL,
              "F: the executive accepted the SYSTEM/ALL identity this scenario "
              "needs (cur_privs = ~0ULL, so every VMS_PRV_M_ENFORCED bit is set)");
        CHECK(strstr(outf, "IDENT_CURPRIV = \"CMKRNL,CMEXEC,SETPRV,WORLD\"") != NULL,
              "F: F$GETJPI CURPRIV renders SYSTEM/ALL's actual enforced "
              "privilege names (CMKRNL,CMEXEC,SETPRV,WORLD), not merely "
              "completes without rendering anything");
    }

    /* ----------------------------------------------------------------
     * G. Every remaining DCL reader of the user name, asked by a
     *    subprocess the executive has not named (vms-f39, vms-f42d).
     * ---------------------------------------------------------------- */
    scenario_g_unnamed_row_reports_nothing();

    printf("=== test_syssvc_ident: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
