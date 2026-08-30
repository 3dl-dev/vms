/*
 * test_syssvc_acp_dlm.c - the Files-11 (ODS-2) ACP serializes concurrent
 * writers with the DLM per-volume synchronization lock, not a userspace flock
 * (vms-233, epic vms-208; design-files11-acp-executive.md §4.7).
 *
 * On real OpenVMS the ODS-2 XQP runs in every requesting process's context in
 * kernel mode and serializes on-disk-structure updates through the distributed
 * lock manager's per-volume "volume synchronization" lock (OpenVMS Internals &
 * Data Structures Manual, XQP execution model). OVMX's executive ACP does the
 * same: each IO$_CREATE/DELETE/MODIFY and each IO$_WRITEVBLK takes an EX-mode
 * $ENQ on a per-volume resource across its allocate/read-modify-write/flush
 * span, enqueued in the caller's ioctl context (vms_lock_acp_vol_ex). This
 * replaces the reverted userspace flock broker (vms-49d) and — unlike flock —
 * generalizes to the clustered / MSCP-served case (the resource is the
 * cluster-wide sync point).
 *
 * WHAT THIS SUITE PROVES, against a real /dev/vms, over the real-VAX ODS-2
 * fixture the harness seeds WRITABLE on VDA0::
 *
 *   N_WORKERS unrelated processes (fork + re-exec, EACH re-registering with the
 *   executive and $ASSIGNing its OWN file-class channel) are released from a
 *   pipe barrier at the SAME instant and each concurrently IO$_CREATEs a
 *   DISTINCT file in the SAME directory [OVMXDIR]. Every create allocates a
 *   fresh FID from INDEXF.SYS's index bitmap and inserts a record in the one
 *   shared directory — the exact shared read-modify-write the volume lock
 *   protects. After every worker exits, the parent asserts EVERY file is present
 *   BY NAME and resolves a DISTINCT real FID (its own header, not a clobber).
 *
 * NEGATIVE DIRECTION (the enforcement mechanism proves BOTH ways). WITHOUT the
 * lock this suite CATCHES the corruption: with the per-volume EX $ENQ dropped
 * (facility_defects.sh negctl `acp-fileop-no-dlm-lock`, which no-ops the enqueue
 * in vms_ioctl_acp_fileop), the workers — released together and each sleeping in
 * submit_bio_wait on the shared index-bitmap block — all read the SAME free bit
 * and hand out the SAME FID: the header writes clobber each other and two names
 * resolve to one FID. The barrier + N_WORKERS make the collision reliable in one
 * run. (submit_bio_wait genuinely sleeps and bypasses any cache, so even the
 * single-CPU QEMU guest interleaves the unlocked critical sections.)
 *
 * WHY CREATE-ONLY, NOT DATA WRITES (stated, not faked — CLAUDE.md Rule 10). The
 * real-VAX fixture is a near-full ~800-block volume; concurrent IO$_WRITEVBLK
 * data-block allocation exhausts it (SS$_DEVICEFULL) and cannot be hosted here.
 * So the concurrency proof targets the CREATE critical section (FID allocation +
 * directory insert) — which IS the negctl's injection point and the sharpest
 * shared-structure race. The IO$_WRITEVBLK volume lock is in place (same
 * mechanism) and is exercised single-threaded by test_syssvc_acp_rw.
 *
 * NO /dev/vms -> honest SKIP (77), never a fake pass: the ACP and its DLM lock
 * are executive-resident, so with no /dev/vms there is nothing to serialize.
 *
 * ISOLATION. Creates DISTINCT names (WnFm.TST) in [OVMXDIR], never HELLO.TXT /
 * CREAT.TST / MODF.TST which the acp_access / acp_rw / acp_create suites use,
 * allocates NO data blocks, and DELETES every file it created — restoring the
 * directory. Each shard boots its own fresh writable fixture copy.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "vmsfs/ods2.h"   /* ODS2_FK_* file-kind selectors for CREATE */

#define EXIT_SKIP 77
#define ODS2_UNIT       "VDA0:"
#define OVMXDIR_FID_NUM 11u     /* [OVMXDIR] in the real-VAX fixture */

#define N_WORKERS  8            /* concurrent, unrelated writer processes */
#define N_PER      1            /* files each worker creates (tight single-wave race) */
#define N_FILES    (N_WORKERS * N_PER)

static int pass = 0;
static int fail = 0;

static void check(int cond, const char *name)
{
    if (cond) { printf("  PASS: %s\n", name); pass++; }
    else      { printf("  FAIL: %s\n", name); fail++; }
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

static void file_name(char *out, size_t sz, int w, int f)
{
    snprintf(out, sz, "W%dF%d.TST", w, f);
}

/*
 * One worker process (re-exec'd so it has its OWN executive registration and
 * kif transport state — a plain fork would inherit the parent's already-bound
 * /dev/vms handle and never re-register under its own tgid, exactly as
 * test_syssvc_mbx_crossproc re-execs for a genuinely separate PCB).
 *
 * ready_wfd/go_rfd form the barrier: signal READY, then block until the parent
 * releases every worker at once, so all N hit FID/directory allocation together.
 */
static int worker_main(int idx, int ready_wfd, int go_rfd)
{
    uint32_t chan = 0, st;
    char go;
    int f;

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL))
        return 10;
    if (!executive_present())
        return 11;

    st = vms_kif_acp_mount(ODS2_UNIT);          /* idempotent: parent mounted it */
    if (!$VMS_STATUS_SUCCESS(st))
        return 12;
    st = vms_kif_acp_assign(ODS2_UNIT, &chan);  /* this worker's OWN channel */
    if (!$VMS_STATUS_SUCCESS(st) || chan == 0)
        return 13;

    /* Barrier: announce ready, then wait for the simultaneous release. */
    { char r = (char)('0' + (idx & 7)); if (write(ready_wfd, &r, 1) != 1) return 14; }
    if (read(go_rfd, &go, 1) != 1)
        return 15;

    for (f = 0; f < N_PER; f++) {
        struct vms_acp_fileop_args fop;
        char nm[VMS_ACP_NAME_SIZE];

        file_name(nm, sizeof(nm), idx, f);

        /* CREATE-ONLY: allocate a FID + write the header + enter it in the
         * directory. exsz=0 => NO data-block allocation (fixture is near-full);
         * no M_ACCESS => no window. This is the FID/index-bitmap + directory
         * critical section the volume lock serializes. */
        memset(&fop, 0, sizeof(fop));
        fop.chan = chan;
        fop.func = VMS_ACP_FOP_CREATE;
        fop.modifiers = VMS_ACP_M_CREATE;
        fop.kind = ODS2_FK_DATA_FIX;
        fop.did_num = OVMXDIR_FID_NUM;
        fop.did_seq = 1;
        fop.version = 0;                        /* highest+1 */
        strncpy(fop.name, nm, VMS_ACP_NAME_SIZE - 1);
        st = vms_kif_acp_fileop(&fop);
        if (!$VMS_STATUS_SUCCESS(st)) {
            fprintf(stderr, "  WORKER %d: CREATE %s failed st=0x%08x\n", idx, nm, st);
            return 20;                          /* honest allocation failure */
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t chan = 0, st;
    int w, f;
    int ready[2], go[2];
    pid_t pids[N_WORKERS];
    int all_workers_ok = 1;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    /* Re-exec'd worker mode: --worker <idx> <ready_wfd> <go_rfd>. */
    if (argc >= 5 && strcmp(argv[1], "--worker") == 0)
        return worker_main(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));

    printf("=== test_syssvc_acp_dlm: ACP serializes concurrent writers with the "
           "DLM per-volume lock (EX $ENQ, replaces the flock broker; vms-233, "
           "epic vms-208) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (!executive_present()) {
        printf("=== test_syssvc_acp_dlm: 0 passed, 0 failed (SKIPPED: no /dev/vms -- "
               "the ACP + its DLM volume lock are executive-resident) ===\n");
        return EXIT_SKIP;
    }

    st = vms_kif_acp_mount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$MOUNT of the genuine ODS-2 " ODS2_UNIT " (precondition)");
    st = vms_kif_acp_assign(ODS2_UNIT, &chan);
    check($VMS_STATUS_SUCCESS(st) && chan != 0, "$ASSIGN a file-class channel (precondition)");
    if (chan == 0) {
        printf("=== test_syssvc_acp_dlm: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    if (pipe(ready) < 0 || pipe(go) < 0) {
        printf("  FAIL: pipe() failed\n");
        return 1;
    }

    /* Launch N workers. Each re-execs itself, inheriting ready[1] (write) and
     * go[0] (read); the parent keeps ready[0] (read) and go[1] (write). */
    for (w = 0; w < N_WORKERS; w++) {
        pid_t pid = fork();
        if (pid < 0) { printf("  FAIL: fork() failed\n"); return 1; }
        if (pid == 0) {
            char sidx[16], swr[16], srd[16];
            snprintf(sidx, sizeof(sidx), "%d", w);
            snprintf(swr, sizeof(swr), "%d", ready[1]);
            snprintf(srd, sizeof(srd), "%d", go[0]);
            execl(argv[0], argv[0], "--worker", sidx, swr, srd, (char *)NULL);
            _exit(99);
        }
        pids[w] = pid;
    }

    /* Wait until every worker is ready ($ASSIGN done, blocked on the barrier),
     * then release them all AT ONCE so their FID/directory allocations collide
     * in the same instant — the condition the volume lock must serialize. */
    for (w = 0; w < N_WORKERS; w++) {
        char r;
        if (read(ready[0], &r, 1) != 1) { printf("  FAIL: worker never signalled ready\n"); }
    }
    for (w = 0; w < N_WORKERS; w++) {
        char g = 'G';
        if (write(go[1], &g, 1) != 1) { /* worker gone; waitpid reports it */ }
    }

    for (w = 0; w < N_WORKERS; w++) {
        int wstatus = 0;
        waitpid(pids[w], &wstatus, 0);
        if (!(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)) {
            all_workers_ok = 0;
            fprintf(stderr, "  WORKER %d exit: ifexited=%d code=%d\n",
                    w, WIFEXITED(wstatus), WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1);
        }
    }
    check(all_workers_ok,
          "every concurrent writer completed its create (no honest allocation failure)");

    /*
     * The proof: re-ACCESS every file each worker created, BY NAME, and assert
     * it is present and resolves a DISTINCT real FID. Without the volume lock,
     * colliding FID allocations hand two names the same FID (and clobber the
     * loser's header), so a file is missing or two names share one FID.
     */
    {
        uint32_t seen_fid[N_FILES];
        int n_present = 0, n_distinct, i, j;

        for (w = 0; w < N_WORKERS; w++) {
            for (f = 0; f < N_PER; f++) {
                struct vms_acp_access_args a;
                char nm[VMS_ACP_NAME_SIZE];
                uint32_t fid;

                file_name(nm, sizeof(nm), w, f);
                memset(&a, 0, sizeof(a));
                a.chan = chan;
                a.did_num = OVMXDIR_FID_NUM;
                a.did_seq = 1;
                a.version = 0;                  /* highest */
                strncpy(a.name, nm, VMS_ACP_NAME_SIZE - 1);
                st = vms_kif_acp_access(&a);
                if (!$VMS_STATUS_SUCCESS(st)) {
                    fprintf(stderr, "  VERIFY: ACCESS %s failed st=0x%08x\n", nm, st);
                    continue;                   /* missing => corruption */
                }
                fid = ((uint32_t)a.fid_num) | ((uint32_t)a.fid_nmx << 16);
                seen_fid[n_present++] = fid;
                (void)vms_kif_acp_deaccess(chan);
            }
        }

        /* All FIDs distinct (no two names share a header slot). */
        n_distinct = n_present;
        for (i = 0; i < n_present && n_distinct == n_present; i++)
            for (j = i + 1; j < n_present; j++)
                if (seen_fid[i] == seen_fid[j]) { n_distinct = 0; break; }

        /* Dropping the per-volume EX $ENQ in vms_ioctl_acp_fileop
         * (vms_lock_acp_vol_ex) lets these simultaneous CREATEs race INDEXF's
         * index bitmap and collide on FIDs, clobbering headers -- so a created
         * name is lost or two names share one FID. Only these cross-process
         * assertions can tell; the single-writer suites cannot. */
        /* negctl: acp-fileop-no-dlm-lock */
        check(n_present == N_FILES,
              "every file the concurrent writers created is present BY NAME (no lost file)");
        /* negctl: acp-fileop-no-dlm-lock */
        check(n_distinct == n_present && n_present > 0,
              "every created file resolves a DISTINCT FID (no duplicate FID from a raced index bitmap)");

        printf("  INFO: %d/%d present, FIDs %sdistinct\n",
               n_present, N_FILES, (n_distinct == n_present) ? "" : "NOT ");
    }

    /* Restore the fixture: delete every file we created (best effort). */
    for (w = 0; w < N_WORKERS; w++) {
        for (f = 0; f < N_PER; f++) {
            struct vms_acp_fileop_args d;
            char nm[VMS_ACP_NAME_SIZE];
            file_name(nm, sizeof(nm), w, f);
            memset(&d, 0, sizeof(d));
            d.chan = chan;
            d.func = VMS_ACP_FOP_DELETE;
            d.modifiers = VMS_ACP_M_DELETE;
            d.did_num = OVMXDIR_FID_NUM;
            d.did_seq = 1;
            d.version = 0;
            strncpy(d.name, nm, VMS_ACP_NAME_SIZE - 1);
            (void)vms_kif_acp_fileop(&d);
        }
    }

    printf("=== test_syssvc_acp_dlm: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
