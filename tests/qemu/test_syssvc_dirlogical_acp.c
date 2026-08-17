/*
 * test_syssvc_dirlogical_acp.c - vms-0044 (FLIP-BLOCKER, epic vms-208): the
 * FULL rooted/concealed directory-logical resolution chain, proven end-to-end
 * against a real /dev/vms.
 *
 *     SYS$SYSTEM:SYSUAF.DAT
 *       (1) lnm + $PARSE compose, through the concealed rooted search list
 *           (SYS$SYSTEM = SYS$SYSROOT:[SYSEXE]; SYS$SYSROOT = SYS$SYSDEVICE:
 *           [SYS0.], SYS$SYSDEVICE:[SYS0.SYSCOMMON.]; SYS$SYSDEVICE = DKA300:),
 *           to the two ordered candidates
 *               DKA300:[SYS0.SYSEXE]SYSUAF.DAT            (node member, first)
 *               DKA300:[SYS0.SYSCOMMON.SYSEXE]SYSUAF.DAT  (common member, second)
 *       (2) the Files-11 ACP walks each candidate: MFD [000000] -> [SYS0] ->
 *           [SYSCOMMON] -> [SYSEXE] via IO$_ACCESS-by-name, each level's
 *           <comp>.DIR entry giving the next directory FID, then SYSUAF.DAT
 *           within -> the file FID. The node member has no [SYS0.SYSEXE] (the
 *           shared files live under the COMMON root), so the walk FALLS THROUGH
 *           it to the common member -- the search-list order a running system
 *           disk presents.
 *       (3) $GET / IO$_READVBLK reads SYSUAF.DAT byte-exact off the ODS-2
 *           volume -- the exact path LOGINOUT needs, with NO vmsfs_to_linux_path.
 *
 * This is the real-/dev/vms half of vms-0044; the pure lnm+$PARSE composition is
 * proven host-side by tests/vmsrms/test_dirlogical_compose.c. The composition
 * itself is product code (vmsfs_compose_ods2_candidates); the ACP directory
 * walk is driven here directly over the KIF ACP helpers (vms_kif_acp_assign /
 * _access / _readvb), exactly as test_syssvc_acp_access drives them -- the
 * footing every ACP-QIO rung sits on until the RMS-over-ACP $OPEN flip wires it
 * into resolve_filename (held capstone; see the PR notes).
 *
 * GROUND TRUTH. DKA300: (vdd) carries the generated system-disk fixture
 * (tests/qemu/mkimage_ods2_sysvol.c, staged /ods2_sysvol.img). SYSUAF.DAT is a
 * 512-byte RFM=FIXED file whose bytes are 0x5A ^ (i*3) -- the writer wrote them
 * verbatim, so IO$_READVBLK reads them back unchanged (byte-exact discriminator).
 *
 * FAIL-HONEST (INV-6): a spec whose directory level is missing resolves
 * RMS$_DNF (directory not found); a spec whose final file is missing resolves
 * RMS$_FNF (file not found) -- never a POSIX fallback, never a fabricated FID.
 *
 * NO /dev/vms -> honest SKIP (77): the ACP, the directory walk and the read are
 * executive-resident, so with no /dev/vms there is nothing to assert.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "rmsdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "vms/logical.h"
#include "vmsfs/filespec.h"
#include "vmsfs/device.h"

#define EXIT_SKIP 77
#define ODS2_UNIT  "DKA300:"     /* vdd: the generated system-disk fixture */

/* The known SYSUAF.DAT bytes (kept in sync with mkimage_ods2_sysvol.c). */
#define SYSUAF_LEN 512
#define SYSUAF_BYTE(i) ((uint8_t)(0x5A ^ ((i) * 3u)))

#define ODS2_FH2_M_DIRECTORY 0x2000u

static int pass = 0, fail = 0;
static void check(int cond, const char *name)
{
    if (cond) { printf("  PASS: %s\n", name); pass++; }
    else      { printf("  FAIL: %s\n", name); fail++; }
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* Seed the concealed-rooted system logicals, pointed at ODS2_UNIT, into the
 * process table -- the exact shape lnm_setup_defaults() seeds, but on the test
 * fixture device rather than the boot default DKA0:. Process-table scope needs
 * no executive LNM writes and is what the compose path reads (PROCESS first). */
static void seed_system_logicals(lnm_manager_t *mgr)
{
    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$SYSDEVICE", ODS2_UNIT,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);
    static const char *sysroot[] = {
        "SYS$SYSDEVICE:[SYS0.]",
        "SYS$SYSDEVICE:[SYS0.SYSCOMMON.]"
    };
    lnm_create_multi(mgr, LNM_PROCESS_TABLE, "SYS$SYSROOT", sysroot, 2,
                     LNM_ATTR_CONCEALED, LNM_MODE_EXEC);
    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$SYSTEM",  "SYS$SYSROOT:[SYSEXE]", 0, LNM_MODE_EXEC);
    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$MANAGER", "SYS$SYSROOT:[SYSMGR]", 0, LNM_MODE_EXEC);
    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$LIBRARY", "SYS$SYSROOT:[SYSLIB]", 0, LNM_MODE_EXEC);
}

/*
 * Walk ONE fully-composed spec "DEV:[A.B.C]NAME.TYP;VER" through the Files-11
 * ACP: $ASSIGN DEV, then MFD -> A.DIR -> B.DIR -> C.DIR -> NAME.TYP by name.
 * On success returns SS$_NORMAL, fills *out_fid_num/seq/nmx, and leaves *chan
 * ASSIGNED with the file ACCESSED (caller reads then deaccesses/dassgns). On
 * failure returns RMS$_DNF (a missing directory component) or RMS$_FNF (missing
 * final file) and releases the channel.
 */
static uint32_t walk_spec(const char *spec, uint32_t *chan,
                          uint16_t *out_fid_num, uint16_t *out_fid_seq,
                          uint8_t *out_fid_nmx, uint16_t *out_efblk)
{
    char dev[VMSFS_MAX_DEVICE + 8] = "";
    char dir[VMSFS_MAX_DIRECTORY + 1] = "";
    char file[VMSFS_MAX_NAME + VMSFS_MAX_TYPE + 8] = "";
    uint16_t want_ver = 0;

    /* --- parse DEV:[DIR]NAME.TYP;VER by hand --- */
    const char *p = spec;
    const char *colon = strchr(p, ':');
    const char *lb = strchr(p, '[');
    const char *rb = lb ? strchr(lb, ']') : NULL;
    if (!colon || !lb || !rb) return RMS$_FNF;
    size_t dl = (size_t)(colon - p);
    if (dl >= sizeof(dev)) dl = sizeof(dev) - 1;
    memcpy(dev, p, dl); dev[dl] = '\0';
    strncat(dev, ":", sizeof(dev) - strlen(dev) - 1);
    size_t dirl = (size_t)(rb - lb - 1);
    if (dirl >= sizeof(dir)) dirl = sizeof(dir) - 1;
    memcpy(dir, lb + 1, dirl); dir[dirl] = '\0';
    const char *fp = rb + 1;
    const char *semi = strchr(fp, ';');
    if (semi) {
        want_ver = (uint16_t)atoi(semi + 1);
        size_t fl = (size_t)(semi - fp);
        if (fl >= sizeof(file)) fl = sizeof(file) - 1;
        memcpy(file, fp, fl); file[fl] = '\0';
    } else {
        strncpy(file, fp, sizeof(file) - 1);
    }

    /* --- $ASSIGN a file channel to the volume --- */
    uint32_t st = vms_kif_acp_assign(dev, chan);
    if (!$VMS_STATUS_SUCCESS(st) || *chan == 0)
        return RMS$_DNF;   /* volume not mountable/assignable -> honest not-found */

    /* --- walk the directory chain from the MFD --- */
    struct vms_acp_access_args a;
    uint16_t did_num = 0;   /* 0 => MFD [000000] */
    uint8_t  did_nmx = 0;
    char *save = NULL;
    for (char *comp = strtok_r(dir, ".", &save); comp; comp = strtok_r(NULL, ".", &save)) {
        char dirname[VMS_ACP_NAME_SIZE];
        snprintf(dirname, sizeof(dirname), "%s.DIR", comp);
        memset(&a, 0, sizeof(a));
        a.chan = *chan;
        a.did_num = did_num;
        a.did_nmx = did_nmx;
        a.version = 1;                    /* directories are ;1 */
        strncpy(a.name, dirname, VMS_ACP_NAME_SIZE - 1);
        st = vms_kif_acp_access(&a);
        if (!$VMS_STATUS_SUCCESS(st) ||
            !(a.attr.filechar & ODS2_FH2_M_DIRECTORY)) {
            (void)vms_kif_dassgn(*chan);
            *chan = 0;
            return RMS$_DNF;              /* directory component not found */
        }
        did_num = a.fid_num;
        did_nmx = a.fid_nmx;
        (void)vms_kif_acp_deaccess(*chan);
    }

    /* --- resolve the file in the final directory (leave it accessed) --- */
    memset(&a, 0, sizeof(a));
    a.chan = *chan;
    a.did_num = did_num;
    a.did_nmx = did_nmx;
    a.version = want_ver;                 /* 0 => highest */
    strncpy(a.name, file, VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_access(&a);
    if (!$VMS_STATUS_SUCCESS(st)) {
        (void)vms_kif_dassgn(*chan);
        *chan = 0;
        return RMS$_FNF;                  /* directory resolved, file absent */
    }
    if (out_fid_num) *out_fid_num = a.fid_num;
    if (out_fid_seq) *out_fid_seq = a.fid_seq;
    if (out_fid_nmx) *out_fid_nmx = a.fid_nmx;
    if (out_efblk)   *out_efblk   = (uint16_t)a.attr.efblk;
    return SS$_NORMAL;
}

/*
 * Resolve a DIRECTORY LOGICAL filespec (SYS$SYSTEM:SYSUAF.DAT) the full way:
 * compose the candidates, then walk each in search order -- first that resolves
 * wins. If none resolve, return the strongest honest error seen (RMS$_FNF if any
 * candidate reached its final directory, else RMS$_DNF).
 */
static uint32_t resolve_logical(const char *filespec, uint32_t *chan,
                                uint16_t *fid_num, uint16_t *fid_seq,
                                uint8_t *fid_nmx, uint16_t *efblk,
                                int *which_candidate)
{
    char cands[LNM_MAX_SEARCHLIST][VMSFS_MAX_FILESPEC];
    int n = vmsfs_compose_ods2_candidates(filespec, cands, LNM_MAX_SEARCHLIST);
    if (n <= 0)
        return RMS$_DNF;

    uint32_t worst = RMS$_DNF;
    for (int i = 0; i < n; i++) {
        uint32_t st = walk_spec(cands[i], chan, fid_num, fid_seq, fid_nmx, efblk);
        if ($VMS_STATUS_SUCCESS(st)) {
            if (which_candidate) *which_candidate = i;
            return SS$_NORMAL;
        }
        if (st == RMS$_FNF)
            worst = RMS$_FNF;   /* a dir resolved but the file was absent */
    }
    return worst;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dirlogical_acp: rooted/concealed directory logicals "
           "resolve through lnm+$PARSE+ACP (vms-0044, epic vms-208) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (!executive_present()) {
        printf("=== test_syssvc_dirlogical_acp: 0 passed, 0 failed (SKIPPED: no "
               "/dev/vms -- the ACP directory walk and the read are executive-resident) ===\n");
        return EXIT_SKIP;
    }

    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { printf("  FAIL: no LNM manager\n"); return 1; }
    seed_system_logicals(mgr);

    uint32_t st = vms_kif_acp_mount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st),
          "$MOUNT of the generated system-disk ODS-2 volume on " ODS2_UNIT " (precondition)");
    if (!$VMS_STATUS_SUCCESS(st)) {
        printf("=== test_syssvc_dirlogical_acp: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    /* --- (A) SYS$SYSTEM:SYSUAF.DAT resolves through the full chain --------- */
    {
        uint32_t chan = 0;
        uint16_t fnum = 0, fseq = 0, efblk = 0;
        uint8_t  fnmx = 0;
        int which = -1;
        uint32_t rs = resolve_logical("SYS$SYSTEM:SYSUAF.DAT", &chan,
                                      &fnum, &fseq, &fnmx, &efblk, &which);
        /* negctl: dirlogical-compose-drops-common-member */
        check($VMS_STATUS_SUCCESS(rs) && chan != 0,
              "SYS$SYSTEM:SYSUAF.DAT resolves through lnm+$PARSE+ACP to a file FID");
        check($VMS_STATUS_SUCCESS(rs) && which == 1,
              "the walk FELL THROUGH the empty node member and resolved on the COMMON member (search order)");
        check($VMS_STATUS_SUCCESS(rs) && efblk >= 1,
              "the resolved SYSUAF.DAT reports a real end-of-file VBN");

        /* Read VBN 1 and compare byte-exact against the known pattern. */
        if ($VMS_STATUS_SUCCESS(rs) && chan != 0) {
            uint8_t buf[SYSUAF_LEN];
            memset(buf, 0, sizeof(buf));
            struct vms_acp_rw_args r;
            memset(&r, 0, sizeof(r));
            r.chan = chan; r.vbn = 1; r.offset = 0; r.length = SYSUAF_LEN;
            r.buffer = (uint64_t)(uintptr_t)buf;
            uint32_t rst = vms_kif_acp_readvb(&r);
            int exact = 1;
            for (int i = 0; i < SYSUAF_LEN; i++)
                if (buf[i] != SYSUAF_BYTE(i)) { exact = 0; break; }
            check($VMS_STATUS_SUCCESS(rst) && r.xferred == SYSUAF_LEN && exact,
                  "$GET/IO$_READVBLK reads SYSUAF.DAT byte-exact off the ODS-2 volume (no vmsfs_to_linux_path)");
            (void)vms_kif_acp_deaccess(chan);
            (void)vms_kif_dassgn(chan);
        }
    }

    /* --- (B) SYS$MANAGER:WELCOME.TXT resolves (a second directory logical) - */
    {
        uint32_t chan = 0;
        uint16_t fnum = 0, fseq = 0, efblk = 0;
        uint8_t  fnmx = 0;
        int which = -1;
        uint32_t rs = resolve_logical("SYS$MANAGER:WELCOME.TXT", &chan,
                                      &fnum, &fseq, &fnmx, &efblk, &which);
        check($VMS_STATUS_SUCCESS(rs) && chan != 0,
              "SYS$MANAGER:WELCOME.TXT resolves through the same rooted chain to a file FID");
        if (chan != 0) { (void)vms_kif_acp_deaccess(chan); (void)vms_kif_dassgn(chan); }
    }

    /* --- (C) FAIL-HONEST: file present nowhere -> RMS$_FNF ----------------- */
    {
        uint32_t chan = 0;
        uint16_t fnum = 0, fseq = 0, efblk = 0; uint8_t fnmx = 0; int which = -1;
        uint32_t rs = resolve_logical("SYS$SYSTEM:NOSUCH.DAT", &chan,
                                      &fnum, &fseq, &fnmx, &efblk, &which);
        check(rs == RMS$_FNF && chan == 0,
              "SYS$SYSTEM:NOSUCH.DAT (dir resolves, file absent) is RMS$_FNF (fail-honest, no POSIX fallback)");
    }

    /* --- (D) FAIL-HONEST: a missing directory level -> RMS$_DNF ------------ */
    {
        uint32_t chan = 0;
        uint16_t fnum = 0, fseq = 0, efblk = 0; uint8_t fnmx = 0;
        /* Walk a concrete spec whose middle directory does not exist. */
        char spec[VMSFS_MAX_FILESPEC];
        snprintf(spec, sizeof(spec), "%s[SYS0.NOSUCH.SYSEXE]SYSUAF.DAT", ODS2_UNIT);
        uint32_t rs = walk_spec(spec, &chan, &fnum, &fseq, &fnmx, &efblk);
        check(rs == RMS$_DNF && chan == 0,
              "a spec with a missing directory level (...[SYS0.NOSUCH.SYSEXE]...) is RMS$_DNF (fail-honest)");
    }

    /* --- cleanup ---------------------------------------------------------- */
    st = vms_kif_acp_dmount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$DISMOUNT removes the ODS-2 volume");

    printf("=== test_syssvc_dirlogical_acp: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
