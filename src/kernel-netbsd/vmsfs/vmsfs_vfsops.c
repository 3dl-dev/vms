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
 * READ-WRITE (rd vms-e7a). The mount honors whatever MNT_RDONLY the caller
 * passed instead of forcing it: a caller that asks for read-write gets the
 * real write VOPs (VOP_SETATTR/WRITE/CREATE/MKDIR/REMOVE) driving the SAME
 * shared allocator / directory-add / header-write core the Linux block-device
 * backend uses (src/kernel-core/vmsfs/vmsfs_alloc.c, vmsfs_dirscan.c,
 * vmsfs_header.c) -- this is what lets PROVISION.EXE stamp UIC file ownership
 * and STARTUP write SYSUAF logs / account-dir files onto the mounted system
 * volume, matching real VMS mounting its system disk read-write. A caller
 * that asks for MNT_RDONLY (the amd64/vax mount+read-only proofs still do)
 * gets an honestly read-only mount: no storage bitmap is loaded, and every
 * write VOP refuses with EROFS before ever reaching the allocator (Rule 9 --
 * no silent no-op, an explicit read-only mount stays exactly that).
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
static int vmsfs_setattr(void *);
static int vmsfs_read(void *);
static int vmsfs_write(void *);
static int vmsfs_create(void *);
static int vmsfs_mkdir(void *);
static int vmsfs_remove(void *);
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

	/*
	 * Toggling read-only <-> read-write via an update-remount is not
	 * supported (the storage bitmap is loaded only at INITIAL mount, below):
	 * request the desired mode at mount(2) time. An update that keeps or
	 * requests MNT_RDONLY is a harmless no-op; one that asks to ADD write
	 * (RDONLY not set) to an already-mounted volume is refused honestly.
	 */
	if (mp->mnt_flag & MNT_UPDATE) {
		if ((mp->mnt_flag & MNT_RDONLY) == 0)
			return EOPNOTSUPP;
		return 0;
	}

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
	 * The storage bitmap is loaded below, ONLY for a read-write mount (a
	 * read-only mount never drives the allocator, so vol.bitmap stays NULL --
	 * see the loop after this block).
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

	/*
	 * Read-write mount (rd vms-e7a): load the on-disk storage bitmap into
	 * memory so the shared core's allocator (vmsfs_alloc_block /
	 * vmsfs_alloc_fid / vmsfs_grow_map, src/kernel-core/vmsfs/vmsfs_alloc.c)
	 * has something to mutate -- exactly the bitmap the Linux blkdev backend
	 * keeps live in sbi->vol.bitmap. A caller that asked for MNT_RDONLY skips
	 * this entirely: no write VOP ever reaches the allocator on a read-only
	 * mount (each refuses first), so there is nothing to load.
	 */
	if ((mp->mnt_flag & MNT_RDONLY) == 0) {
		size_t bmsize = (size_t)vmp->vm_vol.bitmap_blocks * VMSFS_BLOCK_SIZE;
		uint32_t i;

		if (vmp->vm_vol.bitmap_blocks == 0 || bmsize == 0) {
			error = EINVAL;
			goto fail;
		}
		vmp->vm_vol.bitmap = kmem_zalloc(bmsize, KM_SLEEP);
		vmp->vm_bitmap_bytes = bmsize;

		for (i = 0; i < vmp->vm_vol.bitmap_blocks; i++) {
			struct buf *bbp = NULL;

			error = vmsfs_readblk(devvp, vmp->vm_vol.bitmap_lbn + i, &bbp);
			if (error)
				goto fail;
			memcpy((char *)vmp->vm_vol.bitmap + (size_t)i * VMSFS_BLOCK_SIZE,
			    bbp->b_data, VMSFS_BLOCK_SIZE);
			brelse(bbp, 0);
		}
	}

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
	if (vmp->vm_vol.bitmap != NULL)
		kmem_free(vmp->vm_vol.bitmap, vmp->vm_bitmap_bytes);
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

	if (vmp->vm_vol.bitmap != NULL)
		kmem_free(vmp->vm_vol.bitmap, vmp->vm_bitmap_bytes);

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

	/* RENAME is not implemented (VOP_RENAME is still genfs_eopnotsupp, rd
	 * vms-e7a scoped CREATE/REMOVE/MKDIR only) -- refuse it here too, honestly,
	 * before namei ever reaches the unimplemented VOP. CREATE and DELETE are
	 * handled below: CREATE (rd vms-e7a) has a real VOP_CREATE now, and
	 * DELETE needs the target resolved (VOP_REMOVE needs an existing vnode). */
	if ((cnp->cn_flags & ISLASTCN) && cnp->cn_nameiop == RENAME)
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

	/*
	 * CREATE intent is NEVER satisfied from an existing resolution (rd
	 * vms-e7a): on VMS, creating "FOO.TXT" when FOO.TXT;1 exists produces
	 * FOO.TXT;2 -- it never reopens ;1. EJUSTRETURN (a documented BSD VFS
	 * sentinel, <sys/errno.h>) with *vpp left NULL is the standard "this
	 * name does not exist for CREATE purposes; the caller's VOP_CREATE will
	 * make it" signal, so VOP_CREATE (below) always runs and always cuts a
	 * version via the shared core's vmsfs_dir_highest_version(), mirroring
	 * the Linux backend's identical "CREATE INTENT IS NEVER SATISFIED FROM
	 * CACHE" rule (src/kernel/vmsfs/vmsfs_inode.c, vmsfs_d_revalidate()).
	 */
	if ((cnp->cn_flags & ISLASTCN) && cnp->cn_nameiop == CREATE) {
		if (dvp->v_mount->mnt_flag & MNT_RDONLY)
			return EROFS;
		return EJUSTRETURN;
	}

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

	/* A read-only MOUNT refuses a write open; a read-write mount defers to
	 * VOP_ACCESS's SOGW check (already run by namei/vn_open before this). */
	if ((ap->a_mode & FWRITE) && (ap->a_vp->v_mount->mnt_flag & MNT_RDONLY))
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
 * vmsfs_check_access - the SOGW protection check (rd vms-e7a). Same category-
 * assignment rule and nibble semantics as the Linux backend's twin
 * (src/kernel/vmsfs/vmsfs_blkdev.c, vmsfs_blkdev_permission()) -- the shared
 * on-disk format's VMSFS_PROT_* bits/shifts and VMSFS_MAXSYSGROUP
 * (vmsfs_ondisk.h) are the SAME constants both backends check (INV-DRIFT);
 * only credential extraction (kauth(9) here, kuid/kgid there) is
 * substrate-specific.
 *
 * Category assignment (priority order):
 *   System: euid 0, or egid (the process's UIC group) <= VMSFS_MAXSYSGROUP.
 *   Owner:  euid matches the file's UIC member AND egid matches its UIC group.
 *   Group:  egid matches the file's UIC group.
 *   World:  no match.
 */
static int
vmsfs_check_access(struct vmsfs_node *vn, kauth_cred_t cred, accmode_t accmode)
{
	uint16_t prot = vn->vn_prot;
	uint8_t deny;
	uid_t euid = kauth_cred_geteuid(cred);
	gid_t egid = kauth_cred_getegid(cred);

	if (euid == 0 || egid <= VMSFS_MAXSYSGROUP)
		deny = (prot >> VMSFS_PROT_SYS_SHIFT) & 0xF;
	else if (euid == (uid_t)vn->vn_uic_member && egid == (gid_t)vn->vn_uic_group)
		deny = (prot >> VMSFS_PROT_OWN_SHIFT) & 0xF;
	else if (egid == (gid_t)vn->vn_uic_group)
		deny = (prot >> VMSFS_PROT_GRP_SHIFT) & 0xF;
	else
		deny = (prot >> VMSFS_PROT_WLD_SHIFT) & 0xF;

	if ((accmode & VREAD)  && (deny & VMSFS_PROT_R)) return EACCES;
	if ((accmode & VWRITE) && (deny & VMSFS_PROT_W)) return EACCES;
	if ((accmode & VEXEC)  && (deny & VMSFS_PROT_E)) return EACCES;
	return 0;
}

/*
 * vmsfs_access - a read-only MOUNT refuses write outright (Rule 9: an
 * explicit read-only mount stays exactly that, regardless of protection
 * bits); a read-write mount defers to the real SOGW check above.
 */
static int
vmsfs_access(void *v)
{
	struct vop_access_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct vmsfs_node *vn = VTOVMSFS(vp);

	if ((ap->a_accmode & VWRITE) && (vp->v_mount->mnt_flag & MNT_RDONLY))
		return EROFS;

	return vmsfs_check_access(vn, ap->a_cred, ap->a_accmode);
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
 * vmsfs_setattr (VOP_SETATTR) - the OWNER path (rd vms-e7a): uid/gid/mode.
 *
 * This is the specific gap the boot surfaced: PROVISION's provision_ownership()
 * calls lchown(2) on every file in the system tree to stamp UIC file ownership,
 * which reaches here with only va_uid/va_gid set (everything else VNOVAL).
 * chown/chmod are privileged (System category only -- root, or a UIC group <=
 * VMSFS_MAXSYSGROUP -- matching real VMS where only a suitably privileged
 * process may restamp another file's ownership/protection); mode translates
 * through the shared core's vmsfs_mode_to_prot(). Every OTHER attribute
 * (size/truncate, atime/mtime, flags) is refused honestly (EOPNOTSUPP) rather
 * than silently ignored -- Rule 9/INV-6, no fake success.
 */
static int
vmsfs_setattr(void *v)
{
	struct vop_setattr_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	struct vmsfs_node *vn = VTOVMSFS(vp);
	struct vmsfs_mount *vmp = vn->vn_vmp;
	kauth_cred_t cred = ap->a_cred;
	struct vmsfs_bh *bh;
	struct vmsfs_fh_owner ow;
	uint16_t new_prot, new_uic_group, new_uic_member;
	int change_owner, change_mode;
	uid_t euid;
	gid_t egid;
	uint32_t lbn;

	if (vap->va_size != (u_quad_t)VNOVAL || vap->va_flags != (u_long)VNOVAL ||
	    vap->va_atime.tv_sec != VNOVAL || vap->va_mtime.tv_sec != VNOVAL)
		return EOPNOTSUPP;

	change_owner = (vap->va_uid != (uid_t)VNOVAL) || (vap->va_gid != (gid_t)VNOVAL);
	change_mode  = (vap->va_mode != (mode_t)VNOVAL);
	if (!change_owner && !change_mode)
		return 0;

	if (vp->v_mount->mnt_flag & MNT_RDONLY)
		return EROFS;

	/* chown/chmod are privileged: only a System-category credential may
	 * restamp ownership/protection (the exact category PROVISION's SYSTEM
	 * identity runs under). */
	euid = kauth_cred_geteuid(cred);
	egid = kauth_cred_getegid(cred);
	if (euid != 0 && egid > VMSFS_MAXSYSGROUP)
		return EPERM;

	new_uic_member = (vap->va_uid != (uid_t)VNOVAL)
	    ? (uint16_t)vap->va_uid : vn->vn_uic_member;
	new_uic_group  = (vap->va_gid != (gid_t)VNOVAL)
	    ? (uint16_t)vap->va_gid : vn->vn_uic_group;
	new_prot = change_mode ? vmsfs_mode_to_prot((uint32_t)vap->va_mode) : vn->vn_prot;

	lbn = vmp->vm_vol.index_lbn + vn->vn_fid - 1;
	bh = vmsfs_bget(vmp->vm_devvp, lbn);
	if (bh == NULL)
		return EIO;

	ow.protection = new_prot;
	ow.uic_group  = new_uic_group;
	ow.uic_member = new_uic_member;
	vmsfs_fh_write_owner(vmsfs_bdata(bh), &ow);
	vmsfs_bdirty_sync(bh);
	vmsfs_bput(bh);

	vn->vn_prot       = new_prot;
	vn->vn_uic_group  = new_uic_group;
	vn->vn_uic_member = new_uic_member;

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
 * vmsfs_write (VOP_WRITE, rd vms-e7a) - write regular-file data, growing the
 * file's block map as needed via the shared core's vmsfs_grow_map() (the SAME
 * allocator vmsfs_mkdir/vmsfs_create below and the Linux blkdev backend's
 * vmsfs_get_block(create=1) drive). Direct block I/O through the device's
 * buffer cache (vmsfs_bget/vmsfs_bput on vmp->vm_devvp) -- the same path
 * vmsfs_read() already uses, not the UBC/genfs page cache the read-only
 * exec/mmap pager (VOP_GETPAGES, rd vms-63a) rides. This backend has no
 * writable mmap (VOP_ACCESS/VOP_OPEN gate VWRITE at open time, and the genfs
 * ops vector stays read-only-pager-shaped, genfs_null_putpages): every write
 * goes through here, so there is no write-back page cache to keep coherent.
 */
static int
vmsfs_write(void *v)
{
	struct vop_write_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct vmsfs_node *vn = VTOVMSFS(vp);
	struct vmsfs_mount *vmp = vn->vn_vmp;
	int ioflag = ap->a_ioflag;
	struct vmsfs_bh *hbh;
	struct vmsfs_fh_meta meta;
	uint32_t hlbn;
	int error = 0;

	if (vp->v_type == VDIR)
		return EISDIR;
	if (vp->v_type != VREG)
		return EINVAL;
	if (vp->v_mount->mnt_flag & MNT_RDONLY)
		return EROFS;
	if (uio->uio_offset < 0)
		return EINVAL;

	if (ioflag & IO_APPEND)
		uio->uio_offset = (off_t)vn->vn_size;

	while (uio->uio_resid > 0) {
		struct vmsfs_bh *bh;
		uint32_t vbn, lbn, blkoff;
		size_t n;
		int blocks_added = 0;

		vbn    = (uint32_t)(uio->uio_offset / VMSFS_BLOCK_SIZE) + 1;
		blkoff = (uint32_t)(uio->uio_offset % VMSFS_BLOCK_SIZE);

		mutex_enter(&vmp->vm_alloc_lock);
		error = vmsfs_grow_map(&vmp->vm_vol, vn->vn_map, &vn->vn_map_count,
		    vbn, &blocks_added);
		vn->vn_blocks += (uint32_t)blocks_added;
		if (!error)
			error = vmsfs_vbn_to_lbn(vn->vn_map, vn->vn_map_count, vbn, &lbn);
		mutex_exit(&vmp->vm_alloc_lock);
		if (error) {
			error = -error;
			break;
		}

		bh = vmsfs_bget(vmp->vm_devvp, lbn);
		if (bh == NULL) {
			error = EIO;
			break;
		}

		n = VMSFS_BLOCK_SIZE - blkoff;
		if (n > uio->uio_resid)
			n = uio->uio_resid;

		error = uiomove((char *)vmsfs_bdata(bh) + blkoff, n, uio);
		vmsfs_bdirty_sync(bh);
		vmsfs_bput(bh);
		if (error)
			break;

		if ((uint64_t)uio->uio_offset > vn->vn_size)
			vn->vn_size = (uint64_t)uio->uio_offset;
	}

	uvm_vnp_setsize(vp, (voff_t)vn->vn_size);

	/*
	 * Persist size/blocks/map -- even on a partial write (an honest partial
	 * success, never a silently-dropped tail) -- via the shared core's
	 * read-modify-write header writer, the SAME one create/mkdir/rename
	 * already use.
	 */
	hlbn = vmp->vm_vol.index_lbn + vn->vn_fid - 1;
	hbh = vmsfs_bget(vmp->vm_devvp, hlbn);
	if (hbh == NULL)
		return error ? error : EIO;

	meta.size       = vn->vn_size;
	meta.blocks     = vn->vn_blocks;
	meta.link_count = vn->vn_link_count ? vn->vn_link_count : 1;
	meta.protection = vn->vn_prot;
	meta.map_count  = vn->vn_map_count;
	meta.map        = vn->vn_map;
	vmsfs_fh_write_meta(vmsfs_bdata(hbh), &meta);
	vmsfs_bdirty_sync(hbh);
	vmsfs_bput(hbh);

	return error;
}

/*
 * vmsfs_create (VOP_CREATE, rd vms-e7a) - create a new file with VMS
 * auto-versioning. Mirrors the Linux backend's vmsfs_blkdev_create() exactly
 * (same shared-core calls, same version-resolution rule): parse an explicit
 * ";N" version out of the name or bump the highest existing version, allocate
 * a FID, encode a fresh header, add the directory entry, and update the
 * volume free-count. The new file's UIC starts at [0,0] -- the OWNER path
 * (vmsfs_setattr, above) is what the caller uses to stamp real ownership,
 * matching Unix create-then-chown semantics.
 *
 * Per the documented VOP_CREATE contract (vnode_if.src: "% create vpp - U -"),
 * dvp stays exactly as locked as the caller left it (never touched here) and
 * the new vp is returned referenced and UNLOCKED -- the same "namei's caller
 * locks it" contract vmsfs_lookup() already follows.
 */
static int
vmsfs_create(void *v)
{
	struct vop_create_v3_args *ap = v;
	struct vnode *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct vmsfs_mount *vmp = VFSTOVMSFS(dvp->v_mount);
	struct vmsfs_node *dnode = VTOVMSFS(dvp);
	struct vnode *vp;
	char name[VMSFS_MAX_FILENAME + 1];
	char base[VMSFS_MAX_FILENAME + 1];
	char fname[VMSFS_MAX_NAME + 1];
	char ftype[VMSFS_MAX_TYPE + 1];
	uint32_t fid, lbn;
	uint16_t highest, new_version;
	int req_version;
	struct vmsfs_bh *bh;
	struct vmsfs_fh_info fhi;
	uint64_t now;
	int blocks_added;
	int error;

	if (dvp->v_mount->mnt_flag & MNT_RDONLY)
		return EROFS;
	if (cnp->cn_namelen > VMSFS_MAX_FILENAME)
		return ENAMETOOLONG;
	memcpy(name, cnp->cn_nameptr, cnp->cn_namelen);
	name[cnp->cn_namelen] = '\0';

	if (vmsfs_parse_version(name, base, sizeof(base), &req_version))
		return EINVAL;
	vmsfs_strupper(base);
	vmsfs_split_name_type(base, fname, sizeof(fname), ftype, sizeof(ftype));

	mutex_enter(&vmp->vm_alloc_lock);

	if (req_version > 0) {
		new_version = (uint16_t)req_version;
	} else {
		highest = vmsfs_dir_highest_version(&vmp->vm_vol, dnode->vn_map,
		    dnode->vn_map_count, vmp->vm_case_blind, base, strlen(base));
		new_version = (uint16_t)(highest + 1);
		if (new_version > VMSFS_VERSION_MAX) {
			mutex_exit(&vmp->vm_alloc_lock);
			return ENOSPC;
		}
	}

	error = vmsfs_alloc_fid(&vmp->vm_vol, &fid);
	if (error) {
		mutex_exit(&vmp->vm_alloc_lock);
		return -error;
	}

	lbn = vmp->vm_vol.index_lbn + fid - 1;
	bh = vmsfs_bget(vmp->vm_devvp, lbn);
	if (bh == NULL) {
		vmsfs_free_fid(&vmp->vm_vol, fid);
		mutex_exit(&vmp->vm_alloc_lock);
		return EIO;
	}

	now = vmsfs_now_seconds();
	memset(&fhi, 0, sizeof(fhi));
	fhi.fid        = fid;
	fhi.created    = now;
	fhi.modified   = now;
	fhi.accessed   = now;
	fhi.parent_fid = dnode->vn_fid;
	fhi.flags      = VMSFS_FH_INUSE;
	fhi.version    = new_version;
	fhi.protection = VMSFS_PROT_DEFAULT;
	fhi.link_count = 1;
	strscpy(fhi.name, fname, sizeof(fhi.name));
	strscpy(fhi.type, ftype, sizeof(fhi.type));
	vmsfs_fh_encode(&fhi, vmsfs_bdata(bh));
	vmsfs_bdirty_sync(bh);
	vmsfs_bput(bh);

	blocks_added = 0;
	error = vmsfs_dir_add_entry(&vmp->vm_vol, dnode->vn_map, &dnode->vn_map_count,
	    fid, base, (uint8_t)strlen(base), new_version, &blocks_added);
	if (error) {
		vmsfs_free_fid(&vmp->vm_vol, fid);
		mutex_exit(&vmp->vm_alloc_lock);
		return -error;
	}
	if (blocks_added) {
		struct vmsfs_bh *dbh;
		struct vmsfs_fh_meta meta;
		uint32_t dlbn = vmp->vm_vol.index_lbn + dnode->vn_fid - 1;

		dnode->vn_blocks += (uint32_t)blocks_added;
		dnode->vn_size += (uint64_t)blocks_added * VMSFS_BLOCK_SIZE;
		dbh = vmsfs_bget(vmp->vm_devvp, dlbn);
		if (dbh != NULL) {
			meta.size       = dnode->vn_size;
			meta.blocks     = dnode->vn_blocks;
			meta.link_count = dnode->vn_link_count ? dnode->vn_link_count : 2;
			meta.protection = dnode->vn_prot;
			meta.map_count  = dnode->vn_map_count;
			meta.map        = dnode->vn_map;
			vmsfs_fh_write_meta(vmsfs_bdata(dbh), &meta);
			vmsfs_bdirty_sync(dbh);
			vmsfs_bput(dbh);
		}
	}

	vmsfs_update_home_block(&vmp->vm_vol);
	mutex_exit(&vmp->vm_alloc_lock);

	error = vcache_get(dvp->v_mount, &fid, sizeof(fid), &vp);
	if (error)
		return error;
	*ap->a_vpp = vp;
	return 0;
}

/*
 * vmsfs_mkdir (VOP_MKDIR, rd vms-e7a) - create a directory: an unversioned
 * file header (VMSFS_FH_DIRECTORY) with one zeroed data block. Mirrors the
 * Linux backend's vmsfs_blkdev_mkdir(). Same "vp returned unlocked" contract
 * as vmsfs_create() above.
 */
static int
vmsfs_mkdir(void *v)
{
	struct vop_mkdir_v3_args *ap = v;
	struct vnode *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct vmsfs_mount *vmp = VFSTOVMSFS(dvp->v_mount);
	struct vmsfs_node *dnode = VTOVMSFS(dvp);
	struct vnode *vp;
	char uname[VMSFS_MAX_FILENAME + 1];
	char fname[VMSFS_MAX_NAME + 1];
	char ftype[VMSFS_MAX_TYPE + 1];
	uint32_t fid, data_lbn, lbn;
	struct vmsfs_bh *bh;
	struct vmsfs_fh_info fhi;
	uint64_t now;
	size_t nlen;
	int blocks_added;
	int error;

	if (dvp->v_mount->mnt_flag & MNT_RDONLY)
		return EROFS;
	if (cnp->cn_namelen > VMSFS_MAX_FILENAME)
		return ENAMETOOLONG;
	memcpy(uname, cnp->cn_nameptr, cnp->cn_namelen);
	uname[cnp->cn_namelen] = '\0';
	vmsfs_strupper(uname);

	strscpy(fname, uname, sizeof(fname));
	nlen = strlen(fname);
	if (nlen > 4 && strncasecmp(fname + nlen - 4, ".DIR", 4) == 0)
		fname[nlen - 4] = '\0';
	strscpy(ftype, "DIR", sizeof(ftype));

	mutex_enter(&vmp->vm_alloc_lock);

	error = vmsfs_alloc_fid(&vmp->vm_vol, &fid);
	if (error) {
		mutex_exit(&vmp->vm_alloc_lock);
		return -error;
	}

	error = vmsfs_alloc_block(&vmp->vm_vol, &data_lbn);
	if (error) {
		vmsfs_free_fid(&vmp->vm_vol, fid);
		mutex_exit(&vmp->vm_alloc_lock);
		return -error;
	}

	bh = vmsfs_bget(vmp->vm_devvp, data_lbn);
	if (bh == NULL) {
		vmsfs_free_block(&vmp->vm_vol, data_lbn);
		vmsfs_free_fid(&vmp->vm_vol, fid);
		mutex_exit(&vmp->vm_alloc_lock);
		return EIO;
	}
	memset(vmsfs_bdata(bh), 0, VMSFS_BLOCK_SIZE);
	vmsfs_bdirty_sync(bh);
	vmsfs_bput(bh);

	lbn = vmp->vm_vol.index_lbn + fid - 1;
	bh = vmsfs_bget(vmp->vm_devvp, lbn);
	if (bh == NULL) {
		vmsfs_free_block(&vmp->vm_vol, data_lbn);
		vmsfs_free_fid(&vmp->vm_vol, fid);
		mutex_exit(&vmp->vm_alloc_lock);
		return EIO;
	}

	now = vmsfs_now_seconds();
	memset(&fhi, 0, sizeof(fhi));
	fhi.fid        = fid;
	fhi.size       = VMSFS_BLOCK_SIZE;
	fhi.created    = now;
	fhi.modified   = now;
	fhi.accessed   = now;
	fhi.blocks     = 1;
	fhi.parent_fid = dnode->vn_fid;
	fhi.flags      = VMSFS_FH_INUSE | VMSFS_FH_DIRECTORY;
	fhi.version    = 0;
	fhi.protection = VMSFS_PROT_DEFAULT;
	fhi.link_count = 2;
	fhi.map_count  = 1;
	strscpy(fhi.name, fname, sizeof(fhi.name));
	strscpy(fhi.type, ftype, sizeof(fhi.type));
	fhi.map[0].rp_lbn   = data_lbn;
	fhi.map[0].rp_count = 1;
	vmsfs_fh_encode(&fhi, vmsfs_bdata(bh));
	vmsfs_bdirty_sync(bh);
	vmsfs_bput(bh);

	blocks_added = 0;
	error = vmsfs_dir_add_entry(&vmp->vm_vol, dnode->vn_map, &dnode->vn_map_count,
	    fid, uname, (uint8_t)strlen(uname), 0, &blocks_added);
	if (error) {
		vmsfs_free_block(&vmp->vm_vol, data_lbn);
		vmsfs_free_fid(&vmp->vm_vol, fid);
		mutex_exit(&vmp->vm_alloc_lock);
		return -error;
	}

	dnode->vn_link_count = (uint16_t)((dnode->vn_link_count ? dnode->vn_link_count : 1) + 1);
	if (blocks_added) {
		dnode->vn_blocks += (uint32_t)blocks_added;
		dnode->vn_size += (uint64_t)blocks_added * VMSFS_BLOCK_SIZE;
	}
	{
		struct vmsfs_bh *dbh;
		struct vmsfs_fh_meta meta;
		uint32_t dlbn = vmp->vm_vol.index_lbn + dnode->vn_fid - 1;

		dbh = vmsfs_bget(vmp->vm_devvp, dlbn);
		if (dbh != NULL) {
			meta.size       = dnode->vn_size;
			meta.blocks     = dnode->vn_blocks;
			meta.link_count = dnode->vn_link_count;
			meta.protection = dnode->vn_prot;
			meta.map_count  = dnode->vn_map_count;
			meta.map        = dnode->vn_map;
			vmsfs_fh_write_meta(vmsfs_bdata(dbh), &meta);
			vmsfs_bdirty_sync(dbh);
			vmsfs_bput(dbh);
		}
	}

	vmsfs_update_home_block(&vmp->vm_vol);
	mutex_exit(&vmp->vm_alloc_lock);

	error = vcache_get(dvp->v_mount, &fid, sizeof(fid), &vp);
	if (error)
		return error;
	*ap->a_vpp = vp;
	return 0;
}

/*
 * vmsfs_remove (VOP_REMOVE, rd vms-e7a) - delete a file: remove its directory
 * entry, free its data blocks + FID, update the volume free-count. Mirrors
 * the Linux backend's vmsfs_blkdev_unlink(). Per the documented VOP_REMOVE
 * contract (vnode_if.src: "% remove vp L U U"), THIS op is responsible for
 * releasing @a_vp exactly once on every exit path (WILLPUT) -- dvp is left
 * exactly as the caller locked it, untouched here.
 */
static int
vmsfs_remove(void *v)
{
	struct vop_remove_v3_args *ap = v;
	struct vnode *dvp = ap->a_dvp;
	struct vnode *vp = ap->a_vp;
	struct vmsfs_mount *vmp = VFSTOVMSFS(dvp->v_mount);
	struct vmsfs_node *dnode = VTOVMSFS(dvp);
	struct vmsfs_node *vn = VTOVMSFS(vp);
	int error;

	if (dvp->v_mount->mnt_flag & MNT_RDONLY) {
		error = EROFS;
		goto out;
	}
	if (vp->v_type == VDIR) {
		error = EPERM;
		goto out;
	}

	mutex_enter(&vmp->vm_alloc_lock);
	error = vmsfs_dir_remove_entry(&vmp->vm_vol, dnode->vn_map,
	    dnode->vn_map_count, vn->vn_fid, vn->vn_version);
	if (!error) {
		vmsfs_free_file_blocks(&vmp->vm_vol, vn->vn_map, vn->vn_map_count);
		vmsfs_free_fid(&vmp->vm_vol, vn->vn_fid);
		vmsfs_update_home_block(&vmp->vm_vol);
	}
	mutex_exit(&vmp->vm_alloc_lock);

	if (error) {
		error = -error;
	} else {
		vn->vn_flags &= (uint16_t)~VMSFS_FH_INUSE;
		vn->vn_link_count = 0;
	}

out:
	vput(vp);
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
	{ &vop_create_desc,	vmsfs_create },		/* v3, rd vms-e7a */
	{ &vop_open_desc,	vmsfs_open },
	{ &vop_close_desc,	vmsfs_close },
	{ &vop_access_desc,	vmsfs_access },
	{ &vop_accessx_desc,	genfs_accessx },
	{ &vop_getattr_desc,	vmsfs_getattr },
	{ &vop_setattr_desc,	vmsfs_setattr },
	{ &vop_read_desc,	vmsfs_read },
	{ &vop_write_desc,	vmsfs_write },
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
	{ &vop_remove_desc,	vmsfs_remove },
	{ &vop_link_desc,	genfs_eopnotsupp },
	{ &vop_rename_desc,	genfs_eopnotsupp },
	{ &vop_mkdir_desc,	vmsfs_mkdir },
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
