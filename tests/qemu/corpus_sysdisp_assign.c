/*
 * corpus_sysdisp_assign.c — vms-sys corpus ladder, RUNG-1 subject (vms-a2b).
 *
 * THE JOIN GATE. OVMX has two proven-but-DISJOINT halves:
 *   - ACTIVATION: run_*_native.sh + the QEMU image-activation suites prove an
 *     OVMX-native image (LINK.EXE --executable --use {shareables}, PT_INTERP=
 *     IMGACT.EXE, NO ld/ld.so) activates and runs.
 *   - DISPATCH: the test_syssvc_*.c family proves libvms's SYS$ services
 *     dispatch through vms_kif_* into the executive over /dev/vms.
 * NOTHING joined them: the dispatch suites are gcc/ld-linked ctests (not
 * activated), and the activation consumers make no SYS$ calls. vms-sys's whole
 * point ("a REAL VMS image activates AND its SYS$ calls dispatch into OVMX
 * services") is exactly that join. THIS subject is the first rung: an
 * IMGACT-ACTIVATED OVMX-native image that makes a genuine, EXECUTIVE-BACKED SYS$
 * call and proves it dispatched into the executive.
 *
 * WHY $ASSIGN OF THE BOOT UNIT (not $GETTIM). sys$gettim is OVMX-userspace
 * (clock_gettime; src/libvms/syssvc/sys_time.c) — it never touches /dev/vms, so
 * it could not distinguish an executive from its absence and could not
 * fail-honest. $ASSIGN of the ODS-2 boot unit (VDA0:/SYS$SYSDEVICE) is
 * executive-backed: it consults the executive's GLOBAL mount table over the ACP
 * (src/libvms/syssvc/sys_assign.c is_file -> vms_kif_acp_assign), so its answer
 * depends on executive state no local Linux fd could fake:
 *   - executive ABSENT            -> SS$_NOSUCHDEV  (vms-03b; we honest-skip)
 *   - executive present, UNMOUNTED-> SS$_DEVNOTMOUNT (fail-honest, INV-6: NEVER
 *                                    a fabricated channel — the negctl anchor)
 *   - executive present, MOUNTED  -> SS$_NORMAL + a nonzero executive channel
 * That mount-table sensitivity is the proof the call reached the executive; it
 * needs only PUBLIC symbols (no internal vms$$chan_is_file), so it links against
 * the shareables by symbol vector exactly as a real activated image does.
 *
 * OUTPUT IS THE EXIT CODE (the proven minimal-activated-consumer contract, like
 * src/imgact/test/run_libvmssys_native.sh's consumer): this image is activated
 * with only the shareables it --uses, so it keeps zero hard I/O assumptions and
 * hands its verdict back as a small, DIAGNOSTIC exit status the harness maps:
 *     0            proof passed (DEVNOTMOUNT before mount; NORMAL+chan after)
 *     EXIT_SKIP 77 no executive (pre-mount $ASSIGN was SS$_NOSUCHDEV)
 *     RC_PCB   10  vms_pcb_init failed (cannot make the channel table)
 *     RC_FABRIC 11 pre-mount $ASSIGN did NOT fail-honest (fabricated/other) — INV-6
 *     RC_MOUNT 12  $MOUNT of the boot unit failed
 *     RC_ASSIGN 13 post-mount $ASSIGN did not return SS$_NORMAL + nonzero channel
 *     RC_DASSGN 14 $DASSGN failed
 *     RC_DMOUNT 15 $DISMOUNT (restore unmounted state for the next suite) failed
 * (printf is deliberately NOT used — the harness, an ld-linked ctest with a full
 * C runtime, prints the human-readable CHECK lines from the mapped exit code.)
 *
 * BOUNDARY (vms-c09f discipline): this is a SUBJECT + harness + staging only — no
 * link.c / imgact.c edits. A LINK/IMGACT gap that surfaces here is a separate
 * operator-gated item, never smuggled in.
 */
#include <stdint.h>

#include "starlet.h"     /* sys$assign, sys$dassgn */
#include "ssdef.h"       /* SS$_NORMAL / SS$_NOSUCHDEV / SS$_DEVNOTMOUNT */
#include "descrip.h"     /* struct dsc$descriptor_s, DSC$K_DTYPE_T / DSC$K_CLASS_S */
#include "vms_kif.h"     /* vms_kif_acp_mount / vms_kif_acp_dmount */
#include "vms/pcb.h"     /* vms_pcb_init */

/* The ODS-2 boot unit under device-native default naming (epic vms-47d); the
 * same unit test_syssvc_acp_channel.c asserts an executive file channel to. */
#define BOOT_UNIT     "VDA0:"
#define BOOT_UNIT_LEN 5              /* strlen("VDA0:"), avoids libc in -ffreestanding */

#define EXIT_SKIP  77
#define RC_PCB     10
#define RC_FABRIC  11
#define RC_MOUNT   12
#define RC_ASSIGN  13
#define RC_DASSGN  14
#define RC_DMOUNT  15

int main(void)
{
    /* A per-process PCB holds the channel table $ASSIGN/$DASSGN operate on. A
     * real activated image (DCL.EXE, dcl_main.c) makes its own; do the same. */
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL))
        return RC_PCB;

    struct dsc$descriptor_s dev;
    dev.dsc$w_length  = BOOT_UNIT_LEN;
    dev.dsc$b_dtype   = DSC$K_DTYPE_T;
    dev.dsc$b_class   = DSC$K_CLASS_S;
    dev.dsc$a_pointer = (char *)BOOT_UNIT;

    uint16_t chan = 0;

    /* (1) $ASSIGN before any mount. Its status BOTH tells us whether an executive
     *     is present AND is the INV-6 fail-honest assertion. */
    uint32_t st = sys$assign(&dev, &chan, 0, NULL);
    if (st == SS$_NOSUCHDEV)
        return EXIT_SKIP;                 /* no /dev/vms — honest-skip */
    if (st != SS$_DEVNOTMOUNT)
        return RC_FABRIC;                 /* fabricated a channel / wrong status */

    /* (2) $MOUNT the boot unit into the executive-global table. */
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_mount(BOOT_UNIT)))
        return RC_MOUNT;

    /* (3) $ASSIGN now dispatches into the executive and returns a channel. */
    chan = 0;
    st = sys$assign(&dev, &chan, 0, NULL);
    if (!$VMS_STATUS_SUCCESS(st) || chan == 0)
        return RC_ASSIGN;

    /* (4) $DASSGN releases the executive channel. */
    if (!$VMS_STATUS_SUCCESS(sys$dassgn(chan)))
        return RC_DASSGN;

    /* (5) $DISMOUNT restores the unmounted precondition the NEXT suite in this
     *     booted VM expects: $MOUNT is executive-GLOBAL, and the rail convention
     *     (test_syssvc_acp_channel.c asserts SS$_DEVNOTMOUNT as its first act, then
     *     dismounts at the end) is "each suite leaves the fixture unmounted." A
     *     subject that left it mounted would break that sibling's opening assertion. */
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_dmount(BOOT_UNIT)))
        return RC_DMOUNT;

    return 0;
}
