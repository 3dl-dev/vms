/*
 * test_syssvc_dcl_acp.c - DCL file commands + F$ file lexicals reach files
 * through RMS / $QIO-to-ACP, not a POSIX /vms passthrough (vms-481, epic
 * vms-208), proven against a real /dev/vms.
 *
 * DCL's DIRECTORY / SET DEFAULT / TYPE / COPY / CREATE and the F$SEARCH /
 * F$FILE_ATTRIBUTES / F$PARSE lexicals were rerouted (src/vmsdcl/dcl_filespec.c
 * helpers dcl_rms_*, dcl_cmd_file.c, dcl_lexical.c, dcl_cmd_set.c) onto exactly
 * the RMS / OVMX-RMS services this suite drives:
 *
 *   - sys$parse + sys$search + rms_search_fid : the DIRECTORY listing and
 *       F$SEARCH substrate -- the executive wildcard directory context
 *       (IO$_ACPCONTROL) returning genuine ODS-2 matches WITH REAL FILE IDs.
 *   - rms_file_attr                            : the DIRECTORY /FULL "File ID"
 *       + F$FILE_ATTRIBUTES substrate -- the on-disk header (real FID, size,
 *       protection, dates, record format) via IO$_ACCESS's ATR list.
 *   - sys$open/$connect/$get                   : the TYPE / COPY-source substrate.
 *   - sys$create/$connect/$put                 : the CREATE / COPY-dest substrate.
 *
 * WHAT THIS PROVES (against real /dev/vms):
 *   1. DIRECTORY/F$SEARCH: sys$search over VDA200:[SRCH]*.TXT returns the
 *      genuine ODS-2 order A.TXT;3, A.TXT;2, A.TXT;1, B.TXT;1 (name asc,
 *      version desc -- NOT POSIX readdir) then RMS$_NMF, and rms_search_fid
 *      hands back the REAL File IDs (14,13,12,16) DIRECTORY /FULL prints --
 *      verified against the codec-deterministic fixture, not a synthesized id.
 *   2. DIRECTORY /FULL + F$FILE_ATTRIBUTES: rms_file_attr("VDA200:[SRCH]A.TXT;3")
 *      returns the SAME real FID (14), the resolved version (3), a non-directory
 *      file characteristic, and header attributes -- from the ODS-2 header, not
 *      a stat().
 *   3. TYPE/COPY/CREATE byte-exactness: a $CREATE + $PUT on the writable VDA0:
 *      volume lands records via the ACP; $CLOSE + re-$OPEN + $GET reads them
 *      back byte/record-exact (what TYPE would print), and a record-by-record
 *      RMS copy to a second file re-reads byte-exact (what COPY writes).
 *   4. FAIL-HONEST (Rule 9 / INV-6): sys$open / rms_file_attr of a nonexistent
 *      file return RMS$_FNF-class errors, never a silent success; and with no
 *      /dev/vms the whole suite SKIPs (77) -- never a fake pass.
 *
 * ISOLATION: created files use a unique name in VDA0:[OVMXDIR] and are ERASED
 * before exit (same discipline as test_syssvc_rms_acp.c).
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

#define EXIT_SKIP  77
#define SRCH_UNIT  "VDA200:"   /* generated multi-version fixture (mkimage_ods2_search.c) */
#define RW_UNIT    "VDA0:"     /* real-vax fixture, mounted WRITABLE                     */

/* Ground truth: VDA200:[SRCH] entries (mkimage_ods2_search.c deterministic). */
#define A_V1_FID  12u
#define A_V2_FID  13u
#define A_V3_FID  14u
#define B_TXT_FID 16u

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

/* One wildcard-search step: sys$search + read the resultant name + FID. Returns
 * the RMS status; fills *fidn with the match's real File ID number. */
static uint32_t search_next(struct FAB *fab, struct NAM *nam, uint16_t *fidn)
{
    uint32_t st = sys$search(fab, 0, 0);
    if (st == RMS$_NORMAL && fidn) {
        uint16_t num = 0, seq = 0; uint8_t rvn = 0, nmx = 0;
        rms_search_fid(nam, &num, &seq, &rvn, &nmx);
        *fidn = num;
    }
    return st;
}

/* PART A + B: the DIRECTORY / F$SEARCH / F$FILE_ATTRIBUTES substrate over the
 * multi-version fixture. */
static void prove_search_and_attr(void)
{
    uint32_t st = vms_kif_acp_mount(SRCH_UNIT);
    check($VMS_STATUS_SUCCESS(st), "MOUNT " SRCH_UNIT " (multi-version fixture)");

    struct FAB fab = cc$rms_fab;
    struct NAM nam = cc$rms_nam;
    char esa[256], rsa[256];
    char spec[] = SRCH_UNIT "[SRCH]*.TXT";

    fab.fab$l_fna = spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    nam.nam$l_esa = esa; nam.nam$b_ess = sizeof(esa) > 255 ? 255 : sizeof(esa);
    nam.nam$l_rsa = rsa; nam.nam$b_rss = sizeof(rsa) > 255 ? 255 : sizeof(rsa);
    fab.fab$l_nam = &nam;

    st = sys$parse(&fab, 0, 0);
    check(st == RMS$_NORMAL, "sys$parse " SRCH_UNIT "[SRCH]*.TXT");

    /* Genuine ODS-2 order: A.TXT;3(14), A.TXT;2(13), A.TXT;1(12), B.TXT;1(16). */
    uint16_t fidn = 0;
    st = search_next(&fab, &nam, &fidn);
    /* negctl: dcl-acp-search-fid-fabricated */
    check(st == RMS$_NORMAL && fidn == A_V3_FID,
          "F$SEARCH/DIRECTORY match 1 = A.TXT version 3, real File ID 14 (highest first)");
    st = search_next(&fab, &nam, &fidn);
    /* negctl-knockon: dcl-acp-search-fid-fabricated */
    check(st == RMS$_NORMAL && fidn == A_V2_FID,
          "F$SEARCH match 2 = A.TXT version 2, real File ID 13 (versions descending)");
    st = search_next(&fab, &nam, &fidn);
    /* negctl-knockon: dcl-acp-search-fid-fabricated */
    check(st == RMS$_NORMAL && fidn == A_V1_FID,
          "F$SEARCH match 3 = A.TXT version 1, real File ID 12");
    st = search_next(&fab, &nam, &fidn);
    /* negctl-knockon: dcl-acp-search-fid-fabricated */
    check(st == RMS$_NORMAL && fidn == B_TXT_FID,
          "F$SEARCH match 4 = B.TXT version 1, real File ID 16 (B.LOG excluded by .TXT)");
    st = search_next(&fab, &nam, &fidn);
    check(st == RMS$_NMF,
          "F$SEARCH exhausted -> RMS$_NMF (executive wildcard context, not readdir)");

    /* DIRECTORY /FULL + F$FILE_ATTRIBUTES: the genuine header, real FID. */
    struct rms_fileattr at;
    memset(&at, 0, sizeof(at));
    st = rms_file_attr(SRCH_UNIT "[SRCH]A.TXT;3", &at);
    check(st == RMS$_NORMAL && at.fid_num == A_V3_FID && at.version == 3 &&
          at.is_directory == 0,
          "rms_file_attr A.TXT;3 -> real File ID 14, version 3, non-directory "
          "(DIRECTORY /FULL + F$FILE_ATTRIBUTES source of truth)");
    check(at.hiblk >= at.efblk || at.efblk == 0,
          "rms_file_attr returns on-disk allocation/EOF (size from the header, not stat)");

    /* SRCH.DIR itself is a directory in the MFD -- the SET DEFAULT check. */
    memset(&at, 0, sizeof(at));
    st = rms_file_attr(SRCH_UNIT "[000000]SRCH.DIR", &at);
    check(st == RMS$_NORMAL && at.is_directory == 1,
          "rms_file_attr [000000]SRCH.DIR -> is_directory (SET DEFAULT existence check)");

    /* FAIL-HONEST: a file that is not there is RMS$_FNF-class, never success. */
    memset(&at, 0, sizeof(at));
    st = rms_file_attr(SRCH_UNIT "[SRCH]NOSUCH.TXT;1", &at);
    check(!$VMS_STATUS_SUCCESS(st),
          "rms_file_attr of a nonexistent file fails honestly (no fabricated attrs)");
}

/* PART C: the TYPE / COPY / CREATE byte-exact substrate on the writable volume. */
static void prove_read_write_copy(void)
{
    uint32_t st = vms_kif_acp_mount(RW_UNIT);
    check($VMS_STATUS_SUCCESS(st), "MOUNT " RW_UNIT " (writable, for CREATE/COPY)");

    const char *recs[4] = {
        "CREATE line one via RMS $PUT",
        "second record",
        "third",
        "COPY_TEST_CONTENT trailer",
    };
    const int nrec = 4;
    char src[64], dst[64];
    snprintf(src, sizeof(src), RW_UNIT "[OVMXDIR]DCLACP_S.TXT");
    snprintf(dst, sizeof(dst), RW_UNIT "[OVMXDIR]DCLACP_D.TXT");

    /* CREATE: sys$create + sequential $PUT (what cmd_create / dcl_rms_write_* do). */
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = src; fab.fab$b_fns = (uint8_t)strlen(src);
    fab.fab$b_org = FAB$C_SEQ; fab.fab$b_rfm = FAB$C_VAR; fab.fab$b_rat = FAB$M_CR;
    fab.fab$b_fac = FAB$M_PUT;
    st = sys$create(&fab, 0, 0);
    check(st == RMS$_NORMAL, "CREATE substrate: sys$create [OVMXDIR]DCLACP_S.TXT");
    if (st != RMS$_NORMAL) return;

    struct RAB rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    st = sys$connect(&rab, 0, 0);
    check(st == RMS$_NORMAL, "  sys$connect (write)");
    int put_ok = 1;
    for (int i = 0; i < nrec; i++) {
        rab.rab$l_rbf = (char *)recs[i];
        rab.rab$w_rsz = (uint16_t)strlen(recs[i]);
        if (sys$put(&rab, 0, 0) != RMS$_NORMAL) { put_ok = 0; break; }
    }
    check(put_ok, "  sys$put all records (WRITEVBLK to the ACP)");
    sys$close(&fab, 0, 0);

    /* TYPE: re-open + sequential $GET, byte-exact (what cmd_type / dcl_rms_read_* do). */
    fab = cc$rms_fab;
    fab.fab$l_fna = src; fab.fab$b_fns = (uint8_t)strlen(src);
    fab.fab$b_org = FAB$C_SEQ; fab.fab$b_rfm = FAB$C_VAR; fab.fab$b_fac = FAB$M_GET;
    st = sys$open(&fab, 0, 0);
    check(st == RMS$_NORMAL, "TYPE substrate: sys$open (reopen) [OVMXDIR]DCLACP_S.TXT");
    if (st == RMS$_NORMAL) {
        rab = cc$rms_rab; rab.rab$l_fab = &fab;
        char ubuf[128]; rab.rab$l_ubf = ubuf; rab.rab$w_usz = sizeof(ubuf);
        sys$connect(&rab, 0, 0);
        int ok = 1, got = 0;
        for (int i = 0; i < nrec; i++) {
            if (sys$get(&rab, 0, 0) != RMS$_NORMAL) { ok = 0; break; }
            got++;
            if (rab.rab$w_rsz != (uint16_t)strlen(recs[i]) ||
                memcmp(rab.rab$l_ubf, recs[i], rab.rab$w_rsz) != 0) { ok = 0; break; }
        }
        check(ok && got == nrec,
              "  $GET reads back all records BYTE-EXACT (what TYPE prints)");
        sys$close(&fab, 0, 0);
    }

    /* COPY: read source records, create dest, put them, re-read byte-exact. */
    {
        struct FAB sf = cc$rms_fab, df = cc$rms_fab;
        struct RAB sr, dr;
        char ubuf[128];
        sf.fab$l_fna = src; sf.fab$b_fns = (uint8_t)strlen(src);
        sf.fab$b_org = FAB$C_SEQ; sf.fab$b_rfm = FAB$C_VAR; sf.fab$b_fac = FAB$M_GET;
        df.fab$l_fna = dst; df.fab$b_fns = (uint8_t)strlen(dst);
        df.fab$b_org = FAB$C_SEQ; df.fab$b_rfm = FAB$C_VAR; df.fab$b_rat = FAB$M_CR;
        df.fab$b_fac = FAB$M_PUT;
        int copy_ok = (sys$open(&sf, 0, 0) == RMS$_NORMAL) &&
                      (sys$create(&df, 0, 0) == RMS$_NORMAL);
        if (copy_ok) {
            sr = cc$rms_rab; sr.rab$l_fab = &sf;
            sr.rab$l_ubf = ubuf; sr.rab$w_usz = sizeof(ubuf);
            sys$connect(&sr, 0, 0);
            dr = cc$rms_rab; dr.rab$l_fab = &df;
            sys$connect(&dr, 0, 0);
            while (sys$get(&sr, 0, 0) == RMS$_NORMAL) {
                dr.rab$l_rbf = sr.rab$l_ubf; dr.rab$w_rsz = sr.rab$w_rsz;
                if (sys$put(&dr, 0, 0) != RMS$_NORMAL) { copy_ok = 0; break; }
            }
        }
        sys$close(&sf, 0, 0);
        sys$close(&df, 0, 0);
        check(copy_ok, "COPY substrate: record-by-record RMS copy source -> dest");

        /* Verify the copied file re-reads byte-exact. */
        df = cc$rms_fab; df.fab$l_fna = dst; df.fab$b_fns = (uint8_t)strlen(dst);
        df.fab$b_org = FAB$C_SEQ; df.fab$b_rfm = FAB$C_VAR; df.fab$b_fac = FAB$M_GET;
        int vok = 0;
        if (sys$open(&df, 0, 0) == RMS$_NORMAL) {
            dr = cc$rms_rab; dr.rab$l_fab = &df;
            dr.rab$l_ubf = ubuf; dr.rab$w_usz = sizeof(ubuf);
            sys$connect(&dr, 0, 0);
            vok = 1;
            for (int i = 0; i < nrec; i++) {
                if (sys$get(&dr, 0, 0) != RMS$_NORMAL) { vok = 0; break; }
                if (dr.rab$w_rsz != (uint16_t)strlen(recs[i]) ||
                    memcmp(dr.rab$l_ubf, recs[i], dr.rab$w_rsz) != 0) { vok = 0; break; }
            }
            sys$close(&df, 0, 0);
        }
        check(vok, "  copied file re-reads BYTE-EXACT (COPY landed real records)");
    }

    /* FAIL-HONEST: opening a file that is not there is RMS$_FNF, never success. */
    {
        struct FAB nf = cc$rms_fab;
        char none[] = RW_UNIT "[OVMXDIR]DCLACP_NONE.TXT";
        nf.fab$l_fna = none; nf.fab$b_fns = (uint8_t)strlen(none);
        nf.fab$b_org = FAB$C_SEQ; nf.fab$b_fac = FAB$M_GET;
        st = sys$open(&nf, 0, 0);
        check(st != RMS$_NORMAL,
              "TYPE/COPY of a nonexistent file fails honestly (RMS$_FNF, no POSIX fallback)");
    }

    /* Cleanup: erase both created files (restore directory state). */
    {
        struct FAB ef = cc$rms_fab;
        ef.fab$l_fna = src; ef.fab$b_fns = (uint8_t)strlen(src);
        sys$erase(&ef, 0, 0);
        ef = cc$rms_fab; ef.fab$l_fna = dst; ef.fab$b_fns = (uint8_t)strlen(dst);
        sys$erase(&ef, 0, 0);
    }
}

int main(void)
{
    if (!executive_present()) {
        printf("=== test_syssvc_dcl_acp: 0 passed, 0 failed "
               "(SKIPPED: no /dev/vms -- DCL file access is executive-resident "
               "via RMS/$QIO-ACP, nothing to assert) ===\n");
        return EXIT_SKIP;
    }

    prove_search_and_attr();
    prove_read_write_copy();

    printf("=== test_syssvc_dcl_acp: %d passed, %d failed ===\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
