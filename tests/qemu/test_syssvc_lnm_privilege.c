/*
 * test_syssvc_lnm_privilege.c - the executive ENFORCES SYSNAM/GRPNAM on
 * LNM$SYSTEM/LNM$GROUP mutation, against a real /dev/vms (vms-5b7).
 *
 * ============================================================
 * WHAT THIS PROVES, AND WHAT IT DOES NOT.
 *
 * vms-d37/vms-aba made LNM$SYSTEM/LNM$GROUP/LNM$JOB executive-resident, but
 * ANY registered process could write LNM$SYSTEM or LNM$GROUP -- the
 * privilege check was explicitly deferred (see the removed comment this
 * item replaces in src/kernel-core/vms_lnm.c) pending an oracle pin for the
 * SYSNAM/GRPNAM bit values. This suite is the regression proof for the
 * check vms-5b7 adds: real, documented VMS behaviour (OpenVMS DCL
 * Dictionary, DEFINE) is that creating or deleting a name in LNM$SYSTEM
 * needs SYSNAM or SYSPRV, and LNM$GROUP needs GRPNAM, GRPPRV or SYSPRV;
 * LNM$JOB (and LNM$PROCESS, which never reaches vms.ko) need neither.
 *
 * Each scenario below is a SEPARATE forked process that establishes its own
 * identity via vms_kif_setident() (auto-binding on its first KIF call, see
 * vms_kif_setident()'s own header) before making exactly one LNM ioctl --
 * privilege lives on the PCB, so the check can only be proven per-process,
 * the same discipline test_syssvc_identcont.c and test_syssvc_lnm_groupjob.c
 * already use for setident-down and multi-identity scenarios.
 *
 * BOTH DIRECTIONS, for BOTH gated tables:
 *   - a process holding the required privilege DEFINEs/DELETEs successfully.
 *   - a process WITHOUT it gets SS$_NOPRIV, and a following TRANSLATE from a
 *     THIRD, uninvolved process proves the table was NOT touched: still
 *     absent (refused DEFINE) or still present with its original value
 *     (refused DELETE) -- not just "the ioctl returned an error code".
 *   - the SYSPRV/GRPPRV alternate-privilege paths this item's check also
 *     accepts (real, documented VMS behaviour, not an OVMX invention) each
 *     get their own positive case, so both `if` branches in
 *     vms_ioctl_lnm_define()/vms_ioctl_lnm_delete() are exercised, not just
 *     the SYSNAM/GRPNAM half.
 *   - LNM$JOB needs no privilege at all: an identity with NOTHING (not even
 *     SETPRV) still succeeds there, proving the switch's default case is a
 *     no-op gate, not an accidental refusal.
 *
 * NO STATUS CONSTANT IS ASSERTED BY VALUE for a success path other than
 * SS$_NOPRIV itself (oracle-pinned, docs/oracle/vax73-privileges.md §1) and
 * SS$_NOLOGNAM ("absent"/"still gone") -- successes use the VMS odd/even
 * convention, matching test_syssvc_lnm_system.c's and
 * test_syssvc_lnm_crossproc.c's own stated convention.
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * anchored by the lnm-privilege-check-bypassed defect, which makes both
 * privilege switches in src/kernel-core/vms_lnm.c unconditionally fall through
 * (as if every caller held the privilege), reddening exactly the four
 * "refused" assertions below and no others -- the arena write/read paths,
 * the SYSPRV/GRPPRV alternates and the JOB no-privilege case are all
 * untouched by that mutation, so they must stay green.
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>
#include <stdint.h>

#include "ssdef.h"
#include "prvdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77
#define TIMEOUT_MS 20000

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* A distinct, deliberately non-default UIC for every identity below, so a
 * scope_key collision with another suite's data (or another scenario in
 * this one) cannot make a translate find the wrong thing. */
#define MK_UIC(grp, mem) (((uint32_t)(grp) << 16) | (uint32_t)(mem))

struct op_result {
    uint32_t status;
};

static int read_bounded(int fd, void *buf, size_t len)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    size_t got = 0;
    while (got < len) {
        int pr = poll(&pfd, 1, TIMEOUT_MS);
        if (pr <= 0) return 0;
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n <= 0) return 0;
        got += (size_t)n;
    }
    return 1;
}

/*
 * do_define / do_delete - fork a process, establish `privs` as its ONLY
 * current+authorized privileges under identity (name, uic), issue exactly
 * one LNM ioctl, and report the resulting status back over a pipe.
 *
 * The forked process never execs -- it is a distinct PCB (its own
 * registration, its own setident) sharing nothing with the parent's
 * identity, which is the property that makes the privilege check's
 * per-process scoping provable at all.
 */
static uint32_t do_define(const char *ident, uint32_t uic, uint64_t privs,
                          uint32_t table, const char *name, const char *value)
{
    int p[2];
    if (pipe(p) < 0) { perror("pipe"); exit(1); }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid == 0) {
        close(p[0]);
        struct op_result r;
        uint32_t sst = vms_kif_setident(ident, uic, privs);
        if (!(sst & 1)) {
            r.status = sst; /* setident itself failed -- surfaced as-is */
        } else {
            const char *values[1] = { value };
            r.status = vms_kif_lnm_define(table, name, values, 1, 0, PSL_C_EXEC);
        }
        {
            ssize_t w = write(p[1], &r, sizeof(r));
            (void)w;
        }
        close(p[1]);
        _exit(0);
    }
    close(p[1]);
    struct op_result r;
    r.status = 0;
    if (!read_bounded(p[0], &r, sizeof(r)))
        r.status = 0xFFFFFFFF; /* sentinel: child never reported */
    close(p[0]);
    waitpid(pid, NULL, 0);
    return r.status;
}

static uint32_t do_delete(const char *ident, uint32_t uic, uint64_t privs,
                          uint32_t table, const char *name)
{
    int p[2];
    if (pipe(p) < 0) { perror("pipe"); exit(1); }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid == 0) {
        close(p[0]);
        struct op_result r;
        uint32_t sst = vms_kif_setident(ident, uic, privs);
        if (!(sst & 1)) {
            r.status = sst;
        } else {
            r.status = vms_kif_lnm_delete(table, name, PSL_C_EXEC);
        }
        {
            ssize_t w = write(p[1], &r, sizeof(r));
            (void)w;
        }
        close(p[1]);
        _exit(0);
    }
    close(p[1]);
    struct op_result r;
    r.status = 0;
    if (!read_bounded(p[0], &r, sizeof(r)))
        r.status = 0xFFFFFFFF;
    close(p[0]);
    waitpid(pid, NULL, 0);
    return r.status;
}

/*
 * Translate from a THIRD, fully-unprivileged process -- a table read never
 * needs privilege (only creation/deletion is gated), so this is safe to run
 * under any identity and lets us observe the table's true state without
 * relying on the acting process's own (possibly-refused) view of it.
 *
 * `reader_uic` matters ONLY for LNM$GROUP: derive_scope_key() (vms_lnm.c)
 * keys a GROUP entry on the CALLER's own UIC group, so a reader outside the
 * writer's group would legitimately see "absent" regardless of privilege --
 * that is test_syssvc_lnm_groupjob.c's property, not this suite's, and
 * conflating the two would misattribute a scope mismatch as a privilege
 * bug. Callers checking LNM$SYSTEM (scope_key always 0, whoever asks) may
 * pass any UIC; callers checking LNM$GROUP must pass a UIC in the SAME
 * group the writer used.
 */
static int do_translate(uint32_t table, const char *name, uint32_t reader_uic,
                        char *out, size_t outsz)
{
    int p[2];
    if (pipe(p) < 0) { perror("pipe"); exit(1); }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid == 0) {
        close(p[0]);
        char buf[256];
        buf[0] = '\0';
        uint16_t vallen = 0;
        uint32_t attrs = 0;
        int found;
        /* SETPRV is harmless here (translation is never privilege-gated);
         * it only exists so this reader can claim any UIC group to check. */
        uint32_t sst = vms_kif_setident("OVMX5B7R", reader_uic, PRV$M_SETPRV);
        if (!(sst & 1)) {
            found = -1;
        } else {
            found = vms_kif_lnm_translate(table, name, 0, buf, sizeof(buf),
                                          &vallen, &attrs, NULL);
        }
        int32_t rc = found;
        {
            ssize_t w1 = write(p[1], &rc, sizeof(rc));
            ssize_t w2 = write(p[1], buf, sizeof(buf));
            (void)w1; (void)w2;
        }
        close(p[1]);
        _exit(0);
    }
    close(p[1]);
    int32_t rc = -2;
    char buf[256];
    buf[0] = '\0';
    if (!read_bounded(p[0], &rc, sizeof(rc)))
        rc = -2;
    (void)read_bounded(p[0], buf, sizeof(buf));
    close(p[0]);
    waitpid(pid, NULL, 0);
    if (out && outsz) {
        strncpy(out, buf, outsz - 1);
        out[outsz - 1] = '\0';
    }
    return (int)rc;
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* ---- LNM$SYSTEM ------------------------------------------------------ */

static void run_system(void)
{
    char val[256];
    uint32_t st;

    /* (a) SYSNAM alone is sufficient to DEFINE. */
    st = do_define("OVMX5B7A", MK_UIC(210, 1), PRV$M_SYSNAM,
                   VMS_LNM_TBL_SYSTEM, "OVMX5B7$SYS_A", "sysnam-value");
    CHECK((st & 1) || st == SS$_SUPERSEDE,
          "SYSTEM: a process holding SYSNAM can DEFINE/SYSTEM");
    CHECK(do_translate(VMS_LNM_TBL_SYSTEM, "OVMX5B7$SYS_A", MK_UIC(210, 99), val, sizeof(val)) == 1 &&
          strcmp(val, "sysnam-value") == 0,
          "SYSTEM: the SYSNAM-created name is visible with its value");

    /* (b) SYSPRV alone (no SYSNAM) is also sufficient -- the documented
     * alternate (OpenVMS DCL Dictionary, DEFINE). */
    st = do_define("OVMX5B7B", MK_UIC(210, 2), PRV$M_SYSPRV,
                   VMS_LNM_TBL_SYSTEM, "OVMX5B7$SYS_B", "sysprv-value");
    CHECK((st & 1) || st == SS$_SUPERSEDE,
          "SYSTEM: a process holding SYSPRV (not SYSNAM) can also DEFINE/SYSTEM");

    /* (c) No SYSNAM, no SYSPRV, nothing: refused, and the name never
     * appears -- the table is unchanged, not just the ioctl reporting an
     * error. */
    st = do_define("OVMX5B7C", MK_UIC(210, 3), 0,
                   VMS_LNM_TBL_SYSTEM, "OVMX5B7$SYS_C", "should-not-land");
    /* negctl: lnm-privilege-check-bypassed */
    CHECK(st == SS$_NOPRIV,
          "SYSTEM: a process holding neither SYSNAM nor SYSPRV is refused SS$_NOPRIV");
    /* negctl-knockon: lnm-privilege-check-bypassed */
    CHECK(do_translate(VMS_LNM_TBL_SYSTEM, "OVMX5B7$SYS_C", MK_UIC(210, 99), val, sizeof(val)) == 0,
          "SYSTEM: the refused DEFINE never created the name (table unchanged)");

    /* (d) DELETE gated the same way: an unprivileged process cannot remove
     * the name (a) created; a THIRD process's translate proves it is still
     * there, with its ORIGINAL value, after the refused delete. */
    st = do_delete("OVMX5B7D", MK_UIC(210, 4), 0,
                   VMS_LNM_TBL_SYSTEM, "OVMX5B7$SYS_A");
    /* negctl: lnm-privilege-check-bypassed */
    CHECK(st == SS$_NOPRIV,
          "SYSTEM: a process holding neither SYSNAM nor SYSPRV is refused DEASSIGN/SYSTEM SS$_NOPRIV");
    /* negctl-knockon: lnm-privilege-check-bypassed */
    CHECK(do_translate(VMS_LNM_TBL_SYSTEM, "OVMX5B7$SYS_A", MK_UIC(210, 99), val, sizeof(val)) == 1 &&
          strcmp(val, "sysnam-value") == 0,
          "SYSTEM: the refused DELETE left the name and its value unchanged");

    /* Clean up (a) and (b) with a privileged identity so this suite leaves
     * no residue in the shared LNM$SYSTEM arena for a later suite. */
    (void)do_delete("OVMX5B7Z", MK_UIC(210, 9), PRV$M_SYSNAM,
                    VMS_LNM_TBL_SYSTEM, "OVMX5B7$SYS_A");
    (void)do_delete("OVMX5B7Z", MK_UIC(210, 9), PRV$M_SYSNAM,
                    VMS_LNM_TBL_SYSTEM, "OVMX5B7$SYS_B");
}

/* ---- LNM$GROUP --------------------------------------------------------
 * All GROUP scenarios share ONE UIC group (211) so derive_scope_key()
 * resolves every identity below to the SAME LNM$GROUP scope_key -- the
 * privilege check being proven here is orthogonal to the GROUP-scoping
 * property test_syssvc_lnm_groupjob.c already covers, and mixing groups
 * would risk a cross-group "absent" reading as a false negative. */
static void run_group(void)
{
    char val[256];
    uint32_t st;

    /* (a) GRPNAM alone. */
    st = do_define("OVMX5B7E", MK_UIC(211, 1), PRV$M_GRPNAM,
                   VMS_LNM_TBL_GROUP, "OVMX5B7$GRP_A", "grpnam-value");
    CHECK((st & 1) || st == SS$_SUPERSEDE,
          "GROUP: a process holding GRPNAM can DEFINE/GROUP");
    CHECK(do_translate(VMS_LNM_TBL_GROUP, "OVMX5B7$GRP_A", MK_UIC(211, 99), val, sizeof(val)) == 1 &&
          strcmp(val, "grpnam-value") == 0,
          "GROUP: the GRPNAM-created name is visible with its value");

    /* (b) GRPPRV alone (documented alternate). */
    st = do_define("OVMX5B7F", MK_UIC(211, 2), PRV$M_GRPPRV,
                   VMS_LNM_TBL_GROUP, "OVMX5B7$GRP_B", "grpprv-value");
    CHECK((st & 1) || st == SS$_SUPERSEDE,
          "GROUP: a process holding GRPPRV (not GRPNAM) can also DEFINE/GROUP");

    /* (c) SYSPRV alone (documented alternate, same as SYSTEM). */
    st = do_define("OVMX5B7G", MK_UIC(211, 3), PRV$M_SYSPRV,
                   VMS_LNM_TBL_GROUP, "OVMX5B7$GRP_C", "sysprv-value");
    CHECK((st & 1) || st == SS$_SUPERSEDE,
          "GROUP: a process holding SYSPRV (not GRPNAM/GRPPRV) can also DEFINE/GROUP");

    /* (d) Nothing: refused, table unchanged. */
    st = do_define("OVMX5B7H", MK_UIC(211, 4), 0,
                   VMS_LNM_TBL_GROUP, "OVMX5B7$GRP_D", "should-not-land");
    /* negctl: lnm-privilege-check-bypassed */
    CHECK(st == SS$_NOPRIV,
          "GROUP: a process holding none of GRPNAM/GRPPRV/SYSPRV is refused SS$_NOPRIV");
    /* negctl-knockon: lnm-privilege-check-bypassed */
    CHECK(do_translate(VMS_LNM_TBL_GROUP, "OVMX5B7$GRP_D", MK_UIC(211, 99), val, sizeof(val)) == 0,
          "GROUP: the refused DEFINE never created the name (table unchanged)");

    /* (e) DELETE gated the same way. */
    st = do_delete("OVMX5B7I", MK_UIC(211, 5), 0,
                   VMS_LNM_TBL_GROUP, "OVMX5B7$GRP_A");
    /* negctl: lnm-privilege-check-bypassed */
    CHECK(st == SS$_NOPRIV,
          "GROUP: a process holding none of GRPNAM/GRPPRV/SYSPRV is refused DEASSIGN/GROUP SS$_NOPRIV");
    /* negctl-knockon: lnm-privilege-check-bypassed */
    CHECK(do_translate(VMS_LNM_TBL_GROUP, "OVMX5B7$GRP_A", MK_UIC(211, 99), val, sizeof(val)) == 1 &&
          strcmp(val, "grpnam-value") == 0,
          "GROUP: the refused DELETE left the name and its value unchanged");

    /* Clean up. */
    (void)do_delete("OVMX5B7Z", MK_UIC(211, 9), PRV$M_GRPNAM,
                    VMS_LNM_TBL_GROUP, "OVMX5B7$GRP_A");
    (void)do_delete("OVMX5B7Z", MK_UIC(211, 9), PRV$M_GRPNAM,
                    VMS_LNM_TBL_GROUP, "OVMX5B7$GRP_B");
    (void)do_delete("OVMX5B7Z", MK_UIC(211, 9), PRV$M_GRPNAM,
                    VMS_LNM_TBL_GROUP, "OVMX5B7$GRP_C");
}

/* ---- LNM$JOB: no privilege required, at all ---------------------------- */

static void run_job(void)
{
    /*
     * LNM$JOB's scope_key is proc->job_id, fixed once at REGISTRATION from
     * real process ancestry (vms_proc_parent_job_id(), vms_module.c) -- it
     * is NOT settable via setident the way GROUP's scope (proc->uic) is.
     * Two SEPARATE fork()s (as do_define()/do_delete() each use) would
     * therefore become two INDEPENDENT job roots with two DIFFERENT
     * job_ids, and a delete-after-define across them would spuriously
     * report "absent" for a job-scoping reason having nothing to do with
     * privilege. So both calls happen inside the SAME child process here,
     * sharing one PCB and therefore one job_id -- this test's claim is
     * "no privilege is required", not "JOB is visible across processes"
     * (test_syssvc_lnm_groupjob.c already proves that separately).
     */
    int p[2];
    if (pipe(p) < 0) { perror("pipe"); exit(1); }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid == 0) {
        close(p[0]);
        struct { uint32_t define_status; uint32_t delete_status; } r;
        uint32_t sst = vms_kif_setident("OVMX5B7J", MK_UIC(212, 1), 0);
        if (!(sst & 1)) {
            r.define_status = sst;
            r.delete_status = sst;
        } else {
            const char *values[1] = { "job-value" };
            r.define_status = vms_kif_lnm_define(VMS_LNM_TBL_JOB, "OVMX5B7$JOB_A",
                                                 values, 1, 0, PSL_C_EXEC);
            r.delete_status = vms_kif_lnm_delete(VMS_LNM_TBL_JOB, "OVMX5B7$JOB_A",
                                                 PSL_C_EXEC);
        }
        {
            ssize_t w = write(p[1], &r, sizeof(r));
            (void)w;
        }
        close(p[1]);
        _exit(0);
    }
    close(p[1]);
    struct { uint32_t define_status; uint32_t delete_status; } r;
    r.define_status = 0;
    r.delete_status = 0;
    if (!read_bounded(p[0], &r, sizeof(r))) {
        r.define_status = 0xFFFFFFFF;
        r.delete_status = 0xFFFFFFFF;
    }
    close(p[0]);
    waitpid(pid, NULL, 0);

    /* An identity with NOTHING -- not even SETPRV -- still succeeds: the
     * default case of both privilege switches in vms_lnm.c is a genuine
     * no-op, not an accidental refusal that happens to never trigger. */
    CHECK((r.define_status & 1) || r.define_status == SS$_SUPERSEDE,
          "JOB: an unprivileged process can DEFINE/JOB (no privilege required)");
    CHECK(r.delete_status & 1,
          "JOB: an unprivileged process can DEASSIGN/JOB (no privilege required)");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_lnm_privilege (LNM$SYSTEM/LNM$GROUP privilege enforcement, vms-5b7) ===\n");

    if (!executive_present()) {
        printf("=== test_syssvc_lnm_privilege: SKIPPED (no /dev/vms -- privilege enforcement requires a real executive) ===\n");
        return EXIT_SKIP;
    }

    run_system();
    run_group();
    run_job();

    printf("=== test_syssvc_lnm_privilege: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
