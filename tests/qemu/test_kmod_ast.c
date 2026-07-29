/*
 * test_kmod_ast.c - Test AST operations via /dev/vms ioctl
 *
 * Tests:
 *   1. Register process
 *   2. Declare an AST (DCLAST)
 *   3. Enable/disable AST delivery (SETAST)
 *   4. Verify previous state return values
 *   5. DELIVERAST actually fires: proves the kernel queue is real, not
 *      decoration -- declares two ASTs with distinct astprm values,
 *      then drains DELIVERAST with a real buffer and asserts (a) the
 *      first call returns the FIRST-declared entry's astadr/astprm/
 *      acmode unchanged (FIFO, correct content -- the positive
 *      assertion), (b) the second call returns the second entry, and
 *      (c) a third call reports "queue now empty" (EAGAIN) -- the
 *      queue was actually consumed, not just always reporting success.
 *      This is the userspace-facing proof for vms-as1 (AST delivery
 *      routed through the executive): src/libvms/syssvc/sys_ast.c's
 *      sys$dclast/sys$setast now call exactly these three ioctls
 *      (via vms_kif) instead of simulating delivery in a per-process
 *      PCB queue -- see docs/design-executive-retrofit.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "vms_ioctl.h"

#define SS_NORMAL   1
#define SS_WASSET   9
#define SS_WASCLR   5

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

/* Dummy AST handler address (won't actually be called in kernel tests) */
static void dummy_ast(uint32_t param) { (void)param; }

int main(void) {
    printf("=== test_kmod_ast ===\n");

    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) {
        printf("  FAIL: cannot open /dev/vms\n");
        return 1;
    }

    /* Register */
    struct vms_register_args reg = {0};
    reg.vms_pid = (uint32_t)getpid();
    reg.init_privs = 0xFFFFFFFFFFFFFFFFULL;
    ioctl(fd, VMS_IOCTL_REGISTER, &reg);
    CHECK(reg.status == SS_NORMAL, "register");

    /* 1. Disable AST delivery - initially should be enabled (WASSET) */
    struct vms_setast_args sa = {0};
    sa.enable = 0;
    ioctl(fd, VMS_IOCTL_SETAST, &sa);
    CHECK(sa.status == SS_WASSET, "SETAST(disable) returns WASSET");
    CHECK(sa.prev_state != 0, "previous state was enabled");

    /* 2. Disable again - should return WASCLR (already disabled) */
    memset(&sa, 0, sizeof(sa));
    sa.enable = 0;
    ioctl(fd, VMS_IOCTL_SETAST, &sa);
    CHECK(sa.prev_state == 0, "disable again: prev state was disabled");

    /* 3. Declare an AST while delivery is disabled */
    struct vms_ast_args ast = {0};
    ast.astadr = (uint64_t)(uintptr_t)dummy_ast;
    ast.astprm = 42;
    ast.acmode = PSL_C_USER;
    ioctl(fd, VMS_IOCTL_DCLAST, &ast);
    CHECK(ast.status == SS_NORMAL, "DCLAST while disabled: queued");

    /* 4. Declare a second AST (distinct astprm so FIFO order is provable) */
    memset(&ast, 0, sizeof(ast));
    ast.astadr = (uint64_t)(uintptr_t)dummy_ast;
    ast.astprm = 99;
    ast.acmode = PSL_C_USER;
    ioctl(fd, VMS_IOCTL_DCLAST, &ast);
    CHECK(ast.status == SS_NORMAL, "DCLAST second AST queued");

    /* 5. Re-enable AST delivery */
    memset(&sa, 0, sizeof(sa));
    sa.enable = 1;
    ioctl(fd, VMS_IOCTL_SETAST, &sa);
    CHECK(sa.status == SS_WASCLR, "SETAST(enable) returns WASCLR");
    CHECK(sa.prev_state == 0, "prev state was disabled");

    /* 6. DELIVERAST actually fires: pass a real buffer (VMS_IOCTL_DELIVERAST
     * is _IOR -- the kernel writes the delivered AST entry into it) and
     * assert the content the kernel hands back matches what was declared,
     * in declaration order. This is the positive assertion pairing with
     * the "doesn't crash" style check the old version of this test settled
     * for -- an ioctl that always returns -1 (e.g. the NULL-pointer bug
     * this test used to have) would have passed that check too. */
    struct vms_ast_args deliver1 = {0};
    int rc1 = ioctl(fd, VMS_IOCTL_DELIVERAST, &deliver1);
    CHECK(rc1 == 0 && deliver1.astadr == (uint64_t)(uintptr_t)dummy_ast
              && deliver1.astprm == 42 && deliver1.acmode == PSL_C_USER,
          "DELIVERAST #1 returns the first-declared AST (astprm=42)");

    struct vms_ast_args deliver2 = {0};
    int rc2 = ioctl(fd, VMS_IOCTL_DELIVERAST, &deliver2);
    CHECK(rc2 == 0 && deliver2.astadr == (uint64_t)(uintptr_t)dummy_ast
              && deliver2.astprm == 99 && deliver2.acmode == PSL_C_USER,
          "DELIVERAST #2 returns the second-declared AST (astprm=99)");

    struct vms_ast_args deliver3 = {0};
    int rc3 = ioctl(fd, VMS_IOCTL_DELIVERAST, &deliver3);
    CHECK(rc3 != 0, "DELIVERAST #3: queue actually drained (no third AST)");

    close(fd);

    printf("=== test_kmod_ast: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
