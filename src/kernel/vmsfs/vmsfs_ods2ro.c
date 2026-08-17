// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_ods2ro.c - a READ-ONLY Files-11 (ODS-2) presentation driven by the
 * GENUINE ODS-2 codec running KERNEL-RESIDENT (rd vms-dcd, epic vms-208).
 *
 * WHAT THIS IS. The foundation rung of the Files-11 ODS-2 ACP: it proves the
 * genuine on-disk ODS-2 codec (src/vmsfs/ods2/, compiled into vmsfs.ko with
 * -DOVMX_ODS2_KERNEL) reads a real Files-11 volume off a real struct
 * block_device, INSIDE the module, with NO userspace and NO POSIX. Every ODS-2
 * decision -- home-block validation, INDEXF.SYS header arithmetic, FM2
 * retrieval-pointer decode, directory-record parsing, VBN->LBN mapping -- is the
 * codec's, and every block comes through the shared FS engine's vmsfs_bio.h
 * backend (sb_bread) via the ods2_block.h seam. It mounts a genuine
 * "DECFILE11B" volume (e.g. the real-VAX fixture tests/ods2/real_vax_ods2.dsk),
 * lists its directories, and reads file content back byte-identically.
 *
 * WHAT THIS IS NOT. This is NOT the authoritative VMS file path. The ratified
 * architecture (docs/design-files11-acp-executive.md) reaches Files-11 through
 * $QIO/FIB ACP ops in vms.ko in caller context -- NOT through a Linux VFS mount
 * (that record's rejected "option D"). This read-only VFS is exactly the
 * "secondary, non-authoritative presentation for Linux host tooling" §4.1
 * explicitly permits, used here as the honest in-kernel proof vehicle for the
 * codec-runs-kernel-resident rung. It is read-only on purpose: the write path
 * (IO$_WRITEVBLK) belongs to a later ACP rung, not to a VFS shim.
 *
 * Clean-room (CLAUDE.md Rule 8): VFS glue over public Linux APIs; all ODS-2
 * knowledge lives in the codec, whose own provenance citations are unchanged.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>       /* bdev_nr_bytes() -- device geometry */
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/uio.h>
#include <linux/statfs.h>

#include "vmsfs.h"             /* vmsfs_ods2ro_register/_unregister prototypes */
#include "vmsfs/ods2.h"        /* the genuine ODS-2 codec (kernel-resident) */

#define ODS2RO_MAGIC 0x4f443252u   /* "OD2R" */

struct ods2ro_sb_info {
	ods2_bdev_t bv;            /* codec block-backed reader, host = super_block */
};

/* Per-inode: the ODS-2 FID it stands for. Stored directly in i_private. */
static inline uint32_t ods2ro_ino_fid(struct inode *inode)
{
	return (uint32_t)(uintptr_t)inode->i_private;
}

/* Forward decls */
static struct inode *ods2ro_iget(struct super_block *sb, uint32_t fid);
static const struct inode_operations ods2ro_dir_iops;
static const struct file_operations  ods2ro_dir_fops;
static const struct inode_operations ods2ro_file_iops;
static const struct file_operations  ods2ro_file_fops;

/* ================================================================
 * inode load: read the file header via the codec, populate the inode
 * ================================================================ */

static struct inode *ods2ro_iget(struct super_block *sb, uint32_t fid)
{
	struct ods2ro_sb_info *si = sb->s_fs_info;
	struct inode *inode;
	uint8_t hdr[ODS2_BLOCK_SIZE];
	const ods2_recattr_t *ra;
	uint32_t filechar;
	ods2_status_t st;
	int is_dir;

	inode = iget_locked(sb, fid);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;   /* already cached */

	/* ods2_bdev_read_header() already validates the header's additive checksum
	 * (it parses internally), so read the two fields we need straight from the
	 * validated raw block -- avoids a second full 512-byte ods2_fh2_t on the
	 * stack. fh2_filechar is a LE longword at offset 52; the record-attributes
	 * area (FAT) is the packed ods2_recattr_t at offset 20. */
	st = ods2_bdev_read_header(&si->bv, fid, hdr, sizeof(hdr));
	if (st != ODS2_OK)
		goto bad;

	filechar = (uint32_t)hdr[52] | ((uint32_t)hdr[53] << 8) |
		   ((uint32_t)hdr[54] << 16) | ((uint32_t)hdr[55] << 24);
	ra = (const ods2_recattr_t *)(hdr + 20);
	is_dir = (filechar & ODS2_FH2_M_DIRECTORY) != 0;

	inode->i_mode = is_dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
	i_uid_write(inode, 0);
	i_gid_write(inode, 0);
	set_nlink(inode, is_dir ? 2 : 1);
	inode_set_atime(inode, 0, 0);
	inode_set_mtime(inode, 0, 0);
	inode_set_ctime(inode, 0, 0);

	inode->i_size = (loff_t)ods2_recattr_data_bytes(ra);
	if (is_dir) {
		inode->i_op  = &ods2ro_dir_iops;
		inode->i_fop = &ods2ro_dir_fops;
	} else {
		inode->i_op  = &ods2ro_file_iops;
		inode->i_fop = &ods2ro_file_fops;
	}
	inode->i_private = (void *)(uintptr_t)fid;

	unlock_new_inode(inode);
	return inode;

bad:
	iget_failed(inode);
	return ERR_PTR(st == ODS2_ERR_NOTFOUND ? -ENOENT : -EIO);
}

/* ================================================================
 * directory: lookup + iterate, both via the codec
 * ================================================================ */

/*
 * Parse a Linux dentry name into a codec name (NAME.TYPE, no ";ver") plus an
 * explicit version (0 == highest). "HELLO.TXT;3" -> "HELLO.TXT", 3.
 */
static int ods2ro_split_name(const char *in, size_t in_len,
			     char *out, size_t out_sz, uint16_t *ver_out)
{
	size_t i;
	uint16_t ver = 0;
	size_t namelen = in_len;

	for (i = 0; i < in_len; i++) {
		if (in[i] == ';') {
			size_t j;
			namelen = i;
			for (j = i + 1; j < in_len; j++) {
				if (in[j] < '0' || in[j] > '9')
					return -EINVAL;
				ver = (uint16_t)(ver * 10 + (in[j] - '0'));
			}
			break;
		}
	}
	if (namelen == 0 || namelen >= out_sz)
		return -EINVAL;
	memcpy(out, in, namelen);
	out[namelen] = '\0';
	*ver_out = ver;
	return 0;
}

static struct dentry *ods2ro_lookup(struct inode *dir, struct dentry *dentry,
				    unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct ods2ro_sb_info *si = sb->s_fs_info;
	uint8_t dirhdr[ODS2_BLOCK_SIZE];
	char name[96];   /* ODS-2 full name (NAME.TYPE) is <= ~80 chars */
	uint16_t want_ver = 0;
	ods2_fid_t fid;
	ods2_status_t st;
	struct inode *inode = NULL;
	int rc;

	if (dentry->d_name.len >= sizeof(name))
		return ERR_PTR(-ENAMETOOLONG);

	rc = ods2ro_split_name(dentry->d_name.name, dentry->d_name.len,
			       name, sizeof(name), &want_ver);
	if (rc)
		return ERR_PTR(rc);

	st = ods2_bdev_read_header(&si->bv, ods2ro_ino_fid(dir),
				   dirhdr, sizeof(dirhdr));
	if (st != ODS2_OK)
		return ERR_PTR(-EIO);

	st = ods2_bdev_dir_find(&si->bv, dirhdr, name, want_ver, &fid, NULL);
	if (st == ODS2_OK) {
		inode = ods2ro_iget(sb, ods2_fid_number(&fid));
		if (IS_ERR(inode))
			return ERR_CAST(inode);
	} else if (st != ODS2_ERR_NOTFOUND) {
		return ERR_PTR(-EIO);
	}
	/* NOTFOUND -> negative dentry (inode == NULL) */
	return d_splice_alias(inode, dentry);
}

/* iterate: collect entries via the codec, then dir_emit by position. */
struct ods2ro_iter_cap {
	struct dir_context *ctx;
	int emitted;      /* how many real entries emitted so far this call */
	int index;        /* running index over the codec's entries */
	int start;        /* first real-entry index to emit (ctx->pos - 2) */
	int error;
};

static int ods2ro_iter_cb(const char *name, unsigned name_len,
			  uint16_t version, const ods2_fid_t *fid, void *ctx)
{
	struct ods2ro_iter_cap *c = ctx;
	char disp[96];
	int len;

	if (c->index < c->start) {
		c->index++;
		return 0;   /* already emitted in a previous getdents call */
	}
	if (name_len + 8 >= sizeof(disp)) {
		c->index++;
		return 0;   /* pathologically long; skip rather than overflow */
	}
	/* Present "NAME.TYPE;VERSION", the same shape vmsfs's block-dev mode uses. */
	memcpy(disp, name, name_len);
	len = snprintf(disp + name_len, sizeof(disp) - name_len, ";%u",
		       (unsigned)version);
	len += (int)name_len;

	if (!dir_emit(c->ctx, disp, len, ods2_fid_number(fid), DT_UNKNOWN)) {
		c->error = 1;
		return 1;   /* buffer full: stop, resume next call */
	}
	c->ctx->pos++;
	c->emitted++;
	c->index++;
	return 0;
}

static int ods2ro_iterate(struct file *file, struct dir_context *ctx)
{
	struct inode *dir = file_inode(file);
	struct super_block *sb = dir->i_sb;
	struct ods2ro_sb_info *si = sb->s_fs_info;
	uint8_t dirhdr[ODS2_BLOCK_SIZE];
	struct ods2ro_iter_cap cap;
	ods2_status_t st;

	if (!dir_emit_dots(file, ctx))
		return 0;

	st = ods2_bdev_read_header(&si->bv, ods2ro_ino_fid(dir),
				   dirhdr, sizeof(dirhdr));
	if (st != ODS2_OK)
		return -EIO;

	cap.ctx     = ctx;
	cap.emitted = 0;
	cap.index   = 0;
	cap.start   = (int)ctx->pos - 2;   /* "." and ".." already accounted */
	cap.error   = 0;

	st = ods2_bdev_list_dir(&si->bv, dirhdr, ods2ro_iter_cb, &cap);
	if (st != ODS2_OK && !cap.error)
		return -EIO;
	return 0;
}

/* ================================================================
 * file read: pull the whole file's content via the codec, copy the slice
 * ================================================================ */

static ssize_t ods2ro_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct super_block *sb = inode->i_sb;
	struct ods2ro_sb_info *si = sb->s_fs_info;
	uint8_t hdr[ODS2_BLOCK_SIZE];
	loff_t pos = iocb->ki_pos;
	size_t want = iov_iter_count(to);
	size_t datalen;
	uint8_t *buf;
	size_t copied;
	ods2_status_t st;

	if (pos < 0)
		return -EINVAL;
	if (pos >= inode->i_size || want == 0)
		return 0;

	st = ods2_bdev_read_header(&si->bv, ods2ro_ino_fid(inode),
				   hdr, sizeof(hdr));
	if (st != ODS2_OK)
		return -EIO;

	datalen = (size_t)inode->i_size;
	buf = kvmalloc(datalen ? datalen : 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	st = ods2_bdev_read_file(&si->bv, hdr, buf, datalen, NULL);
	if (st != ODS2_OK) {
		kvfree(buf);
		return -EIO;
	}

	if ((size_t)pos + want > datalen)
		want = datalen - (size_t)pos;

	copied = copy_to_iter(buf + pos, want, to);
	kvfree(buf);
	if (copied == 0)
		return -EFAULT;

	iocb->ki_pos = pos + copied;
	return (ssize_t)copied;
}

/* ================================================================
 * super / mount
 * ================================================================ */

static int ods2ro_statfs(struct dentry *d, struct kstatfs *buf)
{
	struct super_block *sb = d->d_sb;
	struct ods2ro_sb_info *si = sb->s_fs_info;

	buf->f_type    = ODS2RO_MAGIC;
	buf->f_bsize   = ODS2_BLOCK_SIZE;
	buf->f_blocks  = si->bv.nblocks;
	buf->f_bfree   = 0;
	buf->f_bavail  = 0;
	buf->f_files   = si->bv.home.hm2_maxfiles;
	buf->f_ffree   = 0;
	buf->f_namelen = 39 + 1 + 39 + 1 + 5;   /* NAME.TYPE;VERSION */
	return 0;
}

static const struct inode_operations ods2ro_dir_iops = {
	.lookup = ods2ro_lookup,
};

static const struct file_operations ods2ro_dir_fops = {
	.read           = generic_read_dir,
	.iterate_shared = ods2ro_iterate,
	.llseek         = generic_file_llseek,
};

static const struct inode_operations ods2ro_file_iops = {
};

static const struct file_operations ods2ro_file_fops = {
	.read_iter = ods2ro_read_iter,
	.llseek    = generic_file_llseek,
};

static void ods2ro_put_super(struct super_block *sb)
{
	struct ods2ro_sb_info *si = sb->s_fs_info;

	if (si) {
		sb->s_fs_info = NULL;
		kfree(si);
	}
}

static const struct super_operations ods2ro_sops = {
	.statfs    = ods2ro_statfs,
	.put_super = ods2ro_put_super,
	.evict_inode = NULL,
};

static int ods2ro_fill_super(struct super_block *sb, void *data, int silent)
{
	struct ods2ro_sb_info *si;
	struct inode *root;
	uint32_t nblocks;
	ods2_status_t st;
	int ret;

	si = kzalloc(sizeof(*si), GFP_KERNEL);
	if (!si)
		return -ENOMEM;
	sb->s_fs_info = si;

	if (!sb_set_blocksize(sb, ODS2_BLOCK_SIZE)) {
		if (!silent)
			pr_err("ods2ro: cannot set block size %d\n", ODS2_BLOCK_SIZE);
		ret = -EINVAL;
		goto err;
	}

	/* Volume size in 512-byte blocks, from the backing device geometry.
	 * bdev_nr_bytes() replaces i_size_read(bdev->bd_inode): 'bd_inode' was
	 * removed from struct block_device in the 6.x block layer (the bdev inode
	 * moved out of the public struct), and bdev_nr_bytes() is the supported
	 * device-size accessor (linux/blkdev.h, since 5.16) -- same byte count. */
	nblocks = (uint32_t)(bdev_nr_bytes(sb->s_bdev) / ODS2_BLOCK_SIZE);

	/*
	 * Bind the genuine ODS-2 codec to this mounted block device. host == sb,
	 * so every codec block read runs vmsfs_bget(sb, lbn) == sb_bread(sb, lbn).
	 * This validates the home block (BOTH additive checksums + "DECFILE11B  "
	 * + strict structure level) IN-KERNEL; a non-ODS-2 volume fails honestly
	 * with EINVAL rather than being accepted (Rule 9 / INV-6, fail-honest).
	 */
	st = ods2_bdev_open_host(&si->bv, sb, nblocks);
	if (st != ODS2_OK) {
		if (!silent)
			pr_err("ods2ro: not a genuine ODS-2 volume (codec status %d)\n",
			       (int)st);
		ret = -EINVAL;
		goto err;
	}

	sb->s_magic     = ODS2RO_MAGIC;
	sb->s_op        = &ods2ro_sops;
	sb->s_maxbytes  = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1;
	sb->s_flags    |= SB_RDONLY;

	root = ods2ro_iget(sb, ODS2_FID_MFD);   /* [000000], FID 4 */
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto err;
	}
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto err;
	}

	pr_info("ods2ro: mounted genuine ODS-2 volume '%.12s', %u blocks, %u max files "
		"(codec kernel-resident, rd vms-dcd)\n",
		si->bv.home.hm2_volname, si->bv.nblocks, si->bv.home.hm2_maxfiles);
	return 0;

err:
	sb->s_fs_info = NULL;
	kfree(si);
	return ret;
}

static struct dentry *ods2ro_mount(struct file_system_type *fs_type, int flags,
				   const char *dev_name, void *data)
{
	/* Always read-only: this is a presentation, not the write path. */
	return mount_bdev(fs_type, flags | SB_RDONLY, dev_name, data,
			  ods2ro_fill_super);
}

static struct file_system_type ods2ro_fs_type = {
	.owner    = THIS_MODULE,
	.name     = "ods2ro",
	.mount    = ods2ro_mount,
	.kill_sb  = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
};

int vmsfs_ods2ro_register(void)
{
	return register_filesystem(&ods2ro_fs_type);
}

void vmsfs_ods2ro_unregister(void)
{
	unregister_filesystem(&ods2ro_fs_type);
}
