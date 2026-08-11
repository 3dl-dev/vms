/*
 * test_syssvc_getjpi_pidarg.c - F$GETJPI honors its pid argument (vms-9e2).
 *
 * WHAT THIS PROVES, AND WHY IT HAS TO RUN A REAL DCL COMMAND.
 *
 * The DCL lexical F$GETJPI(pid, item) used to PARSE its pid argument and
 * DISCARD it: PID came from getpid(), USERNAME from ctx->username, PRCNAM
 * from ctx->process_name -- all facts about the CALLING process. So
 * F$GETJPI("<some other process>","PID") confidently returned the CALLER's
 * PID: a wrong answer about a different process, and the exact facade
 * CLAUDE.md Rule 11 names. SHOW SYSTEM and SHOW PROCESS already read the
 * executive (vms-8019 / vms-70eb), so the tree carried two identity surfaces
 * that disagreed.
 *
 * vms-9e2 routes F$GETJPI through the SAME executive path sys$getjpi uses
 * (vms_kif_getjpi_pid for a hex pid, vms_kif_getjpi_self for the null
 * "current process" form). Proving that requires a SECOND, live process
 * whose identity the DCL caller does NOT share: a per-process memory facade
 * would find nothing across the fork, and a facade that fabricated a
 * plausible answer would return the DCL caller's identity, not the target's.
 *
 * THE SETUP (modelled on test_syssvc_ident.c scenario E):
 *   - Process A registers with the executive, stamps a distinct username,
 *     UIC and process name, publishes ONLY its executive PID over a pipe,
 *     and then blocks -- staying alive for the whole read.
 *   - A DCL process B, in A's SAME UIC group (so the read needs no privilege
 *     at all -- docs/oracle/vax73-privileges.md §5.2) but with a DIFFERENT
 *     username, member and process name, runs F$GETJPI(<A's pid>, ...) and
 *     F$GETJPI("", ...). B never receives A's name/user over the pipe: it
 *     gets them from the executive or not at all.
 *
 * Sequenced by blocking pipe reads, never by sleeps against an emulated
 * guest (a fixed sleep is a flaky test): the parent does not release A until
 * B's DCL has reported, so "A was alive during the read" is a property of the
 * ordering, not of timing.
 *
 * Grounding (Rule 8): F$GETJPI format + item semantics from the public VSI
 * OpenVMS DCL Dictionary (F$GETJPI) and the $GETJPI item codes; the
 * same-group-needs-no-privilege authorization fact from
 * docs/oracle/vax73-privileges.md §5.
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

#define SS_NORMAL  SS$_NORMAL
#define DCL_PATH   "/bin/DCL.EXE"
#define EXIT_SKIP  77

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/*
 * Identity A -- the TARGET. Distinct username, UIC member and process name,
 * so nothing B reports about A can be B looking at itself or a coincidence.
 */
#define A_USER   "FIELDSVC"
#define A_PRCNAM "PAYROLL_A"
#define A_GRP    200u
#define A_MEM    10u
#define A_PRIVS  (PRV$M_TMPMBX | PRV$M_NETMBX)

/*
 * Identity B -- the DCL CALLER. SAME UIC group as A (the read is then free
 * per the oracle) but a DIFFERENT username and member, and NO WORLD, so the
 * read below cannot be explained by B being privileged.
 */
#define B_USER   "AUDIT_B"
#define B_GRP    200u
#define B_MEM    20u
#define B_PRIVS  (PRV$M_TMPMBX | PRV$M_NETMBX)

/* Env handed to DCL. Irrelevant to the executive (it reads none of it); it
 * is present only so the assertions cannot be satisfied by an environment
 * value instead of the executive's answer. */
static char *const poison_env[] = {
    (char *)"VMS_USERNAME=SYSTEM",
    (char *)"VMS_PRIVILEGES=ALL",
    (char *)"VMS_UIC_GROUP=1",
    (char *)"VMS_UIC_MEMBER=4",
    (char *)"PATH=/bin",
    NULL
};

/*
 * run_dcl - establish identity B, exec the real DCL, feed it `script` on
 * stdin, capture stdout+stderr. Returns 0 on a clean run. Modelled on
 * test_syssvc_ident.c's helper of the same name.
 */
static int run_dcl(const char *username, uint32_t uic, uint64_t privs,
                   const char *script, char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2];

    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    fflush(NULL);
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

        uint32_t st = vms_kif_setident(username, uic, privs);
        printf("SETIDENT_STATUS=%u\n", (unsigned)st);
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
    waitpid(pid, NULL, 0);
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    static char out[65536];

    printf("=== test_syssvc_getjpi_pidarg: F$GETJPI honors its pid argument ===\n");

    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        printf("=== test_syssvc_getjpi_pidarg: 0 passed, 1 failed ===\n");
        return 1;
    }

    int fd = vms_kif_open();
    if (fd < 0) {
        /*
         * NEGATIVE CONTROL (no /dev/vms). INV-6: with no executive to read,
         * F$GETJPI must fail honestly and return an EMPTY value -- it must
         * NOT fabricate the caller's PID. Origin/main returns the caller's
         * PID here; the fix returns nothing.
         */
        printf("  INFO: /dev/vms could not be opened (errno %d)\n", errno);
        const char *neg =
            "SELF = F$GETJPI(\"\",\"PID\")\n"
            "OTHER = F$GETJPI(\"7FFFFFFF\",\"PID\")\n"
            "VERDICT = \"FACADE_CALLER_PID\"\n"
            "IF OTHER .EQS. \"\" THEN VERDICT = \"HONEST_DEGRADED\"\n"
            "IF OTHER .NES. \"\" .AND. OTHER .NES. SELF THEN VERDICT = \"OTHER_PROC\"\n"
            "WRITE SYS$OUTPUT \"NEG_VERDICT=''VERDICT'\"\n";
        if (run_dcl(NULL, 0, 0, neg, out, sizeof(out)) != 0) {
            printf("  FAIL: could not run DCL at all\n");
            printf("=== test_syssvc_getjpi_pidarg: %d passed, %d failed ===\n", pass, fail + 1);
            return 1;
        }
        printf("%s", out);
        CHECK(strstr(out, "NEG_VERDICT=FACADE_CALLER_PID") == NULL,
              "no-executive: F$GETJPI(<other pid>,\"PID\") does NOT fabricate the caller's PID");
        printf("=== test_syssvc_getjpi_pidarg: %d passed, %d failed (SKIPPED: no /dev/vms -- the executive-backed scenario was not exercised, but the no-fabrication check above WAS) ===\n",
               pass, fail);
        /* Contract (ci.yml kernel-executive-negative-control): with no
         * executive, a test_syssvc_*.c returns EXIT_SKIP when its
         * no-fabrication checks hold, 1 if any fabricated. */
        return fail > 0 ? 1 : EXIT_SKIP;
    }
    close(fd);

    /* ---- Fork the TARGET, process A, and hold it alive. ---- */
    int a_out[2], a_hold[2];
    uint32_t a_vms_pid = 0;

    if (pipe(a_out) < 0 || pipe(a_hold) < 0) {
        printf("  FAIL: could not create the pipes that sequence A and B\n");
        printf("=== test_syssvc_getjpi_pidarg: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    fflush(NULL);
    pid_t a_pid = fork();
    if (a_pid < 0) {
        printf("  FAIL: could not fork process A\n");
        printf("=== test_syssvc_getjpi_pidarg: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    if (a_pid == 0) {
        uint32_t vpid = 0;
        char go;
        close(a_out[0]); close(a_hold[1]);
        if (vms_kif_register(&vpid) != SS_NORMAL) _exit(2);
        if (!(vms_kif_setident(A_USER, (A_GRP << 16) | A_MEM, A_PRIVS) & 1)) _exit(3);
        if (!(vms_kif_setprn(A_PRCNAM) & 1)) _exit(4);
        /* Publish ONLY the executive PID. B must obtain A's name and user
         * from the executive, never from this pipe. */
        if (write(a_out[1], &vpid, sizeof(vpid)) != (ssize_t)sizeof(vpid)) _exit(5);
        close(a_out[1]);
        if (read(a_hold[0], &go, 1) != 1) _exit(6);
        _exit(0);
    }
    close(a_out[1]); close(a_hold[0]);

    if (read(a_out[0], &a_vms_pid, sizeof(a_vms_pid)) != (ssize_t)sizeof(a_vms_pid)) {
        printf("  FAIL: process A never published its executive-assigned VMS PID\n");
        close(a_hold[1]);
        waitpid(a_pid, NULL, 0);
        printf("=== test_syssvc_getjpi_pidarg: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    close(a_out[0]);

    /* ---- Process B (DCL): read A BY PID, and read self. ---- */
    char apid_hex[16];
    snprintf(apid_hex, sizeof(apid_hex), "%08X", (unsigned)a_vms_pid);

    char script[1024];
    snprintf(script, sizeof(script),
             "APRC = F$GETJPI(\"%s\",\"PRCNAM\")\n"
             "WRITE SYS$OUTPUT \"A_PRCNAM=''APRC'\"\n"
             "AUSR = F$GETJPI(\"%s\",\"USERNAME\")\n"
             "WRITE SYS$OUTPUT \"A_USERNAME=''AUSR'\"\n"
             "APID = F$GETJPI(\"%s\",\"PID\")\n"
             "WRITE SYS$OUTPUT \"A_PID=''APID'\"\n"
             "SPRC = F$GETJPI(\"\",\"PRCNAM\")\n"
             "WRITE SYS$OUTPUT \"SELF_PRCNAM=''SPRC'\"\n"
             "SUSR = F$GETJPI(\"\",\"USERNAME\")\n"
             "WRITE SYS$OUTPUT \"SELF_USERNAME=''SUSR'\"\n",
             apid_hex, apid_hex, apid_hex);

    if (run_dcl(B_USER, (B_GRP << 16) | B_MEM, B_PRIVS, script, out, sizeof(out)) != 0) {
        printf("  FAIL: could not run DCL for process B\n");
        close(a_hold[1]);
        waitpid(a_pid, NULL, 0);
        printf("=== test_syssvc_getjpi_pidarg: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    printf("%s", out);

    {
        char want_prc[128], want_usr[128], want_pid[128], want_self_usr[128];
        snprintf(want_prc, sizeof(want_prc), "A_PRCNAM=%s\n", A_PRCNAM);
        snprintf(want_usr, sizeof(want_usr), "A_USERNAME=%s\n", A_USER);
        snprintf(want_pid, sizeof(want_pid), "A_PID=%s\n", apid_hex);
        snprintf(want_self_usr, sizeof(want_self_usr), "SELF_USERNAME=%s\n", B_USER);

        /* THE FACADE PROOF: A's values, read BY PID from a process B shares
         * nothing with but the executive's table -- not B's own. On
         * origin/main every one of these carries B's identity instead. */
        CHECK(strstr(out, want_prc) != NULL,
              "F$GETJPI(<A pid>,\"PRCNAM\") == A's process name (not the caller's)");
        CHECK(strstr(out, want_usr) != NULL,
              "F$GETJPI(<A pid>,\"USERNAME\") == A's username (not the caller's)");
        CHECK(strstr(out, want_pid) != NULL,
              "F$GETJPI(<A pid>,\"PID\") echoes A's pid (not the caller's)");
        CHECK(strstr(out, "A_USERNAME=" B_USER "\n") == NULL,
              "F$GETJPI(<A pid>,...) did NOT simply report the caller B's own identity");

        /* THE SELF FORM still reads the executive (regression + the null
         * "current process" form): B's own row, not ctx. */
        CHECK(strstr(out, want_self_usr) != NULL,
              "F$GETJPI(\"\",\"USERNAME\") == the caller B's OWN executive username");
    }

    /* Release A only now: "A was alive during the read" is ordering. */
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
              "process A exited normally -- it was still registered and alive while B read its row");
    }

    printf("=== test_syssvc_getjpi_pidarg: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
