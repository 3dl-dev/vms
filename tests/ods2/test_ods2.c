/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2.c - Unit tests for the genuine ODS-2 structures + reader.
 *
 * Two kinds of assertion:
 *   (1) BYTE-LAYOUT: every structure's size and each load-bearing field
 *       offset matches the public Files-11 / Nankervis layout. (The header
 *       also enforces these at compile time via _Static_assert; here we
 *       re-assert at runtime so a failure is a clear test failure, and we
 *       pin a few values the compile-time asserts don't, e.g. FID/UIC.)
 *   (2) READER: build a spec-conformant ODS-2 volume image in memory and
 *       drive the reader end-to-end — home-block parse + checksum validation,
 *       INDEXF.SYS header read, FM2 retrieval-pointer decode, directory list,
 *       plus negative cases (corrupt checksum, bad format string).
 *
 * VALIDATION SCOPE:
 *   This test CONSTRUCTS a spec-conformant image, which proves the reader is
 *   self-consistent with the documented layout -- NOT that the layout is
 *   byte-identical to what a real OpenVMS VAX writes. That byte-genuineness
 *   claim is proven separately, in test_ods2_real.c, which drives this same
 *   reader over a REAL OpenVMS VAX V7.3 volume image pulled from lab-2
 *   (tests/ods2/real_vax_ods2.dsk; see PROVENANCE-real_vax_ods2.md). Keep
 *   both: this file's hand-built image gives fast, exact negative-case
 *   control (corrupt checksum, bad format) that a real image can't easily
 *   provide on demand.
 */

#include "vmsfs/ods2.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_failures = 0;

#define CHECK(cond, msg) do {                                           \
    if (!(cond)) {                                                      \
        printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);     \
        g_failures++;                                                   \
    }                                                                   \
} while (0)

#define CHECK_EQ(a, b, msg) do {                                        \
    long _va = (long)(a), _vb = (long)(b);                             \
    if (_va != _vb) {                                                   \
        printf("  FAIL: %s: got %ld, want %ld  (%s:%d)\n",             \
               (msg), _va, _vb, __FILE__, __LINE__);                    \
        g_failures++;                                                   \
    }                                                                   \
} while (0)

/* ---------------------------------------------------------------- */
/* (1) byte-layout assertions                                        */
/* ---------------------------------------------------------------- */

static void test_layout(void)
{
    printf("test_layout: struct sizes + field offsets\n");

    CHECK_EQ(sizeof(ods2_uic_t),     4,   "sizeof UIC");
    CHECK_EQ(sizeof(ods2_fid_t),     6,   "sizeof FID");
    CHECK_EQ(sizeof(ods2_recattr_t), 32,  "sizeof RECATTR");
    CHECK_EQ(sizeof(ods2_home_t),    512, "sizeof HOME");
    CHECK_EQ(sizeof(ods2_fh2_t),     512, "sizeof FH2");
    CHECK_EQ(sizeof(ods2_ident_t),   120, "sizeof IDENT");
    CHECK_EQ(sizeof(ods2_scb_t),     512, "sizeof SCB");

    CHECK_EQ(offsetof(ods2_home_t, hm2_struclev),  12,  "hm2_struclev@12");
    CHECK_EQ(offsetof(ods2_home_t, hm2_cluster),   14,  "hm2_cluster@14");
    CHECK_EQ(offsetof(ods2_home_t, hm2_ibmaplbn),  24,  "hm2_ibmaplbn@24");
    CHECK_EQ(offsetof(ods2_home_t, hm2_maxfiles),  28,  "hm2_maxfiles@28");
    CHECK_EQ(offsetof(ods2_home_t, hm2_checksum1), 58,  "hm2_checksum1@58");
    CHECK_EQ(offsetof(ods2_home_t, hm2_volname),   472, "hm2_volname@472");
    CHECK_EQ(offsetof(ods2_home_t, hm2_format),    496, "hm2_format@496");
    CHECK_EQ(offsetof(ods2_home_t, hm2_checksum2), 510, "hm2_checksum2@510");

    CHECK_EQ(offsetof(ods2_fh2_t, fh2_fid),       8,   "fh2_fid@8");
    CHECK_EQ(offsetof(ods2_fh2_t, fh2_recattr),   20,  "fh2_recattr@20");
    CHECK_EQ(offsetof(ods2_fh2_t, fh2_map_inuse), 58,  "fh2_map_inuse@58");
    CHECK_EQ(offsetof(ods2_fh2_t, fh2_checksum),  510, "fh2_checksum@510");

    CHECK_EQ(offsetof(ods2_ident_t, fi2_revision), 20, "fi2_revision@20");

    /* FID number reconstruction: num + (nmx << 16). */
    {
        ods2_fid_t f = { 0x0002, 1, 0, 0x01 };  /* nmx=1 -> 0x10002 */
        CHECK_EQ(ods2_fid_number(&f), 0x10002, "fid number nmx<<16");
    }
    /* RECATTR word-swapped longword reconstruction. */
    {
        ods2_recattr_t r;
        memset(&r, 0, sizeof(r));
        r.fat_hiblk[0] = 0x1234; r.fat_hiblk[1] = 0x0001;
        CHECK_EQ(ods2_recattr_hiblk(&r), 0x00011234, "recattr hiblk lo+hi<<16");
    }
}

/* ---------------------------------------------------------------- */
/* helpers to build a spec-conformant image                          */
/* ---------------------------------------------------------------- */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

#define IMG_BLOCKS   32
#define BLK          ODS2_BLOCK_SIZE

/* Volume geometry chosen for the test image. */
#define IBMAP_LBN    2
#define IBMAP_SIZE   1
#define IDX_BASE     (IBMAP_LBN + IBMAP_SIZE)   /* file header for FID N at IDX_BASE + (N-1) */
#define MAXFILES     16
#define MFD_DATA_LBN 20
#define TXT_DATA_LBN 21

/* Header area offsets (in words) chosen to sit after the 108-byte fixed part. */
#define ID_OFF_WORDS 54     /* byte 108 */
#define MP_OFF_WORDS 114    /* byte 228 (after 120-byte ident) */

static uint8_t *block_ptr(uint8_t *img, uint32_t lbn)
{
    return img + (size_t)lbn * BLK;
}

/* Fill in the two additive checksums of a home block in place. */
static void finalize_home_checksums(uint8_t *hb)
{
    /* checksum1 = sum of first 29 words -> stored at offset 58 (word 29). */
    put16(hb + 58, ods2_checksum(hb, 29));
    /* checksum2 = sum of first 255 words -> stored at offset 510 (word 255). */
    put16(hb + 510, ods2_block_checksum(hb));
}

static void build_home(uint8_t *img)
{
    uint8_t *hb = block_ptr(img, 1);
    memset(hb, 0, BLK);

    put32(hb + offsetof(ods2_home_t, hm2_homelbn),  1);
    put16(hb + offsetof(ods2_home_t, hm2_struclev), ODS2_STRUCLEV_V2);
    put16(hb + offsetof(ods2_home_t, hm2_cluster),  1);
    put32(hb + offsetof(ods2_home_t, hm2_ibmaplbn), IBMAP_LBN);
    put32(hb + offsetof(ods2_home_t, hm2_maxfiles), MAXFILES);
    put16(hb + offsetof(ods2_home_t, hm2_ibmapsize), IBMAP_SIZE);
    put16(hb + offsetof(ods2_home_t, hm2_resfiles),  5);
    put16(hb + offsetof(ods2_home_t, hm2_rvn),       0);

    memcpy(hb + offsetof(ods2_home_t, hm2_volname), "OVMXTEST    ", 12);
    memcpy(hb + offsetof(ods2_home_t, hm2_format),  ODS2_FORMAT_STRING, 12);

    finalize_home_checksums(hb);
}

/*
 * Build a file header at LBN for file `fidnum`, with ident filename `name`
 * (space-padded) and an optional single FM2 format-1 retrieval pointer
 * mapping `map_count` blocks starting at `map_lbn` (map_count==0 -> no map).
 */
static void build_header(uint8_t *img, uint32_t fidnum, const char *name,
                         uint32_t map_lbn, uint32_t map_count)
{
    uint32_t lbn = IDX_BASE + (fidnum - 1);
    uint8_t *h = block_ptr(img, lbn);
    unsigned map_inuse = 0;
    ods2_ident_t id;

    memset(h, 0, BLK);

    h[offsetof(ods2_fh2_t, fh2_idoffset)] = ID_OFF_WORDS;
    h[offsetof(ods2_fh2_t, fh2_mpoffset)] = MP_OFF_WORDS;
    h[offsetof(ods2_fh2_t, fh2_acoffset)] = MP_OFF_WORDS; /* empty access area */
    h[offsetof(ods2_fh2_t, fh2_rsoffset)] = 255;

    put16(h + offsetof(ods2_fh2_t, fh2_struclev), ODS2_STRUCLEV_V2);
    /* fh2_fid: num, seq, rvn, nmx */
    put16(h + offsetof(ods2_fh2_t, fh2_fid) + 0, (uint16_t)(fidnum & 0xFFFF));
    put16(h + offsetof(ods2_fh2_t, fh2_fid) + 2, 1); /* seq */
    h[offsetof(ods2_fh2_t, fh2_fid) + 4] = 0;        /* rvn */
    h[offsetof(ods2_fh2_t, fh2_fid) + 5] = (uint8_t)(fidnum >> 16); /* nmx */

    /* ident area */
    memset(&id, 0, sizeof(id));
    memset(id.fi2_filename, ' ', sizeof(id.fi2_filename));
    {
        size_t n = strlen(name);
        if (n > sizeof(id.fi2_filename)) n = sizeof(id.fi2_filename);
        memcpy(id.fi2_filename, name, n);
    }
    put16((uint8_t *)&id.fi2_revision, 1);
    memcpy(h + (size_t)ID_OFF_WORDS * 2, &id, sizeof(id));

    /* map area: one FM2 format-1 pointer if requested */
    if (map_count > 0) {
        uint8_t *mp = h + (size_t)MP_OFF_WORDS * 2;
        /* format 1: word0 = (1<<14) | ((count-1)&0xFF) | high LBN bits;
         * word1 = low 16 bits of LBN. Keep count/LBN small so high bits 0. */
        uint16_t w0 = (uint16_t)(0x4000 | ((map_count - 1) & 0xFF));
        put16(mp + 0, w0);
        put16(mp + 2, (uint16_t)(map_lbn & 0xFFFF));
        map_inuse = 2;
    }
    h[offsetof(ods2_fh2_t, fh2_map_inuse)] = (uint8_t)map_inuse;

    /* checksum over first 255 words -> offset 510 */
    put16(h + 510, ods2_block_checksum(h));
}

/* Build the MFD directory data block: one entry "HELLO.TXT";1 -> FID 5. */
static void build_dir_block(uint8_t *img)
{
    uint8_t *d = block_ptr(img, MFD_DATA_LBN);
    const char *nm = "HELLO.TXT";
    unsigned namecount = (unsigned)strlen(nm);
    unsigned off = 0, name_off, val_off, rec_end;

    memset(d, 0xFF, BLK);   /* 0xFFFF fill = end-of-records everywhere */

    name_off = off + 6;
    val_off  = name_off + namecount;
    if (val_off & 1) val_off++;               /* word align */
    rec_end  = val_off + (unsigned)sizeof(ods2_dir_ent_t);

    put16(d + off + 0, (uint16_t)(rec_end - 2)); /* dir_size: bytes after size word */
    put16(d + off + 2, 1);                        /* dir_verlimit */
    d[off + 4] = 0;                               /* dir_flags */
    d[off + 5] = (uint8_t)namecount;              /* dir_namecount */
    memcpy(d + name_off, nm, namecount);

    put16(d + val_off + 0, 1);                    /* version 1 */
    put16(d + val_off + 2, 5);                    /* fid_num = 5 */
    put16(d + val_off + 4, 1);                    /* fid_seq = 1 */
    d[val_off + 6] = 0;                           /* fid_rvn */
    d[val_off + 7] = 0;                           /* fid_nmx */

    /* rec_end onward stays 0xFF -> next dir_size reads 0xFFFF (end). */
    (void)rec_end;
}

static void build_image(uint8_t *img)
{
    memset(img, 0, (size_t)IMG_BLOCKS * BLK);
    build_home(img);
    /* reserved files 1..4, plus a data file at FID 5 */
    build_header(img, ODS2_FID_INDEXF, "INDEXF.SYS", 0, 0);
    build_header(img, ODS2_FID_BITMAP, "BITMAP.SYS", 0, 0);
    build_header(img, ODS2_FID_BADBLK, "BADBLK.SYS", 0, 0);
    build_header(img, ODS2_FID_MFD,    "000000.DIR", MFD_DATA_LBN, 1);
    build_header(img, 5,               "HELLO.TXT",  TXT_DATA_LBN, 1);
    build_dir_block(img);
    /* HELLO.TXT data */
    memcpy(block_ptr(img, TXT_DATA_LBN), "hello ODS-2\n", 12);
}

/* ---------------------------------------------------------------- */
/* (2) reader tests                                                  */
/* ---------------------------------------------------------------- */

static int map_collect(const ods2_extent_t *ext, void *ctx)
{
    ods2_extent_t *slot = (ods2_extent_t *)ctx;
    *slot = *ext;   /* remember last (test uses single-extent files) */
    return 0;
}

struct dir_capture {
    char     name[64];
    unsigned name_len;
    uint16_t version;
    uint32_t fid_num;
    int      count;
};

static int dir_collect(const char *name, unsigned name_len,
                       uint16_t version, const ods2_fid_t *fid, void *ctx)
{
    struct dir_capture *c = (struct dir_capture *)ctx;
    c->count++;
    if (name_len < sizeof(c->name)) {
        memcpy(c->name, name, name_len);
        c->name[name_len] = '\0';
    }
    c->name_len = name_len;
    c->version  = version;
    c->fid_num  = ods2_fid_number(fid);
    return 0;
}

static void test_reader(void)
{
    static uint8_t img[IMG_BLOCKS * BLK];
    ods2_volume_t vol;
    ods2_status_t st;
    uint8_t hdr[BLK];
    const ods2_ident_t *id;
    ods2_extent_t ext;
    unsigned n;
    struct dir_capture cap;

    printf("test_reader: home parse, header read, map decode, dir list\n");

    build_image(img);

    /* ---- home block ---- */
    st = ods2_volume_open(&vol, img, sizeof(img));
    CHECK_EQ(st, ODS2_OK, "ods2_volume_open");
    CHECK_EQ(vol.home.hm2_struclev, ODS2_STRUCLEV_V2, "struclev == 0x0201");
    CHECK_EQ(vol.home.hm2_maxfiles, MAXFILES, "maxfiles");
    CHECK(memcmp(vol.home.hm2_volname, "OVMXTEST    ", 12) == 0, "volname");
    CHECK(memcmp(vol.home.hm2_format, ODS2_FORMAT_STRING, 12) == 0, "format");

    /* ---- read the MFD header (FID 4) and check its ident ---- */
    st = ods2_volume_read_header(&vol, ODS2_FID_MFD, hdr, sizeof(hdr));
    CHECK_EQ(st, ODS2_OK, "read_header(MFD)");
    id = ods2_fh2_ident(hdr);
    CHECK(id != NULL, "ident(MFD) non-NULL");
    if (id)
        CHECK(memcmp(id->fi2_filename, "000000.DIR", 10) == 0, "MFD ident name");

    /* ---- read HELLO.TXT header (FID 5), decode its map ---- */
    st = ods2_volume_read_header(&vol, 5, hdr, sizeof(hdr));
    CHECK_EQ(st, ODS2_OK, "read_header(HELLO.TXT)");
    id = ods2_fh2_ident(hdr);
    if (id)
        CHECK(memcmp(id->fi2_filename, "HELLO.TXT", 9) == 0, "TXT ident name");

    memset(&ext, 0, sizeof(ext));
    n = 0;
    st = ods2_fh2_map_walk(hdr, map_collect, &ext, &n);
    CHECK_EQ(st, ODS2_OK, "map_walk(HELLO.TXT)");
    CHECK_EQ(n, 1, "one extent");
    CHECK_EQ(ext.lbn, TXT_DATA_LBN, "extent lbn");
    CHECK_EQ(ext.count, 1, "extent count");

    /* the extent's data is the file contents */
    {
        const uint8_t *data = ods2_volume_block(&vol, ext.lbn);
        CHECK(data != NULL, "data block");
        if (data)
            CHECK(memcmp(data, "hello ODS-2\n", 12) == 0, "file contents");
    }

    /* ---- list the MFD directory ---- */
    st = ods2_volume_read_header(&vol, ODS2_FID_MFD, hdr, sizeof(hdr));
    CHECK_EQ(st, ODS2_OK, "re-read MFD header");
    memset(&cap, 0, sizeof(cap));
    st = ods2_volume_list_dir(&vol, hdr, dir_collect, &cap);
    CHECK_EQ(st, ODS2_OK, "list_dir");
    CHECK_EQ(cap.count, 1, "one dir entry");
    CHECK(strcmp(cap.name, "HELLO.TXT") == 0, "dir entry name");
    CHECK_EQ(cap.version, 1, "dir entry version");
    CHECK_EQ(cap.fid_num, 5, "dir entry fid");
}

static void test_negative(void)
{
    static uint8_t img[IMG_BLOCKS * BLK];
    ods2_volume_t vol;
    ods2_status_t st;

    printf("test_negative: corrupt checksum + bad format rejected\n");

    /* corrupt the home block's checksum2 -> parse must reject */
    build_image(img);
    img[1 * BLK + 510] ^= 0xFF;   /* flip a byte of hm2_checksum2 */
    st = ods2_volume_open(&vol, img, sizeof(img));
    CHECK_EQ(st, ODS2_ERR_CHECKSUM, "corrupt checksum rejected");

    /* valid checksums but wrong format string -> ODS2_ERR_FORMAT */
    build_image(img);
    memcpy(img + 1 * BLK + offsetof(ods2_home_t, hm2_format), "NOTAVMSVOL  ", 12);
    finalize_home_checksums(img + 1 * BLK);  /* re-checksum so only format is wrong */
    st = ods2_volume_open(&vol, img, sizeof(img));
    CHECK_EQ(st, ODS2_ERR_FORMAT, "bad format string rejected");

    /* too-small image */
    st = ods2_volume_open(&vol, img, BLK);   /* only 1 block */
    CHECK_EQ(st, ODS2_ERR_SIZE, "undersized image rejected");
}

int main(void)
{
    printf("=== ODS-2 genuine structures + reader unit tests ===\n");
    test_layout();
    test_reader();
    test_negative();

    if (g_failures == 0) {
        printf("PASS: all ODS-2 reader/layout checks\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
