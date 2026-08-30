/*
 * test_syssvc_rms_filelock.c - RMS file-level share arbitration behind the
 * REAL DLM (vms-50e, docs/design-rms-file-lock.md). The DLM engine's first
 * real client: RMS's FAB access/share intent (fab$b_fac/fab$b_shr) now
 * reaches the executive lock manager at sys$open/sys$create via a file-access
 * $ENQ named by the file's FID, and sys$close $DEQs it.
 *
 * WHAT THIS PROVES, against the real-VAX ODS-2 fixture the harness mounts
 * WRITABLE on DKA0: (same fixture test_syssvc_rms_acp.c drives), through the
 * public RMS system services + a direct GETLKI on the lock ID rms_core.c
 * stashes on the internal handle (rms_file_t.access_lkid, exposed here via
 * the private rms_io.h -- the same header test_syssvc_rms_p3_acp.c already
 * includes for its own internal-handle access):
 *
 *   1. CONFLICTING SHARE: open #1 creates the file fac=PUT, shr=0 (=> EX per
 *      rms_fileshare_mode) and GETLKI confirms a REAL granted EX lock on the
 *      FID resource. open #2 (fac=GET, shr=0 => PR) on the SAME file, while
 *      #1 is still open, gets RMS$_SHR -- from a real $ENQ+NOQUEUE conflict
 *      (SS$_NOTQUEUED), not a userspace flag -- and its FAB is left with no
 *      handle. Closing #1 $DEQs its lock (GETLKI on the stale lkid then
 *      returns SS$_IVLOCKID, never stale "still granted" info), and the
 *      SAME open that was just denied now succeeds.
 *   2. COMPATIBLE SHARE: two opens of the SAME file, both fac=GET shr=SHRGET
 *      (=> PR per the table), coexist -- BOTH open at once, each holding its
 *      OWN distinct lkid, GETLKI showing BOTH still granted PR concurrently.
 *
 * Two FAB opens in ONE process against the real single-node lock manager is
 * the natural test (the design note's own words) -- it is the SAME executive
 * lock manager the cluster DLM extends, just not exercised cross-node here.
 *
 * NO /dev/vms -> honest SKIP (77), never a fake pass (Rule 9): with no ACP
 * there is no file to open and no lock manager to arbitrate anything, so
 * there is nothing to assert.
 *
 * ISOLATION: every file is created under a UNIQUE name in [OVMXDIR] and
 * ERASED before exit (same discipline as test_syssvc_rms_acp.c).
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
#include "rms_io.h"   /* rms_file_t + ->access_lkid: the internal handle vms-50e stashes the lkid on */

#define EXIT_SKIP  77
#define ODS2_UNIT  "DKA0:"

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

/* Read the file-access lkid rms_core.c stashed on this FAB's internal handle.
 * 0 if the FAB has no accessed handle (a failed/closed open). */
static uint32_t fab_lkid(const struct FAB *fab)
{
    rms_file_t *h = (rms_file_t *)fab->_rms_file;
    return h ? h->access_lkid : 0;
}

/* ================================================================
 * Test A: conflicting share -> a real DLM $ENQ conflict returns RMS$_SHR;
 * closing the holder $DEQs it and the same open then succeeds.
 * ================================================================ */
static void test_conflicting_share(void)
{
    char spec[128];
    struct FAB fab1, fab2;
    uint32_t st, gm, gst, lkid1;

    snprintf(spec, sizeof(spec), "%s[OVMXDIR]RMSLOCKA.DAT", ODS2_UNIT);

    /* open #1: fac=PUT, shr=0 -- write with no sharing at all => EX. */
    fab1 = cc$rms_fab;
    fab1.fab$l_fna = spec;
    fab1.fab$b_fns = (uint8_t)strlen(spec);
    fab1.fab$b_org = FAB$C_SEQ;
    fab1.fab$b_rfm = FAB$C_STMLF;
    fab1.fab$b_fac = FAB$M_PUT;
    fab1.fab$b_shr = 0;
    st = sys$create(&fab1, 0, 0);
    check(st == RMS$_NORMAL, "open#1 sys$create fac=PUT shr=0 (EX) -> NORMAL");
    if (st != RMS$_NORMAL) return;

    lkid1 = fab_lkid(&fab1);
    check(lkid1 != 0, "open#1 holds a real file-access lkid");

    gm = 0;
    gst = vms_kif_getlki(lkid1, &gm, NULL, NULL, NULL);
    check(gst == SS$_NORMAL && gm == LCK_K_EXMODE,
          "GETLKI: open#1's lock is a REAL granted EX lock on the FID resource");

    /* open #2: fac=GET, shr=0 -- strict reader => PR. PR vs EX conflicts. */
    fab2 = cc$rms_fab;
    fab2.fab$l_fna = spec;
    fab2.fab$b_fns = (uint8_t)strlen(spec);
    fab2.fab$b_org = FAB$C_SEQ;
    fab2.fab$b_rfm = FAB$C_STMLF;
    fab2.fab$b_fac = FAB$M_GET;
    fab2.fab$b_shr = 0;
    st = sys$open(&fab2, 0, 0);
    check(st == RMS$_SHR,
          "open#2 (PR) while open#1 (EX) is held -> RMS$_SHR from a real $ENQ conflict");
    check(fab2._rms_file == NULL, "open#2 retains NO handle after the conflict");

    /* close #1: $DEQs the EX lock for real. */
    st = sys$close(&fab1, 0, 0);
    check(st == RMS$_NORMAL, "close#1 -> NORMAL");

    gm = 0;
    gst = vms_kif_getlki(lkid1, &gm, NULL, NULL, NULL);
    check(gst == SS$_IVLOCKID,
          "GETLKI on open#1's lkid after close -> SS$_IVLOCKID (a real release, not a stale grant)");

    /* The same open that was just denied now succeeds -- the conflict was the
     * lock, not a permanent refusal. */
    fab2 = cc$rms_fab;
    fab2.fab$l_fna = spec;
    fab2.fab$b_fns = (uint8_t)strlen(spec);
    fab2.fab$b_org = FAB$C_SEQ;
    fab2.fab$b_rfm = FAB$C_STMLF;
    fab2.fab$b_fac = FAB$M_GET;
    fab2.fab$b_shr = 0;
    st = sys$open(&fab2, 0, 0);
    check(st == RMS$_NORMAL,
          "open#2 (PR) now succeeds once open#1's EX lock is released");
    if (st == RMS$_NORMAL) sys$close(&fab2, 0, 0);

    fab1 = cc$rms_fab;
    fab1.fab$l_fna = spec;
    fab1.fab$b_fns = (uint8_t)strlen(spec);
    sys$erase(&fab1, 0, 0);
}

/* ================================================================
 * Test B: compatible share -> two PR opens of the SAME file coexist, each
 * holding its own distinct real lock, both still granted concurrently.
 * ================================================================ */
static void test_compatible_share(void)
{
    char spec[128];
    struct FAB fab_seed, fab1, fab2;
    uint32_t st, gm1, gm2, gst, lkid1, lkid2;

    snprintf(spec, sizeof(spec), "%s[OVMXDIR]RMSLOCKB.DAT", ODS2_UNIT);

    /* Seed the file (create, write nothing extra, close). */
    fab_seed = cc$rms_fab;
    fab_seed.fab$l_fna = spec;
    fab_seed.fab$b_fns = (uint8_t)strlen(spec);
    fab_seed.fab$b_org = FAB$C_SEQ;
    fab_seed.fab$b_rfm = FAB$C_STMLF;
    fab_seed.fab$b_fac = FAB$M_PUT;
    fab_seed.fab$b_shr = 0;
    st = sys$create(&fab_seed, 0, 0);
    check(st == RMS$_NORMAL, "seed sys$create RMSLOCKB.DAT -> NORMAL");
    sys$close(&fab_seed, 0, 0);
    if (st != RMS$_NORMAL) return;

    /* open #1: fac=GET, shr=SHRGET -- tolerant-of-nothing-extra strict
     * reader per the table (SHRGET alone does not set SHRPUT/SHRUPD) => PR. */
    fab1 = cc$rms_fab;
    fab1.fab$l_fna = spec;
    fab1.fab$b_fns = (uint8_t)strlen(spec);
    fab1.fab$b_org = FAB$C_SEQ;
    fab1.fab$b_rfm = FAB$C_STMLF;
    fab1.fab$b_fac = FAB$M_GET;
    fab1.fab$b_shr = FAB$M_SHRGET;
    st = sys$open(&fab1, 0, 0);
    check(st == RMS$_NORMAL, "open#1 (GET+SHRGET, PR) -> NORMAL");
    if (st != RMS$_NORMAL) return;
    lkid1 = fab_lkid(&fab1);
    check(lkid1 != 0, "open#1 holds a real file-access lkid");

    /* open #2: identical intent, SAME file, while #1 is still open. */
    fab2 = cc$rms_fab;
    fab2.fab$l_fna = spec;
    fab2.fab$b_fns = (uint8_t)strlen(spec);
    fab2.fab$b_org = FAB$C_SEQ;
    fab2.fab$b_rfm = FAB$C_STMLF;
    fab2.fab$b_fac = FAB$M_GET;
    fab2.fab$b_shr = FAB$M_SHRGET;
    st = sys$open(&fab2, 0, 0);
    check(st == RMS$_NORMAL,
          "open#2 (GET+SHRGET, PR) while open#1's PR is held -> NORMAL (PR/PR compatible)");
    if (st != RMS$_NORMAL) { sys$close(&fab1, 0, 0); return; }
    lkid2 = fab_lkid(&fab2);
    check(lkid2 != 0 && lkid2 != lkid1,
          "open#2 holds its OWN distinct lkid (two real locks, not one shared flag)");

    /* Both still concurrently granted PR -- the DLM, not a mutex. */
    gm1 = 0;
    gst = vms_kif_getlki(lkid1, &gm1, NULL, NULL, NULL);
    check(gst == SS$_NORMAL && gm1 == LCK_K_PRMODE,
          "GETLKI: open#1's lock still granted PR while open#2's PR coexists");

    gm2 = 0;
    gst = vms_kif_getlki(lkid2, &gm2, NULL, NULL, NULL);
    check(gst == SS$_NORMAL && gm2 == LCK_K_PRMODE,
          "GETLKI: open#2's lock is a REAL, independently granted PR lock");

    sys$close(&fab1, 0, 0);
    sys$close(&fab2, 0, 0);

    fab_seed = cc$rms_fab;
    fab_seed.fab$l_fna = spec;
    fab_seed.fab$b_fns = (uint8_t)strlen(spec);
    sys$erase(&fab_seed, 0, 0);
}

int main(void)
{
    uint32_t st;

    printf("=== test_syssvc_rms_filelock (RMS file-level share arbitration "
           "behind the real DLM, vms-50e) ===\n");

    if (!executive_present()) {
        printf("  SKIP: no /dev/vms -- nothing to arbitrate without a real "
               "ACP + lock manager (Rule 9).\n");
        return EXIT_SKIP;
    }

    st = vms_kif_acp_mount(ODS2_UNIT);   /* idempotent */
    check($VMS_STATUS_SUCCESS(st), "DKA0: mounted executive-global for RMS");

    test_conflicting_share();
    test_compatible_share();

    vms_kif_acp_dmount(ODS2_UNIT);

    printf("=== RMS file-lock: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
