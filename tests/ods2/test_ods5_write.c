/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods5_write.c - the volume WRITER (vmsfs_master's engine) lays down an
 * ODS-5 volume the way INITIALIZE/STRUCTURE=5 + file creation do on a real
 * OpenVMS Alpha V8.4 system (rd vms-7e7e).
 *
 * Oracle: tests/lab/captures/ods5-alpha-20261022/ods5t.dsk (RESULTS.md there).
 * The test recreates that volume's [TEST] directory with the writer -- the same
 * names, created in the same order, so they get the same file numbers -- and
 * compares the result with the real volume:
 *   - home block and SCB at level 5, reserved files still level-2 headers;
 *   - every [TEST] directory record (name, name type, version limit, version,
 *     FID, pad byte) equal to the real record, in the real order;
 *   - each file header's level, ident offset, map offset, FI5 name type,
 *     length and name bytes equal to the real header's;
 *   - a case-collision insert (LIBSTDC++.A) merges into libstdc++.a.
 * Negative control: the same tree written at level 2 does NOT match the real
 * directory (byte order, no name types). ODS-2 output is unchanged: the rest
 * of tests/ods2 is the regression.
 */

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

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

#define NBLK 2000u
#define LONGNAME "this_is_a_long_extended_file_name_well_past_the_ods2_limit_of_39.header"

/* The real files of [TEST], in creation order (FID 12..21); the UCS-2 file
 * (FID 17) cannot be named in 8 bits -- a placeholder file takes its number
 * so the later files get the real FIDs, and its record is left out. */
static const char *const g_names[] = {
    "c++config.h", "libstdc++.a", "MixedCase.Txt", "a.b.c.d", "caf\xe9.txt",
    NULL, LONGNAME, "PLAIN.TXT", "a b.txt", "README." };

static uint8_t *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long sz;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc((size_t)sz);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (size_t)sz;
    return b;
}

static const uint8_t *dirblk(const ods2_volume_t *v, uint32_t fid, uint8_t *hdr)
{
    uint32_t lbn;
    if (ods2_volume_read_header(v, fid, hdr, 512) != ODS2_OK ||
        ods2_fh2_map_vbn(hdr, 1, &lbn) != ODS2_OK)
        return NULL;
    return ods2_volume_block(v, lbn);
}

/* Records of a directory block, the UCS-2 ones dropped. */
struct rec { const uint8_t *p; unsigned len; };
static unsigned recs8(const uint8_t *blk, struct rec *r, unsigned max)
{
    unsigned off = 0, n = 0;
    while (off + 6 <= 512 && n < max && rd16(blk + off) != 0xFFFF) {
        unsigned len = 2u + rd16(blk + off);
        if (ODS5_DIR_NAMETYPE(blk[off + 4]) != ODS5_NAMETYPE_UCS2) {
            r[n].p = blk + off;
            r[n].len = len;
            n++;
        }
        off += len;
    }
    return n;
}

/* Build the [TEST] tree at `level`; returns the image (caller frees). */
static uint8_t *build(uint16_t level, ods2_fid_t *testdir)
{
    uint8_t *img = (uint8_t *)calloc(NBLK, 512);
    ods2_format_params_t p;
    ods2_wvolume_t w;
    ods2_fid_t fid;
    unsigned i;
    static const uint8_t data[] = "#define CXXCONFIG 1\n";

    memset(&p, 0, sizeof(p));
    p.total_blocks = NBLK;
    p.maxfiles = 500;
    p.volname = "ODS5T";
    if (!img || ods2_volume_format_ex(img, (size_t)NBLK * 512, &p, &w, level) != ODS2_OK)
        return NULL;
    if (ods2_wvolume_create_dir(&w, "TEST.DIR", 1, w.mfd_fid, testdir) != ODS2_OK ||
        ods2_wvolume_dir_insert(&w, w.mfd_fid, "TEST.DIR", 1, *testdir) != ODS2_OK)
        return NULL;
    for (i = 0; i < sizeof(g_names) / sizeof(g_names[0]); i++) {
        const char *nm = g_names[i] ? g_names[i] : "UCS2PLACEHOLDER.TMP";
        if (ods2_wvolume_create_file_stmlf(&w, nm, 1, data, i == 0 ? sizeof(data) - 1 : 0,
                                           *testdir, &fid) != ODS2_OK)
            return NULL;
        if (g_names[i] && ods2_wvolume_dir_insert(&w, *testdir, nm, 1, fid) != ODS2_OK)
            return NULL;
    }
    return img;
}

int main(void)
{
    const char *dir = getenv("ODS5_FIXTURE_DIR");
    char path[1024];
    size_t rlen = 0;
    uint8_t *real, *mine, *two;
    ods2_volume_t rv, mv, tv;
    ods2_fid_t td, td2;
    uint8_t rh[512], mh[512];
    const uint8_t *rb, *mb, *tb;
    struct rec rr[32], mr[32], tr[32];
    unsigned nr, nm, nt, i;
    uint32_t f;

    if (!dir) {
        printf("FAIL: ODS5_FIXTURE_DIR not set\n");
        return 1;
    }
    snprintf(path, sizeof(path), "%s/ods5t.dsk", dir);
    real = load(path, &rlen);
    mine = build(ODS2_STRUCLEV_V5, &td);
    two = build(ODS2_STRUCLEV_V2, &td2);
    CHECK(real && mine && two, "fixture loads; writer builds the tree at level 5 and level 2");
    if (!real || !mine || !two)
        return 1;
    CHECK(ods2_volume_open(&rv, real, rlen) == ODS2_OK, "real volume opens");
    CHECK(ods2_volume_open(&mv, mine, (size_t)NBLK * 512) == ODS2_OK, "written level-5 volume opens");
    CHECK(ods2_volume_open(&tv, two, (size_t)NBLK * 512) == ODS2_OK, "written level-2 volume opens");

    printf("[1] volume structure\n");
    CHECK(mv.home.hm2_struclev == ODS2_STRUCLEV_V5, "home block level 5 (R1)");
    {
        ods2_scb_t scb;
        uint32_t lbn;
        CHECK(ods2_volume_read_header(&mv, ODS2_FID_BITMAP, mh, 512) == ODS2_OK &&
              ods2_fh2_map_vbn(mh, 1, &lbn) == ODS2_OK &&
              ods2_scb_parse(ods2_volume_block(&mv, lbn), 512, &scb) == ODS2_OK &&
              scb.scb_struclev == ODS2_STRUCLEV_V5, "SCB level 5 (R1)");
    }
    for (f = 1; f <= ODS2_RESFILES; f++)
        CHECK(ods2_volume_read_header(&mv, f, mh, 512) == ODS2_OK &&
              rd16(mh + 6) == ODS2_STRUCLEV_V2, "reserved files keep level-2 headers (R2)");
    CHECK(td.fid_num == 11, "[TEST] gets FID 11, as on the real volume");
    CHECK(ods2_volume_read_header(&mv, 11, mh, 512) == ODS2_OK && rd16(mh + 6) == ODS2_STRUCLEV_V5,
          "the mastered directory has a level-5 header (real TEST.DIR is 0x0501)");

    printf("[2] [TEST] directory records == real\n");
    rb = dirblk(&rv, 11, rh);
    mb = dirblk(&mv, 11, mh);
    CHECK(rb && mb, "both [TEST] data blocks");
    if (rb && mb) {
        nr = recs8(rb, rr, 32);
        nm = recs8(mb, mr, 32);
        CHECK(nr == 9 && nm == 9, "nine 8-bit records on each");
        for (i = 0; i < nr && i < nm; i++) {
            char msg[200];
            snprintf(msg, sizeof(msg), "record %u ('%.*s') byte-identical to the real record",
                     i, (int)rr[i].p[5], (const char *)rr[i].p + 6);
            CHECK(rr[i].len == mr[i].len && memcmp(rr[i].p, mr[i].p, rr[i].len) == 0, msg);
        }
    }

    printf("[3] file headers == real (layout + FI5 name)\n");
    for (f = 12; f <= 21; f++) {
        char msg[160];
        if (f == 17)
            continue;
        if (ods2_volume_read_header(&rv, f, rh, 512) != ODS2_OK ||
            ods2_volume_read_header(&mv, f, mh, 512) != ODS2_OK) {
            CHECK(0, "headers read");
            continue;
        }
        snprintf(msg, sizeof(msg), "FID %u: level, idoffset, mpoffset, acoffset == real", f);
        CHECK(rd16(mh + 6) == rd16(rh + 6) && mh[0] == rh[0] && mh[1] == rh[1] && mh[2] == rh[2], msg);
        snprintf(msg, sizeof(msg), "FID %u: FI5 type, length and name == real", f);
        CHECK(memcmp(mh + 80, rh + 80, 2) == 0 &&
              memcmp(mh + 80 + 76, rh + 80 + 76, (size_t)rh[1] * 2u - 80u - 76u) == 0, msg);
        snprintf(msg, sizeof(msg), "FID %u: backlink == real (11,1,0)", f);
        CHECK(memcmp(mh + 66, rh + 66, 6) == 0, msg);
    }

    printf("[4] negative control: level-2 output does not match\n");
    tb = dirblk(&tv, 11, mh);
    if (tb && rb) {
        int same = 1;
        nt = recs8(tb, tr, 32);
        nr = recs8(rb, rr, 32);
        same = nt == nr;
        for (i = 0; same && i < nt; i++)
            same = tr[i].len == rr[i].len && memcmp(tr[i].p, rr[i].p, tr[i].len) == 0;
        CHECK(!same, "the level-2 [TEST] differs from the real ODS-5 directory");
    }

    printf("[5] case-collision insert merges\n");
    {
        ods2_wvolume_t w;
        ods2_format_params_t p;
        ods2_fid_t d, f1, f2;
        struct rec r[8];
        uint8_t *img = (uint8_t *)calloc(NBLK, 512);
        memset(&p, 0, sizeof(p));
        p.total_blocks = NBLK; p.maxfiles = 100; p.volname = "M";
        CHECK(img && ods2_volume_format_ex(img, (size_t)NBLK * 512, &p, &w, ODS2_STRUCLEV_V5) == ODS2_OK &&
              ods2_wvolume_create_dir(&w, "D.DIR", 1, w.mfd_fid, &d) == ODS2_OK &&
              ods2_wvolume_dir_insert(&w, w.mfd_fid, "D.DIR", 1, d) == ODS2_OK &&
              ods2_wvolume_create_file_stmlf(&w, "libstdc++.a", 1, NULL, 0, d, &f1) == ODS2_OK &&
              ods2_wvolume_dir_insert(&w, d, "libstdc++.a", 1, f1) == ODS2_OK &&
              ods2_wvolume_create_file_stmlf(&w, "LIBSTDC++.A", 2, NULL, 0, d, &f2) == ODS2_OK &&
              ods2_wvolume_dir_insert(&w, d, "LIBSTDC++.A", 2, f2) == ODS2_OK,
              "two cases of one name inserted");
        if (img) {
            ods2_volume_t v;
            const uint8_t *b;
            CHECK(ods2_volume_open(&v, img, (size_t)NBLK * 512) == ODS2_OK, "reopen");
            b = dirblk(&v, d.fid_num, mh);
            CHECK(b && recs8(b, r, 8) == 1 && r[0].p[5] == 11 &&
                  memcmp(r[0].p + 6, "libstdc++.a", 11) == 0 && r[0].len == 34u,
                  "one record, first case kept, two versions (R8)");
            free(img);
        }
    }

    free(real);
    free(mine);
    free(two);
    if (g_failures) {
        printf("test_ods5_write: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("test_ods5_write: all checks passed\n");
    return 0;
}
