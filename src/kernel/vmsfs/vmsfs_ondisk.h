/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmsfs_ondisk.h - On-disk format for the VMSFS block-device filesystem
 *
 * Simplified ODS-2-inspired on-disk layout for OVMX system disks.
 * Provides VMS semantics (case-insensitive lookup, file versioning,
 * SOGW protection) with our own on-disk structures — not byte-compatible
 * with real ODS-2.
 *
 * Shared between kernel module (vmsfs.ko) and userspace tools (INITIALIZE).
 *
 * Volume layout (512-byte blocks):
 *   Block 0            Boot block (reserved)
 *   Block 1            Home block (superblock)
 *   Blocks 2..N        Storage bitmap (1 bit per block)
 *   Blocks N+1..M      File header area (1 header per block)
 *   Blocks M+1..end    Data area
 *
 * All multi-byte fields are little-endian.
 */

#ifndef _VMSFS_ONDISK_H
#define _VMSFS_ONDISK_H

/*
 * Fixed-width types (uint8/16/32/64_t) for the on-disk layout. This header is
 * shared THREE ways -- the Linux vmsfs.ko, the NetBSD/BSD vnode module, and
 * userspace tools (vmsfs_master) -- so the include must be portable across all
 * three. __KERNEL__ is a LINUX-only macro; the NetBSD kernel-module build
 * defines _KERNEL (never __KERNEL__), so a bare `#ifdef __KERNEL__ ... #else
 * <stdint.h>' wrongly pulled userspace <stdint.h> into the -nostdinc BSD kernel
 * build and broke it (INV-DRIFT). Branch on each kernel explicitly.
 */
#if defined(__KERNEL__)         /* Linux kernel */
#include <linux/types.h>
#elif defined(_KERNEL)          /* NetBSD / BSD kernel: fixed-width types, no libc */
#include <sys/stdint.h>
#else                           /* userspace */
#include <stdint.h>
#endif

/* ================================================================
 * Constants
 * ================================================================ */

#define VMSFS_BLOCK_SIZE        512
#define VMSFS_BLOCK_SHIFT       9       /* log2(512) */

#define VMSFS_HOME_MAGIC        0x564D4653      /* "VMFS" */
#define VMSFS_FH_MAGIC          0x56464832      /* "VFH2" */
#define VMSFS_FORMAT_VERSION    1

/* Reserved logical block numbers */
#define VMSFS_BOOT_LBN          0
#define VMSFS_HOME_LBN          1
#define VMSFS_BITMAP_LBN        2       /* bitmap starts at block 2 */

/* Reserved file IDs (file header numbers) — match VMS conventions */
#define VMSFS_FID_INDEXF        1       /* INDEXF.SYS — the index file itself */
#define VMSFS_FID_BITMAP        2       /* BITMAP.SYS — storage bitmap */
#define VMSFS_FID_MFD           3       /* 000000.DIR — Master File Directory */
#define VMSFS_FID_FIRST_USER    4       /* first user-allocatable FID */
#define VMSFS_RESERVED_FIDS     3       /* number of reserved file headers */

/* Name limits — match ODS-2 */
#define VMSFS_NAME_MAX          39      /* max filename component */
#define VMSFS_TYPE_MAX          39      /* max type (extension) component */
#define VMSFS_FULLNAME_MAX      80      /* "NAME.TYPE\0" */

/* Version limits */
#define VMSFS_VERSION_MAX       32767
#define VMSFS_VERSION_NEWEST    0       /* resolve to highest existing */

/* ================================================================
 * Protection mask (SOGW) — VMS convention: set bit = access DENIED
 *
 * The 16-bit fh_protection field is a denial mask organized as four
 * 4-bit fields, one per category:
 *
 *   Bits  0- 3:  System (S)
 *   Bits  4- 7:  Owner  (O)
 *   Bits  8-11:  Group  (G)
 *   Bits 12-15:  World  (W)
 *
 * Within each 4-bit nibble (bit 0 is the LSB of that nibble):
 *   Bit 0: Read    (R) — 1 = Read access denied
 *   Bit 1: Write   (W) — 1 = Write access denied
 *   Bit 2: Execute (E) — 1 = Execute access denied
 *   Bit 3: Delete  (D) — 1 = Delete access denied
 *
 * Example encodings (all denial masks):
 *   0x0000 — S:RWED O:RWED G:RWED W:RWED (full access to all)
 *   0xAA00 — S:RWED O:RWED G:RE   W:RE   (standard VMS default)
 *   0xFF00 — S:RWED O:RWED G:none W:none (group/world denied all)
 *   0xFFFF — all denied for all categories
 *
 * To extract a category's denial nibble:
 *   sys_deny  = (prot >> VMSFS_PROT_SYS_SHIFT) & 0xF
 *   own_deny  = (prot >> VMSFS_PROT_OWN_SHIFT) & 0xF
 *   grp_deny  = (prot >> VMSFS_PROT_GRP_SHIFT) & 0xF
 *   wld_deny  = (prot >> VMSFS_PROT_WLD_SHIFT) & 0xF
 *
 * To test whether a specific access bit is denied for a category:
 *   denied = (prot >> shift) & VMSFS_PROT_R   (for read)
 * ================================================================ */

/* Access bit positions within a 4-bit category nibble */
#define VMSFS_PROT_R            0x1     /* Read    denied */
#define VMSFS_PROT_W            0x2     /* Write   denied */
#define VMSFS_PROT_E            0x4     /* Execute denied */
#define VMSFS_PROT_D            0x8     /* Delete  denied */

/* Bit shift to reach each category's 4-bit denial field */
#define VMSFS_PROT_SYS_SHIFT    0       /* bits  0- 3: System */
#define VMSFS_PROT_OWN_SHIFT    4       /* bits  4- 7: Owner  */
#define VMSFS_PROT_GRP_SHIFT    8       /* bits  8-11: Group  */
#define VMSFS_PROT_WLD_SHIFT    12      /* bits 12-15: World  */

/*
 * MAXSYSGROUP — the SYSGEN parameter that decides which UIC groups get the
 * VMS SYSTEM protection category: every UIC whose GROUP number is <=
 * MAXSYSGROUP (default 8) is System, not only group 0. Pinned (not chosen)
 * to the userspace executive's own copy of this rule
 * (src/libvms/syssvc/sys_security.c, uic_is_system() /
 * src/libvms/include/ovmx_secparam.h's OVMX_MAXSYSGROUP — lab VAX V7.3
 * SYSGEN capture + VSI OpenVMS "UIC Protection" wiki, octal 10 == decimal 8).
 * It is a settable SYSGEN parameter on VMS and a compile-time constant here
 * until OVMX has a SYSGEN parameter store.
 *
 * Lives in the SHARED on-disk header (not the Linux-only vmsfs.h) because
 * every backend's SOGW permission check needs it -- the NetBSD ODS-2 vnode
 * backend (rd vms-e7a) applies the identical System-category rule via
 * kauth_cred_getegid(), mirroring the Linux block-device backend's
 * vmsfs_blkdev_permission() (src/kernel/vmsfs/vmsfs_blkdev.c). One
 * definition, not two independently-drifting ones (INV-DRIFT).
 */
#define VMSFS_MAXSYSGROUP 8

/*
 * Default protection: S:RWED, O:RWED, G:RE, W:RE
 *
 * Standard VMS default — system and owner have full access; group and world
 * may read and execute but not write or delete.
 *
 * Group nibble (bits 8-11):  W=1, D=1, R=0, E=0 → 0xA << 8
 * World nibble (bits 12-15): W=1, D=1, R=0, E=0 → 0xA << 12
 * System (bits 0-3) and Owner (bits 4-7) are both 0 (full access).
 */
#define VMSFS_PROT_DEFAULT      0xAA00

/* ================================================================
 * File header flags
 * ================================================================ */

#define VMSFS_FH_INUSE          0x0001  /* header is allocated */
#define VMSFS_FH_DIRECTORY      0x0002  /* file is a directory */
#define VMSFS_FH_CONTIGUOUS     0x0004  /* file stored contiguously */
#define VMSFS_FH_LOCKED         0x0008  /* file is locked */
#define VMSFS_FH_MARKDEL        0x0010  /* marked for delete */

/* ================================================================
 * Directory entry flags
 * ================================================================ */

#define VMSFS_DE_NEXTREC        0x01    /* continuation record follows */

/* ================================================================
 * Retrieval pointer — maps virtual blocks to volume logical blocks
 *
 * A file's data is addressed by virtual block numbers (VBN, 1-based).
 * Each retrieval pointer maps a contiguous run of VBNs to LBNs:
 *   VBN range [implicit_start .. implicit_start + rp_count - 1]
 *   maps to LBN range [rp_lbn .. rp_lbn + rp_count - 1]
 * ================================================================ */

struct vmsfs_retrieval_ptr {
    uint32_t rp_lbn;             /* starting logical block on volume */
    uint32_t rp_count;           /* number of contiguous blocks */
} __attribute__((packed));

/* ================================================================
 * Home block — block 1 of volume (512 bytes)
 *
 * The superblock equivalent. Describes the volume geometry and
 * locates the bitmap, index file, and data area.
 * ================================================================ */

#define VMSFS_MAX_RETRIEVAL     44      /* max retrieval ptrs per file header */

struct vmsfs_home_block {
    /* Identity — offset 0 */
    uint32_t hb_magic;           /*   0: VMSFS_HOME_MAGIC */
    uint16_t hb_format_ver;      /*   4: on-disk format version */
    uint16_t hb_flags;           /*   6: volume flags */

    /* Timestamps — offset 8 (8-byte aligned) */
    uint64_t hb_created;         /*   8: creation time (Unix epoch seconds) */
    uint64_t hb_modified;        /*  16: last mount/modify time */

    /* Geometry — offset 24 */
    uint32_t hb_block_size;      /*  24: block size in bytes (512) */
    uint32_t hb_total_blocks;    /*  28: total blocks on volume */
    uint32_t hb_free_blocks;     /*  32: free block count */

    /* Bitmap location — offset 36 */
    uint32_t hb_bitmap_lbn;      /*  36: start LBN of storage bitmap */
    uint32_t hb_bitmap_blocks;   /*  40: number of blocks in bitmap */

    /* Index file (file header area) — offset 44 */
    uint32_t hb_index_lbn;       /*  44: start LBN of file header area */
    uint32_t hb_max_files;       /*  48: max file headers allocated */

    /* Data area — offset 52 */
    uint32_t hb_data_lbn;        /*  52: start LBN of data area */

    /* Protection — offset 56 */
    uint16_t hb_cluster_size;    /*  56: allocation cluster (blocks) */
    uint16_t hb_protection;      /*  58: volume protection (SOGW) */
    uint16_t hb_default_prot;    /*  60: default file protection */
    uint16_t hb_reserved1;       /*  62: padding */

    /* Volume name — offset 64 */
    char     hb_volname[12];     /*  64: volume name, space-padded */

    /* Reserved — offset 76 */
    uint8_t  hb_reserved[432];   /*  76: reserved (zero-fill) */

    /* Integrity — offset 508 */
    uint32_t hb_checksum;        /* 508: XOR checksum of preceding bytes */
} __attribute__((packed));

/* ================================================================
 * File header — one per file, one per 512-byte block in index area
 *
 * Analogous to VMS FH2 (File Header Level 2). Contains all metadata
 * for a single file version, including retrieval pointers for locating
 * data blocks on the volume.
 *
 * FID (file ID) = file header number = index into the file header area.
 * FID 0 is invalid. FIDs 1-3 are reserved (INDEXF, BITMAP, MFD).
 * ================================================================ */

struct vmsfs_file_header {
    /* Identity — offset 0 */
    uint32_t fh_magic;           /*   0: VMSFS_FH_MAGIC */
    uint32_t fh_fid;             /*   4: file header number (FID) */

    /* Size — offset 8 (8-byte aligned) */
    uint64_t fh_size;            /*   8: file size in bytes */

    /* Timestamps — offset 16 (8-byte aligned) */
    uint64_t fh_created;         /*  16: creation time */
    uint64_t fh_modified;        /*  24: last modification time */
    uint64_t fh_accessed;        /*  32: last access time */
    uint64_t fh_backup;          /*  40: last backup time */

    /* Allocation — offset 48 */
    uint32_t fh_blocks;          /*  48: allocated blocks */
    uint32_t fh_parent_fid;      /*  52: parent directory FID */

    /* Attributes — offset 56 */
    uint16_t fh_flags;           /*  56: file header flags */
    uint16_t fh_version;         /*  58: file version number */
    uint16_t fh_protection;      /*  60: SOGW protection mask */
    uint16_t fh_uic_group;       /*  62: owner UIC group */
    uint16_t fh_uic_member;      /*  64: owner UIC member */
    uint16_t fh_link_count;      /*  66: link count */
    uint16_t fh_map_count;       /*  68: valid retrieval pointers */
    uint16_t fh_reserved1;       /*  70: reserved */

    /* Name — offset 72 */
    char     fh_name[40];        /*  72: filename, null-terminated */
    char     fh_type[40];        /* 112: file type, null-terminated */

    /* Map area — offset 152 */
    struct vmsfs_retrieval_ptr fh_map[VMSFS_MAX_RETRIEVAL];
                                 /* 152: retrieval pointer array (352 bytes) */

    /* Tail — offset 504 */
    uint32_t fh_reserved2;       /* 504: reserved */
    uint32_t fh_checksum;        /* 508: XOR checksum */
} __attribute__((packed));

/* ================================================================
 * Directory entry — packed sequentially in directory data blocks
 *
 * Fixed-size entries for simplicity. 5 entries per 512-byte block.
 * de_fid == 0 marks a free (deleted) slot.
 *
 * The filename is stored as "NAME.TYPE" in de_name (like "FOO.TXT").
 * Directories have no type component (just "DIRNAME").
 * ================================================================ */

#define VMSFS_DIR_ENTRY_SIZE    88
#define VMSFS_DIR_PER_BLOCK     (VMSFS_BLOCK_SIZE / VMSFS_DIR_ENTRY_SIZE)

struct vmsfs_dir_entry {
    uint32_t de_fid;             /*  0: file header number (0 = free) */
    uint16_t de_version;         /*  4: file version number */
    uint8_t  de_name_len;        /*  6: length of name (excluding NUL) */
    uint8_t  de_flags;           /*  7: entry flags */
    char     de_name[80];        /*  8: "NAME.TYPE" null-terminated */
} __attribute__((packed));

/* ================================================================
 * Checksum — XOR of all uint32_t words in a structure, excluding
 * the checksum field itself (last 4 bytes).
 * ================================================================ */

static inline uint32_t vmsfs_checksum(const void *data, unsigned int size)
{
    const uint32_t *p = (const uint32_t *)data;
    unsigned int words = (size - sizeof(uint32_t)) / sizeof(uint32_t);
    uint32_t sum = 0;
    unsigned int i;

    for (i = 0; i < words; i++)
        sum ^= p[i];

    return sum;
}

/* ================================================================
 * Compile-time size validation
 * ================================================================ */

#ifdef __KERNEL__
#define VMSFS_STATIC_ASSERT(cond, msg) \
    _Static_assert(cond, msg)
#else
#define VMSFS_STATIC_ASSERT(cond, msg) \
    _Static_assert(cond, msg)
#endif

VMSFS_STATIC_ASSERT(sizeof(struct vmsfs_home_block) == VMSFS_BLOCK_SIZE,
    "home block must be exactly 512 bytes");
VMSFS_STATIC_ASSERT(sizeof(struct vmsfs_file_header) == VMSFS_BLOCK_SIZE,
    "file header must be exactly 512 bytes");
VMSFS_STATIC_ASSERT(sizeof(struct vmsfs_dir_entry) == VMSFS_DIR_ENTRY_SIZE,
    "directory entry must be exactly 88 bytes");
VMSFS_STATIC_ASSERT(VMSFS_DIR_PER_BLOCK >= 1,
    "must fit at least 1 directory entry per block");
VMSFS_STATIC_ASSERT(sizeof(struct vmsfs_retrieval_ptr) == 8,
    "retrieval pointer must be exactly 8 bytes");

#endif /* _VMSFS_ONDISK_H */
