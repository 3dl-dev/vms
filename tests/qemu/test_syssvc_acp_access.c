/*
 * test_syssvc_acp_access.c - the Files-11 (ODS-2) ACP answers IO$_ACCESS /
 * IO$_DEACCESS: open a file by NAME and by FID, build its VBN->LBN window,
 * return its FID + attributes, and enforce protection (vms-204, epic vms-208),
 * proven against a real /dev/vms.
 *
 * This is the FOURTH rung of the executive ACP (after the kernel-resident codec
 * vms-dcd, the channel front-end vms-149, and the executive-global $MOUNT
 * vms-127): the ACP-QIO FILE OPERATION the channel/mount rungs were cut to
 * carry. On real OpenVMS this is a $QIO(IO$_ACCESS) on a channel $ASSIGNed to a
 * mounted volume, serviced by the XQP in the caller's context (VSI I/O User's
 * Reference, "ACP-QIO Interface"). OVMX reaches the executive over /dev/vms via
 * the KIF helper vms_kif_acp_access() (the RMS-over-$QIO rung later wires this
 * to $OPEN/$CLOSE); here it is driven directly, exactly as test_syssvc_acp_mount
 * drives vms_kif_acp_mount().
 *
 * WHAT THIS SUITE PROVES, through the sys$/kif API against a real /dev/vms, over
 * the real-VAX ODS-2 fixture the harness seeds on DKA0: (run_tests.sh, from the
 * staged /ods2_real.img == tests/ods2/real_vax_ods2.dsk):
 *
 *   1. OPEN A DIRECTORY BY NAME. IO$_ACCESS of "OVMXDIR.DIR" in the MFD
 *      ([000000], the default DID) resolves the real directory, returns its
 *      real FID (11,1) and the DIRECTORY file-characteristic bit -- walked out
 *      of the on-disk MFD by the codec's directory reader.
 *
 *   2. OPEN A FILE BY NAME + BUILD THE WINDOW. IO$_ACCESS of "HELLO.TXT" with
 *      DID = the OVMXDIR FID from (1) returns its real FID (12,1), its
 *      attributes (owner [1,4], efblk 34), and a VBN->LBN window whose first
 *      extent LBN and total block count match what the codec computes for that
 *      file (first_lbn 32, 34 blocks, one extent). A VBN probe resolves through
 *      the window to the LBN the codec's map-walk computes.
 *
 *   3. OPEN THE SAME FILE BY FID. IO$_ACCESS with fidmode + FIB$W_FID (12,1)
 *      opens HELLO.TXT directly (no directory search) and returns the IDENTICAL
 *      FID, attributes and window as the by-name open in (2).
 *
 *   4. FAIL-HONEST ON A MISSING NAME. IO$_ACCESS of "NOSUCH.TXT" in the MFD is
 *      SS$_NOSUCHFILE -- never a fabricated handle to a file the volume does not
 *      have (INV-6).
 *
 *   5. IO$_DEACCESS releases the accessed file; a second IO$_DEACCESS of the now
 *      un-accessed channel is SS$_FILNOTACC (fail-honest).
 *
 *   6. PROTECTION IS ENFORCED (INV-6). A genuinely UNPRIVILEGED, non-owner
 *      process -- a re-exec'd child that setgid/setuid'd to [100,100] so the
 *      executive derives that UIC from its real credentials and hands it only
 *      TMPMBX/NETMBX -- is REFUSED SS$_NOPRIV opening HELLO.TXT by FID, because
 *      the file's WORLD protection field denies read and the child qualifies for
 *      no other category. The parent (a system-group process) opens the very
 *      same file successfully in (2)/(3): same code, opposite verdict, decided
 *      only by the accessor's executive-derived identity -- the shape a silent
 *      userspace allow could never show.
 *
 *   7. PER-FILE-CLASS PROTECTION (vms-109/vms-548b). On the generated
 *      system-disk volume (DKA300:), the three security-sensitive SYS$SYSTEM
 *      files are mastered with DIFFERENT protections by file class:
 *      SYSUAF.DAT World:none (0xFF88 -- its Purdy hashes must not leak),
 *      RIGHTSLIST.DAT World:R (0xEE00 -- the world-readable rights database),
 *      DCL.EXE World:RE (0xAA00 -- a shipped image stays activatable). A
 *      privileged read pins each file's exact fileprot; the SAME unprivileged
 *      [100,100] child is then REFUSED SYSUAF.DAT but GRANTED RIGHTSLIST.DAT
 *      and DCL.EXE -- three verdicts from one acp_check_access, decided only by
 *      each file's class-assigned protection.
 *
 * NO /dev/vms -> honest SKIP (77), never a fake pass: the ACP, the window, and
 * the protection gate are executive-resident, so with no /dev/vms there is
 * nothing to assert (the contract every test_syssvc_* suite is held to,
 * .github/workflows/ci.yml).
 *
 * GROUND TRUTH. The concrete FIDs/attributes/LBNs asserted below were read off
 * tests/ods2/real_vax_ods2.dsk with the userspace ODS-2 codec (the same reader,
 * compiled userspace) -- see tests/ods2/PROVENANCE-real_vax_ods2.md. They are
 * facts of that pinned real-VAX volume, not values this test invents.
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

#define EXIT_SKIP 77

/* DKA0: (vda) carries the genuine real-VAX ODS-2 volume in the QEMU harness. */
#define ODS2_UNIT  "DKA0:"

/* Ground truth read off tests/ods2/real_vax_ods2.dsk (userspace codec dump). */
#define OVMXDIR_FID_NUM   11u    /* [000000]OVMXDIR.DIR;1  FID (11,1,0) */
#define HELLO_FID_NUM     12u    /* [OVMXDIR]HELLO.TXT;1   FID (12,1,0) */
#define HELLO_EFBLK       34u    /* end-of-file / highest VBN */
#define HELLO_FIRST_LBN   32u    /* first (only) retrieval-pointer extent LBN */
#define OWNER_GROUP       1u     /* [1,4] SYSTEM */
#define OWNER_MEMBER      4u
#define ODS2_FH2_M_DIRECTORY 0x2000u

/* An unprivileged, non-owner identity for the protection proof: group 100 is
 * NOT a system group and NOT the files' owner group (1), so it qualifies for no
 * protection category but WORLD -- which denies read on the fixture files. */
#define UNPRIV_GID 100
#define UNPRIV_UID 100

/* DKA300: (vdd) carries the GENERATED system-disk ODS-2 volume (mkimage_ods2_
 * sysvol.c) holding SYS$SYSTEM:{SYSUAF.DAT,RIGHTSLIST.DAT,DCL.EXE}. The
 * PER-FILE-CLASS protection proof (vms-109/vms-548b) reads these three files
 * over the same acp_check_access gate, at [SYS0.SYSCOMMON.SYSEXE]. */
#define SYSVOL_UNIT  "DKA300:"

/* The per-file-class fh2_fileprot each of the three files is mastered with
 * (ods2_class_fileprot(), ods2.h). Nibbles S/O/G/W low->high, SET bit DENIES. */
#define SYSUAF_PROT      0xFF88u   /* S:RWE,O:RWE,G:none,W:none -- oracle (RWE,RWE,,) */
#define RIGHTSLIST_PROT  0xEE00u   /* S:RWED,O:RWED,G:R,W:R  -- WORLD-READABLE       */
#define DCLEXE_PROT      0xAA00u   /* S:RWED,O:RWED,G:RE,W:RE -- image World:RE       */

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

/* Fill IN fields common to every access; caller sets the mode-specific ones. */
static void acc_init(struct vms_acp_access_args *a, uint32_t chan)
{
    memset(a, 0, sizeof(*a));
    a->chan = chan;
}

/* --- the unprivileged child (protection proof) ----------------------------- */

/* The child reports back the single status it observed opening HELLO.TXT by FID
 * as an unprivileged, non-owner process. Fixed-width so the layout survives the
 * fork+exec. */
struct child_rep {
    uint32_t uic;             /* the UIC the executive derived for the child */
    uint32_t access_status;   /* IO$_ACCESS status the child observed */
    uint32_t assign_status;   /* $ASSIGN status (context for a diagnosis) */
};

static int run_noprivchild(int b2a_write)
{
    struct child_rep rep;
    struct vms_acp_access_args a;
    uint32_t chan = 0;

    memset(&rep, 0, sizeof(rep));

    /* A re-exec'd gcc image has no per-process PCB until it makes one. It has
     * already setgid/setuid'd to [100,100] BEFORE this exec (see the fork arm),
     * so the executive derives that unprivileged UIC from its real creds. */
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        (void)!write(b2a_write, &rep, sizeof(rep));
        return 1;
    }

    /* The parent has already $MOUNTed DKA0: executive-global; the child sees it. */
    rep.assign_status = vms_kif_acp_assign(ODS2_UNIT, &chan);
    if (!$VMS_STATUS_SUCCESS(rep.assign_status) || chan == 0) {
        (void)!write(b2a_write, &rep, sizeof(rep));
        return 1;
    }

    /* Open HELLO.TXT BY FID -- straight to the protection gate, no directory
     * traversal. World protection denies read to [100,100]; no privilege lifts
     * it (an unprivileged process holds only TMPMBX/NETMBX). Expect NOPRIV. */
    acc_init(&a, chan);
    a.fidmode = 1;
    a.fid_num = HELLO_FID_NUM;
    a.fid_seq = 1;
    rep.access_status = vms_kif_acp_access(&a);

    (void)vms_kif_acp_deaccess(chan);
    (void)vms_kif_dassgn(chan);

    (void)!write(b2a_write, &rep, sizeof(rep));
    return 0;
}

/* --- the per-file-class protection proof (vms-109/vms-548b/vms-930) --------- */

/* Resolve one directory level: IO$_ACCESS `dirname` ("NAME.DIR") in `parent_did`
 * and return its FID number, or 0. The parent (privileged, system group) walks
 * the concealed-rooted path this way; the ACP does not protection-check an
 * intermediate directory during acp_dir_find, so once the parent hands the
 * child the leaf directory's FID the child can look up a name inside it and the
 * TARGET FILE's own protection is the only gate (which is the point). */
static uint32_t resolve_did(uint32_t chan, uint16_t parent_did, const char *dirname)
{
    struct vms_acp_access_args a;
    uint32_t st;
    acc_init(&a, chan);
    a.did_num = parent_did;             /* 0 => the MFD [000000] */
    a.did_seq = parent_did ? 1 : 0;
    strncpy(a.name, dirname, VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_access(&a);
    if (!$VMS_STATUS_SUCCESS(st)) return 0;
    { uint32_t fn = a.fid_num; (void)vms_kif_acp_deaccess(chan); return fn; }
}

/* What the unprivileged child reports opening each of the three files by name in
 * [SYSEXE] (whose FID the privileged parent passes in). Fixed-width: survives
 * fork+exec. */
struct prot_child_rep {
    uint32_t uic;
    uint32_t assign_status;
    uint32_t sysuaf_status;     /* expect SS$_NOPRIV -- World:none */
    uint32_t rights_status;     /* expect success   -- World:R    */
    uint32_t dclexe_status;     /* expect success   -- World:RE   */
    uint16_t rights_prot;       /* fileprot the child saw on the granted RIGHTSLIST */
    uint16_t dclexe_prot;       /* fileprot the child saw on the granted DCL.EXE   */
};

static int run_noprivchild_prot(int b2a_write, uint16_t sysexe_did)
{
    struct prot_child_rep rep;
    struct vms_acp_access_args a;
    uint32_t chan = 0;

    memset(&rep, 0, sizeof(rep));

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        (void)!write(b2a_write, &rep, sizeof(rep));
        return 1;
    }

    rep.assign_status = vms_kif_acp_assign(SYSVOL_UNIT, &chan);
    if (!$VMS_STATUS_SUCCESS(rep.assign_status) || chan == 0) {
        (void)!write(b2a_write, &rep, sizeof(rep));
        return 1;
    }

    /* SYSUAF.DAT -- World:none: an unprivileged, non-owner, non-system caller
     * must be REFUSED read (its Purdy hashes stay secret). */
    acc_init(&a, chan);
    a.did_num = sysexe_did; a.did_seq = 1;
    strncpy(a.name, "SYSUAF.DAT", VMS_ACP_NAME_SIZE - 1);
    rep.sysuaf_status = vms_kif_acp_access(&a);
    (void)vms_kif_acp_deaccess(chan);

    /* RIGHTSLIST.DAT -- World:R: the SAME caller must be GRANTED read. */
    acc_init(&a, chan);
    a.did_num = sysexe_did; a.did_seq = 1;
    strncpy(a.name, "RIGHTSLIST.DAT", VMS_ACP_NAME_SIZE - 1);
    rep.rights_status = vms_kif_acp_access(&a);
    if ($VMS_STATUS_SUCCESS(rep.rights_status)) rep.rights_prot = a.attr.fileprot;
    (void)vms_kif_acp_deaccess(chan);

    /* DCL.EXE -- World:RE image: GRANTED read. */
    acc_init(&a, chan);
    a.did_num = sysexe_did; a.did_seq = 1;
    strncpy(a.name, "DCL.EXE", VMS_ACP_NAME_SIZE - 1);
    rep.dclexe_status = vms_kif_acp_access(&a);
    if ($VMS_STATUS_SUCCESS(rep.dclexe_status)) rep.dclexe_prot = a.attr.fileprot;
    (void)vms_kif_acp_deaccess(chan);

    (void)vms_kif_dassgn(chan);
    (void)!write(b2a_write, &rep, sizeof(rep));
    return 0;
}

int main(int argc, char **argv)
{
    struct vms_acp_access_args a;
    uint32_t st, chan = 0;
    uint32_t ovmxdir_fid_num = 0, hello_fid_num = 0, hello_fid_seq = 0;
    int b2a[2];
    pid_t pid;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    /* Re-exec'd unprivileged-child mode (the protection proof's other half). */
    if (argc >= 3 && strcmp(argv[1], "--noprivchild") == 0)
        return run_noprivchild(atoi(argv[2]));
    /* Re-exec'd unprivileged-child mode for the per-file-class protection proof
     * (argv[3] = the [SYSEXE] directory FID the privileged parent resolved). */
    if (argc >= 4 && strcmp(argv[1], "--noprivchildprot") == 0)
        return run_noprivchild_prot(atoi(argv[2]), (uint16_t)atoi(argv[3]));

    printf("=== test_syssvc_acp_access: executive ACP answers IO$_ACCESS/IO$_DEACCESS "
           "(open by name + by FID, window, protection; vms-204, epic vms-208) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        printf("=== test_syssvc_acp_access: 0 passed, 0 failed "
               "(SKIPPED: no /dev/vms -- the ACP, the file window and the "
               "protection gate are executive-resident) ===\n");
        return EXIT_SKIP;
    }

    /* Precondition: DKA0: mounted executive-global, and a file-class channel. */
    st = vms_kif_acp_mount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$MOUNT of the genuine ODS-2 " ODS2_UNIT " (precondition)");
    st = vms_kif_acp_assign(ODS2_UNIT, &chan);
    check($VMS_STATUS_SUCCESS(st) && chan != 0,
          "$ASSIGN a file-class channel to " ODS2_UNIT " (precondition)");
    if (chan == 0) {
        printf("=== test_syssvc_acp_access: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    /* --- (1) open a DIRECTORY by name in the MFD -------------------------- */
    acc_init(&a, chan);
    /* did all-zero => the MFD [000000]; name includes the .DIR type. */
    strncpy(a.name, "OVMXDIR.DIR", VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_access(&a);
    check($VMS_STATUS_SUCCESS(st) && a.fid_num == OVMXDIR_FID_NUM,
          "IO$_ACCESS by NAME resolves [000000]OVMXDIR.DIR to its real FID (11) from the on-disk MFD");
    check($VMS_STATUS_SUCCESS(st) && (a.attr.filechar & ODS2_FH2_M_DIRECTORY),
          "the resolved OVMXDIR.DIR carries the DIRECTORY file characteristic");
    ovmxdir_fid_num = a.fid_num;
    (void)vms_kif_acp_deaccess(chan);

    /* --- (2) open a FILE by name, DID = the OVMXDIR FID, and build window -- */
    acc_init(&a, chan);
    a.did_num = (uint16_t)ovmxdir_fid_num;   /* FIB$W_DID = [OVMXDIR] */
    a.did_seq = 1;
    strncpy(a.name, "HELLO.TXT", VMS_ACP_NAME_SIZE - 1);
    a.probe_vbn = HELLO_EFBLK;               /* resolve the last VBN through the window */
    st = vms_kif_acp_access(&a);
    check($VMS_STATUS_SUCCESS(st) && a.fid_num == HELLO_FID_NUM,
          "IO$_ACCESS by NAME (DID=OVMXDIR) resolves HELLO.TXT to its real FID (12)");
    check($VMS_STATUS_SUCCESS(st) && a.attr.efblk == HELLO_EFBLK,
          "the returned attributes carry HELLO.TXT's real end-of-file VBN (34) from its FH2");
    check($VMS_STATUS_SUCCESS(st) &&
          a.attr.uic_group == OWNER_GROUP && a.attr.uic_member == OWNER_MEMBER,
          "the returned owner UIC is the file's real [1,4]");
    check($VMS_STATUS_SUCCESS(st) &&
          a.window_nextents == 1 && a.total_blocks == HELLO_EFBLK &&
          a.first_lbn == HELLO_FIRST_LBN,
          "the VBN->LBN window matches the codec: 1 extent, 34 blocks, first LBN 32");
    /* negctl: acp-access-window-vbn-offbyone */
    check($VMS_STATUS_SUCCESS(st) &&
          a.probe_lbn == HELLO_FIRST_LBN + (HELLO_EFBLK - 1u),
          "a VBN probe resolves through the window to the LBN the codec computes (VBN 34 -> LBN 65)");
    hello_fid_num = a.fid_num;
    hello_fid_seq = a.fid_seq;
    (void)vms_kif_acp_deaccess(chan);

    /* --- (3) open the SAME file BY FID: identical FID/attrs/window --------- */
    acc_init(&a, chan);
    a.fidmode = 1;
    a.fid_num = (uint16_t)hello_fid_num;
    a.fid_seq = (uint16_t)hello_fid_seq;
    a.probe_vbn = 1;                         /* VBN 1 -> the first extent LBN */
    st = vms_kif_acp_access(&a);
    check($VMS_STATUS_SUCCESS(st) && a.fid_num == HELLO_FID_NUM &&
          a.attr.efblk == HELLO_EFBLK && a.first_lbn == HELLO_FIRST_LBN &&
          a.total_blocks == HELLO_EFBLK,
          "IO$_ACCESS by FID opens the same HELLO.TXT with identical FID/attributes/window");
    check($VMS_STATUS_SUCCESS(st) && a.probe_lbn == HELLO_FIRST_LBN,
          "by-FID open's window maps VBN 1 -> the file's first LBN (32)");
    (void)vms_kif_acp_deaccess(chan);

    /* --- (4) fail-honest on a missing name -------------------------------- */
    acc_init(&a, chan);
    strncpy(a.name, "NOSUCH.TXT", VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_access(&a);
    check(st == SS$_NOSUCHFILE,
          "IO$_ACCESS of a name that is not in the directory is SS$_NOSUCHFILE (fail-honest)");

    /* --- (5) IO$_DEACCESS releases; a second one is SS$_FILNOTACC ---------- */
    acc_init(&a, chan);
    a.fidmode = 1;
    a.fid_num = (uint16_t)hello_fid_num;
    a.fid_seq = (uint16_t)hello_fid_seq;
    st = vms_kif_acp_access(&a);
    check($VMS_STATUS_SUCCESS(st), "re-open HELLO.TXT to exercise IO$_DEACCESS");
    st = vms_kif_acp_deaccess(chan);
    check($VMS_STATUS_SUCCESS(st), "IO$_DEACCESS releases the accessed file");
    st = vms_kif_acp_deaccess(chan);
    check(st == SS$_FILNOTACC,
          "a second IO$_DEACCESS of the now un-accessed channel is SS$_FILNOTACC (fail-honest)");

    (void)vms_kif_dassgn(chan);

    /* --- (6) protection is enforced: an unprivileged, non-owner process is
     *         refused SS$_NOPRIV opening the very file the parent just opened - */
    if (pipe(b2a) < 0) {
        printf("  FAIL: pipe() failed\n");
    } else if ((pid = fork()) < 0) {
        printf("  FAIL: fork() failed\n");
    } else if (pid == 0) {
        char wfd[16];
        close(b2a[0]);
        /* Drop to a genuinely unprivileged, non-owner identity BEFORE exec, so
         * the re-exec'd image registers with UIC [100,100] derived from its real
         * credentials (INV-6 -- a process cannot declare its own UIC) and holds
         * no file-access privilege. setgid before setuid (order matters). */
        if (setgid(UNPRIV_GID) != 0 || setuid(UNPRIV_UID) != 0)
            _exit(1);
        snprintf(wfd, sizeof(wfd), "%d", b2a[1]);
        execl(argv[0], argv[0], "--noprivchild", wfd, (char *)NULL);
        _exit(1);
    } else {
        struct child_rep rep;
        ssize_t got;
        int wstatus = 0;
        close(b2a[1]);
        got = read(b2a[0], &rep, sizeof(rep));
        close(b2a[0]);
        waitpid(pid, &wstatus, 0);

        check(got == (ssize_t)sizeof(rep) && $VMS_STATUS_SUCCESS(rep.assign_status),
              "the unprivileged child $ASSIGNs the executive-global volume (sees the parent's mount)");
        check(got == (ssize_t)sizeof(rep) && rep.access_status == SS$_NOPRIV,
              "an UNPRIVILEGED, non-owner process is REFUSED SS$_NOPRIV opening HELLO.TXT (protection enforced, INV-6)");
    }

    /* --- (7) PER-FILE-CLASS protection over the ACP (vms-109/vms-548b) -----
     *
     * The single "every ordinary file is World:RE" default (vms-37e) made
     * SYSUAF.DAT's Purdy password hashes World-readable. Files-11 protection is
     * per-file: SYSUAF.DAT is World:none, RIGHTSLIST.DAT is World:R (so an
     * unprivileged F$IDENTIFIER can read it), and a shipped image (DCL.EXE) is
     * World:RE. This scenario mounts the generated system-disk volume and reads
     * all three files over the SAME acp_check_access gate -- proving the class
     * mapping BY the executive's own verdict, not by inspecting bytes. */
    {
        uint32_t schan = 0;
        uint16_t sys0, syscommon, sysexe;
        struct vms_acp_access_args pa;

        st = vms_kif_acp_mount(SYSVOL_UNIT);
        check($VMS_STATUS_SUCCESS(st),
              "$MOUNT of the generated system-disk " SYSVOL_UNIT " (protection proof precondition)");
        st = vms_kif_acp_assign(SYSVOL_UNIT, &schan);
        check($VMS_STATUS_SUCCESS(st) && schan != 0,
              "$ASSIGN a file-class channel to " SYSVOL_UNIT " (protection proof precondition)");

        /* Privileged (system-group) walk to [SYS0.SYSCOMMON.SYSEXE]. */
        sys0      = (uint16_t)resolve_did(schan, 0, "SYS0.DIR");
        syscommon = (uint16_t)resolve_did(schan, sys0, "SYSCOMMON.DIR");
        sysexe    = (uint16_t)resolve_did(schan, syscommon, "SYSEXE.DIR");
        check(sys0 && syscommon && sysexe,
              "walk the concealed-rooted path to [SYS0.SYSCOMMON.SYSEXE]");

        /* PRIVILEGED reads pin the EXACT per-class protection each file carries
         * (grounded values); the system-group parent is granted every file. */
        acc_init(&pa, schan);
        pa.did_num = sysexe; pa.did_seq = 1;
        strncpy(pa.name, "SYSUAF.DAT", VMS_ACP_NAME_SIZE - 1);
        st = vms_kif_acp_access(&pa);
        check($VMS_STATUS_SUCCESS(st) && pa.attr.fileprot == SYSUAF_PROT,
              "SYSUAF.DAT is mastered World:none (fileprot 0xFF88 -- oracle (RWE,RWE,,))");
        (void)vms_kif_acp_deaccess(schan);

        acc_init(&pa, schan);
        pa.did_num = sysexe; pa.did_seq = 1;
        strncpy(pa.name, "RIGHTSLIST.DAT", VMS_ACP_NAME_SIZE - 1);
        st = vms_kif_acp_access(&pa);
        check($VMS_STATUS_SUCCESS(st) && pa.attr.fileprot == RIGHTSLIST_PROT,
              "RIGHTSLIST.DAT is mastered World:R (fileprot 0xEE00 -- world-readable rights db)");
        (void)vms_kif_acp_deaccess(schan);

        acc_init(&pa, schan);
        pa.did_num = sysexe; pa.did_seq = 1;
        strncpy(pa.name, "DCL.EXE", VMS_ACP_NAME_SIZE - 1);
        st = vms_kif_acp_access(&pa);
        check($VMS_STATUS_SUCCESS(st) && pa.attr.fileprot == DCLEXE_PROT,
              "DCL.EXE is mastered World:RE (fileprot 0xAA00 -- shipped image default)");
        (void)vms_kif_acp_deaccess(schan);
        (void)vms_kif_dassgn(schan);

        /* The UNPRIVILEGED verdict: same files, opposite outcomes decided ONLY
         * by the accessor's executive-derived UIC and each file's protection. */
        {
            int p2[2];
            pid_t cpid;
            if (pipe(p2) < 0) {
                check(0, "pipe() for the protection child");
            } else if ((cpid = fork()) < 0) {
                check(0, "fork() for the protection child");
            } else if (cpid == 0) {
                char wfd[16], did[16];
                close(p2[0]);
                if (setgid(UNPRIV_GID) != 0 || setuid(UNPRIV_UID) != 0)
                    _exit(1);
                snprintf(wfd, sizeof(wfd), "%d", p2[1]);
                snprintf(did, sizeof(did), "%u", (unsigned)sysexe);
                execl(argv[0], argv[0], "--noprivchildprot", wfd, did, (char *)NULL);
                _exit(1);
            } else {
                struct prot_child_rep rep;
                ssize_t got;
                int wstatus = 0;
                close(p2[1]);
                got = read(p2[0], &rep, sizeof(rep));
                close(p2[0]);
                waitpid(cpid, &wstatus, 0);

                check(got == (ssize_t)sizeof(rep) &&
                      $VMS_STATUS_SUCCESS(rep.assign_status),
                      "the unprivileged child $ASSIGNs " SYSVOL_UNIT " (sees the parent's mount)");
                /* THE SECURITY ASSERTION: World cannot read SYSUAF.DAT. */
                check(got == (ssize_t)sizeof(rep) && rep.sysuaf_status == SS$_NOPRIV,
                      "an UNPRIVILEGED process is REFUSED SS$_NOPRIV reading SYSUAF.DAT "
                      "(Purdy hashes NOT World-readable -- vms-109)");
                /* World CAN read the world-readable rights database. */
                check(got == (ssize_t)sizeof(rep) &&
                      $VMS_STATUS_SUCCESS(rep.rights_status) &&
                      rep.rights_prot == RIGHTSLIST_PROT,
                      "the SAME unprivileged process IS GRANTED read of RIGHTSLIST.DAT "
                      "(World:R honored by acp_check_access -- vms-548b)");
                /* World CAN read (activate) a shipped image. */
                check(got == (ssize_t)sizeof(rep) &&
                      $VMS_STATUS_SUCCESS(rep.dclexe_status) &&
                      rep.dclexe_prot == DCLEXE_PROT,
                      "the SAME unprivileged process IS GRANTED read of DCL.EXE "
                      "(World:RE -- image stays activatable, vms-37e kept)");
            }
        }

        st = vms_kif_acp_dmount(SYSVOL_UNIT);
        check($VMS_STATUS_SUCCESS(st), "$DISMOUNT removes the system-disk volume");
    }

    /* --- cleanup ---------------------------------------------------------- */
    st = vms_kif_acp_dmount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$DISMOUNT removes the ODS-2 volume");

    printf("=== test_syssvc_acp_access: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
