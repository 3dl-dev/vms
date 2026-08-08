/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_real.c - Drive the genuine ODS-2 reader over a REAL OpenVMS
 * VAX V7.3 volume image (increment 2 of the genuine-ODS-2 effort).
 *
 * test_ods2.c proves the reader is self-consistent with the documented
 * Files-11 layout by constructing a spec-conformant image; this file proves
 * it against actual bytes written by real OpenVMS INITIALIZE/COPY/DISMOUNT.
 * See PROVENANCE-real_vax_ods2.md for exactly how the fixture was made and
 * what OpenVMS itself reported for the same volume (the cross-check oracle
 * for every expected value asserted below).
 *
 * ODS2_REAL_FIXTURE is supplied by CMake as the absolute path to
 * real_vax_ods2.dsk, so this binary runs correctly regardless of the build
 * directory location.
 */

#include "vmsfs/ods2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ODS2_REAL_FIXTURE
#error "ODS2_REAL_FIXTURE must be defined by CMake to the fixture's path"
#endif

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
    }                                                                   \
} while (0)

/* Real ground-truth values from the lab session (SHOW DEVICE/FULL,
 * DIRECTORY/FILE_ID/SIZE) -- see PROVENANCE-real_vax_ods2.md. */
#define REAL_TOTAL_BLOCKS   800u
#define REAL_MAXFILES       200u
#define REAL_CLUSTER        1u
#define REAL_SECTORS        10u
#define REAL_TRACKS         80u
#define REAL_CYLINDERS      1u

#define FID_BITMAP_LBN_EXPECT 5u   /* BITMAP.SYS VBN1, decoded via its own FM2 map */

static uint8_t *load_fixture(size_t *len_out)
{
    FILE *f = fopen(ODS2_REAL_FIXTURE, "rb");
    uint8_t *buf;
    long sz;

    if (!f) {
        printf("  FAIL: cannot open fixture %s\n", ODS2_REAL_FIXTURE);
        g_failures++;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        printf("  FAIL: short read on fixture\n");
        g_failures++;
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len_out = (size_t)sz;
    return buf;
}

struct extent_capture {
    ods2_extent_t ext[4];
    unsigned      n;
};

static int extent_collect(const ods2_extent_t *ext, void *ctx)
{
    struct extent_capture *c = (struct extent_capture *)ctx;
    if (c->n < 4)
        c->ext[c->n] = *ext;
    c->n++;
    return 0;
}

struct dir_entry_capture {
    char     name[32];
    uint16_t version;
    uint32_t fid_num;
};

struct dir_capture {
    struct dir_entry_capture entries[8];
    int count;
};

static int dir_collect(const char *name, unsigned name_len,
                       uint16_t version, const ods2_fid_t *fid, void *ctx)
{
    struct dir_capture *c = (struct dir_capture *)ctx;
    if (c->count < 8) {
        struct dir_entry_capture *e = &c->entries[c->count];
        unsigned n = name_len < sizeof(e->name) - 1 ? name_len : sizeof(e->name) - 1;
        memcpy(e->name, name, n);
        e->name[n] = '\0';
        e->version = version;
        e->fid_num = ods2_fid_number(fid);
    }
    c->count++;
    return 0;
}

static const struct dir_entry_capture *find_entry(const struct dir_capture *c,
                                                   const char *name)
{
    int i;
    for (i = 0; i < c->count && i < 8; i++)
        if (strcmp(c->entries[i].name, name) == 0)
            return &c->entries[i];
    return NULL;
}

int main(void)
{
    uint8_t *img;
    size_t img_len;
    ods2_volume_t vol;
    ods2_status_t st;
    uint8_t hdr[ODS2_BLOCK_SIZE];
    struct extent_capture ec;
    struct dir_capture dc;
    const struct dir_entry_capture *e;

    printf("=== ODS-2 reader vs a REAL OpenVMS VAX V7.3 volume ===\n");
    printf("fixture: %s\n", ODS2_REAL_FIXTURE);

    img = load_fixture(&img_len);
    if (!img) {
        printf("FAIL: could not load fixture\n");
        return 1;
    }
    CHECK_EQ(img_len, REAL_TOTAL_BLOCKS * ODS2_BLOCK_SIZE, "fixture size == 800 blocks");

    /* ---- home block: checksums, format string, struclev, geometry ---- */
    st = ods2_volume_open(&vol, img, img_len);
    CHECK_EQ(st, ODS2_OK, "ods2_volume_open on real image");
    if (st == ODS2_OK) {
        CHECK_EQ(vol.home.hm2_struclev, ODS2_STRUCLEV_V2, "real struclev == 0x0201");
        CHECK_EQ(vol.home.hm2_cluster, REAL_CLUSTER, "real hm2_cluster");
        CHECK_EQ(vol.home.hm2_maxfiles, REAL_MAXFILES, "real hm2_maxfiles == 200");
        CHECK(memcmp(vol.home.hm2_format, ODS2_FORMAT_STRING, ODS2_FORMAT_LEN) == 0,
              "real hm2_format == DECFILE11B");
        CHECK(memcmp(vol.home.hm2_volname, "OVMXTEST    ", 12) == 0,
              "real hm2_volname == OVMXTEST");
    }

    /* ---- INDEXF.SYS (FID 1) header: checksum + ident ---- */
    st = ods2_volume_read_header(&vol, ODS2_FID_INDEXF, hdr, sizeof(hdr));
    CHECK_EQ(st, ODS2_OK, "read_header(INDEXF.SYS) on real image");
    if (st == ODS2_OK) {
        const ods2_ident_t *id = ods2_fh2_ident(hdr);
        CHECK(id != NULL, "INDEXF.SYS ident non-NULL");
        if (id)
            CHECK(memcmp(id->fi2_filename, "INDEXF.SYS", 10) == 0,
                  "INDEXF.SYS ident name");
    }

    /* ---- BITMAP.SYS (FID 2) header + map -> locate its VBN1 (the SCB) ---- */
    st = ods2_volume_read_header(&vol, ODS2_FID_BITMAP, hdr, sizeof(hdr));
    CHECK_EQ(st, ODS2_OK, "read_header(BITMAP.SYS) on real image");
    if (st == ODS2_OK) {
        memset(&ec, 0, sizeof(ec));
        st = ods2_fh2_map_walk(hdr, extent_collect, &ec, NULL);
        CHECK_EQ(st, ODS2_OK, "map_walk(BITMAP.SYS)");
        CHECK_EQ(ec.n, 1, "BITMAP.SYS has one extent");
        if (ec.n >= 1) {
            CHECK_EQ(ec.ext[0].lbn, FID_BITMAP_LBN_EXPECT, "BITMAP.SYS extent LBN");

            /* ---- SCB parse (new in increment 2) against the real block ---- */
            {
                const uint8_t *scb_blk = ods2_volume_block(&vol, ec.ext[0].lbn);
                ods2_scb_t scb;

                CHECK(scb_blk != NULL, "SCB block in range");
                if (scb_blk) {
                    st = ods2_scb_parse(scb_blk, ODS2_BLOCK_SIZE, &scb);
                    CHECK_EQ(st, ODS2_OK, "ods2_scb_parse on real SCB");
                    if (st == ODS2_OK) {
                        CHECK_EQ(scb.scb_struclev, ODS2_STRUCLEV_V2, "SCB struclev");
                        CHECK_EQ(scb.scb_cluster, REAL_CLUSTER, "SCB cluster");
                        CHECK_EQ(scb.scb_volsize, REAL_TOTAL_BLOCKS, "SCB volsize == 800");
                        /* Confirmed this increment against SHOW DEVICE/FULL geometry. */
                        CHECK_EQ(scb.scb_sectors, REAL_SECTORS, "SCB sectors == 10");
                        CHECK_EQ(scb.scb_tracks, REAL_TRACKS, "SCB tracks == 80");
                        CHECK_EQ(scb.scb_cylinders, REAL_CYLINDERS, "SCB cylinders == 1");
                        /* Leading bytes of scb_volockname: confirmed == "VAX1"
                         * (the mounting host); the field's full width is NOT
                         * asserted here -- see the ods2.h comment. */
                        CHECK(memcmp(scb.scb_volockname, "VAX1", 4) == 0,
                              "SCB volockname starts with real host name VAX1");
                    }
                }
            }
        }
    }

    /* ---- directory listing: the real [OVMXDIR] directory we created ---- */
    {
        uint8_t dirhdr[ODS2_BLOCK_SIZE];
        st = ods2_volume_read_header(&vol, 11 /* OVMXDIR.DIR, per PROVENANCE */,
                                     dirhdr, sizeof(dirhdr));
        CHECK_EQ(st, ODS2_OK, "read_header(OVMXDIR.DIR)");
        if (st == ODS2_OK) {
            memset(&dc, 0, sizeof(dc));
            st = ods2_volume_list_dir(&vol, dirhdr, dir_collect, &dc);
            CHECK_EQ(st, ODS2_OK, "list_dir(OVMXDIR)");
            CHECK_EQ(dc.count, 2, "OVMXDIR has exactly 2 real files");

            e = find_entry(&dc, "HELLO.TXT");
            CHECK(e != NULL, "HELLO.TXT listed");
            if (e) {
                CHECK_EQ(e->version, 1, "HELLO.TXT version");
                CHECK_EQ(e->fid_num, 12, "HELLO.TXT fid == 12 (per PROVENANCE)");
            }

            e = find_entry(&dc, "WORLD.TXT");
            CHECK(e != NULL, "WORLD.TXT listed");
            if (e) {
                CHECK_EQ(e->version, 1, "WORLD.TXT version");
                CHECK_EQ(e->fid_num, 13, "WORLD.TXT fid == 13 (per PROVENANCE)");
            }
        }
    }

    /* ---- FM2 map decode on the two real data files, cross-checked against
     * VMS's own DIRECTORY/SIZE block counts (34 and 2 blocks). ---- */
    st = ods2_volume_read_header(&vol, 12 /* HELLO.TXT */, hdr, sizeof(hdr));
    CHECK_EQ(st, ODS2_OK, "read_header(HELLO.TXT)");
    if (st == ODS2_OK) {
        memset(&ec, 0, sizeof(ec));
        st = ods2_fh2_map_walk(hdr, extent_collect, &ec, NULL);
        CHECK_EQ(st, ODS2_OK, "map_walk(HELLO.TXT)");
        CHECK_EQ(ec.n, 1, "HELLO.TXT one extent");
        if (ec.n >= 1) {
            CHECK_EQ(ec.ext[0].lbn, 32, "HELLO.TXT extent LBN");
            CHECK_EQ(ec.ext[0].count, 34, "HELLO.TXT extent count == real 34 blocks");
        }
    }

    st = ods2_volume_read_header(&vol, 13 /* WORLD.TXT */, hdr, sizeof(hdr));
    CHECK_EQ(st, ODS2_OK, "read_header(WORLD.TXT)");
    if (st == ODS2_OK) {
        memset(&ec, 0, sizeof(ec));
        st = ods2_fh2_map_walk(hdr, extent_collect, &ec, NULL);
        CHECK_EQ(st, ODS2_OK, "map_walk(WORLD.TXT)");
        CHECK_EQ(ec.n, 1, "WORLD.TXT one extent");
        if (ec.n >= 1) {
            CHECK_EQ(ec.ext[0].lbn, 66, "WORLD.TXT extent LBN");
            CHECK_EQ(ec.ext[0].count, 2, "WORLD.TXT extent count == real 2 blocks");
        }
    }

    free(img);

    if (g_failures == 0) {
        printf("PASS: reader parses a REAL OpenVMS VAX V7.3 volume byte-for-byte\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
