/*
 * vmsfs_vfsops.c - the OVMX/NetBSD ODS-2 vnode/VFS backend (rd vms-308,
 * epic vms-8e8; docs/design-ovmx-netbsd-syskrnl.md; the ODS-2 analog of the
 * NetBSD executive module src/kernel-netbsd/vms_netbsd.c).
 *
 * The NetBSD-substrate sibling of the Linux vmsfs.ko block-device backend
 * (src/kernel/vmsfs/): a real in-kernel filesystem module (module(9),
 * MODULE_CLASS_VFS) that mounts a mastered OVMX ODS-2 volume and READS it. It
 * provides a struct vfsops (VFS_MOUNT / VFS_ROOT / VFS_UNMOUNT / VFS_STATVFS /
 * VFS_LOADVNODE) and a struct vnodeops (VOP_LOOKUP / VOP_READ / VOP_READDIR /
 * VOP_GETATTR / VOP_ACCESS / ...), and the NetBSD realization of the
 * block/inode seam lives in vmsfs_backend_netbsd.h.
 *
 * IT COMPILES THE SAME src/kernel-core/vmsfs sources the Linux vmsfs.ko builds
 * -- there is exactly ONE implementation of every ODS-2 decision (the on-disk
 * format, the file-header decode/encode, the version resolution, the retrieval-
 * map math, the directory-block scanner, the storage allocator). This file is
 * PURELY the NetBSD VFS glue: it walks the vnode/uio/vattr world and delegates
 * every ODS-2 question to the shared core via struct vmsfs_volume + the
 * vmsfs_bio ops (vmsfs_fh_decode, vmsfs_dir_resolve, vmsfs_dir_entry_decode,
 * vmsfs_dir_format_name, vmsfs_vbn_to_lbn). Never re-derives the format -- that
 * would be the exact backend drift epic vms-8e5 exists to prevent (INV-DRIFT).
 *
 * READ-ONLY. This backend mounts the volume MNT_RDONLY: the mount+READ proof
 * (mount a mastered ODS-2 disk on NetBSD/amd64, read DCL.EXE) needs no write
 * path, and a read-only mount is a complete, honest feature. The shared
 * allocator / directory-add / header-write cores are still COMPILED and LINKED
 * here (proving the whole ODS-2 core cross-compiles against the NetBSD
 * backend); they are simply not driven at run time. The write VOPs map to
 * genfs_eopnotsupp, and VOP_ACCESS refuses VWRITE with EROFS.
 *
 * TOOLING, NOT A RUNTIME OF ITS OWN (CLAUDE.md Rule 9): booting a real NetBSD to
 * load and exercise this real kernel module is exactly what tests/qemu/ does for
 * the Linux vmsfs.ko. NetBSD-as-a-real-runtime is the product goal of epic
 * vms-8e8; this module is a step toward it.
 *
 * CLEAN ROOM (CLAUDE.md Rule 8): written from the public NetBSD vfsops(9) /
 * vnodeops(9) / vcache / buf(9) / module(9) interfaces and the OVMX ODS-2
 * on-disk format. No NetBSD or VSI/HPE source is copied; every ODS-2 decision
 * lives in the shared, oracle-aligned core.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/namei.h>
#include <sys/dirent.h>
#include <sys/kmem.h>
#include <sys/buf.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/errno.h>
#include <sys/proc.h>

#include <miscfs/genfs/genfs.h>
#include <miscfs/specfs/specdev.h>   /* v_rdev on the device vnode */
#include <uvm/uvm_extern.h>          /* uvm_vnp_setsize */

#include "vmsfs_nb.h"

/* ================================================================
 * Forward declarations
 * ================================================================ */

static int vmsfs_vfs_mount(struct mount *, const char *, void *, size_t *);
static int vmsfs_vfs_start(struct mount *, int);
static int vmsfs_vfs_unmount(struct mount *, int);
static int vmsfs_vfs_root(struct mount *, int, struct vnode **);
static int vmsfs_vfs_statvfs(struct mount *, struct statvfs *);
static int vmsfs_vfs_sync(struct mount *, int, struct kauth_cred *);
static int vmsfs_vfs_loadvnode(struct mount *, struct vnode *,
    const void *, size_t, const void **);
static void vmsfs_vfs_init(void);
static void vmsfs_vfs_done(void);

static int vmsfs_lookup(void *);
static int vmsfs_open(void *);
static int vmsfs_close(void *);
static int vmsfs_access(void *);
static int vmsfs_getattr(void *);
static int vmsfs_read(void *);
static int vmsfs_readdir(void *);
static int vmsfs_bmap(void *);
static int vmsfs_strategy(void *);
static int vmsfs_inactive(void *);
static int vmsfs_reclaim(void *);
static int vmsfs_print(void *);

/* ================================================================
 * A single 512-byte block read helper (mount-time reads that are not routed
 * through the core's vmsfs_bio wrapper). Returns 0 + a held buf on success.
 * ================================================================ */

static int
vmsfs_readblk(struct vnode *devvp, uint32_t lbn, struct buf **bpp)
{
	int error = bread(devvp, (daddr_t)lbn, VMSFS_BLOCK_SIZE, 0, bpp);

	/*
	 * Guarantee *bpp == NULL on failure so every caller's error path can
	 * skip the release unconditionally (bread already disposes the buf on
	 * error; we must never brelse it a second time).
	 */
	if (error)
		*bpp = NULL;
	return error;
}

/*
 * genfs ops vector for the UVM/UBC vnode pager. Read-only, so exactly cd9660's
 * shape: only gop_size (genfs_size derives EOF from vp->v_size, which
 * vmsfs_vfs_loadvnode sets via uvm_vnp_setsize). gop_write / gop_alloc are the
 * write path (page-out / block allocation) and are never reached -- putpages is
 * genfs_null_putpages and the mount is MNT_RDONLY.
 */
static const struct genfs_ops vmsfs_genfsops = {
	.gop_size = genfs_size,
};

/* ================================================================
 * VFS operations
 * ================================================================ */

/*
 * vmsfs_vfs_mount - mount a mastered ODS-2 volume READ-ONLY.
 *
 * Mirrors the Linux vmsfs_fill_super_blkdev(): open the device, read + validate
 * the home block (magic + checksum) via the shared on-disk format, extract the
 * geometry into struct vmsfs_volume (host == devvp), and load the MFD root
 * through vfs_loadvnode. No write path -- the mount is forced MNT_RDONLY.
 */
static int
vmsfs_vfs_mount(struct mount *mp, const char *path, void *data, size_t *data_len)
{
	struct lwp *l = curlwp;
	struct vmsfs_args *args = data;
	struct vmsfs_mount *vmp;
	struct pathbuf *pb;
	struct vnode *devvp = NULL;
	struct buf *bp = NULL;
	struct vmsfs_home_block *hb;
	uint32_t cksum;
	int error;

	if (args == NULL)
		return EINVAL;
	if (*data_len < sizeof(*args))
		return EINVAL;

	/* MNT_GETARGS: hand back the fspec path we mounted. */
	if (mp->mnt_flag & MNT_GETARGS) {
		vmp = VFSTOVMSFS(mp);
		if (vmp == NULL)
			return EIO;
		args->fspec = NULL;
		*data_len = sizeof(*args);
		return 0;
	}

	/* This backend is read-only; a rw update request is refused. */
	if (mp->mnt_flag & MNT_UPDATE) {
		if ((mp->mnt_flag & MNT_RDONLY) == 0)
			return EOPNOTSUPP;
		return 0;
	}

	/* Force read-only regardless of what the caller asked for. */
	mp->mnt_flag |= MNT_RDONLY;

	if (args->fspec == NULL)
		return EINVAL;

	/* Resolve + open the block device by path. */
	error = pathbuf_copyin(args->fspec, &pb);
	if (error)
		return error;
	error = vn_bdev_openpath(pb, &devvp, l);
	pathbuf_destroy(pb);
	if (error)
		return error;

	vmp = kmem_zalloc(sizeof(*vmp), KM_SLEEP);
	vmp->vm_mp = mp;
	vmp->vm_devvp = devvp;
	vmp->vm_dev = devvp->v_rdev;
	vmp->vm_case_blind = 1;
	mutex_init(&vmp->vm_alloc_lock, MUTEX_DEFAULT, IPL_NONE);

	/* Read + validate the home block (block 1) via the shared on-disk format. */
	error = vmsfs_readblk(devvp, VMSFS_HOME_LBN, &bp);
	if (error)
		goto fail;
	hb = (struct vmsfs_home_block *)bp->b_data;

	if (vmsfs_le32_to_cpu(hb->hb_magic) != VMSFS_HOME_MAGIC) {
		printf("vmsfs: bad home block magic 0x%08x\n",
		    vmsfs_le32_to_cpu(hb->hb_magic));
		error = EINVAL;
		goto fail;
	}
	cksum = vmsfs_checksum(hb, sizeof(*hb));
	if (cksum != vmsfs_le32_to_cpu(hb->hb_checksum)) {
		printf("vmsfs: home block checksum mismatch\n");
		error = EINVAL;
		goto fail;
	}

	memcpy(&vmp->vm_home, hb, sizeof(*hb));

	/*
	 * Geometry into the host-neutral volume descriptor. vol.host == devvp, so
	 * the shared core's vmsfs_bget(vol->host, lbn) reads the volume directly.
	 * The storage bitmap is NOT loaded: it is needed only by the allocator,
	 * which a read-only mount never drives (vol.bitmap stays NULL).
	 */
	vmp->vm_vol.host          = devvp;
	vmp->vm_vol.bitmap        = NULL;
	vmp->vm_vol.bitmap_lbn    = vmsfs_le32_to_cpu(hb->hb_bitmap_lbn);
	vmp->vm_vol.bitmap_blocks = vmsfs_le32_to_cpu(hb->hb_bitmap_blocks);
	vmp->vm_vol.index_lbn     = vmsfs_le32_to_cpu(hb->hb_index_lbn);
	vmp->vm_vol.max_files     = vmsfs_le32_to_cpu(hb->hb_max_files);
	vmp->vm_vol.data_lbn      = vmsfs_le32_to_cpu(hb->hb_data_lbn);
	vmp->vm_vol.total_blocks  = vmsfs_le32_to_cpu(hb->hb_total_blocks);
	vmp->vm_vol.free_blocks   = vmsfs_le32_to_cpu(hb->hb_free_blocks);

	brelse(bp, 0);
	bp = NULL;

	mp->mnt_data = vmp;
	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_stat.f_namemax = VMSFS_FULLNAME_MAX;

	/*
	 * UBC/UVM vnode-pager geometry. The UVM/UBC read pager (genfs_getpages ->
	 * VOP_BMAP -> VOP_STRATEGY) translates a file offset to a device block in
	 * two steps and needs both shifts: mnt_fs_bshift is log2(fs block size) and
	 * mnt_dev_bshift is log2(device block size). An ODS-2 volume is addressed in
	 * 512-byte logical blocks (VMSFS_BLOCK_SIZE) and the underlying disk in
	 * 512-byte DEV_BSIZE sectors, so both shifts are DEV_BSHIFT (== 9) and
	 * VOP_BMAP returns the LBN unscaled. Without these the pager computes bogus
	 * block numbers (fs_bshift defaults to 0) and demand-paging an image off the
	 * volume faults garbage -- this is the seam that lets an ELF32-vax image on
	 * the mounted volume demand-page and RUN (rd vms-63a).
	 */
	__CTASSERT(VMSFS_BLOCK_SIZE == DEV_BSIZE);
	mp->mnt_fs_bshift  = DEV_BSHIFT;
	mp->mnt_dev_bshift = DEV_BSHIFT;

	vfs_getnewfsid(mp);

	error = set_statvfs_info(path, UIO_USERSPACE, args->fspec, UIO_USERSPACE,
	    mp->mnt_op->vfs_name, mp, l);
	if (error)
		goto fail;

	return 0;

fail:
	if (bp != NULL)
		brelse(bp, 0);
	if (devvp != NULL)
		(void)vn_close(devvp, FREAD, l->l_cred);
	mutex_destroy(&vmp->vm_alloc_lock);
	kmem_free(vmp, sizeof(*vmp));
	mp->mnt_data = NULL;
	return error;
}

static int
vmsfs_vfs_start(struct mount *mp, int flags)
{
	(void)mp;
	(void)flags;
	return 0;
}

static int
vmsfs_vfs_unmount(struct mount *mp, int mntflags)
{
	struct vmsfs_mount *vmp = VFSTOVMSFS(mp);
	int error;
	int flags = 0;

	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;

	error = vflush(mp, NULL, flags);
	if (error)
		return error;

	/*
	 * Close the device with the SAME mode vn_bdev_openpath() opened it:
	 * vn_bdev_open() does VOP_OPEN(FREAD | FWRITE) and bumps v_writecount, so
	 * closing FREAD-only leaves the open/ref counts imbalanced and panics
	 * ("vrelel: bad ref count") when the last vnode ref is dropped at unmount.
	 * Every vn_bdev_openpath() consumer (dev/dm, dev/ccd) closes FREAD | FWRITE.
	 */
	if (vmp->vm_devvp != NULL)
		(void)vn_close(vmp->vm_devvp, FREAD | FWRITE, curlwp->l_cred);

	mutex_destroy(&vmp->vm_alloc_lock);
	kmem_free(vmp, sizeof(*vmp));
	mp->mnt_data = NULL;
	mp->mnt_flag &= ~MNT_LOCAL;
	return 0;
}

/*
 * vmsfs_vfs_root - return the volume root (the MFD, FID 3), locked.
 */
static int
vmsfs_vfs_root(struct mount *mp, int lktype, struct vnode **vpp)
{
	struct vnode *vp;
	uint32_t fid = VMSFS_FID_MFD;
	int error;

	error = vcache_get(mp, &fid, sizeof(fid), &vp);
	if (error)
		return error;
	error = vn_lock(vp, lktype);
	if (error) {
		vrele(vp);
		return error;
	}
	*vpp = vp;
	return 0;
}

static int
vmsfs_vfs_statvfs(struct mount *mp, struct statvfs *sbp)
{
	struct vmsfs_mount *vmp = VFSTOVMSFS(mp);

	sbp->f_bsize   = VMSFS_BLOCK_SIZE;
	sbp->f_frsize  = VMSFS_BLOCK_SIZE;
	sbp->f_iosize  = VMSFS_BLOCK_SIZE;
	sbp->f_blocks  = vmp->vm_vol.total_blocks;
	sbp->f_bfree   = vmp->vm_vol.free_blocks;
	sbp->f_bavail  = vmp->vm_vol.free_blocks;
	sbp->f_bresvd  = 0;
	sbp->f_files   = vmp->vm_vol.max_files;
	sbp->f_ffree   = 0;
	sbp->f_favail  = 0;
	sbp->f_fresvd  = 0;
	sbp->f_namemax = VMSFS_FULLNAME_MAX;
	copy_statvfs_info(sbp, mp);
	return 0;
}

/* Read-only: nothing is ever dirty. */
static int
vmsfs_vfs_sync(struct mount *mp, int waitfor, struct kauth_cred *cred)
{
	(void)mp;
	(void)waitfor;
	(void)cred;
	return 0;
}

/*
 * vmsfs_vfs_loadvnode - construct a vnode for the FID in @key by reading and
 * decoding its ODS-2 file header via the shared core (vmsfs_fh_decode). This is
 * the NetBSD twin of the Linux vmsfs_blkdev_iget()'s "read header -> populate
 * the in-core inode" half; the ODS-2 decode is the SAME core function, and only
 * the POD -> vnode copy is NetBSD-specific.
 */
static int
vmsfs_vfs_loadvnode(struct mount *mp, struct vnode *vp,
    const void *key, size_t key_len, const void **new_key)
{
	struct vmsfs_mount *vmp = VFSTOVMSFS(mp);
	struct vmsfs_node *vn;
	struct vmsfs_fh_info fhi;
	struct buf *bp = NULL;
	uint32_t fid;
	uint32_t lbn;
	unsigned int i;
	int error;

	if (key_len != sizeof(uint32_t))
		return EINVAL;
	memcpy(&fid, key, sizeof(fid));

	if (fid == 0 || fid > vmp->vm_vol.max_files)
		return EINVAL;

	/* FID is 1-based; the index (file-header) area starts at index_lbn. */
	lbn = vmp->vm_vol.index_lbn + fid - 1;
	error = vmsfs_readblk(vmp->vm_devvp, lbn, &bp);
	if (error)
		return error;

	/* Validate + decode the ODS-2 header via the shared core. */
	switch (vmsfs_fh_decode(bp->b_data, &fhi)) {
	case VMSFS_FH_OK:
		break;
	case VMSFS_FH_BAD_MAGIC:
	case VMSFS_FH_BAD_CHECKSUM:
		brelse(bp, 0);
		return EIO;
	case VMSFS_FH_NOT_INUSE:
	default:
		brelse(bp, 0);
		return ENOENT;
	}

	vn = kmem_zalloc(sizeof(*vn), KM_SLEEP);
	vn->vn_vp         = vp;
	vn->vn_vmp        = vmp;
	vn->vn_fid        = fid;
	vn->vn_key        = fid;
	vn->vn_parent_fid = fhi.parent_fid;
	vn->vn_flags      = fhi.flags;
	vn->vn_version    = fhi.version;
	vn->vn_prot       = fhi.protection;
	vn->vn_uic_group  = fhi.uic_group;
	vn->vn_uic_member = fhi.uic_member;
	vn->vn_link_count = fhi.link_count;
	vn->vn_size       = fhi.size;
	vn->vn_blocks     = fhi.blocks;
	vn->vn_created    = fhi.created;
	vn->vn_modified   = fhi.modified;
	vn->vn_accessed   = fhi.accessed;
	vn->vn_map_count  = fhi.map_count;
	for (i = 0; i < fhi.map_count && i < VMSFS_MAX_RETRIEVAL; i++)
		vn->vn_map[i] = fhi.map[i];

	brelse(bp, 0);

	vp->v_tag  = VT_NON;   /* OVMX ODS-2: not one of the built-in vtag types */
	vp->v_op   = vmsfs_vnodeop_p;
	vp->v_data = vn;
	vp->v_type = (fhi.flags & VMSFS_FH_DIRECTORY) ? VDIR : VREG;
	if (fid == VMSFS_FID_MFD)
		vp->v_vflag |= VV_ROOT;

	uvm_vnp_setsize(vp, (voff_t)fhi.size);

	/*
	 * Arm the genfs vnode pager hook (vn_gnode is the first member of the
	 * private data, so VTOG(vp) aliases it). Required before any VOP_GETPAGES:
	 * genfs_node_init() installs the ops vector + initializes the getpages
	 * rangelock genfs_getpages() takes. Torn down in vmsfs_reclaim().
	 */
	genfs_node_init(vp, &vmsfs_genfsops);

	*new_key = &vn->vn_key;
	return 0;
}

static void
vmsfs_vfs_init(void)
{
}

static void
vmsfs_vfs_done(void)
{
}

/* ================================================================
 * Vnode operations
 * ================================================================ */

/*
 * vmsfs_lookup (VOP_LOOKUP, v2 calling convention) - resolve one path
 * component in a directory to a vnode. The ODS-2 decision (version resolution +
 * case-blind name match) is the shared core's vmsfs_dir_resolve(); this glue
 * only handles "." / ".." and the vnode/lock plumbing.
 */
static int
vmsfs_lookup(void *v)
{
	struct vop_lookup_v2_args *ap = v;
	struct vnode *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct vnode **vpp = ap->a_vpp;
	struct vmsfs_mount *vmp = VFSTOVMSFS(dvp->v_mount);
	struct vmsfs_node *dnode = VTOVMSFS(dvp);
	struct vnode *vp;
	char name[VMSFS_MAX_FILENAME + 1];
	uint32_t fid = 0;
	int error;

	*vpp = NULL;

	if (dvp->v_type != VDIR)
		return ENOTDIR;

	/* A read-only mount refuses every mutating lookup up front. */
	if ((cnp->cn_flags & ISLASTCN) &&
	    (cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME ||
	     cnp->cn_nameiop == DELETE))
		return EROFS;

	/* "." resolves to the directory itself. */
	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		vref(dvp);
		*vpp = dvp;
		return 0;
	}

	/*
	 * VOP_LOOKUP returns the child REFERENCED but UNLOCKED. In NetBSD's modern
	 * lookup contract (kern/vfs_lookup.c: lookup_once, then the LOCKLEAF
	 * `vn_lock(foundobj)' AFTER the VOP), namei() locks the returned vnode
	 * itself, and it does NOT expect the parent to be unlocked here. A backend
	 * that vn_lock()s the child (or VOP_UNLOCK()s the parent) causes namei to
	 * re-lock the same v_lock -> "rw_vector_enter: locking against myself".
	 * Every stock fs (cd9660, tmpfs, ...) just sets *vpp = <vcache_get result>
	 * and returns; do the same for `..' and for a normal component.
	 */
	if (cnp->cn_flags & ISDOTDOT) {
		uint32_t pfid = dnode->vn_parent_fid;

		if (pfid == 0)
			pfid = VMSFS_FID_MFD;
		error = vcache_get(dvp->v_mount, &pfid, sizeof(pfid), &vp);
		if (error)
			return error;
		*vpp = vp;
		return 0;
	}

	if (cnp->cn_namelen > VMSFS_MAX_FILENAME)
		return ENAMETOOLONG;
	memcpy(name, cnp->cn_nameptr, cnp->cn_namelen);
	name[cnp->cn_namelen] = '\0';

	error = vmsfs_dir_resolve(&vmp->vm_vol, dnode->vn_map,
	    dnode->vn_map_count, vmp->vm_case_blind, name, &fid);
	if (error)
		return EINVAL;   /* unparseable VMS filename */
	if (fid == 0)
		return ENOENT;

	error = vcache_get(dvp->v_mount, &fid, sizeof(fid), &vp);
	if (error)
		return error;
	*vpp = vp;		/* referenced, UNLOCKED -- namei locks it */
	return 0;
}

static int
vmsfs_open(void *v)
{
	struct vop_open_args *ap = v;

	/* Read-only mount: refuse a write open. */
	if (ap->a_mode & FWRITE)
		return EROFS;
	return 0;
}

static int
vmsfs_close(void *v)
{
	(void)v;
	return 0;
}

/*
 * vmsfs_access - a read-only mount grants read/execute and refuses write.
 * (The SOGW protection mask is preserved in the node and reported through
 * getattr; enforcing the full SOGW model on NetBSD is a later slice -- the
 * read proof needs only the read grant and the honest write refusal.)
 */
static int
vmsfs_access(void *v)
{
	struct vop_access_args *ap = v;

	if (ap->a_accmode & (VWRITE))
		return EROFS;
	return 0;
}

static int
vmsfs_getattr(void *v)
{
	struct vop_getattr_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	struct vmsfs_node *vn = VTOVMSFS(vp);

	vattr_null(vap);
	vap->va_type      = vp->v_type;
	vap->va_mode      = (vp->v_type == VDIR) ? 0555 : 0444;
	vap->va_nlink     = (vn->vn_link_count != 0) ? vn->vn_link_count : 1;
	vap->va_uid       = vn->vn_uic_member;
	vap->va_gid       = vn->vn_uic_group;
	vap->va_fsid      = vn->vn_vmp->vm_dev;
	vap->va_fileid    = vn->vn_fid;
	vap->va_size      = vn->vn_size;
	vap->va_blocksize = VMSFS_BLOCK_SIZE;
	vap->va_atime.tv_sec  = (time_t)vn->vn_accessed;
	vap->va_atime.tv_nsec = 0;
	vap->va_mtime.tv_sec  = (time_t)vn->vn_modified;
	vap->va_mtime.tv_nsec = 0;
	vap->va_ctime.tv_sec  = (time_t)vn->vn_modified;
	vap->va_ctime.tv_nsec = 0;
	vap->va_birthtime.tv_sec  = (time_t)vn->vn_created;
	vap->va_birthtime.tv_nsec = 0;
	vap->va_gen       = vn->vn_version;
	vap->va_flags     = 0;
	vap->va_rdev      = NODEV;
	vap->va_bytes     = (u_quad_t)vn->vn_blocks * VMSFS_BLOCK_SIZE;
	vap->va_filerev   = vn->vn_version;
	return 0;
}

/*
 * vmsfs_read - read regular-file data. The VBN->LBN translation is the shared
 * core's vmsfs_vbn_to_lbn() over the node's cached retrieval map; this glue
 * bread()s each 512-byte volume block and uiomove()s the requested slice.
 */
static int
vmsfs_read(void *v)
{
	struct vop_read_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct vmsfs_node *vn = VTOVMSFS(vp);
	int error = 0;

	if (vp->v_type == VDIR)
		return EISDIR;
	if (vp->v_type != VREG)
		return EINVAL;
	if (uio->uio_offset < 0)
		return EINVAL;

	while (uio->uio_resid > 0 &&
	    (uint64_t)uio->uio_offset < vn->vn_size) {
		struct buf *bp;
		uint32_t vbn, lbn, blkoff;
		uint64_t remain;
		size_t n;

		vbn    = (uint32_t)(uio->uio_offset / VMSFS_BLOCK_SIZE) + 1;
		blkoff = (uint32_t)(uio->uio_offset % VMSFS_BLOCK_SIZE);

		error = vmsfs_vbn_to_lbn(vn->vn_map, vn->vn_map_count, vbn, &lbn);
		if (error)
			break;   /* unmapped -- treat as EOF */

		error = vmsfs_readblk(vn->vn_vmp->vm_devvp, lbn, &bp);
		if (error)
			break;

		remain = vn->vn_size - (uint64_t)uio->uio_offset;
		n = VMSFS_BLOCK_SIZE - blkoff;
		if ((uint64_t)n > remain)
			n = (size_t)remain;
		if (n > uio->uio_resid)
			n = uio->uio_resid;

		error = uiomove((char *)bp->b_data + blkoff, n, uio);
		brelse(bp, 0);
		if (error)
			break;
	}

	return error;
}

/*
 * vmsfs_readdir - emit "." / ".." then the directory's ODS-2 entries. The per-
 * entry ODS-2 decode (vmsfs_dir_entry_decode) and the VMS display-name format
 * (vmsfs_dir_format_name) are the shared core's; this glue owns the map-walk,
 * the struct dirent packing and the uio streaming. uio_offset is the byte
 * offset into the synthetic dirent stream (deterministic across calls), so a
 * short read resumes exactly where it stopped.
 */
static int
vmsfs_readdir(void *v)
{
	struct vop_readdir_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct vmsfs_node *dvn = VTOVMSFS(vp);
	struct vmsfs_mount *vmp = dvn->vn_vmp;
	struct dirent *dp;
	off_t streampos = 0;         /* byte offset of the current record */
	off_t startoff = uio->uio_offset;
	uint32_t vbn;
	int error = 0;
	int full = 0;

	if (vp->v_type != VDIR)
		return ENOTDIR;
	if (uio->uio_offset < 0)
		return EINVAL;

	dp = kmem_zalloc(sizeof(*dp), KM_SLEEP);

#define VMSFS_EMIT(fileno, namebuf, nlen, dtype)			\
	do {								\
		uint16_t reclen;					\
		memset(dp, 0, sizeof(*dp));				\
		dp->d_fileno = (fileno);				\
		dp->d_namlen = (uint16_t)(nlen);			\
		dp->d_type   = (dtype);					\
		memcpy(dp->d_name, (namebuf), (nlen));			\
		dp->d_name[(nlen)] = '\0';				\
		reclen = _DIRENT_RECLEN(dp, (nlen));			\
		dp->d_reclen = reclen;					\
		if (streampos >= startoff) {				\
			if (uio->uio_resid < reclen) { full = 1; goto out; } \
			error = uiomove(dp, reclen, uio);		\
			if (error) goto out;				\
		}							\
		streampos += reclen;					\
	} while (0)

	/* "." and ".." */
	VMSFS_EMIT(dvn->vn_fid, ".", 1, DT_DIR);
	VMSFS_EMIT(dvn->vn_parent_fid != 0 ? dvn->vn_parent_fid : VMSFS_FID_MFD,
	    "..", 2, DT_DIR);

	/* Walk the directory's data blocks. */
	for (vbn = 1; ; vbn++) {
		struct buf *bp;
		uint32_t lbn;
		unsigned int j;

		if (vmsfs_vbn_to_lbn(dvn->vn_map, dvn->vn_map_count, vbn, &lbn))
			break;
		if (vmsfs_readblk(vmp->vm_devvp, lbn, &bp))
			break;

		for (j = 0; j < VMSFS_DIR_PER_BLOCK; j++) {
			struct vmsfs_dirent_view view;
			char fullname[VMSFS_MAX_FILENAME + 1];
			int namelen, is_dir;

			if (!vmsfs_dir_entry_decode(bp->b_data, j, &view))
				continue;   /* free slot */

			namelen = vmsfs_dir_format_name(&view, fullname,
			    sizeof(fullname), &is_dir);
			if (namelen <= 0 || namelen >= (int)sizeof(fullname))
				continue;

			/* Cannot use goto out from inside the block loop with a
			 * held buf: release it first when the record won't fit. */
			{
				uint16_t reclen;

				memset(dp, 0, sizeof(*dp));
				dp->d_fileno = view.fid;
				dp->d_namlen = (uint16_t)namelen;
				dp->d_type   = is_dir ? DT_DIR : DT_REG;
				memcpy(dp->d_name, fullname, namelen);
				dp->d_name[namelen] = '\0';
				reclen = _DIRENT_RECLEN(dp, namelen);
				dp->d_reclen = reclen;

				if (streampos >= startoff) {
					if (uio->uio_resid < reclen) {
						brelse(bp, 0);
						full = 1;
						goto out;
					}
					error = uiomove(dp, reclen, uio);
					if (error) {
						brelse(bp, 0);
						goto out;
					}
				}
				streampos += reclen;
			}
		}

		brelse(bp, 0);
	}

out:
#undef VMSFS_EMIT
	kmem_free(dp, sizeof(*dp));
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = (error == 0 && !full);
	return error;
}

/*
 * vmsfs_bmap (VOP_BMAP) - translate a file logical block to its device block.
 *
 * This is the map seam the UVM/UBC read pager rides: genfs_getpages() faults a
 * page of the file by asking VOP_BMAP for the device block backing that file
 * offset, then hands the buf to VOP_STRATEGY. The translation itself is the
 * SHARED ODS-2 core's vmsfs_vbn_to_lbn() over the node's cached retrieval map --
 * the exact same VBN->LBN math vmsfs_read()/vmsfs_readdir()/vmsfs_lookup()
 * already use (INV-DRIFT: one implementation of the ODS-2 retrieval map, in
 * src/kernel-core/vmsfs/vmsfs_map.c; this VOP is pure NetBSD glue).
 *
 * @a_bn is a 0-based file block in fs-block (512-byte) units; an ODS-2 VBN is
 * 1-based, so VBN = a_bn + 1. fs_bshift == dev_bshift (both DEV_BSHIFT), so the
 * LBN is the device block unscaled. An unmapped block (hole / past the mapped
 * range) reports -1, which the pager treats as a zero-fill.
 */
static int
vmsfs_bmap(void *v)
{
	struct vop_bmap_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct vmsfs_node *vn = VTOVMSFS(vp);
	struct vmsfs_mount *vmp = vn->vn_vmp;
	uint32_t vbn, lbn;

	/* The device vnode the block lives on. */
	if (ap->a_vpp != NULL)
		*ap->a_vpp = vmp->vm_devvp;
	if (ap->a_bnp == NULL)
		return 0;

	vbn = (uint32_t)ap->a_bn + 1;   /* 0-based fs block -> 1-based ODS-2 VBN */

	if (vmsfs_vbn_to_lbn(vn->vn_map, vn->vn_map_count, vbn, &lbn) != 0) {
		/* Not mapped: signal a hole so the pager zero-fills the page. */
		*ap->a_bnp = (daddr_t)-1;
	} else {
		/* fs_bshift == dev_bshift == DEV_BSHIFT, so no scale is needed. */
		*ap->a_bnp = (daddr_t)lbn;
	}

	/*
	 * No readahead run advertised. Reporting 0 keeps the pager to a single
	 * block per I/O -- correct and simple; the retrieval map is not walked for
	 * a contiguous run here (a later optimization, not needed to page images).
	 */
	if (ap->a_runp != NULL)
		*ap->a_runp = 0;

	return 0;
}

/*
 * vmsfs_strategy (VOP_STRATEGY) - issue a file block I/O against the underlying
 * device. Called by the UVM/UBC pager (via genfs_getpages) with a buf whose
 * b_lblkno is a file logical block. Resolve it to a device block through
 * VOP_BMAP (if not already resolved), then redirect the buf to the block-device
 * vnode's strategy. Mirrors cd9660_strategy: the read-only ODS-2 analog of a
 * stock NetBSD fs's block funnel. No write path (read-only mount).
 */
static int
vmsfs_strategy(void *v)
{
	struct vop_strategy_args *ap = v;
	struct buf *bp = ap->a_bp;
	struct vnode *vp = ap->a_vp;
	struct vmsfs_node *vn = VTOVMSFS(vp);
	int error;

	if (vp->v_type == VBLK || vp->v_type == VCHR)
		panic("vmsfs_strategy: spec");

	/* Resolve the file block to a device block if the pager left it unmapped. */
	if (bp->b_blkno == bp->b_lblkno) {
		error = VOP_BMAP(vp, bp->b_lblkno, NULL, &bp->b_blkno, NULL);
		if (error) {
			bp->b_error = error;
			biodone(bp);
			return error;
		}
		if (bp->b_blkno == (daddr_t)-1)
			clrbuf(bp);   /* hole: zero-fill, no device read */
	}
	if (bp->b_blkno == (daddr_t)-1) {
		biodone(bp);
		return 0;
	}

	/* Funnel the I/O to the block-device vnode. */
	return VOP_STRATEGY(vn->vn_vmp->vm_devvp, bp);
}

static int
vmsfs_inactive(void *v)
{
	struct vop_inactive_v2_args *ap = v;

	/* Nothing to write back (read-only); keep the vnode cached. */
	if (ap->a_recycle != NULL)
		*ap->a_recycle = false;
	return 0;
}

static int
vmsfs_reclaim(void *v)
{
	struct vop_reclaim_v2_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct vmsfs_node *vn = VTOVMSFS(vp);

	/* Tear down the genfs pager hook (rangelock) armed in loadvnode. */
	genfs_node_destroy(vp);

	vp->v_data = NULL;
	if (vn != NULL)
		kmem_free(vn, sizeof(*vn));
	return 0;
}

static int
vmsfs_print(void *v)
{
	struct vop_print_args {
		const struct vnodeop_desc *a_desc;
		struct vnode *a_vp;
	} *ap = v;
	struct vmsfs_node *vn = VTOVMSFS(ap->a_vp);

	printf("\tvmsfs fid %u version %u size %ju\n",
	    vn ? vn->vn_fid : 0, vn ? vn->vn_version : 0,
	    vn ? (uintmax_t)vn->vn_size : 0);
	return 0;
}

/* ================================================================
 * Operation vectors
 * ================================================================ */

int (**vmsfs_vnodeop_p)(void *);

static const struct vnodeopv_entry_desc vmsfs_vnodeop_entries[] = {
	{ &vop_default_desc,	vn_default_error },
	/*
	 * REQUIRED. namei() computes cnp->cn_namelen for every path component by
	 * calling VOP_PARSEPATH on the directory vnode (kern/vfs_lookup.c). A fs
	 * that omits this op gets vop_default (vn_default_error), so cn_namelen is
	 * left stale/garbage while cn_nameptr is correct -- lookups then fail with
	 * ENOENT/ENAMETOOLONG/ENOTDIR. genfs_parsepath is the stock parser every
	 * NetBSD fs uses (cf. cd9660_vnops.c). Its absence was masked on LP64 but
	 * broke every vmsfs lookup on ILP32/vax.
	 */
	{ &vop_parsepath_desc,	genfs_parsepath },
	{ &vop_lookup_desc,	vmsfs_lookup },		/* v2 */
	{ &vop_open_desc,	vmsfs_open },
	{ &vop_close_desc,	vmsfs_close },
	{ &vop_access_desc,	vmsfs_access },
	{ &vop_accessx_desc,	genfs_accessx },
	{ &vop_getattr_desc,	vmsfs_getattr },
	{ &vop_setattr_desc,	genfs_eopnotsupp },
	{ &vop_read_desc,	vmsfs_read },
	{ &vop_write_desc,	genfs_eopnotsupp },
	{ &vop_fallocate_desc,	genfs_eopnotsupp },
	{ &vop_fdiscard_desc,	genfs_eopnotsupp },
	{ &vop_ioctl_desc,	genfs_enoioctl },
	{ &vop_fcntl_desc,	genfs_fcntl },
	{ &vop_poll_desc,	genfs_poll },
	{ &vop_kqfilter_desc,	genfs_kqfilter },
	{ &vop_revoke_desc,	genfs_revoke },
	/*
	 * Read-only file mapping. genfs_mmap grants a shared/read mapping; it is
	 * what lets a user mmap(2) a file on the volume. Not on the exec fault path
	 * (that goes straight through the uvn pager -> VOP_GETPAGES), but a real
	 * pageable read-only fs advertises it. Writable mappings are refused
	 * upstream (VOP_ACCESS returns EROFS for VWRITE).
	 */
	{ &vop_mmap_desc,	genfs_mmap },
	{ &vop_fsync_desc,	genfs_nullop },
	{ &vop_seek_desc,	genfs_seek },
	{ &vop_remove_desc,	genfs_eopnotsupp },
	{ &vop_link_desc,	genfs_eopnotsupp },
	{ &vop_rename_desc,	genfs_eopnotsupp },
	{ &vop_mkdir_desc,	genfs_eopnotsupp },
	{ &vop_rmdir_desc,	genfs_eopnotsupp },
	{ &vop_symlink_desc,	genfs_eopnotsupp },
	{ &vop_readdir_desc,	vmsfs_readdir },
	{ &vop_readlink_desc,	genfs_eopnotsupp },
	{ &vop_abortop_desc,	genfs_abortop },
	{ &vop_inactive_desc,	vmsfs_inactive },	/* v2 */
	{ &vop_reclaim_desc,	vmsfs_reclaim },	/* v2 */
	{ &vop_lock_desc,	genfs_lock },
	{ &vop_unlock_desc,	genfs_unlock },
	{ &vop_bmap_desc,	vmsfs_bmap },
	{ &vop_strategy_desc,	vmsfs_strategy },
	{ &vop_print_desc,	vmsfs_print },
	{ &vop_islocked_desc,	genfs_islocked },
	{ &vop_pathconf_desc,	genfs_eopnotsupp },
	{ &vop_advlock_desc,	genfs_eopnotsupp },
	{ &vop_bwrite_desc,	genfs_eopnotsupp },
	/*
	 * THE demand-paging seam (rd vms-63a). genfs_getpages is the stock UVM/UBC
	 * read pager: on a page fault against a file mapping (an mmap, or the exec
	 * image activator's paged-vnode vmcmd) it faults the page in by calling
	 * VOP_BMAP + VOP_STRATEGY above. Wiring it here (over the old
	 * genfs_eopnotsupp) is what lets an ELF32-vax image resident on the mounted
	 * ODS-2 volume DEMAND-PAGE and RUN, instead of exec failing at activation.
	 * putpages stays genfs_null_putpages: a read-only mount never has dirty
	 * pages, so page-out is a no-op cleanup.
	 */
	{ &vop_getpages_desc,	genfs_getpages },
	{ &vop_putpages_desc,	genfs_null_putpages },
	{ NULL, NULL }
};

const struct vnodeopv_desc vmsfs_vnodeop_opv_desc = {
	&vmsfs_vnodeop_p, vmsfs_vnodeop_entries
};

static const struct vnodeopv_desc * const vmsfs_vnodeopv_descs[] = {
	&vmsfs_vnodeop_opv_desc,
	NULL
};

struct vfsops vmsfs_vfsops = {
	.vfs_name		= MOUNT_VMSFS,
	.vfs_min_mount_data	= sizeof(struct vmsfs_args),
	.vfs_mount		= vmsfs_vfs_mount,
	.vfs_start		= vmsfs_vfs_start,
	.vfs_unmount		= vmsfs_vfs_unmount,
	.vfs_root		= vmsfs_vfs_root,
	.vfs_quotactl		= (void *)eopnotsupp,
	.vfs_statvfs		= vmsfs_vfs_statvfs,
	.vfs_sync		= vmsfs_vfs_sync,
	.vfs_vget		= (void *)eopnotsupp,
	.vfs_loadvnode		= vmsfs_vfs_loadvnode,
	.vfs_newvnode		= (void *)eopnotsupp,
	.vfs_fhtovp		= (void *)eopnotsupp,
	.vfs_vptofh		= (void *)eopnotsupp,
	.vfs_init		= vmsfs_vfs_init,
	.vfs_reinit		= NULL,
	.vfs_done		= vmsfs_vfs_done,
	.vfs_mountroot		= NULL,
	.vfs_snapshot		= (void *)eopnotsupp,
	.vfs_extattrctl		= vfs_stdextattrctl,
	.vfs_suspendctl		= genfs_suspendctl,
	.vfs_renamelock_enter	= genfs_renamelock_enter,
	.vfs_renamelock_exit	= genfs_renamelock_exit,
	.vfs_fsync		= (void *)eopnotsupp,
	.vfs_opv_descs		= vmsfs_vnodeopv_descs,
};

/* ================================================================
 * Module glue (module(9), MODULE_CLASS_VFS)
 * ================================================================ */

MODULE(MODULE_CLASS_VFS, vmsfs, NULL);

static int
vmsfs_modcmd(modcmd_t cmd, void *arg __unused)
{
	switch (cmd) {
	case MODULE_CMD_INIT:
		return vfs_attach(&vmsfs_vfsops);
	case MODULE_CMD_FINI:
		return vfs_detach(&vmsfs_vfsops);
	default:
		return ENOTTY;
	}
}
