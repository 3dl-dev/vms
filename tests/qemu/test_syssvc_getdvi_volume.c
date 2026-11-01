/*
 * test_syssvc_getdvi_volume.c - $GETDVI answers the mounted-volume items from
 * the executive's mounted-volume table (vms-eed), through the PUBLIC sys$
 * API, and compares them against the executive's own GETVOL answer.
 *
 * THE GAP THIS CLOSES. DVI$_MAXBLOCK / FREEBLOCKS / VOLNAM / CLUSTER /
 * MOUNTCNT fell through sys$getdvi's default arm and came back 0 / empty even
 * for a mounted ODS-2 volume, while F$GETDVI (which reads the same table)
 * answered. A $GETDVI caller was told a volume with a label and blocks had
 * neither. The values now come from vms_kif_getvol, and an unmounted unit
 * still answers 0 / empty -- never a host figure.
 *
 * Needs the real executive; with none it exits EXIT_SKIP (77), as the
 * kernel-executive negative-control job requires of every public-sys$ suite.
 * NEGATIVE CONTROL: tests/qemu/defects.d/getdvi-volume-items-zero.defect.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "dvidef.h"
#include "stsdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77
#define ODS2_UNIT "VDA0:"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static uint32_t getdvi(const char *devnam, uint16_t code, void *buf, uint16_t buflen,
                       uint16_t *retlen)
{
    struct dsc$descriptor_s d = { (uint16_t)strlen(devnam), DSC$K_DTYPE_T,
                                  DSC$K_CLASS_S, (char *)devnam };
    uint16_t rl = 0;
    struct item_list_3 il[2] = { { buflen, code, buf, &rl }, { 0, 0, NULL, NULL } };
    uint32_t st = sys$getdvi(0, 0, &d, il, NULL, NULL, 0, 0);
    if (retlen) *retlen = rl;
    return st;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_getdvi_volume (mounted-volume DVI$ items) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (vms_kif_open() < 0 || !(ovmx_test_self(NULL) & 1)) {
        printf("=== test_syssvc_getdvi_volume: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }

    uint32_t st = vms_kif_acp_mount(ODS2_UNIT);
    CHECK($VMS_STATUS_SUCCESS(st), "$MOUNT of the genuine ODS-2 " ODS2_UNIT " (precondition)");

    struct vms_getvol_args gv;
    memset(&gv, 0, sizeof gv);
    st = vms_kif_getvol(ODS2_UNIT, &gv);
    CHECK($VMS_STATUS_SUCCESS(st) && gv.mounted && gv.volsize > 0 && gv.volnam[0],
          "the executive reports a mounted volume with a size and a label (ground truth)");

    uint32_t maxblock = 0, freeblocks = 0, cluster = 0, mountcnt = 0;
    char volnam[32];
    uint16_t vlen = 0;
    memset(volnam, 0, sizeof volnam);
    CHECK(getdvi(ODS2_UNIT, DVI$_MAXBLOCK, &maxblock, 4, NULL) == SS$_NORMAL, "$GETDVI MAXBLOCK succeeds");
    CHECK(getdvi(ODS2_UNIT, DVI$_FREEBLOCKS, &freeblocks, 4, NULL) == SS$_NORMAL, "$GETDVI FREEBLOCKS succeeds");
    CHECK(getdvi(ODS2_UNIT, DVI$_CLUSTER, &cluster, 4, NULL) == SS$_NORMAL, "$GETDVI CLUSTER succeeds");
    CHECK(getdvi(ODS2_UNIT, DVI$_MOUNTCNT, &mountcnt, 4, NULL) == SS$_NORMAL, "$GETDVI MOUNTCNT succeeds");
    CHECK(getdvi(ODS2_UNIT, DVI$_VOLNAM, volnam, sizeof volnam - 1, &vlen) == SS$_NORMAL, "$GETDVI VOLNAM succeeds");

    /* negctl: getdvi-volume-items-zero */
    CHECK(maxblock == gv.volsize && maxblock > 0,
          "$GETDVI MAXBLOCK equals the volume size the executive holds");
    /* negctl: getdvi-volume-items-zero */
    CHECK(gv.free_valid && freeblocks == gv.freeblocks,
          "$GETDVI FREEBLOCKS equals the executive's free-block count");
    /* negctl: getdvi-volume-items-zero */
    CHECK(vlen == strlen(gv.volnam) && vlen > 0 && memcmp(volnam, gv.volnam, vlen) == 0,
          "$GETDVI VOLNAM is the mounted volume's label");
    /* negctl: getdvi-volume-items-zero */
    CHECK(cluster == gv.cluster && cluster > 0,
          "$GETDVI CLUSTER is the storage-bitmap cluster factor");
    /* negctl: getdvi-volume-items-zero */
    CHECK(mountcnt == 1, "$GETDVI MOUNTCNT is 1 for a mounted volume");

    /* an unmounted unit: honest zero / empty, never a fabricated volume */
    uint32_t t_max = 0xFFFFFFFFu, t_mnt = 0xFFFFFFFFu;
    uint16_t t_vlen = 0xFFFF;
    char t_vol[16] = "x";
    (void)getdvi("OPA0:", DVI$_MAXBLOCK, &t_max, 4, NULL);
    (void)getdvi("OPA0:", DVI$_MOUNTCNT, &t_mnt, 4, NULL);
    (void)getdvi("OPA0:", DVI$_VOLNAM, t_vol, sizeof t_vol - 1, &t_vlen);
    CHECK(t_max == 0 && t_mnt == 0 && t_vlen == 0,
          "an unmounted unit (OPA0:) answers MAXBLOCK 0, MOUNTCNT 0, VOLNAM empty");

    (void)vms_kif_acp_dmount(ODS2_UNIT);
    printf("=== test_syssvc_getdvi_volume: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
