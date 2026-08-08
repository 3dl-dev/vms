/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ods2.h - GENUINE ODS-2 (Files-11 On-Disk Structure Level 2) definitions.
 *
 * Increment 1 of the genuine-ODS-2 effort (see docs/research-ods2-linux-drivers.md).
 * The GOAL of these structures is byte-compatibility with real OpenVMS Files-11
 * volumes, so that a real VAX/Alpha can eventually MOUNT an OVMX-served cluster
 * volume. This is DELIBERATELY DIFFERENT from src/kernel/vmsfs/vmsfs_ondisk.h,
 * which is an "ODS-2-inspired" format with OVMX's own magic ("VMFS"/"VFH2") and
 * is NOT byte-genuine. Nothing here shares bytes with that file.
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md Rule 8): every layout below is derived ONLY
 * from published open-source prior art and public documentation. NO VSI/HPE
 * source, binaries, or leaked material were consulted.
 *
 *   [N] Paul Nankervis's ODS2, via simh/simtools (public, cross-validated by
 *       VMS hobbyists reading real disks):
 *       https://github.com/simh/simtools/tree/master/extracters/ods2
 *       - struct HOME / struct HEAD / struct IDENT / struct RECATTR / struct
 *         fiddef / struct UIC : extracters/ods2/access.h
 *       - retrieval-pointer (map) FM2 format decode : extracters/ods2/access.c
 *         getwindow()
 *       - 16-bit additive checksum : extracters/ods2/access.c checksum()
 *       - volume format string "DECFILE11B  " : extracters/ods2/access.c
 *       - directory record layout : extracters/ods2/direct.h
 *   [S] Public "Files-11 On-Disk Structure" description (VSI OpenVMS wiki,
 *       Wikipedia "Files-11"): home block / SCB / file header roles, and the
 *       structure-level word encoding (level 2, version 1 => 0x0201).
 *
 * BYTE-GENUINENESS IS NOT YET PROVEN. No real VMS-made ODS-2 image has been
 * parsed by this reader. The flagged next increment is to validate against a
 * real OpenVMS VAX 7.3 volume from lab-1 (see the test file and the PR body).
 * Fields marked "[OVMX-inferred]" below are those whose exact byte offset is
 * NOT cleanly published; they are labelled per Rule 8 and must be confirmed
 * against a real image before being relied upon.
 */

#ifndef _VMSFS_ODS2_H
#define _VMSFS_ODS2_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Constants
 * ================================================================ */

/* ODS-2 uses 512-byte logical blocks. [S] */
#define ODS2_BLOCK_SIZE         512

/*
 * Structure level word: high byte = structure level, low byte = version.
 * ODS-2 == Files-11 structure level 2, version 1 => 0x0201. [S]
 * (Nankervis's reader does not itself range-check this word; the value is
 *  taken from the public Files-11 structure-level encoding and is FLAGGED for
 *  oracle confirmation against a real image.)
 */
#define ODS2_STRUCLEV_V2        0x0201u
#define ODS2_STRUCLEV_LEVEL(w)  (((w) >> 8) & 0xFF)
#define ODS2_STRUCLEV_VERSION(w) ((w) & 0xFF)

/* Volume format identifier stored in hm2_format (12 bytes, space padded). [N] */
#define ODS2_FORMAT_STRING      "DECFILE11B  "
#define ODS2_FORMAT_LEN         12

/*
 * Reserved file IDs. The index file INDEXF.SYS is always file number 1.
 * BITMAP.SYS (storage bitmap) is file 2; 000000.DIR (MFD) is file 4. [S]
 */
#define ODS2_FID_INDEXF         1   /* INDEXF.SYS                          */
#define ODS2_FID_BITMAP         2   /* BITMAP.SYS (SCB + storage bitmap)   */
#define ODS2_FID_BADBLK         3   /* BADBLK.SYS                          */
#define ODS2_FID_MFD            4   /* 000000.DIR (master file directory)  */
#define ODS2_FID_CORIMG         5   /* CORIMG.SYS                          */

/* ================================================================
 * Primitive VMS on-disk types
 * ================================================================ */

#pragma pack(push, 1)

/* User Identification Code (owner). [N] access.h struct UIC */
typedef struct ods2_uic {
    uint16_t uic_member;    /* member number */
    uint16_t uic_group;     /* group number  */
} ods2_uic_t;

/*
 * File identifier (FID). 48 bits: 24-bit file number (num + nmx<<16),
 * sequence number, and relative volume number. [N] access.h struct fiddef
 */
typedef struct ods2_fid {
    uint16_t fid_num;       /* fid$w_num : low 16 bits of file number */
    uint16_t fid_seq;       /* fid$w_seq : sequence number            */
    uint8_t  fid_rvn;       /* fid$b_rvn : relative volume number     */
    uint8_t  fid_nmx;       /* fid$b_nmx : high 8 bits of file number */
} ods2_fid_t;

/* Full 24-bit file number from a FID. [N] */
static inline uint32_t ods2_fid_number(const ods2_fid_t *f)
{
    return (uint32_t)f->fid_num | ((uint32_t)f->fid_nmx << 16);
}

/*
 * Record attributes area (FAT). 32 bytes. [N] access.h struct RECATTR.
 * hiblk/efblk are stored as two 16-bit halves (VAX word-swapped longword):
 * value = low + (high << 16).
 */
typedef struct ods2_recattr {
    uint8_t  fat_rtype;         /*  0: record type / file organization */
    uint8_t  fat_rattrib;       /*  1: record attributes               */
    uint16_t fat_rsize;         /*  2: record size                     */
    uint16_t fat_hiblk[2];      /*  4: highest allocated VBN (lo,hi)    */
    uint16_t fat_efblk[2];      /*  8: end-of-file VBN (lo,hi)          */
    uint16_t fat_ffbyte;        /* 12: first free byte in EOF block     */
    uint8_t  fat_bktsize;       /* 14: bucket size                      */
    uint8_t  fat_vfcsize;       /* 15: VFC header size                  */
    uint16_t fat_maxrec;        /* 16: maximum record size              */
    uint16_t fat_defext;        /* 18: default extend quantity          */
    uint16_t fat_gbc;           /* 20: global buffer count              */
    uint8_t  fat_reserved[8];   /* 22: reserved (fat$_UU0)              */
    uint16_t fat_versions;      /* 30: default version limit            */
} ods2_recattr_t;

/* Reconstruct fat_hiblk / fat_efblk as a 32-bit VBN. [N] */
static inline uint32_t ods2_recattr_hiblk(const ods2_recattr_t *r)
{
    return (uint32_t)r->fat_hiblk[0] | ((uint32_t)r->fat_hiblk[1] << 16);
}
static inline uint32_t ods2_recattr_efblk(const ods2_recattr_t *r)
{
    return (uint32_t)r->fat_efblk[0] | ((uint32_t)r->fat_efblk[1] << 16);
}

/* ================================================================
 * HOME BLOCK  (512 bytes) - [N] access.h struct HOME
 *
 * The volume "superblock". Locates the index file bitmap, records the
 * cluster factor, and carries the volume label + format string. Two
 * additive checksums: hm2_checksum1 covers the first 29 words (bytes
 * 0..57); hm2_checksum2 covers all 255 words (bytes 0..509). [N]
 * ================================================================ */
typedef struct ods2_home {
    uint32_t hm2_homelbn;       /*   0: LBN of this home block            */
    uint32_t hm2_alhomelbn;     /*   4: LBN of alternate home block       */
    uint32_t hm2_altidxlbn;     /*   8: LBN of alternate index header     */
    uint16_t hm2_struclev;      /*  12: structure level+version (0x0201)  */
    uint16_t hm2_cluster;       /*  14: storage bitmap cluster factor     */
    uint16_t hm2_homevbn;       /*  16: VBN of home block in INDEXF.SYS   */
    uint16_t hm2_alhomevbn;     /*  18: VBN of alternate home block       */
    uint16_t hm2_altidxvbn;     /*  20: VBN of alternate index header     */
    uint16_t hm2_ibmapvbn;      /*  22: VBN of index file bitmap          */
    uint32_t hm2_ibmaplbn;      /*  24: LBN of index file bitmap          */
    uint32_t hm2_maxfiles;      /*  28: maximum number of files           */
    uint16_t hm2_ibmapsize;     /*  32: index file bitmap size (blocks)   */
    uint16_t hm2_resfiles;      /*  34: number of reserved files          */
    uint16_t hm2_devtype;       /*  36: disk device type                  */
    uint16_t hm2_rvn;           /*  38: relative volume number            */
    uint16_t hm2_setcount;      /*  40: count of volumes in set           */
    uint16_t hm2_volchar;       /*  42: volume characteristics            */
    ods2_uic_t hm2_volowner;    /*  44: volume owner UIC                  */
    uint32_t hm2_reserved1;     /*  48: reserved                          */
    uint16_t hm2_protect;       /*  52: volume protection mask            */
    uint16_t hm2_fileprot;      /*  54: default file protection           */
    uint16_t hm2_reserved2;     /*  56: reserved                          */
    uint16_t hm2_checksum1;     /*  58: checksum of first 29 words        */
    uint8_t  hm2_credate[8];    /*  60: volume creation date (VMS time)   */
    uint8_t  hm2_window;        /*  68: default window size               */
    uint8_t  hm2_lru_lim;       /*  69: directory LRU limit               */
    uint16_t hm2_extend;        /*  70: default extend quantity           */
    uint8_t  hm2_retainmin[8];  /*  72: minimum retention period          */
    uint8_t  hm2_retainmax[8];  /*  80: maximum retention period          */
    uint8_t  hm2_revdate[8];    /*  88: volume revision date              */
    uint8_t  hm2_min_class[20]; /*  96: minimum security class            */
    uint8_t  hm2_max_class[20]; /* 116: maximum security class            */
    uint8_t  hm2_reserved3[320];/* 136: reserved                          */
    uint32_t hm2_serialnum;     /* 456: pack serial number                */
    char     hm2_strucname[12]; /* 460: structure (volume set) name       */
    char     hm2_volname[12];   /* 472: volume label                      */
    char     hm2_ownername[12]; /* 484: volume owner name                 */
    char     hm2_format[12];    /* 496: format id "DECFILE11B  "          */
    uint16_t hm2_reserved4;     /* 508: reserved                          */
    uint16_t hm2_checksum2;     /* 510: checksum of all 255 words         */
} ods2_home_t;

/* ================================================================
 * FILE HEADER  (FH2, 512 bytes) - [N] access.h struct HEAD
 *
 * One per file (primary header) plus zero or more extension headers.
 * The header is divided into areas located by WORD offsets from the
 * start of the header:
 *   fh2_idoffset -> ident area   (struct ods2_ident)
 *   fh2_mpoffset -> map area     (retrieval pointers, FM2 formats)
 *   fh2_acoffset -> access area  (ACLs)
 *   fh2_rsoffset -> reserved area
 * fh2_map_inuse counts the WORDS currently used in the map area.
 * fh2_checksum (offset 510) is the 16-bit additive checksum of the
 * first 255 words. [N]
 * ================================================================ */
typedef struct ods2_fh2 {
    uint8_t  fh2_idoffset;      /*   0: ident area offset   (words) */
    uint8_t  fh2_mpoffset;      /*   1: map area offset      (words) */
    uint8_t  fh2_acoffset;      /*   2: access area offset   (words) */
    uint8_t  fh2_rsoffset;      /*   3: reserved area offset (words) */
    uint16_t fh2_seg_num;       /*   4: extension segment number    */
    uint16_t fh2_struclev;      /*   6: structure level+version     */
    ods2_fid_t fh2_fid;         /*   8: this file's FID             */
    ods2_fid_t fh2_ext_fid;     /*  14: next extension header FID   */
    ods2_recattr_t fh2_recattr; /*  20: record attributes (FAT)     */
    uint32_t fh2_filechar;      /*  52: file characteristics        */
    uint16_t fh2_reserved1;     /*  56: reserved                    */
    uint8_t  fh2_map_inuse;     /*  58: words used in map area       */
    uint8_t  fh2_acc_mode;      /*  59: accessor privilege level     */
    ods2_uic_t fh2_fileowner;   /*  60: file owner UIC               */
    uint16_t fh2_fileprot;      /*  64: file protection mask         */
    ods2_fid_t fh2_backlink;    /*  66: directory back-link FID      */
    uint8_t  fh2_journal;       /*  72: journal control flags        */
    uint8_t  fh2_ru_active;     /*  73: recovery-unit active         */
    uint16_t fh2_reserved2;     /*  74: reserved                     */
    uint32_t fh2_highwater;     /*  76: highwater mark (VBN)         */
    uint8_t  fh2_reserved3[8];  /*  80: reserved                     */
    uint8_t  fh2_class_prot[20];/*  88: classification / protection  */
    uint8_t  fh2_restofit[402]; /* 108: ident+map+access areas live  */
                                /*      here, located by the offsets */
    uint16_t fh2_checksum;      /* 510: additive checksum, 255 words */
} ods2_fh2_t;

/* ================================================================
 * IDENT AREA  (FI2, 120 bytes) - [N] access.h struct IDENT
 *
 * Located at (fh2_idoffset * 2) bytes from the start of the header.
 * Carries the file name and the four VMS timestamps.
 * ================================================================ */
typedef struct ods2_ident {
    char     fi2_filename[20];  /*  0: file name (space padded)      */
    uint16_t fi2_revision;      /* 20: revision number               */
    uint8_t  fi2_credate[8];    /* 22: creation date                 */
    uint8_t  fi2_revdate[8];    /* 30: revision date                 */
    uint8_t  fi2_expdate[8];    /* 38: expiration date               */
    uint8_t  fi2_bakdate[8];    /* 46: backup date                   */
    char     fi2_filenamext[66];/* 54: filename extension (ODS-2:    */
                                /*     name.type;ver continuation)   */
} ods2_ident_t;

/* ================================================================
 * STORAGE CONTROL BLOCK  (SCB) - block 1 (VBN 1) of BITMAP.SYS.
 *
 * The SCB carries volume summary info used for space allocation:
 * structure level, cluster factor, and volume size in blocks. [S]
 * Only the leading fields have a cleanly published byte layout; the
 * remaining fields are [OVMX-inferred] and MUST be confirmed against a
 * real BITMAP.SYS from lab-1 before being trusted (Rule 8). The trailing
 * word (offset 510) is the additive checksum, as with the home block. [S]
 * ================================================================ */
typedef struct ods2_scb {
    uint16_t scb_struclev;      /*   0: structure level+version (0x0201) [S] */
    uint16_t scb_cluster;       /*   2: storage bitmap cluster factor    [S] */
    uint32_t scb_volsize;       /*   4: volume size in blocks            [S] */
    uint32_t scb_blksize;       /*   8: [OVMX-inferred] blocking factor      */
    uint32_t scb_sectors;       /*  12: [OVMX-inferred] sectors per track    */
    uint32_t scb_tracks;        /*  16: [OVMX-inferred] tracks per cylinder  */
    uint32_t scb_cylinders;     /*  20: [OVMX-inferred] cylinders            */
    uint32_t scb_status;        /*  24: [OVMX-inferred] volume status        */
    uint32_t scb_status2;       /*  28: [OVMX-inferred] volume status 2      */
    uint16_t scb_writecnt;      /*  32: [OVMX-inferred] mount write count    */
    char     scb_volockname[12];/*  34: [OVMX-inferred] volume lock name     */
    uint8_t  scb_reserved[464]; /*  46: [OVMX-inferred] reserved             */
    uint16_t scb_checksum;      /* 510: additive checksum, 255 words     [S] */
} ods2_scb_t;

/* ================================================================
 * DIRECTORY RECORD  - [N] direct.h struct dir$rec / struct dir$ent
 *
 * A directory data block is a sequence of variable-length records. Each
 * record header is followed by a name of dir_namecount bytes, then (after
 * word alignment) one or more value entries {version, fid} in DESCENDING
 * version order. dir_size == 0xFFFF marks the end of records in the block.
 * ================================================================ */
#define ODS2_DIR_END        0xFFFFu   /* dir_size sentinel: no more records */

typedef struct ods2_dir_rec {
    uint16_t dir_size;          /* bytes in record AFTER this size word */
    uint16_t dir_verlimit;      /* version limit                        */
    uint8_t  dir_flags;         /* name type / value type               */
    uint8_t  dir_namecount;     /* length of dir_name                    */
    char     dir_name[1];       /* variable: dir_namecount bytes         */
} ods2_dir_rec_t;

typedef struct ods2_dir_ent {
    uint16_t dir_version;       /* file version number */
    ods2_fid_t dir_fid;         /* file identifier     */
} ods2_dir_ent_t;

#pragma pack(pop)

/* ================================================================
 * Compile-time byte-layout assertions.
 *
 * These lock the offsets/sizes to the cited public spec. If a compiler
 * ever changes packing behaviour, the build fails rather than silently
 * emitting a non-genuine layout.
 * ================================================================ */
_Static_assert(sizeof(ods2_uic_t)     == 4,   "UIC must be 4 bytes");
_Static_assert(sizeof(ods2_fid_t)     == 6,   "FID must be 6 bytes");
_Static_assert(sizeof(ods2_recattr_t) == 32,  "RECATTR must be 32 bytes");

_Static_assert(sizeof(ods2_home_t)    == 512, "HOME block must be 512 bytes");
_Static_assert(offsetof(ods2_home_t, hm2_struclev)  == 12,  "hm2_struclev@12");
_Static_assert(offsetof(ods2_home_t, hm2_cluster)   == 14,  "hm2_cluster@14");
_Static_assert(offsetof(ods2_home_t, hm2_ibmaplbn)  == 24,  "hm2_ibmaplbn@24");
_Static_assert(offsetof(ods2_home_t, hm2_maxfiles)  == 28,  "hm2_maxfiles@28");
_Static_assert(offsetof(ods2_home_t, hm2_checksum1) == 58,  "hm2_checksum1@58");
_Static_assert(offsetof(ods2_home_t, hm2_serialnum) == 456, "hm2_serialnum@456");
_Static_assert(offsetof(ods2_home_t, hm2_strucname) == 460, "hm2_strucname@460");
_Static_assert(offsetof(ods2_home_t, hm2_volname)   == 472, "hm2_volname@472");
_Static_assert(offsetof(ods2_home_t, hm2_format)    == 496, "hm2_format@496");
_Static_assert(offsetof(ods2_home_t, hm2_checksum2) == 510, "hm2_checksum2@510");

_Static_assert(sizeof(ods2_fh2_t)     == 512, "FILE HEADER must be 512 bytes");
_Static_assert(offsetof(ods2_fh2_t, fh2_seg_num)  == 4,   "fh2_seg_num@4");
_Static_assert(offsetof(ods2_fh2_t, fh2_fid)      == 8,   "fh2_fid@8");
_Static_assert(offsetof(ods2_fh2_t, fh2_recattr)  == 20,  "fh2_recattr@20");
_Static_assert(offsetof(ods2_fh2_t, fh2_filechar) == 52,  "fh2_filechar@52");
_Static_assert(offsetof(ods2_fh2_t, fh2_map_inuse)== 58,  "fh2_map_inuse@58");
_Static_assert(offsetof(ods2_fh2_t, fh2_fileowner)== 60,  "fh2_fileowner@60");
_Static_assert(offsetof(ods2_fh2_t, fh2_backlink) == 66,  "fh2_backlink@66");
_Static_assert(offsetof(ods2_fh2_t, fh2_highwater)== 76,  "fh2_highwater@76");
_Static_assert(offsetof(ods2_fh2_t, fh2_checksum) == 510, "fh2_checksum@510");

_Static_assert(sizeof(ods2_ident_t)   == 120, "IDENT area must be 120 bytes");
_Static_assert(offsetof(ods2_ident_t, fi2_revision) == 20, "fi2_revision@20");
_Static_assert(offsetof(ods2_ident_t, fi2_credate)  == 22, "fi2_credate@22");

_Static_assert(sizeof(ods2_scb_t)     == 512, "SCB must be 512 bytes");
_Static_assert(offsetof(ods2_scb_t, scb_volsize)  == 4,   "scb_volsize@4");
_Static_assert(offsetof(ods2_scb_t, scb_checksum) == 510, "scb_checksum@510");

/* ================================================================
 * READER API  (implemented in ods2/ods2_reader.c)
 * ================================================================ */

/* Reader status codes (reader-local; not VMS $SSDEF values). */
typedef enum ods2_status {
    ODS2_OK = 0,
    ODS2_ERR_ARGS,          /* bad arguments                          */
    ODS2_ERR_SIZE,          /* buffer/image too small                 */
    ODS2_ERR_CHECKSUM,      /* stored checksum mismatch               */
    ODS2_ERR_FORMAT,        /* format string / structure level bad    */
    ODS2_ERR_RANGE,         /* LBN/VBN out of range                   */
    ODS2_ERR_NOTFOUND       /* file / entry not found                 */
} ods2_status_t;

const char *ods2_strerror(ods2_status_t st);

/*
 * 16-bit additive checksum over `count` little-endian 16-bit words. [N]
 * The Files-11 block checksum sums the first 255 words; the stored
 * checksum sits in the 256th word (byte offset 510).
 */
uint16_t ods2_checksum(const void *block, unsigned count);

/* Convenience: checksum of the first 255 words of a 512-byte block. */
static inline uint16_t ods2_block_checksum(const void *block)
{
    return ods2_checksum(block, 255);
}

/*
 * Parse and validate a home block from a raw 512-byte buffer into `out`.
 * Validates BOTH additive checksums and the "DECFILE11B  " format string.
 * `strict_level` != 0 also requires hm2_struclev == ODS2_STRUCLEV_V2.
 */
ods2_status_t ods2_home_parse(const void *block, size_t block_len,
                              ods2_home_t *out, int strict_level);

/*
 * Parse and validate a file header (FH2) from a raw 512-byte buffer into
 * `out`. Validates the additive checksum.
 */
ods2_status_t ods2_fh2_parse(const void *block, size_t block_len,
                             ods2_fh2_t *out);

/* Locate the ident area inside a validated header block. */
const ods2_ident_t *ods2_fh2_ident(const void *header_block);

/*
 * Retrieval pointer (one contiguous VBN->LBN run) decoded from the map area.
 */
typedef struct ods2_extent {
    uint32_t lbn;       /* starting logical block number on the volume */
    uint32_t count;     /* number of contiguous blocks                 */
} ods2_extent_t;

/* Callback for ods2_fh2_map_walk; return non-zero to stop early. */
typedef int (*ods2_map_cb)(const ods2_extent_t *ext, void *ctx);

/*
 * Walk the FM2 retrieval pointers in a validated header block, invoking
 * `cb` once per extent. Decodes format 0/1/2/3 per Nankervis getwindow(). [N]
 * Returns the number of extents visited via *n_out (may be NULL).
 */
ods2_status_t ods2_fh2_map_walk(const void *header_block,
                                ods2_map_cb cb, void *ctx, unsigned *n_out);

/* ---- Volume-level reader over an in-memory ODS-2 image ---- */

typedef struct ods2_volume {
    const uint8_t *image;   /* whole volume image                 */
    size_t         image_len;
    uint32_t       nblocks; /* image_len / 512                    */
    ods2_home_t    home;    /* parsed, validated home block       */
} ods2_volume_t;

/*
 * Attach a reader to an in-memory volume image. Locates the home block
 * (at hm2_homelbn / standard LBN 1), validating checksums and format, and
 * caches the parsed home block in `vol`.
 */
ods2_status_t ods2_volume_open(ods2_volume_t *vol,
                               const void *image, size_t image_len);

/* Pointer to raw block `lbn` in the image, or NULL if out of range. */
const uint8_t *ods2_volume_block(const ods2_volume_t *vol, uint32_t lbn);

/*
 * Read the primary file header for file number `fid_num` by walking
 * INDEXF.SYS. INDEXF.SYS headers begin at hm2_ibmaplbn + hm2_ibmapsize,
 * one 512-byte header per file. Copies the validated header block into
 * `header_out` (must be >= 512 bytes).
 */
ods2_status_t ods2_volume_read_header(const ods2_volume_t *vol,
                                      uint32_t fid_num,
                                      void *header_out, size_t out_len);

/* Directory listing callback: one call per {name, version, fid}. */
typedef int (*ods2_dir_cb)(const char *name, unsigned name_len,
                           uint16_t version, const ods2_fid_t *fid,
                           void *ctx);

/*
 * List a directory given the directory file's header block. Walks the
 * directory's data blocks via its retrieval pointers and decodes the
 * variable-length directory records. [N]
 */
ods2_status_t ods2_volume_list_dir(const ods2_volume_t *vol,
                                   const void *dir_header_block,
                                   ods2_dir_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* _VMSFS_ODS2_H */
