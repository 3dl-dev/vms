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

    /*
     * Forward EVERY backing entry -- including "." and ".." -- straight into
     * the caller's dir_context.  We deliberately forward the backing dots
     * rather than injecting our own via dir_emit_dots(): doing so keeps the
     * vmsfs directory cursor a 1:1 image of the backing directory's opaque
     * readdir cookie, and that 1:1 mapping is exactly what lets
     * vmsfs_iterate_shared() resume the scan mid-directory instead of
     * restarting from the beginning on every getdents() (see the header
     * comment there and rd vms-93a).
     *
     * The name is presented as-is from the backing store, so versioned files
     * appear as "FOO.TXT;1" etc.  A synthetic inode number is used because
     * vmsfs overlay inodes do not correspond to backing inode numbers; the
     * d_type is passed through from the backing filesystem.
     *
     * dir_emit() returns false when the caller's buffer is full.  That is the
     * NORMAL "come back next getdents()" signal, NOT an error: we stop
     * iterating and let the backing filesystem leave its cursor on the entry
     * that did not fit, so the next call re-emits that entry exactly once.
     * (The previous code turned a full buffer into -EFAULT, corrupting any
     * multi-getdents() listing.)
     */
    return dir_emit(rctx->vmsfs_ctx, name, namlen, ino ? ino : 2, d_type);
}

/*
 * vmsfs_iterate_shared - Read directory entries.
 *
 * Opens the backing directory and iterates its contents, forwarding each
 * entry (including "." and "..") to the caller.
 *
 * TERMINATION (rd vms-93a).  ctx->pos is the vmsfs directory file's
 * persistent cursor: iterate_dir() seeds it from file->f_pos on entry and
 * writes it back to file->f_pos on return, so it must be treated as a resume
 * cookie, not a throwaway counter.  The previous implementation ignored it and
 * re-opened + re-scanned the backing directory from position 0 on EVERY
 * getdents() call, so it always re-emitted the whole listing and getdents()
 * never reached EOF (never returned 0) -- readdir() spun forever, and any
 * cross-process open of a child name that had to list the directory hung with
 * it.  Here we seed the backing directory's f_pos from ctx->pos and adopt the
 * backing cookie it advances to as our own cursor, so each getdents() picks up
 * where the last left off and iteration reaches EOF after emitting each entry
 * exactly once, for every reader (mounter or not).
 *
 * For subdirectories, the backing path is constructed by appending the
 * directory name to the root backing path.  For the root directory of the
 * vmsfs mount, we use the backing path directly.
 *
 * TODO: For nested directories, build the full backing path by walking up the
 * dentry tree and constructing the corresponding backing path.
 */
static int vmsfs_iterate_shared(struct file *file, struct dir_context *ctx)
{
    struct super_block *sb = file_inode(file)->i_sb;
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct file *backing_dir;
    struct vmsfs_readdir_ctx rctx;
    int ret;

    /*
     * Open the backing directory for scanning.
     *
     * For the root directory, use sbi->opts.backing_path directly.
     * For subdirectories, we would need to construct the full backing
     * path.  In this initial implementation, we support the root
     * directory; subdirectory support will be extended as the path
     * resolution layer matures.
     */
    backing_dir = filp_open(sbi->opts.backing_path,
                            O_RDONLY | O_DIRECTORY, 0);
    if (IS_ERR(backing_dir))
        return PTR_ERR(backing_dir);

    /*
     * Resume the backing scan where the previous getdents() left off.
     * iterate_dir() reads the starting cookie from backing_dir->f_pos (it
     * overwrites rctx.backing_ctx.pos with it), so seeding f_pos is what makes
     * the resume take effect.
     */
    backing_dir->f_pos = ctx->pos;

    rctx.backing_ctx.actor = vmsfs_readdir_actor;
    rctx.backing_ctx.pos = ctx->pos;
    rctx.vmsfs_ctx = ctx;

    /* Iterate the backing directory */
    ret = iterate_dir(backing_dir, &rctx.backing_ctx);

    /* Adopt the advanced backing cookie as our own cursor for next time. */
    ctx->pos = rctx.backing_ctx.pos;

    filp_close(backing_dir, NULL);

    return ret < 0 ? ret : 0;
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
