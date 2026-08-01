/*
 * test_kmod_access.c - Access mode enforcement via /dev/vms (vms-2b8)
 *
 * WHAT THIS USED TO DO, AND WHY IT COULD NOT SURVIVE.
 *
 * This suite used to register with `init_privs = 0`, assert that KERNEL
 * mode was then denied, and afterwards RE-REGISTER with
 * 0xFFFFFFFFFFFFFFFF to "test privileged operations". Every one of those
 * steps was the process telling the executive what privileges it had.
 * vms-2b8 deleted the init_privs quadword from VMS_IOCTL_REGISTER for
 * exactly that reason -- so this file no longer compiled, and the
 * Dockerfile's build loop silently dropped it from the initramfs until
 * vms-1d9 added `|| exit 1`.
 *
 * A REFUSAL PROVED BY ASKING FOR IT WITH THE PRIVILEGE SWITCHED OFF BY
 * THE ASKER IS NOT A REFUSAL. So the two halves are now two processes
 * with genuinely different kernel credentials:
 *
 *   - the parent stays root, so the executive derives CMKRNL for it from
 *     capable(CAP_SYS_ADMIN), and KERNEL mode is ALLOWED. Without this
 *     half the refusal below proves nothing: an executive that refused
 *     everything would pass it.
 *   - the child does a real setgid()+setuid() away from root, so the
 *     executive derives no enforced privilege at all, and KERNEL mode is
 *     REFUSED with SS$_NOPRIV and the mode is unchanged afterwards.
 *
 * The child is sequenced by a blocking pipe read, not a sleep.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "vms_ioctl.h"
#include "vms_kif.h"

/* SS$_ status codes (matching kernel module) */
#define SS_NORMAL   1
#define SS_NOPRIV   36

/* Credentials for the unprivileged half. setuid() away from root is what
 * makes capable(CAP_SYS_ADMIN) genuinely false in the executive. */
#define C_GID   301
#define C_UID   1002

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

/* What the unprivileged child reports back. */
struct child_report {
    uint32_t registered;
    uint32_t chkpriv_cmkrnl;    /* does it hold CMKRNL? */
    uint32_t setmode_kernel;    /* status of the escalation attempt */
    uint8_t  mode_after;        /* mode after the refused attempt */
    uint32_t getmode_ok;        /* did the GETMODE re-read itself succeed? */
    uint32_t setprv_cmkrnl;     /* can it grant itself CMKRNL? */
    uint32_t chkpriv_after;     /* ... and did that leave it holding CMKRNL? */
};

static int child_main(int wfd)
{
    struct child_report r;
    struct vms_register_args reg;
    struct vms_priv_args pv;
    struct vms_getmode_args gm;
    int fd;

    memset(&r, 0, sizeof(r));

    if (setgid(C_GID) != 0)
        _exit(70);
    if (setuid(C_UID) != 0)
        _exit(71);
    if (getuid() == 0 || geteuid() == 0)
        _exit(72);

    /* Reachable at all only because /dev/vms is 0666 -- the executive
     * entry point is unprivileged, as the VMS system-service entry is
     * (see vms_misc in src/kernel/vms_module.c). */
    fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        _exit(73);

    memset(&reg, 0, sizeof(reg));
    reg.vms_pid = (uint32_t)getpid();
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) != 0)
        _exit(74);
    r.registered = reg.status;

    memset(&pv, 0, sizeof(pv));
    pv.mask = VMS_PRV_M_CMKRNL;
    ioctl(fd, VMS_IOCTL_CHKPRIV, &pv);
    r.chkpriv_cmkrnl = pv.status;

    /* CONVERTED (vms-290): was raw ioctl(fd, VMS_IOCTL_SETMODE, &sm). Now
     * exercises vms_kif_setmode() (src/libvmssys/vms_kif.c), which had
     * zero callers anywhere in the checkout before this. */
    r.setmode_kernel = vms_kif_setmode(PSL_C_KERNEL);

    memset(&gm, 0, sizeof(gm));
    /* vms-0e4 (audit round 2): the ioctl's own return was previously
     * ignored here. PSL_C_USER happens to be nonzero (3), so the
     * memset(0) default alone did not make this vacuous by accident --
     * but "did not happen to" is not "cannot", so this is now checked
     * the same way the KERNEL-direction re-read below is. */
    r.getmode_ok = (ioctl(fd, VMS_IOCTL_GETMODE, &gm) == 0);
    r.mode_after = gm.mode;

    /* And it cannot talk its way into the privilege either. */
    memset(&pv, 0, sizeof(pv));
    pv.mask = VMS_PRV_M_CMKRNL;
    pv.enable = 1;
    pv.permanent = 0;
    ioctl(fd, VMS_IOCTL_SETPRV, &pv);
    r.setprv_cmkrnl = pv.status;

    memset(&pv, 0, sizeof(pv));
    pv.mask = VMS_PRV_M_CMKRNL;
    ioctl(fd, VMS_IOCTL_CHKPRIV, &pv);
    r.chkpriv_after = pv.status;

    close(fd);

    if (write(wfd, &r, sizeof(r)) != (ssize_t)sizeof(r))
        _exit(75);
    return 0;
}

int main(void)
{
    struct vms_register_args reg;
    struct vms_getmode_args gm;
    struct vms_priv_args pv;
    struct child_report cr;
    int c2p[2];
    pid_t child;
    int fd;

    printf("=== test_kmod_access ===\n");

    fd = open("/dev/vms", O_RDWR);
    if (fd < 0) {
        printf("  FAIL: cannot open /dev/vms\n");
        printf("=== test_kmod_access: 0 passed, 1 failed ===\n");
        return 1;
    }

    /* ---- the PRIVILEGED half ---- */
    memset(&reg, 0, sizeof(reg));
    reg.vms_pid = (uint32_t)getpid();
    CHECK(ioctl(fd, VMS_IOCTL_REGISTER, &reg) == 0 && reg.status == SS_NORMAL,
          "register process");

    memset(&gm, 0, sizeof(gm));
    CHECK(ioctl(fd, VMS_IOCTL_GETMODE, &gm) == 0 && gm.mode == PSL_C_USER,
          "a process starts in USER mode");

    memset(&pv, 0, sizeof(pv));
    pv.mask = VMS_PRV_M_CMKRNL;
    ioctl(fd, VMS_IOCTL_CHKPRIV, &pv);
    CHECK(pv.status == SS_NORMAL,
          "a CAP_SYS_ADMIN process is DERIVED CMKRNL, not granted it on request");

    /* CONVERTED (vms-290): was raw ioctl(fd, VMS_IOCTL_SETMODE, &sm). */
    uint32_t setmode_st = vms_kif_setmode(PSL_C_KERNEL);
    CHECK(setmode_st == SS_NORMAL, "KERNEL mode ALLOWED with CMKRNL");

    /* vms-0e4 (audit round 2): this re-read did not check the ioctl's own
     * return value. PSL_C_KERNEL is 0 -- numerically identical to the
     * memset(0) default below -- so if VMS_IOCTL_GETMODE ever failed
     * outright (returned an error without writing gm), this assertion
     * would read the LEFTOVER zeroed buffer, see mode==0, and report PASS
     * for a GETMODE that told it nothing. MEASURED: injecting exactly
     * that fault into vms_ioctl_getmode() (return before copy_to_user)
     * left this one line green while "a process starts in USER mode" and
     * "... and the mode really returned to USER" both correctly went red
     * -- the defect this rewrite closes. */
    memset(&gm, 0, sizeof(gm));
    CHECK(ioctl(fd, VMS_IOCTL_GETMODE, &gm) == 0 && gm.mode == PSL_C_KERNEL,
          "... and the mode really changed");

    /* Back to USER: a less privileged move is always allowed, and the
     * child below must not be compared against a parent left in kernel
     * mode. */
    /* CONVERTED (vms-290): was raw ioctl(fd, VMS_IOCTL_SETMODE, &sm). */
    setmode_st = vms_kif_setmode(PSL_C_USER);
    CHECK(setmode_st == SS_NORMAL, "returning to USER mode is always allowed");

    /* vms-0e4: the sibling of "... and the mode really changed" above. A
     * service that reports SS$_NORMAL for a drop-to-USER request while
     * actually leaving the process in a MORE PRIVILEGED mode is a
     * privilege escalation reported as success -- the status alone cannot
     * tell the two apart, only the observed mode can. */
    /* Same ioctl-return check added to the KERNEL-direction re-read above,
     * applied here too: PSL_C_USER (3) happens to differ from the
     * memset(0) default, so this one was not vacuous by the same route --
     * but not being exploitable by luck is not the same as being correct. */
    memset(&gm, 0, sizeof(gm));
    CHECK(ioctl(fd, VMS_IOCTL_GETMODE, &gm) == 0 && gm.mode == PSL_C_USER,
          "... and the mode really returned to USER");

    /* ---- the UNPRIVILEGED half ---- */
    if (pipe(c2p) < 0) {
        printf("  FAIL: pipe()\n");
        printf("=== test_kmod_access: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    child = fork();
    if (child < 0) {
        printf("  FAIL: fork()\n");
        printf("=== test_kmod_access: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    if (child == 0) {
        close(c2p[0]);
        close(fd);
        _exit(child_main(c2p[1]));
    }
    close(c2p[1]);

    memset(&cr, 0, sizeof(cr));
    if (read(c2p[0], &cr, sizeof(cr)) != (ssize_t)sizeof(cr)) {
        printf("  FAIL: unprivileged half never reported "
               "(credential drop or /dev/vms open failed)\n");
        waitpid(child, NULL, 0);
        printf("=== test_kmod_access: %d passed, %d failed ===\n", pass, fail + 1);
        close(fd);
        return 1;
    }
    waitpid(child, NULL, 0);

    CHECK(cr.registered == SS_NORMAL,
          "an unprivileged process may register with the executive");
    CHECK(cr.chkpriv_cmkrnl == SS_NOPRIV,
          "an unprivileged process is NOT derived CMKRNL");
    CHECK(cr.setmode_kernel == SS_NOPRIV,
          "KERNEL mode DENIED without CMKRNL (SS$_NOPRIV)");
    CHECK(cr.getmode_ok && cr.mode_after == PSL_C_USER,
          "... and the mode is still USER after the denied escalation");
    CHECK(cr.setprv_cmkrnl != SS_NORMAL,
          "an unprivileged process cannot $SETPRV itself CMKRNL");
    CHECK(cr.chkpriv_after == SS_NOPRIV,
          "... and still does not hold CMKRNL afterwards");

    close(fd);

    printf("=== test_kmod_access: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
