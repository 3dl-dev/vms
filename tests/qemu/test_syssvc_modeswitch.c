/*
 * test_syssvc_modeswitch.c - Access-mode transition + boundary enforcement
 * (vms-68f.iii, increment (iii) of the Option A in-process image
 * activation design, docs/design-in-process-activation.md Part II
 * §A.1.2, §A.1.3, §A.2.3).
 *
 * WHAT THIS PROVES, AND WHAT IT DOES NOT. Increments (i)/(ii) gave the
 * executive P0/P1 bookkeeping; this increment gives it the CHMx/REI-
 * equivalent mode-transition primitive (VMS_IOCTL_ENTER_IMAGE /
 * VMS_IOCTL_IMAGE_RUNDOWN) and closes a gap in the pre-existing
 * VMS_IOCTL_SETMODE (raising into Supervisor required no privilege at
 * all before this item -- see vms_access.c). It does NOT activate a real
 * image into P0, does NOT do real image rundown, and does NOT implement
 * per-page four-mode hardware protection (Intel MPK/PKU is the deferred
 * post-1.0 ceiling, vms-978) -- those are increments (iv)/(v) and a
 * later item respectively. What THIS suite proves against a real
 * /dev/vms:
 *
 *   1. A CONTROLLED MODE TRANSITION CHANGES THE EXECUTIVE'S CURRENT MODE
 *      AND IS OBSERVABLE: a privileged caller raises SUPER->... no --
 *      raises itself to SUPER via SETMODE, then ENTER_IMAGE moves it to
 *      USER, both changes visible via GETMODE.
 *   2. THE ENFORCEMENT: a caller that never legitimately reached
 *      Supervisor (no CMEXEC/CMKRNL) cannot raise its own mode there via
 *      SETMODE, cannot ENTER_IMAGE (which requires already being in
 *      Supervisor), and cannot IMAGE_RUNDOWN its way to Supervisor either
 *      (which requires a matching ENTER_IMAGE already open). Three
 *      distinct doors, all refused SS$_NOPRIV, none of them privilege-
 *      escalation-by-a-different-route.
 *   3. CRITICAL-P1 PAGES ARE PROTECTED WHILE IN USER MODE: a real
 *      mprotect(2) (vms_kif_p1_protect()) makes a P1-shaped region
 *      read-only for the duration of the entered image, and a wild write
 *      to it genuinely faults (SIGSEGV) instead of corrupting DCL's
 *      state -- restored writable at rundown.
 *   4. HONEST NO-EXECUTIVE FAIL: without /dev/vms this program fails at
 *      the first open, the same INV-6 shape every other test_kmod_*
 *      suite in this directory asserts.
 *
 * NEGATIVE CONTROL RIG: under NEGATIVE_CONTROL=1 (tests/qemu/Dockerfile
 * boots without insmod'ing vms.ko) there is no /dev/vms to open and this
 * program fails at the first line of main() saying so -- no per-process
 * fallback (INV-6).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "vms_ioctl.h"
#include "vms_kif.h"

/* SS$_ status codes (matching kernel module / vms_errno.h). */
#define SS_NORMAL   1
#define SS_NOPRIV   36

/* Credentials for the unprivileged half. setuid() away from root is what
 * makes capable(CAP_SYS_ADMIN) genuinely false in the executive -- same
 * rig as test_kmod_access.c, deliberately different uid/gid so the two
 * suites' children can never collide if ever run concurrently. */
#define C_GID   302
#define C_UID   1003

/* test_syssvc_* device-absent contract (vms-d40, ci.yml kernel-executive
 * negative control): with no /dev/vms present the executive is absent and
 * this suite MUST exit exactly 77 (honest SKIP), never 0 and never a plain 1.
 * Reachable only on the executive-absent rig. */
#define EXIT_SKIP   77

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* What an unprivileged child reports back. */
struct door_report {
    uint32_t registered;
    uint32_t status;        /* status of the one door this child tries */
    uint8_t  mode_after;
};

/*
 * EACH DOOR GETS ITS OWN FRESHLY-REGISTERED PROCESS, DELIBERATELY.
 *
 * An earlier draft of this suite ran all three doors sequentially against
 * ONE unprivileged registration. That coupling means a mutation that
 * breaks door 1 (e.g. lets SETMODE(SUPER) through) leaves that SAME
 * process actually IN Supervisor mode when door 2 runs, so door 2 (ENTER_
 * IMAGE, which only checks "am I in Supervisor") ALSO succeeds -- and a
 * successful ENTER_IMAGE sets image_active, so door 3 (IMAGE_RUNDOWN) then
 * ALSO succeeds. One defect, one process, three doors falling in a chain
 * that has nothing to do with door 2 or door 3's OWN enforcement --
 * exactly the blunderbuss shape facility_defects.sh's own header exists to
 * forbid. Three independent, freshly-registered processes keep each
 * door's mutation confined to that door's own assertions, which is why
 * super-mode-escalation reddens ONLY the door-1 child below and
 * image-rundown-without-entry reddens ONLY the door-3 child.
 */
typedef uint32_t (*door_fn)(uint8_t *prev, uint8_t *new_mode);

static uint32_t door_setmode_super(uint8_t *prev, uint8_t *new_mode)
{
    uint32_t st = vms_kif_setmode(PSL_C_SUPER);
    uint8_t mode = 0xFF;
    (void)vms_kif_getmode(&mode, NULL, NULL);
    if (prev) *prev = PSL_C_USER;
    if (new_mode) *new_mode = mode;
    return st;
}

static uint32_t door_enter_image_cold(uint8_t *prev, uint8_t *new_mode)
{
    uint8_t p = 0xFF, n = 0xFF;
    uint32_t st = vms_kif_enter_image(&p, &n);
    uint8_t mode = 0xFF;
    (void)vms_kif_getmode(&mode, NULL, NULL);
    if (prev) *prev = p;
    if (new_mode) *new_mode = mode;
    return st;
}

static uint32_t door_rundown_cold(uint8_t *prev, uint8_t *new_mode)
{
    uint8_t p = 0xFF, n = 0xFF;
    uint32_t st = vms_kif_image_rundown(&p, &n);
    uint8_t mode = 0xFF;
    (void)vms_kif_getmode(&mode, NULL, NULL);
    if (prev) *prev = p;
    if (new_mode) *new_mode = mode;
    return st;
}

static int door_child(int wfd, door_fn door)
{
    struct door_report r;
    uint32_t vms_pid;
    uint8_t new_mode = 0xFF;

    memset(&r, 0, sizeof(r));

    if (setgid(C_GID) != 0)
        _exit(70);
    if (setuid(C_UID) != 0)
        _exit(71);
    if (getuid() == 0 || geteuid() == 0)
        _exit(72);

    if (vms_kif_open() < 0)
        _exit(73);

    if (vms_kif_register(&vms_pid) != SS_NORMAL)
        _exit(74);
    r.registered = SS_NORMAL;

    r.status = door(NULL, &new_mode);
    r.mode_after = new_mode;

    if (write(wfd, &r, sizeof(r)) != (ssize_t)sizeof(r))
        _exit(75);
    return 0;
}

/* Fork one door_child(), collect its door_report, waitpid() it. Returns 1
 * and fills *out on success, 0 (with a printed FAIL) otherwise. */
static int run_door(door_fn door, struct door_report *out)
{
    int c2p[2];
    pid_t child;

    if (pipe(c2p) < 0) {
        printf("  FAIL: pipe()\n");
        fail++;
        return 0;
    }
    child = fork();
    if (child < 0) {
        printf("  FAIL: fork()\n");
        fail++;
        return 0;
    }
    if (child == 0) {
        close(c2p[0]);
        _exit(door_child(c2p[1], door));
    }
    close(c2p[1]);

    memset(out, 0, sizeof(*out));
    if (read(c2p[0], out, sizeof(*out)) != (ssize_t)sizeof(*out)) {
        printf("  FAIL: door child never reported "
               "(credential drop or /dev/vms open failed)\n");
        fail++;
        close(c2p[0]);
        waitpid(child, NULL, 0);
        return 0;
    }
    close(c2p[0]);
    waitpid(child, NULL, 0);
    return 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* line-buffer stdout across fork(), same rationale as test_kmod_access.c */

    struct vms_register_args reg;
    uint8_t mode, prev, cur;
    void *p1region;
    size_t p1len = 4096;

    printf("=== test_syssvc_modeswitch ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_modeswitch: 0 passed, 0 failed "
               "(SKIPPED: no /dev/vms -- executive absent) ===\n");
        return EXIT_SKIP;
    }

    memset(&reg, 0, sizeof(reg));
    CHECK(vms_kif_register(&reg.vms_pid) == SS_NORMAL, "register process");

    /* ---- (1) THE CONTROLLED TRANSITION CHANGES CURRENT MODE, OBSERVABLY ---- */

    CHECK(vms_kif_getmode(&mode, NULL, NULL) == SS_NORMAL && mode == PSL_C_USER,
          "a process starts in USER mode");

    CHECK(vms_kif_setmode(PSL_C_SUPER) == SS_NORMAL,
          "SUPER mode ALLOWED with CMEXEC/CMKRNL");
    CHECK(vms_kif_getmode(&mode, NULL, NULL) == SS_NORMAL && mode == PSL_C_SUPER,
          "... and the mode really changed to SUPER");

    p1region = mmap(NULL, p1len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p1region != MAP_FAILED, "mmap a P1-shaped critical region");
    if (p1region != MAP_FAILED)
        *(volatile int *)p1region = 0x50313031; /* "P101" sentinel, pre-protect */

    prev = cur = 0xFF;
    CHECK(vms_kif_enter_image(&prev, &cur) == SS_NORMAL,
          "ENTER_IMAGE succeeds from SUPER");
    CHECK(prev == PSL_C_SUPER && cur == PSL_C_USER,
          "... reporting the SUPER->USER transition");
    CHECK(vms_kif_getmode(&mode, NULL, NULL) == SS_NORMAL && mode == PSL_C_USER,
          "... and GETMODE confirms USER after ENTER_IMAGE");

    /* ---- (3) CRITICAL-P1 PROTECTION WHILE THE IMAGE (USER MODE) RUNS ---- */

    if (p1region != MAP_FAILED) {
        CHECK(vms_kif_p1_protect((uint64_t)(uintptr_t)p1region,
                                  (uint64_t)(uintptr_t)p1region + p1len, 0) == SS_NORMAL,
              "critical P1 region protected read-only while the image runs");

        pid_t writer = fork();
        if (writer < 0) {
            CHECK(0, "a wild write to the protected critical P1 page faults instead of corrupting DCL");
        } else if (writer == 0) {
            /* A wild write from the "image" into DCL's protected P1
             * page. This must fault -- if it does not, the region
             * was never really protected. */
            *(volatile int *)p1region = 0xBADBADBA;
            _exit(0); /* only reached if the write did NOT fault */
        } else {
            int wstatus = 0;
            waitpid(writer, &wstatus, 0);
            CHECK(WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGSEGV,
                  "a wild write to the protected critical P1 page faults instead of corrupting DCL");
        }
    }

    /* ---- IMAGE RUNDOWN: THE PAIRED RETURN ---- */

    prev = cur = 0xFF;
    CHECK(vms_kif_image_rundown(&prev, &cur) == SS_NORMAL,
          "IMAGE_RUNDOWN succeeds for the process that legitimately entered");
    CHECK(prev == PSL_C_USER && cur == PSL_C_SUPER,
          "... reporting the USER->SUPER transition");
    CHECK(vms_kif_getmode(&mode, NULL, NULL) == SS_NORMAL && mode == PSL_C_SUPER,
          "... and GETMODE confirms SUPER after IMAGE_RUNDOWN");

    if (p1region != MAP_FAILED) {
        CHECK(vms_kif_p1_protect((uint64_t)(uintptr_t)p1region,
                                  (uint64_t)(uintptr_t)p1region + p1len, 1) == SS_NORMAL,
              "critical P1 region restored writable at rundown");
        *(volatile int *)p1region = 0x50313032; /* "P102": must NOT fault now */
        CHECK(*(volatile int *)p1region == 0x50313032,
              "... and a write to it after rundown genuinely succeeds");
    }

    CHECK(vms_kif_image_rundown(&prev, &cur) == SS_NOPRIV,
          "a second IMAGE_RUNDOWN with no intervening ENTER_IMAGE is refused");
    CHECK(vms_kif_getmode(&mode, NULL, NULL) == SS_NORMAL && mode == PSL_C_SUPER,
          "... and the mode is unaffected by the refused replay");

    /* ---- (2) THE ENFORCEMENT: an unprivileged caller cannot use ANY of
     * the three doors to reach Supervisor -- each door tried by its OWN
     * freshly-registered process, see run_door()'s header comment for why. */

    struct door_report d1, d2, d3;

    if (run_door(door_setmode_super, &d1)) {
        CHECK(d1.registered == SS_NORMAL,
              "an unprivileged process may register with the executive");
        /* negctl: super-mode-escalation */
        CHECK(d1.status == SS_NOPRIV,
              "SUPER mode DENIED without CMEXEC/CMKRNL (SS$_NOPRIV)");
        CHECK(d1.mode_after == PSL_C_USER,
              "... and the mode is still USER after the denied SUPER escalation");
    }

    if (run_door(door_enter_image_cold, &d2)) {
        CHECK(d2.status == SS_NOPRIV,
              "ENTER_IMAGE refused from USER mode without first reaching SUPER");
        CHECK(d2.mode_after == PSL_C_USER,
              "... and the mode is still USER after the refused ENTER_IMAGE");
    }

    if (run_door(door_rundown_cold, &d3)) {
        /* negctl: image-rundown-without-entry */
        CHECK(d3.status == SS_NOPRIV,
              "IMAGE_RUNDOWN refused with no active image entry");
        CHECK(d3.mode_after == PSL_C_USER,
              "... and the mode is still USER after the refused IMAGE_RUNDOWN -- no door reaches Supervisor without the controlled transition");
    }

    printf("=== test_syssvc_modeswitch: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
