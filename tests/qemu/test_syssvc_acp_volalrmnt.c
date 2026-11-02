/*
 * test_syssvc_acp_volalrmnt.c - $MOUNT refuses a SECOND volume carrying the
 * label of one already mounted (rd vms-3ea), against a real /dev/vms.
 *
 * WHY. The ODS-2 volume label is the Files-11 lock namespace -- the executive
 * names a volume's locks "F11B$<kind><label>" -- and the volume's name. Two
 * DIFFERENT units mounted under one label would sit behind one set of lock
 * names, and every structure write on either would serialize against the
 * other. VMS's MOUNT refuses the second with %MOUNT-F-VOLALRMNT, "another
 * volume of same label already mounted". OVMX's ACP returns that condition as
 * SS$_DUPLNAM (vmsfs_acp.c ACP_SS_VOLALRMNT: the MOUNT-facility value is not
 * one this tree carries, so the grounded SS$ value stands in and DCL MOUNT
 * prints VMS's text).
 *
 * THE DISKS. vdc (VDA200:/DKA200:) is the generated $SEARCH volume, label
 * OVMXSEARCH. vdg (VDA600:/DKA600:) is a SECOND copy of that image
 * (run_tests.sh): a different unit with the same label.
 *
 * WHAT THIS PROVES:
 *   1. With VDA200: mounted, $MOUNT of VDA600: is refused SS$_DUPLNAM, and
 *      records nothing (its $DISMOUNT is SS$_NOSUCHDEV).
 *   2. The refusal is the LABEL, not the media: with VDA200: dismounted the
 *      very same VDA600: mounts -- and then VDA200: is the one refused.
 *   3. Remounting the SAME unit stays idempotent (SS$_NORMAL), never VOLALRMNT,
 *      and a volume with a different label mounts beside it.
 * The negative control acp-mount-dup-label-accepted deletes the label check;
 * (1) and the symmetric half of (2) then go red.
 *
 * The cluster half of the rule (a LOCAL disk whose label another member
 * already has mounted) needs two members and is proven by
 * tests/qemu/test_cluster_config_lan_2node_e2e.sh.
 *
 * NO /dev/vms -> honest SKIP (77).
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

#define EXIT_SKIP 77

static int pass = 0;
static int fail = 0;

static void check(int cond, const char *name)
{
    if (cond) { printf("  PASS: %s\n", name); pass++; }
    else      { printf("  FAIL: %s\n", name); fail++; }
}

/* The first of `units` the executive knows (VDAn: on Linux, DKAn: on Alpha). */
static const char *pick(const char *const units[2])
{
    char backing[32];
    uint32_t maj = 0, min = 0;
    int i;

    for (i = 0; i < 2; i++)
        if (vms_kif_disk_resolve(units[i], backing, sizeof(backing), &maj, &min) == SS$_NORMAL)
            return units[i];
    return NULL;
}

int main(void)
{
    static const char *const first_u[2]  = { "VDA200:", "DKA200:" };
    static const char *const second_u[2] = { "VDA600:", "DKA600:" };
    static const char *const other_u[2]  = { "VDA400:", "DKA400:" };
    const char *first, *second, *other;
    uint32_t st;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_acp_volalrmnt: $MOUNT refuses a second volume of an "
           "already-mounted label (rd vms-3ea) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    {
        int fd = vms_kif_open();
        if (fd < 0) {
            printf("=== test_syssvc_acp_volalrmnt: 0 passed, 0 failed "
                   "(SKIPPED: no /dev/vms -- the mount table is executive-resident) ===\n");
            return EXIT_SKIP;
        }
        vms_kif_close();
    }

    first  = pick(first_u);
    second = pick(second_u);
    other  = pick(other_u);
    check(first != NULL, "the OVMXSEARCH volume is attached (VDA200:/DKA200:, the third disk)");
    check(second != NULL, "its second copy is attached (VDA600:/DKA600:, the seventh disk)");
    check(other != NULL, "the OVMXIMGACT volume is attached (VDA400:/DKA400:)");
    if (!first || !second || !other) {
        printf("=== test_syssvc_acp_volalrmnt: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    /* A suite before this one may have left either mounted; start from a
     * known state (NOSUCHDEV = was not mounted, also fine). */
    (void)vms_kif_acp_dmount(second);
    (void)vms_kif_acp_dmount(first);

    /* --- (1) the second volume of a mounted label is refused -------------- */
    st = vms_kif_acp_mount(first);
    check(st == SS$_NORMAL, "$MOUNT of the OVMXSEARCH volume on its first unit succeeds");

    st = vms_kif_acp_mount(second);
    /* negctl: acp-mount-dup-label-accepted */
    check(st == SS$_DUPLNAM,
          "$MOUNT of a SECOND unit carrying the mounted label OVMXSEARCH is refused (VOLALRMNT, SS$_DUPLNAM)");
    st = vms_kif_acp_dmount(second);
    check(st == SS$_NOSUCHDEV,
          "the refused second unit was not recorded ($DISMOUNT is SS$_NOSUCHDEV)");

    /* The same unit again is the idempotent case, never VOLALRMNT. */
    st = vms_kif_acp_mount(first);
    check(st == SS$_NORMAL, "remounting the SAME unit stays idempotent (SS$_NORMAL), not VOLALRMNT");

    /* A different label mounts beside it. */
    st = vms_kif_acp_mount(other);
    check(st == SS$_NORMAL, "a volume with a DIFFERENT label (OVMXIMGACT) mounts beside it");

    /* --- (2) it is the label, not the media ------------------------------- */
    st = vms_kif_acp_dmount(first);
    check(st == SS$_NORMAL, "$DISMOUNT frees the label");
    st = vms_kif_acp_mount(second);
    check(st == SS$_NORMAL,
          "with the label free, the very unit that was refused mounts (the refusal was the label)");
    st = vms_kif_acp_mount(first);
    check(st == SS$_DUPLNAM,
          "and now the FIRST unit is the one refused while the second holds the label");

    /* --- cleanup ----------------------------------------------------------- */
    st = vms_kif_acp_dmount(second);
    check(st == SS$_NORMAL, "cleanup: $DISMOUNT the second unit");

    printf("=== test_syssvc_acp_volalrmnt: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
