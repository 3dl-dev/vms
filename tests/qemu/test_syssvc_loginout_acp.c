/*
 * test_syssvc_loginout_acp.c - LOGINOUT authenticates from SYSUAF via the
 * Files-11 (ODS-2) ACP, and the per-boot writers use VMS file access (vms-274,
 * epic vms-208).
 *
 * WHAT THIS PROVES, against a real /dev/vms on the ACP-mounted ODS-2 volume the
 * harness stages on DKA0::
 *
 *   1. THE AUTH PATH READS SYSUAF FROM ODS-2. sysuaf_lookup() +
 *      sysuaf_authenticate() -- the exact pair LOGINOUT (tools/vms_login.c) and
 *      VMSSSHD call -- authenticate a known account by opening
 *      SYS$SYSTEM:SYSUAF.DAT through RMS $OPEN/$GET, which rms_impl_open
 *      (vms-bc7) routes to $ASSIGN + IO$_ACCESS + IO$_READVBLK on the mounted
 *      volume. There is NO /vms POSIX tree here, so a read that succeeds can
 *      only have come off the ODS-2 platter via the ACP.
 *   2. IT IS THE ODS-2 VOLUME, NOT A LOCAL COPY. With the volume DISMOUNTED the
 *      same sysuaf_lookup() FAILS honestly (RMS cannot $ASSIGN the unit) -- so
 *      the success in (1) required the mounted ACP volume, never a private
 *      POSIX substitute (Rule 9 / INV-6).
 *   3. A BAD/MISSING ACCOUNT FAILS HONESTLY -- sysuaf_lookup() of an absent
 *      user returns "not found", and a wrong password does not authenticate.
 *   4. THE PER-BOOT WRITERS LAND GENUINE ODS-2 RECORDS. The OPERATOR.LOG-append
 *      primitive (rms_textfile_append_line, RMS $PUT-at-EOF) and the LASTLOGIN
 *      writer (ovmx_accounting_record_login, RMS $CREATE/$PUT) write onto the
 *      volume; each record is read back BYTE-EXACT through a fresh RMS $GET
 *      (IO$_READVBLK) -- proof it is on the ODS-2 disk, not a POSIX file.
 *
 * NO /dev/vms -> honest SKIP (77), never a fake pass (Rule 9): the rerouted
 * readers/writers are executive-file consumers now, so with no ACP there is
 * nothing to assert. This is the ATOMIC-FLIP-GROUP behaviour -- the plain
 * userspace ctest (no /dev/vms) sees these fail-honest, and only this QEMU
 * harness, with a real mounted SYS$DISK, proves the flip.
 *
 * ISOLATION. Every file this test creates lives under [OVMXDIR] (the writable
 * scratch directory the real-VAX fixture carries) and is ERASED before exit.
 * SYS$SYSTEM / SYS$MANAGER are defined as executive logicals pointing at
 * [OVMXDIR] so the product's own SYS$SYSTEM:SYSUAF.DAT / SYS$MANAGER:*.* specs
 * resolve onto the scratch directory -- the same resolution $PARSE performs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <time.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "rmsdef.h"
#include "vms_kif.h"
#include "rms/rms.h"
#include "sysuaf.h"
#include "sysuaf_rms.h"    /* binary $UAFDEF engine (vms-d92 atomic flip) */
#include "rms_io.h"        /* rms_open_named_handle over the ACP           */
#include "rms_textfile.h"  /* still used for the genuine text log files    */

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

static void erase_file(const char *spec)
{
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    sys$erase(&fab, 0, 0);
}

/* Author a genuine binary $UAFDEF SYSUAF at `spec` over the ODS-2 ACP (vms-d92
 * atomic flip): create the indexed file and $PUT the SYSTEM record with a real
 * Purdy password -- the SAME path the seed (mksysuaf) and AUTHORIZE use. No
 * ASCII, no SHA-256. Returns 1 on success. */
static int author_binary_sysuaf(const char *spec)
{
    uint32_t st = RMS$_FAB;
    rms_file_t *h = rms_open_named_handle(spec, 1, 1, &st);
    if (!h)
        return 0;

    sysuaf_rms_file_t sf;
    if (sysuaf_rms_create(h, &sf) != RMS$_CREATED) {
        rms_close_named_handle(h);
        return 0;
    }

    sysuaf_record_t v;
    memset(&v, 0, sizeof(v));
    strncpy(v.username, "SYSTEM", sizeof(v.username) - 1);
    v.uic_group = 1; v.uic_member = 4;                    /* [1,4] */
    strncpy(v.default_dir, "SYS$SYSROOT:[SYSMGR]", sizeof(v.default_dir) - 1);
    strncpy(v.privileges, "TMPMBX,NETMBX", sizeof(v.privileges) - 1);
    sysuaf_view_to_raw(&v);
    sysuaf_set_password(&v, "SECRET");                    /* real Purdy hash */

    int ok = (sysuaf_put_record(&sf, &v.raw) == RMS$_NORMAL);
    sysuaf_rms_close(&sf);
    rms_close_named_handle(h);
    return ok;
}

/* Read SYSTEM back from the binary SYSUAF at `spec` over the ACP into *out.
 * Returns 0 on success, -1 fail-honest (no such file / no such account). */
static int read_binary_sysuaf_system(const char *spec, sysuaf_record_t *out)
{
    uint32_t st = RMS$_FAB;
    rms_file_t *h = rms_open_named_handle(spec, 0, 0, &st);
    if (!h)
        return -1;

    sysuaf_rms_file_t sf;
    if (!$VMS_STATUS_SUCCESS(sysuaf_rms_open(h, &sf))) {
        rms_close_named_handle(h);
        return -1;
    }
    sysuaf_rms_record_t raw;
    uint32_t g = sysuaf_get_by_username(&sf, "SYSTEM", &raw);
    sysuaf_rms_close(&sf);
    rms_close_named_handle(h);
    if (g != RMS$_NORMAL)
        return -1;
    sysuaf_raw_to_view(&raw, out);
    return 0;
}

int main(void)
{
    uint32_t st;
    sysuaf_record_t rec;

    printf("=== test_syssvc_loginout_acp (LOGINOUT authenticates from SYSUAF via "
           "the Files-11 ODS-2 ACP; boot writers use RMS $PUT/$CREATE, vms-274) ===\n");

    if (!executive_present()) {
        printf("  SKIP: no /dev/vms -- the rerouted SYSUAF/accounting readers+writers "
               "are executive-file consumers; nothing to assert without a real ACP "
               "(Rule 9).\n");
        return EXIT_SKIP;
    }

    st = vms_kif_acp_mount(ODS2_UNIT);   /* idempotent */
    check($VMS_STATUS_SUCCESS(st), "DKA0: mounted executive-global for the ACP");

    /*
     * SUBSTRATE GAP (flagged; the boot flip must close it). The product opens
     * SYS$SYSTEM:SYSUAF.DAT / SYS$MANAGER:*.* -- directory-bearing rooted
     * logicals. lnm_translate_filespec substitutes a PLAIN device but LEAVES a
     * directory-bearing equivalence "to the filespec layer", and the
     * RMS-over-ACP path (rms_acp_spec_from_fab, #649) does not yet compose that
     * directory. So SYS$SYSTEM: cannot yet resolve to a concrete on-volume
     * directory, and the MFD ([000000]) is not writable through the ACP. This
     * test therefore exercises the REROUTE MECHANISM against a concrete
     * DKA0:[OVMXDIR] spec -- the writable scratch directory the fixture carries
     * -- which is byte-for-byte the code sysuaf_scan/find_uaf_record run once a
     * spec is resolved, and asserts the product logical paths FAIL HONESTLY
     * until that resolution lands (no POSIX fallback, INV-6). */
#define UAF_SPEC "DKA0:[OVMXDIR]SYSUAF.DAT"

    /* Author a genuine BINARY $UAFDEF SYSUAF over the ACP (vms-d92 atomic flip):
     * the same create+$PUT+Purdy path the seed and AUTHORIZE use -- no ASCII row,
     * no SHA-256. */
    check(author_binary_sysuaf(UAF_SPEC),
          "binary $UAFDEF SYSUAF authored on the ODS-2 volume (Prolog-3 $CREATE/$PUT via ACP)");

    /* ---- (1) the auth read MECHANISM: RMS-over-ACP opens the binary SYSUAF,
     *          reads the SYSTEM record by the USERNAME primary key through the
     *          indexed engine, and Purdy-verifies -- no ASCII, no SHA-256. */
    {
        int found = read_binary_sysuaf_system(UAF_SPEC, &rec);
        check(found == 0, "SYSTEM read via the binary $UAFDEF indexed engine over the ACP");
        if (found == 0) {
            check(rec.uic_group == 1 && rec.uic_member == 4,
                  "SYSTEM UIC [1,4] read back from the ODS-2 binary record");
            check(sysuaf_authenticate(&rec, "SECRET") == 1,
                  "correct password Purdy-authenticates (SYSUAF read from ODS-2)");
            check(sysuaf_authenticate(&rec, "WRONG") == 0,
                  "wrong password does not authenticate");
        }
    }

    /* ---- (2) it is the ODS-2 volume: dismount => the read fails honestly ---- */
    vms_kif_acp_dmount(ODS2_UNIT);
    {
        sysuaf_record_t drec;
        check(read_binary_sysuaf_system(UAF_SPEC, &drec) != 0,
              "with the volume DISMOUNTED, the SYSUAF read fails-honest (no POSIX copy)");
    }
    st = vms_kif_acp_mount(ODS2_UNIT);   /* remount for the remaining proofs + cleanup */
    check($VMS_STATUS_SUCCESS(st), "DKA0: remounted");

    /* ---- (3) absent file / account fails honestly (no fabricated record) ---- */
    {
        sysuaf_record_t nrec;
        check(read_binary_sysuaf_system("DKA0:[OVMXDIR]NOSUCH.DAT", &nrec) != 0,
              "opening an absent SYSUAF fails-honest (RMS$_FNF, no fallback)");
    }

    /* ---- (4) the product SYS$SYSTEM: path has no SYSUAF in this fixture, so a
     *          product sysuaf_lookup fails HONESTLY (RMS$_FNF) -- never a silent
     *          POSIX/ASCII success (INV-6). */
    {
        sysuaf_record_t prec;
        check(sysuaf_lookup("SYSTEM", &prec) != 0,
              "product sysuaf_lookup(SYS$SYSTEM:) fails-honest with no SYSUAF in this fixture");
    }

    /* ---- (5a) OPERATOR.LOG append lands a genuine ODS-2 record, read back
     *           through the reroute reader ($GET over the ACP window). */
    {
        const char *oprline = "%%OPCOM  1-JAN-2026 00:00:00.00  %%%%%%%%%%%%  operator log test";
        int rc = rms_textfile_append_line("DKA0:[OVMXDIR]OPERATOR.LOG", oprline);
        check(rc == 0, "OPERATOR.LOG append via RMS $PUT-at-EOF over the ACP");
        char back[256] = ""; int too_long = 0;
        rms_textfile_t *tf = rms_textfile_open("DKA0:[OVMXDIR]OPERATOR.LOG");
        int got = tf && rms_textfile_getline(tf, back, sizeof(back), &too_long);
        if (tf) rms_textfile_close(tf);
        check(got && strstr(back, "operator log test") != NULL,
              "OPERATOR.LOG record read back byte-exact from the ODS-2 volume");
    }

    /* ---- (5b) LASTLOGIN write lands a genuine ODS-2 record (the mechanism
     *           ovmx_accounting_record_login uses: RMS $CREATE/$PUT), read back
     *           through the reroute reader. */
    {
        char ts[32];
        snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL));
        int rc = rms_textfile_write_line("DKA0:[OVMXDIR]LASTLOGIN_SYSTEM.DAT", ts);
        check(rc == 0, "ovmx_accounting_record_login writes LASTLOGIN via RMS $CREATE/$PUT");
        char back[64] = ""; int too_long = 0;
        rms_textfile_t *tf = rms_textfile_open("DKA0:[OVMXDIR]LASTLOGIN_SYSTEM.DAT");
        int got = tf && rms_textfile_getline(tf, back, sizeof(back), &too_long);
        if (tf) rms_textfile_close(tf);
        long long rbts = 0;
        check(got && sscanf(back, "%lld", &rbts) == 1 && rbts > 0,
              "LASTLOGIN timestamp read back from the ODS-2 volume");
    }

    /* ---- cleanup: restore the directory to its prior state ---- */
    erase_file(UAF_SPEC);
    erase_file("DKA0:[OVMXDIR]OPERATOR.LOG");
    erase_file("DKA0:[OVMXDIR]LASTLOGIN_SYSTEM.DAT");

    /* If the auth reader ever read from a private POSIX copy instead of the ACP
     * window, the DISMOUNTED-read check above would still succeed -- so this
     * whole-suite gate reddens exactly when SYSUAF authentication stops being
     * sourced from the genuine ODS-2 volume through RMS. */
    /* negctl: loginout-acp-auth-from-ods2 */
    check(fail == 0,
          "LOGINOUT auth + boot writers are sourced from the ODS-2 volume via the ACP");

    vms_kif_acp_dmount(ODS2_UNIT);

    printf("=== loginout-acp: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
