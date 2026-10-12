/*
 * test_syssvc_lnm_groupjob.c - executive-resident LNM$GROUP and LNM$JOB,
 * through the vmslnm-MANAGER API, against a real /dev/vms (vms-aba).
 *
 * ============================================================
 * WHAT THIS PROVES, AND WHY test_syssvc_lnm_system.c DOES NOT COVER IT.
 *
 * vms-d37/#193 (SYSTEM) and vms-96e2 made LNM$SYSTEM executive-resident:
 * shared by every process on the node. vms-aba is the deferred other half
 * of that same design (docs/design-logical-name-placement.md §3.2): LNM$GROUP
 * is shared by every process in the caller's UIC GROUP, and LNM$JOB by every
 * process in the caller's JOB TREE (a top-level process and every subprocess
 * it SPAWNs) -- both scoped, unlike SYSTEM's single node-wide table. That
 * scoping is the whole point of this suite: a name DEFINE/GROUP'd or
 * DEFINE/JOB'd must be visible to a process that shares the scope and
 * INVISIBLE to one that does not, and no existing suite proves the "does
 * NOT see it" half for either table.
 *
 * FOUR PROCESSES, ONE SCOPE VARIABLE EACH:
 *
 *   A is this suite's own process -- a SUBPROCESS the harness boot task
 *   created (rd vms-3fa), SYSTEM's identity -- DEFINE/GROUP's and DEFINE/JOB's
 *   a name. Every process here is one the executive CREATED (VMS_IOCTL_CREPRC_
 *   PCB through tests/qemu/ovmx_test_fork.h); its UIC and job come from the
 *   executive, never from substrate credentials or fork position.
 *
 *   B is a SUBPROCESS of A: same UIC group, same job (a subprocess stays in its
 *   creator's job, DCL Dictionary SPAWN). B must see BOTH names.
 *
 *   C is a DETACHED process A creates in ANOTHER UIC group -- $CREPRC's uic
 *   argument (System Services Reference, $CREPRC: a uic makes the process
 *   detached). Different group and, being detached, a different job: C must
 *   see NEITHER name. (A process of another UIC in A's own job is not
 *   something a VMS program can create: a subprocess always carries its
 *   creator's UIC.)
 *
 *   D is a DETACHED process A creates with A's own identity: a new job, the
 *   same UIC group. D must see the GROUP name but NOT the JOB name.
 *
 * Together B/C/D exercise every combination a VMS program can create:
 * same group + same job (a subprocess), same group + different job (a detached
 * process with the creator's UIC), different group + different job (a
 * detached process with another UIC).
 *
 * NO STATUS CONSTANT IS ASSERTED BY VALUE for the success path (VMS odd/even
 * convention only), matching test_syssvc_lnm_crossproc.c's / test_syssvc_
 * lnm_system.c's convention. SS$_NOLOGNAM is named for "absent, not
 * executive-absent" negative controls, and SS$_NOSUCHDEV for the
 * no-executive branch (INV-6).
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * anchored by the lnm-group-scope-collapsed defect. That defect makes
 * derive_scope_key()'s LNM$GROUP case return the same scope_key SYSTEM uses
 * (0) instead of the caller's UIC group, so a GROUP-scoped name becomes
 * visible across UIC groups -- reddening exactly C's "does NOT see the
 * GROUP name" assertion and no other (a single dropped-scoping property,
 * not a blunderbuss over the whole facility).
 *
 * SYNCHRONISATION: pipes only, no sleeps; every wait is bounded, matching
 * test_syssvc_lnm_crossproc.c / test_syssvc_ef_mproc.c.
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
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "vms/logical.h"

#define EXIT_SKIP 77
#define PEER_TIMEOUT_MS 20000

/* A UIC group distinct from the default (root's, [0,0]) that C moves into.
 * Chosen away from tests/qemu/test_kmod_procnam.c's ALT_UIC_GROUP (300) and
 * test_kmod_ident.c's B_GID/D_GID (300/400) purely for readability -- these
 * are independent process trees with no shared executive state, so there is
 * no correctness reason they must differ, but a distinct value makes a
 * cross-suite log grep unambiguous. */
#define ALT_UIC_GROUP 7654

#define GRP_NAME "OVMXABA$GRPTEST"
#define GRP_VAL  "GROUP-SCOPED-VALUE"
#define JOB_NAME "OVMXABA$JOBTEST"
#define JOB_VAL  "JOB-SCOPED-VALUE"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

struct child_msg { uint32_t pass; uint32_t fail; };

static int read_bounded(int fd, void *buf, size_t len, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    size_t got = 0;
    while (got < len) {
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0) return 0;
        if (pr < 0) return -1;
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 1;
}

static int send_token(int fd, char tok) { return write(fd, &tok, 1) == 1 ? 0 : -1; }

static int wait_for_token(int fd, const char *who)
{
    char tok;
    if (read_bounded(fd, &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'G') {
        printf("  FAIL: %s: never saw the go token\n", who);
        fail++;
        return 0;
    }
    return 1;
}

static int report_and_exit(int out_fd)
{
    struct child_msg msg;
    msg.pass = (uint32_t)pass;
    msg.fail = (uint32_t)fail;
    if (write(out_fd, &msg, sizeof(msg)) != (ssize_t)sizeof(msg))
        return 1;
    return fail > 0 ? 1 : 0;
}

static int collect(pid_t pid, int in_fd, const char *who)
{
    struct child_msg cm = {0, 0};
    int r = read_bounded(in_fd, &cm, sizeof(cm), PEER_TIMEOUT_MS);
    if (r != 1) {
        printf("  FAIL: no final report from %s (r=%d)\n", who, r);
        fail++;
    } else {
        pass += (int)cm.pass;
        fail += (int)cm.fail;
    }
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    return r == 1 ? (int)cm.fail : 1;
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/*
 * run_b - same UIC group, same job (a subprocess A created). Must see BOTH
 * names.
 */
static int run_b(int in_fd, int out_fd)
{
    lnm_manager_t *mgr = lnm_get_manager();
    char val[256];
    uint16_t rlen = 0;
    uint32_t attrs = 0;
    uint32_t st;

    if (!wait_for_token(in_fd, "B")) return report_and_exit(out_fd);

    st = lnm_translate(mgr, LNM_GROUP_TABLE, GRP_NAME, val, sizeof(val), &rlen, &attrs);
    printf("  INFO: B: lnm_translate(LNM$GROUP, %s) status=%u value=\"%s\"\n", GRP_NAME, st, val);
    CHECK(st == SS$_NORMAL && strcmp(val, GRP_VAL) == 0,
          "B (same group, same job): sees the GROUP-scoped name A defined");

    st = lnm_translate(mgr, LNM_JOB_TABLE, JOB_NAME, val, sizeof(val), &rlen, &attrs);
    printf("  INFO: B: lnm_translate(LNM$JOB, %s) status=%u value=\"%s\"\n", JOB_NAME, st, val);
    CHECK(st == SS$_NORMAL && strcmp(val, JOB_VAL) == 0,
          "B (same group, same job): sees the JOB-scoped name A defined");

    return report_and_exit(out_fd);
}

/*
 * run_c - DIFFERENT UIC group and DIFFERENT job ($CREPRC uic: detached).
 * Must see neither name.
 */
static int run_c(int in_fd, int out_fd)
{
    lnm_manager_t *mgr;
    char val[256];
    uint16_t rlen = 0;
    uint32_t attrs = 0;
    uint32_t st;

    mgr = lnm_get_manager();
    if (!wait_for_token(in_fd, "C")) return report_and_exit(out_fd);

    st = lnm_translate(mgr, LNM_GROUP_TABLE, GRP_NAME, val, sizeof(val), &rlen, &attrs);
    printf("  INFO: C: lnm_translate(LNM$GROUP, %s) status=%u value=\"%s\"\n", GRP_NAME, st, val);
    /* negctl: lnm-group-scope-collapsed */
    CHECK(st == SS$_NOLOGNAM,
          "C (different group, different job): does NOT see the GROUP-scoped name A defined");

    st = lnm_translate(mgr, LNM_JOB_TABLE, JOB_NAME, val, sizeof(val), &rlen, &attrs);
    printf("  INFO: C: lnm_translate(LNM$JOB, %s) status=%u value=\"%s\"\n", JOB_NAME, st, val);
    CHECK(st == SS$_NOLOGNAM,
          "C (different group, different job): does NOT see the JOB-scoped name either");

    return report_and_exit(out_fd);
}

/*
 * run_d - SAME UIC group as A, DIFFERENT job: a DETACHED process A created
 * with A's identity. Must see the GROUP name, must NOT see the JOB name.
 */
static int run_d(int in_fd, int out_fd)
{
    lnm_manager_t *mgr = lnm_get_manager();
    char val[256];
    uint16_t rlen = 0;
    uint32_t attrs = 0;
    uint32_t st;

    if (!wait_for_token(in_fd, "D")) return report_and_exit(out_fd);

    st = lnm_translate(mgr, LNM_GROUP_TABLE, GRP_NAME, val, sizeof(val), &rlen, &attrs);
    printf("  INFO: D: lnm_translate(LNM$GROUP, %s) status=%u value=\"%s\"\n", GRP_NAME, st, val);
    CHECK(st == SS$_NORMAL && strcmp(val, GRP_VAL) == 0,
          "D (same group, different job): sees the GROUP-scoped name A defined");

    st = lnm_translate(mgr, LNM_JOB_TABLE, JOB_NAME, val, sizeof(val), &rlen, &attrs);
    printf("  INFO: D: lnm_translate(LNM$JOB, %s) status=%u value=\"%s\"\n", JOB_NAME, st, val);
    CHECK(st == SS$_NOLOGNAM,
          "D (same group, different job): does NOT see the JOB-scoped name (different job tree)");

    return report_and_exit(out_fd);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    lnm_manager_t *mgr;
    char val[256];
    uint16_t rlen = 0;
    uint32_t attrs = 0;
    uint32_t st;

    printf("=== test_syssvc_lnm_groupjob (executive-resident LNM$GROUP / LNM$JOB via the vmslnm-manager API) ===\n");

    if (!executive_present()) {
        /* CI negative control only: no /dev/vms means honest failure, never
         * a per-process fallback (INV-6), exactly like SYSTEM already. */
        printf("  FAIL: cannot open /dev/vms\n");
        mgr = lnm_get_manager();
        st = lnm_create(mgr, LNM_GROUP_TABLE, GRP_NAME, GRP_VAL, LNM_ATTR_TERMINAL, LNM_MODE_USER);
        CHECK(st == SS$_NOSUCHDEV,
              "lnm_create against LNM$GROUP fails SS$_NOSUCHDEV with no executive (no per-process fallback)");
        st = lnm_create(mgr, LNM_JOB_TABLE, JOB_NAME, JOB_VAL, LNM_ATTR_TERMINAL, LNM_MODE_USER);
        CHECK(st == SS$_NOSUCHDEV,
              "lnm_create against LNM$JOB fails SS$_NOSUCHDEV with no executive (no per-process fallback)");
        printf("=== test_syssvc_lnm_groupjob: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n", pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* This process is "A". */
    mgr = lnm_get_manager();

    /* Negative control WITH an executive: a never-defined name is absent
     * (SS$_NOLOGNAM), proving the executive is present and answering before
     * any positive assertion below could be mistaken for that. */
    st = lnm_translate(mgr, LNM_GROUP_TABLE, "OVMXABA$NEVER_DEFINED", val, sizeof(val), &rlen, &attrs);
    CHECK(st == SS$_NOLOGNAM, "A: a never-defined LNM$GROUP name is absent, not executive-absent");
    st = lnm_translate(mgr, LNM_JOB_TABLE, "OVMXABA$NEVER_DEFINED", val, sizeof(val), &rlen, &attrs);
    CHECK(st == SS$_NOLOGNAM, "A: a never-defined LNM$JOB name is absent, not executive-absent");

    st = lnm_create(mgr, LNM_GROUP_TABLE, GRP_NAME, GRP_VAL, LNM_ATTR_TERMINAL, LNM_MODE_USER);
    printf("  INFO: A: lnm_create(LNM$GROUP, %s=%s) status=%u\n", GRP_NAME, GRP_VAL, st);
    CHECK(st == SS$_NORMAL || st == SS$_SUPERSEDE, "A: DEFINE/GROUP reported success");

    st = lnm_create(mgr, LNM_JOB_TABLE, JOB_NAME, JOB_VAL, LNM_ATTR_TERMINAL, LNM_MODE_USER);
    printf("  INFO: A: lnm_create(LNM$JOB, %s=%s) status=%u\n", JOB_NAME, JOB_VAL, st);
    CHECK(st == SS$_NORMAL || st == SS$_SUPERSEDE, "A: DEFINE/JOB reported success");

    /* A itself: SHOW LOGICAL / F$TRNLNM's search-list path (LNM$FILE_DEV)
     * must resolve both of its own names too. */
    st = lnm_translate(mgr, LNM_FILE_DEV, GRP_NAME, val, sizeof(val), &rlen, &attrs);
    CHECK(st == SS$_NORMAL && strcmp(val, GRP_VAL) == 0,
          "A: GROUP name resolves through the default LNM$FILE_DEV search list");
    st = lnm_translate(mgr, LNM_FILE_DEV, JOB_NAME, val, sizeof(val), &rlen, &attrs);
    CHECK(st == SS$_NORMAL && strcmp(val, JOB_VAL) == 0,
          "A: JOB name resolves through the default LNM$FILE_DEV search list");

    /* D: a DETACHED process with A's identity -- a new job, the same group. */
    int d_go[2], d_done[2];
    if (pipe(d_go) < 0 || pipe(d_done) < 0) { printf("  FAIL: pipe() failed\n"); return 1; }
    pid_t d_pid = -1;
    {
        struct vms_procinfo me;
        uint32_t dst = 0;
        if (vms_kif_getjpi_self(&me) & 1)
            d_pid = ovmx_fork_detached_st(me.username, me.uic, me.perm_privs, &dst);
        if (d_pid == 0) {
            close(d_go[1]); close(d_done[0]);
            _exit(run_d(d_go[0], d_done[1]));
        }
        if (d_pid < 0) { printf("  FAIL: A could not create the detached D (%08X)\n", (unsigned)dst); fail++; }
    }
    close(d_go[0]); close(d_done[1]);
    if (send_token(d_go[1], 'G') < 0) fail++;

    /* B: a subprocess of A, no identity change. */
    int b_go[2], b_done[2];
    if (pipe(b_go) < 0 || pipe(b_done) < 0) { printf("  FAIL: pipe() failed\n"); fail++; }
    pid_t b_pid = ovmx_fork_subprocess();
    if (b_pid < 0) { printf("  FAIL: fork() for B failed\n"); fail++; }
    if (b_pid == 0) {
        close(b_go[1]); close(b_done[0]);
        _exit(run_b(b_go[0], b_done[1]));
    }
    close(b_go[0]); close(b_done[1]);
    if (send_token(b_go[1], 'G') < 0) fail++;

    /* C: a DETACHED process in another UIC group ($CREPRC uic). */
    int c_go[2], c_done[2];
    if (pipe(c_go) < 0 || pipe(c_done) < 0) { printf("  FAIL: pipe() failed\n"); fail++; }
    pid_t c_pid = -1;
    {
        struct vms_procinfo me;
        uint32_t cst = 0;
        if (vms_kif_getjpi_self(&me) & 1)
            c_pid = ovmx_fork_detached_st("OVMXABAC", ((uint32_t)ALT_UIC_GROUP << 16) | 1u,
                                          me.perm_privs, &cst);
        if (c_pid < 0) { printf("  FAIL: A could not create C in UIC group %d (%08X)\n", ALT_UIC_GROUP, (unsigned)cst); fail++; }
    }
    if (c_pid == 0) {
        close(c_go[1]); close(c_done[0]);
        _exit(run_c(c_go[0], c_done[1]));
    }
    close(c_go[0]); close(c_done[1]);
    if (send_token(c_go[1], 'G') < 0) fail++;

    collect(b_pid, b_done[0], "B");
    collect(c_pid, c_done[0], "C");
    collect(d_pid, d_done[0], "D");

    /* Tidy up so a re-run in the same booted guest starts clean (the
     * executive's LNM$GROUP/LNM$JOB tables persist across suites, same as
     * LNM$SYSTEM -- test_syssvc_lnm_crossproc.c's cleanup comment applies
     * here too). */
    (void)lnm_delete(mgr, LNM_GROUP_TABLE, GRP_NAME, LNM_MODE_USER);
    (void)lnm_delete(mgr, LNM_JOB_TABLE, JOB_NAME, LNM_MODE_USER);

    printf("=== test_syssvc_lnm_groupjob: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
