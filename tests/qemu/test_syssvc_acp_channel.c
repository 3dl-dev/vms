/*
 * test_syssvc_acp_channel.c - the Files-11 (ODS-2) ACP assigns an
 * EXECUTIVE-backed file channel to a mounted volume (vms-149, epic vms-208),
 * proven against a real /dev/vms.
 *
 * This is the second rung of the executive ACP (after the kernel-resident codec
 * vms-dcd): the CHANNEL front-end. It exercises, through the PUBLIC sys$ API and
 * the executive mount ioctls, exactly the outcome vms-149 owns:
 *
 *   1. $ASSIGN of the boot unit while NO volume is mounted fails honestly with
 *      SS$_DEVNOTMOUNT -- never a fabricated channel to a volume the executive
 *      does not have (CLAUDE.md Rule 9 / INV-6). This is DISTINCT from the
 *      SS$_NOSUCHDEV that only an ABSENT /dev/vms yields (vms-03b): reaching the
 *      $ASSIGN handler proves the executive is present, so "not mounted" must
 *      not masquerade as "no executive."
 *   2. After $MOUNT records the unit as an ODS-2 volume in the executive-global
 *      table, $ASSIGN returns an EXECUTIVE-backed channel of the file class
 *      (PCB_CHAN_FILE / vms$$chan_is_file), NOT a Linux fd.
 *   3. $DASSGN releases it; the channel is reusable.
 *   4. $DISMOUNT removes the volume, and $ASSIGN is once again SS$_DEVNOTMOUNT.
 *
 * WHY A MOUNT STEP HERE. On real OpenVMS a file channel is $ASSIGNed to a
 * MOUNTED volume's device (VSI I/O User's Reference, "ACP-QIO Interface"); the
 * mounted-volume table is executive-global (design §4.3), so this suite plays
 * the role PID 1's boot-time $MOUNT will later fill (a fast-follow rung of
 * epic vms-208). The ACP-QIO file OPERATIONS (IO$_ACCESS/READVBLK/...) are NOT
 * in this rung -- it establishes the channel those operations will ride.
 *
 * NO /dev/vms -> honest SKIP (77), never a fake pass: with no executive the
 * mount table does not exist and $ASSIGN of the boot unit cannot return a
 * channel, so there is nothing to assert -- the contract every test_syssvc_*
 * suite is held to (.github/workflows/ci.yml).
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

/*
 * The boot unit under the device-native default naming (epic vms-47d). DKA0:
 * (vda) carries a GENUINE real-VAX ODS-2 volume in the QEMU harness
 * (tests/qemu/run_tests.sh seeds it from the staged /ods2_real.img fixture),
 * because $MOUNT now VALIDATES the media is genuine Files-11 ODS-2 (vms-127) and
 * $ASSIGN routes the boot unit to the ACP (sys_assign.c). The validation itself
 * is proven by test_syssvc_acp_mount; here it is only the precondition for a
 * mounted volume to $ASSIGN a file channel to.
 */
#define ACP_BOOT_UNIT "DKA0:"

/*
 * Internal channel-class reader, declared extern exactly as sys_qio.c declares
 * its siblings (vms$$chan_is_mailbox / vms$$chan_is_bg): these are internal
 * helpers, not public system services. This one proves the channel $ASSIGN
 * handed back is the EXECUTIVE's file channel, not a Linux-fd-backed one.
 */
extern int vms$$chan_is_file(uint16_t chan);

static int pass = 0;
static int fail = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  PASS: %s\n", name);
        pass++;
    } else {
        printf("  FAIL: %s\n", name);
        fail++;
    }
}

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

/* Probe only -- vms_kif_open()/close() decide skip-vs-run and nothing else. */
static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

int main(void)
{
    struct dsc$descriptor_s dev = mkdsc(ACP_BOOT_UNIT);
    uint16_t chan = 0;
    uint32_t st;

    printf("=== test_syssvc_acp_channel: executive ACP file channel to a "
           "mounted ODS-2 volume (vms-149, epic vms-208) ===\n");

    /*
     * A per-process PCB is a prerequisite for every sys$ channel call (the
     * channel table $ASSIGN/$DASSGN operate on lives in it); a gcc/musl test
     * binary must make its own -- see test_syssvc_bg_echo.c /
     * test_syssvc_mbx_crossproc.c. Without it vms_pcb_get() is NULL and
     * sys$assign returns SS$_BADPARAM before it ever reaches the ACP path.
     */
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        printf("=== test_syssvc_acp_channel: 0 passed, 0 failed "
               "(SKIPPED: no /dev/vms -- the ACP mount table and file channel "
               "are executive-resident) ===\n");
        return EXIT_SKIP;
    }

    /* --- (1) FAIL-HONEST: no volume mounted -> SS$_DEVNOTMOUNT ------------
     * This is the INV-6 heart of the rung: an $ASSIGN of the boot unit when
     * the executive has no mounted volume for it must refuse, never fabricate
     * a channel. The negctl acp-assign-unmounted-fabricates-channel injects
     * exactly the mutation (return SS$_NORMAL instead) this assertion catches. */
    chan = 0;
    st = sys$assign(&dev, &chan, 0, NULL);
    /* negctl: acp-assign-unmounted-fabricates-channel */
    check(st == SS$_DEVNOTMOUNT,
          "$ASSIGN of an UNMOUNTED boot unit returns SS$_DEVNOTMOUNT -- fail-honest, no fabricated channel");

    /* --- (2) $MOUNT the boot unit into the executive-global table --------- */
    st = vms_kif_acp_mount(ACP_BOOT_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$MOUNT records " ACP_BOOT_UNIT " as an ODS-2 volume in the executive");

    /* Idempotent: a second $MOUNT of the same unit is still success. */
    st = vms_kif_acp_mount(ACP_BOOT_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$MOUNT of an already-mounted unit is idempotent (SS$_NORMAL)");

    /* --- (3) $ASSIGN now yields an EXECUTIVE-backed file channel ---------- */
    chan = 0;
    st = sys$assign(&dev, &chan, 0, NULL);
    check($VMS_STATUS_SUCCESS(st) && chan != 0,
          "$ASSIGN of the mounted boot unit returns a channel (SS$_NORMAL)");
    check(vms$$chan_is_file(chan),
          "the assigned channel is an EXECUTIVE file channel (PCB_CHAN_FILE), not a Linux fd");

    /* --- (4) $DASSGN releases it ------------------------------------------ */
    st = sys$dassgn(chan);
    check($VMS_STATUS_SUCCESS(st), "$DASSGN releases the file channel");
    check(!vms$$chan_is_file(chan), "the channel slot is no longer a file channel after $DASSGN");

    /* --- (5) the channel is reusable (re-$ASSIGN, re-$DASSGN) ------------- */
    chan = 0;
    st = sys$assign(&dev, &chan, 0, NULL);
    check($VMS_STATUS_SUCCESS(st) && chan != 0,
          "re-$ASSIGN of the mounted boot unit succeeds (channel reusable)");
    st = sys$dassgn(chan);
    check($VMS_STATUS_SUCCESS(st), "re-$DASSGN releases the file channel");

    /* --- (6) $DISMOUNT, then $ASSIGN is fail-honest again ----------------- */
    st = vms_kif_acp_dmount(ACP_BOOT_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$DISMOUNT removes the volume from the executive table (no channels assigned)");

    chan = 0;
    st = sys$assign(&dev, &chan, 0, NULL);
    check(st == SS$_DEVNOTMOUNT,
          "after $DISMOUNT, $ASSIGN of the boot unit is SS$_DEVNOTMOUNT again (volume gone)");

    printf("=== test_syssvc_acp_channel: %d passed, %d failed ===\n",
           pass, fail);
    return fail > 0 ? 1 : 0;
}
