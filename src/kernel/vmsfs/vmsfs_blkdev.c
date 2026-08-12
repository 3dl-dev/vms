// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_blkdev.c - Block-device mode operations for vmsfs
 *
 * Implements filesystem operations for mounting a block device
 * formatted with the VMSFS on-disk format (vmsfs_ondisk.h).
 *
 * Read operations:
 *   - vmsfs_blkdev_iget():  read file header by FID -> populate VFS inode
 *   - lookup:  scan directory data blocks for entries
 *   - readdir: iterate directory data blocks, emit entries
 *   - read:    VBN->LBN translation via retrieval pointers, sb_bread()
 *
 * Write operations:
 *   - create:  allocate FID, add directory entry (auto-versioning)
 *   - mkdir:   allocate FID + data block, add directory entry
 *   - write:   VBN->LBN with block allocation, buffer_head writes
 *   - unlink:  remove directory entry, free blocks and FID
 *
 * OVMX Project - Phase 7: Block-device vmsfs
 */

#include <linux/kernel.h>
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
static int vmsfs_vbn_to_lbn(struct vmsfs_inode_info *vi, uint32_t vbn,
                            uint32_t *lbn_out);
static int vmsfs_ensure_blocks(struct super_block *sb, struct inode *inode,
                               uint32_t target_vbn);

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

    ret = vmsfs_vbn_to_lbn(vi, vbn, &lbn);
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

        ret = vmsfs_vbn_to_lbn(vi, vbn, &lbn);
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

static int vmsfs_write_begin(struct file *file, struct address_space *mapping,
                             loff_t pos, unsigned len,
                             struct page **pagep, void **fsdata)
{
    return block_write_begin(mapping, pos, len, pagep, vmsfs_get_block);
}

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
 * File header -> VFS inode
 * ================================================================ */

/*
 * Read a file header from the index area by FID and populate a VFS inode.
 *
 * The file header area starts at sbi->index_lbn. Each header occupies
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
    uint32_t lbn;
    uint32_t cksum;
    unsigned int i;

    if (fid == 0 || fid > sbi->max_files)
        return ERR_PTR(-EINVAL);

    /* Check inode cache first */
    inode = iget_locked(sb, fid);
    if (!inode)
        return ERR_PTR(-ENOMEM);
    if (!(inode->i_state & I_NEW))
        return inode;  /* already cached */

    /* FID is 1-based, index area starts at index_lbn */
    lbn = sbi->index_lbn + fid - 1;

    bh = sb_bread(sb, lbn);
    if (!bh) {
        pr_err("vmsfs: unable to read file header for FID %u (LBN %u)\n",
               fid, lbn);
        iget_failed(inode);
        return ERR_PTR(-EIO);
    }

    fh = (struct vmsfs_file_header *)bh->b_data;

    /* Validate magic */
    if (le32_to_cpu(fh->fh_magic) != VMSFS_FH_MAGIC) {
        pr_err("vmsfs: invalid file header magic for FID %u: 0x%08x\n",
               fid, le32_to_cpu(fh->fh_magic));
        brelse(bh);
        iget_failed(inode);
        return ERR_PTR(-EIO);
    }

    /* Validate checksum */
    cksum = vmsfs_checksum(fh, sizeof(*fh));
    if (cksum != le32_to_cpu(fh->fh_checksum)) {
        pr_err("vmsfs: file header checksum mismatch for FID %u\n", fid);
        brelse(bh);
        iget_failed(inode);
        return ERR_PTR(-EIO);
    }

    /* Verify in-use */
    if (!(le16_to_cpu(fh->fh_flags) & VMSFS_FH_INUSE)) {
        brelse(bh);
        iget_failed(inode);
        return ERR_PTR(-ENOENT);
    }

    /* Populate VFS inode */
    vi = VMSFS_I(inode);
    vi->fid = fid;
    vi->version = le16_to_cpu(fh->fh_version);
    vi->vms_prot = le16_to_cpu(fh->fh_protection);

    /* Copy name and type */
    memcpy(vi->base_name, fh->fh_name,
           min_t(size_t, sizeof(vi->base_name), sizeof(fh->fh_name)));
    vi->base_name[sizeof(vi->base_name) - 1] = '\0';
    memcpy(vi->extension, fh->fh_type,
           min_t(size_t, sizeof(vi->extension), sizeof(fh->fh_type)));
    vi->extension[sizeof(vi->extension) - 1] = '\0';

    /* Cache retrieval pointers */
    vi->map_count = le16_to_cpu(fh->fh_map_count);
    if (vi->map_count > VMSFS_MAX_RETRIEVAL)
        vi->map_count = VMSFS_MAX_RETRIEVAL;
    for (i = 0; i < vi->map_count; i++) {
        vi->map[i].rp_lbn = le32_to_cpu(fh->fh_map[i].rp_lbn);
        vi->map[i].rp_count = le32_to_cpu(fh->fh_map[i].rp_count);
    }

    /* Set VFS inode fields */
    inode->i_size = le64_to_cpu(fh->fh_size);
    inode->i_blocks = le32_to_cpu(fh->fh_blocks);

    /* Timestamps (stored as Unix epoch seconds) */
    inode_set_ctime(inode, le64_to_cpu(fh->fh_created), 0);
    inode_set_mtime(inode, le64_to_cpu(fh->fh_modified), 0);
    inode_set_atime(inode, le64_to_cpu(fh->fh_accessed), 0);

    /* UIC -> uid/gid (simplified mapping) */
    inode->i_uid = KUIDT_INIT(le16_to_cpu(fh->fh_uic_member));
    inode->i_gid = KGIDT_INIT(le16_to_cpu(fh->fh_uic_group));

    set_nlink(inode, le16_to_cpu(fh->fh_link_count));
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
    if (le16_to_cpu(fh->fh_flags) & VMSFS_FH_DIRECTORY) {
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
 * ================================================================ */

int vmsfs_blkdev_flush_inode(struct super_block *sb, struct inode *inode)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct vmsfs_inode_info *vi = VMSFS_I(inode);
    struct buffer_head *bh;
    struct vmsfs_file_header *fh;
    uint32_t lbn;
    unsigned int i;

    if (vi->fid == 0 || vi->fid > sbi->max_files)
        return -EINVAL;

    lbn = sbi->index_lbn + vi->fid - 1;

    bh = sb_bread(sb, lbn);
    if (!bh)
        return -EIO;

    fh = (struct vmsfs_file_header *)bh->b_data;

    /* Update fields from VFS inode */
    fh->fh_size = cpu_to_le64(inode->i_size);
    fh->fh_blocks = cpu_to_le32(inode->i_blocks);
    fh->fh_modified = cpu_to_le64(ktime_get_real_seconds());
    fh->fh_link_count = cpu_to_le16(inode->i_nlink);
    fh->fh_protection = cpu_to_le16(vi->vms_prot);

    /* Update retrieval pointers */
    fh->fh_map_count = cpu_to_le16(vi->map_count);
    for (i = 0; i < vi->map_count && i < VMSFS_MAX_RETRIEVAL; i++) {
        fh->fh_map[i].rp_lbn = cpu_to_le32(vi->map[i].rp_lbn);
        fh->fh_map[i].rp_count = cpu_to_le32(vi->map[i].rp_count);
    }
    /* Zero remaining map slots */
    for (; i < VMSFS_MAX_RETRIEVAL; i++) {
        fh->fh_map[i].rp_lbn = 0;
        fh->fh_map[i].rp_count = 0;
    }

    /* Recompute checksum */
    fh->fh_checksum = 0;
    fh->fh_checksum = cpu_to_le32(vmsfs_checksum(fh, sizeof(*fh)));

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    return 0;
}

/* ================================================================
 * VBN -> LBN translation
 *
 * Virtual block numbers (VBN) are 1-based. Each retrieval pointer
 * maps a run of VBNs to LBNs. We walk the map to find the LBN
 * for a given VBN.
 * ================================================================ */

static int vmsfs_vbn_to_lbn(struct vmsfs_inode_info *vi, uint32_t vbn,
                            uint32_t *lbn_out)
{
    uint32_t cur_vbn = 1;
    unsigned int i;

    for (i = 0; i < vi->map_count; i++) {
        uint32_t count = vi->map[i].rp_count;

        if (vbn >= cur_vbn && vbn < cur_vbn + count) {
            *lbn_out = vi->map[i].rp_lbn + (vbn - cur_vbn);
            return 0;
        }
        cur_vbn += count;
    }

    return -EIO;  /* VBN not mapped */
}

/*
 * Compute the next unallocated VBN for a file (sum of all mapped VBNs + 1).
 */
static uint32_t vmsfs_next_vbn(struct vmsfs_inode_info *vi)
{
    uint32_t vbn = 1;
    unsigned int i;

    for (i = 0; i < vi->map_count; i++)
        vbn += vi->map[i].rp_count;
    return vbn;
}

/* ================================================================
 * Bitmap and home block write-back helpers
 * ================================================================ */

/*
 * Write a single bitmap block back to disk after modifying the in-memory
 * bitmap. bit_pos determines which bitmap block to write.
 */
static int vmsfs_write_bitmap_block(struct super_block *sb, uint32_t bit_pos)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    uint32_t block_idx = bit_pos / (VMSFS_BLOCK_SIZE * 8);
    uint32_t lbn = sbi->bitmap_lbn + block_idx;
    struct buffer_head *bh;

    if (block_idx >= sbi->bitmap_blocks)
        return -EINVAL;

    bh = sb_bread(sb, lbn);
    if (!bh)
        return -EIO;

    memcpy(bh->b_data,
           (char *)sbi->bitmap + block_idx * VMSFS_BLOCK_SIZE,
           VMSFS_BLOCK_SIZE);

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    return 0;
}

/*
 * Update the home block on disk with current free_blocks count.
 */
static int vmsfs_update_home_block(struct super_block *sb)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct buffer_head *bh;
    struct vmsfs_home_block *hb;

    bh = sb_bread(sb, VMSFS_HOME_LBN);
    if (!bh)
        return -EIO;

    hb = (struct vmsfs_home_block *)bh->b_data;
    hb->hb_free_blocks = cpu_to_le32(sbi->free_blocks);
    hb->hb_modified = cpu_to_le64(ktime_get_real_seconds());

    hb->hb_checksum = 0;
    hb->hb_checksum = cpu_to_le32(vmsfs_checksum(hb, sizeof(*hb)));

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    return 0;
}

/* ================================================================
 * Block allocation and free
 *
 * The storage bitmap has one bit per block on the volume.
 * Bit set = allocated, bit clear = free.
 * Caller must hold sbi->alloc_lock.
 * ================================================================ */

static int vmsfs_alloc_block(struct super_block *sb, uint32_t *lbn_out)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    uint32_t bit;
    int ret;

    if (sbi->free_blocks == 0)
        return -ENOSPC;

    bit = find_next_zero_bit(sbi->bitmap, sbi->total_blocks, sbi->data_lbn);
    if (bit >= sbi->total_blocks)
        return -ENOSPC;

    set_bit(bit, sbi->bitmap);
    sbi->free_blocks--;

    ret = vmsfs_write_bitmap_block(sb, bit);
    if (ret) {
        clear_bit(bit, sbi->bitmap);
        sbi->free_blocks++;
        return ret;
    }

    *lbn_out = bit;
    return 0;
}

static int vmsfs_free_block(struct super_block *sb, uint32_t lbn)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    int ret;

    if (lbn >= sbi->total_blocks)
        return -EINVAL;

    if (!test_bit(lbn, sbi->bitmap))
        return 0;  /* already free */

    clear_bit(lbn, sbi->bitmap);
    sbi->free_blocks++;

    ret = vmsfs_write_bitmap_block(sb, lbn);
    if (ret) {
        set_bit(lbn, sbi->bitmap);
        sbi->free_blocks--;
        return ret;
    }

    return 0;
}

/* ================================================================
 * FID allocation and free
 *
 * Scan the file header area for a free slot (no valid INUSE header).
 * Caller must hold sbi->alloc_lock.
 * ================================================================ */

static int vmsfs_alloc_fid(struct super_block *sb, uint32_t *fid_out)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    uint32_t fid;

    for (fid = VMSFS_FID_FIRST_USER; fid <= sbi->max_files; fid++) {
        struct buffer_head *bh;
        struct vmsfs_file_header *fh;
        uint32_t lbn = sbi->index_lbn + fid - 1;

        bh = sb_bread(sb, lbn);
        if (!bh)
            continue;

        fh = (struct vmsfs_file_header *)bh->b_data;

        if (le32_to_cpu(fh->fh_magic) != VMSFS_FH_MAGIC ||
            !(le16_to_cpu(fh->fh_flags) & VMSFS_FH_INUSE)) {
            brelse(bh);
            *fid_out = fid;
            return 0;
        }

        brelse(bh);
    }

    return -ENOSPC;
}

static int vmsfs_free_fid(struct super_block *sb, uint32_t fid)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct buffer_head *bh;
    uint32_t lbn;

    if (fid < VMSFS_FID_FIRST_USER || fid > sbi->max_files)
        return -EINVAL;

    lbn = sbi->index_lbn + fid - 1;
    bh = sb_bread(sb, lbn);
    if (!bh)
        return -EIO;

    memset(bh->b_data, 0, VMSFS_BLOCK_SIZE);
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    return 0;
}

/* ================================================================
 * Retrieval pointer map extension
 *
 * Adds a new block to a file's map. Tries contiguous extension of
 * the last pointer first; falls back to a new pointer entry.
 * ================================================================ */

static int vmsfs_extend_map(struct vmsfs_inode_info *vi, uint32_t lbn)
{
    /* Try contiguous extension of last pointer */
    if (vi->map_count > 0) {
        struct vmsfs_retrieval_ptr *last = &vi->map[vi->map_count - 1];

        if (lbn == last->rp_lbn + last->rp_count) {
            last->rp_count++;
            return 0;
        }
    }

    /* Need a new pointer slot */
    if (vi->map_count >= VMSFS_MAX_RETRIEVAL)
        return -ENOSPC;

    vi->map[vi->map_count].rp_lbn = lbn;
    vi->map[vi->map_count].rp_count = 1;
    vi->map_count++;
    return 0;
}

/*
 * Ensure the file has blocks allocated through the given VBN.
 * Allocates blocks to fill any gap between current allocation and target.
 * Caller must hold sbi->alloc_lock.
 */
static int vmsfs_ensure_blocks(struct super_block *sb, struct inode *inode,
                               uint32_t target_vbn)
{
    struct vmsfs_inode_info *vi = VMSFS_I(inode);
    uint32_t next;
    int ret;

    next = vmsfs_next_vbn(vi);

    while (next <= target_vbn) {
        uint32_t lbn;
        struct buffer_head *bh;

        ret = vmsfs_alloc_block(sb, &lbn);
        if (ret)
            return ret;

        ret = vmsfs_extend_map(vi, lbn);
        if (ret) {
            vmsfs_free_block(sb, lbn);
            return ret;
        }

        /* Zero newly allocated block */
        bh = sb_bread(sb, lbn);
        if (bh) {
            memset(bh->b_data, 0, VMSFS_BLOCK_SIZE);
            mark_buffer_dirty(bh);
            brelse(bh);
        }

        inode->i_blocks++;
        next++;
    }

    return 0;
}

/* ================================================================
 * Name helpers
 * ================================================================ */

/*
 * Split "FOO.TXT" into name="FOO" and type="TXT".
 * If no dot, type is empty string.
 */
static void vmsfs_split_name_type(const char *fullname,
                                  char *name, size_t name_size,
                                  char *type, size_t type_size)
{
    const char *dot = strrchr(fullname, '.');

    if (dot && dot != fullname) {
        size_t nlen = dot - fullname;

        if (nlen >= name_size)
            nlen = name_size - 1;
        memcpy(name, fullname, nlen);
        name[nlen] = '\0';
        strscpy(type, dot + 1, type_size);
    } else {
        strscpy(name, fullname, name_size);
        type[0] = '\0';
    }
}

/*
 * Uppercase a string in place. VMS/ODS-2 convention.
 */
static void vmsfs_strupper(char *s)
{
    while (*s) {
        *s = toupper(*s);
        s++;
    }
}

/* ================================================================
 * Name matching helper
 * ================================================================ */

/*
 * Case-insensitive comparison of a directory entry name against a target.
 */
static bool vmsfs_blkdev_name_match(const char *de_name, uint8_t de_name_len,
                                    const char *target, unsigned int target_len,
                                    int case_blind)
{
    if (de_name_len != target_len)
        return false;

    if (case_blind)
        return strncasecmp(de_name, target, target_len) == 0;
    else
        return memcmp(de_name, target, target_len) == 0;
}

/* ================================================================
 * Directory entry management
 * ================================================================ */

/*
 * Add a directory entry. Scans existing blocks for a free slot (de_fid==0),
 * or allocates a new data block if all slots are full.
 * Caller must hold sbi->alloc_lock.
 */
static int vmsfs_dir_add_entry(struct super_block *sb, struct inode *dir,
                               uint32_t fid, const char *name,
                               uint8_t name_len, uint16_t version)
{
    struct vmsfs_inode_info *dir_vi = VMSFS_I(dir);
    uint32_t vbn;
    int ret;

    /* Scan existing blocks for a free slot */
    for (vbn = 1; ; vbn++) {
        uint32_t lbn;
        struct buffer_head *bh;
        unsigned int j;

        ret = vmsfs_vbn_to_lbn(dir_vi, vbn, &lbn);
        if (ret)
            break;  /* no more mapped blocks */

        bh = sb_bread(sb, lbn);
        if (!bh)
            return -EIO;

        for (j = 0; j < VMSFS_DIR_PER_BLOCK; j++) {
            struct vmsfs_dir_entry *de;
            unsigned int off = j * VMSFS_DIR_ENTRY_SIZE;

            de = (struct vmsfs_dir_entry *)(bh->b_data + off);

            if (le32_to_cpu(de->de_fid) == 0) {
                /* Found free slot */
                memset(de, 0, VMSFS_DIR_ENTRY_SIZE);
                de->de_fid = cpu_to_le32(fid);
                de->de_version = cpu_to_le16(version);
                /* Clamp name_len to avoid overflowing de_name[] */
                if (name_len > sizeof(de->de_name) - 1)
                    name_len = sizeof(de->de_name) - 1;
                de->de_name_len = name_len;
                memcpy(de->de_name, name, name_len);
                de->de_name[name_len] = '\0';

                mark_buffer_dirty(bh);
                sync_dirty_buffer(bh);
                brelse(bh);
                return 0;
            }
        }

        brelse(bh);
    }

    /* No free slot found -- extend directory with a new data block */
    {
        uint32_t new_lbn;
        struct buffer_head *bh;
        struct vmsfs_dir_entry *de;

        ret = vmsfs_alloc_block(sb, &new_lbn);
        if (ret)
            return ret;

        ret = vmsfs_extend_map(dir_vi, new_lbn);
        if (ret) {
            vmsfs_free_block(sb, new_lbn);
            return ret;
        }

        dir->i_blocks++;
        dir->i_size += VMSFS_BLOCK_SIZE;

        ret = vmsfs_blkdev_flush_inode(sb, dir);
        if (ret)
            return ret;

        /* Zero the new block and write first entry */
        bh = sb_bread(sb, new_lbn);
        if (!bh)
            return -EIO;

        memset(bh->b_data, 0, VMSFS_BLOCK_SIZE);
        de = (struct vmsfs_dir_entry *)bh->b_data;
        de->de_fid = cpu_to_le32(fid);
        de->de_version = cpu_to_le16(version);
        /* Clamp name_len to avoid overflowing de_name[] */
        if (name_len > sizeof(de->de_name) - 1)
            name_len = sizeof(de->de_name) - 1;
        de->de_name_len = name_len;
        memcpy(de->de_name, name, name_len);
        de->de_name[name_len] = '\0';

        mark_buffer_dirty(bh);
        sync_dirty_buffer(bh);
        brelse(bh);
        return 0;
    }
}

/*
 * Remove a directory entry by FID and version.
 * If version == 0, removes the first entry matching the FID.
 */
static int vmsfs_dir_remove_entry(struct super_block *sb, struct inode *dir,
                                  uint32_t fid, uint16_t version)
{
    struct vmsfs_inode_info *dir_vi = VMSFS_I(dir);
    uint32_t vbn;
    int ret;

    for (vbn = 1; ; vbn++) {
        uint32_t lbn;
        struct buffer_head *bh;
        unsigned int j;

        ret = vmsfs_vbn_to_lbn(dir_vi, vbn, &lbn);
        if (ret)
            return -ENOENT;

        bh = sb_bread(sb, lbn);
        if (!bh)
            return -EIO;

        for (j = 0; j < VMSFS_DIR_PER_BLOCK; j++) {
            struct vmsfs_dir_entry *de;
            unsigned int off = j * VMSFS_DIR_ENTRY_SIZE;

            de = (struct vmsfs_dir_entry *)(bh->b_data + off);

            if (le32_to_cpu(de->de_fid) == fid &&
                (version == 0 ||
                 le16_to_cpu(de->de_version) == version)) {
                memset(de, 0, VMSFS_DIR_ENTRY_SIZE);
                mark_buffer_dirty(bh);
                sync_dirty_buffer(bh);
                brelse(bh);
                return 0;
            }
        }

        brelse(bh);
    }
}

/*
 * Find the highest version of a file in a directory by base name.
 * Returns 0 if no matching entry found.
 */
static uint16_t vmsfs_blkdev_highest_version(struct super_block *sb,
                                              struct inode *dir,
                                              const char *base_name,
                                              size_t base_len)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct vmsfs_inode_info *dir_vi = VMSFS_I(dir);
    uint16_t highest = 0;
    uint32_t vbn;
    int ret;

    for (vbn = 1; ; vbn++) {
        uint32_t lbn;
        struct buffer_head *bh;
        unsigned int j;

        ret = vmsfs_vbn_to_lbn(dir_vi, vbn, &lbn);
        if (ret)
            break;

        bh = sb_bread(sb, lbn);
        if (!bh)
            break;

        for (j = 0; j < VMSFS_DIR_PER_BLOCK; j++) {
            struct vmsfs_dir_entry *de;
            unsigned int off = j * VMSFS_DIR_ENTRY_SIZE;
            uint16_t de_ver;

            de = (struct vmsfs_dir_entry *)(bh->b_data + off);

            if (le32_to_cpu(de->de_fid) == 0)
                continue;

            de_ver = le16_to_cpu(de->de_version);

            if (vmsfs_blkdev_name_match(
                    de->de_name, de->de_name_len,
                    base_name, base_len,
                    sbi->opts.case_blind)) {
                if (de_ver > highest)
                    highest = de_ver;
            }
        }

        brelse(bh);
    }

    return highest;
}

/* ================================================================
 * Directory lookup -- scan directory data blocks
 * ================================================================ */

/*
 * vmsfs_blkdev_resolve - Resolve a (possibly versioned) name to a FID.
 *
 * Scans the directory's data blocks (via retrieval pointers) for an
 * entry matching @name. Handles version resolution:
 *   "FOO.TXT"    -> highest version
 *   "FOO.TXT;0"  -> highest version
 *   "FOO.TXT;3"  -> exact version 3
 *
 * Returns 0 on success and stores the FID in *fid_out; *fid_out is 0 when
 * the directory holds no entry for @name. Returns a negative errno only when
 * @name is not a parseable VMS filename.
 *
 * Factored out of vmsfs_blkdev_lookup() so that ->d_revalidate can ask the
 * SAME question a lookup would ask ("what does this name resolve to right
 * now?") without allocating a dentry -- see vmsfs_d_revalidate() in
 * vmsfs_inode.c.
 */
int vmsfs_blkdev_resolve(struct inode *dir, const char *name,
                         uint32_t *fid_out)
{
    struct super_block *sb = dir->i_sb;
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct vmsfs_inode_info *dir_vi = VMSFS_I(dir);
    char base[VMSFS_MAX_FILENAME + 1];
    int req_version;
    int ret;
    uint32_t vbn;
    uint32_t best_fid = 0;
    uint16_t best_version = 0;

    *fid_out = 0;

    /* Parse requested name for version */
    ret = vmsfs_parse_version(name, base, sizeof(base), &req_version);
    if (ret)
        return ret;

    /* Scan all data blocks of this directory */
    for (vbn = 1; ; vbn++) {
        uint32_t lbn;
        struct buffer_head *bh;
        unsigned int j;

        ret = vmsfs_vbn_to_lbn(dir_vi, vbn, &lbn);
        if (ret)
            break;  /* no more mapped blocks */

        bh = sb_bread(sb, lbn);
        if (!bh)
            break;

        /* Scan entries in this block */
        for (j = 0; j < VMSFS_DIR_PER_BLOCK; j++) {
            struct vmsfs_dir_entry *de;
            unsigned int off = j * VMSFS_DIR_ENTRY_SIZE;
            uint32_t de_fid;
            uint16_t de_ver;
            char de_base[VMSFS_MAX_FILENAME + 1];
            int de_parsed_ver;

            if (off + VMSFS_DIR_ENTRY_SIZE > VMSFS_BLOCK_SIZE)
                break;

            de = (struct vmsfs_dir_entry *)(bh->b_data + off);
            de_fid = le32_to_cpu(de->de_fid);

            if (de_fid == 0)
                continue;  /* free slot */

            de_ver = le16_to_cpu(de->de_version);

            /*
             * de_name contains "NAME.TYPE". Parse out the base
             * for comparison. For directories, de_name has no
             * semicolon or version.
             */
            ret = vmsfs_parse_version(de->de_name, de_base,
                                      sizeof(de_base), &de_parsed_ver);
            if (ret) {
                /* If parse fails, try direct comparison */
                if (vmsfs_blkdev_name_match(
                        de->de_name, de->de_name_len,
                        base, strlen(base),
                        sbi->opts.case_blind)) {
                    best_fid = de_fid;
                    best_version = de_ver;
                    if (req_version != 0) {
                        brelse(bh);
                        goto found;
                    }
                }
                continue;
            }

            /* Compare base names */
            if (!vmsfs_blkdev_name_match(
                    de_base, strlen(de_base),
                    base, strlen(base),
                    sbi->opts.case_blind))
                continue;

            /* Version matching */
            if (req_version != 0) {
                /* Exact version requested */
                if (de_ver == req_version) {
                    best_fid = de_fid;
                    best_version = de_ver;
                    brelse(bh);
                    goto found;
                }
            } else {
                /* Find highest version */
                if (de_ver > best_version || best_fid == 0) {
                    best_fid = de_fid;
                    best_version = de_ver;
                }
            }
        }

        brelse(bh);
    }

found:
    *fid_out = best_fid;
    (void)best_version;
    return 0;
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

        ret = vmsfs_vbn_to_lbn(dir_vi, vbn, &lbn);
        if (ret)
            break;

        bh = sb_bread(sb, lbn);
        if (!bh)
            break;

        for (j = 0; j < VMSFS_DIR_PER_BLOCK; j++) {
            struct vmsfs_dir_entry *de;
            unsigned int off = j * VMSFS_DIR_ENTRY_SIZE;
            uint32_t de_fid;
            uint16_t de_ver;
            char fullname[VMSFS_MAX_FILENAME + 1];
            int namelen;
            unsigned int d_type;

            if (off + VMSFS_DIR_ENTRY_SIZE > VMSFS_BLOCK_SIZE)
                break;

            de = (struct vmsfs_dir_entry *)(bh->b_data + off);
            de_fid = le32_to_cpu(de->de_fid);

            /* Always count every slot for stable positioning */
            if (de_fid == 0) {
                entry_pos++;
                continue;
            }

            /* Skip entries already emitted in previous calls */
            if (entry_pos < ctx->pos) {
                entry_pos++;
                continue;
            }

            de_ver = le16_to_cpu(de->de_version);

            /*
             * Build the display name. For versioned files,
             * append ";version". For directories (version 0),
             * show just the name.
             */
            if (de_ver > 0) {
                namelen = snprintf(fullname, sizeof(fullname),
                                   "%.*s;%u",
                                   de->de_name_len, de->de_name,
                                   de_ver);
                d_type = DT_REG;
            } else {
                namelen = snprintf(fullname, sizeof(fullname),
                                   "%.*s",
                                   de->de_name_len, de->de_name);
                d_type = DT_DIR;
            }

            if (namelen <= 0 || namelen >= (int)sizeof(fullname)) {
                entry_pos++;
                continue;
            }

            if (!dir_emit(ctx, fullname, namelen, de_fid, d_type)) {
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
    struct inode *inode;
    char base[VMSFS_MAX_FILENAME + 1];
    char fname[VMSFS_MAX_NAME + 1];
    char ftype[VMSFS_MAX_TYPE + 1];
    int req_version;
    uint16_t highest, new_version;
    uint32_t fid, lbn;
    struct buffer_head *bh;
    struct vmsfs_file_header *fh;
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
        highest = vmsfs_blkdev_highest_version(sb, dir, base, strlen(base));
        new_version = highest + 1;
        if (new_version > VMSFS_VERSION_MAX) {
            mutex_unlock(&sbi->alloc_lock);
            return -ENOSPC;
        }
    }

    /* Allocate FID */
    ret = vmsfs_alloc_fid(sb, &fid);
    if (ret) {
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    /* Initialize file header on disk */
    lbn = sbi->index_lbn + fid - 1;
    bh = sb_bread(sb, lbn);
    if (!bh) {
        mutex_unlock(&sbi->alloc_lock);
        return -EIO;
    }

    fh = (struct vmsfs_file_header *)bh->b_data;
    memset(fh, 0, sizeof(*fh));
    fh->fh_magic = cpu_to_le32(VMSFS_FH_MAGIC);
    fh->fh_fid = cpu_to_le32(fid);
    fh->fh_size = 0;
    fh->fh_created = cpu_to_le64(ktime_get_real_seconds());
    fh->fh_modified = fh->fh_created;
    fh->fh_accessed = fh->fh_created;
    fh->fh_blocks = 0;
    fh->fh_parent_fid = cpu_to_le32(VMSFS_I(dir)->fid);
    fh->fh_flags = cpu_to_le16(VMSFS_FH_INUSE);
    fh->fh_version = cpu_to_le16(new_version);
    fh->fh_protection = cpu_to_le16(VMSFS_PROT_DEFAULT);
    {
        uint16_t uic_member, uic_group;

        vmsfs_current_uic(&uic_member, &uic_group);
        fh->fh_uic_member = cpu_to_le16(uic_member);
        fh->fh_uic_group = cpu_to_le16(uic_group);
    }
    fh->fh_link_count = cpu_to_le16(1);
    strscpy(fh->fh_name, fname, sizeof(fh->fh_name));
    strscpy(fh->fh_type, ftype, sizeof(fh->fh_type));

    fh->fh_checksum = 0;
    fh->fh_checksum = cpu_to_le32(vmsfs_checksum(fh, sizeof(*fh)));

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    /* Add directory entry */
    ret = vmsfs_dir_add_entry(sb, dir, fid, base, strlen(base), new_version);
    if (ret) {
        vmsfs_free_fid(sb, fid);
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    vmsfs_update_home_block(sb);
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
    struct inode *inode;
    const char *name = dentry->d_name.name;
    char uname[VMSFS_MAX_FILENAME + 1];
    char fname[VMSFS_MAX_NAME + 1];
    char ftype[VMSFS_MAX_TYPE + 1];
    uint32_t fid, data_lbn, lbn;
    struct buffer_head *bh;
    struct vmsfs_file_header *fh;
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

    ret = vmsfs_alloc_fid(sb, &fid);
    if (ret) {
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    /* Allocate initial data block for directory entries */
    ret = vmsfs_alloc_block(sb, &data_lbn);
    if (ret) {
        vmsfs_free_fid(sb, fid);
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    /* Zero the data block */
    bh = sb_bread(sb, data_lbn);
    if (!bh) {
        vmsfs_free_block(sb, data_lbn);
        vmsfs_free_fid(sb, fid);
        mutex_unlock(&sbi->alloc_lock);
        return -EIO;
    }
    memset(bh->b_data, 0, VMSFS_BLOCK_SIZE);
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    /* Initialize file header on disk */
    lbn = sbi->index_lbn + fid - 1;
    bh = sb_bread(sb, lbn);
    if (!bh) {
        vmsfs_free_block(sb, data_lbn);
        vmsfs_free_fid(sb, fid);
        mutex_unlock(&sbi->alloc_lock);
        return -EIO;
    }

    fh = (struct vmsfs_file_header *)bh->b_data;
    memset(fh, 0, sizeof(*fh));
    fh->fh_magic = cpu_to_le32(VMSFS_FH_MAGIC);
    fh->fh_fid = cpu_to_le32(fid);
    fh->fh_size = cpu_to_le64(VMSFS_BLOCK_SIZE);
    fh->fh_created = cpu_to_le64(ktime_get_real_seconds());
    fh->fh_modified = fh->fh_created;
    fh->fh_accessed = fh->fh_created;
    fh->fh_blocks = cpu_to_le32(1);
    fh->fh_parent_fid = cpu_to_le32(VMSFS_I(dir)->fid);
    fh->fh_flags = cpu_to_le16(VMSFS_FH_INUSE | VMSFS_FH_DIRECTORY);
    fh->fh_version = 0;  /* directories are unversioned */
    fh->fh_protection = cpu_to_le16(VMSFS_PROT_DEFAULT);
    {
        uint16_t uic_member, uic_group;

        vmsfs_current_uic(&uic_member, &uic_group);
        fh->fh_uic_member = cpu_to_le16(uic_member);
        fh->fh_uic_group = cpu_to_le16(uic_group);
    }
    fh->fh_link_count = cpu_to_le16(2);
    fh->fh_map_count = cpu_to_le16(1);
    strscpy(fh->fh_name, fname, sizeof(fh->fh_name));
    strscpy(fh->fh_type, ftype, sizeof(fh->fh_type));
    fh->fh_map[0].rp_lbn = cpu_to_le32(data_lbn);
    fh->fh_map[0].rp_count = cpu_to_le32(1);

    fh->fh_checksum = 0;
    fh->fh_checksum = cpu_to_le32(vmsfs_checksum(fh, sizeof(*fh)));

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    /* Add entry to parent directory */
    ret = vmsfs_dir_add_entry(sb, dir, fid, uname, strlen(uname), 0);
    if (ret) {
        vmsfs_free_block(sb, data_lbn);
        vmsfs_free_fid(sb, fid);
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    inc_nlink(dir);
    vmsfs_blkdev_flush_inode(sb, dir);
    vmsfs_update_home_block(sb);
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
    unsigned int i;
    uint32_t j;
    int ret;

    mutex_lock(&sbi->alloc_lock);

    /* Remove directory entry from parent */
    ret = vmsfs_dir_remove_entry(sb, dir, vi->fid, vi->version);
    if (ret) {
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    /* Free all data blocks */
    for (i = 0; i < vi->map_count; i++) {
        for (j = 0; j < vi->map[i].rp_count; j++)
            vmsfs_free_block(sb, vi->map[i].rp_lbn + j);
    }

    /* Free the file header */
    vmsfs_free_fid(sb, vi->fid);

    drop_nlink(inode);
    vmsfs_update_home_block(sb);
    mutex_unlock(&sbi->alloc_lock);

    return 0;
}

static int vmsfs_blkdev_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode = d_inode(dentry);
    struct vmsfs_inode_info *dir_vi = VMSFS_I(inode);
    uint32_t vbn;
    int ret;

    /* Check directory is empty */
    for (vbn = 1; ; vbn++) {
        uint32_t lbn;
        struct buffer_head *bh;
        unsigned int j;

        ret = vmsfs_vbn_to_lbn(dir_vi, vbn, &lbn);
        if (ret)
            break;

        bh = sb_bread(inode->i_sb, lbn);
        if (!bh)
            break;

        for (j = 0; j < VMSFS_DIR_PER_BLOCK; j++) {
            struct vmsfs_dir_entry *de;
            unsigned int off = j * VMSFS_DIR_ENTRY_SIZE;

            de = (struct vmsfs_dir_entry *)(bh->b_data + off);
            if (le32_to_cpu(de->de_fid) != 0) {
                brelse(bh);
                return -ENOTEMPTY;
            }
        }

        brelse(bh);
    }

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
    struct vmsfs_file_header *fh;
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
        uint32_t vbn;

        for (vbn = 1; ; vbn++) {
            uint32_t tlbn;
            struct buffer_head *tbh;
            unsigned int j;

            if (vmsfs_vbn_to_lbn(tvi, vbn, &tlbn))
                break;
            tbh = sb_bread(sb, tlbn);
            if (!tbh)
                break;
            for (j = 0; j < VMSFS_DIR_PER_BLOCK; j++) {
                struct vmsfs_dir_entry *de =
                    (struct vmsfs_dir_entry *)(tbh->b_data +
                                               j * VMSFS_DIR_ENTRY_SIZE);
                if (le32_to_cpu(de->de_fid) != 0) {
                    brelse(tbh);
                    mutex_unlock(&sbi->alloc_lock);
                    return -ENOTEMPTY;
                }
            }
            brelse(tbh);
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
            uint16_t highest = vmsfs_blkdev_highest_version(sb, new_dir, base,
                                                            strlen(base));
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
        unsigned int i;
        uint32_t j;

        ret = vmsfs_dir_remove_entry(sb, new_dir, tvi->fid, tvi->version);
        if (ret) {
            mutex_unlock(&sbi->alloc_lock);
            return ret;
        }
        for (i = 0; i < tvi->map_count; i++)
            for (j = 0; j < tvi->map[i].rp_count; j++)
                vmsfs_free_block(sb, tvi->map[i].rp_lbn + j);
        vmsfs_free_fid(sb, tvi->fid);
        clear_nlink(new_inode);
        if (target_was_dir)
            drop_nlink(new_dir);
    }

    /* (b) Remove the source's old directory entry. */
    ret = vmsfs_dir_remove_entry(sb, old_dir, old_vi->fid, old_vi->version);
    if (ret) {
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    /* (c) Add the new directory entry pointing at the SAME FID. */
    ret = vmsfs_dir_add_entry(sb, new_dir, old_vi->fid, base, strlen(base),
                              new_version);
    if (ret) {
        /* Best-effort rollback so a full new_dir cannot orphan the file. */
        vmsfs_dir_add_entry(sb, old_dir, old_vi->fid, old_full,
                            strlen(old_full), old_vi->version);
        mutex_unlock(&sbi->alloc_lock);
        return ret;
    }

    /* (d) Rewrite the file header's name/type/version/parent on disk. */
    lbn = sbi->index_lbn + old_vi->fid - 1;
    bh = sb_bread(sb, lbn);
    if (bh) {
        fh = (struct vmsfs_file_header *)bh->b_data;
        memset(fh->fh_name, 0, sizeof(fh->fh_name));
        memset(fh->fh_type, 0, sizeof(fh->fh_type));
        strscpy(fh->fh_name, fname, sizeof(fh->fh_name));
        strscpy(fh->fh_type, ftype, sizeof(fh->fh_type));
        fh->fh_version = cpu_to_le16(new_version);
        fh->fh_parent_fid = cpu_to_le32(VMSFS_I(new_dir)->fid);
        fh->fh_modified = cpu_to_le64(ktime_get_real_seconds());
        fh->fh_checksum = 0;
        fh->fh_checksum = cpu_to_le32(vmsfs_checksum(fh, sizeof(*fh)));
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
     * vmsfs_blkdev_resolve match on fid+version / parsed name).
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
    vmsfs_update_home_block(sb);

    mutex_unlock(&sbi->alloc_lock);

    inode_set_ctime(old_inode, ktime_get_real_seconds(), 0);
    return 0;
}
