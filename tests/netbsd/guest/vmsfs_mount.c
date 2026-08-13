/*
 * vmsfs_mount.c - a tiny in-guest userspace helper that mounts an OVMX ODS-2
 * volume on NetBSD by calling mount(2) directly (rd vms-308, epic vms-8e8).
 *
 * NetBSD's mount(8) execs a per-fstype helper (/sbin/mount_<type>) that OVMX
 * does not ship, so the nightly mount-proof harness (drive_netbsd_vmsfs.py)
 * builds and runs THIS instead: it fills the vmsfs mount-args blob (the block-
 * device path) and issues the mount(2) syscall the vmsfs VFS_MOUNT consumes.
 * The volume is mounted READ-ONLY, which is all the vnode backend supports and
 * all the mount+read proof needs.
 *
 *   usage: vmsfs_mount <block-device> <mount-point>
 *
 * This is TEST TOOLING that lives under tests/netbsd/guest/ (never under src/ or
 * tools/, so the host userspace_service_register gate does not compile it -- it
 * is a NetBSD-only program). It is built in-guest by the nightly harness.
 *
 * Clean-room (CLAUDE.md Rule 8): written from the public NetBSD mount(2)
 * interface. The struct vmsfs_args mirror below is OVMX's own mount-args
 * contract (src/kernel-netbsd/vmsfs/vmsfs_nb.h); no NetBSD source is copied.
 */

#include <sys/param.h>
#include <sys/mount.h>

#include <err.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Must match struct vmsfs_args in src/kernel-netbsd/vmsfs/vmsfs_nb.h. */
struct vmsfs_args {
	char *fspec;
};

int
main(int argc, char *argv[])
{
	struct vmsfs_args args;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <block-device> <mount-point>\n",
		    argv[0]);
		return 2;
	}

	memset(&args, 0, sizeof(args));
	args.fspec = argv[1];

	if (mount("vmsfs", argv[2], MNT_RDONLY, &args, sizeof(args)) != 0)
		err(1, "mount vmsfs %s on %s", argv[1], argv[2]);

	printf("MOUNT_OK\n");
	return 0;
}
