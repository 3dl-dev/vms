/*
 * test_kmod_ast.c - Test AST operations via /dev/vms ioctl
 *
 * Tests:
 *   1. Register process
 *   2. Declare an AST (DCLAST)
 *   3. Enable/disable AST delivery (SETAST)
 *   4. Verify previous state return values
 *
 * Note: Actual AST delivery to userspace requires signal handling
 * which is more complex to test. Here we test the ioctl interface
 * and queue management.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "vms_ioctl.h"
#include "vms_kif.h"

/* ORACLE-PINNED (vms-68c), docs/oracle/vax73-event-flags.md: $SSDEF gives
 * SS$_WASCLR 1 on the reference lab VAX V7.3 -- the same value as
 * SS$_NORMAL, which is VMS. Was 5 here, matching the kernel's old value,
 * which the same oracle shows is not a status at all (F$MESSAGE(5) =
 * %NONAME-?-NOMSG). */
#define SS_NORMAL   1
#define SS_WASSET   9
#define SS_WASCLR   1

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

/* Dummy AST handler address (won't actually be called in kernel tests) */
static void dummy_ast(uint32_t param) { (void)param; }

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: keep stdout line-buffered even when init.sh redirects it to a file, so an unflushed fork() cannot splice output */
    printf("=== test_kmod_ast ===\n");

    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) {
        printf("  FAIL: cannot open /dev/vms\n");
        return 1;
    }

    /* Register */
    struct vms_register_args reg = {0};
    reg.vms_pid = (uint32_t)getpid();
    /* NO init_privs (vms-2b8): VMS_IOCTL_REGISTER no longer carries a
     * privilege mask. It used to, and this line asked for all 64 bits --
     * a process declaring its own privileges, which is the honor system
     * that item removed. The executive now DERIVES the mask from the
     * task's real credentials, and nothing below needs a privilege. */
    ioctl(fd, VMS_IOCTL_REGISTER, &reg);
    CHECK(reg.status == SS_NORMAL, "register");

    /*
     * 1-5. CONVERTED (vms-290): these five call sites used to be raw
     * ioctl(VMS_IOCTL_SETAST/DCLAST) against the local `fd`. They now go
     * through vms_kif_setast()/vms_kif_dclast() -- src/libvmssys/vms_kif.c
     * -- so the wrappers' own marshalling is exercised against a real
     * /dev/vms for the first time (vms_kif_setast/vms_kif_dclast had zero
     * callers anywhere in the checkout before this). The wrapper opens and
     * registers on its OWN thread-local descriptor (kif_bind()), separate
     * from `fd` above; that is safe because VMS_IOCTL_REGISTER is
     * per-PROCESS and idempotent (src/kernel/vms_module.c
     * vms_ioctl_register() adopts the existing vms_proc via
     * vms_proc_find_or_err() rather than erroring), so both descriptors
     * end up bound to the one VMS process this test registered above.
     *
     * The CHECK message text below is matched literally by the
     * ast-setast-disable entry in tests/qemu/facility_defects.sh. It is not
     * quoted here: the manifest selftest looks for those strings anywhere in a
     * suite source, so a comment reciting them would satisfy the check on the
     * CHECK's behalf. Read them from the CHECK lines. The struct field
     * those messages originally named (prev_state) is no longer readable
     * through vms_kif_setast(), which returns only status -- a genuine
     * interface gap (reported in vms-290) -- but the kernel derives status
     * from prev_state in lockstep (src/kernel/vms_ast.c: `args.status =
     * args.prev_state ? SS__WASSET : SS__WASCLR;`), so `status == SS_WASSET`
     * and `prev_state != 0` are the same fact under two names.
     */
    uint32_t setast_st = vms_kif_setast(0);
    CHECK(setast_st == SS_WASSET,
          "SETAST(disable) returns WASSET (== previous state was enabled)");

    /* 2. Disable again - should return WASCLR (already disabled) */
    setast_st = vms_kif_setast(0);
    /* negctl: ast-setast-disable */
    CHECK(setast_st == SS_WASCLR, "disable again: prev state was disabled");

    /* 3. Declare an AST while delivery is disabled */
    uint32_t dclast_st = vms_kif_dclast((uint64_t)(uintptr_t)dummy_ast, 42, PSL_C_USER);
    CHECK(dclast_st == SS_NORMAL, "DCLAST while disabled: queued");

    /* 4. Declare a second AST */
    dclast_st = vms_kif_dclast((uint64_t)(uintptr_t)dummy_ast, 99, PSL_C_USER);
    CHECK(dclast_st == SS_NORMAL, "DCLAST second AST queued");

    /* 5. Re-enable AST delivery */
    setast_st = vms_kif_setast(1);
    /* negctl: ast-setast-disable */
    CHECK(setast_st == SS_WASCLR,
          "SETAST(enable) returns WASCLR (== prev state was disabled)");

    /*
     * 5b. NEW (vms-290 round 3): prove vms_kif_dclast()'s astprm marshalling
     * against a real /dev/vms. Steps 3-4 above only ever checked the
     * completion status (SS_NORMAL), which is identical whether or not
     * vms_kif_dclast() actually carries the caller's astprm into the
     * kernel queue -- a wrapper that silently dropped astprm (e.g. always
     * sent 0) would leave both of those CHECKs green. VMS_IOCTL_DELIVERAST
     * dequeues FIFO (src/kernel/vms_ast.c vms_ioctl_deliverast(): "Dequeue
     * first entry" from a list the DCLAST handler appends to with
     * list_add_tail()), so after steps 3-4 the pending queue for this
     * process/mode is [astprm=42, astprm=99] in that order. Reading the
     * head back here through vms_kif_deliverast() and checking astprm==42
     * closes the gap: if vms_kif_dclast() mis-marshalled astprm, this is
     * the only assertion in the suite that would catch it. (Verified by
     * mutation -- see the round-3 commit message: dropping astprm in
     * vms_kif_dclast() reddens exactly this assertion and nothing else.)
     */
    uint64_t dlv_astadr = 0, dlv_astprm = 0;
    uint8_t dlv_acmode = 0xFF;
    int dlv_rc = vms_kif_deliverast(&dlv_astadr, &dlv_astprm, &dlv_acmode);
    CHECK(dlv_rc == 0, "DELIVERAST via wrapper returns the queued AST");
    CHECK(dlv_astprm == 42,
          "DELIVERAST astprm matches the value DCLAST declared (42)");

    /*
     * 6. Request delivery of the SECOND queued AST (astprm=99, left in the
     * queue by step 5b above) via the kernel will try to deliver via
     * signal.
     *
     * NOT CONVERTED (vms-290 finding): this call passes NULL as the ioctl
     * argument on purpose, to prove the kernel does not crash on a bad
     * userspace pointer for VMS_IOCTL_DELIVERAST. vms_kif_deliverast()
     * always passes the address of a valid on-stack struct (src/libvmssys/
     * vms_kif.c) -- it has no parameter that lets a caller ask it to hand
     * the kernel a NULL buffer -- so this specific assertion is not
     * expressible through the wrapper and stays a raw ioctl deliberately.
     */
    int rc = ioctl(fd, VMS_IOCTL_DELIVERAST, NULL);
    (void)rc;
    /*
     * ROUND-4 FIX (vms-290 adversary finding, hygiene): this used to end
     * with `CHECK(rc == 0 || rc == -1, "DELIVERAST ioctl doesn't crash")`.
     * ioctl(2) on Linux only ever returns a driver's non-negative success
     * value or -1 (with errno set from the driver's negative return), and
     * vms_ioctl_deliverast() (src/kernel/vms_ast.c) never returns anything
     * outside {0, -EFAULT, -EAGAIN} -- so `rc == 0 || rc == -1` is true of
     * every possible outcome of this call site and asserts nothing a bug
     * could fail. It sat next to two real assertions above (the DELIVERAST
     * wrapper checks added in round 3) and borrowed their credibility
     * without earning it, which is exactly the "assertion satisfiable by
     * something other than the behavior under test" failure mode this
     * dispatch's method rules forbid. Deleted rather than restated: the
     * actual property this call site exercises -- the kernel does not
     * crash on a NULL VMS_IOCTL_DELIVERAST buffer -- is proven by
     * execution reaching the "=== test_kmod_ast: N passed, M failed ==="
     * print below at all, not by any comparison on `rc`. */

    close(fd);

    printf("=== test_kmod_ast: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
