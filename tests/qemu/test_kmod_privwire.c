/*
 * test_kmod_privwire.c - The USERSPACE privilege wiring, against a real
 *                        executive.
 *
 * test_kmod_access.c proves that vms.ko enforces privileges when the ioctls
 * are driven directly. That is not the same claim as "OVMX userspace
 * consults the executive" -- for a long time vms.ko implemented SETMODE /
 * GETMODE / SETPRV / CHKPRIV and NOTHING in userspace ever called them, so
 * a direct-ioctl test would have passed while $SETPRV was still a per-
 * process variable a process wrote about itself.
 *
 * This suite therefore links the PRODUCTION gateway -- src/libvmssys/
 * vms_exec.c and vms_kif.c, the exact objects libvms and vmsprocess link --
 * and drives it against the real /dev/vms inside the QEMU job (vms-e4d).
 *
 * Shape: every denial is paired with a positive assertion, so the suite
 * cannot pass against a gateway that has been gutted into always-denying.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "vms_exec.h"
#include "vms_kif.h"     /* PSL_C_* access-mode constants (via vms_ioctl.h) */

/*
 * Privilege bits, from src/libvms/include/prvdef.h. Redefined here because
 * the QEMU test programs do not compile against the libvms headers.
 */
#define PRV_CMKRNL      (1ULL << 0)
#define PRV_SETPRV      (1ULL << 14)
#define PRV_TMPMBX      (1ULL << 15)
#define PRV_SYSPRV      (1ULL << 28)

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

/*
 * An UNPRIVILEGED process, driven entirely through the production gateway.
 */
static int run_unprivileged(void)
{
    uint64_t granted = 0xdeadbeef, cur = 0xdeadbeef, perm = 0xdeadbeef;
    uint8_t mode = 0;
    int before = fail;
    uint32_t st;

    printf("--- gateway: unprivileged process ---\n");

    st = vms_exec_attach((uint32_t)getpid(), 0, &granted);
    if (st == VMS_EXEC_SS_NOSUCHDEV)
        printf("  REASON: vms_exec_attach -> SS$_NOSUCHDEV (%d):"
               " cannot open /dev/vms, the executive is absent\n",
               VMS_EXEC_SS_NOSUCHDEV);
    CHECK(st == VMS_EXEC_SS_NORMAL,
          "vms_exec_attach reaches the executive");
    CHECK(vms_exec_attached() == 1, "gateway reports attached");
    CHECK(granted == 0, "executive granted nothing (nothing was requested)");

    CHECK(vms_exec_getprv(&cur, &perm, &mode) == VMS_EXEC_SS_NORMAL &&
          cur == 0 && perm == 0 && mode == PSL_C_USER,
          "gateway reads back USER mode and an empty privilege set");

    /* Denials, through the same calls libvms/vmsprocess make. */
    CHECK(vms_exec_chkpriv(PRV_SYSPRV) == VMS_EXEC_SS_NOPRIV,
          "vms_exec_chkpriv denies a privilege that is not held");
    CHECK(vms_exec_setmode(PSL_C_KERNEL) == VMS_EXEC_SS_NOPRIV,
          "vms_exec_setmode(KERNEL) denied without CMKRNL");
    CHECK(vms_exec_setprv(PRV_SYSPRV, 1, 0, NULL) == VMS_EXEC_SS_NOTALLPRIV,
          "vms_exec_setprv(SYSPRV) refused -> SS$_NOTALLPRIV");

    /* ...and the refusal actually stuck in the executive. */
    CHECK(vms_exec_getprv(&cur, NULL, &mode) == VMS_EXEC_SS_NORMAL &&
          cur == 0 && mode == PSL_C_USER,
          "executive state unchanged by the refused calls");

    /*
     * SECOND ENTRY POINT: re-attach asking for everything. The gateway must
     * not let a thread re-open the privilege question, and the executive
     * must not hand out a fresh, larger assertion.
     */
    CHECK(vms_exec_attach((uint32_t)getpid(), ~0ULL, &granted)
              == VMS_EXEC_SS_NORMAL && granted == 0,
          "re-attach with an ALL-privileges request grants nothing");
    CHECK(vms_exec_chkpriv(PRV_CMKRNL) == VMS_EXEC_SS_NOPRIV,
          "still no CMKRNL after the greedy re-attach");
    CHECK(vms_exec_setmode(PSL_C_KERNEL) == VMS_EXEC_SS_NOPRIV,
          "SETMODE(KERNEL) still denied after the greedy re-attach");

    return fail > before ? 1 : 0;
}

/*
 * POSITIVE control: the same gateway grants what it should. Without this,
 * everything above would pass against a gateway that returns "denied"
 * unconditionally -- including one that never reaches the executive at all.
 */
static int run_privileged(void)
{
    uint64_t granted = 0, cur = 0, perm = 0, prev = 0;
    uint8_t mode = 0;
    int before = fail;

    printf("--- gateway: privileged process ---\n");

    CHECK(vms_exec_attach((uint32_t)getpid(),
                          PRV_CMKRNL | PRV_SETPRV | PRV_TMPMBX, &granted)
              == VMS_EXEC_SS_NORMAL,
          "vms_exec_attach with a privilege request");
    CHECK(granted == (PRV_CMKRNL | PRV_SETPRV | PRV_TMPMBX),
          "executive granted exactly the requested set");

    CHECK(vms_exec_chkpriv(PRV_CMKRNL) == VMS_EXEC_SS_NORMAL,
          "vms_exec_chkpriv confirms a privilege that IS held");
    CHECK(vms_exec_chkpriv(PRV_SYSPRV) == VMS_EXEC_SS_NOPRIV,
          "...and still denies one that is not");

    CHECK(vms_exec_setprv(PRV_SYSPRV, 1, 0, &prev) == VMS_EXEC_SS_NORMAL,
          "vms_exec_setprv(SYSPRV) succeeds WITH SETPRV privilege");
    CHECK(prev == (PRV_CMKRNL | PRV_SETPRV | PRV_TMPMBX),
          "setprv reported the previous privilege mask");
    CHECK(vms_exec_chkpriv(PRV_SYSPRV) == VMS_EXEC_SS_NORMAL,
          "SYSPRV is now genuinely held");

    CHECK(vms_exec_setmode(PSL_C_KERNEL) == VMS_EXEC_SS_NORMAL,
          "vms_exec_setmode(KERNEL) succeeds WITH CMKRNL");
    CHECK(vms_exec_getprv(&cur, &perm, &mode) == VMS_EXEC_SS_NORMAL &&
          mode == PSL_C_KERNEL,
          "gateway reads back KERNEL mode");

    CHECK(vms_exec_setmode(PSL_C_USER) == VMS_EXEC_SS_NORMAL,
          "vms_exec_setmode(USER) is always allowed");
    CHECK(vms_exec_setprv(PRV_SYSPRV | PRV_SETPRV, 0, 0, &prev)
              == VMS_EXEC_SS_NORMAL,
          "disabling privileges through the gateway is allowed");
    CHECK(vms_exec_chkpriv(PRV_SYSPRV) == VMS_EXEC_SS_NOPRIV,
          "SYSPRV really went away when disabled");

    return fail > before ? 1 : 0;
}

int main(void)
{
    /* Line-buffer: the role suites run in forked children that _exit(),
     * which does not flush stdio. Without this their PASS/FAIL lines are
     * lost whenever stdout is a pipe (i.e. under the CI runner). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== test_kmod_privwire ===\n");

    /*
     * The two roles need separate executive registrations and the executive
     * keys per task, so each runs in its own child process. The parent only
     * asserts on their exit status; their PASS/FAIL lines are printed by the
     * children themselves.
     */
    pid_t c1 = fork();
    if (c1 == 0) _exit(run_unprivileged());

    int ws = 0;
    waitpid(c1, &ws, 0);
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0,
          "unprivileged gateway child: all checks passed");

    pid_t c2 = fork();
    if (c2 == 0) _exit(run_privileged());

    ws = 0;
    waitpid(c2, &ws, 0);
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0,
          "privileged gateway child: all checks passed");

    printf("=== test_kmod_privwire: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
