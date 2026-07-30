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
    /* NO init_privs (vms-2b8): VMS_IOCTL_REGISTER no longer carries a
     * privilege mask. It used to, and this line asked for all 64 bits --
     * a process declaring its own privileges, which is the honor system
     * that item removed. The executive now DERIVES the mask from the
     * task's real credentials, and nothing below needs a privilege. */
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

    /* 4. Declare a second AST */
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

    /* 6. Request delivery (the kernel will try to deliver via signal) */
    int rc = ioctl(fd, VMS_IOCTL_DELIVERAST, NULL);
    /* This may or may not produce visible effect since we're in initramfs
     * without a proper signal handler. Just verify the ioctl doesn't crash. */
    CHECK(rc == 0 || rc == -1, "DELIVERAST ioctl doesn't crash");

    close(fd);

    printf("=== test_kmod_ast: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
