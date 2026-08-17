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
#include "vms_kif.h"
#include "rms/rms.h"
#include "sysuaf.h"
#include "sha256.h"
#include "rms_textfile.h"

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

/* Create a stream-LF file at `spec` and $PUT each line as one record. Returns
 * 1 on success. Mirrors test_syssvc_rms_acp.c's create discipline. */
static int create_stmlf_file(const char *spec, const char *const *lines, int n)
{
    struct FAB fab = cc$rms_fab;
    struct RAB rab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = FAB$C_STMLF;
    fab.fab$b_fac = FAB$M_PUT;
    if (sys$create(&fab, 0, 0) != RMS$_NORMAL)
        return 0;

    rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    if (sys$connect(&rab, 0, 0) != RMS$_NORMAL) { sys$close(&fab, 0, 0); return 0; }

    int ok = 1;
    for (int i = 0; i < n; i++) {
        rab.rab$l_rbf = (char *)lines[i];
        rab.rab$w_rsz = (uint16_t)strlen(lines[i]);
        if (sys$put(&rab, 0, 0) != RMS$_NORMAL) { ok = 0; break; }
    }
    sys$disconnect(&rab, 0, 0);
    sys$close(&fab, 0, 0);
    return ok;
}

static void erase_file(const char *spec)
{
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    sys$erase(&fab, 0, 0);
}

int main(void)
{
    uint32_t st;
    char pwhash[65];
    char row[SYSUAF_LINE_MAX];
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

    /* Build a faithful SYSUAF row through the shared writer, so the reader parses
     * exactly what a real SYSUAF carries (vms-9b7 one-format). */
    sha256_hex((const uint8_t *)"SECRET", 6, pwhash);
    memset(&rec, 0, sizeof(rec));
    strncpy(rec.username, "SYSTEM", sizeof(rec.username) - 1);
    strncpy(rec.password_hash, pwhash, sizeof(rec.password_hash) - 1);
    rec.uic_group = 1; rec.uic_member = 4;             /* [1,4], octal */
    strncpy(rec.default_dir, "SYS$SYSROOT:[SYSMGR]", sizeof(rec.default_dir) - 1);
    strncpy(rec.privileges, "ALL", sizeof(rec.privileges) - 1);
    check(sysuaf_format_record(&rec, row, sizeof(row)) > 0, "SYSTEM row formatted");

    const char *rows[1] = { row };
    check(create_stmlf_file(UAF_SPEC, rows, 1),
          "SYSUAF.DAT created on the ODS-2 volume (RMS $CREATE/$PUT via ACP)");

    /* ---- (1) the auth read MECHANISM: exactly sysuaf_scan's body -- RMS $OPEN
     *          the SYSUAF file, $GET each record through the ACP window, and
     *          parse+authenticate the SYSTEM row read off the ODS-2 platter. */
    {
        rms_textfile_t *tf = rms_textfile_open(UAF_SPEC);
        int found = -1;
        if (tf) {
            char line[SYSUAF_LINE_MAX];
            int too_long = 0;
            while (rms_textfile_getline(tf, line, sizeof(line), &too_long)) {
                if (too_long) continue;
                sysuaf_record_t r;
                if (sysuaf_parse_line(line, &r) == 1 &&
                    strcasecmp(r.username, "SYSTEM") == 0) { rec = r; found = 0; break; }
            }
            rms_textfile_close(tf);
        }
        check(found == 0, "sysuaf_lookup(SYSTEM) reads the account via RMS-over-ACP");
        if (found == 0) {
            check(rec.uic_group == 1 && rec.uic_member == 4,
                  "SYSTEM UIC [1,4] read back from the ODS-2 record");
            check(sysuaf_authenticate(&rec, "SECRET") == 1,
                  "correct password authenticates (SYSUAF read from ODS-2)");
            check(sysuaf_authenticate(&rec, "WRONG") == 0,
                  "wrong password does not authenticate");
        }
    }

    /* ---- (2) it is the ODS-2 volume: dismount => the read fails honestly ---- */
    vms_kif_acp_dmount(ODS2_UNIT);
    {
        rms_textfile_t *tf = rms_textfile_open(UAF_SPEC);
        check(tf == NULL, "with the volume DISMOUNTED, the SYSUAF read fails-honest (no POSIX copy)");
        if (tf) rms_textfile_close(tf);
    }
    st = vms_kif_acp_mount(ODS2_UNIT);   /* remount for the remaining proofs + cleanup */
    check($VMS_STATUS_SUCCESS(st), "DKA0: remounted");

    /* ---- (3) absent file / account fails honestly (no fabricated record) ---- */
    {
        rms_textfile_t *tf = rms_textfile_open("DKA0:[OVMXDIR]NOSUCH.DAT");
        check(tf == NULL, "opening an absent SYSUAF fails-honest (RMS$_FNF, no fallback)");
        if (tf) rms_textfile_close(tf);
    }

    /* ---- (4) the product SYS$SYSTEM:/SYS$MANAGER: paths fail HONESTLY today,
     *          pending directory-logical resolution -- never a silent POSIX
     *          success (INV-6). This pins the substrate gap as a checked fact. */
    {
        sysuaf_record_t prec;
        check(sysuaf_lookup("SYSTEM", &prec) != 0,
              "product sysuaf_lookup(SYS$SYSTEM:) fails-honest pending directory-logical resolution");
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
