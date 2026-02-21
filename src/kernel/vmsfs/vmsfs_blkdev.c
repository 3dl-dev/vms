// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_blkdev.c - Block-device mode operations for vmsfs
 *
 * Implements read-only filesystem operations for mounting a block device
 * formatted with the VMSFS on-disk format (vmsfs_ondisk.h).
 *
 * Operations:
 *   - vmsfs_blkdev_iget():  read file header by FID → populate VFS inode
 *   - lookup:  scan directory data blocks for entries
 *   - readdir: iterate directory data blocks, emit entries
 *   - read:    VBN→LBN translation via retrieval pointers, sb_bread()
 *
 * OVMX Project - Phase 7: Block-device vmsfs
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/stat.h>

#include "vmsfs.h"

/* ================================================================
 * File header → VFS inode
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

    /* UIC → uid/gid (simplified mapping) */
    inode->i_uid = KUIDT_INIT(le16_to_cpu(fh->fh_uic_member));
    inode->i_gid = KGIDT_INIT(le16_to_cpu(fh->fh_uic_group));

    set_nlink(inode, le16_to_cpu(fh->fh_link_count));
    if (inode->i_nlink == 0)
        set_nlink(inode, 1);

    /* Set mode based on directory flag */
    if (le16_to_cpu(fh->fh_flags) & VMSFS_FH_DIRECTORY) {
        inode->i_mode = S_IFDIR | 0755;
        inode->i_op = &vmsfs_blkdev_dir_iops;
        inode->i_fop = &vmsfs_blkdev_dir_fops;
        set_nlink(inode, 2);
    } else {
        inode->i_mode = S_IFREG | 0644;
        inode->i_op = &vmsfs_blkdev_file_iops;
        inode->i_fop = &vmsfs_blkdev_file_fops;
    }

    brelse(bh);
    unlock_new_inode(inode);
    return inode;
}

/* ================================================================
 * VBN → LBN translation
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

/* ================================================================
 * Directory lookup — scan directory data blocks
 * ================================================================ */

/*
 * Case-insensitive comparison of a directory entry name against a target.
 * The entry name is stored as "NAME.TYPE" in de_name.
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

/*
 * vmsfs_blkdev_lookup - Look up a file in a block-device directory.
 *
 * Scans the directory's data blocks (via retrieval pointers) for an
 * entry matching the requested name. Handles version resolution:
 *   "FOO.TXT"    → highest version
 *   "FOO.TXT;0"  → highest version
 *   "FOO.TXT;3"  → exact version 3
 */
static struct dentry *vmsfs_blkdev_lookup(struct inode *dir,
                                          struct dentry *dentry,
                                          unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct vmsfs_inode_info *dir_vi = VMSFS_I(dir);
    char base[VMSFS_MAX_FILENAME + 1];
    int req_version;
    int ret;
    struct inode *inode = NULL;
    uint32_t vbn;
    uint32_t best_fid = 0;
    uint16_t best_version = 0;

    /* Parse requested name for version */
    ret = vmsfs_parse_version(dentry->d_name.name, base, sizeof(base),
                              &req_version);
    if (ret)
        return ERR_PTR(ret);

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
    if (best_fid == 0) {
        /* Not found — negative dentry */
        return d_splice_alias(NULL, dentry);
    }

    inode = vmsfs_blkdev_iget(sb, best_fid);
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

            if (de_fid == 0)
                continue;

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
 * File read (read_iter via VBN→LBN translation)
 * ================================================================ */

static ssize_t vmsfs_blkdev_read_iter(struct kiocb *iocb,
                                      struct iov_iter *to)
{
    struct inode *inode = file_inode(iocb->ki_filp);
    struct super_block *sb = inode->i_sb;
    struct vmsfs_inode_info *vi = VMSFS_I(inode);
    loff_t pos = iocb->ki_pos;
    loff_t size = i_size_read(inode);
    ssize_t total = 0;

    if (pos >= size)
        return 0;

    while (iov_iter_count(to) > 0 && pos < size) {
        uint32_t vbn;
        uint32_t offset_in_block;
        uint32_t lbn;
        struct buffer_head *bh;
        size_t bytes;
        int ret;

        /* Compute VBN (1-based) and offset within block */
        vbn = (pos / VMSFS_BLOCK_SIZE) + 1;
        offset_in_block = pos % VMSFS_BLOCK_SIZE;

        /* Translate VBN → LBN */
        ret = vmsfs_vbn_to_lbn(vi, vbn, &lbn);
        if (ret)
            break;

        bh = sb_bread(sb, lbn);
        if (!bh)
            break;

        /* How many bytes to copy from this block */
        bytes = VMSFS_BLOCK_SIZE - offset_in_block;
        if (pos + bytes > size)
            bytes = size - pos;
        if (bytes > iov_iter_count(to))
            bytes = iov_iter_count(to);

        if (copy_to_iter(bh->b_data + offset_in_block, bytes, to)
            != bytes) {
            brelse(bh);
            if (total == 0)
                total = -EFAULT;
            break;
        }

        brelse(bh);
        pos += bytes;
        total += bytes;
    }

    iocb->ki_pos = pos;
    return total;
}

static loff_t vmsfs_blkdev_llseek(struct file *file, loff_t offset,
                                  int whence)
{
    return generic_file_llseek(file, offset, whence);
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
 * Operations tables
 * ================================================================ */

const struct inode_operations vmsfs_blkdev_dir_iops = {
    .lookup  = vmsfs_blkdev_lookup,
    .getattr = vmsfs_blkdev_getattr,
};

const struct file_operations vmsfs_blkdev_dir_fops = {
    .owner          = THIS_MODULE,
    .read           = generic_read_dir,
    .iterate_shared = vmsfs_blkdev_iterate,
    .llseek         = generic_file_llseek,
};

const struct inode_operations vmsfs_blkdev_file_iops = {
    .getattr = vmsfs_blkdev_getattr,
};

const struct file_operations vmsfs_blkdev_file_fops = {
    .owner     = THIS_MODULE,
    .read_iter = vmsfs_blkdev_read_iter,
    .llseek    = vmsfs_blkdev_llseek,
};
