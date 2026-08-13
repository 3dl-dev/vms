/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmsfs.h - Internal header for the VMS filesystem kernel module (vmsfs.ko)
 *
 * Supports two mount modes:
 *   1. Overlay mode (backing=PATH) — overlays a host directory with VMS
 *      versioning, case-insensitive lookup, and SOGW protection.
 *   2. Block-device mode — mounts a block device formatted with the VMSFS
 *      on-disk format (vmsfs_ondisk.h). Supports read-write operations.
 *
 * OVMX Project - Phase 4b/7: Kernel-native VMS Filesystem
 */

#ifndef _VMSFS_H
#define _VMSFS_H

#include <linux/fs.h>
#include <linux/types.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/mutex.h>

#include "vmsfs_ondisk.h"
/*
 * The substrate-neutral ODS-2 core (rd vms-544, epic vms-8e8): the ODS-2
 * name-shape limits (VMSFS_MAX_VERSION / _NAME / _TYPE / _FILENAME) and the
 * version-resolution + retrieval-map algorithm prototypes now live in
 * src/kernel-core/vmsfs/vmsfs_core.h, implemented against the vmsfs_backend
 * shim. This Linux VFS glue includes it so the rest of vmsfs.ko still sees
 * those constants and prototypes.
 */
#include "vmsfs_core.h"
/*
 * The block/inode seam (rd vms-d69, epic vms-8e8): the storage/FID allocator,
 * the directory-block scanner and the file-header decode/encode now live in the
 * substrate-neutral core (vmsfs_alloc.c / vmsfs_dirscan.c / vmsfs_header.c) and
 * reach the volume through struct vmsfs_volume + the vmsfs_bio ops. This Linux
 * VFS glue includes the seam contract so its vmsfs_sb_info can embed a
 * vmsfs_volume and the backend can call the core algorithms.
 */
#include "vmsfs_bio.h"

#define VMSFS_MAGIC       0x564D5346  /* "VMSF" */

/* Mount modes */
enum vmsfs_mode {
    VMSFS_MODE_OVERLAY,   /* backing=PATH directory overlay */
    VMSFS_MODE_BLKDEV,    /* block-device with on-disk format */
};

/* Mount options */
struct vmsfs_mount_opts {
    char backing_path[PATH_MAX];  /* backing directory on host FS (overlay) */
    int  version_limit;           /* max versions per file (default 32767) */
    int  case_blind;              /* case-insensitive lookup (default 1) */
};

/* Superblock private data */
struct vmsfs_sb_info {
    enum vmsfs_mode mode;
    struct vmsfs_mount_opts opts;

    /* Overlay mode */
    struct path backing;          /* backing directory path */

    /* Block-device mode */
    struct vmsfs_home_block *home;  /* cached home block */
    struct mutex alloc_lock;        /* protects vol.bitmap/free_blocks + FID alloc */

    /*
     * Host-neutral volume descriptor (rd vms-d69): geometry + the cached
     * storage bitmap + free-block count, reached by the substrate-neutral
     * allocator/dir-scanner. vol.host is set to this super_block at mount so
     * the core's vmsfs_bget() can read/write the volume.
     */
    struct vmsfs_volume vol;
};

/* Inode private data - embedded VFS inode for container_of */
struct vmsfs_inode_info {
    struct inode vfs_inode;       /* must be first for container_of */
    int version;                  /* file version number (0 for dirs) */
    uint16_t vms_prot;           /* SOGW protection mask */
    char base_name[VMSFS_MAX_NAME + 1];  /* name without version */
    char extension[VMSFS_MAX_TYPE + 1];  /* extension (after dot) */

    /* Block-device mode: cached retrieval pointers */
    uint32_t fid;                /* file header number */
    uint16_t map_count;          /* valid retrieval pointers */
    struct vmsfs_retrieval_ptr map[VMSFS_MAX_RETRIEVAL];
};

static inline struct vmsfs_sb_info *VMSFS_SB(struct super_block *sb)
{
    return sb->s_fs_info;
}

static inline struct vmsfs_inode_info *VMSFS_I(struct inode *inode)
{
    return container_of(inode, struct vmsfs_inode_info, vfs_inode);
}

/* Slab cache for inode allocation (defined in vmsfs_inode.c) */
extern struct kmem_cache *vmsfs_inode_cachep;

/* ================================================================
 * Operations tables
 * ================================================================ */

/* Super operations (vmsfs_super.c / vmsfs_inode.c) */
extern const struct super_operations vmsfs_sops;

/* Directory inode operations (vmsfs_inode.c) */
extern const struct inode_operations vmsfs_dir_iops;

/* File inode operations (vmsfs_inode.c) */
extern const struct inode_operations vmsfs_file_iops;

/* Directory file operations (vmsfs_dir.c) */
extern const struct file_operations vmsfs_dir_fops;

/* File operations (vmsfs_file.c) */
extern const struct file_operations vmsfs_file_fops;

/* Dentry operations for case-insensitive matching (vmsfs_inode.c) */
extern const struct dentry_operations vmsfs_dops;

/*
 * Version-resolution operations (vmsfs_parse_version /
 * vmsfs_build_versioned_name) are declared in kernel-core/vmsfs/vmsfs_core.h,
 * included above. The former backing-directory scanner
 * (vmsfs_find_highest_version) was dead code -- a stub with no callers -- and
 * was removed with the src/kernel-core extraction (rd vms-544).
 */

/* ================================================================
 * Helpers (vmsfs_inode.c)
 * ================================================================ */

/*
 * VFS inode allocation callback (sb->s_op->alloc_inode).
 */
struct inode *vmsfs_alloc_inode(struct super_block *sb);

/*
 * VFS inode free callback (sb->s_op->free_inode).
 */
void vmsfs_free_inode(struct inode *inode);

/*
 * Allocate and initialize a new vmsfs inode.
 */
struct inode *vmsfs_new_inode(struct super_block *sb, umode_t mode);

/* Module init/exit for inode cache (called from vmsfs_super.c) */
int vmsfs_inode_cache_init(void);
void vmsfs_inode_cache_destroy(void);

/*
 * MAXSYSGROUP — the SYSGEN parameter that decides which UIC groups get the
 * VMS SYSTEM protection category: every UIC whose GROUP number is <=
 * MAXSYSGROUP (default 8) is System, not only group 0. This kernel constant
 * MUST match the userspace executive, which already applies this rule in
 * src/libvms/syssvc/sys_security.c (uic_is_system()); the value and its
 * two-source pin (lab VAX V7.3 SYSGEN capture + VSI OpenVMS "UIC Protection"
 * wiki, octal 10 == decimal 8) live in src/libvms/include/ovmx_secparam.h
 * (OVMX_MAXSYSGROUP). It is a settable SYSGEN parameter on VMS and a
 * compile-time constant here until OVMX has a SYSGEN parameter store.
 *
 * WHY IT MATTERS HERE (vms-581): LOGINOUT drops each session to its
 * authenticated UIC (tools/vms_login.c: setgid(uic_group); setuid(uic_member)),
 * so the SYSTEM account runs as Linux gid=1 (UIC group 1), NOT gid=0. A
 * `group == 0` System test therefore never matched SYSTEM, dropping it to the
 * Group/World nibble — which VMSFS_PROT_DEFAULT denies WRITE — so RMS CREATE
 * (open(2) O_CREAT) failed %RMS-E-CRE/EACCES on every real vmsfs.ko mount.
 */
#define VMSFS_MAXSYSGROUP 8

/* ================================================================
 * SOGW permission helpers (vmsfs_blkdev.c)
 *
 * VMS uses a four-category protection model: System, Owner, Group, World.
 * Each category has four access bits: Read, Write, Execute, Delete.
 * Protection bits are denial masks — 1 means the access is DENIED.
 *
 * Category is determined by comparing the process UIC to the file's owner UIC:
 *   System: process UIC group <= MAXSYSGROUP (SYSTEM groups) — granted full access
 *   Owner:  process UIC group and member both match file owner UIC
 *   Group:  process UIC group matches file owner UIC group
 *   World:  no match
 * ================================================================ */

/*
 * Convert a SOGW denial nibble to a Unix-style mode nibble (rwx bits).
 * This is used only to approximate i_mode for display by tools like ls.
 * Actual access control goes through .permission, not mode bits.
 */
static inline umode_t vmsfs_sogw_nibble_to_mode(uint8_t deny)
{
    umode_t bits = 0;

    if (!(deny & VMSFS_PROT_R))
        bits |= 0x4;  /* r */
    if (!(deny & VMSFS_PROT_W))
        bits |= 0x2;  /* w */
    if (!(deny & VMSFS_PROT_E))
        bits |= 0x1;  /* x */
    return bits;
}

/*
 * Build the Unix i_mode permission bits from a SOGW protection mask.
 * Used for display purposes only — real access control uses .permission.
 *
 * Maps: owner nibble -> user bits, group nibble -> group bits,
 *       world nibble -> other bits.
 */
static inline umode_t vmsfs_prot_to_mode(uint16_t prot)
{
    uint8_t own_deny = (prot >> VMSFS_PROT_OWN_SHIFT) & 0xF;
    uint8_t grp_deny = (prot >> VMSFS_PROT_GRP_SHIFT) & 0xF;
    uint8_t wld_deny = (prot >> VMSFS_PROT_WLD_SHIFT) & 0xF;

    return (vmsfs_sogw_nibble_to_mode(own_deny) << 6) |
           (vmsfs_sogw_nibble_to_mode(grp_deny) << 3) |
            vmsfs_sogw_nibble_to_mode(wld_deny);
}

/* ================================================================
 * Block-device mode operations (vmsfs_blkdev.c)
 * ================================================================ */

/* Read a file header by FID and create/populate a VFS inode */
struct inode *vmsfs_blkdev_iget(struct super_block *sb, uint32_t fid);

/* Write inode metadata + retrieval pointers back to on-disk file header */
int vmsfs_blkdev_flush_inode(struct super_block *sb, struct inode *inode);

/*
 * Resolve a (possibly versioned) name in @dir to a FID, exactly as
 * ->lookup would. *fid_out is 0 when the name does not exist. Returns a
 * negative errno only when @name is not a parseable VMS filename.
 * Used by ->lookup and by ->d_revalidate (vmsfs_inode.c).
 */
int vmsfs_blkdev_resolve(struct inode *dir, const char *name,
                         uint32_t *fid_out);

/* Block-device directory inode operations */
extern const struct inode_operations vmsfs_blkdev_dir_iops;

/* Block-device directory file operations */
extern const struct file_operations vmsfs_blkdev_dir_fops;

/* Block-device file operations */
extern const struct file_operations vmsfs_blkdev_file_fops;

/* Block-device file inode operations */
extern const struct inode_operations vmsfs_blkdev_file_iops;

#endif /* _VMSFS_H */
