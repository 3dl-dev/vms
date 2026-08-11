/*
 * test_kmod_procacct.c - the executive sources a process's ACCOUNTING
 * (CPU time, page faults, resident pages) and login time from the real
 * Linux task it owns, and reflects it through $GETJPI / $PROCESS_SCAN
 * (struct vms_procinfo.cputim / pageflts / pages / logintim + fields_valid).
 * vms-a7e, the executive per-process data model under the SHOW/SET parity
 * tree (vms-8ad / vms-6b8).
 *
 * WHAT THIS PROVES. Before vms-a7e the executive row carried only identity
 * (pid/name/uic/mode/privs/username/terminal) plus the P0/P1 extents; SHOW
 * SYSTEM's CPU column was sourced by a per-row sys$getjpi and Page-flts /
 * Pages were absent entirely (docs/oracle/vax73-show-system-process.md §5.1).
 * This suite asserts the executive now carries the accounting IN THE ROW,
 * sourced in-kernel from the task, with a VMS_PI_V_* validity bit per field:
 *
 *   - a live process's own row reports its real page faults (> 0 -- a
 *     process always faults its image in) and resident pages (> 0 -- a live
 *     user process always has an address space), each with its valid bit;
 *   - after burning measurable CPU, JPI$_CPUTIM is valid and non-zero;
 *   - fields with no faithful OVMX source (state, priority, dirio/bufio,
 *     the quota block) come back with their valid bits CLEAR -- absent, not
 *     a fabricated zero (INV-6, Rule 10).
 *
 * THE DECISIVE TEST IS A-BURNS / B-READS (CLAUDE.md Rule 11), the same
 * discipline as tests/qemu/test_kmod_p0.c. A per-process fake would pass
 * every SELF assertion here perfectly; only a SECOND process reading A's
 * accounting out of the executive proves it is executive-resident (sourced
 * from the task the executive pins by pid_ref) rather than a local variable
 * this program remembers about itself. A's page-fault/resident-page figures
 * are real properties of A that B cannot know except by reading A's row.
 *
 * FAIL-PRE / PASS-POST. Against an executive that does NOT run fill_proc_acct
 * (the pre-vms-a7e code path) every fields_valid bit is 0 and every
 * accounting field is 0, so every assertion below FAILS; with the executive
 * sourcing them it passes. The suite is its own before/after.
 *
 * NEGATIVE CONTROL: under NEGATIVE_CONTROL=1 (tests/qemu/Dockerfile boots
 * without insmod'ing vms.ko) there is no /dev/vms and this program fails at
 * open, saying so -- no per-process fallback (INV-6).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>

#include "vms_kif.h"

#define SS_NORMAL       1

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* What process B reports back to process A over a pipe. */
struct cross_report {
    uint32_t a_status;      /* B's VMS_IOCTL_GETJPI(PID=A) status */
    uint32_t a_valid;       /* A row's fields_valid, as B sees it */
    uint32_t a_pageflts;
    uint32_t a_pages;
    uint32_t a_cputim;
};

static int open_and_register(uint32_t *vms_pid)
{
    if (vms_kif_open() < 0) {
        printf("  FAIL: cannot open /dev/vms (executive absent)\n");
        return -1;
    }
    if (vms_kif_register(vms_pid) != SS_NORMAL) {
        printf("  FAIL: VMS_IOCTL_REGISTER rejected\n");
        return -1;
    }
    return 0;
}

/* One chunk of busy user CPU. volatile defeats the optimiser so the work is
 * really done. Sized small so the adaptive burn below re-checks CPU time
 * often -- important under QEMU TCG, where instructions are emulated. */
static void burn_chunk(void)
{
    volatile uint64_t acc = 0;
    for (uint64_t i = 0; i < 20000000ULL; i++)
        acc += i * 3 + 1;
    (void)acc;
}

/*
 * burn_until_cputim - consume real CPU until JPI$_CPUTIM registers a
 * non-zero figure, then return the row.
 *
 * ADAPTIVE AND DOUBLY BOUNDED so it is neither flaky nor unbounded: it
 * stops the instant cputim > 0 (one 10ms unit) -- so it burns almost
 * nothing on a fast native host -- and it gives up after a wall-clock
 * deadline so a broken accounting path fails the assertion honestly
 * instead of spinning past the harness's 120s timeout (memory gotcha:
 * QEMU proofs need a hard bound). JPI$_CPUTIM's unit is 10ms and the
 * guest tick is coarse, so this loops rather than assuming one fixed
 * burn crosses the threshold under emulation.
 */
static uint32_t burn_until_cputim(struct vms_procinfo *info)
{
    time_t deadline = time(NULL) + 10;   /* generous, still << 120s harness cap */
    uint32_t status = 0;

    do {
        burn_chunk();
        memset(info, 0, sizeof(*info));
        status = vms_kif_getjpi_self(info);
    } while (status == SS_NORMAL &&
             !((info->fields_valid & VMS_PI_V_CPUTIM) && info->cputim > 0) &&
             time(NULL) < deadline);

    return status;
}

/*
 * Process B. A separate Linux process, registered as a SEPARATE VMS
 * process -- it knows A only by the VMS process id A sent down the pipe,
 * and has no access to A's memory. Whatever B reads about A's accounting,
 * it read out of the executive's row for A.
 */
static void cross_reader(int wfd, uint32_t a_vms_pid)
{
    struct cross_report rep;
    struct vms_procinfo info;

    memset(&rep, 0, sizeof(rep));

    if (vms_kif_open() < 0 || vms_kif_register(NULL) != SS_NORMAL)
        _exit(3);

    memset(&info, 0, sizeof(info));
    rep.a_status   = vms_kif_getjpi_pid(a_vms_pid, &info);
    rep.a_valid    = info.fields_valid;
    rep.a_pageflts = info.pageflts;
    rep.a_pages    = info.pages;
    rep.a_cputim   = info.cputim;

    if (write(wfd, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        _exit(4);

    vms_kif_close();
    _exit(0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct vms_procinfo info;
    struct cross_report rep;
    uint32_t status, my_vms_pid = 0;
    int pipefd[2];
    pid_t child;
    ssize_t n;

    signal(SIGPIPE, SIG_IGN);

    printf("=== test_kmod_procacct: executive per-process accounting (vms-a7e) ===\n");

    if (open_and_register(&my_vms_pid) < 0) {
        printf("=== test_kmod_procacct: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    /* ---- a live process's own row carries real accounting ------------- */

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_self(&info);
    CHECK(status == SS_NORMAL, "GETJPI(SELF) succeeds");

    /* Diagnostic: the actual figures the executive sourced, so a reader of
     * the transcript sees real values, not just a pass/fail. */
    printf("  [self row] fields_valid=0x%03x cputim=%u pageflts=%u pages=%u "
           "logintim=%llu\n",
           info.fields_valid, info.cputim, info.pageflts, info.pages,
           (unsigned long long)info.logintim);

    /* negctl: proc-acct-not-sourced */
    CHECK((info.fields_valid & VMS_PI_V_PAGEFLTS) && info.pageflts > 0,
          "page faults are sourced and non-zero (a live process has faulted "
          "its image in) -- JPI$_PAGEFLTS from the executive");
    CHECK((info.fields_valid & VMS_PI_V_PAGES) && info.pages > 0,
          "resident pages are sourced and non-zero (a live user process has "
          "an address space) -- JPI$_PPGCNT from the executive");
    CHECK(info.fields_valid & VMS_PI_V_CPUTIM,
          "CPU time is sourced (valid bit set) -- JPI$_CPUTIM from the executive");
    CHECK(info.fields_valid & VMS_PI_V_LOGINTIM,
          "login time is sourced (valid bit set) -- JPI$_LOGINTIM from the "
          "executive");

    /* ---- unsourced fields are ABSENT, not a fabricated zero ----------- */

    /* OVMX has no VMS scheduler, no VMS priority, no direct/buffered I/O
     * split and no quota facility, so these must report their valid bit
     * CLEAR -- a reader must be able to tell "not available" from a real 0. */
    CHECK(!(info.fields_valid & VMS_PI_V_STATE),
          "scheduler state is honestly absent (no VMS scheduler) -- valid bit clear");
    CHECK(!(info.fields_valid & (VMS_PI_V_PRI | VMS_PI_V_PRIB)),
          "priority is honestly absent (no VMS priority) -- valid bits clear");
    CHECK(!(info.fields_valid & (VMS_PI_V_DIRIO | VMS_PI_V_BUFIO)),
          "direct/buffered I/O is honestly absent (no VMS I/O split) -- clear");
    CHECK(!(info.fields_valid & VMS_PI_V_QUOTA),
          "the quota block is honestly absent (no quota facility) -- clear");

    /* ---- CPU time advances as real CPU is consumed -------------------- */

    status = burn_until_cputim(&info);
    /* negctl: proc-acct-not-sourced */
    CHECK(status == SS_NORMAL && (info.fields_valid & VMS_PI_V_CPUTIM) &&
          info.cputim > 0,
          "after burning CPU, JPI$_CPUTIM reports a non-zero figure -- the "
          "executive is measuring real consumed CPU, not reporting a stuck 0");

    /* ---- A-BURNS / B-READS: the accounting is executive-resident ------ */

    /* Snapshot A's own figures so the cross-read can be compared against a
     * real property of A that B has no other way to know. */
    uint32_t a_pageflts = info.pageflts;
    uint32_t a_pages    = info.pages;

    if (pipe(pipefd) != 0) {
        CHECK(0, "pipe()");
        goto done;
    }
    child = fork();
    if (child < 0) {
        CHECK(0, "fork()");
        close(pipefd[0]); close(pipefd[1]);
        goto done;
    }
    if (child == 0) {
        close(pipefd[0]);
        cross_reader(pipefd[1], my_vms_pid);
        _exit(5); /* unreachable */
    }
    close(pipefd[1]);

    memset(&rep, 0, sizeof(rep));
    n = read(pipefd[0], &rep, sizeof(rep));
    close(pipefd[0]);
    waitpid(child, NULL, 0);

    CHECK(n == (ssize_t)sizeof(rep) && rep.a_status == SS_NORMAL,
          "a second process (B) could read A's row at all");
    /* negctl-knockon: proc-acct-not-sourced */
    CHECK(rep.a_status == SS_NORMAL &&
          (rep.a_valid & VMS_PI_V_PAGEFLTS) && rep.a_pageflts > 0 &&
          (rep.a_valid & VMS_PI_V_PAGES) && rep.a_pages > 0,
          "A-BURNS/B-READS: B reads A's real page-fault and resident-page "
          "counts out of A's executive row -- accounting a per-process fake "
          "could not show for another process");
    CHECK((rep.a_valid & VMS_PI_V_CPUTIM) && rep.a_cputim > 0,
          "B reads A's non-zero CPU time out of A's row (A had burned CPU) -- "
          "sourced from the task the executive pins, not from B's own state");
    /* The counts B saw are A's, so they are >= A's last self-read minus scan
     * jitter; the decisive point is they are A's magnitude, not B's ~startup
     * values. B has done almost no work, so if these were B's own figures
     * echoed back they would be a small fraction of A's post-burn counts. */
    CHECK(rep.a_pages >= a_pages / 2 && rep.a_pageflts >= a_pageflts / 2,
          "the figures B reports for A are A's magnitude, not B's own state "
          "misattributed to A");

    /* ---- $PROCESS_SCAN carries the accounting too (SHOW SYSTEM's path) - */

    {
        uint32_t index = 0;
        int found_self = 0, self_has_acct = 0;
        struct vms_procinfo row;
        while (vms_kif_procscan(&index, &row) == SS_NORMAL) {
            if (row.vms_pid == my_vms_pid) {
                found_self = 1;
                self_has_acct =
                    (row.fields_valid & VMS_PI_V_PAGEFLTS) && row.pageflts > 0 &&
                    (row.fields_valid & VMS_PI_V_PAGES)    && row.pages > 0 &&
                    (row.fields_valid & VMS_PI_V_CPUTIM);
                break;
            }
        }
        CHECK(found_self, "PROCSCAN enumerates this process's own row");
        /* negctl: proc-acct-not-sourced */
        CHECK(self_has_acct,
              "PROCSCAN rows carry the same executive accounting GETJPI does "
              "-- this is the exact path SHOW SYSTEM's columns read");
    }

done:
    vms_kif_close();
    printf("=== test_kmod_procacct: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
