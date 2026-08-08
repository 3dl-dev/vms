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
 *   A (this binary's own top-level process) DEFINE/GROUP's and
 *   DEFINE/JOB's a name. Being the top of a fresh process tree (its real
 *   parent -- the QEMU init harness that execs it -- never registers with
 *   vms.ko), A is its own JOB ROOT (vms_module.c's vms_proc_parent_job_id()):
 *   its job_id becomes its own vms_pid.
 *
 *   B is fork()'d from A, no credential change: SAME UIC group (derived
 *   from the same real uid/gid) and, because A is already a registered VMS
 *   process by the time B forks, SAME job (B inherits A's job_id) --
 *   exactly how SPAWN's child stays in the parent's job on real VMS (DCL
 *   Dictionary, SPAWN). B must see BOTH names.
 *
 *   C is also fork()'d from A, but setgid()s to a different UIC group
 *   BEFORE its first executive call (same technique as
 *   tests/qemu/test_kmod_procnam.c's ALT_UIC_GROUP case: uid stays 0, so
 *   CAP_SYS_ADMIN to open /dev/vms survives, but the derived UIC group
 *   changes). C is still a direct child of A, so it still inherits A's job.
 *   C must see the JOB name (job matches) but NOT the GROUP name (group
 *   does not).
 *
 *   D is fork()'d directly from the ORIGINAL harness process, BEFORE A ever
 *   touches vms_kif -- a sibling of A, not a descendant, so D's own real
 *   parent (the harness) is unregistered and D becomes its OWN job root,
 *   distinct from A's. D never setgid()s, so it shares A's default UIC
 *   group. D must see the GROUP name (group matches) but NOT the JOB name
 *   (job does not).
 *
 * Together B/C/D exercise every combination this design has: same+same,
 * same-job+diff-group, diff-job+same-group. (diff+diff is not additionally
 * informative once the other three hold, and would need a fifth process
 * tree with no shared ancestor -- not worth the harness complexity here.)
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
 * run_b - same UIC group (no setgid), same job (direct child of an already-
 * registered A). Must see BOTH names.
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
 * run_c - DIFFERENT UIC group (setgid before the first executive call),
 * SAME job (still a direct child of A). Must see the JOB name, must NOT
 * see the GROUP name.
 */
static int run_c(int in_fd, int out_fd)
{
    lnm_manager_t *mgr;
    char val[256];
    uint16_t rlen = 0;
    uint32_t attrs = 0;
    uint32_t st;

    /* setgid() only, before touching vms_kif at all: staying uid 0 keeps
     * CAP_SYS_ADMIN (needed to open /dev/vms), while the UIC group this
     * registration derives moves to ALT_UIC_GROUP (same technique as
     * tests/qemu/test_kmod_procnam.c). */
    if (setgid(ALT_UIC_GROUP) != 0) {
        printf("  FAIL: C: setgid(%d) failed\n", ALT_UIC_GROUP);
        fail++;
        return report_and_exit(out_fd);
    }

    mgr = lnm_get_manager();
    if (!wait_for_token(in_fd, "C")) return report_and_exit(out_fd);

    st = lnm_translate(mgr, LNM_GROUP_TABLE, GRP_NAME, val, sizeof(val), &rlen, &attrs);
    printf("  INFO: C: lnm_translate(LNM$GROUP, %s) status=%u value=\"%s\"\n", GRP_NAME, st, val);
    /* negctl: lnm-group-scope-collapsed */
    CHECK(st == SS$_NOLOGNAM,
          "C (different group, same job): does NOT see the GROUP-scoped name A defined");

    st = lnm_translate(mgr, LNM_JOB_TABLE, JOB_NAME, val, sizeof(val), &rlen, &attrs);
    printf("  INFO: C: lnm_translate(LNM$JOB, %s) status=%u value=\"%s\"\n", JOB_NAME, st, val);
    CHECK(st == SS$_NORMAL && strcmp(val, JOB_VAL) == 0,
          "C (different group, same job): still sees the JOB-scoped name (job is shared)");

    return report_and_exit(out_fd);
}

/*
 * run_d - SAME UIC group as A (no setgid, same default creds), DIFFERENT
 * job: forked from the harness process before A ever registers, so D's own
 * real parent is unregistered and D is its own job root. Must see the
 * GROUP name, must NOT see the JOB name.
 *
 * REGISTRATION ORDER IS THE WHOLE POINT, AND IT IS NOT A SCHEDULING ACCIDENT
 * (round 1 of this test learned that the hard way -- it relied on fork()
 * order alone and D inherited A's job anyway). vms_module.c's
 * vms_proc_parent_job_id() looks at whether the CALLER'S real_parent is
 * ALREADY registered at the moment the CALLER itself first reaches vms.ko --
 * not at which process forked first. Being forked before A touches vms_kif
 * only matters if D also REGISTERS before A does, and fork() gives no
 * ordering guarantee over which of a parent/child pair runs first. So D
 * forces its own registration with a throwaway LNM$SYSTEM translate (any
 * table triggers vms_kif's kif_bind()) and only THEN tells A it is safe to
 * touch vms_kif at all -- A's negative-control checks, a few lines into
 * main(), are themselves executive calls and would otherwise race this.
 */
static int run_d(int reg_fd, int in_fd, int out_fd)
{
    lnm_manager_t *mgr = lnm_get_manager();
    char val[256];
    uint16_t rlen = 0;
    uint32_t attrs = 0;
    uint32_t st;

    /* Force registration NOW, deterministically before A's first executive
     * call: this is what makes D a job root rather than an inheritor. The
     * result of this throwaway translate is irrelevant. */
    (void)lnm_translate(mgr, LNM_SYSTEM_TABLE, "OVMXABA$D_EARLY_TOUCH",
                        val, sizeof(val), &rlen, &attrs);
    if (send_token(reg_fd, 'R') < 0) { fail++; return report_and_exit(out_fd); }

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

    /* Fork D FIRST, before this process (soon to be "A") ever calls a
     * vms_kif function -- D's real parent is this harness process, which
     * never registers, so D becomes its own job root PROVIDED D actually
     * registers with vms.ko before A does (see run_d's header comment: fork
     * order alone does not guarantee that). d_reg is the barrier that makes
     * it deterministic: A blocks on it below before its own first executive
     * call (the negative-control translates just past this block). */
    int d_reg[2], d_go[2], d_done[2];
    if (pipe(d_reg) < 0 || pipe(d_go) < 0 || pipe(d_done) < 0) {
        printf("  FAIL: pipe() failed\n"); return 1;
    }
    pid_t d_pid = fork();
    if (d_pid < 0) { printf("  FAIL: fork() for D failed\n"); return 1; }
    if (d_pid == 0) {
        close(d_reg[0]); close(d_go[1]); close(d_done[0]);
        _exit(run_d(d_reg[1], d_go[0], d_done[1]));
    }
    close(d_reg[1]); close(d_go[0]); close(d_done[1]);

    /* Block here until D confirms it has ALREADY registered with vms.ko.
     * Nothing below this line may be this process's first vms_kif call --
     * that ordering, not fork() order, is what makes D a job root instead
     * of an inheritor (see run_d's header comment). */
    {
        char tok;
        if (read_bounded(d_reg[0], &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'R') {
            printf("  FAIL: A: never saw D's registration-complete token\n");
            fail++;
        }
    }

    /* This process is now "A". */
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

    /* Now that both names are defined, release D. */
    if (send_token(d_go[1], 'G') < 0) fail++;

    /* Fork B: no credential change. */
    int b_go[2], b_done[2];
    if (pipe(b_go) < 0 || pipe(b_done) < 0) { printf("  FAIL: pipe() failed\n"); fail++; }
    pid_t b_pid = fork();
    if (b_pid < 0) { printf("  FAIL: fork() for B failed\n"); fail++; }
    if (b_pid == 0) {
        close(b_go[1]); close(b_done[0]);
        _exit(run_b(b_go[0], b_done[1]));
    }
    close(b_go[0]); close(b_done[1]);
    if (send_token(b_go[1], 'G') < 0) fail++;

    /* Fork C: setgid happens INSIDE run_c, before its first executive call. */
    int c_go[2], c_done[2];
    if (pipe(c_go) < 0 || pipe(c_done) < 0) { printf("  FAIL: pipe() failed\n"); fail++; }
    pid_t c_pid = fork();
    if (c_pid < 0) { printf("  FAIL: fork() for C failed\n"); fail++; }
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
