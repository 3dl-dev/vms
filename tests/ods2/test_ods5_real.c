/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods5_real.c - the codec against REAL OpenVMS Alpha V8.4 ODS-5 volumes
 * (rd vms-7e7e, epic vms-810).
 *
 * Fixtures: tests/lab/captures/ods5-alpha-20261022/{ods5t,ods5s,ods5e}.dsk,
 * each INITIALIZE/STRUCTURE=5'd and populated by the real system and copied
 * out byte for byte (RESULTS.md there says how, and what the system reported).
 * Every expected value below is from that capture, not from this codec.
 *
 * Proves, per layer of the codec:
 *   1. home block + SCB at level 5 parse (strict); a level the codec does not
 *      read (3) is still refused -- negative control.
 *   2. FI5 ident decode: case-preserved names, name types, the 73-byte name,
 *      the UCS-2 name, the FI2 header of a reserved file on the same volume.
 *   3. directory records: the typed scan sees every record with its name
 *      type; the 8-bit scan omits only the UCS-2 ones.
 *   4. name rules: ods5_name_type gives every 8-bit name the type the real
 *      system stored; ods5_name_cmp orders every real directory as stored.
 *   5. directory REBUILD: inserting the real files, in creation order, into a
 *      block holding only the real UCS-2 records reproduces the real directory
 *      block byte for byte -- including E.txt;2 merged case-blind into E.txt.
 *      The same inserts in ODS-2 mode do NOT -- negative control.
 *   6. header BUILD: a level-5 header for c++config.h / the 73-byte name lays
 *      out ident + map exactly where the real headers have them; rename moves
 *      the map with the name.
 *   7. ODS-2 behaviour: the real VAX ODS-2 fixture still reads (ident_get on
 *      an FI2 header) -- the full ODS-2 regression is the rest of tests/ods2.
 */

#include "vmsfs/ods2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond, msg) do {                                          \
    if (!(cond)) {                                                     \
        printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);    \
        g_failures++;                                                  \
    }                                                                  \
} while (0)

#define CHECK_EQ(a, b, msg) do {                                       \
    long _va = (long)(a), _vb = (long)(b);                             \
    if (_va != _vb) {                                                  \
        printf("  FAIL: %s: got %ld, want %ld  (%s:%d)\n",            \
               (msg), _va, _vb, __FILE__, __LINE__);                   \
        g_failures++;                                                  \
    }                                                                  \
} while (0)

static uint8_t *load(const char *dir, const char *name, size_t *len_out)
{
    char path[1024];
    FILE *f;
    uint8_t *buf;
    long sz;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f) {
        printf("  FAIL: cannot open fixture %s\n", path);
        g_failures++;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        printf("  FAIL: cannot read fixture %s\n", path);
        g_failures++;
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len_out = (size_t)sz;
    return buf;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* The one data block of a directory file (both real test directories are
 * one block). */
static const uint8_t *dir_block(const ods2_volume_t *vol, uint32_t fid, uint8_t *hdr)
{
    uint32_t lbn = 0;

    if (ods2_volume_read_header(vol, fid, hdr, ODS2_BLOCK_SIZE) != ODS2_OK)
        return NULL;
    if (ods2_fh2_map_vbn(hdr, 1, &lbn) != ODS2_OK)
        return NULL;
    return ods2_volume_block(vol, lbn);
}

/* ---- record walking (test-local, independent of the codec's scanner) ---- */

struct rec { unsigned off, len, type, nc; const uint8_t *name; uint16_t ver; uint16_t fidnum; };

static unsigned recs_of(const uint8_t *blk, struct rec *r, unsigned max)
{
    unsigned off = 0, n = 0;

    while (off + 6 <= ODS2_BLOCK_SIZE && n < max) {
        uint16_t sz = rd16(blk + off);
        unsigned vo;
        if (sz == 0xFFFF)
            break;
        r[n].off = off;
        r[n].len = 2u + sz;
        r[n].type = ODS5_DIR_NAMETYPE(blk[off + 4]);
        r[n].nc = blk[off + 5];
        r[n].name = blk + off + 6;
        vo = off + 6 + r[n].nc;
        if (vo & 1) vo++;
        r[n].ver = rd16(blk + vo);
        r[n].fidnum = rd16(blk + vo + 2);
        n++;
        off += 2u + sz;
    }
    return n;
}

/* ---- 1. home block / SCB ---- */

static void test_home_scb(const uint8_t *img)
{
    ods2_home_t home;
    ods2_scb_t scb;
    uint8_t bad[ODS2_BLOCK_SIZE];

    printf("[1] level-5 home block + SCB\n");
    CHECK_EQ(ods2_home_parse(img + 512, 512, &home, 1), ODS2_OK, "home parses strict");
    CHECK_EQ(home.hm2_struclev, ODS2_STRUCLEV_V5, "home struclev 0x0501 (R1)");
    CHECK_EQ(ods2_scb_parse(img + 5 * 512, 512, &scb), ODS2_OK, "SCB parses");
    CHECK_EQ(scb.scb_struclev, ODS2_STRUCLEV_V5, "SCB struclev 0x0501 (R1)");
    CHECK_EQ(scb.scb_volsize, 2000, "SCB volume size = the 2000-block LD container");

    /* Negative control: level 3 is not a level this codec reads. */
    memcpy(bad, img + 512, 512);
    bad[12] = 0x01; bad[13] = 0x03;
    {
        uint16_t c1 = ods2_checksum(bad, 29);
        bad[58] = (uint8_t)c1; bad[59] = (uint8_t)(c1 >> 8);
        c1 = ods2_block_checksum(bad);
        bad[510] = (uint8_t)c1; bad[511] = (uint8_t)(c1 >> 8);
    }
    CHECK_EQ(ods2_home_parse(bad, 512, &home, 1), ODS2_ERR_FORMAT, "level 3 refused (strict)");
    CHECK_EQ(ods2_home_parse(bad, 512, &home, 0), ODS2_OK, "level 3 accepted non-strict");
}

/* ---- 2. ident decode ---- */

static void expect_ident(const ods2_volume_t *vol, uint32_t fid, const char *name,
                         unsigned namelen, unsigned type, unsigned rev)
{
    uint8_t hdr[ODS2_BLOCK_SIZE];
    ods2_hdr_ident_t id;
    char msg[160];

    snprintf(msg, sizeof(msg), "FID %u header reads", fid);
    CHECK_EQ(ods2_volume_read_header(vol, fid, hdr, sizeof(hdr)), ODS2_OK, msg);
    snprintf(msg, sizeof(msg), "FID %u ident_get", fid);
    CHECK_EQ(ods2_fh2_ident_get(hdr, &id), ODS2_OK, msg);
    snprintf(msg, sizeof(msg), "FID %u name length", fid);
    CHECK_EQ(id.namelen, namelen, msg);
    snprintf(msg, sizeof(msg), "FID %u name bytes", fid);
    CHECK(id.namelen == namelen && memcmp(id.name, name, namelen) == 0, msg);
    snprintf(msg, sizeof(msg), "FID %u name type", fid);
    CHECK_EQ(id.nametype, type, msg);
    snprintf(msg, sizeof(msg), "FID %u revision", fid);
    CHECK_EQ(id.revision, rev, msg);
}

static void test_ident(const ods2_volume_t *t)
{
    uint8_t hdr[ODS2_BLOCK_SIZE];
    static const char ucs2[] = { 0x2D, 0x4E, '.', 0, 't', 0, 'x', 0, 't', 0, ';', 0, '1', 0 };
    const char *longn = "this_is_a_long_extended_file_name_well_past_the_ods2_limit_of_39.header;1";

    printf("[2] FI5 ident decode (R2-R4)\n");
    expect_ident(t, 12, "c++config.h;1", 13, ODS5_NAMETYPE_ISL1, 1);
    expect_ident(t, 14, "MixedCase.Txt;1", 15, ODS5_NAMETYPE_ODS2, 1);
    expect_ident(t, 16, "caf\xe9.txt;1", 10, ODS5_NAMETYPE_ISL1, 1);
    expect_ident(t, 17, ucs2, 14, ODS5_NAMETYPE_UCS2, 1);
    expect_ident(t, 18, longn, 73, ODS5_NAMETYPE_ISL1, 1);
    expect_ident(t, 11, "TEST.DIR;1", 10, ODS5_NAMETYPE_ODS2, 0);
    /* Reserved file on the ODS-5 volume: an ODS-2 (FI2) header (R2). */
    expect_ident(t, 4, "000000.DIR;1", 12, ODS5_NAMETYPE_ODS2, 1);
    CHECK_EQ(ods2_volume_read_header(t, 4, hdr, sizeof(hdr)), ODS2_OK, "MFD header");
    CHECK_EQ(rd16(hdr + 6), ODS2_STRUCLEV_V2, "MFD header is level 2 on an ODS-5 volume (R2)");
    CHECK_EQ(ods2_volume_read_header(t, 18, hdr, sizeof(hdr)), ODS2_OK, "FID 18 header");
    CHECK_EQ(hdr[0], 40, "ODS-5 header idoffset 40 (R3)");
    CHECK_EQ(hdr[1], 115, "73-byte name: mpoffset 40 + (76+74)/2 (R4)");
}

/* ---- 3. scanners ---- */

struct scan_ctx { unsigned n, ucs2; };

static int tcb_count(const char *name, unsigned len, unsigned type, uint16_t ver,
                     const ods2_fid_t *fid, void *ctx)
{
    struct scan_ctx *c = (struct scan_ctx *)ctx;
    (void)name; (void)len; (void)ver; (void)fid;
    c->n++;
    if (type == ODS5_NAMETYPE_UCS2)
        c->ucs2++;
    return 0;
}

static int cb_count(const char *name, unsigned len, uint16_t ver,
                    const ods2_fid_t *fid, void *ctx)
{
    (void)name; (void)len; (void)ver; (void)fid;
    ((struct scan_ctx *)ctx)->n++;
    return 0;
}

static void test_scan(const ods2_volume_t *t, const ods2_volume_t *s)
{
    uint8_t hdr[ODS2_BLOCK_SIZE];
    const uint8_t *bt = dir_block(t, 11, hdr);
    const uint8_t *bs;
    struct scan_ctx a = { 0, 0 }, b = { 0, 0 };

    printf("[3] directory scanners (R5)\n");
    CHECK(bt != NULL, "[TEST] data block");
    if (!bt)
        return;
    ods2_dir_block_scan_typed(bt, tcb_count, &a);
    ods2_dir_block_scan(bt, cb_count, &b);
    CHECK_EQ(a.n, 10, "[TEST]: typed scan sees all 10 entries");
    CHECK_EQ(a.ucs2, 1, "[TEST]: one UCS-2 entry");
    CHECK_EQ(b.n, 9, "[TEST]: 8-bit scan sees the 9 8-bit entries");

    bs = dir_block(s, 11, hdr);
    CHECK(bs != NULL, "[S] data block");
    if (!bs)
        return;
    a.n = a.ucs2 = 0;
    ods2_dir_block_scan_typed(bs, tcb_count, &a);
    CHECK_EQ(a.n, 14, "[S]: 14 entries (13 names, E.txt has ;2 and ;1)");
    CHECK_EQ(a.ucs2, 4, "[S]: four UCS-2 entries");
}

/* ---- 4. name rules ---- */

static void check_dir_rules(const uint8_t *blk, const char *which)
{
    struct rec r[32];
    unsigned n = recs_of(blk, r, 32), i;
    char msg[200];

    for (i = 0; i < n; i++) {
        if (r[i].type != ODS5_NAMETYPE_UCS2) {
            snprintf(msg, sizeof(msg), "%s: name type of '%.*s' (R6)", which,
                     (int)r[i].nc, (const char *)r[i].name);
            CHECK_EQ(ods5_name_type((const char *)r[i].name, r[i].nc), r[i].type, msg);
        }
        if (i > 0) {
            snprintf(msg, sizeof(msg), "%s: record %u sorts after record %u (R7)", which, i, i - 1);
            CHECK(ods5_name_cmp(r[i - 1].name, r[i - 1].nc, r[i - 1].type,
                                r[i].name, r[i].nc, r[i].type) < 0, msg);
        }
    }
}

static void test_rules(const ods2_volume_t *t, const ods2_volume_t *s)
{
    uint8_t hdr[ODS2_BLOCK_SIZE];
    const uint8_t *b;

    printf("[4] name type + order rules (R6/R7)\n");
    if ((b = dir_block(t, 11, hdr)) != NULL)
        check_dir_rules(b, "[TEST]");
    if ((b = dir_block(s, 11, hdr)) != NULL)
        check_dir_rules(b, "[S]");
    CHECK(ods5_name_cmp("E.txt", 5, 0, "e.TXT", 5, 0) == 0, "E.txt == e.TXT case-blind (R8)");
    CHECK(ods5_name_cmp("\xe9.txt", 5, 1, "\xc9.TXT", 5, 1) == 0, "e-acute == E-acute (R8)");
    CHECK_EQ(ods5_fold(0xFF), 0x178, "y-diaeresis folds to U+0178 (R7)");
    CHECK_EQ(ods5_name_type("README.", 7), ODS5_NAMETYPE_ODS2, "README. is ODS-2");
    CHECK_EQ(ods5_name_type("a.b", 3), ODS5_NAMETYPE_ODS2, "a.b is ODS-2");
    CHECK_EQ(ods5_name_type("a.b.c", 5), ODS5_NAMETYPE_ISL1, "a.b.c is ISL-1");
}

/* ---- 5. directory rebuild ---- */

/* Rebuild a real directory block: start from a block holding only its UCS-2
 * records (which the 8-bit insert API cannot create), insert the 8-bit
 * {name, version, FID} entries in FID order (the order the files were made),
 * compare with the real block up to its end-of-records word. */
static int rebuild_equals(const uint8_t *real, unsigned dflags)
{
    struct rec r[32], *order[64];
    unsigned n = recs_of(real, r, 32), i, j, k = 0, cur = 0, nblk = 0;
    uint8_t in[ODS2_BLOCK_SIZE], out[2 * ODS2_BLOCK_SIZE], flat[4 * ODS2_BLOCK_SIZE];
    unsigned real_end = 0;
    struct { const uint8_t *name; unsigned nc; uint16_t ver; uint16_t fid; } ent[64];
    unsigned ne = 0;

    (void)order;
    memset(in, 0xFF, sizeof(in));
    for (i = 0; i < n; i++) {
        unsigned vo = r[i].off + 6 + r[i].nc, nent;
        if (vo & 1) vo++;
        nent = (r[i].off + r[i].len - vo) / 8u;
        if (r[i].type == ODS5_NAMETYPE_UCS2) {
            memcpy(in + cur, real + r[i].off, r[i].len);
            cur += r[i].len;
        } else {
            for (j = 0; j < nent; j++) {
                ent[ne].name = r[i].name;
                ent[ne].nc = r[i].nc;
                ent[ne].ver = rd16(real + vo + 8u * j);
                ent[ne].fid = rd16(real + vo + 8u * j + 2u);
                ne++;
            }
        }
        real_end = r[i].off + r[i].len;
    }
    /* creation order = ascending FID; the case-collision file (E.txt;2, FID
     * 25) was really created as "e.TXT" -- insert it under that spelling, so
     * the rebuild must merge it case-blind into the existing record. */
    for (k = 0; k < ne; k++) {
        unsigned best = 0;
        for (i = 1; i < ne; i++)
            if (ent[i].fid < ent[best].fid)
                best = i;
        {
            ods2_fid_t fid = { ent[best].fid, 1, 0, 0 };
            const char *nm = (const char *)ent[best].name;
            unsigned nc = ent[best].nc;
            if (ent[best].ver == 2 && nc == 5 && memcmp(nm, "E.txt", 5) == 0)
                nm = "e.TXT";
            if (ods2_dir_insert_blocks_ex(in, 1, nm, nc, ent[best].ver, fid, 0,
                                          flat, sizeof(flat), out, 2, &nblk,
                                          dflags) != ODS2_OK || nblk != 1)
                return 0;
            memcpy(in, out, ODS2_BLOCK_SIZE);
        }
        ent[best].fid = 0xFFFF;
    }
    for (i = 0; i < real_end + 2u; i++)
        if (in[i] != real[i]) {
            if (dflags)
                printf("  note: first difference at byte %u: rebuilt %02x, real %02x\n",
                       i, in[i], real[i]);
            return 0;
        }
    return 1;
}

static void test_rebuild(const ods2_volume_t *t, const ods2_volume_t *s)
{
    uint8_t hdr[ODS2_BLOCK_SIZE];
    const uint8_t *b;

    printf("[5] directory rebuild == real block (R5-R8)\n");
    if ((b = dir_block(t, 11, hdr)) != NULL) {
        CHECK(rebuild_equals(b, ODS2_DIRF_ODS5), "[TEST] rebuilt byte-identical in ODS-5 mode");
        CHECK(!rebuild_equals(b, 0), "[TEST] NOT reproduced in ODS-2 mode (negative control)");
    }
    if ((b = dir_block(s, 11, hdr)) != NULL) {
        CHECK(rebuild_equals(b, ODS2_DIRF_ODS5), "[S] rebuilt byte-identical in ODS-5 mode");
        CHECK(!rebuild_equals(b, 0), "[S] NOT reproduced in ODS-2 mode (negative control)");
    }

    /* Remove E.txt;2 by a different-case spelling; ;1 stays. */
    if ((b = dir_block(s, 11, hdr)) != NULL) {
        uint8_t out[ODS2_BLOCK_SIZE], flat[2 * ODS2_BLOCK_SIZE];
        struct rec r[32];
        unsigned nblk = 0, n, i;
        int removed = 0;
        CHECK_EQ(ods2_dir_remove_blocks_ex(b, 1, "e.txt", 5, 2, flat, sizeof(flat),
                                           out, 1, &nblk, &removed, ODS2_DIRF_ODS5),
                 ODS2_OK, "remove e.txt;2 (ODS-5)");
        CHECK(removed, "case-blind remove found E.txt;2");
        n = recs_of(out, r, 32);
        for (i = 0; i < n; i++)
            if (r[i].nc == 5 && memcmp(r[i].name, "E.txt", 5) == 0)
                CHECK_EQ(r[i].ver, 1, "E.txt keeps ;1 after removing ;2");
        removed = 0;
        ods2_dir_remove_blocks_ex(b, 1, "e.txt", 5, 2, flat, sizeof(flat),
                                  out, 1, &nblk, &removed, 0);
        CHECK(!removed, "ODS-2 mode remove is exact-case (negative control)");
    }
}

/* ---- 6. header build + rename ---- */

static void test_build(const ods2_volume_t *t)
{
    uint8_t real[ODS2_BLOCK_SIZE], h[ODS2_BLOCK_SIZE];
    ods2_fid_t back = { 11, 1, 0, 0 };
    ods2_uic_t owner = { 4, 1 };
    ods2_hdr_ident_t id;
    ods2_extent_t ext = { 32, 1 };
    const char *longn = "this_is_a_long_extended_file_name_well_past_the_ods2_limit_of_39.header";

    printf("[6] level-5 header build + rename (R3/R4)\n");
    CHECK_EQ(ods2_volume_read_header(t, 12, real, sizeof(real)), ODS2_OK, "real c++config.h header");
    CHECK_EQ(ods2_fh2_build_ex(h, 12, 1, "c++config.h", 1, 0, ODS2_FK_DATA, &ext, 1, 19,
                               back, owner, 0xFA00, 500, ODS2_STRUCLEV_V5),
             ODS2_OK, "build c++config.h at level 5");
    CHECK_EQ(rd16(h + 6), ODS2_STRUCLEV_V5, "struclev 0x0501");
    CHECK_EQ(h[0], real[0], "idoffset == real");
    CHECK_EQ(h[1], real[1], "mpoffset == real");
    CHECK_EQ(h[2], real[2], "acoffset == real");
    CHECK_EQ(h[3], real[3], "rsoffset == real");
    CHECK(memcmp(h + 80, real + 80, 2) == 0, "FI5 control + namelen == real");
    CHECK(memcmp(h + 80 + 76, real + 80 + 76, 44) == 0, "FI5 name area == real");
    CHECK(memcmp(h + 8, real + 8, 6) == 0, "FID == real");
    CHECK(memcmp(h + 66, real + 66, 6) == 0, "backlink == real");
    CHECK(memcmp(h + 200, real + 200, 4) == 0, "map pointer at mpoffset == real");
    CHECK_EQ(ods2_fh2_ident_get(h, &id), ODS2_OK, "built header ident_get");
    CHECK(id.namelen == 13 && memcmp(id.name, "c++config.h;1", 13) == 0, "built name reads back literally");

    CHECK_EQ(ods2_volume_read_header(t, 18, real, sizeof(real)), ODS2_OK, "real long-name header");
    CHECK_EQ(ods2_fh2_build_ex(h, 18, 1, longn, 1, 0, ODS2_FK_DATA, NULL, 0, 0,
                               back, owner, 0xFA00, 500, ODS2_STRUCLEV_V5),
             ODS2_OK, "build 73-byte name at level 5");
    CHECK_EQ(h[1], real[1], "73-byte name: mpoffset == real (115)");
    CHECK(memcmp(h + 80, real + 80, 2) == 0, "73-byte name: control + namelen == real");
    CHECK(memcmp(h + 80 + 76, real + 80 + 76, 74) == 0, "73-byte name area == real");

    /* rename a mapped file to the long name and back: the map moves with it. */
    CHECK_EQ(ods2_fh2_build_ex(h, 12, 1, "c++config.h", 1, 0, ODS2_FK_DATA, &ext, 1, 19,
                               back, owner, 0xFA00, 500, ODS2_STRUCLEV_V5), ODS2_OK, "rebuild");
    CHECK_EQ(ods2_fh2_rename(h, longn, 1, NULL), ODS2_OK, "rename to the 73-byte name");
    CHECK_EQ(h[1], 115, "mpoffset follows the longer name");
    {
        uint32_t lbn = 0;
        CHECK_EQ(ods2_fh2_map_vbn(h, 1, &lbn), ODS2_OK, "map still decodes");
        CHECK_EQ(lbn, 32, "map moved intact");
    }
    CHECK_EQ(ods2_fh2_rename(h, "libstdc++.a", 1, NULL), ODS2_OK, "rename back to a short name");
    CHECK_EQ(h[1], 100, "mpoffset back to 100");
    CHECK_EQ(ods2_fh2_ident_get(h, &id), ODS2_OK, "ident after rename");
    CHECK(id.namelen == 13 && memcmp(id.name, "libstdc++.a;1", 13) == 0, "renamed name");
    CHECK_EQ(id.nametype, ODS5_NAMETYPE_ISL1, "renamed name type ISL-1");

    /* Level 2 build is unchanged: an 87-byte ident limit, uppercase-agnostic. */
    CHECK_EQ(ods2_fh2_build_ex(h, 12, 1, longn, 1, 0, ODS2_FK_DATA, NULL, 0, 0,
                               back, owner, 0, 500, ODS2_STRUCLEV_V2),
             ODS2_OK, "level-2 build of a 73-char name fits FI2's 86 bytes");
    CHECK_EQ(h[0], 54, "level-2 header keeps idoffset 54");
}

/* ---- 7. ODS-2 fixture via ident_get ---- */

static void test_ods2_fixture(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *img;
    long sz;
    ods2_volume_t vol;
    uint8_t hdr[ODS2_BLOCK_SIZE];
    ods2_hdr_ident_t id;

    printf("[7] ODS-2 (real VAX) header via ident_get\n");
    if (!f) {
        printf("  FAIL: cannot open %s\n", path);
        g_failures++;
        return;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    img = (uint8_t *)malloc((size_t)sz);
    if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(img);
        g_failures++;
        return;
    }
    fclose(f);
    CHECK_EQ(ods2_volume_open(&vol, img, (size_t)sz), ODS2_OK, "VAX volume opens");
    CHECK_EQ(vol.home.hm2_struclev, ODS2_STRUCLEV_V2, "VAX volume is level 2");
    CHECK_EQ(ods2_volume_read_header(&vol, ODS2_FID_INDEXF, hdr, sizeof(hdr)), ODS2_OK, "INDEXF header");
    CHECK_EQ(ods2_fh2_ident_get(hdr, &id), ODS2_OK, "FI2 ident_get");
    CHECK(id.namelen == 12 && memcmp(id.name, "INDEXF.SYS;1", 12) == 0, "INDEXF.SYS;1 from FI2");
    free(img);
}

/* ---- 8. block-backed reader reads it; the ODS-2 writer refuses it ---- */

static void test_bdev(const uint8_t *img, size_t len)
{
    char path[] = "/tmp/ods5_real_XXXXXX";
    int fd = mkstemp(path);
    ods2_bdev_t bv;
    ods2_wvolume_t wv;
    uint8_t hdr[ODS2_BLOCK_SIZE];
    ods2_hdr_ident_t id;

    printf("[8] block-backed reader + writer refusal\n");
    if (fd < 0 || write(fd, img, len) != (ssize_t)len) {
        printf("  FAIL: temp image\n");
        g_failures++;
        if (fd >= 0) { close(fd); unlink(path); }
        return;
    }
    CHECK_EQ(ods2_bdev_open(&bv, fd, len), ODS2_OK, "bdev opens the ODS-5 volume");
    CHECK_EQ(ods2_bdev_read_header(&bv, 18, hdr, sizeof(hdr)), ODS2_OK,
             "bdev reads FID 18 (past INDEXF's first extent)");
    CHECK_EQ(ods2_fh2_ident_get(hdr, &id), ODS2_OK, "FID 18 ident");
    CHECK_EQ(id.namelen, 73, "FID 18 is the 73-byte name");
    CHECK_EQ(ods2_wvolume_open_bdev(fd, len, &wv), ODS2_ERR_FORMAT,
             "the volume writer refuses to append to a layout it did not lay down "
             "(a real VMS-INITIALIZEd volume), fail-honest");
    close(fd);
    unlink(path);
}

int main(void)
{
    const char *dir = getenv("ODS5_FIXTURE_DIR");
    const char *vax = getenv("ODS2_REAL_FIXTURE");
    size_t tl = 0, sl = 0;
    uint8_t *ti, *si;
    ods2_volume_t t, s;

    if (!dir || !*dir || !vax || !*vax) {
        printf("FAIL: ODS5_FIXTURE_DIR / ODS2_REAL_FIXTURE not set\n");
        return 1;
    }
    ti = load(dir, "ods5t.dsk", &tl);
    si = load(dir, "ods5s.dsk", &sl);
    if (!ti || !si)
        return 1;
    CHECK_EQ(ods2_volume_open(&t, ti, tl), ODS2_OK, "ods5t.dsk opens");
    CHECK_EQ(ods2_volume_open(&s, si, sl), ODS2_OK, "ods5s.dsk opens");

    test_home_scb(ti);
    test_ident(&t);
    test_scan(&t, &s);
    test_rules(&t, &s);
    test_rebuild(&t, &s);
    test_build(&t);
    test_ods2_fixture(vax);
    test_bdev(ti, tl);

    free(ti);
    free(si);
    if (g_failures) {
        printf("test_ods5_real: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("test_ods5_real: all checks passed\n");
    return 0;
}
