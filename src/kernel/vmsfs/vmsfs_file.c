// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_file.c - File operations for vmsfs
 *
 * File read/write is delegated to the backing file on the host filesystem.
 * When a vmsfs file is opened, the corresponding backing file (with its
 * versioned name, e.g., "FOO.TXT;3") is opened and stored in
 * file->private_data.  All subsequent I/O operations are forwarded to
 * the backing file.
 *
 * OVMX Project - Phase 4b: Kernel-native VMS Filesystem
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uio.h>
#include <linux/file.h>
#include <linux/ctype.h>

#include "vmsfs.h"

/* Forward declaration - defined later in this file */
static int vmsfs_scan_highest_from_sbi(struct vmsfs_sb_info *sbi,
                                       const char *base);

/*
 * Build the full path to the backing file from the superblock's backing
 * path and the versioned filename.
 */
static int vmsfs_build_backing_path(struct vmsfs_sb_info *sbi,
                                    const char *versioned_name,
                                    char *buf, size_t buf_size)
{
    int n;

    n = snprintf(buf, buf_size, "%s/%s", sbi->opts.backing_path,
                 versioned_name);
    if (n < 0 || (size_t)n >= buf_size)
        return -ENAMETOOLONG;

    return 0;
}

/*
 * vmsfs_open - Open the corresponding backing file.
 *
 * Resolves the versioned filename from the inode's metadata, constructs
 * the full backing path, opens the backing file, and stores the file
 * pointer in file->private_data.
 */
static int vmsfs_open(struct inode *inode, struct file *file)
{
    struct vmsfs_inode_info *vi = VMSFS_I(inode);
    struct vmsfs_sb_info *sbi = VMSFS_SB(inode->i_sb);
    char versioned[VMSFS_MAX_FILENAME + 1];
    char *backing_path;
    struct file *backing_file;
    int ret;
    int version;

    version = vi->version;
    if (version <= 0) {
        /*
         * Shouldn't happen for regular files, but if the version is
         * unresolved, try to resolve it now.
         */
        version = vmsfs_scan_highest_from_sbi(sbi, vi->base_name);
        if (version <= 0)
            return -ENOENT;
    }

    /* Build the versioned filename */
    ret = vmsfs_build_versioned_name(vi->base_name, version,
                                     versioned, sizeof(versioned));
    if (ret)
        return ret;

    /* Allocate backing path on the heap to avoid stack overflow */
    backing_path = kmalloc(PATH_MAX, GFP_KERNEL);
    if (!backing_path)
        return -ENOMEM;

    /* Build the full backing path */
    ret = vmsfs_build_backing_path(sbi, versioned,
                                   backing_path, PATH_MAX);
    if (ret) {
        kfree(backing_path);
        return ret;
    }

    /* Open the backing file with the same flags */
    backing_file = filp_open(backing_path, file->f_flags, 0);
    kfree(backing_path);

    if (IS_ERR(backing_file))
        return PTR_ERR(backing_file);

    file->private_data = backing_file;
    return 0;
}

/*
 * vmsfs_release - Close the backing file.
 */
static int vmsfs_release(struct inode *inode, struct file *file)
{
    struct file *backing_file = file->private_data;

    if (backing_file) {
        fput(backing_file);
        file->private_data = NULL;
    }

    return 0;
}

/*
 * vmsfs_read_iter - Read from the backing file.
 *
 * Delegates to the backing file's read_iter, which handles the actual
 * I/O against the host filesystem.
 */
static ssize_t vmsfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct file *backing_file = iocb->ki_filp->private_data;
    struct file *vmsfs_file = iocb->ki_filp;
    ssize_t ret;

    if (!backing_file || !backing_file->f_op->read_iter)
        return -EIO;

    /* Synchronize the position */
    backing_file->f_pos = vmsfs_file->f_pos;

    iocb->ki_filp = backing_file;
    ret = backing_file->f_op->read_iter(iocb, to);
    iocb->ki_filp = vmsfs_file;

    /* Update the vmsfs file position */
    vmsfs_file->f_pos = backing_file->f_pos;

    /* Update the vmsfs inode size if the backing file grew */
    if (ret > 0) {
        loff_t backing_size = i_size_read(file_inode(backing_file));
        if (backing_size > i_size_read(file_inode(vmsfs_file)))
            i_size_write(file_inode(vmsfs_file), backing_size);
    }

    return ret;
}

/*
 * vmsfs_write_iter - Write to the backing file.
 *
 * Delegates to the backing file's write_iter.  After writing, updates
 * the vmsfs inode's size to match the backing file.
 */
static ssize_t vmsfs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct file *backing_file = iocb->ki_filp->private_data;
    struct file *vmsfs_file = iocb->ki_filp;
    ssize_t ret;

    if (!backing_file || !backing_file->f_op->write_iter)
        return -EIO;

    /* Synchronize the position */
    backing_file->f_pos = vmsfs_file->f_pos;

    iocb->ki_filp = backing_file;
    ret = backing_file->f_op->write_iter(iocb, from);
    iocb->ki_filp = vmsfs_file;

    /* Update the vmsfs file position */
    vmsfs_file->f_pos = backing_file->f_pos;

    /* Update the vmsfs inode size from the backing file */
    if (ret > 0) {
        loff_t backing_size = i_size_read(file_inode(backing_file));
        i_size_write(file_inode(vmsfs_file), backing_size);
    }

    return ret;
}

/*
 * vmsfs_llseek - Seek in the backing file.
 */
static loff_t vmsfs_llseek(struct file *file, loff_t offset, int whence)
{
    struct file *backing_file = file->private_data;
    loff_t ret;

    if (!backing_file)
        return -EIO;

    ret = vfs_llseek(backing_file, offset, whence);
    if (ret >= 0)
        file->f_pos = ret;

    return ret;
}

/*
 * vmsfs_fsync - Sync the backing file to disk.
 */
static int vmsfs_fsync(struct file *file, loff_t start, loff_t end,
                       int datasync)
{
    struct file *backing_file = file->private_data;

    if (!backing_file)
        return -EIO;

    return vfs_fsync_range(backing_file, start, end, datasync);
}

/*
 * vmsfs_mmap - Memory-map the backing file.
 *
 * We delegate mmap to the backing file so that page cache I/O works
 * transparently through the host filesystem.
 */
static int vmsfs_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct file *backing_file = file->private_data;

    if (!backing_file || !backing_file->f_op->mmap)
        return -ENODEV;

    return backing_file->f_op->mmap(backing_file, vma);
}

/* ================================================================
 * Internal helper: scan highest version from sbi
 * ================================================================ */

/*
 * Scan the backing directory for the highest version of @base.
 * Uses the same iterate_dir approach as vmsfs_inode.c but accepts
 * the sbi directly (avoids circular dependency).
 */
struct vmsfs_file_scan_ctx {
    struct dir_context ctx;
    const char *base;
    size_t base_len;
    int highest;
    int case_blind;
};

static bool vmsfs_file_scan_actor(struct dir_context *ctx, const char *name,
                                  int namlen, loff_t offset, u64 ino,
                                  unsigned int d_type)
{
    struct vmsfs_file_scan_ctx *sctx =
        container_of(ctx, struct vmsfs_file_scan_ctx, ctx);
    const char *semi;
    const char *p;
    size_t entry_base_len;
    int ver;

    /* Find semicolon */
    semi = memchr(name, ';', namlen);
    if (!semi)
        return true;

    entry_base_len = semi - name;
    if (entry_base_len != sctx->base_len)
        return true;

    if (sctx->case_blind) {
        if (strncasecmp(name, sctx->base, sctx->base_len) != 0)
            return true;
    } else {
        if (memcmp(name, sctx->base, sctx->base_len) != 0)
            return true;
    }

    /* Parse version */
    p = semi + 1;
    ver = 0;
    while (p < name + namlen) {
        if (!isdigit((unsigned char)*p))
            return true;
        ver = ver * 10 + (*p - '0');
        if (ver > VMSFS_MAX_VERSION)
            return true;
        p++;
    }

    if (ver > sctx->highest)
        sctx->highest = ver;

    return true;
}

static int vmsfs_scan_highest_from_sbi(struct vmsfs_sb_info *sbi,
                                       const char *base)
{
    struct vmsfs_file_scan_ctx sctx = {
        .ctx.actor = vmsfs_file_scan_actor,
        .ctx.pos = 0,
        .base = base,
        .base_len = strlen(base),
        .highest = 0,
        .case_blind = sbi->opts.case_blind,
    };
    struct file *dir_file;

    dir_file = filp_open(sbi->opts.backing_path,
                         O_RDONLY | O_DIRECTORY, 0);
    if (IS_ERR(dir_file))
        return 0;

    iterate_dir(dir_file, &sctx.ctx);
    filp_close(dir_file, NULL);

    return sctx.highest;
}

/* ================================================================
 * File operations table
 * ================================================================ */

const struct file_operations vmsfs_file_fops = {
    .owner      = THIS_MODULE,
    .open       = vmsfs_open,
    .release    = vmsfs_release,
    .read_iter  = vmsfs_read_iter,
    .write_iter = vmsfs_write_iter,
    .llseek     = vmsfs_llseek,
    .fsync      = vmsfs_fsync,
    .mmap       = vmsfs_mmap,
};
