// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_inode.c - Inode operations for vmsfs
 *
 * Handles inode allocation/freeing (via slab cache), directory lookup
 * with version resolution, file/directory creation with auto-versioning,
 * and case-insensitive dentry operations.
 *
 * Each vmsfs inode maps to a file or directory in the backing store.
 * Files in the backing store use versioned names (e.g., "FOO.TXT;1").
 * Directories are stored as-is without version suffixes.
 *
 * OVMX Project - Phase 4b: Kernel-native VMS Filesystem
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/namei.h>
#include <linux/ctype.h>
#include <linux/dcache.h>

#include "vmsfs.h"

/* ================================================================
 * Inode slab cache
 *
 * We use a kmem_cache to allocate vmsfs_inode_info structures, which
 * embed the VFS inode.  This is the standard pattern for filesystem
 * inode allocation in the Linux kernel.
 * ================================================================ */

struct kmem_cache *vmsfs_inode_cachep;

static void vmsfs_inode_init_once(void *obj)
{
    struct vmsfs_inode_info *vi = obj;

    inode_init_once(&vi->vfs_inode);
}

int vmsfs_inode_cache_init(void)
{
    vmsfs_inode_cachep = kmem_cache_create("vmsfs_inode",
                                           sizeof(struct vmsfs_inode_info),
                                           0,
                                           SLAB_RECLAIM_ACCOUNT |
                                           SLAB_ACCOUNT,
                                           vmsfs_inode_init_once);
    if (!vmsfs_inode_cachep)
        return -ENOMEM;

    return 0;
}

void vmsfs_inode_cache_destroy(void)
{
    kmem_cache_destroy(vmsfs_inode_cachep);
}

/*
 * Allocate a new vmsfs inode from the slab cache.
 * Called by the VFS via sb->s_op->alloc_inode.
 */
struct inode *vmsfs_alloc_inode(struct super_block *sb)
{
    struct vmsfs_inode_info *vi;

    vi = alloc_inode_sb(sb, vmsfs_inode_cachep, GFP_KERNEL);
    if (!vi)
        return NULL;

    /* Initialize vmsfs-specific fields */
    vi->version = 0;
    vi->vms_prot = 0;
    vi->base_name[0] = '\0';
    vi->extension[0] = '\0';
    vi->fid = 0;
    vi->map_count = 0;

    return &vi->vfs_inode;
}

/*
 * Free a vmsfs inode back to the slab cache.
 * Called by the VFS via sb->s_op->free_inode (RCU-deferred).
 */
void vmsfs_free_inode(struct inode *inode)
{
    kmem_cache_free(vmsfs_inode_cachep, VMSFS_I(inode));
}

/*
 * vmsfs_new_inode - Create and initialize a new vmsfs inode.
 *
 * Sets up timestamps, uid/gid from current process, and assigns
 * a unique inode number.
 */
struct inode *vmsfs_new_inode(struct super_block *sb, umode_t mode)
{
    struct inode *inode;

    inode = new_inode(sb);
    if (!inode)
        return NULL;

    inode->i_ino = get_next_ino();
    inode_init_owner(&nop_mnt_idmap, inode, NULL, mode);
    simple_inode_init_ts(inode);

    return inode;
}

/* ================================================================
 * Dentry operations (case-insensitive support)
 *
 * When case_blind is enabled (default), dentry lookups use
 * case-insensitive comparison and hashing.
 * ================================================================ */

/*
 * Case-insensitive hash function for dentries.
 * Folds all characters to lowercase before hashing.
 */
static int vmsfs_d_hash(const struct dentry *dentry, struct qstr *qstr)
{
    const struct vmsfs_sb_info *sbi;
    unsigned long hash;
    const unsigned char *p;
    unsigned int i, len;

    sbi = VMSFS_SB(dentry->d_sb);
    if (!sbi->opts.case_blind)
        return 0;  /* Use default hash */

    /*
     * Compute a case-insensitive hash by folding to lowercase.
     * We use init_name_hash / partial_name_hash / end_name_hash
     * which are the standard kernel dentry hash helpers.
     */
    p = (const unsigned char *)qstr->name;
    len = qstr->len;

    hash = init_name_hash(dentry);
    for (i = 0; i < len; i++)
        hash = partial_name_hash(tolower(p[i]), hash);
    qstr->hash = end_name_hash(hash);

    return 0;
}

/*
 * Case-insensitive dentry comparison.
 * Compares two names ignoring case, but also handles version suffixes:
 * "FOO.TXT;1" and "foo.txt;1" should match.
 */
static int vmsfs_d_compare(const struct dentry *dentry, unsigned int len,
                           const char *str, const struct qstr *name)
{
    const struct vmsfs_sb_info *sbi = VMSFS_SB(dentry->d_sb);

    if (!sbi->opts.case_blind) {
        /* Case-sensitive: exact match required */
        if (len != name->len)
            return 1;
        return memcmp(str, name->name, len);
    }

    /* Case-insensitive comparison */
    if (len != name->len)
        return 1;

    return strncasecmp(str, name->name, len);
}

/*
 * vmsfs_d_revalidate - Force re-lookup for regular file dentries.
 *
 * VMS versioning means a file's identity can change between accesses
 * (e.g., open("FOO.TXT") should find the highest version, which may
 * have changed).  Returning 0 for regular file dentries forces the
 * VFS to invalidate and re-lookup, which then triggers atomic_open
 * for O_CREAT operations.
 */
static int vmsfs_d_revalidate(struct dentry *dentry, unsigned int flags)
{
    /* Drop out of RCU walk - we can't do real revalidation in RCU mode */
    if (flags & LOOKUP_RCU)
        return -ECHILD;

    /* Directories are stable - only invalidate regular file dentries */
    if (d_is_positive(dentry) && d_is_reg(dentry))
        return 0;
    return 1;
}

const struct dentry_operations vmsfs_dops = {
    .d_hash        = vmsfs_d_hash,
    .d_compare     = vmsfs_d_compare,
    .d_revalidate  = vmsfs_d_revalidate,
};

/* ================================================================
 * Helper: find or create a backing dentry for a versioned filename
 * ================================================================ */

/*
 * Look up a specific versioned file in the backing directory.
 * Returns the backing dentry (positive or negative), or ERR_PTR on error.
 */
static struct dentry *vmsfs_backing_lookup(struct vmsfs_sb_info *sbi,
                                           const char *versioned_name)
{
    struct dentry *backing_dentry;

    inode_lock(d_inode(sbi->backing.dentry));
    backing_dentry = lookup_one_len(versioned_name, sbi->backing.dentry,
                                    strlen(versioned_name));
    inode_unlock(d_inode(sbi->backing.dentry));

    return backing_dentry;
}

/*
 * Scan the backing directory for the highest version of a file.
 * This is a wrapper that uses filp_open on the backing path to
 * iterate the directory, since we always have the path string available.
 */
struct vmsfs_scan_ctx {
    struct dir_context ctx;
    const char *base;
    size_t base_len;
    int highest;
    int case_blind;
};

static bool vmsfs_scan_actor(struct dir_context *ctx, const char *name,
                             int namlen, loff_t offset, u64 ino,
                             unsigned int d_type)
{
    struct vmsfs_scan_ctx *sctx =
        container_of(ctx, struct vmsfs_scan_ctx, ctx);
    const char *semi;
    const char *p;
    int ver;

    /* Skip . and .. */
    if (namlen == 1 && name[0] == '.')
        return true;
    if (namlen == 2 && name[0] == '.' && name[1] == '.')
        return true;

    /* Find the semicolon in the entry name */
    semi = memchr(name, ';', namlen);
    if (!semi)
        return true;  /* No version suffix - skip */

    /* Check if the base part matches */
    {
        size_t entry_base_len = semi - name;

        if (entry_base_len != sctx->base_len)
            return true;

        if (sctx->case_blind) {
            if (strncasecmp(name, sctx->base, sctx->base_len) != 0)
                return true;
        } else {
            if (memcmp(name, sctx->base, sctx->base_len) != 0)
                return true;
        }
    }

    /* Parse the version number */
    p = semi + 1;
    ver = 0;
    while (p < name + namlen) {
        if (!isdigit((unsigned char)*p))
            return true;  /* Invalid version */
        ver = ver * 10 + (*p - '0');
        if (ver > VMSFS_MAX_VERSION)
            return true;
        p++;
    }

    if (ver > sctx->highest)
        sctx->highest = ver;

    return true;
}

static int vmsfs_scan_highest(struct vmsfs_sb_info *sbi, const char *base)
{
    struct vmsfs_scan_ctx sctx = {
        .ctx.actor = vmsfs_scan_actor,
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
 * Directory inode operations: lookup, create, mkdir, unlink
 * ================================================================ */

/*
 * vmsfs_lookup - Look up a file by name with VMS version resolution.
 *
 * Version resolution rules:
 *   "FOO.TXT"    -> highest version (same as ;0)
 *   "FOO.TXT;0"  -> highest version
 *   "FOO.TXT;3"  -> exactly version 3
 *
 * For directories, no version suffix is used.
 */
static struct dentry *vmsfs_lookup(struct inode *dir, struct dentry *dentry,
                                   unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    char base[VMSFS_MAX_FILENAME + 1];
    char versioned[VMSFS_MAX_FILENAME + 1];
    int version;
    int ret;
    struct dentry *backing_dentry;
    struct inode *inode = NULL;
    struct vmsfs_inode_info *vi;

    /* Parse the requested name for version info */
    ret = vmsfs_parse_version(dentry->d_name.name, base, sizeof(base),
                              &version);
    if (ret)
        return ERR_PTR(ret);

    /*
     * First, check if it is a directory (no version suffix in backing).
     */
    backing_dentry = vmsfs_backing_lookup(sbi, base);
    if (!IS_ERR(backing_dentry) && d_is_positive(backing_dentry)) {
        struct inode *backing_inode = d_inode(backing_dentry);

        if (S_ISDIR(backing_inode->i_mode)) {
            /* It is a directory - create inode for it */
            inode = vmsfs_new_inode(sb, S_IFDIR | 0755);
            if (!inode) {
                dput(backing_dentry);
                return ERR_PTR(-ENOMEM);
            }
            inode->i_op = &vmsfs_dir_iops;
            inode->i_fop = &vmsfs_dir_fops;
            set_nlink(inode, 2);

            vi = VMSFS_I(inode);
            strscpy(vi->base_name, base, sizeof(vi->base_name));
            vi->version = 0;

            dput(backing_dentry);
            return d_splice_alias(inode, dentry);
        }
        dput(backing_dentry);
    } else if (!IS_ERR(backing_dentry)) {
        dput(backing_dentry);
    }

    /*
     * Not a directory - resolve the version.
     * version == 0 means find the highest.
     */
    if (version == 0) {
        version = vmsfs_scan_highest(sbi, base);
        if (version == 0) {
            /* No matching file found */
            return d_splice_alias(NULL, dentry);
        }
    }

    /* Build the versioned name and look it up in the backing dir */
    ret = vmsfs_build_versioned_name(base, version, versioned,
                                     sizeof(versioned));
    if (ret)
        return ERR_PTR(ret);

    backing_dentry = vmsfs_backing_lookup(sbi, versioned);
    if (IS_ERR(backing_dentry))
        return ERR_CAST(backing_dentry);

    if (!d_is_positive(backing_dentry)) {
        /* Specific version not found */
        dput(backing_dentry);
        return d_splice_alias(NULL, dentry);
    }

    /* Create a vmsfs inode for this versioned file */
    inode = vmsfs_new_inode(sb, S_IFREG | 0644);
    if (!inode) {
        dput(backing_dentry);
        return ERR_PTR(-ENOMEM);
    }

    inode->i_op = &vmsfs_file_iops;
    inode->i_fop = &vmsfs_file_fops;
    inode->i_size = i_size_read(d_inode(backing_dentry));

    vi = VMSFS_I(inode);
    strscpy(vi->base_name, base, sizeof(vi->base_name));
    vi->version = version;

    dput(backing_dentry);
    return d_splice_alias(inode, dentry);
}

/*
 * vmsfs_create - Create a new file with auto-incrementing version.
 *
 * When a file "FOO.TXT" is created:
 *   1. Find the highest existing version N of FOO.TXT
 *   2. Create FOO.TXT;N+1 in the backing directory
 *   3. Return the new inode
 *
 * If the file does not exist yet, it is created as version 1.
 */
static int vmsfs_create(struct mnt_idmap *idmap, struct inode *dir,
                        struct dentry *dentry, umode_t mode, bool excl)
{
    struct super_block *sb = dir->i_sb;
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    char base[VMSFS_MAX_FILENAME + 1];
    char versioned[VMSFS_MAX_FILENAME + 1];
    int version;
    int highest;
    int new_version;
    int ret;
    struct dentry *backing_dentry;
    struct inode *inode;
    struct inode *backing_dir_inode;
    struct vmsfs_inode_info *vi;

    /* Parse the name - strip any version suffix */
    ret = vmsfs_parse_version(dentry->d_name.name, base, sizeof(base),
                              &version);
    if (ret)
        return ret;

    /* Find the highest existing version */
    highest = vmsfs_scan_highest(sbi, base);
    new_version = highest + 1;

    if (new_version > sbi->opts.version_limit) {
        pr_warn("vmsfs: version limit (%d) reached for %s\n",
                sbi->opts.version_limit, base);
        return -ENOSPC;
    }

    /* Build the versioned name for the backing file */
    ret = vmsfs_build_versioned_name(base, new_version, versioned,
                                     sizeof(versioned));
    if (ret)
        return ret;

    /* Create the file in the backing directory */
    backing_dir_inode = d_inode(sbi->backing.dentry);

    inode_lock(backing_dir_inode);
    backing_dentry = lookup_one_len(versioned, sbi->backing.dentry,
                                    strlen(versioned));
    if (IS_ERR(backing_dentry)) {
        inode_unlock(backing_dir_inode);
        return PTR_ERR(backing_dentry);
    }

    ret = vfs_create(&nop_mnt_idmap, backing_dir_inode, backing_dentry,
                     mode, false);
    inode_unlock(backing_dir_inode);

    if (ret) {
        dput(backing_dentry);
        return ret;
    }

    /* Create the vmsfs inode */
    inode = vmsfs_new_inode(sb, mode);
    if (!inode) {
        dput(backing_dentry);
        return -ENOMEM;
    }

    inode->i_op = &vmsfs_file_iops;
    inode->i_fop = &vmsfs_file_fops;

    vi = VMSFS_I(inode);
    strscpy(vi->base_name, base, sizeof(vi->base_name));
    vi->version = new_version;

    d_instantiate(dentry, inode);
    dput(backing_dentry);

    return 0;
}

/*
 * vmsfs_mkdir - Create a directory in the backing store.
 *
 * Directories in vmsfs do not use version numbers.  The directory is
 * created directly in the backing directory with its original name.
 */
static int vmsfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
                       struct dentry *dentry, umode_t mode)
{
    struct super_block *sb = dir->i_sb;
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct dentry *backing_dentry;
    struct inode *backing_dir_inode;
    struct inode *inode;
    struct vmsfs_inode_info *vi;
    int ret;

    backing_dir_inode = d_inode(sbi->backing.dentry);

    inode_lock(backing_dir_inode);
    backing_dentry = lookup_one_len(dentry->d_name.name,
                                    sbi->backing.dentry,
                                    dentry->d_name.len);
    if (IS_ERR(backing_dentry)) {
        inode_unlock(backing_dir_inode);
        return PTR_ERR(backing_dentry);
    }

    ret = vfs_mkdir(&nop_mnt_idmap, backing_dir_inode, backing_dentry,
                    mode);
    inode_unlock(backing_dir_inode);

    if (ret) {
        dput(backing_dentry);
        return ret;
    }

    /* Create the vmsfs directory inode */
    inode = vmsfs_new_inode(sb, S_IFDIR | mode);
    if (!inode) {
        dput(backing_dentry);
        return -ENOMEM;
    }

    inode->i_op = &vmsfs_dir_iops;
    inode->i_fop = &vmsfs_dir_fops;
    set_nlink(inode, 2);

    vi = VMSFS_I(inode);
    strscpy(vi->base_name, dentry->d_name.name, sizeof(vi->base_name));
    vi->version = 0;

    /* Increment parent directory link count */
    inc_nlink(dir);

    d_instantiate(dentry, inode);
    dput(backing_dentry);

    return 0;
}

/*
 * vmsfs_unlink - Delete a specific version of a file.
 *
 * If the filename includes a version (e.g., "FOO.TXT;3"), that specific
 * version is deleted from the backing store.  If no version is specified,
 * the highest version is deleted.
 */
static int vmsfs_unlink(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    struct inode *inode = d_inode(dentry);
    struct vmsfs_inode_info *vi = VMSFS_I(inode);
    char versioned[VMSFS_MAX_FILENAME + 1];
    struct dentry *backing_dentry;
    struct inode *backing_dir_inode;
    int ret;
    int version;

    version = vi->version;
    if (version == 0) {
        /* No version stored - parse from dentry name and resolve */
        char base[VMSFS_MAX_FILENAME + 1];
        int parsed_ver;

        ret = vmsfs_parse_version(dentry->d_name.name, base,
                                  sizeof(base), &parsed_ver);
        if (ret)
            return ret;

        if (parsed_ver == 0) {
            parsed_ver = vmsfs_scan_highest(sbi, base);
            if (parsed_ver == 0)
                return -ENOENT;
        }
        version = parsed_ver;

        ret = vmsfs_build_versioned_name(base, version, versioned,
                                         sizeof(versioned));
    } else {
        ret = vmsfs_build_versioned_name(vi->base_name, version,
                                         versioned, sizeof(versioned));
    }
    if (ret)
        return ret;

    /* Delete from the backing directory */
    backing_dir_inode = d_inode(sbi->backing.dentry);

    inode_lock(backing_dir_inode);
    backing_dentry = lookup_one_len(versioned, sbi->backing.dentry,
                                    strlen(versioned));
    if (IS_ERR(backing_dentry)) {
        inode_unlock(backing_dir_inode);
        return PTR_ERR(backing_dentry);
    }

    if (!d_is_positive(backing_dentry)) {
        inode_unlock(backing_dir_inode);
        dput(backing_dentry);
        return -ENOENT;
    }

    ret = vfs_unlink(&nop_mnt_idmap, backing_dir_inode, backing_dentry,
                     NULL);
    inode_unlock(backing_dir_inode);

    if (!ret) {
        drop_nlink(inode);
        d_drop(dentry);
    }

    dput(backing_dentry);
    return ret;
}

/*
 * vmsfs_getattr - Return file attributes.
 * Delegates to the backing file to get accurate size and times.
 */
static int vmsfs_getattr(struct mnt_idmap *idmap,
                         const struct path *path, struct kstat *stat,
                         u32 request_mask, unsigned int query_flags)
{
    struct inode *inode = d_inode(path->dentry);

    generic_fillattr(&nop_mnt_idmap, request_mask, inode, stat);
    return 0;
}

/*
 * vmsfs_atomic_open - Handle open with VMS version auto-increment.
 *
 * The Linux VFS normally won't call .create if a file already exists
 * (i.e., O_CREAT without O_EXCL just opens the existing file).
 * In VMS, every CREATE of "FOO.TXT" must produce a new version
 * (FOO.TXT;N+1).  atomic_open intercepts the open path before the
 * VFS makes its "exists → just open" decision, giving us full
 * control over version semantics.
 *
 * For non-O_CREAT opens, we do a lookup and let the VFS handle the
 * actual open via f_op->open (vmsfs_open in vmsfs_file.c).
 */
static int vmsfs_atomic_open(struct inode *dir, struct dentry *dentry,
                              struct file *file, unsigned int open_flag,
                              umode_t create_mode)
{
    struct super_block *sb = dir->i_sb;
    struct vmsfs_sb_info *sbi = VMSFS_SB(sb);
    char base[VMSFS_MAX_FILENAME + 1];
    char versioned[VMSFS_MAX_FILENAME + 1];
    int version, highest, new_version;
    int ret;
    struct dentry *backing_dentry;
    struct inode *inode;
    struct inode *backing_dir_inode;
    struct vmsfs_inode_info *vi;

    if (!(open_flag & O_CREAT)) {
        /*
         * Not creating - do a normal lookup.  If the file is found,
         * return 1 via finish_no_open() to let the VFS complete the
         * open through f_op->open (vmsfs_open).
         */
        struct dentry *res = vmsfs_lookup(dir, dentry, 0);

        if (IS_ERR(res))
            return PTR_ERR(res);
        if (res)
            dentry = res;
        if (d_is_negative(dentry))
            return -ENOENT;
        return finish_no_open(file, dentry);
    }

    /* O_CREAT: VMS semantics -- always create a new version */

    /* Parse the name - strip any version suffix */
    ret = vmsfs_parse_version(dentry->d_name.name, base, sizeof(base),
                              &version);
    if (ret)
        return ret;

    /* Find the highest existing version */
    highest = vmsfs_scan_highest(sbi, base);
    new_version = highest + 1;

    if (new_version > sbi->opts.version_limit) {
        pr_warn("vmsfs: version limit (%d) reached for %s\n",
                sbi->opts.version_limit, base);
        return -ENOSPC;
    }

    /* Build the versioned name for the backing file */
    ret = vmsfs_build_versioned_name(base, new_version, versioned,
                                     sizeof(versioned));
    if (ret)
        return ret;

    /* Create the file in the backing directory */
    backing_dir_inode = d_inode(sbi->backing.dentry);

    inode_lock(backing_dir_inode);
    backing_dentry = lookup_one_len(versioned, sbi->backing.dentry,
                                    strlen(versioned));
    if (IS_ERR(backing_dentry)) {
        inode_unlock(backing_dir_inode);
        return PTR_ERR(backing_dentry);
    }

    ret = vfs_create(&nop_mnt_idmap, backing_dir_inode, backing_dentry,
                     create_mode, false);
    inode_unlock(backing_dir_inode);

    if (ret) {
        dput(backing_dentry);
        return ret;
    }

    /* Create the vmsfs inode */
    inode = vmsfs_new_inode(sb, create_mode);
    if (!inode) {
        dput(backing_dentry);
        return -ENOMEM;
    }

    inode->i_op = &vmsfs_file_iops;
    inode->i_fop = &vmsfs_file_fops;

    vi = VMSFS_I(inode);
    strscpy(vi->base_name, base, sizeof(vi->base_name));
    vi->version = new_version;

    /*
     * Use d_splice_alias instead of d_instantiate: the dentry from
     * d_alloc_parallel is still in "lookup" state (d_u.d_in_lookup_hash
     * overlaps d_u.d_alias in the union), so d_instantiate's BUG_ON
     * fires.  d_splice_alias calls __d_add which properly handles
     * in-lookup dentries via __d_lookup_unhash first.
     */
    d_splice_alias(inode, dentry);
    dput(backing_dentry);

    /* Mark as newly created and complete the open via f_op->open */
    file->f_mode |= FMODE_CREATED;
    return finish_open(file, dentry, NULL);
}

/* ================================================================
 * Operations tables
 * ================================================================ */

const struct inode_operations vmsfs_dir_iops = {
    .lookup     = vmsfs_lookup,
    .create     = vmsfs_create,
    .atomic_open = vmsfs_atomic_open,
    .mkdir      = vmsfs_mkdir,
    .unlink     = vmsfs_unlink,
    .getattr    = vmsfs_getattr,
};

const struct inode_operations vmsfs_file_iops = {
    .getattr = vmsfs_getattr,
};
