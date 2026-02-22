// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_super.c - Superblock, mount, and module init for vmsfs
 *
 * Registers the "vmsfs" filesystem type with the Linux VFS.
 * Supports two mount modes:
 *
 *   1. Overlay mode (backing=PATH):
 *      mount -t vmsfs -o backing=/var/vmsfs none /vms/sys$disk
 *
 *   2. Block-device mode:
 *      mount -t vmsfs /dev/vda /vms/sys$disk
 *
 * Mount options:
 *   backing=PATH      - backing directory path (overlay mode)
 *   version_limit=N   - max versions per file (default: 32767)
 *   case_blind=0|1    - case-insensitive lookup (default: 1)
 *
 * OVMX Project - Phase 4b/7: Kernel-native VMS Filesystem
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
#include <linux/parser.h>
#include <linux/namei.h>

#include "vmsfs.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OVMX Project");
MODULE_DESCRIPTION("VMS filesystem kernel module - versioning, case-insensitive");
MODULE_VERSION("1.0");

/* ================================================================
 * Mount option parsing
 * ================================================================ */

enum {
    Opt_backing,
    Opt_version_limit,
    Opt_case_blind,
    Opt_err,
};

static const match_table_t vmsfs_tokens = {
    { Opt_backing,       "backing=%s" },
    { Opt_version_limit, "version_limit=%d" },
    { Opt_case_blind,    "case_blind=%d" },
    { Opt_err,           NULL },
};

/*
 * Parse the mount options string into a vmsfs_mount_opts structure.
 * The "backing" option is mandatory.
 */
static int vmsfs_parse_options(char *options, struct vmsfs_mount_opts *opts)
{
    char *p;
    substring_t args[MAX_OPT_ARGS];
    int token;
    int intval;
    char *backing_str;

    /* Set defaults */
    opts->backing_path[0] = '\0';
    opts->version_limit = VMSFS_MAX_VERSION;
    opts->case_blind = 1;

    if (!options || !*options)
        return 0;  /* no options — block-device mode */

    while ((p = strsep(&options, ",")) != NULL) {
        if (!*p)
            continue;

        token = match_token(p, vmsfs_tokens, args);
        switch (token) {
        case Opt_backing:
            backing_str = match_strdup(&args[0]);
            if (!backing_str)
                return -ENOMEM;
            strscpy(opts->backing_path, backing_str, PATH_MAX);
            kfree(backing_str);
            break;

        case Opt_version_limit:
            if (match_int(&args[0], &intval))
                return -EINVAL;
            if (intval < 1 || intval > VMSFS_MAX_VERSION)
                return -EINVAL;
            opts->version_limit = intval;
            break;

        case Opt_case_blind:
            if (match_int(&args[0], &intval))
                return -EINVAL;
            opts->case_blind = !!intval;
            break;

        default:
            pr_err("vmsfs: unrecognized mount option: %s\n", p);
            return -EINVAL;
        }
    }

    /* backing= is optional — if absent, block-device mode is used */
    return 0;
}

/* ================================================================
 * Superblock operations
 * ================================================================ */

static void vmsfs_put_super(struct super_block *sb)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);

    if (sbi) {
        if (sbi->mode == VMSFS_MODE_OVERLAY) {
            path_put(&sbi->backing);
        } else {
            kfree(sbi->home);
            kfree(sbi->bitmap);
        }
        kfree(sbi);
        sb->s_fs_info = NULL;
    }
}

static int vmsfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(dentry->d_sb);

    if (sbi->mode == VMSFS_MODE_OVERLAY) {
        struct kstatfs backing_stat;
        int ret;

        ret = vfs_statfs(&sbi->backing, &backing_stat);
        if (ret)
            return ret;

        *buf = backing_stat;
        buf->f_type = VMSFS_MAGIC;
        return 0;
    }

    /* Block-device mode — report from cached home block */
    buf->f_type = VMSFS_MAGIC;
    buf->f_bsize = VMSFS_BLOCK_SIZE;
    buf->f_blocks = sbi->total_blocks;
    buf->f_bfree = sbi->free_blocks;
    buf->f_bavail = sbi->free_blocks;
    buf->f_files = sbi->max_files;
    buf->f_ffree = 0;  /* TODO: count free file headers */
    buf->f_namelen = VMSFS_FULLNAME_MAX;
    return 0;
}

static int vmsfs_show_options(struct seq_file *m, struct dentry *root)
{
    struct vmsfs_sb_info *sbi = VMSFS_SB(root->d_sb);

    if (sbi->mode == VMSFS_MODE_OVERLAY)
        seq_printf(m, ",backing=%s", sbi->opts.backing_path);

    if (sbi->opts.version_limit != VMSFS_MAX_VERSION)
        seq_printf(m, ",version_limit=%d", sbi->opts.version_limit);

    if (!sbi->opts.case_blind)
        seq_puts(m, ",case_blind=0");

    return 0;
}

/* Forward declarations from vmsfs_inode.c */
extern struct inode *vmsfs_alloc_inode(struct super_block *sb);
extern void vmsfs_free_inode(struct inode *inode);

/*
 * Super operations table.
 * alloc_inode / free_inode are defined in vmsfs_inode.c and linked
 * directly since all objects are compiled into the same module.
 */
const struct super_operations vmsfs_sops = {
    .alloc_inode   = vmsfs_alloc_inode,
    .free_inode    = vmsfs_free_inode,
    .put_super     = vmsfs_put_super,
    .statfs        = vmsfs_statfs,
    .show_options  = vmsfs_show_options,
};

/* ================================================================
 * Superblock fill — overlay mode (called during mount_nodev)
 * ================================================================ */

static int vmsfs_fill_super_overlay(struct super_block *sb, void *data,
                                    int silent)
{
    struct vmsfs_sb_info *sbi;
    struct inode *root_inode;
    int ret;

    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi)
        return -ENOMEM;

    sb->s_fs_info = sbi;
    sbi->mode = VMSFS_MODE_OVERLAY;

    /* Parse mount options */
    ret = vmsfs_parse_options((char *)data, &sbi->opts);
    if (ret) {
        if (!silent)
            pr_err("vmsfs: failed to parse mount options\n");
        goto err_free_sbi;
    }

    /* Resolve the backing directory path */
    ret = kern_path(sbi->opts.backing_path, LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
                    &sbi->backing);
    if (ret) {
        if (!silent)
            pr_err("vmsfs: backing path '%s' not found: %d\n",
                   sbi->opts.backing_path, ret);
        goto err_free_sbi;
    }

    /* Verify the backing path is a directory */
    if (!d_is_dir(sbi->backing.dentry)) {
        if (!silent)
            pr_err("vmsfs: backing path '%s' is not a directory\n",
                   sbi->opts.backing_path);
        ret = -ENOTDIR;
        goto err_put_path;
    }

    /* Configure superblock */
    sb->s_magic = VMSFS_MAGIC;
    sb->s_op = &vmsfs_sops;
    sb->s_d_op = &vmsfs_dops;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_blocksize = PAGE_SIZE;
    sb->s_blocksize_bits = PAGE_SHIFT;
    sb->s_time_gran = 1;

    /* Create the root inode */
    root_inode = vmsfs_new_inode(sb, S_IFDIR | 0755);
    if (!root_inode) {
        ret = -ENOMEM;
        goto err_put_path;
    }

    root_inode->i_op = &vmsfs_dir_iops;
    root_inode->i_fop = &vmsfs_dir_fops;

    /* Set nlink to 2 for directory (. and ..) */
    set_nlink(root_inode, 2);

    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        ret = -ENOMEM;
        goto err_put_path;
    }

    pr_info("vmsfs: mounted with backing=%s version_limit=%d case_blind=%d\n",
            sbi->opts.backing_path, sbi->opts.version_limit,
            sbi->opts.case_blind);

    return 0;

err_put_path:
    path_put(&sbi->backing);
err_free_sbi:
    kfree(sbi);
    sb->s_fs_info = NULL;
    return ret;
}

/* ================================================================
 * Superblock fill — block-device mode (called during mount_bdev)
 * ================================================================ */

static int vmsfs_fill_super_blkdev(struct super_block *sb, void *data,
                                   int silent)
{
    struct vmsfs_sb_info *sbi;
    struct buffer_head *bh;
    struct vmsfs_home_block *hb;
    struct inode *root_inode;
    uint32_t cksum;
    unsigned int bm_bytes;
    unsigned int i;
    int ret;

    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi)
        return -ENOMEM;

    sb->s_fs_info = sbi;
    sbi->mode = VMSFS_MODE_BLKDEV;

    /* Parse options (version_limit, case_blind) */
    ret = vmsfs_parse_options((char *)data, &sbi->opts);
    if (ret) {
        if (!silent)
            pr_err("vmsfs: failed to parse mount options\n");
        goto err_free_sbi;
    }

    /* Set block size to 512 (VMSFS native block size) */
    if (!sb_set_blocksize(sb, VMSFS_BLOCK_SIZE)) {
        if (!silent)
            pr_err("vmsfs: unable to set block size to %d\n",
                   VMSFS_BLOCK_SIZE);
        ret = -EINVAL;
        goto err_free_sbi;
    }

    /* Read home block (block 1) */
    bh = sb_bread(sb, VMSFS_HOME_LBN);
    if (!bh) {
        if (!silent)
            pr_err("vmsfs: unable to read home block\n");
        ret = -EIO;
        goto err_free_sbi;
    }

    hb = (struct vmsfs_home_block *)bh->b_data;

    /* Validate magic */
    if (le32_to_cpu(hb->hb_magic) != VMSFS_HOME_MAGIC) {
        if (!silent)
            pr_err("vmsfs: invalid home block magic: 0x%08x\n",
                   le32_to_cpu(hb->hb_magic));
        ret = -EINVAL;
        goto err_brelse;
    }

    /* Validate checksum */
    cksum = vmsfs_checksum(hb, sizeof(*hb));
    if (cksum != le32_to_cpu(hb->hb_checksum)) {
        if (!silent)
            pr_err("vmsfs: home block checksum mismatch\n");
        ret = -EINVAL;
        goto err_brelse;
    }

    /* Cache home block */
    sbi->home = kmemdup(hb, sizeof(*hb), GFP_KERNEL);
    if (!sbi->home) {
        ret = -ENOMEM;
        goto err_brelse;
    }

    /* Extract geometry */
    sbi->bitmap_lbn = le32_to_cpu(hb->hb_bitmap_lbn);
    sbi->bitmap_blocks = le32_to_cpu(hb->hb_bitmap_blocks);
    sbi->index_lbn = le32_to_cpu(hb->hb_index_lbn);
    sbi->max_files = le32_to_cpu(hb->hb_max_files);
    sbi->data_lbn = le32_to_cpu(hb->hb_data_lbn);
    sbi->total_blocks = le32_to_cpu(hb->hb_total_blocks);
    sbi->free_blocks = le32_to_cpu(hb->hb_free_blocks);

    brelse(bh);
    bh = NULL;

    /* Read and cache the storage bitmap */
    bm_bytes = sbi->bitmap_blocks * VMSFS_BLOCK_SIZE;
    sbi->bitmap = kzalloc(bm_bytes, GFP_KERNEL);
    if (!sbi->bitmap) {
        ret = -ENOMEM;
        goto err_free_home;
    }

    for (i = 0; i < sbi->bitmap_blocks; i++) {
        bh = sb_bread(sb, sbi->bitmap_lbn + i);
        if (!bh) {
            if (!silent)
                pr_err("vmsfs: unable to read bitmap block %u\n", i);
            ret = -EIO;
            goto err_free_bitmap;
        }
        memcpy((char *)sbi->bitmap + i * VMSFS_BLOCK_SIZE,
               bh->b_data, VMSFS_BLOCK_SIZE);
        brelse(bh);
    }

    /* Configure superblock */
    sb->s_magic = VMSFS_MAGIC;
    sb->s_op = &vmsfs_sops;
    sb->s_d_op = &vmsfs_dops;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_time_gran = 1;
    mutex_init(&sbi->alloc_lock);

    /* Load root inode from MFD (FID 3) */
    root_inode = vmsfs_blkdev_iget(sb, VMSFS_FID_MFD);
    if (IS_ERR(root_inode)) {
        ret = PTR_ERR(root_inode);
        if (!silent)
            pr_err("vmsfs: failed to read MFD (FID %d): %d\n",
                   VMSFS_FID_MFD, ret);
        goto err_free_bitmap;
    }

    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        ret = -ENOMEM;
        goto err_free_bitmap;
    }

    pr_info("vmsfs: mounted block device, volume '%.12s', %u blocks, "
            "%u free, %u max files\n",
            sbi->home->hb_volname, sbi->total_blocks,
            sbi->free_blocks, sbi->max_files);

    return 0;

err_free_bitmap:
    kfree(sbi->bitmap);
err_free_home:
    kfree(sbi->home);
    goto err_free_sbi;
err_brelse:
    brelse(bh);
err_free_sbi:
    kfree(sbi);
    sb->s_fs_info = NULL;
    return ret;
}

/* ================================================================
 * Mount / unmount
 *
 * Detect mount mode: if backing= option is present, use overlay
 * mode (mount_nodev). Otherwise, attempt block-device mount.
 * ================================================================ */

static bool vmsfs_has_backing_opt(void *data)
{
    const char *opts = data;

    if (!opts)
        return false;
    return strstr(opts, "backing=") != NULL;
}

static struct dentry *vmsfs_mount(struct file_system_type *fs_type, int flags,
                                  const char *dev_name, void *data)
{
    if (vmsfs_has_backing_opt(data))
        return mount_nodev(fs_type, flags, data,
                           vmsfs_fill_super_overlay);

    return mount_bdev(fs_type, flags, dev_name, data,
                      vmsfs_fill_super_blkdev);
}

static void vmsfs_kill_sb(struct super_block *sb)
{
    /*
     * Use sb->s_bdev to determine kill method, not sbi->mode.
     * mount_bdev sets s_bdev before calling fill_super, so even if
     * fill_super fails and frees sbi, s_bdev is still valid.
     * Without this, a failed blkdev mount leaks the bdev reference
     * and subsequent mounts fail with EBUSY.
     */
    if (sb->s_bdev)
        kill_block_super(sb);
    else
        kill_anon_super(sb);
}

static struct file_system_type vmsfs_fs_type = {
    .owner    = THIS_MODULE,
    .name     = "vmsfs",
    .mount    = vmsfs_mount,
    .kill_sb  = vmsfs_kill_sb,
};

/* ================================================================
 * Module init / exit
 * ================================================================ */

static int __init vmsfs_init(void)
{
    int ret;

    pr_info("vmsfs: initializing VMS filesystem module\n");

    /* Initialize the inode slab cache */
    ret = vmsfs_inode_cache_init();
    if (ret) {
        pr_err("vmsfs: failed to create inode cache\n");
        return ret;
    }

    /* Register the filesystem type */
    ret = register_filesystem(&vmsfs_fs_type);
    if (ret) {
        pr_err("vmsfs: failed to register filesystem: %d\n", ret);
        vmsfs_inode_cache_destroy();
        return ret;
    }

    pr_info("vmsfs: filesystem registered successfully\n");
    return 0;
}

static void __exit vmsfs_exit(void)
{
    pr_info("vmsfs: unloading VMS filesystem module\n");

    unregister_filesystem(&vmsfs_fs_type);
    /*
     * RCU grace period to ensure all inode frees have completed
     * before destroying the cache.
     */
    rcu_barrier();
    vmsfs_inode_cache_destroy();

    pr_info("vmsfs: module unloaded\n");
}

module_init(vmsfs_init);
module_exit(vmsfs_exit);
