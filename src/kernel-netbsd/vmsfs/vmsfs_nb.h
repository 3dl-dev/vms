/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmsfs_nb.h - NetBSD-side glue types for the OVMX ODS-2 / vmsfs vnode backend
 * (rd vms-308, epic vms-8e8). The NetBSD analog of src/kernel/vmsfs/vmsfs.h's
 * block-device half: the in-core mount (embedding the substrate-neutral
 * struct vmsfs_volume) and the in-core node (the vnode private data holding the
 * decoded ODS-2 file-header POD fields + the cached retrieval map).
 *
 * This header is included ONLY by the NetBSD backend translation unit
 * (vmsfs_vfsops.c), never by the shared core .c files -- so it may freely name
 * NetBSD kernel types (struct mount / struct vnode / kmutex_t). The shared core
 * reaches the volume solely through struct vmsfs_volume + the vmsfs_bio ops.
 *
 * Clean-room (CLAUDE.md Rule 8): OVMX's own types over the public NetBSD VFS
 * interface. No NetBSD or VSI/HPE source is copied.
 */

#ifndef OVMX_VMSFS_NB_H
#define OVMX_VMSFS_NB_H

#include <sys/param.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/mutex.h>

/*
 * The substrate-neutral ODS-2 core contract: struct vmsfs_volume, the vmsfs_bio
 * ops, struct vmsfs_fh_info + the pure decode/encode, the allocator and the
 * directory scanner, plus (transitively) the on-disk format and the name/
 * version/map algorithms. Selecting OVMX_KBACKEND_NETBSD binds it to
 * vmsfs_backend_netbsd.h.
 */
#include "vmsfs_bio.h"

#define MOUNT_VMSFS "vmsfs"

/*
 * Mount arguments (the mount(2) data blob). fspec is the block-device path of
 * the mastered ODS-2 volume, exactly like every other NetBSD disk filesystem.
 */
struct vmsfs_args {
	char *fspec;   /* block special device to mount */
};

/* In-core mount: one per mounted ODS-2 volume. */
struct vmsfs_mount {
	struct mount           *vm_mp;        /* back-pointer to the VFS mount */
	struct vnode           *vm_devvp;     /* the opened block-device vnode */
	dev_t                   vm_dev;       /* its dev_t */
	struct vmsfs_home_block vm_home;      /* cached home block (superblock) */
	struct vmsfs_volume     vm_vol;       /* geometry; vm_vol.host == vm_devvp */
	kmutex_t                vm_alloc_lock; /* guards vm_vol allocation state */
	int                     vm_case_blind; /* case-insensitive lookup (default 1) */
};

/*
 * In-core node: the vnode private data. Carries the decoded ODS-2 file-header
 * fields (struct vmsfs_fh_info's meaning) and the cached retrieval map so
 * VOP_READ / VOP_READDIR / VOP_LOOKUP reach the file's data blocks without a
 * re-read of the header. vn_key is the STABLE vcache key (== the FID) whose
 * address is handed back to vcache from vfs_loadvnode.
 */
struct vmsfs_node {
	struct vnode              *vn_vp;         /* back-pointer to the vnode */
	struct vmsfs_mount        *vn_vmp;        /* owning mount */
	uint32_t                   vn_fid;        /* file header number (FID) */
	uint32_t                   vn_parent_fid; /* parent directory FID */
	uint16_t                   vn_flags;      /* ODS-2 fh flags (DIRECTORY, ...) */
	uint16_t                   vn_version;    /* file version */
	uint16_t                   vn_prot;       /* SOGW protection mask */
	uint16_t                   vn_uic_group;
	uint16_t                   vn_uic_member;
	uint16_t                   vn_link_count;
	uint16_t                   vn_map_count;  /* valid retrieval pointers */
	uint64_t                   vn_size;       /* file size in bytes */
	uint32_t                   vn_blocks;     /* allocated blocks */
	uint64_t                   vn_created;    /* Unix epoch seconds */
	uint64_t                   vn_modified;
	uint64_t                   vn_accessed;
	uint32_t                   vn_key;        /* stable vcache key (== vn_fid) */
	struct vmsfs_retrieval_ptr vn_map[VMSFS_MAX_RETRIEVAL];
};

#define VFSTOVMSFS(mp) ((struct vmsfs_mount *)((mp)->mnt_data))
#define VTOVMSFS(vp)   ((struct vmsfs_node *)((vp)->v_data))

/* The vnode operations vector (built in vmsfs_vfsops.c). */
extern int (**vmsfs_vnodeop_p)(void *);

#endif /* OVMX_VMSFS_NB_H */
