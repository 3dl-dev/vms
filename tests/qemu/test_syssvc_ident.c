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
 * The four variables the deleted readers took identity from, set to the
 * most privileged account on the system. Handed to every DCL process this
 * test starts. PATH is present only so exec'd utilities behave normally;
 * DCL itself does not consult it for the commands used here.
 */
static char *const poison_env[] = {
    (char *)"VMS_USERNAME=SYSTEM",
    (char *)"VMS_PRIVILEGES=ALL",
    (char *)"VMS_UIC_GROUP=1",
    (char *)"VMS_UIC_MEMBER=4",
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
 * instruction, and SETPRV let it stamp itself SYSTEM [1,4] with all 37
 * privileges. Measured, not argued: the child's DCL printed
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
        CHECK(strstr(outa, "SYSTEM") == NULL,
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

    const char *script = "SHOW PROCESS\nSHOW PROCESS/PRIVILEGES\n";

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
     *    ordinary session's child became SYSTEM with all 37 privileges.
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

    printf("=== test_syssvc_ident: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
