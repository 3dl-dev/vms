// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_blkdev.c - Block-device mode operations for vmsfs (Linux VFS backend)
 *
 * Implements the Linux VFS operations for mounting a block device formatted
 * with the VMSFS on-disk format (vmsfs_ondisk.h). This file is now the THIN
 * Linux backend for the ODS-2 block/inode seam (rd vms-d69, epic vms-8e8): the
 * storage/FID allocator, the directory-block scanner and the file-header
 * decode/encode ALGORITHMS live in the substrate-neutral core
 * (src/kernel-core/vmsfs/vmsfs_alloc.c, vmsfs_dirscan.c, vmsfs_header.c) and are
 * reached through struct vmsfs_volume + the vmsfs_bio ops. What remains here is
 * VFS plumbing: inode iget/flush (the POD <-> struct inode copy around the core
 * decode/encode), the address_space ops, dentry ops, SOGW permission, and the
 * ->create/->mkdir/->unlink/->rmdir/->rename/->lookup/->iterate entry points,
 * each of which calls the core for every ODS-2 decision.
 *
 * OVMX Project - Phase 7: Block-device vmsfs; V2b: ODS-2 core extraction.
 */

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/stat.h>
#include <linux/uio.h>
#include <linux/ktime.h>
#include <linux/uidgid.h>
#include <linux/capability.h>

#include "vmsfs.h"

/* Forward declarations for ops table references */
static struct dentry *vmsfs_blkdev_lookup(struct inode *dir,
                                          struct dentry *dentry,
                                          unsigned int flags);
static int vmsfs_blkdev_iterate(struct file *file, struct dir_context *ctx);
static int vmsfs_blkdev_getattr(struct mnt_idmap *idmap,
                                const struct path *path, struct kstat *stat,
                                u32 request_mask, unsigned int query_flags);
static int vmsfs_blkdev_permission(struct mnt_idmap *idmap,
                                   struct inode *inode, int mask);
static int vmsfs_blkdev_create(struct mnt_idmap *idmap, struct inode *dir,
                               struct dentry *dentry, umode_t mode, bool excl);
static int vmsfs_blkdev_mkdir(struct mnt_idmap *idmap, struct inode *dir,
                              struct dentry *dentry, umode_t mode);
static int vmsfs_blkdev_unlink(struct inode *dir, struct dentry *dentry);
static int vmsfs_blkdev_rmdir(struct inode *dir, struct dentry *dentry);
static int vmsfs_blkdev_rename(struct mnt_idmap *idmap, struct inode *old_dir,
                               struct dentry *old_dentry, struct inode *new_dir,
                               struct dentry *new_dentry, unsigned int flags);
static int vmsfs_ensure_blocks(struct super_block *sb, struct inode *inode,
                               uint32_t target_vbn);
static int vmsfs_blkdev_add_entry(struct super_block *sb, struct inode *dir,
                                  uint32_t fid, const char *name,
                                  uint8_t name_len, uint16_t version);

/* ================================================================
 * Operations tables
 * ================================================================ */

const struct inode_operations vmsfs_blkdev_dir_iops = {
    .lookup     = vmsfs_blkdev_lookup,
    .getattr    = vmsfs_blkdev_getattr,
    .permission = vmsfs_blkdev_permission,
    .create     = vmsfs_blkdev_create,
    .mkdir      = vmsfs_blkdev_mkdir,
    .unlink     = vmsfs_blkdev_unlink,
    .rmdir      = vmsfs_blkdev_rmdir,
    .rename     = vmsfs_blkdev_rename,
};

const struct file_operations vmsfs_blkdev_dir_fops = {
    .owner          = THIS_MODULE,
    .read           = generic_read_dir,
    .iterate_shared = vmsfs_blkdev_iterate,
    .llseek         = generic_file_llseek,
};

const struct inode_operations vmsfs_blkdev_file_iops = {
    .getattr    = vmsfs_blkdev_getattr,
    .permission = vmsfs_blkdev_permission,
};

const struct file_operations vmsfs_blkdev_file_fops = {
    .owner     = THIS_MODULE,
    .read_iter = generic_file_read_iter,
    .write_iter = generic_file_write_iter,
    .llseek    = generic_file_llseek,
    .mmap      = generic_file_mmap,
    .fsync     = generic_file_fsync,
    .splice_read = filemap_splice_read,
};

/*
 * Map a logical file block to a device block (for page cache / mmap).
 * Required for execve to mmap ELF segments from the filesystem.
 */
static int vmsfs_get_block(struct inode *inode, sector_t block,
                           struct buffer_head *bh_result, int create)
{
    struct vmsfs_inode_info *vi = VMSFS_I(inode);
    uint32_t vbn;
    uint32_t lbn;
    int ret;

    /* Validate block number fits in uint32_t after +1 conversion */
    if (block >= (sector_t)UINT_MAX)
        return -EFBIG;
    vbn = (uint32_t)block + 1;  /* VBN is 1-based */

    ret = vmsfs_vbn_to_lbn(vi->map, vi->map_count, vbn, &lbn);
    if (ret) {
        if (!create)
            return 0;  /* Hole — return unmapped (zeroes) */

        /* Allocate new block */
        struct super_block *sb = inode->i_sb;
        struct vmsfs_sb_info *sbi = VMSFS_SB(sb);

        mutex_lock(&sbi->alloc_lock);
        ret = vmsfs_ensure_blocks(sb, inode, vbn);
        mutex_unlock(&sbi->alloc_lock);
        if (ret)
            return ret;

        /* Flush updated inode metadata (block map) to disk */
        vmsfs_blkdev_flush_inode(sb, inode);

        ret = vmsfs_vbn_to_lbn(vi->map, vi->map_count, vbn, &lbn);
        if (ret)
            return ret;

        set_buffer_new(bh_result);
    }

    map_bh(bh_result, inode->i_sb, lbn);
    return 0;
}

static int vmsfs_read_folio(struct file *file, struct folio *folio)
{
    return block_read_full_folio(folio, vmsfs_get_block);
}

/*
 * ->write_begin / block_write_begin() carry the page-cache handle back to the
 * VFS. Kernel 6.12 converted that handle from `struct page **` to
 * `struct folio **` (the folio conversion of the buffer-head write path); the
 * matching ->write_end handle (generic_write_end) is the kernel's own function,
 * so it needs no shim here. OVMX builds vmsfs.ko against two kernels -- the
 * from-source pinned kernel (>= 6.12, distro/Dockerfile.bootable, vms-448) and
 * the stock Ubuntu module-test kernels (6.8, src/kernel/Dockerfile +
 * tests/qemu/Dockerfile) -- so guard on the version boundary that separates the
 * two rather than hard-switching (which would break the stock-kernel harnesses).
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
static int vmsfs_write_begin(struct file *file, struct address_space *mapping,
                             loff_t pos, unsigned len,
                             struct folio **foliop, void **fsdata)
{
    return block_write_begin(mapping, pos, len, foliop, vmsfs_get_block);
}
#else
static int vmsfs_write_begin(struct file *file, struct address_space *mapping,
                             loff_t pos, unsigned len,
                             struct page **pagep, void **fsdata)
{
    return block_write_begin(mapping, pos, len, pagep, vmsfs_get_block);
}
#endif

/*
 * Write dirty page-cache folios back to the block device.
 *
 * Without a writepages hook the dirty file data staged in the page cache by
 * the write path is never flushed to the underlying device — reads within a
 * mount are served from the still-dirty cache, but after unmount/remount the
 * on-disk data blocks read back as zero. mpage_writepages() maps each dirty
 * folio to its device block via vmsfs_get_block() and submits the I/O.
 */
static int vmsfs_writepages(struct address_space *mapping,
                            struct writeback_control *wbc)
{
    return mpage_writepages(mapping, wbc, vmsfs_get_block);
}

static const struct address_space_operations vmsfs_blkdev_aops = {
    .read_folio = vmsfs_read_folio,
    .writepages = vmsfs_writepages,
    .write_begin = vmsfs_write_begin,
    .write_end = generic_write_end,
    .dirty_folio = block_dirty_folio,
    .invalidate_folio = block_invalidate_folio,
};

/* ================================================================
 * File header <-> VFS inode
 *
 * The ODS-2 header INTERPRETATION (validate + decode / encode a 512-byte
 * file header, all fields little-endian + XOR-checksummed) lives in the
 * substrate-neutral core (vmsfs_header.c: vmsfs_fh_decode / vmsfs_fh_encode /
 * vmsfs_fh_write_meta). What stays here is the thin POD <-> struct inode copy
 * and the buffer I/O around it (sb_bread / brelse / iget_locked).
 * ================================================================ */

/*
 * Read a file header from the index area by FID and populate a VFS inode.
 *
 * The file header area starts at sbi->vol.index_lbn. Each header occupies
 * one 512-byte block. FID N is at LBN (index_lbn + N - 1) since FIDs
 * are 1-based.
 */
struct inode *vmsfs_blkdev_iget(struct super_block *sb, uint32_t fid)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct buffer_head *bh;
    struct vmsfs_file_header *fh;
    struct inode *inode;
    struct vmsfs_inode_info *vi;
    struct vmsfs_fh_info fhi;
    uint32_t lbn;
    unsigned int i;

    if (fid == 0 || fid > sbi->vol.max_files)
        return ERR_PTR(-EINVAL);

    /* Check inode cache first */
    inode = iget_locked(sb, fid);
    if (!inode)
        return ERR_PTR(-ENOMEM);
    if (!(inode->i_state & I_NEW))
        return inode;  /* already cached */

    /* FID is 1-based, index area starts at index_lbn */
    lbn = sbi->vol.index_lbn + fid - 1;

    bh = sb_bread(sb, lbn);
    if (!bh) {
        pr_err("vmsfs: unable to read file header for FID %u (LBN %u)\n",
               fid, lbn);
        iget_failed(inode);
        return ERR_PTR(-EIO);
    }

    fh = (struct vmsfs_file_header *)bh->b_data;

    /* Validate + decode the ODS-2 header via the core. */
    switch (vmsfs_fh_decode(bh->b_data, &fhi)) {
    case VMSFS_FH_OK:
        break;
    case VMSFS_FH_BAD_MAGIC:
        pr_err("vmsfs: invalid file header magic for FID %u: 0x%08x\n",
               fid, le32_to_cpu(fh->fh_magic));
        brelse(bh);
        iget_failed(inode);
        return ERR_PTR(-EIO);
    case VMSFS_FH_BAD_CHECKSUM:
        pr_err("vmsfs: file header checksum mismatch for FID %u\n", fid);
        brelse(bh);
        iget_failed(inode);
        return ERR_PTR(-EIO);
    case VMSFS_FH_NOT_INUSE:
        brelse(bh);
        iget_failed(inode);
        return ERR_PTR(-ENOENT);
    }

    /* Populate VFS inode from the decoded POD (host-side copy). */
    vi = VMSFS_I(inode);
    vi->fid = fid;
    vi->version = fhi.version;
    vi->vms_prot = fhi.protection;

    /* Copy name and type */
    memcpy(vi->base_name, fhi.name,
           min_t(size_t, sizeof(vi->base_name), sizeof(fhi.name)));
    vi->base_name[sizeof(vi->base_name) - 1] = '\0';
    memcpy(vi->extension, fhi.type,
           min_t(size_t, sizeof(vi->extension), sizeof(fhi.type)));
    vi->extension[sizeof(vi->extension) - 1] = '\0';

    /* Cache retrieval pointers (already clamped by the decoder) */
    vi->map_count = fhi.map_count;
    for (i = 0; i < vi->map_count; i++)
        vi->map[i] = fhi.map[i];

    /* Set VFS inode fields */
    inode->i_size = fhi.size;
    inode->i_blocks = fhi.blocks;

    /* Timestamps (stored as Unix epoch seconds) */
    inode_set_ctime(inode, fhi.created, 0);
    inode_set_mtime(inode, fhi.modified, 0);
    inode_set_atime(inode, fhi.accessed, 0);

    /* UIC -> uid/gid (simplified mapping) */
    inode->i_uid = KUIDT_INIT(fhi.uic_member);
    inode->i_gid = KGIDT_INIT(fhi.uic_group);

    set_nlink(inode, fhi.link_count);
    if (inode->i_nlink == 0)
        set_nlink(inode, 1);

    /*
     * Set VFS i_mode from the SOGW protection mask for display purposes
     * (tools like ls read stat() mode bits). Actual access control is
     * enforced by .permission using the native SOGW model — i_mode is
     * not consulted for access decisions on vmsfs inodes.
     *
     * For directories we always include the sticky/execute bits so that
     * the VFS can traverse them; .permission gates the real check.
     */
    if (fhi.flags & VMSFS_FH_DIRECTORY) {
        inode->i_mode = S_IFDIR | 0111 |
                        vmsfs_prot_to_mode(vi->vms_prot);
        inode->i_op = &vmsfs_blkdev_dir_iops;
        inode->i_fop = &vmsfs_blkdev_dir_fops;
        set_nlink(inode, 2);
    } else {
        inode->i_mode = S_IFREG | vmsfs_prot_to_mode(vi->vms_prot);
        inode->i_op = &vmsfs_blkdev_file_iops;
        inode->i_fop = &vmsfs_blkdev_file_fops;
        inode->i_mapping->a_ops = &vmsfs_blkdev_aops;
    }

    brelse(bh);
    unlock_new_inode(inode);
    return inode;
}

/* ================================================================
 * Write inode metadata + retrieval pointers to on-disk file header
 *
 * The backend reads the header block; the core (vmsfs_fh_write_meta) does the
 * ODS-2 read-modify-write of the size / blocks / link-count / protection /
 * retrieval-map fields (with a refreshed modification time) + re-checksum.
 * ================================================================ */

int vmsfs_blkdev_flush_inode(struct super_block *sb, struct inode *inode)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct vmsfs_inode_info *vi = VMSFS_I(inode);
    struct buffer_head *bh;
    struct vmsfs_fh_meta meta;
    uint32_t lbn;

    if (vi->fid == 0 || vi->fid > sbi->vol.max_files)
        return -EINVAL;

    lbn = sbi->vol.index_lbn + vi->fid - 1;

    bh = sb_bread(sb, lbn);
    if (!bh)
        return -EIO;

    meta.size       = inode->i_size;
    meta.blocks     = inode->i_blocks;
    meta.link_count = inode->i_nlink;
    meta.protection = vi->vms_prot;
    meta.map_count  = vi->map_count;
    meta.map        = vi->map;
    vmsfs_fh_write_meta(bh->b_data, &meta);

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    return 0;
}

/* ================================================================
 * VBN -> LBN translation and retrieval-map math (substrate-neutral core:
 * src/kernel-core/vmsfs/vmsfs_map.c, rd vms-544).
 *
 * Storage / FID / cluster allocation and home-block write-back (substrate-
 * neutral core: src/kernel-core/vmsfs/vmsfs_alloc.c, rd vms-d69) -- reached
 * through struct vmsfs_volume + the vmsfs_bio ops. This backend calls them with
 * &sbi->vol under sbi->alloc_lock, exactly as the former file-static helpers
 * ran under that lock.
 * ================================================================ */

/*
 * Ensure the file has blocks allocated through the given VBN.
 *
 * The block-level growth (allocate + extend the retrieval map + zero each new
 * block) is the core's vmsfs_grow_map(); this wrapper applies the resulting
 * block-count delta to the host inode. Caller must hold sbi->alloc_lock.
 */
static int vmsfs_ensure_blocks(struct super_block *sb, struct inode *inode,
                               uint32_t target_vbn)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct vmsfs_inode_info *vi = VMSFS_I(inode);
    int blocks_added = 0;
    int ret;

    ret = vmsfs_grow_map(&sbi->vol, vi->map, &vi->map_count, target_vbn,
                         &blocks_added);
    inode->i_blocks += blocks_added;
    return ret;
}

/* ================================================================
 * Name helpers and the directory-block scanner
 *
 * The pure ODS-2 filename FORMAT algorithms (vmsfs_split_name_type,
 * vmsfs_strupper, vmsfs_name_match; src/kernel-core/vmsfs/vmsfs_name.c) and the
 * directory-block SCANNER (lookup/version-resolution/add/remove/emptiness;
 * src/kernel-core/vmsfs/vmsfs_dirscan.c, rd vms-d69) live in the core and name
 * no host object. This backend supplies the retrieval map + case-blind flag and
 * keeps the struct-inode mutation.
 * ================================================================ */

/*
 * Add a directory entry via the core scanner, then apply any directory growth
 * to the host inode. When the core had to allocate a new directory data block
 * (blocks_added), the directory's on-disk header (with the extended retrieval
 * map) is flushed here. Caller must hold sbi->alloc_lock.
 */
static int vmsfs_blkdev_add_entry(struct super_block *sb, struct inode *dir,
                                  uint32_t fid, const char *name,
                                  uint8_t name_len, uint16_t version)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct vmsfs_inode_info *dir_vi = VMSFS_I(dir);
    int blocks_added = 0;
    int ret;

    ret = vmsfs_dir_add_entry(&sbi->vol, dir_vi->map, &dir_vi->map_count,
                              fid, name, name_len, version, &blocks_added);
    if (blocks_added) {
        int fret;

        dir->i_blocks += blocks_added;
        dir->i_size += (loff_t)blocks_added * VMSFS_BLOCK_SIZE;
        fret = vmsfs_blkdev_flush_inode(sb, dir);
        if (fret)
            return fret;
    }
    return ret;
}

/* ================================================================
 * Directory lookup
 * ================================================================ */

/*
 * vmsfs_blkdev_resolve - Resolve a (possibly versioned) name to a FID.
 *
 * Thin wrapper over the core directory scanner (vmsfs_dir_resolve): it supplies
 * the directory's retrieval map + the volume + the case-blind option. *fid_out
 * is 0 when the directory holds no entry for @name; a negative errno is returned
 * only when @name is not a parseable VMS filename.
 *
 * Factored out of vmsfs_blkdev_lookup() so that ->d_revalidate can ask the
 * SAME question a lookup would ask ("what does this name resolve to right
 * now?") without allocating a dentry -- see vmsfs_d_revalidate() in
 * vmsfs_inode.c.
 */
int vmsfs_blkdev_resolve(struct inode *dir, const char *name,
                         uint32_t *fid_out)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(dir->i_sb);
    struct vmsfs_inode_info *dir_vi = VMSFS_I(dir);

    return vmsfs_dir_resolve(&sbi->vol, dir_vi->map, dir_vi->map_count,
                             sbi->opts.case_blind, name, fid_out);
}

/*
 * vmsfs_blkdev_lookup - Look up a file in a block-device directory.
 *
 * Thin wrapper over vmsfs_blkdev_resolve(): name -> FID -> inode -> dentry.
 */
static struct dentry *vmsfs_blkdev_lookup(struct inode *dir,
                                          struct dentry *dentry,
                                          unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    struct inode *inode;
    uint32_t fid = 0;
    int ret;

    (void)flags;

    ret = vmsfs_blkdev_resolve(dir, dentry->d_name.name, &fid);
    if (ret)
        return ERR_PTR(ret);

    if (fid == 0) {
        /* Not found -- negative dentry */
        return d_splice_alias(NULL, dentry);
    }

    inode = vmsfs_blkdev_iget(sb, fid);
    if (IS_ERR(inode))
        return ERR_CAST(inode);

    return d_splice_alias(inode, dentry);
}

/* ================================================================
 * Directory iteration (readdir)
 *
 * The backend owns the map-walk + dir_emit emission (both inherently VFS); the
 * core owns the ODS-2 per-entry decode (vmsfs_dir_entry_decode) and the display
 * name format (vmsfs_dir_format_name).
 * ================================================================ */

static int vmsfs_blkdev_iterate(struct file *file, struct dir_context *ctx)
{
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    struct vmsfs_inode_info *dir_vi = VMSFS_I(inode);
    uint32_t vbn;
    loff_t entry_pos;
    int ret;

    if (!dir_emit_dots(file, ctx))
        return 0;

    /* ctx->pos >= 2 after dots. Track which data entry we're at. */
    entry_pos = 2;

    /* Walk all data blocks of this directory */
    for (vbn = 1; ; vbn++) {
        uint32_t lbn;
        struct buffer_head *bh;
        unsigned int j;

        ret = vmsfs_vbn_to_lbn(dir_vi->map, dir_vi->map_count, vbn, &lbn);
        if (ret)
            break;

        bh = sb_bread(sb, lbn);
        if (!bh)
            break;

        for (j = 0; j < VMSFS_DIR_PER_BLOCK; j++) {
            struct vmsfs_dirent_view view;
            unsigned int off = j * VMSFS_DIR_ENTRY_SIZE;
            char fullname[VMSFS_MAX_FILENAME + 1];
            int namelen;
            int is_dir;
            unsigned int d_type;

            if (off + VMSFS_DIR_ENTRY_SIZE > VMSFS_BLOCK_SIZE)
                break;

            /* Free slots still count, for stable positioning. */
            if (!vmsfs_dir_entry_decode(bh->b_data, j, &view)) {
                entry_pos++;
                continue;
            }

            /* Skip entries already emitted in previous calls */
            if (entry_pos < ctx->pos) {
                entry_pos++;
                continue;
            }

            /*
             * Build the display name. Versioned files get ";version"
             * and DT_REG; unversioned entries (directories) get just
             * the name and DT_DIR.
             */
            namelen = vmsfs_dir_format_name(&view, fullname,
                                            sizeof(fullname), &is_dir);
            d_type = is_dir ? DT_DIR : DT_REG;

            if (namelen <= 0 || namelen >= (int)sizeof(fullname)) {
                entry_pos++;
                continue;
            }

            if (!dir_emit(ctx, fullname, namelen, view.fid, d_type)) {
                brelse(bh);
                return 0;
            }
            entry_pos++;
            ctx->pos = entry_pos;
        }

        brelse(bh);
    }

    return 0;
}

/* ================================================================
 * getattr
 * ================================================================ */

static int vmsfs_blkdev_getattr(struct mnt_idmap *idmap,
                                const struct path *path, struct kstat *stat,
                                u32 request_mask, unsigned int query_flags)
{
    struct inode *inode = d_inode(path->dentry);

    generic_fillattr(&nop_mnt_idmap, request_mask, inode, stat);
    return 0;
}

/* ================================================================
 * SOGW permission check
 *
 * This is the authoritative access-control function for vmsfs block-device
 * inodes. It replaces the default generic_permission() check so that access
 * decisions are made using the VMS SOGW model rather than Unix rwx bits.
 *
 * Category assignment (in priority order):
 *   System: process is root OR process UIC group <= MAXSYSGROUP (vms-581)
 *   Owner:  process euid matches inode uid AND egid matches inode gid
 *   Group:  process egid matches inode gid
 *   World:  all other processes
 *
 * The System category gets full access — matching VMS behavior where the
 * SYSTEM UIC group bypasses protection checks.
 *
 * mask bits checked:
 *   MAY_READ  -> VMSFS_PROT_R
 *   MAY_WRITE -> VMSFS_PROT_W
 *   MAY_EXEC  -> VMSFS_PROT_E
 *   MAY_OPEN  -> treated as MAY_READ (open requires at least read)
 *   MAY_UNLINK_SELF -> VMSFS_PROT_D (delete)
 *
 * Note: the VMS Delete bit is not checked here for unlink/rmdir because
 * those operations go through the directory's inode ops (.unlink/.rmdir)
 * before reaching the target inode's .permission. Delete permission on
 * the file itself is enforced at the VFS level via MAY_WRITE on the parent
 * directory. VMS delete semantics (checking D bit on the file being deleted)
 * would require custom VFS hooks beyond the scope of this bead; the D bit
 * is stored and preserved but enforcement is deferred.
 * ================================================================ */

/*
 * vmsfs_current_uic - the UIC (owner) a NEWLY created inode should carry,
 * derived from the creating process's own credentials (vms-221).
 *
 * fh_uic_group/fh_uic_member are separate on-disk fields from fh_protection
 * (see vmsfs_ondisk.h) and vmsfs_blkdev_iget() reads them straight into the
 * VFS inode (inode->i_uid = fh_uic_member, inode->i_gid = fh_uic_group).
 * vmsfs_blkdev_create()/vmsfs_blkdev_mkdir() used to leave them at their
 * memset(0) value -- every new file/directory was owned UIC [0,0] on disk,
 * regardless of who created it. That is invisible on the file's OWN first
 * open (Linux always lets the creator use what it just made, regardless of
 * the resulting mode/protection), but it means every SUBSEQUENT reopen by
 * the real creator falls through vmsfs_blkdev_permission()'s Owner check
 * (creator's UIC != [0,0]) to Group or World, which VMSFS_PROT_DEFAULT
 * denies WRITE. Matches plain Unix inode-creation semantics: the creator
 * owns what it creates.
 */
static void vmsfs_current_uic(uint16_t *uic_member, uint16_t *uic_group)
{
    kuid_t euid = current_euid();
    kgid_t egid = current_egid();

    *uic_member = (uint16_t)__kuid_val(euid);
    *uic_group  = (uint16_t)__kgid_val(egid);
}

static int vmsfs_blkdev_permission(struct mnt_idmap *idmap,
                                   struct inode *inode, int mask)
{
    struct vmsfs_inode_info *vi = VMSFS_I(inode);
    uint16_t prot = vi->vms_prot;
    uint8_t deny;
    kuid_t inode_uid = inode->i_uid;
    kgid_t inode_gid = inode->i_gid;
    kuid_t euid;
    kgid_t egid;

    /* MAY_NOT_BLOCK: caller cannot sleep — fall back to generic check */
    if (mask & MAY_NOT_BLOCK)
        return -ECHILD;

    euid = current_euid();
    egid = current_egid();

    /*
     * System category: root (uid 0) or UIC group <= MAXSYSGROUP (vms-581).
     * In our UIC mapping, egid holds the process UIC group. The documented
     * VMS rule is a group COMPARISON, not equality with 0: every UIC whose
     * group number is <= MAXSYSGROUP (default 8) is in the System category
     * (OpenVMS Guide to System Security, "System" access category) — the same
     * rule the userspace executive uses (uic_is_system(),
     * src/libvms/syssvc/sys_security.c). A `group == 0` test denied the SYSTEM
     * account (real UIC [1,4], Linux gid=1 after LOGINOUT's credential drop)
     * the System category on every real vmsfs.ko mount, so it fell to the
     * Group/World nibble and RMS CREATE failed %RMS-E-CRE/EACCES. Group 0 is
     * not a valid VMS UIC group; root's [0,0] is covered incidentally by
     * 0 <= MAXSYSGROUP as well as by the explicit root check below.
     */
    if (uid_eq(euid, GLOBAL_ROOT_UID) ||
        __kgid_val(egid) <= VMSFS_MAXSYSGROUP) {
        /* System category — full access regardless of protection bits */
        deny = (prot >> VMSFS_PROT_SYS_SHIFT) & 0xF;
        /* System gets full access: even if someone set denial bits,
         * honor them for predictability, but by VMS convention System
         * is never denied. We implement the strict VMS behavior: the
         * System nibble is checked, but a well-formatted volume will
         * always have System=0 (no denials). */
        goto check;
    }

    /*
     * Owner category: euid matches owner uid AND egid matches owner gid.
     * (Both group and member must match the file's UIC.)
     */
    if (uid_eq(euid, inode_uid) && gid_eq(egid, inode_gid)) {
        deny = (prot >> VMSFS_PROT_OWN_SHIFT) & 0xF;
        goto check;
    }

    /*
     * Group category: egid matches owner gid (UIC group matches).
     */
    if (gid_eq(egid, inode_gid)) {
        deny = (prot >> VMSFS_PROT_GRP_SHIFT) & 0xF;
        goto check;
    }

    /* World category: no UIC match */
    deny = (prot >> VMSFS_PROT_WLD_SHIFT) & 0xF;

check:
    if ((mask & MAY_READ) && (deny & VMSFS_PROT_R))
        return -EACCES;
    if ((mask & MAY_WRITE) && (deny & VMSFS_PROT_W))
        return -EACCES;
    if ((mask & MAY_EXEC) && (deny & VMSFS_PROT_E))
        return -EACCES;

    return 0;
}

/* ================================================================
 * File creation (with VMS auto-versioning)
 * ================================================================ */

static int vmsfs_blkdev_create(struct mnt_idmap *idmap, struct inode *dir,
                               struct dentry *dentry, umode_t mode, bool excl)
{
    struct super_block *sb = dir->i_sb;
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct vmsfs_inode_info *dir_vi = VMSFS_I(dir);
    struct inode *inode;
    char base[VMSFS_MAX_FILENAME + 1];
    char fname[VMSFS_MAX_NAME + 1];
    char ftype[VMSFS_MAX_TYPE + 1];
    int req_version;
    uint16_t highest, new_version;
    uint32_t fid, lbn;
    struct buffer_head *bh;
    struct vmsfs_fh_info fhi;
    uint16_t uic_member, uic_group;
    uint64_t now;
    int ret;

    /* Parse version from requested name */
    ret = vmsfs_parse_version(dentry->d_name.name, base, sizeof(base),
                              &req_version);
    if (ret)
        return ret;

    /* Uppercase for VMS ODS-2 convention */
    vmsfs_strupper(base);

    /* Split into name and type components */
    vmsfs_split_name_type(base, fname, sizeof(fname), ftype, sizeof(ftype));

    mutex_lock(&sbi->alloc_lock);

    /* Determine version number */
    if (req_version > 0) {
        new_version = req_version;
    } else {
        highest = vmsfs_dir_highest_version(&sbi->vol, dir_vi->map,
                                            dir_vi->map_count,
                                            sbi->opts.case_blind,
                                            base, strlen(base));
        new_version = highest + 1;
        if (new_version > VMSFS_VERSION_MAX) {
            mutex_unlock(&sbi->alloc_lock);
            return -ENOSPC;
        }
    }

    /* Allocate FID */
    ret = vmsfs_alloc_fid(&sbi->vol, &fid);
    if (ret) {
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    /* Initialize file header on disk (encoded by the core) */
    lbn = sbi->vol.index_lbn + fid - 1;
    bh = sb_bread(sb, lbn);
    if (!bh) {
        mutex_unlock(&sbi->alloc_lock);
        return -EIO;
    }

    now = ktime_get_real_seconds();
    vmsfs_current_uic(&uic_member, &uic_group);

    memset(&fhi, 0, sizeof(fhi));
    fhi.fid        = fid;
    fhi.size       = 0;
    fhi.created    = now;
    fhi.modified   = now;
    fhi.accessed   = now;
    fhi.blocks     = 0;
    fhi.parent_fid = dir_vi->fid;
    fhi.flags      = VMSFS_FH_INUSE;
    fhi.version    = new_version;
    fhi.protection = VMSFS_PROT_DEFAULT;
    fhi.uic_member = uic_member;
    fhi.uic_group  = uic_group;
    fhi.link_count = 1;
    fhi.map_count  = 0;
    strscpy(fhi.name, fname, sizeof(fhi.name));
    strscpy(fhi.type, ftype, sizeof(fhi.type));
    vmsfs_fh_encode(&fhi, bh->b_data);

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    /* Add directory entry */
    ret = vmsfs_blkdev_add_entry(sb, dir, fid, base, strlen(base), new_version);
    if (ret) {
        vmsfs_free_fid(&sbi->vol, fid);
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    vmsfs_update_home_block(&sbi->vol);
    mutex_unlock(&sbi->alloc_lock);

    /* Create VFS inode via iget (reads the header we just wrote) */
    inode = vmsfs_blkdev_iget(sb, fid);
    if (IS_ERR(inode))
        return PTR_ERR(inode);

    d_instantiate(dentry, inode);
    return 0;
}

/* ================================================================
 * Directory creation
 * ================================================================ */

static int vmsfs_blkdev_mkdir(struct mnt_idmap *idmap, struct inode *dir,
                              struct dentry *dentry, umode_t mode)
{
    struct super_block *sb = dir->i_sb;
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct vmsfs_inode_info *dir_vi = VMSFS_I(dir);
    struct inode *inode;
    const char *name = dentry->d_name.name;
    char uname[VMSFS_MAX_FILENAME + 1];
    char fname[VMSFS_MAX_NAME + 1];
    char ftype[VMSFS_MAX_TYPE + 1];
    uint32_t fid, data_lbn, lbn;
    struct buffer_head *bh;
    struct vmsfs_fh_info fhi;
    uint16_t uic_member, uic_group;
    uint64_t now;
    size_t nlen;
    int ret;

    /* Uppercase the directory name (VMS convention) */
    strscpy(uname, name, sizeof(uname));
    vmsfs_strupper(uname);

    /* Directories are "NAME.DIR" in VMS; strip .DIR if user added it */
    strscpy(fname, uname, sizeof(fname));
    nlen = strlen(fname);
    if (nlen > 4 && strcasecmp(fname + nlen - 4, ".DIR") == 0)
        fname[nlen - 4] = '\0';
    strscpy(ftype, "DIR", sizeof(ftype));

    mutex_lock(&sbi->alloc_lock);

    ret = vmsfs_alloc_fid(&sbi->vol, &fid);
    if (ret) {
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    /* Allocate initial data block for directory entries */
    ret = vmsfs_alloc_block(&sbi->vol, &data_lbn);
    if (ret) {
        vmsfs_free_fid(&sbi->vol, fid);
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    /* Zero the data block */
    bh = sb_bread(sb, data_lbn);
    if (!bh) {
        vmsfs_free_block(&sbi->vol, data_lbn);
        vmsfs_free_fid(&sbi->vol, fid);
        mutex_unlock(&sbi->alloc_lock);
        return -EIO;
    }
    memset(bh->b_data, 0, VMSFS_BLOCK_SIZE);
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    /* Initialize file header on disk (encoded by the core) */
    lbn = sbi->vol.index_lbn + fid - 1;
    bh = sb_bread(sb, lbn);
    if (!bh) {
        vmsfs_free_block(&sbi->vol, data_lbn);
        vmsfs_free_fid(&sbi->vol, fid);
        mutex_unlock(&sbi->alloc_lock);
        return -EIO;
    }

    now = ktime_get_real_seconds();
    vmsfs_current_uic(&uic_member, &uic_group);

    memset(&fhi, 0, sizeof(fhi));
    fhi.fid        = fid;
    fhi.size       = VMSFS_BLOCK_SIZE;
    fhi.created    = now;
    fhi.modified   = now;
    fhi.accessed   = now;
    fhi.blocks     = 1;
    fhi.parent_fid = dir_vi->fid;
    fhi.flags      = VMSFS_FH_INUSE | VMSFS_FH_DIRECTORY;
    fhi.version    = 0;  /* directories are unversioned */
    fhi.protection = VMSFS_PROT_DEFAULT;
    fhi.uic_member = uic_member;
    fhi.uic_group  = uic_group;
    fhi.link_count = 2;
    fhi.map_count  = 1;
    strscpy(fhi.name, fname, sizeof(fhi.name));
    strscpy(fhi.type, ftype, sizeof(fhi.type));
    fhi.map[0].rp_lbn = data_lbn;
    fhi.map[0].rp_count = 1;
    vmsfs_fh_encode(&fhi, bh->b_data);

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    /* Add entry to parent directory */
    ret = vmsfs_blkdev_add_entry(sb, dir, fid, uname, strlen(uname), 0);
    if (ret) {
        vmsfs_free_block(&sbi->vol, data_lbn);
        vmsfs_free_fid(&sbi->vol, fid);
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    inc_nlink(dir);
    vmsfs_blkdev_flush_inode(sb, dir);
    vmsfs_update_home_block(&sbi->vol);
    mutex_unlock(&sbi->alloc_lock);

    inode = vmsfs_blkdev_iget(sb, fid);
    if (IS_ERR(inode))
        return PTR_ERR(inode);

    d_instantiate(dentry, inode);
    return 0;
}

/* ================================================================
 * File and directory deletion
 * ================================================================ */

static int vmsfs_blkdev_unlink(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct inode *inode = d_inode(dentry);
    struct vmsfs_inode_info *vi = VMSFS_I(inode);
    struct vmsfs_inode_info *dir_vi = VMSFS_I(dir);
    int ret;

    mutex_lock(&sbi->alloc_lock);

    /* Remove directory entry from parent */
    ret = vmsfs_dir_remove_entry(&sbi->vol, dir_vi->map, dir_vi->map_count,
                                 vi->fid, vi->version);
    if (ret) {
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    /* Free all data blocks */
    vmsfs_free_file_blocks(&sbi->vol, vi->map, vi->map_count);

    /* Free the file header */
    vmsfs_free_fid(&sbi->vol, vi->fid);

    drop_nlink(inode);
    vmsfs_update_home_block(&sbi->vol);
    mutex_unlock(&sbi->alloc_lock);

    return 0;
}

static int vmsfs_blkdev_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode = d_inode(dentry);
    struct vmsfs_sb_info *sbi = VMSFS_SB(inode->i_sb);
    struct vmsfs_inode_info *dir_vi = VMSFS_I(inode);
    int ret;

    /* Check directory is empty */
    if (!vmsfs_dir_is_empty(&sbi->vol, dir_vi->map, dir_vi->map_count))
        return -ENOTEMPTY;

    ret = vmsfs_blkdev_unlink(dir, dentry);
    if (ret)
        return ret;

    drop_nlink(dir);
    vmsfs_blkdev_flush_inode(dir->i_sb, dir);
    return 0;
}

/* ================================================================
 * Rename — move a directory entry from (old_dir, old name) to
 * (new_dir, new name), replacing an existing target if present.
 *
 * The file keeps its FID and file header (and therefore its data
 * blocks and retrieval map); only the directory entry that names it
 * and the header's name/type/version/parent fields change. This is
 * the write-tmp + rename(2) save path AUTHORIZE save_sysuaf(), SET
 * PASSWORD, DCL RENAME, RMS $RENAME and SYSGEN .PAR WRITE all depend
 * on. vmsfs previously had no ->rename, so the Linux VFS returned
 * EPERM for every rename on a vmsfs.ko volume (observed as
 * %UAF-E-RENAMEFAIL "... Operation not permitted" — rd vms-8b3).
 *
 * Semantics: standard POSIX rename. RENAME_NOREPLACE is honored;
 * RENAME_EXCHANGE / RENAME_WHITEOUT are not supported and fail
 * -EINVAL — an honest error, never a fabricated success (INV-6). A
 * pre-existing target is replaced: its directory entry, file header,
 * FID and data blocks are freed exactly as unlink would free them. A
 * directory target must be empty (-ENOTEMPTY), mirroring rmdir.
 *
 * Directory-entry identity here is (de_fid, de_version): the file is
 * moved by removing its old entry and adding a new one that points at
 * the SAME FID, then rewriting the header's name/type/version and
 * refreshing the cached in-core name/version so a later
 * unlink/lookup (which keys on fid+version) still matches.
 * ================================================================ */
static int vmsfs_blkdev_rename(struct mnt_idmap *idmap, struct inode *old_dir,
                               struct dentry *old_dentry, struct inode *new_dir,
                               struct dentry *new_dentry, unsigned int flags)
{
    struct super_block *sb = old_dir->i_sb;
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct inode *old_inode = d_inode(old_dentry);
    struct inode *new_inode = d_inode(new_dentry);
    struct vmsfs_inode_info *old_vi = VMSFS_I(old_inode);
    bool is_dir = S_ISDIR(old_inode->i_mode);
    bool target_was_dir = new_inode && S_ISDIR(new_inode->i_mode);
    char base[VMSFS_MAX_FILENAME + 1];
    char fname[VMSFS_MAX_NAME + 1];
    char ftype[VMSFS_MAX_TYPE + 1];
    char old_full[VMSFS_MAX_FILENAME + 1];
    int req_version;
    uint16_t new_version = 0;
    struct buffer_head *bh;
    uint32_t lbn;
    int ret;

    (void)idmap;

    /* We honor RENAME_NOREPLACE; EXCHANGE/WHITEOUT are unsupported. */
    if (flags & ~RENAME_NOREPLACE)
        return -EINVAL;

    /* Nothing to do if the VFS handed us the same object both ways. */
    if (old_inode == new_inode)
        return 0;

    if (new_inode) {
        if (flags & RENAME_NOREPLACE)
            return -EEXIST;
        /* POSIX source/target type compatibility. */
        if (target_was_dir && !is_dir)
            return -EISDIR;
        if (!target_was_dir && is_dir)
            return -ENOTDIR;
    }

    /* Parse and normalize the requested new name (VMS ODS-2 uppercase). */
    ret = vmsfs_parse_version(new_dentry->d_name.name, base, sizeof(base),
                              &req_version);
    if (ret)
        return ret;
    vmsfs_strupper(base);

    if (is_dir) {
        size_t nlen = strlen(base);

        /* Directories are unversioned; drop a trailing .DIR the user typed. */
        if (nlen > 4 && strcasecmp(base + nlen - 4, ".DIR") == 0)
            base[nlen - 4] = '\0';
        strscpy(fname, base, sizeof(fname));
        strscpy(ftype, "DIR", sizeof(ftype));
        new_version = 0;
    } else {
        vmsfs_split_name_type(base, fname, sizeof(fname),
                              ftype, sizeof(ftype));
    }

    /* Reconstruct the source entry's stored name, for rollback on failure. */
    if (old_vi->extension[0] != '\0')
        snprintf(old_full, sizeof(old_full), "%s.%s",
                 old_vi->base_name, old_vi->extension);
    else
        strscpy(old_full, old_vi->base_name, sizeof(old_full));

    mutex_lock(&sbi->alloc_lock);

    /* A directory target must be empty, mirroring rmdir. */
    if (target_was_dir) {
        struct vmsfs_inode_info *tvi = VMSFS_I(new_inode);

        if (!vmsfs_dir_is_empty(&sbi->vol, tvi->map, tvi->map_count)) {
            mutex_unlock(&sbi->alloc_lock);
            return -ENOTEMPTY;
        }
    }

    /*
     * Resolve the version for the new entry now that we hold the lock. A
     * versioned request pins that version; replacing a target reuses the
     * target's version (POSIX in-place replace); otherwise auto-version
     * like create (highest existing + 1).
     */
    if (!is_dir) {
        if (req_version > 0) {
            new_version = req_version;
        } else if (new_inode) {
            new_version = (uint16_t)VMSFS_I(new_inode)->version;
        } else {
            uint16_t highest =
                vmsfs_dir_highest_version(&sbi->vol, VMSFS_I(new_dir)->map,
                                          VMSFS_I(new_dir)->map_count,
                                          sbi->opts.case_blind,
                                          base, strlen(base));
            new_version = highest + 1;
            if (new_version > VMSFS_VERSION_MAX) {
                mutex_unlock(&sbi->alloc_lock);
                return -ENOSPC;
            }
        }
    }

    /* (a) Replace: free the target's entry, header, FID and data blocks. */
    if (new_inode) {
        struct vmsfs_inode_info *tvi = VMSFS_I(new_inode);

        ret = vmsfs_dir_remove_entry(&sbi->vol, VMSFS_I(new_dir)->map,
                                     VMSFS_I(new_dir)->map_count,
                                     tvi->fid, tvi->version);
        if (ret) {
            mutex_unlock(&sbi->alloc_lock);
            return ret;
        }
        vmsfs_free_file_blocks(&sbi->vol, tvi->map, tvi->map_count);
        vmsfs_free_fid(&sbi->vol, tvi->fid);
        clear_nlink(new_inode);
        if (target_was_dir)
            drop_nlink(new_dir);
    }

    /* (b) Remove the source's old directory entry. */
    ret = vmsfs_dir_remove_entry(&sbi->vol, VMSFS_I(old_dir)->map,
                                 VMSFS_I(old_dir)->map_count,
                                 old_vi->fid, old_vi->version);
    if (ret) {
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    /* (c) Add the new directory entry pointing at the SAME FID. */
    ret = vmsfs_blkdev_add_entry(sb, new_dir, old_vi->fid, base,
                                 strlen(base), new_version);
    if (ret) {
        /* Best-effort rollback so a full new_dir cannot orphan the file. */
        vmsfs_blkdev_add_entry(sb, old_dir, old_vi->fid, old_full,
                               strlen(old_full), old_vi->version);
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    /* (d) Rewrite the file header's name/type/version/parent on disk. */
    lbn = sbi->vol.index_lbn + old_vi->fid - 1;
    bh = sb_bread(sb, lbn);
    if (bh) {
        struct vmsfs_fh_rename rn = {
            .name       = fname,
            .type       = ftype,
            .version    = new_version,
            .parent_fid = VMSFS_I(new_dir)->fid,
        };

        vmsfs_fh_write_rename(bh->b_data, &rn);
        mark_buffer_dirty(bh);
        sync_dirty_buffer(bh);
        brelse(bh);
    }

    /* (e) A cross-directory move adjusts both directories' link counts. */
    if (is_dir && old_dir != new_dir) {
        drop_nlink(old_dir);
        inc_nlink(new_dir);
    }

    /*
     * (f) Refresh the cached in-core name/version so a later
     * unlink/lookup keys on the NEW values (vmsfs_dir_remove_entry and
     * vmsfs_dir_resolve match on fid+version / parsed name).
     */
    old_vi->version = new_version;
    memset(old_vi->base_name, 0, sizeof(old_vi->base_name));
    memset(old_vi->extension, 0, sizeof(old_vi->extension));
    strscpy(old_vi->base_name, fname, sizeof(old_vi->base_name));
    strscpy(old_vi->extension, ftype, sizeof(old_vi->extension));

    /* Persist directory metadata that changed, then the free-count. */
    vmsfs_blkdev_flush_inode(sb, old_dir);
    if (new_dir != old_dir)
        vmsfs_blkdev_flush_inode(sb, new_dir);
    vmsfs_update_home_block(&sbi->vol);

    mutex_unlock(&sbi->alloc_lock);

    inode_set_ctime(old_inode, ktime_get_real_seconds(), 0);
    return 0;
}
