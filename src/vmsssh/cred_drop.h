/*
 * cred_drop.h - SSH session credential drop to the authenticated UIC.
 *
 * vms-49e (Tier-0 security): vmssshd authenticates against SYSUAF and stamps
 * the executive identity, but historically exec'd DCL WITHOUT dropping the
 * process's Linux credentials -- so every SSH session (and every process it
 * forked) ran as euid=0/root, which src/kernel/vmsfs/vmsfs_blkdev.c reads as
 * full System-category access to the whole volume, and which a forked child
 * re-registering with CAP_SYS_ADMIN turns into all privileges. The console
 * path (tools/vms_login.c / LOGINOUT) already drops correctly; this is the
 * SSH mirror of that drop, factored out so its ordering and fail-closed
 * behaviour can be unit-tested without the test process itself dropping
 * privileges.
 *
 * The mapping UIC [group,member] -> (gid=group, uid=member) is OVMX's stand-in
 * for the VMS UIC (OpenVMS has no Linux credentials); it matches exactly what
 * the executive derives (proc->uic) and what RMS enforces file protection
 * against (src/vmsrms/rms_core.c) -- see the long comment in tools/vms_login.c.
 * This is labelled an OVMX design choice, not VMS-authentic behaviour
 * (CLAUDE.md Rule 8/10).
 */

#ifndef OVMX_VMSSSH_CRED_DROP_H
#define OVMX_VMSSSH_CRED_DROP_H

#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

/*
 * Injectable credential syscalls. Production wires these to the real
 * setgroups/setgid/setuid/getuid/... (ovmx_cred_real_syscalls below); the
 * unit test wires them to mocks that record call order and can be made to
 * fail, so the security-critical policy (clear groups, setgid BEFORE setuid,
 * verify, fail closed) is exercised without the test giving up its own uid.
 */
struct ovmx_cred_syscalls {
    int   (*fn_setgroups)(size_t size, const gid_t *list);
    int   (*fn_setgid)(gid_t gid);
    int   (*fn_setuid)(uid_t uid);
    uid_t (*fn_getuid)(void);
    uid_t (*fn_geteuid)(void);
    gid_t (*fn_getgid)(void);
    gid_t (*fn_getegid)(void);
};

/* The production syscall table (real setgroups/setgid/setuid/...). */
extern const struct ovmx_cred_syscalls ovmx_cred_real_syscalls;

/*
 * Derive the target credentials from the SYSUAF UIC [uic_group, uic_member]
 * and drop this process's credentials to them PERMANENTLY (real setgid/setuid,
 * NOT seteuid -- a saved-set-uid must not survive), clearing supplementary
 * groups first and applying setgid BEFORE setuid (so the setgid is not refused
 * after the uid is dropped). The drop is then verified with getuid/geteuid/
 * getgid/getegid.
 *
 * Returns 0 on success. Returns -1 on ANY failure (a failing syscall or a drop
 * that did not verify), leaving errno set for the caller's diagnostic. The
 * caller MUST treat -1 as fatal and NEVER exec the session as root -- failing
 * closed is the whole point of this fix.
 *
 * Mirrors the inline drop in tools/vms_login.c (LOGINOUT).
 */
int ovmx_cred_drop_to_uic(uint32_t uic_group, uint32_t uic_member,
                          const struct ovmx_cred_syscalls *sc);

#endif /* OVMX_VMSSSH_CRED_DROP_H */
