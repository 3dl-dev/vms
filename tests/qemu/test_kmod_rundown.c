/*
 * test_kmod_rundown.c - SYS$RUNDWN image-scoped resource release, proven
 * against a real /dev/vms (vms-68f increment v).
 *
 * THE PROPERTY. On OpenVMS, image rundown -- the executive step that runs when
 * an activated image exits (SYS$EXIT from user mode) -- does not merely switch
 * the process back to Supervisor mode: it RELEASES the resources the image
 * owned (the channels it $ASSIGNed, the locks it $ENQed, the ASTs it declared,
 * all at USER access mode) while PROCESS-PERMANENT state owned at an inner mode
 * (Supervisor/Exec/Kernel) survives, so DCL resumes with its state intact. This
 * is the resource-level half of "P0/image state dies at rundown, P1 survives"
 * (docs/design-in-process-activation.md Part II §A.2.1 step 2, §A.6.1 -- the
 * design's flagged "hard part"). Grounding for which class is image-scoped vs
 * process-permanent: docs/design-image-rundown-resource-classes.md.
 *
 * WHAT THIS SUITE ASSERTS, against real vms.ko:
 *   1. A lock $ENQed from USER mode (inside the image) is DEQUEUED by image
 *      rundown -- its lock ID is SS$_IVLOCKID afterward.  [negctl anchor]
 *   2. A lock $ENQed from SUPER mode (before the descent) SURVIVES -- still
 *      dequeueable. (Proves the release is image-scoped, not a blunderbuss.)
 *   3. A channel $ASSIGNed from USER mode is DEASSIGNED by rundown -- SS$_IVCHAN
 *      afterward; a SUPER-mode channel survives.
 *   4. A USER-mode AST declared inside the image does NOT survive rundown --
 *      the queue is drained.
 *   5. The P1 control-region extent registered before the image is UNCHANGED
 *      after rundown -- process-permanent, exactly as it must be.
 *
 * INV-6 (CLAUDE.md Rule 9): this drives the real executive through /dev/vms.
 * With no vms.ko (NEGATIVE_CONTROL build), vms_kif_open() fails and main()
 * reports the failure at its first line -- there is no per-process fallback.
 *
 * This process runs privileged (init.sh execs it as root), so it holds
 * CMEXEC/CMKRNL and may SETMODE(SUPER) and ENTER_IMAGE; the enforcement that
 * an UNprivileged/User-mode caller cannot do those is test_syssvc_modeswitch.c's
 * job, not this suite's.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>

#include "vms_ioctl.h"
#include "vms_kif.h"

/* SS$_ status codes (matching the kernel module, src/kernel/vms_internal.h). */
#define SS_NORMAL     1
#define SS_IVCHAN     602
#define SS_IVLOCKID   8484
#define SS_NOSUCHDEV  2680

#define P1_WIN_SIZE   (64UL * 1024)

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

int main(void)
{
    uint32_t status, my_vms_pid = 0;
    uint32_t chan_perm = 0, chan_img = 0;
    uint32_t lkid_perm = 0, lkid_img = 0;
    uint8_t  prev = 0xFF, cur = 0xFF, mode = 0xFF;
    uint64_t ast_adr = 0; uint64_t ast_prm = 0; uint8_t ast_mode = 0xFF;
    struct vms_procinfo info;
    struct vms_devinfo  dinfo;
    void *p1_win;
    uint64_t p1_base, p1_limit;

    /* Line-buffer so the SUITE tally lines survive interleaving in the guest. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== test_kmod_rundown: SYS$RUNDWN image-scoped release (vms-68f.v) ===\n");

    if (vms_kif_open() < 0) {
        printf("  FAIL: cannot open /dev/vms (executive absent)\n");
        printf("=== test_kmod_rundown: 0 passed, 1 failed ===\n");
        return 1;
    }
    if (vms_kif_register(&my_vms_pid) != SS_NORMAL) {
        printf("  FAIL: cannot register with the executive\n");
        printf("=== test_kmod_rundown: 0 passed, 1 failed ===\n");
        return 1;
    }

    /* A privileged process may raise to Supervisor -- everything created now is
     * process-permanent (inner mode), the state that must survive rundown. */
    CHECK(vms_kif_setmode(PSL_C_SUPER) == SS_NORMAL,
          "raise to Supervisor mode (privileged process)");
    CHECK(vms_kif_getmode(&mode, NULL, NULL) == SS_NORMAL && mode == PSL_C_SUPER,
          "current mode is Supervisor before the image is entered");

    /* Register a real P1 control-region window: it must be intact after RUN. */
    p1_win = mmap(NULL, P1_WIN_SIZE, PROT_NONE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p1_win != MAP_FAILED, "reserved a real P1 window in this address space");
    p1_base  = (uint64_t)(uintptr_t)p1_win;
    p1_limit = p1_base + P1_WIN_SIZE;
    CHECK(vms_kif_p1_map(p1_base, p1_limit) == SS_NORMAL,
          "registered the P1 control-region extent");

    /* Process-permanent resources, taken at Supervisor mode. */
    CHECK(vms_kif_assign("OPA0:", &chan_perm) == SS_NORMAL,
          "assigned a Supervisor-mode channel to OPA0: (process-permanent)");
    status = vms_kif_enq(1, LCK_K_EXMODE, 0, "OVMX_RUNDWN_PERM", 0,
                         0, 0, 0, &lkid_perm, NULL);
    CHECK(status == SS_NORMAL && lkid_perm != 0,
          "enqueued a Supervisor-mode lock (process-permanent)");

    /* ---- descend into the image (Supervisor -> User) ------------------- */
    CHECK(vms_kif_enter_image(&prev, &cur) == SS_NORMAL &&
          prev == PSL_C_SUPER && cur == PSL_C_USER,
          "ENTER_IMAGE: descended Supervisor -> User");

    /* Image-scoped resources, taken at User mode -- these must NOT survive. */
    CHECK(vms_kif_assign("OPA0:", &chan_img) == SS_NORMAL,
          "the image assigned a User-mode channel to OPA0:");
    status = vms_kif_enq(1, LCK_K_EXMODE, 0, "OVMX_RUNDWN_IMG", 0,
                         0, 0, 0, &lkid_img, NULL);
    CHECK(status == SS_NORMAL && lkid_img != 0,
          "the image enqueued a User-mode lock");
    (void)vms_kif_setast(1);   /* enable User-mode AST delivery */
    CHECK(vms_kif_dclast(0xDEAD, 0x1234, PSL_C_USER) == SS_NORMAL,
          "the image declared a User-mode AST");

    /* ---- image exit: rundown returns to Supervisor and releases the image ---- */
    prev = cur = 0xFF;
    CHECK(vms_kif_image_rundown(&prev, &cur) == SS_NORMAL &&
          prev == PSL_C_USER && cur == PSL_C_SUPER,
          "IMAGE_RUNDOWN: returned User -> Supervisor");
    CHECK(vms_kif_getmode(&mode, NULL, NULL) == SS_NORMAL && mode == PSL_C_SUPER,
          "DCL's Supervisor mode is restored after rundown");

    /* ==== THE PROPERTY: image-scoped state gone, process-permanent kept ==== */

    /* 1. the image's User-mode lock was dequeued by rundown */
    /* negctl: image-rundown-leaks-user-lock */
    CHECK(vms_kif_deq(lkid_img, NULL, 0) == SS_IVLOCKID,
          "image rundown DEQUEUED the User-mode lock (its lkid is now invalid)");

    /* 2. the Supervisor-mode lock survived (dequeuing it now cleans it up) */
    CHECK(vms_kif_deq(lkid_perm, NULL, 0) == SS_NORMAL,
          "the Supervisor-mode lock SURVIVED image rundown (still dequeueable)");

    /* 3. the image's User-mode channel was deassigned; the Supervisor one kept */
    memset(&dinfo, 0, sizeof(dinfo));
    CHECK(vms_kif_getdvi_chan(chan_img, &dinfo) == SS_IVCHAN,
          "image rundown DEASSIGNED the User-mode channel (SS$_IVCHAN)");
    memset(&dinfo, 0, sizeof(dinfo));
    CHECK(vms_kif_getdvi_chan(chan_perm, &dinfo) == SS_NORMAL,
          "the Supervisor-mode channel SURVIVED image rundown");

    /* 4. the image's User-mode AST did not survive (queue drained) */
    CHECK(vms_kif_deliverast(&ast_adr, &ast_prm, &ast_mode) == -1,
          "no User-mode AST survived image rundown (the queue was drained)");

    /* 5. the P1 control region is process-permanent -- untouched by rundown */
    memset(&info, 0, sizeof(info));
    CHECK(vms_kif_getjpi_self(&info) == SS_NORMAL &&
          info.p1_base == p1_base && info.p1_limit == p1_limit,
          "the P1 control-region extent SURVIVED image rundown intact");

    vms_kif_close();
    printf("=== test_kmod_rundown: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
