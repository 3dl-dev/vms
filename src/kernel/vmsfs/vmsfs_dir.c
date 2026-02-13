// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_dir.c - Directory operations for vmsfs
 *
 * Implements readdir (iterate_shared) for VMS-style directory listing.
 * Scans the backing directory and presents entries with their version
 * numbers intact, so userspace sees entries like "FOO.TXT;1", "FOO.TXT;2".
 *
 * The backing directory stores files with versioned names (FOO.TXT;N)
 * and directories without version suffixes.  Both are presented as-is
 * during directory iteration.
 *
 * OVMX Project - Phase 4b: Kernel-native VMS Filesystem
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "vmsfs.h"

/* ================================================================
 * Directory iteration
 *
 * vmsfs_iterate_shared() opens the backing directory, scans its
 * entries, and emits each one to the VFS via dir_emit().  Versioned
 * files appear with their full "NAME.TYPE;N" form.  Directories
 * appear without version suffixes.
 * ================================================================ */

/*
 * Context for the backing directory scan.
 * We iterate the backing dir using iterate_dir() and forward each
 * entry to the vmsfs dir_context.
 */
struct vmsfs_readdir_ctx {
    struct dir_context backing_ctx;  /* context for iterate_dir on backing */
    struct dir_context *vmsfs_ctx;   /* caller's dir_context to emit into */
    struct super_block *sb;
    int error;                       /* any error from dir_emit */
};

/*
 * Callback invoked by iterate_dir() on the backing directory.
 * For each backing entry, emit it into the vmsfs directory listing.
 */
static bool vmsfs_readdir_actor(struct dir_context *ctx, const char *name,
                                int namlen, loff_t offset, u64 ino,
                                unsigned int d_type)
{
    struct vmsfs_readdir_ctx *rctx =
        container_of(ctx, struct vmsfs_readdir_ctx, backing_ctx);

    /* Skip . and .. - the VFS handles these automatically */
    if (namlen == 1 && name[0] == '.')
        return true;
    if (namlen == 2 && name[0] == '.' && name[1] == '.')
        return true;

    /*
     * Emit the entry to the caller's dir_context.
     *
     * We use a synthetic inode number since vmsfs inodes don't
     * directly correspond to backing inode numbers.  The d_type
     * is passed through from the backing filesystem.
     *
     * The name is presented as-is from the backing store, which
     * means versioned files appear as "FOO.TXT;1" etc.
     */
    if (!dir_emit(rctx->vmsfs_ctx, name, namlen,
                  ino ? ino : 2, d_type)) {
        rctx->error = -EFAULT;
        return false;  /* stop iteration */
    }

    rctx->vmsfs_ctx->pos++;
    return true;  /* continue iteration */
}

/*
 * vmsfs_iterate_shared - Read directory entries.
 *
 * Opens the backing directory and iterates its contents, forwarding
 * each entry to the caller.  The "." and ".." entries are emitted
 * first (by the VFS), then the backing directory entries follow.
 *
 * For subdirectories, the backing path is constructed by appending
 * the directory name to the root backing path.  For the root directory
 * of the vmsfs mount, we use the backing path directly.
 */
static int vmsfs_iterate_shared(struct file *file, struct dir_context *ctx)
{
    struct super_block *sb = file_inode(file)->i_sb;
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct file *backing_dir;
    struct vmsfs_readdir_ctx rctx;

    /* Emit . and .. first */
    if (!dir_emit_dots(file, ctx))
        return 0;

    /*
     * Open the backing directory for scanning.
     *
     * For the root directory, use sbi->opts.backing_path directly.
     * For subdirectories, we would need to construct the full backing
     * path.  In this initial implementation, we support the root
     * directory; subdirectory support will be extended as the path
     * resolution layer matures.
     *
     * TODO: For nested directories, build the full backing path by
     * walking up the dentry tree and constructing the corresponding
     * backing path.
     */
    backing_dir = filp_open(sbi->opts.backing_path,
                            O_RDONLY | O_DIRECTORY, 0);
    if (IS_ERR(backing_dir))
        return PTR_ERR(backing_dir);

    /* Set up the readdir context */
    rctx.backing_ctx.actor = vmsfs_readdir_actor;
    rctx.backing_ctx.pos = 0;
    rctx.vmsfs_ctx = ctx;
    rctx.sb = sb;
    rctx.error = 0;

    /* Iterate the backing directory */
    iterate_dir(backing_dir, &rctx.backing_ctx);

    filp_close(backing_dir, NULL);

    return rctx.error;
}

/*
 * vmsfs_dir_open - Open a vmsfs directory.
 *
 * No special action needed; the backing dir is opened on iterate.
 */
static int vmsfs_dir_open(struct inode *inode, struct file *file)
{
    return 0;
}

/*
 * vmsfs_dir_release - Close a vmsfs directory.
 */
static int vmsfs_dir_release(struct inode *inode, struct file *file)
{
    return 0;
}

/*
 * vmsfs_dir_llseek - Seek within a directory.
 *
 * Supports simple sequential access and rewind to beginning.
 */
static loff_t vmsfs_dir_llseek(struct file *file, loff_t offset, int whence)
{
    return generic_file_llseek(file, offset, whence);
}

/* ================================================================
 * Directory file operations table
 * ================================================================ */

const struct file_operations vmsfs_dir_fops = {
    .owner          = THIS_MODULE,
    .open           = vmsfs_dir_open,
    .release        = vmsfs_dir_release,
    .read           = generic_read_dir,
    .iterate_shared = vmsfs_iterate_shared,
    .llseek         = vmsfs_dir_llseek,
};
