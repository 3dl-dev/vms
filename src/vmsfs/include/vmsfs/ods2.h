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
 * BYTE-GENUINENESS, INCREMENT 2 (this revision): validated against a REAL
 * OpenVMS VAX V7.3 volume -- INITIALIZE'd, populated, and DISMOUNTed on
 * lab-2 (`vaxlab-9`, since lab-1 was in active use elsewhere), the raw image
 * pulled out byte-for-byte. See tests/ods2/real_vax_ods2.dsk (the fixture),
 * tests/ods2/PROVENANCE-real_vax_ods2.md (exact commands + observed values),
 * and tests/ods2/test_ods2_real.c (the test that drives this reader over it).
 * Home block (checksums + "DECFILE11B  " + struclev 0x0201), the INDEXF.SYS
 * and BITMAP.SYS/directory/data FH2 headers, FM2 retrieval-pointer decode,
 * and directory listing all parsed the real image byte-exact -- no reader
 * changes were needed for those paths. The SCB (struct ods2_scb below) DID
 * need correction; see its per-field comments. Fields still marked
 * "[OVMX-inferred]" below remain genuinely unconfirmed (single real sample,
 * no induced-error test) and must not be relied on without further
 * oracle work; this is recorded per Rule 8, not asserted as fact.
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
 *
 * FIDs 6-10 (VOLSET.SYS, CONTIN.SYS, BACKUP.SYS, BADLOG.SYS, SECURITY.SYS)
 * and hm2_resfiles == 10 are NOT from public docs -- they were read directly
 * off the real-VAX fixture tests/ods2/real_vax_ods2.dsk (the same increment-2
 * fixture, see PROVENANCE-real_vax_ods2.md) by decoding each reserved
 * header's ident area, as part of increment 3 (the writer). This is
 * observed-oracle grounding under Rule 8, not a public-doc citation.
 */
#define ODS2_FID_INDEXF         1   /* INDEXF.SYS                          */
#define ODS2_FID_BITMAP         2   /* BITMAP.SYS (SCB + storage bitmap)   */
#define ODS2_FID_BADBLK         3   /* BADBLK.SYS                          */
#define ODS2_FID_MFD            4   /* 000000.DIR (master file directory)  */
#define ODS2_FID_CORIMG         5   /* CORIMG.SYS                          */
#define ODS2_FID_VOLSET         6   /* VOLSET.SYS   [real-fixture oracle]  */
#define ODS2_FID_CONTIN         7   /* CONTIN.SYS   [real-fixture oracle]  */
#define ODS2_FID_BACKUP         8   /* BACKUP.SYS   [real-fixture oracle]  */
#define ODS2_FID_BADLOG         9   /* BADLOG.SYS   [real-fixture oracle]  */
#define ODS2_FID_SECURITY       10  /* SECURITY.SYS [real-fixture oracle]  */
#define ODS2_RESFILES           10  /* hm2_resfiles observed on real fixture */

/*
 * SECURITY.SYS data allocation: 6 contiguous blocks (VBN1..6), matching the
 * real fixture's observed retrieval-pointer count (see PROVENANCE-real_vax_
 * ods2.md's increment-3 addendum: "SECURITY.SYS;1 ... map_inuse=2 (1 extent,
 * CONTIG)" with a decoded block count of 6). Only VBN1 carries content
 * (increment 4, [F6] below); VBN2..6 stay zeroed, matching the real fixture.
 */
#define ODS2_SECURITY_DATA_BLOCKS 6

/*
 * File characteristics bits (fh2_filechar). [N] access.h FH2$M_* defines.
 * Only the two this writer needs are pulled in; access.h also defines
 * FH2$M_NOBACKUP (0x2), FH2$M_MARKDEL (0x8000), FH2$M_ERASE (0x20000).
 */
#define ODS2_FH2_M_CONTIG       0x0080u   /* contiguous allocation   [N] */
#define ODS2_FH2_M_DIRECTORY    0x2000u   /* file is a directory     [N] */

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
 *
 * hiblk/efblk are stored as two 16-bit halves. CORRECTED in increment 3
 * (the ODS-2 writer, src/vmsfs/ods2/ods2_writer.c): the field is (hi, lo)
 * word order, NOT (lo, hi) as increment 1 assumed -- value = word[1] +
 * (word[0] << 16). This was never oracle-checked in increments 1-2 (the
 * PROVENANCE confirmed-list never mentions hiblk/efblk); increment 3
 * decoded fat_hiblk on all 13 real files in tests/ods2/real_vax_ods2.dsk
 * and cross-checked the corrected formula against each file's OWN FM2
 * map-derived block count (independently decoded, not read from any
 * field) -- exact match on all 13 (INDEXF.SYS=21, BITMAP.SYS=2,
 * 000000.DIR=2, SECURITY.SYS=6, OVMXDIR.DIR=1, HELLO.TXT=34, WORLD.TXT=2,
 * and 0 for every zero-length reserved stub); the OLD (lo, hi) formula
 * produced values in the hundred-thousands, off by exactly a factor of
 * 65536 on every one. See tests/ods2/PROVENANCE-real_vax_ods2.md's
 * increment-3 addendum.
 */
typedef struct ods2_recattr {
    uint8_t  fat_rtype;         /*  0: record type / file organization */
    uint8_t  fat_rattrib;       /*  1: record attributes               */
    uint16_t fat_rsize;         /*  2: record size                     */
    uint16_t fat_hiblk[2];      /*  4: highest allocated VBN (hi,lo)    */
    uint16_t fat_efblk[2];      /*  8: end-of-file VBN (hi,lo)          */
    uint16_t fat_ffbyte;        /* 12: first free byte in EOF block     */
    uint8_t  fat_bktsize;       /* 14: bucket size                      */
    uint8_t  fat_vfcsize;       /* 15: VFC header size                  */
    uint16_t fat_maxrec;        /* 16: maximum record size              */
    uint16_t fat_defext;        /* 18: default extend quantity          */
    uint16_t fat_gbc;           /* 20: global buffer count              */
    uint8_t  fat_reserved[8];   /* 22: reserved (fat$_UU0)              */
    uint16_t fat_versions;      /* 30: default version limit            */
} ods2_recattr_t;

/*
 * Reconstruct fat_hiblk / fat_efblk as a 32-bit VBN: word[1] is the LOW
 * half, word[0] is the HIGH half -- see the ods2_recattr_t comment for the
 * increment-3 real-fixture correction (increments 1-2 had this backwards).
 */
static inline uint32_t ods2_recattr_hiblk(const ods2_recattr_t *r)
{
    return (uint32_t)r->fat_hiblk[1] | ((uint32_t)r->fat_hiblk[0] << 16);
}
static inline uint32_t ods2_recattr_efblk(const ods2_recattr_t *r)
{
    return (uint32_t)r->fat_efblk[1] | ((uint32_t)r->fat_efblk[0] << 16);
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
 * The trailing word (offset 510) is the additive checksum, as with the
 * home block. [S]
 *
 * INCREMENT 2 (Rule 8 oracle pass, tests/ods2/PROVENANCE-real_vax_ods2.md):
 * a real lab-2 volume's SCB (BITMAP.SYS VBN1, real LBN 5 on that image) was
 * dumped and cross-checked field-by-field against that same session's
 * OpenVMS `SHOW DEVICE/FULL $2$DUA3:` output. Per-field results below;
 * fields still flagged [OVMX-inferred] were NOT resolved by this pass
 * (single sample, no induced-error test) and remain open per Rule 8 --
 * "corrected" below means corrected relative to increment 1's comments,
 * not that the true layout is now fully known.
 * ================================================================ */
typedef struct ods2_scb {
    uint16_t scb_struclev;      /*   0: structure level+version (0x0201). CONFIRMED
                                  *      byte-identical on the real image. [S] */
    uint16_t scb_cluster;       /*   2: storage bitmap cluster factor. CONFIRMED:
                                  *      value 1 matches SHOW DEVICE/FULL "Cluster
                                  *      size". [S] */
    uint32_t scb_volsize;       /*   4: volume size in blocks. CONFIRMED: value 800
                                  *      matches SHOW DEVICE/FULL "Total blocks". [S] */
    uint32_t scb_blksize;       /*   8: [OVMX-inferred, UNCONFIRMED] observed = 1 on
                                  *      the real image; no SHOW DEVICE/FULL field
                                  *      matched it, so this name/semantics is a
                                  *      guess, not a finding. */
    uint32_t scb_sectors;       /*  12: sectors per track. CONFIRMED: value 10
                                  *      matches SHOW DEVICE/FULL "Sectors per
                                  *      track" for the same real volume. */
    uint32_t scb_tracks;        /*  16: tracks per cylinder. CONFIRMED: value 80
                                  *      matches SHOW DEVICE/FULL "Tracks per
                                  *      cylinder". */
    uint32_t scb_cylinders;     /*  20: cylinders. CONFIRMED: value 1 matches SHOW
                                  *      DEVICE/FULL "Total cylinders". */
    uint32_t scb_status;        /*  24: [OVMX-inferred, UNCONFIRMED] observed = 0 on
                                  *      a healthy, cleanly-dismounted real volume;
                                  *      no error/mount-verify state was induced, so
                                  *      this pass cannot say what a nonzero value
                                  *      means. */
    uint32_t scb_status2;       /*  28: [OVMX-inferred, UNCONFIRMED] observed = 0;
                                  *      same caveat as scb_status. */
    uint16_t scb_writecnt;      /*  32: [OVMX-inferred, CORRECTED-TO-UNKNOWN]
                                  *      increment 1 called this "mount write
                                  *      count"; the real image has 0 here while
                                  *      OpenVMS itself reported "Mount count 1"
                                  *      for that very volume in the same session
                                  *      -- so that name is WRONG. Real semantics
                                  *      undetermined; do not rely on the old name. */
    char     scb_volockname[12];/*  34: PARTIALLY CONFIRMED, WIDTH CORRECTED: the
                                  *      real image has ASCII "VAX1" starting here
                                  *      (the mounting host's name -- matches SHOW
                                  *      DEVICE/FULL "Host name"), so the FIELD
                                  *      EXISTS and starts at the right offset.
                                  *      But it is NOT a clean 12-byte space-padded
                                  *      string as increment 1 assumed: bytes 8-19
                                  *      of this field (offset 42-53) instead look
                                  *      like a VMS 64-bit absolute-time quadword
                                  *      (its last 4 bytes match hm2_credate's last
                                  *      4 bytes on the same volume). The exact
                                  *      field width is therefore still open; only
                                  *      the leading host-name bytes are grounded. */
    uint8_t  scb_reserved[464]; /*  46: CORRECTED (was asserted plain "reserved"):
                                  *      NOT proven all-zero. This region's first 8
                                  *      bytes are almost certainly the tail of the
                                  *      timestamp described above, not padding;
                                  *      bytes 54..509 were all-zero in the one real
                                  *      sample examined here. No public spec covers
                                  *      this area byte-for-byte, so it stays opaque
                                  *      to this reader rather than guessing a
                                  *      structure from a single sample. */
    uint16_t scb_checksum;      /* 510: additive checksum, 255 words. CONFIRMED on
                                  *      the real image. [S] */
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

/*
 * [F14] (increment 7, vms-0f3): dir_verlimit is NOT this entry's version
 * number (that is dir_version, in the trailing value entry, and was
 * already correct) -- it is a per-name version-LIMIT policy value, and it
 * is NOT a single constant either. Byte-diffing tests/ods2/real_vax_ods2.dsk's
 * own [000000] MFD directory block (FID 4, all 11 records) against this
 * writer's own equivalent output found TWO distinct real values, cleanly
 * split by whether the entry is one of the 10 traditionally-reserved
 * files or a caller-created one:
 *
 *   - The 10 reserved-file entries (INDEXF.SYS .. SECURITY.SYS, i.e.
 *     entry_fid.fid_num <= ODS2_RESFILES) ALL read verlimit == 0x0001 --
 *     i.e. locked to their own (always 1) version, matching the "system
 *     files are never versioned" VMS convention.
 *   - OVMXDIR.DIR's own entry (fid_num 11, > ODS2_RESFILES) reads
 *     verlimit == 0x7FFF (32767, "no limit set") -- confirmed again by
 *     [OVMXDIR]'s OWN directory block (FID 11), where BOTH its entries
 *     (HELLO.TXT, WORLD.TXT, also both fid_num > ODS2_RESFILES) read the
 *     same 0x7FFF, despite different FIDs/content.
 *
 * ods2_wvolume_dir_insert() previously wrote the caller's `version`
 * argument into dir_verlimit unconditionally -- coincidentally correct
 * for reserved files (whose version is always 1) but wrong for every
 * caller-created file/directory. See PROVENANCE-real_vax_ods2.md's
 * increment-7 addendum for the splice-diagnostic trail that isolated
 * this (a real-VAX MOUNT of a volume with the REAL fixture's own
 * [000000] spliced in mounted clean, but then failed BADIRECTORY again
 * on [OVMXDIR] alone -- proving the same dir-insert defect lived in both
 * places).
 */
#define ODS2_DIR_VERLIMIT_DEFAULT  0x7FFFu   /* caller-created files/dirs */

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
    ODS2_ERR_NOTFOUND,      /* file / entry not found                 */
    ODS2_ERR_NOSPACE        /* no free blocks/FIDs/directory room     */
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
 * Parse and validate the Storage Control Block (SCB, BITMAP.SYS VBN1) from a
 * raw 512-byte buffer into `out`. Validates the additive checksum and the
 * structure-level word, same convention as ods2_home_parse / ods2_fh2_parse.
 * Added in increment 2 alongside the real-image oracle pass (Rule 8) that
 * confirmed/corrected the struct's per-field comments -- see ods2_scb_t.
 */
ods2_status_t ods2_scb_parse(const void *block, size_t block_len,
                             ods2_scb_t *out);

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

/* ================================================================
 * WRITER  (implemented in ods2/ods2_writer.c) -- increment 3.
 *
 * Produces a genuine ODS-2 volume image: primary + alternate home block,
 * the index file bitmap, the ten traditional reserved system files
 * (INDEXF.SYS .. SECURITY.SYS, ODS2_FID_* above), the storage bitmap
 * (BITMAP.SYS: SCB + real free/allocated bit accounting), and the MFD
 * ([000000]). After ods2_volume_format(), a caller can create additional
 * files/directories with real FM2 retrieval-pointer extents and directory-
 * record insertion into an existing directory.
 *
 * CLEAN-ROOM PROVENANCE (Rule 8), increment-3 additions beyond what
 * ods2_reader.c / this header already cite for increments 1-2:
 *
 *   [N2] Storage-bitmap bit semantics and packing, from Paul Nankervis's
 *        deallocfile() in simh/simtools extracters/ods2/access.c
 *        (https://github.com/simh/simtools/blob/master/extracters/ods2/access.c):
 *        a bit value of 1 means the block/cluster is FREE, 0 means
 *        ALLOCATED (deallocfile ORs a mask into the bitmap word to mark
 *        blocks free again). Bits are packed as 32-bit little-endian
 *        words, 4096 bits (one 512-byte block) per bitmap VBN; VBN =
 *        (LBN / cluster) / 4096 + 2 (VBN 1 is the SCB, matching increment
 *        2's confirmed SCB-at-VBN1 finding); word index = (clusterno %
 *        4096) / 32; bit index = clusterno % 32. The SAME function's
 *        index-file-bitmap code (which clears a bit on file deallocation)
 *        grounds the index file bitmap's (hm2_ibmaplbn) analogous packing,
 *        with the opposite sense: bit == 1 means the FID IS in use.
 *   [N3] FH2$M_DIRECTORY (0x2000) and FH2$M_CONTIG (0x80) file-
 *        characteristic bits, from access.h's FH2$M_* defines (same file,
 *        same repo).
 *   [F]  The ten traditional reserved file names/order and hm2_resfiles ==
 *        10: NOT from public docs -- read directly off the real-VAX
 *        fixture tests/ods2/real_vax_ods2.dsk (increment 2's fixture) by
 *        decoding each reserved header's ident area. Observed-oracle
 *        grounding under Rule 8; see PROVENANCE-real_vax_ods2.md addendum.
 *   [F2] fh2_backlink and fh2_fid.seq semantics: discovered empirically
 *        when a first writer draft, which left fh2_backlink all-zero, was
 *        lab-tested against a real VAX and rejected ("%MOUNT-W-IDXHDRBAD,
 *        index file header is bad; backup used" then "Files-11 home block
 *        not found on volume"). Re-decoding the real fixture's headers
 *        showed EVERY one of the 10 reserved files has fh2_backlink ==
 *        FID 4 (000000.DIR, including 000000.DIR's own header backlinking
 *        to ITSELF) and fh2_fid.seq == its own fid_num (1..10); the
 *        user-created files (FID 11 OVMXDIR.DIR, FID 12/13 HELLO.TXT/
 *        WORLD.TXT) have fh2_fid.seq == 1 and fh2_backlink == their real
 *        parent directory's FID (4 for OVMXDIR, 11 for the two files
 *        inside it). This writer reproduces both rules exactly.
 *   [F4] fh2_acoffset MUST be the sentinel value 255 ("no ACL area"), not
 *        an arbitrary "empty area starts here" value equal to fh2_mpoffset
 *        -- even though fh2_idoffset/fh2_mpoffset ARE genuinely flexible
 *        (the reader locates ident/map purely via these stored offsets,
 *        with no fixed expectation). This was the LAST bug found after
 *        [F2]/RECATTR/LBN-0 fixes still failed to mount: bisected on
 *        lab-2 by taking the real fixture's own byte-for-byte-working FID1
 *        header and changing ONLY its 3 offset bytes to this writer's
 *        chosen values (54/114/114 instead of real's 40/100/255) --
 *        MOUNT rejected THAT alone with the identical IDXHDRBAD/home-
 *        block-not-found pair, and setting acoffset back to 255 (while
 *        leaving idoffset/mpoffset at 54/114) mounted cleanly. Full
 *        bisection trail (4 hybrid images, each isolating one variable)
 *        in PROVENANCE-real_vax_ods2.md's increment-3 addendum.
 *   [F6] SECURITY.SYS's VBN1 data-block format (increment 4): derived PURELY by DIFFERENTIAL
 *        BEHAVIORAL OBSERVATION on lab-2 (never disassembled, never copied
 *        verbatim) -- 12 real `INITIALIZE`+`MOUNT` trials on pod `vaxlab-8`
 *        across 9 distinct volume labels (lengths 1, 2, 3, 6, 7, 8, 12) and
 *        2 device geometries (RX50/800 blocks, RX33/2400 blocks), each
 *        pulled via `DUMP/BYTE` and a raw disk-image byte compare (`cmp -l`)
 *        against the previous trial. Findings, all cross-validated across
 *        every trial (see PROVENANCE-real_vax_ods2.md's increment-4
 *        addendum for the full table):
 *          - Offset 0x00 (4 bytes): a checksum. Zeroing it on an otherwise-
 *            untouched real volume changes MOUNT's failure from success to
 *            "%MOUNT-F-BADSECSYS ... -SYSTEM-E-BADCHECKSUM, message
 *            checksum failure" -- i.e. this field IS enforced by a real
 *            MOUNT, which is the actual reason increment 3's zero-filled/
 *            size-matched stubs were rejected. ALGORITHM (fully derived,
 *            reproduces all 12 samples exactly, see
 *            ods2_security_checksum() in ods2_writer.c): let strlen = the
 *            volume label's length in bytes; let n = ((78 + strlen) / 4) * 4
 *            (integer division, i.e. round down to a multiple of 4); XOR
 *            each of the n bytes starting at block offset 4 into one of 4
 *            lanes by byte position modulo 4; the stored checksum is the
 *            4 lanes packed as a little-endian longword. This is a plain
 *            byte-lane XOR fold, NOT a CRC -- CRC-32 (several standard
 *            polynomial/init/reflection variants) and additive-16/32 sums
 *            were tried first and did NOT reproduce the observed values.
 *          - Offset 0x08 (1 byte): equals 0x52 + strlen, i.e. the block
 *            offset one past the end of the label text -- an "end of
 *            variable data" pointer. Derived from a monotonic byte-for-
 *            byte match across all 12 trials (value tracks strlen exactly).
 *          - Offset 0x1C (4 bytes, ods2_uic_t layout): the volume's owner
 *            UIC as a longword (member low word, group high word) -- every
 *            trial used the implicit default (no /OWNER_UIC given) and
 *            read back as [1,4] (SYSTEM), matching the SAME reserved
 *            files' fh2_fileowner elsewhere on the same volume. NOT
 *            independently varied (no trial used /OWNER_UIC), so "this
 *            field is UIC and honors /OWNER_UIC" is [OVMX-inferred] by
 *            analogy with the FH2 header's identically-shaped owner field;
 *            the writer always emits SYSTEM [1,4] here, same as it does
 *            for other reserved-file ownership.
 *          - Offset 0x50 (word): strlen + 4.
 *          - Offset 0x52 (strlen bytes): the volume label, ASCII, NOT
 *            null-padded to any fixed width beyond the record's own extent
 *            (trailing bytes through the end of the 512-byte block are 0).
 *          - Offset 0x04..0x4F, EXCLUDING 0x08 and 0x1C-0x1F above: IDENTICAL
 *            byte-for-byte across all 12 trials regardless of label content
 *            OR volume geometry -- i.e. a fixed template for "zero ACL
 *            entries" (a freshly-INITIALIZEd volume has never had an ACL
 *            applied to it). This writer reproduces that CONSTANT template
 *            verbatim (ods2_security_template[] in ods2_writer.c) because it
 *            is deterministic content-independent structure, not per-volume
 *            VSI-generated data -- the same category of reproduction as the
 *            FH2$M_* bit values and reserved-file name/order already cited
 *            under [N3]/[F] above, NOT the "copy one real file's bytes into
 *            every volume" approach increment 3 correctly rejected. The
 *            individual field MEANINGS within this constant region (quota
 *            fields, ACL-journal state, etc. per the public "OpenVMS Guide
 *            to System Security"'s volume-security-profile description)
 *            were NOT decoded field-by-field; it is carried as an opaque,
 *            [OVMX-inferred: byte layout observed and reproduced, semantics
 *            of most individual bytes NOT decoded] structural constant.
 *          - A single-character volume label (strlen == 1) has one
 *            open discrepancy: this writer's checksum formula reproduces it
 *            correctly (verified against 3 independent one-letter labels,
 *            all identical), but during derivation an earlier, narrower
 *            formula matched every length EXCEPT 1 by exactly one extra
 *            XOR term; the FINAL formula above was re-derived to close
 *            that gap and re-verified against all 12 samples (see
 *            PROVENANCE increment-4 addendum) -- flagged here in case a
 *            future 13th sample reopens it.
 *   [F7] fh2_recattr.fat_efblk for SECURITY.SYS's header is set to 2 (not
 *        map_count == 6), matching the real fixture's own SECURITY.SYS
 *        header exactly (hiblk=6/efblk=2 -- re-decoded during this
 *        increment; an EARLIER re-read had misremembered it as efblk=1).
 *        Patched directly onto the header written by write_fh2_header()'s
 *        generic path (which otherwise sets hiblk==efblk==map_count, the
 *        already-accepted [OVMX-inferred] convention still used as-is for
 *        INDEXF.SYS/BITMAP.SYS). This did NOT, by itself, change the real
 *        MOUNT outcome (see the [F9] status note below) -- kept because it
 *        is still a real, oracle-grounded correction over the generic
 *        convention, not because it was proven load-bearing.
 *   [F8] MFD directory entries for all 10 reserved files (increment 4):
 *        discovered while bisecting a SECOND real-MOUNT failure that
 *        survived the [F6] checksum fix -- see [F9]. Decoding the real
 *        fixture's own [000000] directory data block (`strings` over the
 *        raw bytes at its data LBN) showed named entries for 000000.DIR,
 *        INDEXF.SYS, BITMAP.SYS, BADBLK.SYS, CORIMG.SYS, VOLSET.SYS,
 *        CONTIN.SYS, BACKUP.SYS, BADLOG.SYS, AND SECURITY.SYS -- i.e. a
 *        real INIT lists every reserved file by name in the MFD, not just
 *        caller-created ones. This writer previously inserted none of
 *        them. ods2_volume_format() now calls ods2_wvolume_dir_insert()
 *        for all 10 before returning. A real, oracle-grounded finding
 *        (kept regardless of [F9]'s outcome), but ALSO did not by itself
 *        change the real MOUNT outcome.
 *   [F9] CURRENT STATUS (increment 4, HONEST, NOT A SUCCESS CLAIM): the
 *        SPECIFIC, NAMED increment-3 defect --
 *        "%MOUNT-F-BADSECSYS ... -SYSTEM-E-BADCHECKSUM, message checksum
 *        failure" -- IS resolved: induced-error testing on a real, other-
 *        wise-untouched real-VAX volume (zeroing ONLY its SECURITY.SYS
 *        checksum bytes) reproduces that exact BADCHECKSUM failure, and
 *        ods2_security_build()'s output does not trigger it. HOWEVER, a
 *        real end-to-end MOUNT of this writer's OWN complete volume output
 *        does NOT reach completion: it still fails with
 *        "%MOUNT-F-BADSECSYS ... -SYSTEM-W-FILENUMCHK, file identification
 *        number check" -- a DIFFERENT secondary status than BADCHECKSUM.
 *        Extensive lab-2 bisection (pod vaxlab-8) proves this is NOT about
 *        SECURITY.SYS's own header or data content: splicing a REAL
 *        fixture's own complete SECURITY.SYS header (every field: owner
 *        UIC, fileprot, highwater, efblk, idoffset/mpoffset, all real)
 *        -- with ONLY its retrieval-pointer LBN adjusted to point at this
 *        writer's own correctly-checksummed data block -- into this
 *        writer's own volume STILL reproduces FILENUMCHK, identically to
 *        this writer's ORIGINAL zero-length stub (also re-verified: main-
 *        branch's pre-increment-4 output shows FILENUMCHK too, not
 *        BADCHECKSUM as increment-3's own PROVENANCE text describes --
 *        that description apparently did not capture the secondary status
 *        line for the exact zero-length-stub variant). Reducing maxfiles
 *        from 200 to 13 (matching the real fixture's actual file count)
 *        did not change the outcome either. The remaining defect is
 *        therefore somewhere ELSE in this writer's volume-wide structure
 *        -- the SIMPLIFICATIONS list below's INDEXF.SYS single-contiguous-
 *        extent choice (vs. the real fixture's 3-extent fragmented map) was
 *        the leading untested candidate, since it was the only OTHER
 *        already-flagged "not reproduced, presumed OK" structural
 *        difference in the whole writer. NOT bisected further in this
 *        increment (out of the SECURITY.SYS-focused scope this work was
 *        chartered under) -- see PROVENANCE-real_vax_ods2.md's increment-4
 *        addendum for the full trial log and a concrete next-step
 *        recommendation.
 *
 *        UPDATE (increment 6, vms-0f3): the INDEXF.SYS single-extent
 *        candidate WAS confirmed load-bearing -- giving it a genuine
 *        3-extent map (see [F12] below) moved the real-VAX MOUNT failure
 *        from `-SYSTEM-W-FILENUMCHK` to a DIFFERENT secondary status,
 *        `-SYSTEM-W-BADIRECTORY, bad directory file format`, a real,
 *        reproducible state transition. `FILENUMCHK` is therefore
 *        RESOLVED. `BADIRECTORY` is the new, NOT YET RESOLVED wall --
 *        surviving both a directory-sort-order fix ([F13]) and a stray-
 *        byte fix found while making it, so it is not (solely) a
 *        directory-content-ordering issue. See PROVENANCE-real_vax_ods2.md's
 *        increment-6 addendum for the full fix list, what was tried and
 *        ruled out, and the recommended next diagnostic (a real-fixture
 *        `[000000]` header+data splice, in step 3's style above).
 *
 *   [F10] wvol->mfd_fid.fid_seq bug (increment 6): was 1 (the "first
 *        generation of a newly created file" convention) instead of
 *        ODS2_FID_MFD (4, what the MFD's own on-disk fh2_fid.seq actually
 *        is). Every caller-created top-level file/dir's fh2_backlink is
 *        built from this struct, so it silently disagreed with the real
 *        on-disk MFD header. See ods2_writer.c's [F10] comment. Tried
 *        alone on lab-2: FILENUMCHK reproduced identically -- not
 *        sufficient alone, but a real, kept fix.
 *   [F11] fh2_fileowner/fh2_fileprot/fh2_reserved1/fh2_highwater
 *        (increment 6): four previously-zero per-file-header fields found
 *        non-zero on every one of the real fixture's 13 real headers via
 *        a full field-by-field diff (see ods2_writer.c's [F11] comment
 *        for the derivation of each). Tried together on lab-2:
 *        FILENUMCHK reproduced identically -- not sufficient, but real
 *        and kept.
 *   [F12] INDEXF.SYS genuinely fragmented into 3 extents (increment 6):
 *        re-derived the real shape directly off real_vax_ods2.dsk's own
 *        FID1 header and found the actual reason for the fragmentation --
 *        BITMAP.SYS's and 000000.DIR's own data extents sit PHYSICALLY
 *        BETWEEN the home-block pair and the index file bitmap on the
 *        real volume, not after the header area as this writer previously
 *        placed them. Reordered the physical layout to match and gave
 *        INDEXF.SYS a genuine 3-extent map via write_fh2_header_ext()
 *        (see ods2_writer.c). CONFIRMED LOAD-BEARING: real-VAX MOUNT's
 *        secondary status moved from FILENUMCHK to BADIRECTORY -- see the
 *        UPDATE note above.
 *   [F13] Directory entries must be in ascending name order (increment 6):
 *        decoding the real fixture's own [000000] MFD directory block
 *        record-by-record (not just via `strings`, unlike [F8]) showed
 *        its 11 entries in strict ascending byte order by filename.
 *        ods2_wvolume_dir_insert() now finds the correct sorted position
 *        instead of always appending. Tried on lab-2: BADIRECTORY
 *        persisted unchanged -- real and now byte-exact with the real
 *        fixture's own order, but not (solely) BADIRECTORY's cause. A
 *        related stray-byte regression (the sorted insert's word-
 *        alignment pad byte, opened by memmove(), wasn't reset to the
 *        0xFF empty-fill convention) was found and fixed in the same
 *        pass; also did not change the MOUNT outcome.
 *
 *        UPDATE (increment 7, vms-0f3): a real-fixture splice of [000000]'s
 *        own FH2 header + data block (byte-exact, only the retrieval
 *        pointer LBN rewritten) mounted CLEAN on a real VAX -- isolating
 *        the still-open BADIRECTORY defect to this writer's OWN [000000]
 *        construction. The splice ALSO exposed a second, independent
 *        instance of the same defect class on [OVMXDIR] (100% this
 *        writer's own code, untouched by the splice). A dir_verlimit
 *        field-confusion bug was found and fixed ([F14] below) but proved
 *        NOT sufficient alone for either directory.
 *
 *        UPDATE (increment 8, vms-0f3): [F15] below (efblk/hiblk for
 *        directory files) WAS the remaining defect for [OVMXDIR]. Fixed,
 *        and validated end-to-end on a real VAX (lab-2 pod vaxlab-9):
 *        `MOUNT $2$DUA3: OVMXWRIT` returns `%MOUNT-I-MOUNTED` with ZERO
 *        warnings (no QUOTAFAIL, no BADIRECTORY, no BADSECSYS -- the
 *        first fully clean real-VAX MOUNT of an all-OVMX-written volume),
 *        `DIRECTORY $2$DUA3:[000000]` lists all 11 entries, and
 *        `DIRECTORY $2$DUA3:[OVMXDIR]` lists both HELLO.TXT and
 *        WORLD.TXT. **MOUNT-to-completion and directory traversal are
 *        therefore ACHIEVED.** `DUMP` confirms HELLO.TXT's exact raw
 *        bytes are present on disk, but `TYPE` of that same file shows no
 *        content -- a NEW, separate wall: this writer stores plain-text
 *        file data as a raw byte stream, not as RMS variable-length
 *        records (each of which needs its own 2-byte little-endian
 *        length-prefix word on disk); `TYPE`'s record-oriented read
 *        apparently misparses the first content bytes as a bogus record
 *        length. NOT investigated further this increment (out of the
 *        [OVMXDIR]-BADIRECTORY-focused scope) -- recommended as
 *        increment 9's target. See PROVENANCE-real_vax_ods2.md's
 *        increment-8 addendum for the full lab-2 transcript.
 *   [F15] (increment 8, vms-0f3): fh2_recattr.fat_efblk for DIRECTORY
 *        files is NOT simply hiblk (the writer's prior assumption,
 *        correct for 000000.DIR's real sample but never independently
 *        checked against a second directory). Directly re-decoding
 *        real_vax_ods2.dsk's own FID11 (OVMXDIR.DIR) header via a
 *        struct-level byte decode (not a transcription) this increment
 *        gives hiblk=1 (map extent is exactly 1 block, LBN 31 count 1)
 *        but efblk=2 -- EFBLK EXCEEDS the file's own allocation. This
 *        matches the documented Files-11/RMS convention that EFBLK/FFBYTE
 *        express an end-of-file POSITION, not an allocation size: when
 *        the last valid byte lands exactly on a block boundary, EFBLK is
 *        set to the FOLLOWING block (FFBYTE 0) even if that block was
 *        never separately allocated. Re-checked against 000000.DIR's own
 *        real header: hiblk=2/efblk=2/ffbyte=0 is the SAME rule applied
 *        to a file whose second (trailing) block happens to already be
 *        allocated -- not a contradiction once "efblk = (last block with
 *        data) + 1" is the invariant instead of "efblk == hiblk". Every
 *        directory this writer creates (MFD and caller directories alike)
 *        is a single CONTIG block, so `efblk = hiblk + 1, ffbyte = 0`
 *        generalizes correctly to both without needing to model a
 *        real multi-block MFD (a separately-flagged, still-open
 *        [OVMX-inferred] simplification -- see SIMPLIFICATIONS below).
 *        Fixed in write_fh2_header_ext()'s FH2_KIND_DIR branch
 *        (ods2_writer.c). CONFIRMED LOAD-BEARING: real-VAX MOUNT of the
 *        full writer output went from BADIRECTORY to a completely clean
 *        `%MOUNT-I-MOUNTED` with both [000000] and [OVMXDIR] listable.
 *
 * SIMPLIFICATIONS -- explicitly [OVMX-inferred], NOT claimed byte-genuine:
 *   - Reserved-file and created-file timestamps are left zero.
 *   - The stub reserved files (BADBLK/CORIMG/VOLSET/CONTIN/BACKUP/BADLOG)
 *     get zero-length headers with no data extent, matching the real
 *     fixture (all had map_inuse == 0 there too). SECURITY.SYS (increment 4,
 *     [F6]/[F7] above) gets a genuine, clean-room-derived "zero ACL entries"
 *     data block in VBN1 with a correctly-computed, oracle-validated
 *     checksum -- see ods2_security_build() in ods2_writer.c -- resolving
 *     the SPECIFIC increment-3 BADCHECKSUM failure mode. A full real MOUNT
 *     NOW COMPLETES CLEANLY (increment 8, [F15]) and both [000000] and
 *     [OVMXDIR] are directory-listable; see [F9]'s UPDATE trail. The
 *     writer's MFD is still a single block (vs. the real fixture's 2 --
 *     out of reach until multi-block directory growth is implemented) and
 *     data-file content is a raw byte stream rather than RMS
 *     variable-length records with per-record length prefixes, so `TYPE`
 *     of a plain data file does not yet show its content even though the
 *     bytes are genuinely on disk (confirmed via `DUMP`) -- open for a
 *     future increment.
 * ================================================================ */

/* Volume-format parameters for ods2_volume_format(). */
typedef struct ods2_format_params {
    uint32_t    total_blocks;  /* volume size in blocks; must be large
                                   enough for the fixed reserved-file layout
                                   (a few dozen blocks) plus intended data */
    uint32_t    maxfiles;      /* index file capacity; must be >= ODS2_RESFILES */
    const char *volname;       /* <= 12 chars; space-padded on write */
} ods2_format_params_t;

/*
 * Writer handle for an in-memory volume image under construction. `image`
 * is caller-owned (typically a heap buffer of total_blocks * ODS2_BLOCK_SIZE
 * bytes) and is written into directly by every ods2_wvolume_*() call.
 */
typedef struct ods2_wvolume {
    uint8_t  *image;
    size_t    image_len;
    uint32_t  nblocks;
    uint32_t  maxfiles;
    uint32_t  ibmap_lbn;            /* hm2_ibmaplbn                        */
    uint32_t  ibmap_size;           /* hm2_ibmapsize, in blocks            */
    uint32_t  hdr_base_lbn;         /* ibmap_lbn + ibmap_size               */
    uint32_t  bitmap_scb_lbn;       /* BITMAP.SYS VBN1 (the SCB)            */
    uint32_t  bitmap_data_blocks;   /* BITMAP.SYS VBN2.. bit-block count    */
    uint32_t  next_free_lbn;        /* bump allocator watermark, blocks     */
    uint32_t  next_free_fid;        /* bump allocator watermark, FIDs       */
    ods2_fid_t mfd_fid;             /* FID of [000000] (ODS2_FID_MFD)       */
} ods2_wvolume_t;

/*
 * Format a fresh, genuine ODS-2 volume into `image` (already sized to
 * params->total_blocks * ODS2_BLOCK_SIZE bytes -- caller-owned, will be
 * zero-filled by this call). Writes the home block pair, index file
 * bitmap, the ten reserved system files, BITMAP.SYS's SCB + storage
 * bitmap, and the empty MFD. `wvol` is initialized for subsequent
 * ods2_wvolume_*() calls.
 */
ods2_status_t ods2_volume_format(uint8_t *image, size_t image_len,
                                 const ods2_format_params_t *params,
                                 ods2_wvolume_t *wvol);

/*
 * Allocate `count` contiguous free blocks via the storage bitmap
 * (first-fit from the bump watermark). Marks them allocated (bit -> 0)
 * in BITMAP.SYS's on-disk bitmap. Returns ODS2_ERR_NOSPACE if the volume
 * has no contiguous run of that size left before its end.
 */
ods2_status_t ods2_wvolume_alloc_blocks(ods2_wvolume_t *wvol,
                                        uint32_t count, uint32_t *lbn_out);

/*
 * Create a new file with a single contiguous data extent holding `data`
 * (data_len bytes, rounded up to whole blocks; the tail of the last block
 * is zero-padded). Allocates the next free FID, writes its FH2 header
 * (ident name "NAME;version", FM2 map, checksum, fh2_backlink == parent_dir
 * -- see below) and marks its FID bit used in the index file bitmap. Does
 * NOT insert a directory entry -- call ods2_wvolume_dir_insert() separately
 * (with the SAME parent_dir).
 *
 * `parent_dir` is the FID of the directory this file will be inserted
 * into (e.g. wvol->mfd_fid, or a FID from ods2_wvolume_create_dir()) --
 * required because a real VAX MOUNT validates fh2_backlink and rejects a
 * volume where it is zero/unresolvable ("index file header is bad" /
 * "Files-11 home block not found"), discovered while lab-testing this
 * writer; see ods2.h's WRITER provenance note below and
 * PROVENANCE-real_vax_ods2.md's increment-3 addendum.
 */
ods2_status_t ods2_wvolume_create_file(ods2_wvolume_t *wvol,
                                       const char *name, uint16_t version,
                                       const uint8_t *data, size_t data_len,
                                       ods2_fid_t parent_dir,
                                       ods2_fid_t *fid_out);

/*
 * Create a new, empty directory file: one contiguous data block
 * initialized to "no records" (all 0xFF, matching ODS2_DIR_END sentinel
 * at offset 0), FH2 header with FH2$M_DIRECTORY | FH2$M_CONTIG set and
 * fh2_backlink == parent_dir. Does NOT insert a directory entry in the
 * parent -- call ods2_wvolume_dir_insert() separately (e.g. against
 * wvol->mfd_fid).
 */
ods2_status_t ods2_wvolume_create_dir(ods2_wvolume_t *wvol,
                                      const char *name, uint16_t version,
                                      ods2_fid_t parent_dir,
                                      ods2_fid_t *fid_out);

/*
 * Insert a {name, version, entry_fid} directory record into the single
 * data block of directory `dir_fid` (as read via ods2_volume_read_header
 * on the wvolume's own image). Walks existing records the same way
 * ods2_volume_list_dir()/dir_scan_block() do, to find the first free
 * (ODS2_DIR_END) slot, and writes the new record there. Only ONE data
 * block per directory is supported (no directory growth) -- returns
 * ODS2_ERR_NOSPACE if the record does not fit.
 */
ods2_status_t ods2_wvolume_dir_insert(ods2_wvolume_t *wvol,
                                      ods2_fid_t dir_fid,
                                      const char *name, uint16_t version,
                                      ods2_fid_t entry_fid);

/*
 * Build a genuine "zero ACL entries" SECURITY.SYS VBN1 data block for
 * `volname` (1..12 characters) into `block` (must be ODS2_BLOCK_SIZE
 * bytes; fully overwritten). `owner_uic` is the volume's owner UIC to
 * embed at offset 0x1C -- see ods2.h's WRITER [F6] provenance comment for
 * the full clean-room derivation (lab-2 differential trials) and which
 * fields are understood vs. reproduced-as-observed-constant.
 */
ods2_status_t ods2_security_build(uint8_t block[ODS2_BLOCK_SIZE],
                                  const char *volname, ods2_uic_t owner_uic);

/*
 * Recompute and validate the checksum ([F6]) of a SECURITY.SYS VBN1 data
 * block. On success, copies the embedded volume label into `label_out`
 * (NUL-terminated, up to label_out_size - 1 bytes) and the owner UIC into
 * `*owner_out` (either out param may be NULL). Returns ODS2_ERR_CHECKSUM
 * on mismatch, ODS2_ERR_FORMAT if the embedded length field is out of the
 * 1..12 range.
 */
ods2_status_t ods2_security_parse(const void *block, size_t block_len,
                                  char *label_out, size_t label_out_size,
                                  ods2_uic_t *owner_out);

#ifdef __cplusplus
}
#endif

#endif /* _VMSFS_ODS2_H */
