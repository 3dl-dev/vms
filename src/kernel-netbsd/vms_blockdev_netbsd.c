/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_blockdev_netbsd.c - the NetBSD/vax realization of the executive
 * block-device seam (exec_blockdev_read_block / exec_blockdev_write_block,
 * exec_kbackend.h S8) plus the single-unit disk resolve
 * (vms_devtab_disk_backing) the Files-11 ODS-2 ACP $MOUNT calls to bind
 * SYS$DISK. rd vms-d5d, epic vms-8e8.
 *
 * WHY THIS EXISTS. The Files-11 ODS-2 ACP lives in the shared executive core
 * (kernel-core/vmsfs_acp.c) and reaches the raw disk ONLY through the
 * exec_blockdev_read_block/write_block seam keyed by (major,minor); the Linux
 * vms.ko realizes that seam with submit_bio_wait (exec_kbackend_linux.h). This
 * TU is the NetBSD twin, so the VAX runtime reads/writes SYS$DISK over
 * IO$_ACCESS/READVBLK/WRITEVBLK via /dev/vms -- the executive ACP -- instead of
 * the vmsfs.ko VFS mount + POSIX bypass (the class Rule 9 / INV-6 excise).
 *
 * FAITHFUL, NOT COPIED (Rule 8). Every disk touch is a PUBLIC, documented
 * NetBSD kernel primitive, and specifically the SAME ones the sibling
 * vmsfs.kmod already uses against this very volume (vmsfs/vmsfs_vfsops.c):
 * vn_bdev_openpath() for the device vnode, bread(9) for reads, getblk(9)+
 * bwrite(9) for the synchronous single-block write twin of Linux's REQ_OP_WRITE
 * bio, and vn_close(FREAD|FWRITE) to release with the SAME mode the open used
 * (vmsfs_vfsops.c:315-359 documents the "vrelel: bad ref count" panic that a
 * mismatched close provokes). No VSI/HPE source is used. INV-6: real block I/O
 * against a real device, or an honest failure -- never a fabricated success.
 *
 * DEVICE-NATIVE RESOLVE (vms-47d). The VMS unit NAME is a label; the resolve
 * binds it to the REAL NetBSD block device (/dev/ra1c, the 2nd MSCP disk under
 * SIMH that carries the mastered OVMX ODS-2 volume -- PR #606). The current
 * boot unit name "DKA0:" is a placeholder: a real VMS-VAX calls an MSCP/RQ disk
 * DUxn: (DUA0:), not DKA0: (DK == SCSI/RK). The DKA0:->DUA0: rename is tracked
 * as vms-47d; this resolve accepts BOTH so it stays correct across that rename.
 * The unit map is a single entry today but a TABLE, so a future multi-unit VAX
 * extends without a rewrite (conductor caveat, vms-d5d).
 */

#include <sys/param.h>			/* __arraycount, makedev/major/minor */
#include <sys/types.h>
#include <sys/systm.h>			/* strlcpy */
#include <sys/buf.h>			/* bread/getblk/bwrite/brelse */
#include <sys/vnode.h>
#include <sys/namei.h>			/* pathbuf */
#include <sys/fcntl.h>			/* FREAD / FWRITE */
#include <sys/mutex.h>
#include <sys/conf.h>
#include <miscfs/specfs/specdev.h>	/* v_rdev on the device vnode */

/*
 * QUARANTINE (the vms_lnm_arena_netbsd.c precedent). This TU pulls the heavy
 * NetBSD block/vnode headers -- sys/buf.h drags <sys/rbtree.h>, whose
 * rb_left/rb_right macros COLLIDE with the executive's exec_rbtree_netbsd.h. So
 * it must NOT include vms_internal.h (which pulls exec_rbtree.h). Instead it
 * forward-declares the executive symbols it defines and inlines the two SS$_
 * status codes it returns. Canonical source: vms_internal.h (SS__NORMAL /
 * SS__NOSUCHDEV) and exec_kbackend.h S8 (exec_blockdev_*).
 */
#ifndef SS__NORMAL
#define SS__NORMAL	0x00000001u	/* SS$_NORMAL   (vms_internal.h) */
#endif
#ifndef SS__NOSUCHDEV
#define SS__NOSUCHDEV	2680u		/* SS$_NOSUCHDEV (vms_internal.h) */
#endif

uint32_t vms_devtab_disk_backing(const char *devnam, uint32_t *major_out,
				 uint32_t *minor_out);
uint32_t vms_devtab_disk_resolve(const char *devnam, char *backing,
				 size_t backing_sz, uint32_t *major_out,
				 uint32_t *minor_out);
void vms_blockdev_netbsd_release_all(void);
void vms_blockdev_netbsd_register_units(void);
/* The executive device table's one substrate entry point (rd vms-618). DEFINED
 * in the shared src/kernel-core/vms_devtab.c; declared here rather than pulled
 * from vms_internal.h because of this TU's rbtree quarantine (see below). */
int vms_devtab_add_disk(const char *devnam, const char *backing,
			uint32_t backing_major, uint32_t backing_minor);
int exec_blockdev_read_block(unsigned int major_, unsigned int minor_,
			     uint64_t lbn, void *buf, size_t buflen);
int exec_blockdev_write_block(unsigned int major_, unsigned int minor_,
			      uint64_t lbn, const void *buf, size_t buflen);

/*
 * VMS_IOCTL_DISK_RESOLVE handler (rd vms-f60). struct vms_proc is opaque here
 * (the handler does not touch it -- INITIALIZE opens the target itself, the
 * executive only names it), so a forward declaration suffices; the quarantine
 * (no vms_internal.h) stands. struct vms_diskresolve_args comes from the shared
 * /dev/vms contract header vms_acp_nb.h, which -- unlike vms_internal.h -- pulls
 * no exec_rbtree twin and so does not collide with sys/rbtree.h.
 */
struct vms_proc;
long vms_ioctl_disk_resolve(struct vms_proc *proc, unsigned long arg);
#include "vms_acp_nb.h"

#define OVMX_ACP_BLOCK_SIZE	512u	/* ODS-2 LBN == 512-byte block */

#ifndef OVMX_ACP_MAX_DISKS
#define OVMX_ACP_MAX_DISKS	4	/* single unit today; table for future */
#endif

/*
 * VMS unit name -> native NetBSD block device path. Device-native (vms-47d):
 * the name is a label, the path is the real device. Accept the current
 * placeholder DKA0: AND the correct MSCP name DUA0: so the resolve survives the
 * vms-47d rename either way.
 */
static const struct ovmx_acp_unit {
	const char *vmsname;		/* upcased, colon-stripped */
	const char *devpath;
	const char *backing;		/* native block-dev name (vms-f60 resolve) */
	int	    primary;		/* 1 = the name this unit is ENTERED under */
} ovmx_acp_unitmap[] = {
	{ "DKA0",   "/dev/ra1c", "ra1c", 1 },	/* placeholder name, pending vms-47d */
	{ "DUA0",   "/dev/ra1c", "ra1c", 0 },	/* the real VMS-VAX MSCP name */
	{ "DKA100", "/dev/ra2c", "ra2c", 1 },	/* 2nd MSCP disk = INITIALIZE target */
	{ "DUA100", "/dev/ra2c", "ra2c", 0 },	/* its correct MSCP name */
};

/*
 * WHY `primary' (rd vms-618). The map has FOUR rows but names only TWO disks:
 * DUA0:/DUA100: are the correct MSCP names for the same units the runtime still
 * calls DKA0:/DKA100: (the vms-47d rename is open), and the two RESOLVERS below
 * accept either spelling so they stay correct across that rename. The executive
 * DEVICE TABLE cannot: a table row carries OWNERSHIP, so entering both spellings
 * would create two independently-allocatable devices for one physical disk --
 * ALLOCATE DKA100: would not conflict with ALLOCATE DUA100:, which is a lie
 * about the hardware (INV-6). So exactly the `primary' rows are ENTERED, and the
 * alias stays a resolver-only spelling until the rename lands and the flag moves.
 */

/* Open-once-per-volume cache: bound at $MOUNT, reused by every block op, closed
 * at teardown. Single entry today, indexed by dev_t. */
struct ovmx_acp_disk {
	int		in_use;
	dev_t		dev;
	struct vnode   *devvp;
	char		vmsname[16];
};

static kmutex_t			ovmx_acp_disk_lk;
static int			ovmx_acp_disk_lk_inited;	/* module-load once */
static struct ovmx_acp_disk	ovmx_acp_disks[OVMX_ACP_MAX_DISKS];

/* The SYS$DISK $MOUNT is serialized (PID 1, once at boot), so a lazy init-once
 * of the guard mutex is race-free in practice; guard the flag regardless. */
static void
ovmx_acp_disk_init_once(void)
{
	if (!ovmx_acp_disk_lk_inited) {
		mutex_init(&ovmx_acp_disk_lk, MUTEX_DEFAULT, IPL_NONE);
		ovmx_acp_disk_lk_inited = 1;
	}
}

/* Normalize a VMS unit spec ("dka0:", "DKA0", "DKA0:") to upcased,
 * colon-stripped ("DKA0") for the unit-map compare. */
static void
ovmx_acp_normname(const char *in, char *out, size_t outsz)
{
	size_t i = 0;

	if (outsz == 0)
		return;
	for (; in && *in && *in != ':' && i + 1 < outsz; in++) {
		char c = *in;
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		out[i++] = c;
	}
	out[i] = '\0';
}

static struct vnode *
ovmx_acp_disk_find(unsigned int major_, unsigned int minor_)
{
	dev_t dev = makedev((int)major_, (int)minor_);
	struct vnode *vp = NULL;
	int i;

	if (!ovmx_acp_disk_lk_inited)
		return NULL;
	mutex_enter(&ovmx_acp_disk_lk);
	for (i = 0; i < OVMX_ACP_MAX_DISKS; i++) {
		if (ovmx_acp_disks[i].in_use && ovmx_acp_disks[i].dev == dev) {
			vp = ovmx_acp_disks[i].devvp;
			break;
		}
	}
	mutex_exit(&ovmx_acp_disk_lk);
	return vp;
}

/*
 * vms_devtab_disk_backing - resolve a VMS disk unit name to its backing
 * (major,minor), opening the device vnode ONCE and caching it for the volume's
 * life. The Files-11 ACP $MOUNT (kernel-core/vmsfs_acp.c) calls this, then
 * validates ODS-2 and drives all subsequent block I/O through the cached
 * handle. Returns a VMS status (odd == success): SS$_NORMAL on bind,
 * SS$_NOSUCHDEV for an unknown unit or an open failure (fail-honest -- Rule 9 /
 * INV-6; no fabricated success).
 */
uint32_t
vms_devtab_disk_backing(const char *devnam, uint32_t *major_out,
			uint32_t *minor_out)
{
	char norm[16];
	const char *devpath = NULL;
	struct pathbuf *pb;
	struct vnode *devvp = NULL;
	int i, error, slot;

	if (devnam == NULL || major_out == NULL || minor_out == NULL)
		return SS__NOSUCHDEV;

	ovmx_acp_normname(devnam, norm, sizeof(norm));

	for (i = 0; i < (int)__arraycount(ovmx_acp_unitmap); i++) {
		if (strcmp(norm, ovmx_acp_unitmap[i].vmsname) == 0) {
			devpath = ovmx_acp_unitmap[i].devpath;
			break;
		}
	}
	if (devpath == NULL)
		return SS__NOSUCHDEV;	/* unknown unit -- honest */

	ovmx_acp_disk_init_once();

	/* Already bound? Return the cached (major,minor). */
	mutex_enter(&ovmx_acp_disk_lk);
	for (i = 0; i < OVMX_ACP_MAX_DISKS; i++) {
		if (ovmx_acp_disks[i].in_use &&
		    strcmp(ovmx_acp_disks[i].vmsname, norm) == 0) {
			*major_out = (uint32_t)major(ovmx_acp_disks[i].dev);
			*minor_out = (uint32_t)minor(ovmx_acp_disks[i].dev);
			mutex_exit(&ovmx_acp_disk_lk);
			return SS__NORMAL;
		}
	}
	mutex_exit(&ovmx_acp_disk_lk);

	/*
	 * Open the block device by path ONCE, exactly as vmsfs_vfsops.c does
	 * (vn_bdev_openpath, FREAD|FWRITE). devpath is a KERNEL constant, so
	 * pathbuf_create (not pathbuf_copyin, which copies from userspace).
	 */
	pb = pathbuf_create(devpath);
	if (pb == NULL)
		return SS__NOSUCHDEV;
	error = vn_bdev_openpath(pb, &devvp, curlwp);
	pathbuf_destroy(pb);
	if (error || devvp == NULL)
		return SS__NOSUCHDEV;	/* device absent/unopenable -- honest */

	mutex_enter(&ovmx_acp_disk_lk);
	slot = -1;
	for (i = 0; i < OVMX_ACP_MAX_DISKS; i++) {
		if (!ovmx_acp_disks[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		mutex_exit(&ovmx_acp_disk_lk);
		(void)vn_close(devvp, FREAD | FWRITE, curlwp->l_cred);
		return SS__NOSUCHDEV;	/* table full -- honest */
	}
	ovmx_acp_disks[slot].in_use = 1;
	ovmx_acp_disks[slot].dev = devvp->v_rdev;
	ovmx_acp_disks[slot].devvp = devvp;
	strlcpy(ovmx_acp_disks[slot].vmsname, norm,
	    sizeof(ovmx_acp_disks[slot].vmsname));
	*major_out = (uint32_t)major(devvp->v_rdev);
	*minor_out = (uint32_t)minor(devvp->v_rdev);
	mutex_exit(&ovmx_acp_disk_lk);
	return SS__NORMAL;
}

/*
 * vms_devtab_disk_resolve - resolve a VMS disk unit name to its backing block
 * device NAME + (major,minor), WITHOUT caching the vnode (rd vms-f60). This is
 * the deliberate twin of vms_devtab_disk_backing: $MOUNT caches SYS$DISK for
 * its whole life because the ACP does all the block I/O through that handle;
 * INITIALIZE.EXE, by contrast, opens the target device ITSELF and writes the
 * fresh ODS-2 structure -- the executive only NAMES the device for it. So this
 * resolver opens the device purely to read its dev_t (major/minor) and its
 * native name, then closes it again with the SAME mode (FREAD|FWRITE) the open
 * used -- the vmsfs_vfsops.c "vrelel: bad ref count" discipline. No cache slot
 * is consumed, so resolving a unit never perturbs a mounted SYS$DISK binding.
 *
 * Returns a VMS status (odd == success): SS$_NORMAL on resolve, SS$_NOSUCHDEV
 * for an unknown unit or an open failure (fail-honest -- Rule 9 / INV-6; no
 * fabricated success, no invented device name).
 */
uint32_t
vms_devtab_disk_resolve(const char *devnam, char *backing, size_t backing_sz,
			uint32_t *major_out, uint32_t *minor_out)
{
	char norm[16];
	const char *devpath = NULL;
	const char *backname = NULL;
	struct pathbuf *pb;
	struct vnode *devvp = NULL;
	int i, error;

	if (devnam == NULL || backing == NULL || backing_sz == 0 ||
	    major_out == NULL || minor_out == NULL)
		return SS__NOSUCHDEV;

	ovmx_acp_normname(devnam, norm, sizeof(norm));

	for (i = 0; i < (int)__arraycount(ovmx_acp_unitmap); i++) {
		if (strcmp(norm, ovmx_acp_unitmap[i].vmsname) == 0) {
			devpath  = ovmx_acp_unitmap[i].devpath;
			backname = ovmx_acp_unitmap[i].backing;
			break;
		}
	}
	if (devpath == NULL)
		return SS__NOSUCHDEV;	/* unknown unit -- honest */

	/*
	 * Open the block device by its KERNEL-constant path (pathbuf_create, not
	 * pathbuf_copyin) ONLY to read its v_rdev, then close it right back. No
	 * cache slot, no lock: this resolve owns nothing past return.
	 */
	pb = pathbuf_create(devpath);
	if (pb == NULL)
		return SS__NOSUCHDEV;
	error = vn_bdev_openpath(pb, &devvp, curlwp);
	pathbuf_destroy(pb);
	if (error || devvp == NULL)
		return SS__NOSUCHDEV;	/* device absent/unopenable -- honest */

	*major_out = (uint32_t)major(devvp->v_rdev);
	*minor_out = (uint32_t)minor(devvp->v_rdev);
	(void)vn_close(devvp, FREAD | FWRITE, curlwp->l_cred);

	strlcpy(backing, backname, backing_sz);
	return SS__NORMAL;
}

/*
 * vms_ioctl_disk_resolve - the VMS_IOCTL_DISK_RESOLVE /dev/vms handler (rd
 * vms-f60), letting INITIALIZE.EXE turn a VMS unit name ("DKA0:") into the real
 * backing device it labels. `arg' is the cdevsw framework's kernel buffer for
 * the <8KB _IOWR (already copied in from userspace by the framework), so this
 * quarantined TU uses plain memcpy to/from (void*)arg -- NOT exec_copyin (which
 * lives behind vms_internal.h). `proc' is unused: naming a device needs no
 * process context. Always returns 0 (a facility-level status rides back in
 * a.status); a bad unit is SS$_NOSUCHDEV in-band, not an ioctl errno.
 */
long
vms_ioctl_disk_resolve(struct vms_proc *proc, unsigned long arg)
{
	struct vms_diskresolve_args a;

	(void)proc;

	memcpy(&a, (const void *)arg, sizeof(a));
	a.devnam[VMS_DEVNAM_SIZE - 1] = '\0';	/* trust no userspace NUL */

	a.status = vms_devtab_disk_resolve(a.devnam, a.backing,
	    sizeof(a.backing), &a.backing_major, &a.backing_minor);

	memcpy((void *)arg, &a, sizeof(a));
	return 0;
}

/*
 * vms_blockdev_netbsd_register_units - enter this node's DISK units in the
 * EXECUTIVE DEVICE TABLE (rd vms-618), the way a VMS driver enters its units in
 * the I/O database during system initialization.
 *
 * WHY THE SUBSTRATE DOES THIS. The shared facility's own enumeration
 * (vms_devtab_probe_disks, src/kernel-core/vms_devtab.c) walks the Linux
 * virtio-blk name space /dev/vda../dev/vdz, which does not exist here: on
 * NetBSD/vax the node's disks are MSCP units, and the device-native unit map
 * (vms-47d -- the VMS name is a LABEL, the path is the real device) lives in
 * THIS TU. Everything a device MEANS -- the row, its ownership, allocation and
 * reference count -- stays in the one shared facility; only WHICH units exist is
 * the substrate's to say.
 *
 * INV-6 / Rule 9: a unit is entered ONLY for a device that really resolves. The
 * resolve is vms_devtab_disk_resolve() above -- a real open of the real block
 * device to read its dev_t, closed again immediately -- so an absent disk (e.g.
 * a single-disk sysboot with no rq2 attached) simply has NO table row, and
 * $ALLOC DKA100: is then an honest SS$_NOSUCHDEV. Nothing is invented for a
 * device that is not there.
 *
 * TRANSIENT open, DELIBERATELY. NetBSD allows effectively one open of a block
 * device, so this must not HOLD anything: SYS$DISK is opened and cached later,
 * at $MOUNT (vms_devtab_disk_backing), and the INITIALIZE target is opened by
 * INITIALIZE.EXE itself (rd vms-f60). Called once from vms_modcmd(INIT), before
 * /dev/vms exists and before any $MOUNT, so this open never races either.
 */
void
vms_blockdev_netbsd_register_units(void)
{
	int i;

	for (i = 0; i < (int)__arraycount(ovmx_acp_unitmap); i++) {
		char backing[VMS_BACKING_SIZE];
		char devnam[VMS_DEVNAM_SIZE];
		uint32_t maj = 0, min = 0, st;

		if (!ovmx_acp_unitmap[i].primary)
			continue;	/* alias spelling -- resolver-only (above) */

		st = vms_devtab_disk_resolve(ovmx_acp_unitmap[i].vmsname, backing,
		    sizeof(backing), &maj, &min);
		if ((st & 1u) == 0) {
			printf("vms: disk unit %s: no backing device (%s absent)"
			    " -- not entered\n", ovmx_acp_unitmap[i].vmsname,
			    ovmx_acp_unitmap[i].devpath);
			continue;	/* honest: no device, no unit */
		}

		/* The table is keyed by the canonical physical form "DDCU:". */
		strlcpy(devnam, ovmx_acp_unitmap[i].vmsname, sizeof(devnam));
		strlcat(devnam, ":", sizeof(devnam));

		(void)vms_devtab_add_disk(devnam, backing, maj, min);
	}
}

/*
 * exec_blockdev_read_block - read ONE 512-byte LBN off the cached backing
 * device via bread(9) (the exact primitive vmsfs_readblk uses). Returns 0 on
 * success, -1 on any failure (contract of exec_kbackend.h S8). Honest failure
 * if the unit was never $MOUNTed (no cached vnode).
 */
int
exec_blockdev_read_block(unsigned int major_, unsigned int minor_,
			 uint64_t lbn, void *buf, size_t buflen)
{
	struct vnode *devvp;
	struct buf *bp = NULL;
	int error;

	if (buf == NULL || buflen < OVMX_ACP_BLOCK_SIZE)
		return -1;
	devvp = ovmx_acp_disk_find(major_, minor_);
	if (devvp == NULL)
		return -1;	/* not bound -- honest */

	error = bread(devvp, (daddr_t)lbn, OVMX_ACP_BLOCK_SIZE, 0, &bp);
	if (error || bp == NULL) {
		if (bp != NULL)
			brelse(bp, 0);
		return -1;
	}
	memcpy(buf, bp->b_data, OVMX_ACP_BLOCK_SIZE);
	brelse(bp, 0);
	return 0;
}

/*
 * exec_blockdev_write_block - commit ONE 512-byte LBN to the cached backing
 * device with a SYNCHRONOUS write (getblk(9) + bwrite(9)) -- the NetBSD twin of
 * Linux's submit_bio_wait/REQ_OP_WRITE. The ACP writes a RAW volume (no mounted
 * super_block), so this is a full-block replace, not a read-modify-write.
 * Returns 0 on success, -1 on failure.
 */
int
exec_blockdev_write_block(unsigned int major_, unsigned int minor_,
			  uint64_t lbn, const void *buf, size_t buflen)
{
	struct vnode *devvp;
	struct buf *bp;
	int error;

	if (buf == NULL || buflen < OVMX_ACP_BLOCK_SIZE)
		return -1;
	devvp = ovmx_acp_disk_find(major_, minor_);
	if (devvp == NULL)
		return -1;	/* not bound -- honest */

	bp = getblk(devvp, (daddr_t)lbn, OVMX_ACP_BLOCK_SIZE, 0, 0);
	if (bp == NULL)
		return -1;
	memcpy(bp->b_data, buf, OVMX_ACP_BLOCK_SIZE);
	error = bwrite(bp);	/* synchronous; disposes bp */
	return error ? -1 : 0;
}

/*
 * vms_blockdev_netbsd_release_all - close every cached backing vnode with the
 * SAME mode vn_bdev_openpath opened it (FREAD|FWRITE), avoiding the
 * "vrelel: bad ref count" panic vmsfs_vfsops.c documents. Called at module
 * detach (SYS$DISK stays mounted for the OS life, so there is no normal
 * $DMOUNT of it).
 */
void
vms_blockdev_netbsd_release_all(void)
{
	int i;

	if (!ovmx_acp_disk_lk_inited)
		return;
	mutex_enter(&ovmx_acp_disk_lk);
	for (i = 0; i < OVMX_ACP_MAX_DISKS; i++) {
		if (ovmx_acp_disks[i].in_use) {
			(void)vn_close(ovmx_acp_disks[i].devvp,
			    FREAD | FWRITE, curlwp->l_cred);
			ovmx_acp_disks[i].in_use = 0;
			ovmx_acp_disks[i].devvp = NULL;
		}
	}
	mutex_exit(&ovmx_acp_disk_lk);
}
