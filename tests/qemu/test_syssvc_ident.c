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
 * with a DIFFERENT mask, so B's report cannot be A's by coincidence. */
#define B_NAME  "OPERATOR"
#define B_GRP   1u
#define B_MEM   6u
#define B_PRIVS (PRV$M_TMPMBX | PRV$M_NETMBX | PRV$M_SYSPRV)

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
    CHECK(strstr(outa, "Privileges:        NETMBX OPER TMPMBX\n") != NULL,
          "A: SHOW PROCESS's privilege summary is EXACTLY the executive's mask");
    CHECK(strstr(outa, "may perform operator functions") != NULL,
          "A: SHOW PROCESS/PRIVILEGES lists a privilege that is in the executive's mask");
    CHECK(strstr(outa, "may set any privilege bit") == NULL,
          "A: the privilege display does NOT carry SETPRV -- the writer held it, "
          "the identity it established did not, and the drop is one-way");
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
    CHECK(strstr(outb, "Privileges:        NETMBX SYSPRV TMPMBX\n") != NULL &&
          strstr(outb, "may perform operator functions") == NULL,
          "B: the privilege display is B's mask, not A's -- two processes running "
          "the same image with the same environment report differently");

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
    CHECK(strstr(outc, "Privileges:        NETMBX TMPMBX\n") != NULL,
          "C: the privilege display is EXACTLY the two privileges the executive "
          "granted an unprivileged process");
    CHECK(strstr(outc, "may perform operator functions") == NULL &&
          strstr(outc, "may set any privilege bit") == NULL &&
          strstr(outc, "may bypass all object access controls") == NULL &&
          strstr(outc, "may access objects via system protection") == NULL,
          "C: VMS_PRIVILEGES=ALL bought the unprivileged process nothing");

    printf("=== test_syssvc_ident: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
