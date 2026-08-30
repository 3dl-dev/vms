/*
 * test_syssvc_rms_reclock.c - RMS RECORD-level locking behind the real DLM
 * (vms-0dd, vms-407-B, docs/design-rms-record-lock.md). Completes RMS-behind-
 * DLM after vms-50e's FILE-level share arbitration: a RAB's record-locking
 * intent (rab$l_rop) now takes a per-record $ENQ on the real executive lock
 * manager, a CHILD of the FAB's file-access lock (vms-50e), so two streams
 * contending for one record are arbitrated by the real DLM -- never a
 * userspace record table (INV-6).
 *
 * CRITICAL, and the whole reason this test exists rather than a simpler one:
 * RAB$M_NLK / RAB$M_RLK are NOT lock modes, they are read MODIFIERS (Guide to
 * OpenVMS File Applications; the RMS status codes are the oracle):
 *
 *   default (neither bit)  -- the LOCKING read: a real EX $ENQ, child of the
 *                             file lock. A second stream's default $get of
 *                             the SAME record -> RMS$_RLK (98986).
 *   RAB$M_NLK               -- "no lock": no $ENQ at all, a dirty read that
 *                             never blocks and is never blocked.
 *   RAB$M_RLK                -- "read locked record": read the record EVEN IF
 *                             another stream holds it locked, without taking
 *                             a lock of its own. Realized as a throwaway
 *                             EX/NOQUEUE PROBE: refused (SS$_NOTQUEUED) means
 *                             the record IS genuinely locked right now ->
 *                             RMS$_OK_RLK (98337); granted means it was free
 *                             -> immediately $DEQ'd (RLK holds nothing).
 *
 * WHAT THIS PROVES, against the real-VAX ODS-2 fixture the harness mounts
 * WRITABLE on VDA0: (same fixture test_syssvc_rms_filelock.c drives), through
 * the public RMS system services + a direct GETLKI (vms_kif_getlki_parent) on
 * the lkid the RAB stashes internally (RAB._rec_lock_lkid, exposed here via
 * the private rms/rab.h struct definition every RMS test already includes):
 *
 *   1. DEFAULT-vs-DEFAULT (the core conflict): stream #1's default $get of a
 *      record holds a REAL granted EX lock whose GETLKI parent_id equals the
 *      file-access lock (the record held UNDER its file lock, vms-0dd half a).
 *      Stream #2's default $get of the SAME record -> RMS$_RLK from a real
 *      $ENQ+NOQUEUE conflict, and stashes no lock of its own.
 *   2. RAB$M_RLK read-through: while #1 still holds the record EX, a RAB$M_RLK
 *      $get of the SAME record by #2 SUCCEEDS and reports RMS$_OK_RLK,
 *      deciding that by REAL DLM probe state, not a guess -- and stashes no
 *      lock of its own either (RLK holds nothing).
 *   3. RAB$M_NLK: a dirty $get of the SAME still-EX-locked record ALSO
 *      succeeds (RMS$_NORMAL, no $ENQ at all) -- the direct contrast against
 *      the default $get's RMS$_RLK above, under the identical circumstance.
 *   4. $UPDATE/$DELETE: the holder (#1) updates then deletes the record it
 *      locked -- both succeed. A stream holding NO record lock (freshly
 *      reconnected #2) gets RMS$_CUR from both -- never a silent write to a
 *      record nobody here holds.
 *   5. RELEASE: sys$disconnect $DEQs #1's EX lock for real (GETLKI on the
 *      stale lkid -> SS$_IVLOCKID, not a stale "still granted"), and the
 *      SAME default $get that was just refused now succeeds.
 *
 * NO /dev/vms -> honest SKIP (77), never a fake pass (Rule 9).
 *
 * ISOLATION: a UNIQUE filename under [OVMXDIR], ERASED before exit (same
 * discipline as test_syssvc_rms_filelock.c / test_syssvc_rms_acp.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "rms/rms.h"
#include "rms_io.h"   /* rms_file_t + ->access_lkid: the file-access lock rms-50e stashes */

#define EXIT_SKIP  77
#define ODS2_UNIT  "VDA0:"
#define CELL_MRS   32
#define CELL_MRN   8

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

/* The file-access lkid rms_core.c (vms-50e) stashed on this FAB's internal
 * handle. 0 if the FAB has no accessed handle. */
static uint32_t fab_lkid(const struct FAB *fab)
{
    rms_file_t *h = (rms_file_t *)fab->_rms_file;
    return h ? h->access_lkid : 0;
}

static void put_cell(struct FAB *fab, uint32_t rrn, const char *data)
{
    struct RAB rab = cc$rms_rab;
    rab.rab$l_fab = fab;
    uint32_t st = sys$connect(&rab, 0, 0);
    check(st == RMS$_NORMAL, "  seed sys$connect -> NORMAL");

    rab.rab$b_rac = RAB$C_KEY;
    rab.rab$l_bkt = rrn;
    rab.rab$l_rbf = (char *)data;
    rab.rab$w_rsz = (uint16_t)strlen(data);
    st = sys$put(&rab, 0, 0);
    check(st == RMS$_NORMAL, "  seed sys$put(rrn) -> NORMAL");

    sys$disconnect(&rab, 0, 0);
}

static void test_reclock(void)
{
    char spec[128];
    struct FAB fab1, fab2, fab_seed;
    struct RAB rab1, rab2;
    uint32_t st, gm, gst, parent, lkid1_rec0;

    snprintf(spec, sizeof(spec), "%s[OVMXDIR]RMSRECLOCK.DAT", ODS2_UNIT);

    /* ---- $CREATE the relative fixture: 2 seeded cells, record 0 & 1. ---- */
    fab_seed = cc$rms_fab;
    fab_seed.fab$l_fna = spec;
    fab_seed.fab$b_fns = (uint8_t)strlen(spec);
    fab_seed.fab$b_org = FAB$C_REL;
    fab_seed.fab$b_rfm = FAB$C_FIX;
    fab_seed.fab$w_mrs = CELL_MRS;
    fab_seed.fab$l_mrn = CELL_MRN;
    fab_seed.fab$b_fac = FAB$M_PUT;
    fab_seed.fab$b_shr = 0;
    st = sys$create(&fab_seed, 0, 0);
    check(st == RMS$_NORMAL, "sys$create RMSRECLOCK.DAT (REL) -> NORMAL");
    if (st != RMS$_NORMAL) return;

    put_cell(&fab_seed, 0, "REC-A");
    put_cell(&fab_seed, 1, "REC-B");
    sys$close(&fab_seed, 0, 0);

    /* ---- two concurrent opens: fac=GET|PUT|UPD|DEL, shr=SHRPUT|SHRUPD
     * (=> CW per rms_fileshare_mode) so BOTH file-access locks coexist and
     * only RECORD-level arbitration is under test. ---- */
    fab1 = cc$rms_fab;
    fab1.fab$l_fna = spec;
    fab1.fab$b_fns = (uint8_t)strlen(spec);
    fab1.fab$b_org = FAB$C_REL;
    fab1.fab$b_rfm = FAB$C_FIX;
    fab1.fab$w_mrs = CELL_MRS;
    fab1.fab$l_mrn = CELL_MRN;
    fab1.fab$b_fac = FAB$M_GET | FAB$M_PUT | FAB$M_UPD | FAB$M_DEL;
    fab1.fab$b_shr = FAB$M_SHRPUT | FAB$M_SHRUPD;
    st = sys$open(&fab1, 0, 0);
    check(st == RMS$_NORMAL, "open#1 (CW) -> NORMAL");
    if (st != RMS$_NORMAL) return;

    fab2 = fab1;
    st = sys$open(&fab2, 0, 0);
    check(st == RMS$_NORMAL, "open#2 (CW) while open#1 held -> NORMAL (CW/CW file-level)");
    if (st != RMS$_NORMAL) { sys$close(&fab1, 0, 0); goto erase; }

    rab1 = cc$rms_rab; rab1.rab$l_fab = &fab1;
    st = sys$connect(&rab1, 0, 0);
    check(st == RMS$_NORMAL, "rab1 sys$connect -> NORMAL");

    rab2 = cc$rms_rab; rab2.rab$l_fab = &fab2;
    st = sys$connect(&rab2, 0, 0);
    check(st == RMS$_NORMAL, "rab2 sys$connect -> NORMAL");

    /* ================================================================
     * 1. DEFAULT-vs-DEFAULT on record 0: the core conflict.
     * ================================================================ */
    char buf1[CELL_MRS + 1], buf2[CELL_MRS + 1];

    rab1.rab$b_rac = RAB$C_KEY; rab1.rab$l_bkt = 0;
    rab1.rab$l_rop = 0;                        /* default: locking read */
    rab1.rab$l_ubf = buf1; rab1.rab$w_usz = sizeof(buf1) - 1;
    st = sys$get(&rab1, 0, 0);
    check(st == RMS$_NORMAL, "rab1 default $get(rrn=0) -> NORMAL");
    check(rab1._rec_lock_lkid != 0, "rab1 stashed a real record lkid");
    lkid1_rec0 = rab1._rec_lock_lkid;

    gm = 0; parent = 0xFFFFFFFFu;
    gst = vms_kif_getlki_parent(lkid1_rec0, &gm, NULL, NULL, NULL, &parent);
    check(gst == SS$_NORMAL && gm == LCK_K_EXMODE,
          "GETLKI: rab1's record lock is a REAL granted EX lock");
    check(parent == fab_lkid(&fab1),
          "GETLKI: record lock's parent_id == the file-access lock (vms-0dd half a)");

    rab2.rab$b_rac = RAB$C_KEY; rab2.rab$l_bkt = 0;
    rab2.rab$l_rop = 0;                        /* default: locking read, from a 2nd stream */
    rab2.rab$l_ubf = buf2; rab2.rab$w_usz = sizeof(buf2) - 1;
    st = sys$get(&rab2, 0, 0);
    check(st == RMS$_RLK,
          "rab2 default $get(rrn=0) while rab1 holds EX -> RMS$_RLK (real $ENQ conflict)");
    check(rab2._rec_lock_lkid == 0, "rab2 stashed NO lock on the RMS$_RLK conflict");

    /* ================================================================
     * 2. RAB$M_RLK read-through of the SAME still-EX-locked record.
     * ================================================================ */
    rab2.rab$b_rac = RAB$C_KEY; rab2.rab$l_bkt = 0;
    rab2.rab$l_rop = RAB$M_RLK;
    rab2.rab$l_ubf = buf2; rab2.rab$w_usz = sizeof(buf2) - 1;
    st = sys$get(&rab2, 0, 0);
    check(st == RMS$_OK_RLK,
          "rab2 RAB$M_RLK $get(rrn=0) while rab1 holds EX -> RMS$_OK_RLK (read-through)");
    /* REL $get always reads the FULL fixed cell (fab$w_mrs bytes, calloc-
     * padded by put_cell's seed write), not just the record's original
     * strlen -- rab$w_rsz == CELL_MRS is the correct shape here. */
    check(rab2.rab$w_rsz == CELL_MRS &&
          memcmp(rab2.rab$l_ubf, "REC-A", strlen("REC-A")) == 0,
          "rab2 RAB$M_RLK actually read the locked record's data");
    check(rab2._rec_lock_lkid == 0, "rab2 RAB$M_RLK stashed NO lock (RLK holds nothing)");

    /* ================================================================
     * 3. RAB$M_NLK dirty read of the SAME still-EX-locked record.
     * ================================================================ */
    rab2.rab$b_rac = RAB$C_KEY; rab2.rab$l_bkt = 0;
    rab2.rab$l_rop = RAB$M_NLK;
    rab2.rab$l_ubf = buf2; rab2.rab$w_usz = sizeof(buf2) - 1;
    st = sys$get(&rab2, 0, 0);
    check(st == RMS$_NORMAL,
          "rab2 RAB$M_NLK $get(rrn=0) while rab1 holds EX -> NORMAL (no $ENQ, never conflicts)");
    check(rab2._rec_lock_lkid == 0, "rab2 RAB$M_NLK stashed NO lock");

    /* ================================================================
     * 4. $UPDATE / $DELETE: holder succeeds, a lock-less stream is refused.
     *    rab1's default $get(rrn=1) below ALSO exercises the "next $get
     *    releases the prior record lock" release path on its rrn=0 lock.
     * ================================================================ */
    rab1.rab$b_rac = RAB$C_KEY; rab1.rab$l_bkt = 1;
    rab1.rab$l_rop = 0;
    rab1.rab$l_ubf = buf1; rab1.rab$w_usz = sizeof(buf1) - 1;
    st = sys$get(&rab1, 0, 0);
    check(st == RMS$_NORMAL, "rab1 default $get(rrn=1) -> NORMAL (releases rrn=0's lock first)");
    check(rab1._rec_lock_lkid != 0, "rab1 holds a fresh record lkid on rrn=1");
    uint32_t lkid1_rec1 = rab1._rec_lock_lkid;

    gm = 0;
    gst = vms_kif_getlki_parent(lkid1_rec0, &gm, NULL, NULL, NULL, NULL);
    check(gst == SS$_IVLOCKID,
          "GETLKI on rab1's rrn=0 lkid after its $get(rrn=1) -> SS$_IVLOCKID "
          "(the next $get released the prior record lock)");

    static const char newdata[] = "REC-B-UPDATED";
    rab1.rab$l_rbf = (char *)newdata;
    rab1.rab$w_rsz = (uint16_t)strlen(newdata);
    st = sys$update(&rab1, 0, 0);
    check(st == RMS$_NORMAL, "rab1 (holder) sys$update(rrn=1) -> NORMAL");

    st = sys$delete(&rab1, 0, 0);
    check(st == RMS$_NORMAL, "rab1 (holder) sys$delete(rrn=1) -> NORMAL");

    /* rab2 currently holds a RECORD 0 lock from step 1 -- drop it via
     * disconnect/reconnect so it holds NONE, then prove the gate. */
    sys$disconnect(&rab2, 0, 0);
    rab2 = cc$rms_rab; rab2.rab$l_fab = &fab2;
    st = sys$connect(&rab2, 0, 0);
    check(st == RMS$_NORMAL, "rab2 reconnect (fresh, no record lock) -> NORMAL");

    rab2.rab$l_rbf = (char *)"X"; rab2.rab$w_rsz = 1;
    st = sys$update(&rab2, 0, 0);
    check(st == RMS$_CUR, "rab2 (no held record lock) sys$update -> RMS$_CUR");
    st = sys$delete(&rab2, 0, 0);
    check(st == RMS$_CUR, "rab2 (no held record lock) sys$delete -> RMS$_CUR");

    /* ================================================================
     * 5. RELEASE via sys$disconnect: $DEQs rab1's rrn=1 lock for real
     *    (GETLKI -> SS$_IVLOCKID, never a stale "still granted"). Separately,
     *    rab2's default $get(rrn=0) -- refused with RMS$_RLK back in step 1
     *    while rab1 held it -- now succeeds, since that lock released back
     *    in step 4's "next $get" transition.
     * ================================================================ */
    sys$disconnect(&rab1, 0, 0);
    check(rab1._rec_lock_lkid == 0, "rab1 disconnect cleared its stashed lkid");

    gm = 0;
    gst = vms_kif_getlki_parent(lkid1_rec1, &gm, NULL, NULL, NULL, NULL);
    check(gst == SS$_IVLOCKID,
          "GETLKI on rab1's rrn=1 lkid after sys$disconnect -> SS$_IVLOCKID "
          "(a real release, not a stale grant)");

    rab2.rab$b_rac = RAB$C_KEY; rab2.rab$l_bkt = 0;
    rab2.rab$l_rop = 0;
    rab2.rab$l_ubf = buf2; rab2.rab$w_usz = sizeof(buf2) - 1;
    st = sys$get(&rab2, 0, 0);
    check(st == RMS$_NORMAL,
          "rab2 default $get(rrn=0), refused earlier with RMS$_RLK, now succeeds");

    sys$disconnect(&rab2, 0, 0);
    sys$close(&fab1, 0, 0);
    sys$close(&fab2, 0, 0);

erase:
    fab_seed = cc$rms_fab;
    fab_seed.fab$l_fna = spec;
    fab_seed.fab$b_fns = (uint8_t)strlen(spec);
    sys$erase(&fab_seed, 0, 0);
}

int main(void)
{
    uint32_t st;

    printf("=== test_syssvc_rms_reclock (RMS record-level locking behind "
           "the real DLM, vms-0dd) ===\n");

    if (!executive_present()) {
        printf("  SKIP: no /dev/vms -- nothing to arbitrate without a real "
               "ACP + lock manager (Rule 9).\n");
        return EXIT_SKIP;
    }

    st = vms_kif_acp_mount(ODS2_UNIT);   /* idempotent */
    check($VMS_STATUS_SUCCESS(st), "VDA0: mounted executive-global for RMS");

    test_reclock();

    vms_kif_acp_dmount(ODS2_UNIT);

    printf("=== RMS record-lock: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
