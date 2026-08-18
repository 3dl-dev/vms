/*
 * test_syssvc_acp_create.c - the Files-11 (ODS-2) ACP answers IO$_CREATE /
 * IO$_DELETE / IO$_MODIFY: allocate a real file header from INDEXF.SYS, enter a
 * versioned directory record, deallocate a header + its blocks, and
 * extend/truncate/write attributes -- all against a real /dev/vms (vms-5303,
 * epic vms-208).
 *
 * The SIXTH and final rung of the executive ACP-QIO surface (after the
 * kernel-resident codec vms-dcd, the channel front-end vms-149, the
 * executive-global $MOUNT vms-127, IO$_ACCESS/DEACCESS vms-204,
 * IO$_READVBLK/WRITEVBLK vms-c60, and IO$_ACPCONTROL/$SEARCH vms-a0b). On real
 * OpenVMS these are $QIO function codes (IO$_CREATE/IO$_DELETE/IO$_MODIFY) on a
 * channel $ASSIGNed to a mounted volume, serviced by the XQP in the caller's
 * context; RMS $CREATE/$ERASE/$EXTEND are layered on them (VSI I/O User's
 * Reference, "ACP-QIO Interface"). OVMX reaches the executive over /dev/vms via
 * vms_kif_acp_fileop(), whose `func` field carries the $QIO function code.
 *
 * WHAT THIS SUITE PROVES, through the sys$/kif API against a real /dev/vms, over
 * the real-VAX ODS-2 fixture the harness seeds WRITABLE on DKA0::
 *
 *   1. IO$_CREATE ALLOCATES A REAL FILE. Creating [OVMXDIR]CREAT.TST assigns a
 *      genuine FID from INDEXF.SYS's index bitmap (a new file number, distinct
 *      from every reserved and existing file), enters it in the directory at
 *      version ;1, and the file is READABLE BACK BY NAME (the directory record
 *      resolves to the assigned FID; the header parses at its INDEXF slot).
 *   2. THE FILE IS A REAL, WRITABLE, DURABLE FILE. WRITEVBLK to the freshly
 *      created (initially empty) file extends + writes; after DEACCESS +
 *      re-ACCESS the bytes persist -- it hit the platter (INV-6).
 *   3. VERSIONS. Creating CREAT.TST again yields ;2 (a SECOND file, a SECOND
 *      FID); the directory record now carries both versions, highest first.
 *   4. IO$_DELETE REMOVES + DEALLOCATES. Deleting ;2 removes its directory
 *      entry, frees its blocks in BITMAP.SYS and its FID in the index bitmap;
 *      ACCESS of ;2 is then SS$_NOSUCHFILE, while ;1 still resolves. Deleting
 *      ;1 empties the name entirely: ACCESS is SS$_NOSUCHFILE.
 *   5. IO$_MODIFY EXTENDS / TRUNCATES / WRITES ATTRIBUTES, and each persists
 *      (re-read exact): extend grows HIBLK, truncate shrinks EOF + frees blocks,
 *      a protection change survives DEACCESS + re-ACCESS.
 *   7. IO$_MODIFY!IO$M_MOVE RENAMES a file (vms-de7): RENM.TST -> RENM2.TST
 *      removes the old directory entry, inserts the new, and rewrites the FH2
 *      ident name while KEEPING the file's FID -- the new name re-ACCESSes, the
 *      old name is SS$_NOSUCHFILE. This is the create-tmp -> rename atomic-
 *      replace primitive $SETUAI / AUTHORIZE-seed build on.
 *   6. FAIL-HONEST EDGES (INV-6, never a fabricated success): an unknown $QIO
 *      function is SS$_BADPARAM; CREATE into a DID that is not a directory is
 *      SS$_NOSUCHFILE; DELETE of a name that does not exist is SS$_NOSUCHFILE.
 *
 *      NOT TESTABLE IN THIS HARNESS (stated, not faked -- CLAUDE.md Rule 10):
 *      SS$_NOPRIV (a protection-denied create) needs an UNPRIVILEGED caller, and
 *      SS$_DEVICEFULL needs an exhausted INDEXF/BITMAP; the QEMU harness runs as
 *      the all-privileged SYSTEM identity against a 16 MB fixture, so neither is
 *      reachable here. The protection GATE itself is exercised by the (already
 *      green) acp_access/acp_rw suites' NOPRIV assertions.
 *
 * ORDERING / ISOLATION. This suite MUTATES its DKA0: fixture COPY: it creates
 * files in [OVMXDIR] under UNIQUE names (CREAT.TST / MODF.TST / RENM.TST ->
 * RENM2.TST -- never HELLO.TXT,
 * which the acp_access / acp_rw suites read) and DELETES every file it creates,
 * so the net directory state is restored (only BITMAP/INDEXF bits cycle,
 * harmlessly). Each shard boots its OWN fresh writable fixture copy
 * (run_tests.sh cp per boot); this suite sorts alphabetically BEFORE acp_mount /
 * acp_rw, and touches no file they read. Same reasoning as test_syssvc_acp_rw.c.
 *
 * NO /dev/vms -> honest SKIP (77), never a fake pass: IO$_CREATE/DELETE/MODIFY
 * are executive-resident, so with no /dev/vms there is nothing to assert.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "vmsfs/ods2.h"   /* ODS2_FK_* file-kind selectors for CREATE */

#define EXIT_SKIP 77
#define ODS2_UNIT       "DKA0:"
#define OVMXDIR_FID_NUM 11u     /* [OVMXDIR] in the real-VAX fixture */

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

/* IO$_CREATE [OVMXDIR]<name>;<version 0=>highest+1>, optionally accessed. */
static uint32_t create_file(uint32_t chan, const char *name, uint16_t version,
                            unsigned modifiers, int want_write,
                            struct vms_acp_fileop_args *f)
{
    memset(f, 0, sizeof(*f));
    f->chan = chan;
    f->func = VMS_ACP_FOP_CREATE;
    f->modifiers = modifiers;
    f->kind = ODS2_FK_DATA_FIX;
    f->did_num = OVMXDIR_FID_NUM;
    f->did_seq = 1;
    f->version = version;
    if (want_write)
        f->acctl = VMS_ACP_ACCTL_WRITE;
    strncpy(f->name, name, VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_fileop(f);
}

/* IO$_DELETE [OVMXDIR]<name>;<version> (M_DELETE => also deallocate). */
static uint32_t delete_file(uint32_t chan, const char *name, uint16_t version,
                            struct vms_acp_fileop_args *f)
{
    memset(f, 0, sizeof(*f));
    f->chan = chan;
    f->func = VMS_ACP_FOP_DELETE;
    f->modifiers = VMS_ACP_M_DELETE;
    f->did_num = OVMXDIR_FID_NUM;
    f->did_seq = 1;
    f->version = version;
    strncpy(f->name, name, VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_fileop(f);
}

/* IO$_ACCESS [OVMXDIR]<name>;<version 0=>highest> for read/write. */
static uint32_t access_named(uint32_t chan, const char *name, uint16_t version,
                             int want_write, struct vms_acp_access_args *a)
{
    memset(a, 0, sizeof(*a));
    a->chan = chan;
    a->did_num = OVMXDIR_FID_NUM;
    a->did_seq = 1;
    a->version = version;
    if (want_write)
        a->acctl = VMS_ACP_ACCTL_WRITE;
    strncpy(a->name, name, VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_access(a);
}

int main(void)
{
    struct vms_acp_fileop_args f;
    struct vms_acp_access_args a;
    struct vms_acp_rw_args r;
    uint32_t st, chan = 0;
    uint32_t fid1 = 0, fid2 = 0;
    uint8_t *pat = NULL, *rd = NULL;
    unsigned i;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_acp_create: executive ACP answers IO$_CREATE/IO$_DELETE/"
           "IO$_MODIFY (header alloc + dir insert + dealloc; vms-5303, epic vms-208) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (!executive_present()) {
        printf("=== test_syssvc_acp_create: 0 passed, 0 failed (SKIPPED: no /dev/vms -- "
               "IO$_CREATE/DELETE/MODIFY are executive-resident) ===\n");
        return EXIT_SKIP;
    }

    pat = malloc(512);
    rd  = malloc(512);
    if (!pat || !rd) { printf("  FAIL: malloc\n"); return 1; }
    for (i = 0; i < 512; i++) pat[i] = (uint8_t)(0x3C ^ (i * 7u));

    st = vms_kif_acp_mount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$MOUNT of the genuine ODS-2 " ODS2_UNIT " (precondition)");
    st = vms_kif_acp_assign(ODS2_UNIT, &chan);
    check($VMS_STATUS_SUCCESS(st) && chan != 0, "$ASSIGN a file-class channel (precondition)");
    if (chan == 0) {
        printf("=== test_syssvc_acp_create: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    /* --- (6) fail-honest: an unknown $QIO function is SS$_BADPARAM ---------- */
    memset(&f, 0, sizeof(f));
    f.chan = chan; f.func = 0xDEAD;
    st = vms_kif_acp_fileop(&f);
    check(st == SS$_BADPARAM, "IO$_ fileop with an unknown function code is SS$_BADPARAM (fail-honest)");

    /* --- (6) CREATE into a DID that is not a directory is SS$_NOSUCHFILE ---- */
    memset(&f, 0, sizeof(f));
    f.chan = chan; f.func = VMS_ACP_FOP_CREATE; f.modifiers = VMS_ACP_M_CREATE;
    f.did_num = 1; f.did_seq = 1;             /* INDEXF.SYS (FID 1) -- not a directory */
    f.kind = ODS2_FK_DATA_FIX;
    strncpy(f.name, "NOPE.TST", VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_fileop(&f);
    check(st == SS$_NOSUCHFILE, "IO$_CREATE into a non-directory DID is SS$_NOSUCHFILE (fail-honest)");

    /* --- (1) CREATE [OVMXDIR]CREAT.TST -> real FID, dir entry ;1 ------------ */
    st = create_file(chan, "CREAT.TST", 0, VMS_ACP_M_CREATE | VMS_ACP_M_ACCESS, 1, &f);
    fid1 = ((uint32_t)f.fid_num) | ((uint32_t)f.fid_nmx << 16);
    check($VMS_STATUS_SUCCESS(st) && f.out_version == 1 && fid1 > OVMXDIR_FID_NUM,
          "IO$_CREATE CREAT.TST allocates a real FID (a new file number) at version ;1");

    /* --- (1) the created file is readable back BY NAME ---------------------- */
    (void)vms_kif_acp_deaccess(chan);
    /* An off-by-one in the INDEXF header-slot LBN writes the header one slot too
     * high, so the FID the directory record resolves to has no valid header and
     * the file cannot be re-opened by name. The CREATE itself still returns a
     * FID, so ONLY this re-ACCESS-by-name assertion (and its knock-ons) can tell. */
    st = access_named(chan, "CREAT.TST", 0, 0, &a);
    /* negctl: acp-create-header-slot-offbyone */
    check($VMS_STATUS_SUCCESS(st) &&
          (((uint32_t)a.fid_num | ((uint32_t)a.fid_nmx << 16)) == fid1) &&
          a.out_version == 1,
          "IO$_ACCESS CREAT.TST by name resolves the created FID at version 1 (header at its INDEXF slot)");
    (void)vms_kif_acp_deaccess(chan);

    /* --- (2) the created file is a real, writable, DURABLE file ------------- */
    st = access_named(chan, "CREAT.TST", 0, 1, &a);
    check($VMS_STATUS_SUCCESS(st), "re-ACCESS CREAT.TST for WRITE");
    memset(&r, 0, sizeof(r));
    r.chan = chan; r.vbn = 1; r.offset = 0; r.length = 512;
    r.buffer = (uint64_t)(uintptr_t)pat;
    st = vms_kif_acp_writevb(&r);
    check($VMS_STATUS_SUCCESS(st) && r.xferred == 512,
          "IO$_WRITEVBLK to the freshly created (empty) file extends + writes VBN 1");
    (void)vms_kif_acp_deaccess(chan);
    st = access_named(chan, "CREAT.TST", 0, 0, &a);
    memset(rd, 0, 512);
    memset(&r, 0, sizeof(r));
    r.chan = chan; r.vbn = 1; r.offset = 0; r.length = 512;
    r.buffer = (uint64_t)(uintptr_t)rd;
    st = vms_kif_acp_readvb(&r);
    check($VMS_STATUS_SUCCESS(st) && memcmp(rd, pat, 512) == 0,
          "after DEACCESS + re-ACCESS the written bytes persist -- it hit the platter (INV-6)");
    (void)vms_kif_acp_deaccess(chan);

    /* --- (3) CREATE again -> ;2, a second file with a second FID ----------- */
    st = create_file(chan, "CREAT.TST", 0, VMS_ACP_M_CREATE, 0, &f);
    fid2 = ((uint32_t)f.fid_num) | ((uint32_t)f.fid_nmx << 16);
    check($VMS_STATUS_SUCCESS(st) && f.out_version == 2 && fid2 != 0 && fid2 != fid1,
          "IO$_CREATE CREAT.TST again yields version ;2 with a DISTINCT FID");
    st = access_named(chan, "CREAT.TST", 0, 0, &a);   /* highest */
    check($VMS_STATUS_SUCCESS(st) && a.out_version == 2 &&
          (((uint32_t)a.fid_num | ((uint32_t)a.fid_nmx << 16)) == fid2),
          "IO$_ACCESS CREAT.TST (highest) now resolves ;2 (versions descending in the dir record)");
    (void)vms_kif_acp_deaccess(chan);
    st = access_named(chan, "CREAT.TST", 1, 0, &a);   /* explicit ;1 */
    check($VMS_STATUS_SUCCESS(st) && a.out_version == 1 &&
          (((uint32_t)a.fid_num | ((uint32_t)a.fid_nmx << 16)) == fid1),
          "IO$_ACCESS CREAT.TST;1 still resolves the FIRST file (both versions coexist)");
    (void)vms_kif_acp_deaccess(chan);

    /* --- (4) DELETE ;2 removes the entry + deallocates --------------------- */
    st = delete_file(chan, "CREAT.TST", 2, &f);
    check($VMS_STATUS_SUCCESS(st), "IO$_DELETE CREAT.TST;2 (remove entry + deallocate header/blocks)");
    st = access_named(chan, "CREAT.TST", 2, 0, &a);
    check(st == SS$_NOSUCHFILE, "IO$_ACCESS CREAT.TST;2 after delete is SS$_NOSUCHFILE (entry gone, FID freed)");
    st = access_named(chan, "CREAT.TST", 0, 0, &a);
    check($VMS_STATUS_SUCCESS(st) && a.out_version == 1,
          "IO$_ACCESS CREAT.TST (highest) after deleting ;2 resolves ;1 again");
    (void)vms_kif_acp_deaccess(chan);

    /* --- (4) DELETE ;1 empties the name ------------------------------------ */
    st = delete_file(chan, "CREAT.TST", 1, &f);
    check($VMS_STATUS_SUCCESS(st), "IO$_DELETE CREAT.TST;1 (last version)");
    st = access_named(chan, "CREAT.TST", 0, 0, &a);
    check(st == SS$_NOSUCHFILE, "IO$_ACCESS CREAT.TST after deleting every version is SS$_NOSUCHFILE");

    /* --- (6) DELETE of a name that never existed is SS$_NOSUCHFILE ---------- */
    st = delete_file(chan, "GHOST.TST", 0, &f);
    check(st == SS$_NOSUCHFILE, "IO$_DELETE of a nonexistent name is SS$_NOSUCHFILE (fail-honest)");

    /* --- (5) IO$_MODIFY extend / truncate / write-attributes --------------- */
    st = create_file(chan, "MODF.TST", 0, VMS_ACP_M_CREATE, 0, &f);
    check($VMS_STATUS_SUCCESS(st) && f.out_version == 1, "IO$_CREATE MODF.TST for the MODIFY proofs");

    /* extend by 3 blocks */
    memset(&f, 0, sizeof(f));
    f.chan = chan; f.func = VMS_ACP_FOP_MODIFY;
    f.did_num = OVMXDIR_FID_NUM; f.did_seq = 1; f.version = 1;
    f.exsz = 3;
    strncpy(f.name, "MODF.TST", VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_fileop(&f);
    check($VMS_STATUS_SUCCESS(st) && f.new_hiblk == 3,
          "IO$_MODIFY extend by 3 grows the allocation to HIBLK 3 (BITMAP.SYS alloc + FH2 map append)");

    /* re-ACCESS confirms the grown allocation persisted */
    st = access_named(chan, "MODF.TST", 0, 0, &a);
    check($VMS_STATUS_SUCCESS(st) && a.attr.hiblk == 3,
          "after re-ACCESS the on-disk FH2 reports HIBLK 3 (extend persisted)");
    (void)vms_kif_acp_deaccess(chan);

    /* truncate to EOF block 1 */
    memset(&f, 0, sizeof(f));
    f.chan = chan; f.func = VMS_ACP_FOP_MODIFY;
    f.did_num = OVMXDIR_FID_NUM; f.did_seq = 1; f.version = 1;
    f.trunc_efblk = 1; f.trunc_ffbyte = 0;
    strncpy(f.name, "MODF.TST", VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_fileop(&f);
    check($VMS_STATUS_SUCCESS(st) && f.new_hiblk == 1 && f.new_efblk == 1,
          "IO$_MODIFY truncate to EOF block 1 shrinks HIBLK to 1 (blocks past it freed)");
    st = access_named(chan, "MODF.TST", 0, 0, &a);
    check($VMS_STATUS_SUCCESS(st) && a.attr.hiblk == 1,
          "after re-ACCESS the on-disk FH2 reports the truncated HIBLK 1 (truncate persisted)");
    (void)vms_kif_acp_deaccess(chan);

    /* write attributes: change the protection mask */
    memset(&f, 0, sizeof(f));
    f.chan = chan; f.func = VMS_ACP_FOP_MODIFY;
    f.did_num = OVMXDIR_FID_NUM; f.did_seq = 1; f.version = 1;
    f.attr_ctl = VMS_ACP_ATTR_PROT;
    f.attr.fileprot = 0xFF00u;     /* System/Owner keep access; World fully denied */
    strncpy(f.name, "MODF.TST", VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_fileop(&f);
    check($VMS_STATUS_SUCCESS(st), "IO$_MODIFY write attributes (protection = 0xFF00)");
    st = access_named(chan, "MODF.TST", 0, 0, &a);
    check($VMS_STATUS_SUCCESS(st) && a.attr.fileprot == 0xFF00u,
          "after re-ACCESS the on-disk FH2 reports the modified protection 0xFF00 (attr write persisted)");
    (void)vms_kif_acp_deaccess(chan);

    /* clean up MODF.TST so the fixture copy is left as found */
    st = delete_file(chan, "MODF.TST", 1, &f);
    check($VMS_STATUS_SUCCESS(st), "IO$_DELETE MODF.TST (restore the fixture directory state)");

    /* --- (7) IO$_MODIFY!IO$M_MOVE renames a file within a directory (vms-de7)
     * -- the write primitive $SETUAI / AUTHORIZE-seed use for the
     * create-tmp -> rename atomic-replace pattern (write NAME.DAT;n+1, rename
     * over the live copy). ---------------------------------------------------- */
    st = create_file(chan, "RENM.TST", 0, VMS_ACP_M_CREATE, 0, &f);
    check($VMS_STATUS_SUCCESS(st) && f.out_version == 1,
          "IO$_CREATE RENM.TST for the RENAME proof");

    memset(&f, 0, sizeof(f));
    f.chan = chan; f.func = VMS_ACP_FOP_MODIFY; f.modifiers = VMS_ACP_M_MOVE;
    f.did_num = OVMXDIR_FID_NUM; f.did_seq = 1; f.version = 1;
    strncpy(f.name, "RENM.TST", VMS_ACP_NAME_SIZE - 1);
    strncpy(f.new_name, "RENM2.TST", VMS_ACP_NAME_SIZE - 1);
    /* new_did 0 => the same directory; new_version 0 => highest+1 (== 1). */
    st = vms_kif_acp_fileop(&f);
    check($VMS_STATUS_SUCCESS(st) && f.out_version == 1,
          "IO$_MODIFY!IO$M_MOVE renames RENM.TST -> RENM2.TST (remove old dir "
          "entry, insert new, rewrite the FH2 ident name; FID kept)");

    /* the NEW name resolves back to the SAME file; the OLD name is gone */
    st = access_named(chan, "RENM2.TST", 0, 0, &a);
    check($VMS_STATUS_SUCCESS(st),
          "IO$_ACCESS RENM2.TST resolves the renamed file (new directory entry)");
    (void)vms_kif_acp_deaccess(chan);
    st = access_named(chan, "RENM.TST", 0, 0, &a);
    check(st == SS$_NOSUCHFILE,
          "IO$_ACCESS RENM.TST after rename is SS$_NOSUCHFILE (old entry gone)");

    /* rename of a name that does not exist is fail-honest (never fabricated) */
    memset(&f, 0, sizeof(f));
    f.chan = chan; f.func = VMS_ACP_FOP_MODIFY; f.modifiers = VMS_ACP_M_MOVE;
    f.did_num = OVMXDIR_FID_NUM; f.did_seq = 1; f.version = 0;
    strncpy(f.name, "GHOST.TST", VMS_ACP_NAME_SIZE - 1);
    strncpy(f.new_name, "GHOST2.TST", VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_fileop(&f);
    check(st == SS$_NOSUCHFILE,
          "IO$_MODIFY!IO$M_MOVE of a nonexistent source name is SS$_NOSUCHFILE");

    /* clean up RENM2.TST so the fixture copy is left as found */
    st = delete_file(chan, "RENM2.TST", 1, &f);
    check($VMS_STATUS_SUCCESS(st), "IO$_DELETE RENM2.TST (restore the fixture directory state)");

    (void)vms_kif_dassgn(chan);
    st = vms_kif_acp_dmount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$DISMOUNT removes the ODS-2 volume");

    free(pat); free(rd);
    printf("=== test_syssvc_acp_create: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
