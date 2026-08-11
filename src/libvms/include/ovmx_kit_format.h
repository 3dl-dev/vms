/*
 * ovmx_kit_format.h - OVMX product kit container format (vms-0b6).
 *
 * ----------------------------------------------------------------------------
 * CLEAN-ROOM LABELING (Rule 8) -- read this before touching a byte offset.
 * ----------------------------------------------------------------------------
 *
 * OpenVMS installs and layered products arrive as PCSI kit files (PRODUCT
 * INSTALL VMS /SOURCE=...). VSI has never published the PCSI kit's byte
 * layout -- only its *behavior*, observed on real media (both VAX and Alpha,
 * docs/design-vms-faithful-install.md secs 2-3) and documented in the PCSI
 * utility reference: a kit carries product identification (name, producer,
 * version), a file payload, and enough per-file metadata for the installer to
 * place each file at its target VMS filespec with the right protection and
 * ownership. `PRODUCT SHOW PRODUCT` then lists what SOURCE= wrote to the
 * product database.
 *
 * BEHAVIOR-DERIVED (grounded, cited):
 *   - A product kit is one file, self-identifying (name/producer/version),
 *     carrying a set of target-system files. (PCSI utility behavior; the
 *     Alpha oracle capture, design-vms-faithful-install.md sec 2, literally
 *     shows "The following product has been selected: DEC AXPVMS OPENVMS
 *     V8.4 ... Configuring DEC AXPVMS VMS V8.4: OpenVMS Operating System".)
 *   - Per-file protection is the standard 16-bit VMS SOGW mask -- same
 *     mask FAB$W_PRO/XAB$W_PRO/$CRMPSC use. Bit layout is pinned in
 *     ovmx_fileprot.h (VSI $CRMPSC wiki page + worked default-mask example,
 *     fetched 2026-08-09), not re-derived here.
 *   - "SYS$COMMON:" as the common-system-volume logical and "[DIR]NAME.TYPE"
 *     VMS filespec syntax are standard, documented OpenVMS conventions.
 *
 * OVMX-INVENTED (labeled, not VMS-authentic -- nobody has published PCSI's
 * byte layout, so none of the below claims to match it):
 *   - The container's magic, header field layout/order/sizes, the flat
 *     file-index-then-payload arrangement, and the XOR content checksum.
 *   - The product naming SHAPE this OVMX build uses: "OVMX X86VMS VMS"
 *     (echoing the Alpha oracle's "DEC AXPVMS VMS" shape -- vendor, arch
 *     code, "VMS" -- with OVMX's own honest vendor token and an OVMX arch
 *     code for x86-64, never "DEC" or "VSI"). Assembled by the packer from
 *     OVMX_VENDOR_TOKEN (ovmx_identity.h) plus a literal arch/product
 *     suffix -- deliberately NOT OVMX_PRODUCT_NAME (the "OpenVMX" human
 *     brand, rd vms-700): this is a stable machine-read vendor token, not
 *     the brand. The VERSION portion is OVMX_PRODUCT_VERSION, not a second
 *     hardcoded literal (see tools/ovmx_kit_pack.c).
 *   - Default per-file protection/owner-UIC policy for packed files: SYSTEM
 *     UIC [1,4], S:RWED,O:RWED,G:RE,W:RE (0xAA00) -- the OVMX build-tool
 *     convention already used by tools/vmsfs_master.c's MASTER_UIC_* /
 *     VMSFS_PROT_DEFAULT for the exact same class of mastered system files,
 *     applied here independently (this header does not include
 *     vmsfs_ondisk.h -- the kit format has no vmsfs dependency).
 *   - Per-entry installer flags (`ke_flags`, vms-2c9) -- in particular
 *     OVMX_KIT_ENTRY_FLAG_SEED_ONCE, marking a kit-shipped file as a SITE
 *     template (SYSTARTUP_VMS.COM, SYCONFIG.COM, SYLOGICALS.COM) that a
 *     later PRODUCT INSTALL (an upgrade) must never reprovision once it
 *     exists on the target. VSI has never published a PCSI per-entry flags
 *     field; this bitmask, its bit assignment, and its "preserve if already
 *     present" semantics are entirely OVMX's own invention. The underlying
 *     BEHAVIOR it approximates -- real OpenVMS sites are drilled never to
 *     let an OS upgrade touch SYS$MANAGER: startup procedures once seeded
 *     -- is standard, documented VMS system-management practice (Guide to
 *     OpenVMS System Management), just not a fact about PCSI's own kit
 *     format.
 *
 * SCOPE (vms-0b6): this header defines the container only -- what a kit
 * FILE looks like on disk. Building one is tools/ovmx_kit_pack.c (factory
 * tooling, never shipped, never run at boot). Reading one to actually
 * install/register a product (PRODUCT INSTALL, the product database,
 * PRODUCT SHOW PRODUCT) is vms-df9's job, not this file's -- this header
 * gives that consumer one description to include instead of re-deriving the
 * layout, exactly the "one shared header" pattern vmsfs_ondisk.h already
 * established for vmsfs.ko / INITIALIZE.EXE / vmsfs_master.
 *
 * Design record: docs/design-ovmx-kit-format.md.
 *
 * ----------------------------------------------------------------------------
 * ON-DISK LAYOUT
 * ----------------------------------------------------------------------------
 *
 *   +------------------------------------------+  offset 0
 *   | struct ovmx_kit_header (128 bytes)        |
 *   +------------------------------------------+  offset kh_index_offset (128)
 *   | struct ovmx_kit_entry[kh_file_count]      |  (160 bytes each)
 *   |   ...                                     |
 *   +------------------------------------------+  offset kh_payload_offset
 *   | file 0 content (ke_size bytes)            |  (ke_offset of entry 0)
 *   | file 1 content (ke_size bytes)            |
 *   |   ...                                     |
 *   +------------------------------------------+
 *
 * Sequential, single file (CONSTRAINT: it must eventually travel a VMS
 * filesystem) -- no compression, no random-access index beyond the flat
 * entry array, which is small enough to read in one shot before touching
 * any payload byte. All multi-byte fields are native little-endian: OVMX
 * targets only little-endian architectures (x86-64, aarch64), so this is a
 * build-tool convenience, not a portability format.
 */

#ifndef OVMX_KIT_FORMAT_H
#define OVMX_KIT_FORMAT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "ovmx_fileprot.h"   /* VMS_PROT_* bit/shift macros for OVMX_KIT_PROT_DEFAULT */

#ifdef __cplusplus
extern "C" {
#endif

/* 8 raw bytes, no NUL terminator -- compare with memcmp(), not strcmp(). */
#define OVMX_KIT_MAGIC          "OVMXKIT1"
#define OVMX_KIT_MAGIC_LEN      8

#define OVMX_KIT_FORMAT_VERSION 1

#define OVMX_KIT_NAME_MAX       40   /* product name field */
#define OVMX_KIT_PRODUCER_MAX   16   /* producer field */
#define OVMX_KIT_VERSION_MAX    16   /* product version field */
#define OVMX_KIT_FILESPEC_MAX   128  /* target VMS filespec field */

struct ovmx_kit_header {
    char     kh_magic[OVMX_KIT_MAGIC_LEN];      /*   0: OVMX_KIT_MAGIC */
    uint32_t kh_format_version;                 /*   8: OVMX_KIT_FORMAT_VERSION */
    uint32_t kh_reserved0;                      /*  12 */
    char     kh_product_name[OVMX_KIT_NAME_MAX];    /*  16: e.g. "OVMX X86VMS VMS" */
    char     kh_producer[OVMX_KIT_PRODUCER_MAX];    /*  56: e.g. "OVMX" */
    char     kh_product_version[OVMX_KIT_VERSION_MAX]; /* 72: OVMX_PRODUCT_VERSION, verbatim */
    uint64_t kh_build_time;                     /*  88: seconds since epoch */
    uint32_t kh_file_count;                     /*  96 */
    uint32_t kh_reserved1;                      /* 100 */
    uint64_t kh_index_offset;                   /* 104: == sizeof(header) == 128 */
    uint64_t kh_payload_offset;                 /* 112: index_offset + file_count*sizeof(entry) */
    uint32_t kh_checksum;                       /* 120: ovmx_kit_checksum() over the header with this field zeroed */
    uint32_t kh_reserved2;                      /* 124 */
} __attribute__((packed));                      /* 128 bytes total */

/*
 * ke_flags bit assignments (vms-2c9, OVMX-INVENTED, Rule 8 -- see the file
 * header comment). This field occupies what was previously `ke_reserved`
 * (uint16_t, offset 134): the struct's size and layout do not change, so
 * this is NOT a kit format version bump.
 *
 * BACKWARD COMPATIBLE BY CONSTRUCTION: tools/ovmx_kit_pack.c has always
 * calloc()d its entry array, so every kit built before this flag existed
 * has these two bytes zeroed on disk. 0 means "no flags" -- a pre-vms-2c9
 * kit installs exactly as it always has (src/product/product.c's
 * do_install() writes every listed file unconditionally, every time,
 * including on an upgrade). Only a kit packed by a vms-2c9-or-later
 * ovmx_kit_pack sets OVMX_KIT_ENTRY_FLAG_SEED_ONCE, and only for the
 * site-customizable files it chooses to (tools/ovmx_kit_pack.c's
 * is_seed_once_filename()).
 */
#define OVMX_KIT_ENTRY_FLAG_SEED_ONCE  ((uint16_t)0x0001)
    /* Write this entry's file only if the target does NOT already exist on
     * the destination volume (a fresh install, or an upgrade target that
     * has never had this file). If the target already exists (an upgrade
     * over an already-populated destination), PRESERVE it -- do not
     * truncate/overwrite, and do not touch its protection or owner UIC.
     * See src/product/product.c do_install(). */

struct ovmx_kit_entry {
    char     ke_filespec[OVMX_KIT_FILESPEC_MAX]; /*   0: target VMS filespec, e.g. "SYS$COMMON:[SYSEXE]DCL.EXE" */
    uint16_t ke_protection;                      /* 128: VMS SOGW mask (ovmx_fileprot.h encoding) */
    uint16_t ke_uic_group;                       /* 130: owner UIC group */
    uint16_t ke_uic_member;                      /* 132: owner UIC member */
    uint16_t ke_flags;                           /* 134: OVMX_KIT_ENTRY_FLAG_* bitmask; 0 on every kit predating vms-2c9 */
    uint64_t ke_size;                            /* 136: file content length in bytes */
    uint64_t ke_offset;                          /* 144: absolute byte offset of content in the kit file */
    uint32_t ke_checksum;                        /* 152: ovmx_kit_checksum() over the file content */
    uint32_t ke_reserved2;                       /* 156 */
} __attribute__((packed));                       /* 160 bytes total */

/*
 * ovmx_kit_checksum - simple XOR-fold checksum over an arbitrary-length
 * buffer. Same non-cryptographic "catch accidental corruption/truncation"
 * role as vmsfs_ondisk.h's vmsfs_checksum(), reimplemented independently
 * (word-XOR generalized to any length, since file content is not
 * word-aligned in general) so this header carries no vmsfs dependency.
 */
static inline uint32_t ovmx_kit_checksum(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i + 4 <= len; i += 4) {
        uint32_t word;
        memcpy(&word, p + i, 4);
        sum ^= word;
    }
    for (; i < len; i++)
        sum ^= ((uint32_t)p[i]) << (8 * (unsigned)(i % 4));

    return sum;
}

/*
 * OVMX build-tool default protection for packed files: S:RWED,O:RWED,
 * G:RE,W:RE. Computed from ovmx_fileprot.h's pinned bit layout rather than
 * a bare literal, and numerically identical to vmsfs_ondisk.h's
 * VMSFS_PROT_DEFAULT (0xAA00) -- the same policy tools/vmsfs_master.c
 * already applies to mastered system files, arrived at independently here.
 */
#define OVMX_KIT_PROT_DEFAULT \
    ((uint16_t)(((VMS_PROT_D | VMS_PROT_W) << VMS_PROT_WORLD_SHIFT) | \
                ((VMS_PROT_D | VMS_PROT_W) << VMS_PROT_GROUP_SHIFT) | \
                (VMS_PROT_ALL_ALLOWED    << VMS_PROT_OWNER_SHIFT)  | \
                (VMS_PROT_ALL_ALLOWED    << VMS_PROT_SYSTEM_SHIFT)))

/* OVMX build-tool default owner UIC for packed files: SYSTEM, [1,4]. */
#define OVMX_KIT_UIC_GROUP_DEFAULT   1
#define OVMX_KIT_UIC_MEMBER_DEFAULT  4

#ifdef __cplusplus
}
#endif

#endif /* OVMX_KIT_FORMAT_H */
