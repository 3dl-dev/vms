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
 *   - an initial allocation a directory cannot hold, or a non-zero relative volume,
 *     is refused SS$_BADPARAM and creates nothing.
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

#define EXIT_SKIP 77
#define ODS2_UNIT "VDA0:"
#define OVMXDIR_FID_NUM 11u
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
    uint32_t big = 65, rvn1 = 1;
    uint16_t newd = 0, parent = 0;

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

    /* -- refusals --------------------------------------------------------------- */
    st = cdir("[.TOOBIG]", NULL, NULL, NULL, NULL, NULL, &big);
    check(st == SS$_BADPARAM, "an initial allocation of 65 blocks is SS$_BADPARAM");
    st = access_in(chan, OVMXDIR_FID_NUM, "TOOBIG.DIR", &b);
    check(st == SS$_NOSUCHFILE, "the refused call created nothing");
    st = cdir("[.RVN1]", NULL, NULL, NULL, NULL, &rvn1, NULL);
    check(st == SS$_BADPARAM, "a non-zero relative volume is SS$_BADPARAM");

    /* -- clean up --------------------------------------------------------------- */
    (void)delete_in(chan, newd, "INNER.DIR");
    check($VMS_STATUS_SUCCESS(delete_in(chan, OVMXDIR_FID_NUM, "NEWD.DIR")), "delete NEWD.DIR (restore)");
    (void)delete_in(chan, parent, "CHILD.DIR");
    check($VMS_STATUS_SUCCESS(delete_in(chan, OVMXDIR_FID_NUM, "PARENT.DIR")), "delete PARENT.DIR (restore)");

    (void)vms_kif_dassgn(chan);
    (void)vms_kif_acp_dmount(ODS2_UNIT);
    printf("=== test_syssvc_create_dir: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
