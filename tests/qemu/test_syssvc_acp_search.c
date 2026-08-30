/*
 * test_syssvc_acp_search.c - the Files-11 (ODS-2) ACP answers IO$_ACPCONTROL
 * with a wildcard FIB$L_WCC directory context: the $SEARCH / DIRECTORY
 * primitive (vms-a0b, epic vms-208), proven against a real /dev/vms.
 *
 * This is the FIFTH rung of the executive ACP (after the kernel-resident codec
 * vms-dcd, the channel front-end vms-149, the executive-global $MOUNT vms-127,
 * and IO$_ACCESS/IO$_DEACCESS vms-204): the wildcard directory search RMS
 * $SEARCH and the DCL DIRECTORY / F$SEARCH lexical are layered on. On real
 * OpenVMS this is a $QIO(IO$_ACPCONTROL) on a channel $ASSIGNed to the mounted
 * volume, with a FIB whose FIB$V_WILD is set and whose FIB$L_WCC carries the
 * wildcard-continuation context across successive one-per-call matches, ending
 * SS$_NOMOREFILES when the directory is exhausted (VSI I/O User's Reference,
 * "ACP-QIO Interface"). OVMX reaches the executive over /dev/vms via the KIF
 * helper vms_kif_acp_acpcontrol(), driven here directly, exactly as
 * test_syssvc_acp_access drives vms_kif_acp_access().
 *
 * WHAT THIS SUITE PROVES, through the sys$/kif API against a real /dev/vms, over
 * a GENERATED multi-version ODS-2 volume the harness seeds on VDA200: (vdc, from
 * /ods2_search.img == tests/qemu/mkimage_ods2_search.c -- the real-VAX fixture
 * on VDA0: carries only single versions, so this proof needs a directory with a
 * name at several versions):
 *
 *   Directory [SRCH] (FID 11), on-disk records (name ascending, versions
 *   descending -- the genuine ODS-2 order the codec decodes, NOT POSIX readdir):
 *       A.TXT;3 (14)  A.TXT;2 (13)  A.TXT;1 (12)
 *       B.LOG;1 (15)  B.TXT;1 (16)  README.DAT;1 (17)
 *   (FIDs are mkimage_ods2_search.c's deterministic sequential allocation --
 *   see its header; cross-checked with the userspace codec ods2_bdev_list_dir.)
 *
 *   1. ALL-VERSIONS WILDCARD (*.TXT). A wildcard context returns EACH matching
 *      version, one per call, in ODS-2 order: A.TXT;3, A.TXT;2, A.TXT;1,
 *      B.TXT;1 (B.LOG excluded by the .TXT type), then SS$_NOMOREFILES -- the
 *      FIB$L_WCC continuation maintained across four calls.
 *   2. HIGHEST-OF-EACH-NAME (*.*;0). ;0 returns only the highest version of
 *      each name across all types: A.TXT;3, B.LOG;1, B.TXT;1, README.DAT;1,
 *      then SS$_NOMOREFILES.
 *   3. EXACT VERSION (A.TXT;2). ;2 matches only that one version (FID 13), then
 *      SS$_NOMOREFILES.
 *   4. NO MATCH (NOSUCH.*). A pattern matching nothing returns SS$_NOMOREFILES
 *      on the very first call.
 *   5. FAIL-HONEST. A search on an unassigned channel is SS$_IVCHAN; a
 *      "continue" on a channel with no open wildcard context is SS$_BADPARAM.
 *   6. THE FIDs ARE GENUINE. IO$_ACCESS by the FID the search returned for
 *      A.TXT;3 (14) opens that same on-disk file -- the search did not fabricate
 *      an identity (INV-6).
 *
 * NO /dev/vms -> honest SKIP (77), never a fake pass: the ACP and its wildcard
 * search are executive-resident, so with no /dev/vms there is nothing to assert
 * (the contract every test_syssvc_* suite is held to, .github/workflows/ci.yml).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

/* VDA200: (vdc) carries the generated multi-version ODS-2 volume. */
#define ODS2_UNIT  "VDA200:"

/* Ground truth: [SRCH] and its entries (mkimage_ods2_search.c deterministic). */
#define SRCH_FID_NUM   11u
#define A_V1_FID       12u
#define A_V2_FID       13u
#define A_V3_FID       14u
#define B_LOG_FID      15u
#define B_TXT_FID      16u
#define README_FID     17u
#define ODS2_FH2_M_DIRECTORY 0x2000u

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

/* One IO$_ACPCONTROL($SEARCH) step. reset==1 (re)opens the context with
 * `pattern`; reset==0 continues it. Returns args by value in *a. */
static uint32_t do_search(struct vms_acp_acpcontrol_args *a, uint32_t chan,
                          uint16_t did_num, int reset, const char *pattern)
{
    memset(a, 0, sizeof(*a));
    a->chan = chan;
    a->func = VMS_ACP_CTL_SEARCH;
    a->did_num = did_num;
    a->did_seq = 1;
    a->wcc_reset = reset ? 1 : 0;
    if (pattern)
        strncpy(a->pattern, pattern, VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_acpcontrol(a);
}

/* Build "NAME;VER" without a ';' literal in the caller's assertion text. */
static void mkresnam(char *buf, size_t sz, const char *nametype, unsigned ver)
{
    snprintf(buf, sz, "%s;%u", nametype, ver);
}

int main(void)
{
    struct vms_acp_access_args ac;
    struct vms_acp_acpcontrol_args a;
    uint32_t st, chan = 0, chan2 = 0, srch_fid = 0;
    char exp[VMS_ACP_RESNAM_SIZE];

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_acp_search: executive ACP answers IO$_ACPCONTROL "
           "wildcard directory search ($SEARCH primitive; vms-a0b, epic vms-208) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        printf("=== test_syssvc_acp_search: 0 passed, 0 failed "
               "(SKIPPED: no /dev/vms -- the ACP and its wildcard directory "
               "search are executive-resident) ===\n");
        return EXIT_SKIP;
    }

    /* Precondition: mount the generated multi-version volume, assign a channel. */
    st = vms_kif_acp_mount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st),
          "$MOUNT of the generated multi-version ODS-2 " ODS2_UNIT " (precondition)");
    st = vms_kif_acp_assign(ODS2_UNIT, &chan);
    check($VMS_STATUS_SUCCESS(st) && chan != 0,
          "$ASSIGN a file-class channel to " ODS2_UNIT " (precondition)");
    if (chan == 0) {
        printf("=== test_syssvc_acp_search: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    /* Resolve [SRCH] by name in the MFD -- the DID the searches run against. */
    memset(&ac, 0, sizeof(ac));
    ac.chan = chan;
    strncpy(ac.name, "SRCH.DIR", VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_access(&ac);
    check($VMS_STATUS_SUCCESS(st) && ac.fid_num == SRCH_FID_NUM &&
          (ac.attr.filechar & ODS2_FH2_M_DIRECTORY),
          "IO$_ACCESS resolves [000000]SRCH.DIR to its real directory FID (11)");
    srch_fid = ac.fid_num;
    (void)vms_kif_acp_deaccess(chan);
    if (srch_fid == 0)
        srch_fid = SRCH_FID_NUM;

    /* --- (1) ALL-VERSIONS wildcard "*.TXT" -- the multi-version iteration ---
     * A.TXT at 3 versions (descending) then B.TXT;1, then SS$_NOMOREFILES. */
    st = do_search(&a, chan, (uint16_t)srch_fid, 1, "*.TXT;*");
    check(st == SS$_NORMAL && a.fid_num == A_V3_FID && a.out_version == 3,
          "wildcard search *.TXT match 1 is A.TXT version 3 FID 14 (highest first)");
    mkresnam(exp, sizeof(exp), "A.TXT", 3);
    check(strcmp(a.resnam, exp) == 0,
          "  ... its resultant name string is the ODS-2 NAME.TYPE and version");

    st = do_search(&a, chan, (uint16_t)srch_fid, 0, NULL);
    /* negctl: acp-search-cursor-skips-versions */
    check(st == SS$_NORMAL && a.fid_num == A_V2_FID && a.out_version == 2,
          "wildcard search *.TXT match 2 is A.TXT version 2 FID 13 (versions descending within a name)");

    st = do_search(&a, chan, (uint16_t)srch_fid, 0, NULL);
    /* negctl-knockon: acp-search-cursor-skips-versions */
    check(st == SS$_NORMAL && a.fid_num == A_V1_FID && a.out_version == 1,
          "wildcard search *.TXT match 3 is A.TXT version 1 FID 12");

    st = do_search(&a, chan, (uint16_t)srch_fid, 0, NULL);
    /* negctl-knockon: acp-search-cursor-skips-versions */
    check(st == SS$_NORMAL && a.fid_num == B_TXT_FID && a.out_version == 1,
          "wildcard search *.TXT match 4 is B.TXT version 1 FID 16");

    st = do_search(&a, chan, (uint16_t)srch_fid, 0, NULL);
    check(st == SS$_NOMOREFILES,
          "wildcard search *.TXT is SS$_NOMOREFILES past the last match (context exhausted)");

    /* --- (2) HIGHEST-OF-EACH-NAME "*.*;0" across all types ---------------- */
    st = do_search(&a, chan, (uint16_t)srch_fid, 1, "*.*;0");
    check(st == SS$_NORMAL && a.fid_num == A_V3_FID && a.out_version == 3,
          "highest-version search *.* match 1 is A.TXT version 3 (only the highest of A.TXT)");
    st = do_search(&a, chan, (uint16_t)srch_fid, 0, NULL);
    check(st == SS$_NORMAL && a.fid_num == B_LOG_FID && a.out_version == 1,
          "highest-version search *.* match 2 is B.LOG version 1 (B.LOG sorts before B.TXT)");
    st = do_search(&a, chan, (uint16_t)srch_fid, 0, NULL);
    check(st == SS$_NORMAL && a.fid_num == B_TXT_FID && a.out_version == 1,
          "highest-version search *.* match 3 is B.TXT version 1");
    st = do_search(&a, chan, (uint16_t)srch_fid, 0, NULL);
    check(st == SS$_NORMAL && a.fid_num == README_FID && a.out_version == 1,
          "highest-version search *.* match 4 is README.DAT version 1");
    st = do_search(&a, chan, (uint16_t)srch_fid, 0, NULL);
    check(st == SS$_NOMOREFILES,
          "highest-version search *.* is SS$_NOMOREFILES past the last name");

    /* --- (3) EXACT VERSION "A.TXT;2" -------------------------------------- */
    st = do_search(&a, chan, (uint16_t)srch_fid, 1, "A.TXT;2");
    check(st == SS$_NORMAL && a.fid_num == A_V2_FID && a.out_version == 2,
          "exact-version search A.TXT version 2 returns only that version (FID 13)");
    st = do_search(&a, chan, (uint16_t)srch_fid, 0, NULL);
    check(st == SS$_NOMOREFILES,
          "exact-version search A.TXT version 2 is SS$_NOMOREFILES after its one match");

    /* --- (4) NO MATCH "NOSUCH.*" ------------------------------------------ */
    st = do_search(&a, chan, (uint16_t)srch_fid, 1, "NOSUCH.*;*");
    check(st == SS$_NOMOREFILES,
          "a wildcard that matches nothing is SS$_NOMOREFILES on the first call");

    /* --- (5) FAIL-HONEST -------------------------------------------------- */
    st = do_search(&a, 0x7fffffffu, (uint16_t)srch_fid, 1, "*.*;*");
    check(st == SS$_IVCHAN,
          "a search on an unassigned channel is SS$_IVCHAN (fail-honest, INV-6)");

    st = vms_kif_acp_assign(ODS2_UNIT, &chan2);
    check($VMS_STATUS_SUCCESS(st) && chan2 != 0,
          "$ASSIGN a second channel (for the no-open-context check)");
    if (chan2 != 0) {
        st = do_search(&a, chan2, (uint16_t)srch_fid, 0, NULL);   /* continue, none open */
        check(st == SS$_BADPARAM,
              "a continue on a channel with no open wildcard context is SS$_BADPARAM");
        (void)vms_kif_dassgn(chan2);
    }

    /* --- (6) the returned FID is a GENUINE on-disk file ------------------- */
    memset(&ac, 0, sizeof(ac));
    ac.chan = chan;
    ac.fidmode = 1;
    ac.fid_num = A_V3_FID;
    ac.fid_seq = 1;
    st = vms_kif_acp_access(&ac);
    check($VMS_STATUS_SUCCESS(st) && ac.fid_num == A_V3_FID &&
          !(ac.attr.filechar & ODS2_FH2_M_DIRECTORY),
          "IO$_ACCESS by the FID the search returned (14) opens that same file (FID not fabricated)");
    (void)vms_kif_acp_deaccess(chan);

    (void)vms_kif_dassgn(chan);
    st = vms_kif_acp_dmount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$DISMOUNT removes the generated ODS-2 volume");

    printf("=== test_syssvc_acp_search: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
