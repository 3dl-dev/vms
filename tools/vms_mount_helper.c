/*
 * vms_mount_helper.c - the ONLY thing on the node still Linux root when
 * DCL's MOUNT/DISMOUNT need mount(2)/umount(2) (vms-651).
 *
 * WHY THIS EXISTS. LOGINOUT setuid()/setgid()'s every VMS session onto its
 * SYSUAF UIC (vms_login.c) -- VMS privilege is not Linux capability, and
 * OVMX does not blur that line by handing a VMS session real Linux root or
 * CAP_SYS_ADMIN just because it holds PRV$M_MOUNT. So by the time
 * cmd_mount()/cmd_dismount() (src/vmsdcl/dcl_cmd_misc.c) know MOUNT is
 * authorized, the calling process cannot call mount(2)/umount(2) itself --
 * both require CAP_SYS_ADMIN unconditionally, regardless of VMS privilege.
 *
 * A kernel-mediated mount (a new vms.ko ioctl calling the mount-syscall
 * internals directly) was tried first and does NOT work on this kernel:
 * fs/namespace.c's do_mount()/path_mount()/do_umount()/path_umount() are
 * NOT in the exported-symbol table at all (confirmed against this build's
 * own Module.symvers, not guessed), and the public replacement API
 * (fs_context_for_mount()/vfs_get_tree()/vfs_create_mount()) has no
 * exported way to GRAFT the resulting mount onto a path in a process's
 * namespace -- that step is deliberately kept private to the mount(2)
 * syscall's own implementation. A kernel module mounting an arbitrary path
 * on a process's behalf is not a supported operation on this platform.
 *
 * THE DESIGN THIS SETTLES ON INSTEAD -- the classic Unix shape (mount(8),
 * su, ping, ... were all setuid-root for the same reason): a small,
 * setuid-root helper that does ONLY mount(2)/umount(2) of vmsfs on an
 * already-existing directory, and re-derives its OWN authorization from
 * the executive before doing anything privileged -- it does not trust
 * whoever invoked it, including its own parent.
 *
 * WHY RE-DERIVING AUTHORIZATION HERE IS NOT OPTIONAL. This binary is
 * setuid-root on disk; if it trusted its caller, ANY Linux process on the
 * node (any VMS session, any UID) could invoke it directly with crafted
 * arguments and mount/unmount anything, bypassing PRV$M_MOUNT entirely.
 * Instead it CONTINUES the calling process's VMS identity onto itself
 * (vms_kif_register_continue() -- the SAME "forked child before doing
 * privileged work inherits the parent's authenticated identity" shape
 * image activation already uses, tools/vms_login.c's caller in
 * dcl_activate_image) and asks the executive whether PRV$M_MOUNT is held
 * (vms_kif_chkpriv, vms_ioctl_chkpriv, src/kernel/vms_access.c -- checked
 * against proc->cur_privs, kernel-resident state). A process invoked
 * directly, outside that fork-before-exec sequence, registers as a fresh,
 * unauthenticated process with zero privileges and is refused here, exactly
 * as the executive would refuse it anywhere else.
 *
 * cmd_mount()/cmd_dismount() are the only product callers: they fork(),
 * call vms_kif_register_continue() in the child (so THIS process's pid is
 * bound to the parent's VMS identity before exec() replaces the image --
 * pid/tgid survive exec, uid/gid/capabilities do not), then execv() this
 * binary.
 *
 * Deliberately minimal: no VMS filespec resolution, no DCL parsing, no
 * qualifiers -- it takes exactly the two forms below and refuses anything
 * else. Every argument DCL hands it is already a resolved Linux path.
 *
 *   vms_mount_helper mount <backing-dev> <mount-point>
 *   vms_mount_helper umount <mount-point>
 *
 * Exit 0 on success. Exit 1 with "ERRNO <n> <text>" on stderr if the
 * mount(2)/umount(2) itself failed (the caller renders this honestly --
 * there is no oracle-pinned VMS status for a failed Linux mount syscall,
 * CLAUDE.md Rule 10). Exit 2 for a usage error. Exit 3 if PRV$M_MOUNT is
 * not held.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>

#include "vms_kif.h"
#include "prvdef.h"

int main(int argc, char *argv[])
{
    /* Continue the CALLER's VMS identity onto this process -- see the
     * file header for why this, and not trusting argv, is the actual
     * security boundary. */
    (void)vms_kif_open();
    (void)vms_kif_register_continue();

    uint32_t priv_status = vms_kif_chkpriv(PRV$M_MOUNT);
    if (!(priv_status & 1)) {
        fprintf(stderr, "NOPRIV\n");
        return 3;
    }

    if (argc == 4 && strcmp(argv[1], "mount") == 0) {
        if (mount(argv[2], argv[3], "vmsfs", 0, NULL) != 0) {
            fprintf(stderr, "ERRNO %d %s\n", errno, strerror(errno));
            return 1;
        }
        return 0;
    }

    if (argc == 3 && strcmp(argv[1], "umount") == 0) {
        if (umount(argv[2]) != 0) {
            fprintf(stderr, "ERRNO %d %s\n", errno, strerror(errno));
            return 1;
        }
        return 0;
    }

    fprintf(stderr, "usage: vms_mount_helper mount <backing-dev> <mount-point>\n"
                     "       vms_mount_helper umount <mount-point>\n");
    return 2;
}
