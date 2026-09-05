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

/*
 * The genuine ODS-2 codec compiles THREE ways: userspace (INITIALIZE/
 * vmsfs_master + host ctest), Linux kernel-resident (the shared FS engine's
 * ODS-2 ACP + vmsfs.ko, built with -DOVMX_ODS2_KERNEL), and -- as of rd
 * vms-6a7f -- a second, freestanding kernel-resident build: the elf32-vax
 * cross-compile of src/kernel-core/vmsfs_acp.c (the Files-11 ACP handlers,
 * epic vms-208) against the NetBSD SYSKRNL contract headers
 * (tools/cross-vax/build-vms-module-vax.sh), which also defines
 * OVMX_ODS2_KERNEL but is NOT Linux. Pull fixed-width types + size_t/offsetof
 * from the right world; the block-access seam (pread vs vmsfs_bio) is
 * ods2_block.h.
 *
 * __KERNEL__ is a LINUX-only macro; the NetBSD kernel-module build defines
 * _KERNEL (never __KERNEL__) -- a bare `#ifdef OVMX_ODS2_KERNEL ... <linux/
 * types.h>' unconditionally would have pulled the Linux kernel headers into
 * the NetBSD -nostdinc cross-build and broken it. This is the SAME
 * three-way split src/kernel/vmsfs/vmsfs_ondisk.h already carries for
 * exactly this reason (rd vms-9172 / vms-bbf) and the same Linux-detection
 * idiom src/kernel-core/vmsfs/vmsfs_backend.h uses (OVMX_KBACKEND_LINUX /
 * __linux__ / __KERNEL__).
 */
#if defined(OVMX_ODS2_KERNEL) && \
    (defined(OVMX_KBACKEND_LINUX) || defined(__linux__) || defined(__KERNEL__))
#include <linux/types.h>
#include <linux/stddef.h>
#elif defined(OVMX_ODS2_KERNEL) && defined(_KERNEL)
/*
 * NetBSD / BSD kernel: fixed-width types, no libc, no <linux/...>. offsetof
 * is normally pulled in transitively (vms_internal.h -> <sys/systm.h> ->
 * <lib/libkern/libkern.h>) when this header is reached via vmsfs_acp.c, but
 * src/vmsfs/ods2/ods2_edit.c compiles as ITS OWN standalone TU (its only
 * include is this header, by design -- it is a PURE, dependency-free EDIT
 * surface, see its own top-of-file note) and never pulls that chain in, so
 * offsetof must be self-sufficient here rather than assumed present. The
 * compiler builtin is exactly what <lib/libkern/libkern.h> itself expands to
 * (also #ifndef-guarded there), so this is a no-op if that header lands
 * first, and a definition if it does not.
 */
#include <sys/stdint.h>
#include <sys/types.h>
#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
/*
 * NULL is likewise not guaranteed when ods2_edit.c compiles as its own
 * standalone TU (it never pulls the <sys/systm.h> -> libkern chain that would
 * otherwise define it). ods2_kind_for_filespec() and other inline surfaces in
 * this header use NULL, so define it self-sufficiently here too, guarded so it
 * is a no-op if a real <stddef.h>/libkern already landed first.
 */
#ifndef NULL
#define NULL ((void *)0)
#endif
#else
#include <stdint.h>
#include <stddef.h>
#endif

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
 * ods2_name_eq_ci - case-insensitive exact match of two file-spec names
 * ("NAME.TYPE", no version). Freestanding: a self-contained A-Z upcase, no
 * libc/libkern <ctype.h> dependency, so it is identical on the userspace
 * writer path and the kernel-resident (ods2_edit.c) ACP-create path.
 */
static inline int ods2_name_eq_ci(const char *a, const char *b)
{
    size_t i = 0;
    for (;;) {
        int ca = (unsigned char)a[i];
        int cb = (unsigned char)b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        if (ca == 0)  return 1;
        i++;
    }
}

/*
 * ods2_class_fileprot - the PER-FILE-CLASS default fh2_fileprot (vms-109).
 *
 * Files-11 protection is per-file on real VMS; a single "every ordinary file
 * is World:RE" default (vms-37e) mastered SYSUAF.DAT -- whose Purdy password
 * hashes must be UNREADABLE by the World category -- as World:RE, a hole. The
 * shared writer (ods2_writer.c) AND the kernel ACP-create path (ods2_edit.c)
 * both take this default when the caller passes fileprot == 0, so the class
 * mapping lives here, once, keyed on the file's own name/kind:
 *
 *   directory                      -> 0xBA00  S:RWED,O:RWED,G:RWE,W:E  (fixture
 *                                             directory-shaped mask, [F11])
 *   reserved metadata (FID<=RESF)  -> 0xFA00  S:RWED,O:RWED,G:RE,W:none (fixture)
 *   SYSUAF.DAT                     -> 0xFF88  S:RWE, O:RWE, G:none,W:none
 *                                             -- oracle docs/oracle/
 *                                             vax73-authorize-privilege.md:
 *                                             SYSUAF.DAT (RWE,RWE,,), no WORLD
 *                                             access; protects the hashes.
 *   RIGHTSLIST.DAT                 -> 0xEE00  S:RWED,O:RWED,G:R,W:R
 *                                             -- WORLD-READABLE on real VMS so an
 *                                             unprivileged process can resolve
 *                                             identifiers (F$IDENTIFIER) without
 *                                             SYSPRV; the world-readable half of
 *                                             the rights database (vms-930).
 *   any other ordinary file        -> 0xAA00  S:RWED,O:RWED,G:RE,W:RE  -- the
 *                                             documented OpenVMS DEFAULT file
 *                                             protection (World Read+Execute);
 *                                             every shipped SYS$SYSTEM image
 *                                             (DCL.EXE, LOGINOUT.EXE, ...) must
 *                                             be World-activatable (vms-37e).
 *
 * Within each 4-bit field a SET bit DENIES (bit0=R,1=W,2=E,3=D); fields are
 * System, Owner, Group, World from low to high nibble (ovmx_fileprot.h).
 * Clean-room: the two grounded constants are the real-VAX fixture / oracle
 * captures cited above; the World:R and default values are the documented
 * public OpenVMS protections (Rule 8).
 */
static inline uint16_t ods2_class_fileprot(const char *name, int is_dir,
                                           uint32_t fidnum)
{
    if (is_dir)                  return 0xBA00u;
    if (fidnum <= ODS2_RESFILES) return 0xFA00u;
    if (name && ods2_name_eq_ci(name, "SYSUAF.DAT"))     return 0xFF88u;
    if (name && ods2_name_eq_ci(name, "RIGHTSLIST.DAT")) return 0xEE00u;
    return 0xAA00u;
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
/*
 * fat_rattrib bits (RMS "record attributes", FAB$B_RAT's on-disk analog).
 * Public convention [S] (OpenVMS RMS Reference Manual / Guide to File
 * Applications): FTN = Fortran carriage control, CR = implied
 * carriage-return carriage control (the common case for plain text files),
 * PRN = print-file carriage control, BLK = fixed-length records may not
 * span a block boundary. fat_rtype's small-integer values are the on-disk
 * analog of FAB$B_RFM: 1 = FIXED, 2 = VARIABLE, 3 = VFC, 4 = STREAM,
 * 5 = STREAMLF, 6 = STREAMCR.
 */
#define ODS2_RAT_FTN   0x01u
#define ODS2_RAT_CR    0x02u
#define ODS2_RAT_PRN   0x04u
#define ODS2_RAT_BLK   0x08u
#define ODS2_RTYPE_FIX   1u
#define ODS2_RTYPE_VAR   2u
#define ODS2_RTYPE_STMLF 5u  /* stream, LF-terminated (FAB$C_STMLF) */

typedef struct ods2_recattr {
    uint8_t  fat_rtype;         /*  0: record type / file organization */
    uint8_t  fat_rattrib;       /*  1: record attributes               */
    /* [F16] (increment 9, vms-0f3): re-decoded directly off
     * tests/ods2/real_vax_ods2.dsk's own FID12/FID13 (HELLO.TXT/WORLD.TXT,
     * both real-VMS-COPY'd files) this increment -- fat_rsize is NOT a
     * fixed 0 for variable-length data files as increment 3's writer
     * comment claimed (never itself re-checked against this field). The
     * real values are 105 and 66 respectively, which exactly match each
     * file's own LONGEST on-disk record's content length (confirmed by
     * fully decoding both files' VAR-record streams -- see
     * ods2_var_records_decode() and PROVENANCE-real_vax_ods2.md's
     * increment-9 addendum). I.e. for RFM=VAR, RMS sets RSIZE to the
     * longest record actually written when no fixed size was specified at
     * creation (fat_maxrec stays 0 in both real samples, matching "no cap
     * given"). ods2_writer.c's ods2_wvolume_create_file() now reproduces
     * this instead of hardcoding 0. */
    uint16_t fat_rsize;         /*  2: record size (VAR: longest record)*/
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

/*
 * [F16] (increment 9, vms-0f3): the file's total VALID data byte count,
 * counted from VBN 1 -- (efblk-1)*512 + ffbyte. Formula confirmed exact
 * against tests/ods2/real_vax_ods2.dsk's own FID12/FID13: fully decoding
 * HELLO.TXT's VAR-record stream (see ods2_var_records_decode()) consumes
 * exactly 17218 bytes with clean zero-fill after, and
 * (efblk=34-1)*512+ffbyte=322 == 17218 exactly; WORLD.TXT likewise
 * (efblk=2-1)*512+ffbyte=208 == 720, matching its own last real record's
 * end offset exactly. This is the documented Files-11/RMS "EFBLK/FFBYTE
 * express an end-of-file position, not an allocation size" convention
 * (see ods2_writer.c's [F15] comment for the companion directory-side
 * finding) -- not previously used by the reader to bound a data file's
 * own content.
 */
static inline size_t ods2_recattr_data_bytes(const ods2_recattr_t *r)
{
    uint32_t efblk = ods2_recattr_efblk(r);
    size_t base = efblk > 0 ? (size_t)(efblk - 1) * ODS2_BLOCK_SIZE : 0;
    return base + r->fat_ffbyte;
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
    ODS2_ERR_NOSPACE,       /* no free blocks/FIDs/directory room     */
    ODS2_ERR_IO             /* backing-store I/O error (pread short/failed) */
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

/*
 * Decode ONE 512-byte directory data block's variable-length records,
 * invoking `cb` once per {name, version, fid}. Returns non-zero if the
 * callback asked to stop early, 0 otherwise. This is the record-decoding
 * core shared by BOTH the in-memory volume walker (ods2_volume_list_dir)
 * and the block-backed one (ods2_bdev_list_dir) -- extracted so there is a
 * single, identical directory-record decoder behind either block source
 * (no new format facts; same layout the reader always used). [N] direct.h
 */
int ods2_dir_block_scan(const void *dir_block, ods2_dir_cb cb, void *ctx);

/* ---- Volume-level reader over a real BLOCK DEVICE / image fd ----
 *
 * A block-backed twin of ods2_volume_t: instead of a whole-volume in-memory
 * `const uint8_t *image`, blocks are fetched on demand via pread() over a
 * backing fd (a loop/disk image file, or a block/character device such as
 * /dev/vms). This is the FOUNDATION rung (vms-6cb) that lets the SAME
 * genuine reader logic (ods2_home_parse / ods2_fh2_parse / ods2_fh2_map_walk
 * / ods2_dir_block_scan) run over live storage without loading the entire
 * volume into RAM. The pread block-transfer shape deliberately mirrors
 * the MSCP server's raw-block path (whole-block reads, a short
 * read is a hard failure, offsets computed in 64 bits). The in-memory
 * ods2_volume_t path is UNCHANGED and both variants coexist.
 *
 * The fd is NOT owned by ods2_bdev_t -- the caller opens and closes it.
 */
typedef struct ods2_bdev {
#ifdef OVMX_ODS2_KERNEL
    void       *host;       /* opaque block backend handle (Linux: super_block)
                            * passed to vmsfs_bget(); see ods2_block.h. Bound by
                            * ods2_bdev_open_host() in the kernel FS engine.   */
#else
    int         fd;         /* backing block device / image fd (borrowed)  */
#endif
    uint32_t    nblocks;    /* volume size in 512-byte blocks              */
    ods2_home_t home;       /* parsed, validated home block (LBN 1)        */
} ods2_bdev_t;

/*
 * Attach a block-backed reader to the volume on `fd`. `span_bytes` is the
 * usable size of the volume in bytes; pass 0 to auto-detect via
 * lseek(fd, 0, SEEK_END) (works for regular/loop image files; a device that
 * cannot report a size via lseek must pass span_bytes explicitly). Reads and
 * validates the home block at LBN 1 (checksums + "DECFILE11B  " + strict
 * structure level), caching it in `bv->home`, exactly as ods2_volume_open()
 * does for the in-memory image.
 */
ods2_status_t ods2_bdev_open(ods2_bdev_t *bv, int fd, uint64_t span_bytes);

#ifdef OVMX_ODS2_KERNEL
/*
 * KERNEL-RESIDENT open (rd vms-dcd, epic vms-208): attach a block-backed reader
 * to a mounted volume in the shared FS engine. `host` is the opaque handle the
 * vmsfs_bio.h backend takes (a struct super_block *, its block size already set
 * to ODS2_BLOCK_SIZE by the ACP mount); `nblocks` is the volume size in 512-byte
 * blocks (the fd-based ods2_bdev_open()'s lseek auto-detect has no kernel
 * analog, so the caller supplies it, e.g. i_size_read(bdev->bd_inode) / 512).
 * Reads + validates the home block at LBN 1 exactly as ods2_bdev_open() does.
 */
ods2_status_t ods2_bdev_open_host(ods2_bdev_t *bv, void *host, uint32_t nblocks);
#endif

/*
 * Read raw block `lbn` (512 bytes) into `buf` (must be >= ODS2_BLOCK_SIZE)
 * via pread. Returns ODS2_ERR_RANGE if lbn is past the volume, ODS2_ERR_IO
 * on a short/failed read. The block-backed analog of ods2_volume_block(),
 * but copying into caller storage rather than returning an interior pointer.
 */
ods2_status_t ods2_bdev_read_block(const ods2_bdev_t *bv, uint32_t lbn,
                                   void *buf, size_t buf_len);

/*
 * Read + validate the primary file header for file number `fid_num` by the
 * SAME INDEXF.SYS arithmetic ods2_volume_read_header() uses
 * (idx_lbn = hm2_ibmaplbn + hm2_ibmapsize; header N at idx_lbn + (N-1)),
 * copying the validated 512-byte header into `header_out` (>= 512 bytes).
 */
ods2_status_t ods2_bdev_read_header(const ods2_bdev_t *bv, uint32_t fid_num,
                                    void *header_out, size_t out_len);

/*
 * List a directory given its (already-read) header block, walking the
 * directory's data blocks via its FM2 retrieval pointers and decoding the
 * variable-length records -- the block-backed twin of ods2_volume_list_dir().
 * Each data block is pread on demand; records are decoded by the shared
 * ods2_dir_block_scan(), so the result is byte-for-byte identical to the
 * in-memory walker over the same volume.
 */
ods2_status_t ods2_bdev_list_dir(const ods2_bdev_t *bv,
                                 const void *dir_header_block,
                                 ods2_dir_cb cb, void *ctx);

/*
 * [F16] (increment 9, vms-0f3): decode a contiguous RMS variable-length
 * (RFM=VAR) record stream -- as ods2_wvolume_create_file() now writes it,
 * and as tests/ods2/real_vax_ods2.dsk's own real-VMS-written HELLO.TXT/
 * WORLD.TXT are laid out on disk -- into a newline-joined text buffer (one
 * '\n' appended after each decoded record's content, reconstructing the
 * usual "one VMS record == one text line" convention). `data_bytes` MUST
 * be the file's true valid byte count (see ods2_recattr_data_bytes()), not
 * the raw allocated-block span, since trailing zero-fill past end-of-file
 * is not itself record-framed and would otherwise decode as spurious
 * zero-length records.
 *
 * On-disk record framing, confirmed by fully decoding both real fixture
 * files byte-for-byte against their own SHOW DEVICE-reported end-of-file
 * position (see PROVENANCE-real_vax_ods2.md's increment-9 addendum):
 * each record is a 2-byte little-endian length word followed by that many
 * content bytes; a single 0x00 pad byte follows whenever the content
 * length is odd, so the next record's length word always starts on an
 * even byte offset. Records are packed back-to-back with NO other
 * padding -- in particular a record MAY straddle a 512-byte block
 * boundary (an initial hypothesis that they could not was directly
 * disproved by this decode: real record 13 of HELLO.TXT starts at byte
 * 492 of block 1 and ends at byte 39 of block 2).
 *
 * Returns ODS2_ERR_SIZE if `out` is too small, ODS2_ERR_FORMAT if a
 * record's length word would read past `data_bytes`.
 */
ods2_status_t ods2_var_records_decode(const void *data, size_t data_bytes,
                                      char *out, size_t out_cap,
                                      size_t *out_len);

/*
 * Convenience wrapper: parse+validate a data file's own header block,
 * require it have exactly one retrieval-pointer extent (the only shape
 * ods2_wvolume_create_file() ever produces), and decode its VAR-record
 * content via ods2_var_records_decode() above.
 */
ods2_status_t ods2_file_read_text(const ods2_volume_t *vol,
                                  const void *file_header_block,
                                  char *out, size_t out_cap, size_t *out_len);

/* ================================================================
 * RUNTIME PATH RESOLUTION + CONTENT READ  (ods2/ods2_path.c)
 *
 * vms-5eb read-path foundation: compose the block-backed primitives above
 * into the two operations the live userspace RMS/DCL/MOUNT path needs --
 * resolve an on-volume path to a file/dir header+FID (walking from the MFD),
 * and read a file's content off the block device. Adds NO on-disk format
 * facts (Rule 8): it only sequences directory traversal + extent reads the
 * reader already implements. Name matching is VMS filespec semantics (a dir
 * component "SYS0" is the on-disk entry "SYS0.DIR"; a file "LOGIN.COM;3" is
 * entry "LOGIN.COM" at version 3), case-insensitive.
 * ================================================================ */

/*
 * Find an entry by name (INCLUDING its type, e.g. "SYS0.DIR" / "LOGIN.COM")
 * in the directory whose validated header block is `dir_header_block`.
 * want_version == 0 returns the highest version present; otherwise the exact
 * version. On success fills *fid_out and *version_out (either may be NULL).
 * Returns ODS2_ERR_NOTFOUND if no such entry.
 */
ods2_status_t ods2_bdev_dir_find(const ods2_bdev_t *bv,
                                 const void *dir_header_block,
                                 const char *name, uint16_t want_version,
                                 ods2_fid_t *fid_out, uint16_t *version_out);

/*
 * Resolve a directory given as an array of components WITHOUT the ".DIR"
 * type (e.g. {"SYS0","SYSCOMMON","SYSEXE"}), starting at the MFD (FID 4).
 * ndirs == 0 resolves the MFD itself. Copies the target directory's
 * validated 512-byte header into `dir_header_out` (>= ODS2_BLOCK_SIZE) and
 * its FID into *fid_out (may be NULL).
 */
ods2_status_t ods2_bdev_resolve_dir(const ods2_bdev_t *bv,
                                    const char *const *comps, unsigned ndirs,
                                    ods2_fid_t *fid_out,
                                    void *dir_header_out, size_t out_len);

/*
 * Resolve a file: walk `comps`[0..ndirs) to its directory, then find
 * `filename` ("NAME.EXT", type included; version 0 => highest). Copies the
 * file's validated header block into `file_header_out` and its FID into
 * *fid_out (may be NULL).
 */
ods2_status_t ods2_bdev_resolve_file(const ods2_bdev_t *bv,
                                     const char *const *comps, unsigned ndirs,
                                     const char *filename, uint16_t version,
                                     ods2_fid_t *fid_out,
                                     void *file_header_out, size_t out_len);

/*
 * Read a file's raw content bytes (up to its recattr valid byte count) by
 * pread-ing ALL of its retrieval-pointer extents in VBN order. Unlike
 * ods2_file_read_text(), this supports multi-extent files. Returns
 * ODS2_ERR_SIZE if out_cap is smaller than the valid byte count.
 */
ods2_status_t ods2_bdev_read_file(const ods2_bdev_t *bv,
                                  const void *file_header_block,
                                  void *out, size_t out_cap, size_t *out_len);

/*
 * Block-backed twin of ods2_file_read_text(): read a VAR-record file's
 * content and decode it to newline-joined text (one '\n' per record).
 */
ods2_status_t ods2_bdev_read_file_text(const ods2_bdev_t *bv,
                                       const void *file_header_block,
                                       char *out, size_t out_cap,
                                       size_t *out_len);

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
 *     [OVMXDIR] are directory-listable; see [F9]'s UPDATE trail.
 *   - RESOLVED (increment 9, [F16]): created-file content is written as
 *     RMS variable-length (RFM=VAR) records with per-record 2-byte LE
 *     length prefixes (on-disk framing oracle-grounded against the
 *     fixture's own HELLO.TXT), so a real VAX's RMS `TYPE` shows a created
 *     file's content, not just `DUMP`. Round-tripped through
 *     ods2_var_records_decode()/ods2_file_read_text().
 *   - RESOLVED (increment 10, [F17], vms-1bd): directories are no longer
 *     capped at one data block. ods2_wvolume_dir_insert() grows a
 *     directory across as many blocks as its name-sorted records need,
 *     allocating blocks + extending the FH2 map/recattr on demand;
 *     records never cross a 512-byte block boundary (the Files-11
 *     public-spec rule the reader already assumes). A single-block
 *     directory still serializes byte-for-byte as before, so the
 *     [F13]/[F14]/[F15] real-VAX-MOUNT-clean output is unchanged when no
 *     growth occurs. FLAGGED (Rule 8): the exact record-to-block
 *     distribution is NOT fixture-grounded -- the fixture's only 2-block
 *     directory (FID 4 MFD) has an empty, past-EOF second block and never
 *     demonstrates a live multi-block split; greedy first-fit packing is a
 *     spec-faithful OVMX choice, not claimed byte-identical to VMS's own
 *     directory maintenance. See [F17] in ods2_writer.c.
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
 * Writer handle for a volume under construction. TWO modes, selected by
 * which constructor initializes `wvol`:
 *
 *   - IN-MEMORY mode (ods2_volume_format()): `image` is a caller-owned flat
 *     buffer (typically total_blocks * ODS2_BLOCK_SIZE bytes) and every
 *     ods2_wvolume_*() call writes into it directly via plain pointer
 *     arithmetic. `is_bdev` is 0, `image` is non-NULL.
 *
 *   - BLOCK-DEVICE-BACKED mode (ods2_wvolume_format_bdev(), vms-6d3b, R2 of
 *     the real-ODS-2-runtime epic vms-5eb following R1's block-backed
 *     READER, vms-6cb / ods2_bdev.c): `image` is NULL, `is_bdev` is 1, and
 *     every touched block is committed to `bdev_fd` via pwrite instead of
 *     living in a buffer sized to the whole volume -- the WRITE counterpart
 *     of ods2_bdev_t. See the "BLOCK-DEVICE-BACKED WRITER" section below the
 *     WRITER section for the full design + provenance.
 *
 * ALL existing ods2_wvolume_*() entry points (create_file/create_dir/
 * dir_insert/alloc_blocks) work UNCHANGED in either mode -- they go through
 * the single wblk() choke point internally, which dispatches on `is_bdev`.
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

    /* ---- block-device-backed mode only (vms-6d3b) ---- */
    int            is_bdev;         /* 0 == in-memory `image` mode (above)  */
#ifdef OVMX_ODS2_KERNEL
    void          *host;            /* backing device handle for vmsfs_bget()
                                    * (kernel-resident writer, rd vms-dcd)  */
#else
    int            bdev_fd;         /* backing fd, borrowed (not owned)     */
#endif
    void          *wcache_priv;     /* opaque sparse block cache, see .c    */
    /*
     * wcache "block returned so a caller's field writes land somewhere valid
     * but harmless after a cache miss reports io_error". rd vms-dcd moved this
     * OUT of a file-scope static and INTO the handle: the kernel-resident ACP
     * runs the writer concurrently in many caller contexts, so a shared static
     * scratch block would be a cross-process data race. One per wvolume.
     */
    uint8_t        wcache_scratch[ODS2_BLOCK_SIZE];
    ods2_status_t  io_error;        /* sticky I/O/capacity failure, checked
                                      * at the tail of every public entry
                                      * point so a pread/pwrite failure deep
                                      * inside wblk() (which cannot itself
                                      * return a status -- see the .c file's
                                      * design note) is never silently
                                      * swallowed as a fabricated success. */
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
 * Create a new file whose `data_len` bytes are written to disk VERBATIM
 * (raw, byte-for-byte), NOT re-framed into RFM=VAR records the way
 * ods2_wvolume_create_file() above does. The file is stamped RFM=FIXED
 * (fat_rtype == ODS2_RTYPE_FIX, the public FAB$B_RFM==FIX$C_FIX on-disk
 * analog [S]) with a 512-byte record size; its fh2_recattr efblk/ffbyte
 * express the exact valid-byte length, so ods2_bdev_read_file() reads the
 * original `data_len` bytes back unchanged.
 *
 * WHY THIS EXISTS (vms-5eb R6-build): the system disk carries BINARY image
 * files -- DCL.EXE, LOGINOUT.EXE, IMGACT.EXE, DECC$SHR.EXE, the LINK.EXE
 * shareable graph, PARTS.EXE -- that IMGACT activates by reading their raw
 * blocks. Re-framing those bytes as newline-split VAR records (what
 * create_file() does, correct for .COM/.TXT text) would corrupt every
 * binary. A boot-master built purely on create_file() therefore cannot
 * produce a bootable genuine-ODS-2 system disk; this verbatim path is the
 * missing primitive. Text records still go through create_file(); raw
 * images go through here. Same allocation / FID / dir_insert contract as
 * create_file(): call ods2_wvolume_dir_insert() separately with the SAME
 * parent_dir.
 *
 * Rule 8: fat_rtype==1 (FIXED) and a 512-byte rsize are public Files-11 /
 * RMS FAB$B_RFM facts (see the ods2_recattr_t comment); the verbatim
 * block layout adds no new on-disk format fact beyond what the FIXED
 * record format and the existing FH2/FM2 map already describe.
 */
ods2_status_t ods2_wvolume_create_file_raw(ods2_wvolume_t *wvol,
                                           const char *name, uint16_t version,
                                           const uint8_t *data, size_t data_len,
                                           ods2_fid_t parent_dir,
                                           ods2_fid_t *fid_out);

/*
 * ods2_wvolume_create_file_stmlf() (vms-5f0, epic vms-208): the TEXT twin of
 * ods2_wvolume_create_file_raw(). Writes `data` VERBATIM (byte-for-byte, no
 * re-framing -- the same contiguous block layout create_file_raw() uses, so
 * the on-disk content bytes are identical to the host file) but stamps the
 * header RFM=STMLF (fat_rtype == ODS2_RTYPE_STMLF, the FAB$C_STMLF on-disk
 * analog [S]) with implied-CR record attributes.
 *
 * WHY THIS EXISTS: a genuine VMS text file (.COM, .DAT, SYSUAF, ...) is a
 * STREAM of LF-terminated records, NOT one giant fixed 512-byte record.
 * create_file_raw()'s RFM=FIXED/512 makes RMS/DCL read the WHOLE file as a
 * single 512-byte padded record, so a line-oriented reader (STARTUP.COM's
 * phase driver, LOGINOUT's SYSUAF scan) sees one bogus record then EOF. STMLF
 * keeps the bytes verbatim (so the byte-identical-to-/vms property the master
 * relies on holds) AND frames records on the LF the reader already expects, so
 * a $GET returns one line per call. Binary images (.EXE) stay on
 * create_file_raw() (FIXED) -- IMGACT reads their blocks, not records.
 *
 * Rule 8: fat_rtype==5 (STREAM-LF) and implied-CR (ODS2_RAT_CR) are public
 * Files-11 / RMS FAB$B_RFM / FAB$B_RAT facts; the verbatim block layout adds
 * no new on-disk format fact. Same allocation / FID / dir_insert contract as
 * create_file_raw().
 */
ods2_status_t ods2_wvolume_create_file_stmlf(ods2_wvolume_t *wvol,
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
 * Insert a {name, version, entry_fid} directory record into directory
 * `dir_fid` (as read via ods2_volume_read_header on the wvolume's own
 * image), keeping the whole directory in ascending name order ([F13]).
 * The directory GROWS beyond one block as needed ([F17], increment 10,
 * vms-1bd): each insert re-packs the sorted record stream across the
 * file's data blocks (a record never crosses a 512-byte boundary),
 * allocating additional blocks and extending the FH2 map/recattr when the
 * packed form no longer fits the current allocation.
 *
 * [vms-9794] If `name` already has a directory record, `version` is ADDED
 * as a new {version, fid} value entry to that SAME record (versions kept
 * in descending order, highest first -- matching the reader's and this
 * writer's own [F17] value-entry convention), instead of being rejected --
 * this is how a caller mints ";2"/";3" of an existing file. A duplicate
 * {name, version} pair (re-inserting the SAME version) is still rejected.
 * A single-version insert (the first version of a NAME) is byte-identical
 * to the pre-vms-9794 output -- no regression.
 *
 * Returns ODS2_ERR_ARGS on a duplicate {name, version} pair, ODS2_ERR_NOSPACE
 * if the volume has no free blocks left, the directory exceeds the writer's
 * block/extent caps (ODS2_WDIR_MAX_BLOCKS / _MAX_EXTENTS), or a name's
 * merged record would not fit in a single directory block, ODS2_ERR_FORMAT
 * if an existing directory block/record is malformed.
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

/* ================================================================
 * PURE EDIT surface (implemented in ods2/ods2_edit.c) -- the write-side twins
 * of the reader's pure parse operations, for the Files-11 ODS-2 ACP's
 * IO$_WRITEVBLK / implicit-extend path (vms-c60, epic vms-208). Each operates
 * on a caller-supplied 512-byte block buffer with NO allocation and NO I/O, so
 * the executive ACP (src/kernel-core/vmsfs_acp.c) can SEQUENCE the raw
 * exec_blockdev_* block reads/writes around them exactly as #633's IO$_ACCESS
 * sequences the pure PARSE helpers -- keeping every on-disk ODS-2 format fact in
 * the codec (Rule 8). See ods2_edit.c for the per-field provenance.
 * ================================================================ */

/* Storage-bitmap bits per 512-byte BITMAP.SYS data block ([N2]): 128 words *
 * 32 bits. The ACP maps whole-volume bit N (== LBN N, cluster factor 1) to
 * bitmap block N / this and bit N % this. */
#define ODS2_SBM_BITS_PER_BLOCK   4096u

/* Append one format-1 FM2 retrieval pointer [lbn, lbn+count) to a file header's
 * map area (bumps fh2_map_inuse). ODS2_ERR_NOSPACE if the map area is full,
 * ODS2_ERR_ARGS if count/lbn exceed the format-1 range. Reseal after. */
ods2_status_t ods2_fh2_map_append(void *header_block, uint32_t lbn, uint32_t count);

/* Set a file header's RECATTR size fields (fat_hiblk/fat_efblk/fat_ffbyte) +
 * fh2_highwater to a new end-of-file / allocation position. Reseal after. */
ods2_status_t ods2_fh2_set_eof(void *header_block, uint32_t hiblk,
                               uint32_t efblk, uint16_t ffbyte);

/* Recompute + store the FH2 additive checksum after edits (so it re-parses). */
void ods2_fh2_reseal(void *header_block);

/* Storage-bitmap (one 512-byte BITMAP.SYS data block): test a bit FREE, mark it
 * ALLOCATED, or mark it FREE (rollback). bit_in_block in 0..4095. */
int  ods2_sbm_block_bit_free(const void *bitmap_block, unsigned bit_in_block);
void ods2_sbm_block_alloc(void *bitmap_block, unsigned bit_in_block);
void ods2_sbm_block_free(void *bitmap_block, unsigned bit_in_block);

/* ================================================================
 * FILE-HEADER allocation + creation (vms-5303, epic vms-208) -- the IO$_CREATE
 * / IO$_DELETE / IO$_MODIFY write-side twins of ods2_writer.c's
 * write_fh2_header_ext() / ifile_bitmap / dir_insert. All PURE (caller-supplied
 * block buffers, no I/O, no allocation); the ACP sequences the raw block reads
 * and writes around them. See ods2_edit.c for the per-field provenance.
 * ================================================================ */

/* Index-file bitmap (INDEXF.SYS index bitmap, hm2_ibmaplbn..): OPPOSITE bit
 * sense from the storage bitmap -- a SET bit == a FID IN USE. bit_in_block is
 * (fidnum-1) % 4096; the block is index (fidnum-1) / 4096 of the bitmap. */
int  ods2_ifbm_block_fid_used(const void *bitmap_block, unsigned bit_in_block);
void ods2_ifbm_block_alloc(void *bitmap_block, unsigned bit_in_block);
void ods2_ifbm_block_free(void *bitmap_block, unsigned bit_in_block);

/* FH2 file "kind" -- the RECATTR (FAT) / efblk preset, matching
 * ods2_writer.c's internal enum fh2_kind values byte-for-byte. */
enum ods2_fh2_kind {
    ODS2_FK_SYSTEM     = 0, /* reserved-file stub (rtype 1)                   */
    ODS2_FK_DIR        = 1, /* directory (rtype 2, rattrib 0x08)              */
    ODS2_FK_DATA       = 2, /* RFM=VAR data file (rtype 2, rattrib CR)        */
    ODS2_FK_DATA_FIX   = 3, /* RFM=FIXED 512-byte data file (rtype 1)         */
    ODS2_FK_DATA_STMLF = 4  /* RFM=STMLF (stream, LF) text file (rtype 5, CR) */
};

/*
 * ods2_type_is_binary_image - is a file TYPE (extension, no dot) a binary image
 * that must be stored RFM=FIXED verbatim rather than RFM=STMLF? The single
 * source of truth for the master (tools/vmsfs_master.c) AND the live installer
 * (PRODUCT INSTALL, src/product/product.c): a byte-stream copy of an .EXE must
 * keep RFM=FIXED (a line-oriented STMLF reframing would corrupt block-read
 * image activation), while a .COM/.DAT text file must be RFM=STMLF so DCL/RMS
 * read it one LF-record at a time. Before this helper the two writers diverged:
 * the master chose per-type, the ACP installer created EVERYTHING RFM=VAR, so a
 * live-installed STARTUP.COM read back as one bogus VAR record and DCL saw the
 * file name itself as a command verb (%DCL-E-IVVERB, vms-3a8). Kept inline and
 * dependency-free so both writers link the identical list.
 */
static inline int ods2_type_is_binary_image(const char *type)
{
    static const char *const bin[] = {
        "EXE", "OLB", "OBJ", "STB", "DMP", "KIT", "GZ", "IMG", "ISO",
        "BIN", "ELF", "TLB", "MLB", "SYS", "KO",
    };
    size_t i;
    if (!type)
        return 0;
    for (i = 0; i < sizeof(bin) / sizeof(bin[0]); i++) {
        const char *a = type, *b = bin[i];
        while (*a && *a != ';' && *b) {         /* ';version' terminates the type */
            unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
            if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - 32);
            if (ca != cb) break;
            a++; b++;
        }
        if ((*a == '\0' || *a == ';') && *b == '\0')
            return 1;
    }
    return 0;
}

/*
 * ods2_kind_for_filespec - the RFM `kind` a byte-stream copy of a file named by
 * VMS filespec (or bare "NAME.TYPE") should be created with: RFM=FIXED for a
 * binary image, RFM=STMLF for everything else. Mirrors tools/vmsfs_master.c's
 * per-file choice so a live PRODUCT INSTALL lays the target volume down with the
 * SAME record formats the mastered distribution disk carries (vms-3a8).
 */
static inline unsigned ods2_kind_for_filespec(const char *filespec)
{
    const char *name = filespec, *dot = NULL, *p;
    if (!filespec)
        return ODS2_FK_DATA_STMLF;
    /* Isolate the filename: it begins after the last directory/device delimiter,
     * so dots WITHIN a rooted directory ("[SYS0.SYSCOMMON.SYSEXE]") never look
     * like a file type. */
    for (p = filespec; *p; p++)
        if (*p == ']' || *p == ':' || *p == '/' || *p == '>')
            name = p + 1;
    for (p = name; *p && *p != ';'; p++)
        if (*p == '.')
            dot = p + 1;                        /* the file type (post-last-dot) */
    if (dot && ods2_type_is_binary_image(dot))
        return ODS2_FK_DATA_FIX;
    return ODS2_FK_DATA_STMLF;
}

/* Build a complete FH2 file header into a caller-supplied 512-byte block.
 * `owner`={0,0} + `fileprot`=0 => the writer's kind default (SYSTEM [1,4],
 * 0xFA00/0xBA00). `extents`/`n_extents` are the file's initial allocation
 * (may be 0). Reseals its own checksum. ODS2_ERR_ARGS on a bad fidnum/name;
 * ODS2_ERR_NOSPACE if the extents overflow the FH2 map area. */
ods2_status_t ods2_fh2_build(void *header_block, uint32_t fidnum, uint16_t seq,
                             const char *name, uint16_t version, uint32_t filechar,
                             unsigned kind, const ods2_extent_t *extents,
                             unsigned n_extents, size_t data_len,
                             ods2_fid_t backlink, ods2_uic_t owner,
                             uint16_t fileprot, uint32_t maxfiles);

/* Rewrite an EXISTING file header's ident area (file name + version), and --
 * when `new_backlink` != NULL -- its fh2_backlink (parent-directory FID), for an
 * IO$_MODIFY!IO$M_MOVE rename/move (vms-de7). Touches ONLY the name/revision/
 * filename-extension (+ optional backlink); the FID, RECATTR/EOF, retrieval map,
 * owner/prot and dates are left byte-for-byte unchanged, so the file keeps its
 * identity + allocation. Reseal (ods2_fh2_reseal) after. ODS2_ERR_ARGS on a bad
 * block/name, ODS2_ERR_FORMAT if the header's ident offset is out of range. */
ods2_status_t ods2_fh2_rename(void *header_block, const char *name,
                              uint16_t version, const ods2_fid_t *new_backlink);

/* Insert a {name, version, entry_fid} directory record into a directory's
 * data blocks (in_blocks = in_nblk contiguous 512-byte blocks), producing the
 * repacked result in out_blocks (up to out_nblk_cap blocks) and its block
 * count in *out_nblk (may exceed in_nblk -- the ACP then allocates the growth
 * and rewrites the FH2 map). `flat` is caller scratch, >= in_nblk*512 + 528.
 * is_resfile controls dir_verlimit ([F14]). ODS2_ERR_ARGS on a duplicate
 * {name, version}; ODS2_ERR_NOSPACE if the repack exceeds out_nblk_cap. */
ods2_status_t ods2_dir_insert_blocks(const uint8_t *in_blocks, unsigned in_nblk,
                                     const char *name, unsigned namecount,
                                     uint16_t version, ods2_fid_t entry_fid,
                                     int is_resfile,
                                     uint8_t *flat, size_t flat_cap,
                                     uint8_t *out_blocks, unsigned out_nblk_cap,
                                     unsigned *out_nblk);

/* Remove `version` (0 => every version) of `name` from a directory's data
 * blocks, repacking into out_blocks. Never grows or deallocates directory
 * blocks: *out_nblk == in_nblk (trailing blocks emptied), so the ACP leaves
 * the FH2 map untouched. *removed set iff a matching entry was dropped. */
ods2_status_t ods2_dir_remove_blocks(const uint8_t *in_blocks, unsigned in_nblk,
                                     const char *name, unsigned namecount,
                                     uint16_t version,
                                     uint8_t *flat, size_t flat_cap,
                                     uint8_t *out_blocks, unsigned out_nblk_cap,
                                     unsigned *out_nblk, int *removed);

/* ================================================================
 * BLOCK-DEVICE-BACKED WRITER (implemented in ods2/ods2_writer.c) --
 * increment 11, vms-6d3b, R2 of the real-ODS-2-runtime epic vms-5eb.
 *
 * The WRITE counterpart to R1's block-backed reader (vms-6cb, ods2_bdev.c):
 * ods2_wvolume_format_bdev() + the SAME ods2_wvolume_create_file()/
 * _create_dir()/_dir_insert()/_alloc_blocks() entry points the in-memory
 * writer already uses build a genuine ODS-2 volume directly against a real
 * block device / loop-image fd (pwrite), WITHOUT ever allocating a buffer
 * sized to the whole volume. `total_blocks` in ods2_format_params_t can
 * therefore represent a real disk far larger than any buffer this process
 * could hold in RAM.
 *
 * DESIGN (Rule 8: this is layout-NEUTRAL bookkeeping, not an on-disk format
 * choice -- it changes nothing about the bytes this writer produces, only
 * WHERE they live before being committed; the byte-genuineness citations
 * for every field this writer emits are unchanged, see the WRITER section
 * above):
 *
 *   Every static helper in ods2_writer.c (write_fh2_header_ext(),
 *   bitmap_set(), write_home_block(), ods2_wvolume_dir_insert()'s
 *   flatten/repack, ...) already reaches the volume through exactly ONE
 *   choke point: a static wblk(wvol, lbn) returning a uint8_t* to that
 *   block's 512 bytes. In IN-MEMORY mode this is `wvol->image +
 *   lbn*ODS2_BLOCK_SIZE`, unchanged. In BLOCK-DEVICE-BACKED mode (this
 *   section), it instead returns a pointer into a bounded, SPARSE cache
 *   keyed by LBN (a fixed-capacity open-addressed hash table, ~2MB
 *   regardless of volume size -- OVMX design choice, not a Files-11 fact):
 *     - a cache hit returns the existing entry (so a block written earlier
 *       in the SAME top-level call, then re-touched later in that call --
 *       e.g. the alternate-index-header memcpy, or a directory's re-pack --
 *       sees its own prior writes, exactly like the in-memory buffer would);
 *     - a cache miss on an LBN this writer is about to OVERWRITE FROM
 *       SCRATCH (the fixed reserved-layout region at format() time, or a
 *       block just handed out by ods2_wvolume_alloc_blocks()'s bump
 *       allocator) is seeded ALL-ZERO without any I/O -- this writer never
 *       depends on a freshly allocated block's prior on-device content (it
 *       always either 0xFF/0x00-fills it outright or overwrites every field
 *       the byte-genuine layout defines), the same way ods2_volume_format()
 *       zeroes the WHOLE in-memory image up front; seeding just the
 *       about-to-be-used LBN range is the block-device-backed equivalent of
 *       that same zero-fill, scoped to what will actually be touched;
 *     - a cache miss on any OTHER LBN (an existing directory's data block,
 *       or its own header, read back by a LATER top-level call after an
 *       earlier one flushed and cleared the cache) is fetched via pread --
 *       always returning exactly what THIS writer itself pwrote earlier,
 *       never foreign/ambient device content, since this writer never reads
 *       a block it did not itself create.
 *   Every top-level entry point (format_bdev/create_file/create_dir/
 *   dir_insert) commits its cache to `bdev_fd` via ods2_wvolume_flush()
 *   (pwrite, whole-block, hard-failure-on-short-write -- the same shape
 *   ods2_bdev.c's pread side and the MSCP server's raw-block path use) and clears
 *   it before returning success, so the cache never holds more than one
 *   call's own working set regardless of how many files/inserts a caller
 *   makes across the volume's lifetime.
 *
 *   A pread/pwrite failure or cache-capacity overflow inside wblk() cannot
 *   itself return a status (wblk()'s signature, shared with the in-memory
 *   path, is a plain pointer accessor with no error channel) -- it is
 *   recorded in `wvol->io_error` and a scratch dummy block is returned so
 *   the caller's subsequent field writes do not crash; every public
 *   ods2_wvolume_*() entry point checks `io_error` before returning success,
 *   so a real I/O failure always surfaces as an honest ods2_status_t to the
 *   caller (never a silently-fabricated ODS2_OK) -- the same "fail honest,
 *   never fake" convention the reader side (ods2_bdev.c) and CLAUDE.md's
 *   Rule 9 executive-facility invariant require.
 * ================================================================ */

/*
 * Format a fresh, genuine ODS-2 volume directly onto `fd` (a real block
 * device / character device such as /dev/vms, or a loop-image regular
 * file) -- the block-device-backed twin of ods2_volume_format() above.
 * `span_bytes` is the usable size of the volume in bytes; pass 0 to
 * auto-detect via lseek(fd, 0, SEEK_END), same convention as
 * ods2_bdev_open(). Every block this call touches (the fixed reserved
 * layout -- home block pair, index file bitmap, the ten reserved files,
 * BITMAP.SYS's SCB + storage bitmap, the MFD) is committed via pwrite
 * before this call returns; NO buffer sized to `params->total_blocks` is
 * ever allocated. `wvol` is initialized (is_bdev == 1) for subsequent
 * ods2_wvolume_create_file()/_create_dir()/_dir_insert() calls, which work
 * completely unchanged from the in-memory path.
 */
ods2_status_t ods2_wvolume_format_bdev(int fd, uint64_t span_bytes,
                                       const ods2_format_params_t *params,
                                       ods2_wvolume_t *wvol);

/*
 * Commit every block currently held in `wvol`'s sparse cache to its backing
 * fd via pwrite, then clear the cache (freeing it to hold the next call's
 * working set). A no-op returning ODS2_OK in in-memory mode. Called
 * automatically by every top-level ods2_wvolume_*() entry point in
 * block-device-backed mode when it succeeds; exposed publicly for a caller
 * that wants an explicit sync point (e.g. before reading the volume back
 * with ods2_bdev_* over the same fd).
 */
ods2_status_t ods2_wvolume_flush(ods2_wvolume_t *wvol);

/*
 * Release `wvol`'s sparse block cache (best-effort ods2_wvolume_flush()
 * first, then frees the cache memory). Does NOT close `bdev_fd` -- exactly
 * like ods2_bdev_t, the fd is borrowed, not owned. A no-op in in-memory
 * mode. Safe to call on an already-closed/never-opened `wvol`.
 */
void ods2_wvolume_close(ods2_wvolume_t *wvol);

/*
 * OPEN AN EXISTING volume for incremental WRITE (vms-02e, epic vms-5eb, the
 * WRITE half of the ODS-2 runtime flip). The reattach twin of
 * ods2_wvolume_format_bdev(): instead of laying down a fresh reserved layout,
 * it RECONSTRUCTS the writer's bump-allocator state (next free block / next
 * free FID) + the fixed-layout LBN fields from a volume THIS writer (or
 * INITIALIZE) already formatted onto `fd`, so a subsequent
 * ods2_wvolume_create_file()/_create_file_raw()/_create_dir()/_dir_insert()/
 * _append_file() continues allocating exactly where the volume left off.
 * `span_bytes` follows the ods2_bdev_open() convention (0 == auto-detect via
 * lseek). Validates the home block (checksums + DECFILE11B + level) and
 * CROSS-CHECKS the reconstructed geometry against the on-disk home block
 * (hm2_ibmaplbn / hm2_ibmapsize) -- a volume whose layout this writer cannot
 * reconstruct is refused with ODS2_ERR_FORMAT (fail-honest, Rule 9 / INV-6),
 * never silently mis-appended to.
 *
 * ADDITIVE: reads only the home block, SCB, and the two on-disk bitmaps to
 * rebuild the allocator watermark; writes NOTHING until the caller performs
 * an actual create/append. `wvol` is initialized (is_bdev == 1); the caller
 * closes it with ods2_wvolume_close() (which flushes + frees, leaving `fd`
 * open -- borrowed, not owned).
 *
 * Returns ODS2_OK, ODS2_ERR_ARGS (bad fd/wvol), the ods2_bdev_open() error
 * for a non-genuine volume, ODS2_ERR_FORMAT (SCB/home disagree or a layout
 * this writer did not produce), ODS2_ERR_SIZE (volume too small), or
 * ODS2_ERR_IO / ODS2_ERR_NOSPACE (backing-store read / cache-alloc failure).
 */
ods2_status_t ods2_wvolume_open_bdev(int fd, uint64_t span_bytes,
                                     ods2_wvolume_t *wvol);

/*
 * APPEND `data_len` verbatim bytes to the END of an EXISTING file identified
 * by `file_fid` on `wvol` (vms-02e). The write twin of
 * ods2_wvolume_create_file_raw() for a file that already exists: it extends
 * the file's FM2 retrieval-pointer allocation on demand (allocating and
 * chaining additional extents when the appended bytes overflow the current
 * allocation, merging a physically-contiguous run into the last extent) and
 * updates the FH2 header's end-of-file position (fat_efblk / fat_ffbyte /
 * fat_hiblk / fh2_highwater) + checksum so a following ods2_bdev_read_file()
 * returns the FULL pre-existing + appended content, byte-exact. An
 * OPERATOR.LOG-style repeated append is exactly this call, repeated.
 *
 * Rule 8: the extent-extension + EOF-position update reuse the SAME on-disk
 * field encodings ods2_wvolume_create_file_raw() already writes for an
 * RFM=FIXED file (see write_fh2_header_ext()'s FH2_KIND_DATA_FIX branch and
 * the ods2_recattr_t / [F15]/[F16] provenance) -- no new on-disk format fact
 * is introduced; the appended bytes and the grown map/EOF are the same shape
 * a from-scratch create_file_raw() of the concatenation would have produced.
 *
 * DEFINED ONLY for RFM=FIXED (verbatim, create_file_raw-shaped) data files --
 * the shape SYS$DISK's images/logs carry. Appending raw bytes to an RFM=VAR
 * text file or a directory would corrupt its record/directory framing, so
 * those are refused with ODS2_ERR_ARGS (fail-honest), never silently
 * mis-framed. A zero-length append is a successful no-op.
 *
 * Returns ODS2_OK, ODS2_ERR_ARGS (bad args / not a FIXED data file),
 * ODS2_ERR_NOTFOUND (the header does not self-report `file_fid`),
 * ODS2_ERR_CHECKSUM / ODS2_ERR_FORMAT (corrupt existing header),
 * ODS2_ERR_NOSPACE (no free blocks / extent-map full), or ODS2_ERR_IO.
 */
ods2_status_t ods2_wvolume_append_file(ods2_wvolume_t *wvol,
                                       ods2_fid_t file_fid,
                                       const void *data, size_t data_len);

/*
 * RENAME/MOVE an existing file (vms-de7, epic vms-208 -- the userspace-writer
 * twin of the executive ACP's IO$_MODIFY!IO$M_MOVE). Removes {oldname, oldver}
 * (oldver 0 => every version) from `src_dir`, inserts {newname, newver} in
 * `dst_dir` (may equal src_dir), and rewrites the file header's ident name (+
 * fh2_backlink on a cross-directory move). The file KEEPS its FID, allocation
 * and data -- only its name (and parent) change. Insert-new-then-remove-old
 * order (the file is never unreferenced, the crash-safe XQP order). `file_fid`
 * must be the file's real FID; the header at its slot must self-report it.
 *
 * Rule 8: every on-disk fact is the codec's (ods2_dir_insert_blocks /
 * ods2_dir_remove_blocks / ods2_fh2_rename) -- no new format fact. Returns
 * ODS2_OK, ODS2_ERR_ARGS (bad args / dst not a directory), ODS2_ERR_NOTFOUND
 * (header does not self-report file_fid, or old {name,version} absent),
 * ODS2_ERR_NOSPACE (dir growth needs blocks and none free), or the underlying
 * checksum/format/IO error.
 */
ods2_status_t ods2_wvolume_rename(ods2_wvolume_t *wvol,
                                  ods2_fid_t src_dir, const char *oldname,
                                  uint16_t oldver,
                                  ods2_fid_t dst_dir, const char *newname,
                                  uint16_t newver, ods2_fid_t file_fid);

#ifdef __cplusplus
}
#endif

#endif /* _VMSFS_ODS2_H */
