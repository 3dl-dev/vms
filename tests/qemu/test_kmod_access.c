/*
 * test_kmod_access.c - Access mode / privilege enforcement via /dev/vms
 *
 * Runs inside the QEMU kernel-executive job (vms-e4d) with a real vms.ko
 * loaded, so every assertion below is against the real executive.
 *
 * This suite is deliberately DENIAL-shaped: the interesting claim is not
 * "a privileged process can do the privileged thing", it is "an
 * unprivileged process CANNOT, and cannot get there by another route".
 * Each denial is paired with a positive assertion that the facility still
 * works when it is supposed to -- an all-negative suite passes just as
 * happily against a feature that has been gutted.
 *
 * NOTE ON THE TEST ENVIRONMENT: the QEMU initramfs runs everything as root,
 * so vms.ko's CAP_SYS_ADMIN clamp on VMS_IOCTL_REGISTER is inert here. That
 * is exactly why the denials below are driven by the REGISTERED privilege
 * set (register with no privileges, then try to escalate) rather than by
 * the capability clamp -- they would be vacuous otherwise.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "vms_ioctl.h"

/*
 * SS$_ status codes. PROVENANCE (clean-room, CLAUDE.md rule 8): observed on
 * the reference lab (~/vax/cluster, OpenVMS VAX V7.3, node VAX1) via
 * F$MESSAGE from a DCL command procedure --
 *   F$MESSAGE(1)    -> %SYSTEM-S-NORMAL,      normal successful completion
 *   F$MESSAGE(36)   -> %SYSTEM-F-NOPRIV,      insufficient privilege or
 *                                             object protection violation
 *   F$MESSAGE(1664) -> %SYSTEM-W-NOTALLPRIV,  not all requested privileges
 *                                             authorized
 * SS_DUPNAM is the kernel module's own "already registered" status.
 */
#define SS_NORMAL       1
#define SS_NOPRIV       36
#define SS_NOTALLPRIV   1664
#define SS_DUPNAM       0x1C

/*
 * Privilege bits, from prvdef.h (src/libvms/include/prvdef.h) -- redefined
 * here because the QEMU test programs compile against the kernel headers
 * only. Values must stay in step with prvdef.h.
 */
#define PRV_CMKRNL      (1ULL << 0)
#define PRV_CMEXEC      (1ULL << 1)
#define PRV_SETPRV      (1ULL << 14)
#define PRV_TMPMBX      (1ULL << 15)
#define PRV_SYSPRV      (1ULL << 28)

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

static int reg_privs(int fd, uint64_t privs)
{
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    reg.vms_pid = (uint32_t)getpid();
    reg.init_privs = privs;
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) != 0)
        return -1;
    return (int)reg.status;
}

static int get_state(int fd, uint8_t *mode, uint64_t *cur, uint64_t *perm)
{
    struct vms_getmode_args gm;
    memset(&gm, 0, sizeof(gm));
    if (ioctl(fd, VMS_IOCTL_GETMODE, &gm) != 0)
        return -1;
    if (mode) *mode = gm.mode;
    if (cur)  *cur  = gm.cur_privs;
    if (perm) *perm = gm.perm_privs;
    return 0;
}

static uint32_t do_setprv(int fd, uint64_t mask, int enable, int permanent,
                          uint64_t *prev)
{
    struct vms_priv_args sp;
    memset(&sp, 0, sizeof(sp));
    sp.mask = mask;
    sp.enable = enable;
    sp.permanent = permanent;
    if (ioctl(fd, VMS_IOCTL_SETPRV, &sp) != 0)
        return 0xFFFFFFFFu;
    if (prev) *prev = sp.prev;
    return sp.status;
}

static uint32_t do_chkpriv(int fd, uint64_t mask)
{
    struct vms_priv_args cp;
    memset(&cp, 0, sizeof(cp));
    cp.mask = mask;
    if (ioctl(fd, VMS_IOCTL_CHKPRIV, &cp) != 0)
        return 0xFFFFFFFFu;
    return cp.status;
}

static uint32_t do_setmode(int fd, uint8_t mode)
{
    struct vms_mode_args sm;
    memset(&sm, 0, sizeof(sm));
    sm.mode = mode;
    if (ioctl(fd, VMS_IOCTL_SETMODE, &sm) != 0)
        return 0xFFFFFFFFu;
    return sm.status;
}

/* ================================================================
 * Part 1 -- an UNPRIVILEGED registration cannot escalate.
 * ================================================================ */
static void test_denial(void)
{
    uint8_t mode;
    uint64_t cur, perm, prev;
    uint32_t st;

    printf("--- unprivileged process cannot escalate ---\n");

    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) { printf("  FAIL: cannot open /dev/vms\n"); fail++; return; }

    /* Register holding NOTHING. Everything below is measured against this. */
    CHECK(reg_privs(fd, 0) == SS_NORMAL, "register with no privileges");

    CHECK(get_state(fd, &mode, &cur, &perm) == 0 && mode == PSL_C_USER &&
          cur == 0 && perm == 0,
          "executive reports USER mode and an empty privilege set");

    /* --- access mode --- */
    CHECK(do_setmode(fd, PSL_C_KERNEL) == SS_NOPRIV,
          "SETMODE(KERNEL) denied without CMKRNL");
    CHECK(do_setmode(fd, PSL_C_EXEC) == SS_NOPRIV,
          "SETMODE(EXEC) denied without CMEXEC");
    CHECK(get_state(fd, &mode, NULL, NULL) == 0 && mode == PSL_C_USER,
          "mode still USER after both denied escalations");

    /* --- $SETPRV --- */
    st = do_setprv(fd, PRV_SYSPRV, 1, 0, &prev);
    CHECK(st == SS_NOTALLPRIV,
          "SETPRV(SYSPRV) returns SS$_NOTALLPRIV without SETPRV privilege");
    CHECK(get_state(fd, NULL, &cur, NULL) == 0 && (cur & PRV_SYSPRV) == 0,
          "SYSPRV was NOT granted by the denied SETPRV");
    CHECK(do_chkpriv(fd, PRV_SYSPRV) == SS_NOPRIV,
          "CHKPRIV(SYSPRV) still denies after the refused SETPRV");

    /*
     * SECOND ENTRY POINT: widening the PERMANENT (authorized) mask. If an
     * unprivileged process could do this, the "you may re-enable what you
     * are authorized for" rule becomes a grant-anything primitive on the
     * next call. Prove the permanent mask does not move, and that the
     * follow-up re-enable is still refused.
     */
    st = do_setprv(fd, PRV_CMKRNL, 1, 1 /* permanent */, &prev);
    CHECK(st == SS_NOTALLPRIV,
          "SETPRV(CMKRNL, permanent) denied without SETPRV privilege");
    CHECK(get_state(fd, NULL, &cur, &perm) == 0 &&
          (perm & PRV_CMKRNL) == 0 && (cur & PRV_CMKRNL) == 0,
          "permanent mask NOT widened by the denied permanent SETPRV");
    CHECK(do_setmode(fd, PSL_C_KERNEL) == SS_NOPRIV,
          "SETMODE(KERNEL) still denied after the permanent-SETPRV attempt");

    /*
     * SECOND ENTRY POINT: re-REGISTER. A process that has been denied must
     * not be able to re-open the privilege question by registering again
     * with a bigger request.
     */
    CHECK(reg_privs(fd, ~0ULL) == SS_DUPNAM,
          "re-REGISTER with all privileges is refused (already registered)");
    CHECK(get_state(fd, NULL, &cur, &perm) == 0 && cur == 0 && perm == 0,
          "privilege set unchanged after the refused re-REGISTER");
    CHECK(do_setmode(fd, PSL_C_KERNEL) == SS_NOPRIV,
          "SETMODE(KERNEL) still denied after the refused re-REGISTER");

    close(fd);
}

/* ================================================================
 * Part 2 -- POSITIVE control: the facility still does its job.
 *
 * Without this, Part 1 would pass just as well against an executive that
 * denies everything unconditionally (i.e. a gutted one).
 * ================================================================ */
static void test_grant(void)
{
    uint8_t mode;
    uint64_t cur, perm, prev;

    printf("--- privileged process CAN do the privileged thing ---\n");

    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) { printf("  FAIL: cannot open /dev/vms\n"); fail++; return; }

    /* Fresh registration in a child process: the executive keys per task,
     * so this is a clean slate independent of test_denial's registration. */
    CHECK(reg_privs(fd, PRV_CMKRNL | PRV_SETPRV | PRV_TMPMBX) == SS_NORMAL,
          "register with CMKRNL+SETPRV+TMPMBX");

    CHECK(get_state(fd, &mode, &cur, &perm) == 0 &&
          (cur & PRV_CMKRNL) && (cur & PRV_SETPRV) && (cur & PRV_TMPMBX),
          "executive reports exactly the registered privileges");

    CHECK(do_chkpriv(fd, PRV_CMKRNL | PRV_SETPRV) == SS_NORMAL,
          "CHKPRIV succeeds for privileges that ARE held");
    CHECK(do_chkpriv(fd, PRV_SYSPRV) == SS_NOPRIV,
          "CHKPRIV still denies a privilege that is NOT held");

    /* SETPRV privilege means arbitrary privileges may be enabled. */
    CHECK(do_setprv(fd, PRV_SYSPRV, 1, 0, &prev) == SS_NORMAL,
          "SETPRV(SYSPRV) succeeds WITH SETPRV privilege");
    CHECK(do_chkpriv(fd, PRV_SYSPRV) == SS_NORMAL,
          "SYSPRV is now actually held");

    /* Access mode escalation succeeds with CMKRNL. */
    CHECK(do_setmode(fd, PSL_C_KERNEL) == SS_NORMAL,
          "SETMODE(KERNEL) succeeds WITH CMKRNL");
    CHECK(get_state(fd, &mode, NULL, NULL) == 0 && mode == PSL_C_KERNEL,
          "executive reports KERNEL mode after the granted escalation");

    /* Back down, then prove the VMS re-enable rule: disabling a privilege
     * leaves it in the authorized (permanent) mask, so it can be turned
     * back on without SETPRV. Observed on the reference lab: after
     * SET PROCESS/PRIVILEGE=(NOSYSPRV,NOSETPRV,...), a bare
     * SET PROCESS/PRIVILEGE=SYSPRV succeeded ($STATUS = %X10000001). */
    CHECK(do_setmode(fd, PSL_C_USER) == SS_NORMAL,
          "SETMODE(USER) always allowed (less privileged)");
    CHECK(do_setprv(fd, PRV_SETPRV | PRV_CMKRNL, 0, 0, &prev) == SS_NORMAL,
          "disabling privileges is always allowed");
    CHECK(do_chkpriv(fd, PRV_SETPRV) == SS_NOPRIV,
          "SETPRV is no longer held after being disabled");
    CHECK(do_setprv(fd, PRV_CMKRNL, 1, 0, &prev) == SS_NORMAL,
          "re-enabling an AUTHORIZED privilege works without SETPRV");
    CHECK(do_chkpriv(fd, PRV_CMKRNL) == SS_NORMAL,
          "the re-enabled privilege is actually held again");

    /* ...but only the authorized subset. TMPMBX is authorized, SYSPRV is
     * not (it was only ever enabled in the CURRENT mask, never permanent),
     * so a mixed request is partially granted with SS$_NOTALLPRIV. */
    CHECK(do_setprv(fd, PRV_TMPMBX | PRV_SYSPRV, 0, 0, &prev) == SS_NORMAL,
          "clear TMPMBX and SYSPRV before the mixed-request check");
    CHECK(do_setprv(fd, PRV_TMPMBX | PRV_SYSPRV, 1, 0, &prev)
              == SS_NOTALLPRIV,
          "mixed authorized/unauthorized enable returns SS$_NOTALLPRIV");
    CHECK(do_chkpriv(fd, PRV_TMPMBX) == SS_NORMAL,
          "the AUTHORIZED half of the mixed request WAS granted");
    CHECK(do_chkpriv(fd, PRV_SYSPRV) == SS_NOPRIV,
          "the UNAUTHORIZED half of the mixed request was NOT granted");

    close(fd);
}

/* ================================================================
 * Part 3 -- closing an fd must not destroy another fd's registration,
 * and must not hand back a fresh privilege assertion.
 * ================================================================ */
static void test_release_scope(void)
{
    uint64_t cur, perm;

    printf("--- fd close does not reset the privilege question ---\n");

    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) { printf("  FAIL: cannot open /dev/vms\n"); fail++; return; }

    CHECK(reg_privs(fd, PRV_TMPMBX) == SS_NORMAL,
          "register with TMPMBX only");

    /*
     * A SECOND, unregistered fd. Closing it must not tear down the
     * registration made on the first fd -- vms_dev_release used to free
     * whatever vms_proc was hashed under current->pid, so this close
     * destroyed the executive state that fd is still using.
     */
    int fd2 = open("/dev/vms", O_RDWR);
    CHECK(fd2 >= 0, "second /dev/vms fd opens");
    close(fd2);

    CHECK(get_state(fd, NULL, &cur, &perm) == 0 && cur == PRV_TMPMBX,
          "registration survives the close of an unrelated fd");
    CHECK(do_chkpriv(fd, PRV_SYSPRV) == SS_NOPRIV,
          "and it still denies a privilege it never had");

    close(fd);
}

int main(void)
{
    /* Line-buffer: parts 2 and 3 run in forked children that _exit(), which
     * does not flush stdio. Without this their PASS/FAIL lines are lost
     * whenever stdout is a pipe (i.e. under the CI runner). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== test_kmod_access ===\n");

    /* Part 2 needs a registration distinct from Part 1's, and the executive
     * keys per task -- run it in a child so both get a clean slate. */
    test_denial();

    pid_t child = fork();
    if (child == 0) {
        int before = fail;
        test_grant();
        _exit(fail > before ? 1 : 0);
    } else if (child > 0) {
        int wstatus = 0;
        waitpid(child, &wstatus, 0);
        CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0,
              "privileged-path child reported all checks passing");
    } else {
        CHECK(0, "fork for the privileged-path child");
    }

    child = fork();
    if (child == 0) {
        int before = fail;
        test_release_scope();
        _exit(fail > before ? 1 : 0);
    } else if (child > 0) {
        int wstatus = 0;
        waitpid(child, &wstatus, 0);
        CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0,
              "release-scope child reported all checks passing");
    } else {
        CHECK(0, "fork for the release-scope child");
    }

    printf("=== test_kmod_access: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
