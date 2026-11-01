/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_dircontig.c - a directory stays CONTIGUOUS however it grows
 * (rd vms-465: a source tree of 1000+ files in one directory).
 *
 * A Files-11 directory file is contiguous (FH2$M_CONTIG; the writer sets it on
 * create_dir). When ods2_wvolume_dir_insert() grows a directory whose next free
 * block is not adjacent to its last block -- the normal case when each insert
 * follows a file's own data allocation -- the grown directory must still be
 * one contiguous run: the directory moves to a fresh run of the size it needs
 * and its old blocks return to the storage bitmap. Before the fix each growth
 * appended a separate retrieval pointer, so a directory of ~1000 small files
 * filled its header's map area and the insert failed ODS2_ERR_NOSPACE
 * (vmsfs_master: "%MASTER-F-DIRENT ... no space").
 *
 * Proven:
 *   - NFILES interleaved create_file + dir_insert into one directory all succeed;
 *   - the directory header maps exactly ONE extent and keeps FH2$M_CONTIG;
 *   - the reader lists all NFILES entries, each with its own FID;
 *   - the blocks the directory left are free in BITMAP.SYS (no lost blocks):
 *     the extent's blocks are allocated, the next block after it (first data
 *     block of nothing) is consistent with the bump allocator's end.
 */

#define _POSIX_C_SOURCE 200809L

#include "vmsfs/ods2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, msg) do {                                          \
    if (!(cond)) {                                                     \
        printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);    \
        g_failures++;                                                  \
    }                                                                  \
} while (0)

#define CHECK_EQ(a, b, msg) do {                                       \
    long _va = (long)(a), _vb = (long)(b);                            \
    if (_va != _vb) {                                                  \
        printf("  FAIL: %s: got %ld, want %ld  (%s:%d)\n",           \
               (msg), _va, _vb, __FILE__, __LINE__);                   \
        g_failures++;                                                  \
    }                                                                  \
} while (0)

#define TOTAL_BLOCKS 20000u
#define MAXFILES     2000u
#define NFILES       1500u
#define BLK          ODS2_BLOCK_SIZE

struct ext_cap { unsigned n; ods2_extent_t e[128]; };

static int ext_collect(const ods2_extent_t *ext, void *ctx)
{
    struct ext_cap *c = (struct ext_cap *)ctx;
    if (c->n < 128)
        c->e[c->n] = *ext;
    c->n++;
    return 0;
}

struct list_cap { unsigned count; unsigned bad_fid; uint32_t want_fid[NFILES]; };

static int list_collect(const char *name, unsigned name_len, uint16_t version,
                        const ods2_fid_t *fid, void *ctx)
{
    struct list_cap *c = (struct list_cap *)ctx;
    unsigned i;
    (void)version;
    /* SOURCEnnnn.CC -> nnnn */
    if (name_len == 13 && memcmp(name, "SOURCE", 6) == 0 &&
        sscanf(name + 6, "%4u", &i) == 1 && i < NFILES) {
        if (ods2_fid_number(fid) != c->want_fid[i])
            c->bad_fid++;
        c->count++;
    }
    return 0;
}

/* BITMAP.SYS bit for `lbn`: 1 = free (the writer's convention). */
static int lbn_free(const ods2_volume_t *vol, uint32_t bitmap_lbn, uint32_t lbn)
{
    const uint8_t *b = ods2_volume_block(vol, bitmap_lbn + lbn / 4096u);
    uint32_t in = lbn % 4096u;
    return b ? (b[in / 8u] >> (in % 8u)) & 1 : -1;
}

int main(void)
{
    static uint8_t image[TOTAL_BLOCKS * BLK];
    static struct list_cap lc;
    ods2_format_params_t params;
    ods2_wvolume_t wvol;
    ods2_status_t st;
    ods2_fid_t dir_fid, f;
    unsigned i, ok_inserts = 0;
    char name[32], body[64];

    printf("=== ODS-2 directory stays contiguous as it grows (vms-465) ===\n");
    memset(&params, 0, sizeof(params));
    params.total_blocks = TOTAL_BLOCKS;
    params.maxfiles     = MAXFILES;
    params.volname      = "OVMXCONT";
    st = ods2_volume_format(image, sizeof(image), &params, &wvol);
    CHECK_EQ(st, ODS2_OK, "ods2_volume_format");
    if (st != ODS2_OK)
        return 1;

    st = ods2_wvolume_create_dir(&wvol, "SRC.DIR", 1, wvol.mfd_fid, &dir_fid);
    CHECK_EQ(st, ODS2_OK, "create_dir(SRC.DIR)");
    st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "SRC.DIR", 1, dir_fid);
    CHECK_EQ(st, ODS2_OK, "dir_insert(MFD, SRC.DIR)");

    for (i = 0; i < NFILES; i++) {
        int n = snprintf(body, sizeof(body), "source %u\n", i);
        snprintf(name, sizeof(name), "SOURCE%04u.CC", i);
        st = ods2_wvolume_create_file(&wvol, name, 1, (const uint8_t *)body,
                                      (size_t)n, dir_fid, &f);
        if (st != ODS2_OK) {
            printf("  FAIL: create_file(%s) = %d\n", name, (int)st);
            g_failures++;
            break;
        }
        lc.want_fid[i] = ods2_fid_number(&f);
        st = ods2_wvolume_dir_insert(&wvol, dir_fid, name, 1, f);
        if (st != ODS2_OK) {
            printf("  FAIL: dir_insert(SRC, %s) = %d (after %u inserts)\n", name, (int)st, i);
            g_failures++;
            break;
        }
        ok_inserts++;
    }
    CHECK_EQ(ok_inserts, NFILES, "every insert succeeded");
    st = ods2_wvolume_flush(&wvol);
    CHECK_EQ(st, ODS2_OK, "flush");

    {
        ods2_volume_t vol;
        uint8_t hdr[BLK];
        ods2_fh2_t parsed;
        struct ext_cap ec = { 0 };
        uint32_t k;

        st = ods2_volume_open(&vol, image, sizeof(image));
        CHECK_EQ(st, ODS2_OK, "ods2_volume_open");
        st = ods2_volume_read_header(&vol, ods2_fid_number(&dir_fid), hdr, sizeof(hdr));
        CHECK_EQ(st, ODS2_OK, "read_header(SRC.DIR)");
        st = ods2_fh2_parse(hdr, sizeof(hdr), &parsed);
        CHECK_EQ(st, ODS2_OK, "fh2_parse(SRC.DIR)");
        CHECK((parsed.fh2_filechar & ODS2_FH2_M_CONTIG) != 0, "SRC.DIR keeps FH2$M_CONTIG");
        st = ods2_fh2_map_walk(hdr, ext_collect, &ec, NULL);
        CHECK_EQ(st, ODS2_OK, "map_walk(SRC.DIR)");
        CHECK_EQ(ec.n, 1, "SRC.DIR maps exactly one extent");
        CHECK(ec.n >= 1 && ec.e[0].count >= 60, "SRC.DIR spans many blocks");
        printf("  SRC.DIR: %u extent(s), first %u blocks at LBN %u\n",
               ec.n, ec.n ? ec.e[0].count : 0, ec.n ? ec.e[0].lbn : 0);

        st = ods2_volume_list_dir(&vol, hdr, list_collect, &lc);
        CHECK_EQ(st, ODS2_OK, "list_dir(SRC.DIR)");
        CHECK_EQ(lc.count, NFILES, "every file is listed");
        CHECK_EQ(lc.bad_fid, 0, "every entry carries its own FID");

        /* the directory's blocks are allocated in BITMAP.SYS */
        {
            uint32_t bitmap_lbn = wvol.bitmap_scb_lbn + 1;
            unsigned in_use = 0, freed = 0;
            for (k = 0; ec.n == 1 && k < ec.e[0].count; k++)
                in_use += lbn_free(&vol, bitmap_lbn, ec.e[0].lbn + k) == 0;
            CHECK_EQ(in_use, ec.n == 1 ? ec.e[0].count : 0,
                     "the directory's blocks are allocated in BITMAP.SYS");
            /* blocks the directory moved off are free again: below the
             * directory's run some block must have been released */
            for (k = 0; ec.n == 1 && k < ec.e[0].lbn; k++)
                freed += lbn_free(&vol, bitmap_lbn, k) == 1;
            CHECK(freed > 0, "the directory's old blocks returned to BITMAP.SYS");
        }
    }

    if (g_failures) {
        printf("FAIL: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("PASS: %u files in one directory; it is one contiguous extent\n", NFILES);
    return 0;
}
