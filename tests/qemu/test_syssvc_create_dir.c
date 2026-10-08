/*
 * test_syssvc_create_dir.c - LIB$CREATE_DIR over the executive's Files-11 ACP
 * (vms-44a; corpus program lib_create_dir).
 *
 * Against a writable real-VAX ODS-2 fixture on VDA0: the process default directory is
 * set to [OVMXDIR] ($SETDDIR's executive-held store) and LIB$CREATE_DIR is called with
 * a RELATIVE spec, a protection mask/value, a version limit and an initial allocation.
 * What the ACP then holds is read back through IO$_ACCESS:
 *
 *   - the call returns SS$_CREATED; a second identical call SS$_NORMAL (existed);
 *   - the new file is a DIRECTORY, carries the requested protection bits (the rest
 *     from the process default), the requested owner UIC, a FAT version limit of 7
 *     and 3 allocated blocks;
 *   - a deeper spec creates the missing parents too ([.PARENT.CHILD]);
 *   - an initial allocation of 200 blocks (the corpus program's value) is honoured:
 *     the directory holds 200 blocks, uses one, and grows into the preallocated ones
 *     as entries are added -- its allocation does not change;
 *   - a default directory on a concealed rooted SEARCH LIST (SYSTEM's is
 *     SYS$SYSROOT:[SYSMGR]) completes a relative spec in the member that exists;
 *   - a zero initial allocation or a non-zero relative volume is refused
 *     SS$_BADPARAM.
 *
 * Every directory it creates is deleted again. No /dev/vms -> honest SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "lib$routines.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "vms/logical.h"

#define EXIT_SKIP 77
#define ODS2_UNIT "VDA0:"
#define OVMXDIR_FID_NUM 11u
#define MFD_FID_NUM 4u
#define BIG_ALLOC 200u
#define BIG_ENTRIES 50
#define DIRFLAG 0x2000u

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

static uint32_t cdir(const char *spec, const uint32_t *owner, const uint32_t *pe,
                     const uint32_t *pv, const uint32_t *mv, const uint32_t *rvn,
                     const uint32_t *ia)
{
    struct dsc$descriptor_s d = { (unsigned short)strlen(spec), DSC$K_DTYPE_T,
                                  DSC$K_CLASS_S, (char *)spec };
    return lib$create_dir(&d, owner, pe, pv, mv, rvn, ia);
}

static uint32_t access_in(uint32_t chan, uint16_t did, const char *name,
                          struct vms_acp_access_args *a)
{
    memset(a, 0, sizeof(*a));
    a->chan = chan;
    a->did_num = did;
    a->did_seq = 1;
    a->version = 1;
    strncpy(a->name, name, VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_access(a);
}

static uint32_t delete_in(uint32_t chan, uint16_t did, const char *name)
{
    struct vms_acp_fileop_args f;
    memset(&f, 0, sizeof(f));
    f.chan = chan;
    f.func = VMS_ACP_FOP_DELETE;
    f.modifiers = VMS_ACP_M_DELETE;
    f.did_num = did;
    f.did_seq = 1;
    f.version = 1;
    strncpy(f.name, name, VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_fileop(&f);
}

int main(void)
{
    uint32_t st, chan = 0;
    struct vms_acp_access_args a, b;
    uint32_t pe = 0xFF00u, pv = 0xAA00u, mv = 7, ia = 3, owner = (5u << 16) | 6u;
    uint32_t big = BIG_ALLOC, zero = 0, rvn1 = 1;
    uint16_t newd = 0, parent = 0, bigd = 0, rsub = 0;
    int i, made = 0, found = 0;

    printf("=== test_syssvc_create_dir: LIB$CREATE_DIR over the ACP ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_create_dir: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    st = vms_kif_acp_mount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$MOUNT of the writable ODS-2 " ODS2_UNIT);
    st = vms_kif_acp_assign(ODS2_UNIT, &chan);
    check($VMS_STATUS_SUCCESS(st) && chan != 0, "$ASSIGN a file-class channel");
    st = vms_kif_ddir("VDA0:[OVMXDIR]", NULL, 0);
    check($VMS_STATUS_SUCCESS(st), "the executive default directory is VDA0:[OVMXDIR]");

    /* -- a relative spec with every argument ---------------------------------- */
    st = cdir("[.NEWD]", &owner, &pe, &pv, &mv, NULL, &ia);
    check(st == SS$_CREATED, "LIB$CREATE_DIR [.NEWD] returns SS$_CREATED");
    st = access_in(chan, OVMXDIR_FID_NUM, "NEWD.DIR", &a);
    check($VMS_STATUS_SUCCESS(st), "NEWD.DIR exists under [OVMXDIR]");
    newd = a.fid_num;
    check(a.attr.filechar & DIRFLAG, "it is a directory file");
    /* masked bits come from the value; the rest from the process default protection */
    /* negctl: libcreatedir-protection-ignored */
    check((a.attr.fileprot & 0xFF00u) == (pv & 0xFF00u),
          "the directory carries the requested protection bits");
    check(a.attr.uic_group == 5 && a.attr.uic_member == 6, "the directory is owned by the requested UIC [5,6]");
    /* negctl: acp-fat-versions-not-applied */
    check(a.attr.recattr[30] == 7 && a.attr.recattr[31] == 0, "the directory's version limit is 7");
    /* negctl: acp-dir-exsz-ignored */
    check(a.attr.hiblk == 3, "the new directory holds the requested 3 blocks");
    (void)vms_kif_acp_deaccess(chan);

    st = cdir("[.NEWD]", NULL, NULL, NULL, NULL, NULL, NULL);
    check(st == SS$_NORMAL, "a second LIB$CREATE_DIR of [.NEWD] returns SS$_NORMAL (already there)");

    /* -- a directory inside the directory the call made still works ------------ */
    st = cdir("[.NEWD.INNER]", NULL, NULL, NULL, NULL, NULL, NULL);
    check(st == SS$_CREATED, "a directory can be created inside the preallocated one");

    /* -- missing parents -------------------------------------------------------- */
    st = cdir("[.PARENT.CHILD]", NULL, NULL, NULL, NULL, NULL, NULL);
    check(st == SS$_CREATED, "[.PARENT.CHILD] creates both levels");
    st = access_in(chan, OVMXDIR_FID_NUM, "PARENT.DIR", &b);
    check($VMS_STATUS_SUCCESS(st), "the missing parent PARENT.DIR was made");
    parent = b.fid_num;
    (void)vms_kif_acp_deaccess(chan);

    /* -- a large initial allocation, then growth into it ----------------------- */
    st = cdir("[.BIGD]", NULL, NULL, NULL, NULL, NULL, &big);
    check(st == SS$_CREATED, "an initial allocation of 200 blocks is SS$_CREATED");
    st = access_in(chan, OVMXDIR_FID_NUM, "BIGD.DIR", &b);
    check($VMS_STATUS_SUCCESS(st) && b.attr.hiblk == BIG_ALLOC,
          "BIGD.DIR holds the 200 blocks asked for");
    check(b.attr.efblk == 2, "BIGD.DIR uses one of them (end of file at VBN 2)");
    bigd = b.fid_num;
    (void)vms_kif_acp_deaccess(chan);
    for (i = 0; i < BIG_ENTRIES; i++) {
        char sp[32];
        snprintf(sp, sizeof(sp), "[.BIGD.D%02d]", i);
        if (cdir(sp, NULL, NULL, NULL, NULL, NULL, NULL) == SS$_CREATED)
            made++;
    }
    /* negctl: acp-dir-used-blocks-ignore-eof */
    check(made == BIG_ENTRIES, "50 directories are entered in BIGD.DIR");
    for (i = 0; i < BIG_ENTRIES; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "D%02d.DIR", i);
        if ($VMS_STATUS_SUCCESS(access_in(chan, bigd, nm, &b))) {
            found++;
            (void)vms_kif_acp_deaccess(chan);
        }
    }
    check(found == BIG_ENTRIES, "every one of them is found again by name");
    st = access_in(chan, OVMXDIR_FID_NUM, "BIGD.DIR", &b);
    check($VMS_STATUS_SUCCESS(st) && b.attr.efblk > 2,
          "BIGD.DIR grew past its first block");
    check(b.attr.hiblk == BIG_ALLOC,
          "it grew into its preallocated blocks: the allocation is still 200");
    (void)vms_kif_acp_deaccess(chan);

    /* -- a default directory on a concealed rooted search list ------------------ */
    st = cdir("VDA0:[OVMXDIR.RSUB]", NULL, NULL, NULL, NULL, NULL, NULL);
    check(st == SS$_CREATED, "VDA0:[OVMXDIR.RSUB] is created by an absolute spec");
    st = access_in(chan, OVMXDIR_FID_NUM, "RSUB.DIR", &b);
    rsub = b.fid_num;
    (void)vms_kif_acp_deaccess(chan);
    {
        static const char *roots[] = { "VDA0:[NOROOT.]", "VDA0:[OVMXDIR.]" };
        lnm_manager_t *mgr = lnm_get_manager();
        if (mgr)
            lnm_create_multi(mgr, LNM_PROCESS_TABLE, "CDIR$ROOT", roots, 2,
                             LNM_ATTR_CONCEALED, LNM_MODE_EXEC);
        check(mgr != NULL, "CDIR$ROOT = VDA0:[NOROOT.],VDA0:[OVMXDIR.] (concealed, rooted)");
    }
    st = vms_kif_ddir("CDIR$ROOT:[RSUB]", NULL, 0);
    check($VMS_STATUS_SUCCESS(st), "the default directory is CDIR$ROOT:[RSUB]");
    st = cdir("[.LEAF]", NULL, NULL, NULL, NULL, NULL, NULL);
    /* negctl: libcreatedir-rooted-default-unresolved */
    check(st == SS$_CREATED, "LIB$CREATE_DIR [.LEAF] under a rooted default is SS$_CREATED");
    st = access_in(chan, rsub, "LEAF.DIR", &b);
    check($VMS_STATUS_SUCCESS(st), "LEAF.DIR is in [OVMXDIR.RSUB], the member that exists");
    if ($VMS_STATUS_SUCCESS(st))
        (void)vms_kif_acp_deaccess(chan);
    st = access_in(chan, MFD_FID_NUM, "NOROOT.DIR", &b);
    check(st == SS$_NOSUCHFILE, "nothing was made under the missing first member");
    (void)vms_kif_ddir("VDA0:[OVMXDIR]", NULL, 0);

    /* -- refusals --------------------------------------------------------------- */
    st = cdir("[.ZERO]", NULL, NULL, NULL, NULL, NULL, &zero);
    check(st == SS$_BADPARAM, "an initial allocation of 0 blocks is SS$_BADPARAM");
    st = access_in(chan, OVMXDIR_FID_NUM, "ZERO.DIR", &b);
    check(st == SS$_NOSUCHFILE, "the refused call created nothing");
    st = cdir("[.RVN1]", NULL, NULL, NULL, NULL, &rvn1, NULL);
    check(st == SS$_BADPARAM, "a non-zero relative volume is SS$_BADPARAM");

    /* -- clean up --------------------------------------------------------------- */
    (void)delete_in(chan, newd, "INNER.DIR");
    check($VMS_STATUS_SUCCESS(delete_in(chan, OVMXDIR_FID_NUM, "NEWD.DIR")), "delete NEWD.DIR (restore)");
    (void)delete_in(chan, parent, "CHILD.DIR");
    check($VMS_STATUS_SUCCESS(delete_in(chan, OVMXDIR_FID_NUM, "PARENT.DIR")), "delete PARENT.DIR (restore)");
    for (i = 0; i < BIG_ENTRIES; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "D%02d.DIR", i);
        (void)delete_in(chan, bigd, nm);
    }
    check($VMS_STATUS_SUCCESS(delete_in(chan, OVMXDIR_FID_NUM, "BIGD.DIR")), "delete BIGD.DIR (restore)");
    (void)delete_in(chan, rsub, "LEAF.DIR");
    check($VMS_STATUS_SUCCESS(delete_in(chan, OVMXDIR_FID_NUM, "RSUB.DIR")), "delete RSUB.DIR (restore)");

    (void)vms_kif_dassgn(chan);
    (void)vms_kif_acp_dmount(ODS2_UNIT);
    printf("=== test_syssvc_create_dir: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
