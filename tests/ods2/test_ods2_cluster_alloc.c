/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_cluster_alloc.c - the storage-bitmap allocator works in CLUSTERS
 * (rd vms-563): on a volume with cluster factor 3, bit N of BITMAP.SYS is
 * LBNs 3N..3N+2, and a file's data goes to the blocks the bitmap marks.
 *
 * THE DEFECT. The executive ACP took bitmap bit N for LBN N. In the vms-5fa
 * fire (tests/lab/captures/vms-5fa-shared-volume-r2-20261102) OVMXE extended
 * EVAC.DAT on VAX1's $2$DUA2 (INITIALIZEd with cluster size 3): it cleared bit
 * 29706 in BITMAP.SYS (MSCP frame 4687, LBN 445547) and then wrote the file's
 * data to LBN 29706 (frame 4693) -- a block of some other part of the volume
 * -- while LBNs 89118-89120, the cluster it had allocated, were mapped by
 * nothing. VAX1's ANALYZE/DISK_STRUCTURE: "ALLOCCLR, blocks incorrectly marked
 * allocated, LBN 89118 to 89120" (vax1.log).
 *
 * The ACP (src/kernel-core/vmsfs_acp.c) now does its BITMAP.SYS work through
 * the codec's ods2_sbm_alloc / ods2_sbm_free_range, which this test drives
 * with the same kind of block-I/O callbacks the ACP supplies:
 *
 *   1. The incident, by its own numbers: clusters 0..29705 in use, a 1-block
 *      extend gets cluster 29706 = LBN 89118, 3 blocks; the file's map points
 *      there; nothing lands at LBN 29706.
 *   2. On a cluster-factor-3 ODS-2 image (writer format + the
 *      ods2_cluster_fixture.h conversion), extend a real file the way
 *      IO$_WRITEVBLK does: the new extent starts on a cluster boundary and is
 *      whole clusters, the data block the map resolves to lies inside it, and
 *      the bitmap and the map agree -- exactly the new clusters went from free
 *      to allocated and every block the file maps sits in an allocated cluster.
 *   3. Freeing the extent gives back exactly those clusters; a range that
 *      starts inside a cluster leaves that cluster allocated (a truncate never
 *      frees a cluster still holding data).
 *   4. Cluster factor 1 is unchanged (bit N == LBN N); a full volume is an
 *      honest ODS2_ERR_NOSPACE.
 */

#include "vmsfs/ods2.h"
#include "ods2_cluster_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_failures = 0;

#define CHECK(cond, msg) do {                                          \
    if (!(cond)) {                                                     \
        printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);    \
        g_failures++;                                                  \
    } else {                                                          \
        printf("  PASS: %s\n", (msg));                                 \
    }                                                                 \
} while (0)

#define CHECK_EQ(a, b, msg) do {                                       \
    long _va = (long)(a), _vb = (long)(b);                             \
    if (_va != _vb) {                                                  \
        printf("  FAIL: %s: got %ld, want %ld  (%s:%d)\n",             \
               (msg), _va, _vb, __FILE__, __LINE__);                   \
        g_failures++;                                                  \
    } else {                                                          \
        printf("  PASS: %s (%ld)\n", (msg), _va);                      \
    }                                                                 \
} while (0)

/* ---- a bitmap held in memory (part 1, 4) ---------------------------- */

struct mem_bm {
    uint8_t *bits;          /* BITMAP.SYS VBN2.. */
    uint32_t nblk;          /* bit blocks */
    unsigned reads, writes;
};

static int mem_read(void *ctx, uint32_t bmvbn, void *blk)
{
    struct mem_bm *m = ctx;
    if (bmvbn < 2 || bmvbn - 2u >= m->nblk)
        return 1;
    memcpy(blk, m->bits + (size_t)(bmvbn - 2u) * ODS2_BLOCK_SIZE, ODS2_BLOCK_SIZE);
    m->reads++;
    return 0;
}

static int mem_write(void *ctx, uint32_t bmvbn, const void *blk)
{
    struct mem_bm *m = ctx;
    if (bmvbn < 2 || bmvbn - 2u >= m->nblk)
        return -1;
    memcpy(m->bits + (size_t)(bmvbn - 2u) * ODS2_BLOCK_SIZE, blk, ODS2_BLOCK_SIZE);
    m->writes++;
    return 0;
}

static int bit_free(const uint8_t *bits, uint32_t bit)
{
    return (bits[bit / 8u] >> (bit % 8u)) & 1u;
}

static void set_bit(uint8_t *bits, uint32_t bit, int free)
{
    if (free)
        bits[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
    else
        bits[bit / 8u] &= (uint8_t)~(1u << (bit % 8u));
}

/* ---- the image's own BITMAP.SYS (part 2, 3) -------------------------- */

struct img_bm {
    uint8_t *image;
    uint8_t  bmhdr[ODS2_BLOCK_SIZE];   /* BITMAP.SYS (FID 2) header */
};

static int img_read(void *ctx, uint32_t bmvbn, void *blk)
{
    struct img_bm *m = ctx;
    uint32_t lbn;
    if (ods2_fh2_map_vbn(m->bmhdr, bmvbn, &lbn) != ODS2_OK)
        return 1;
    memcpy(blk, m->image + (size_t)lbn * ODS2_BLOCK_SIZE, ODS2_BLOCK_SIZE);
    return 0;
}

static int img_write(void *ctx, uint32_t bmvbn, const void *blk)
{
    struct img_bm *m = ctx;
    uint32_t lbn;
    if (ods2_fh2_map_vbn(m->bmhdr, bmvbn, &lbn) != ODS2_OK)
        return -1;
    memcpy(m->image + (size_t)lbn * ODS2_BLOCK_SIZE, blk, ODS2_BLOCK_SIZE);
    return 0;
}

struct ext_list {
    unsigned n;
    uint32_t lbn[64], count[64];
};

static int ext_cb(const ods2_extent_t *e, void *ctx)
{
    struct ext_list *l = ctx;
    if (l->n < 64) {
        l->lbn[l->n] = e->lbn;
        l->count[l->n] = e->count;
        l->n++;
    }
    return 0;
}

/* -------------------------------------------------------------------- */

static void part1_incident(void)
{
    const uint32_t c = 3, nclusters = 40000, volsize = nclusters * c;
    struct mem_bm m;
    ods2_sbm_io_t io;
    uint8_t blk[ODS2_BLOCK_SIZE], hdr[ODS2_BLOCK_SIZE];
    uint32_t lbn = 0, count = 0, k, v, mapped;
    ods2_fid_t fid;
    ods2_uic_t owner;
    ods2_status_t st;

    printf("--- 1. the vms-5fa incident: cluster 29706 is LBN 89118-89120 ---\n");
    m.nblk = (nclusters + ODS2_SBM_BITS_PER_BLOCK - 1u) / ODS2_SBM_BITS_PER_BLOCK;
    m.bits = calloc(m.nblk, ODS2_BLOCK_SIZE);
    m.reads = m.writes = 0;
    for (k = 29706; k < nclusters; k++)
        set_bit(m.bits, k, 1);                  /* clusters 0..29705 in use */
    io.ctx = &m; io.volsize = volsize; io.cluster = c;
    io.read = mem_read; io.write = mem_write;

    st = ods2_sbm_alloc(&io, blk, 1, 0, 1, &lbn, &count);
    CHECK_EQ(st, ODS2_OK, "a 1-block extend is allocated");
    CHECK_EQ(lbn, 89118, "the run starts at LBN 89118 (cluster 29706 * 3), not LBN 29706");
    CHECK_EQ(count, 3, "a whole cluster (3 blocks) is allocated");
    CHECK(!bit_free(m.bits, 29706), "bit 29706 is now ALLOCATED");
    CHECK(bit_free(m.bits, 29707), "bit 29707 is still free (one cluster taken, no more)");

    memset(&fid, 0, sizeof(fid));
    memset(&owner, 0, sizeof(owner));
    fid.fid_num = 4; fid.fid_seq = 4;          /* backlink: the MFD */
    memset(hdr, 0, sizeof(hdr));
    st = ods2_fh2_build(hdr, 12, 1, "EVAC.DAT", 1, 0, ODS2_FK_DATA_FIX,
                        NULL, 0, 0, fid, owner, 0, 200);
    CHECK_EQ(st, ODS2_OK, "a file header for EVAC.DAT is built");
    CHECK_EQ(ods2_fh2_map_append(hdr, lbn, count), ODS2_OK,
             "the allocated run is appended to its map");
    for (v = 1; v <= 3; v++) {
        mapped = 0;
        st = ods2_fh2_map_vbn(hdr, v, &mapped);
        CHECK(st == ODS2_OK && mapped >= 89118 && mapped <= 89120 && mapped != 29706,
              "VBN 1..3 map inside LBN 89118-89120 (where the bitmap says), never LBN 29706");
    }
    free(m.bits);
}

static void part2_3_image(void)
{
    const uint32_t c = 3, total = 3000;
    size_t len = (size_t)total * ODS2_BLOCK_SIZE;
    uint8_t *image = calloc(1, len), *before, *after;
    ods2_format_params_t params;
    ods2_wvolume_t wvol;
    ods2_fid_t fid;
    struct img_bm m;
    ods2_sbm_io_t io;
    ods2_home_t home;
    ods2_scb_t scb;
    struct ext_list el;
    uint8_t hdr[ODS2_BLOCK_SIZE], blk[ODS2_BLOCK_SIZE], data[600];
    uint32_t hdr_lbn, bmsize, hint, lbn = 0, count = 0, old_hiblk, data_lbn = 0;
    uint32_t k, e, changed = 0, nbits;
    int agree = 1;
    ods2_status_t st;

    printf("--- 2. extend a file on a cluster-factor-3 ODS-2 image ---\n");
    params.total_blocks = total;
    params.maxfiles = 32;
    params.volname = "CLUSTER3";
    st = ods2_volume_format(image, len, &params, &wvol);
    CHECK_EQ(st, ODS2_OK, "ods2_volume_format");
    memset(data, 'D', sizeof(data));
    st = ods2_wvolume_create_file_raw(&wvol, "EVAC.DAT", 1, data, sizeof(data),
                                      wvol.mfd_fid, &fid);
    CHECK_EQ(st, ODS2_OK, "EVAC.DAT (2 blocks) written");
    st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "EVAC.DAT", 1, fid);
    CHECK_EQ(st, ODS2_OK, "EVAC.DAT entered in [000000]");
    CHECK_EQ(ods2_fixture_recluster(image, len, &wvol, c), 0, "volume converted to cluster factor 3");

    st = ods2_home_parse(image + ODS2_BLOCK_SIZE, ODS2_BLOCK_SIZE, &home, 1);
    CHECK(st == ODS2_OK && home.hm2_cluster == 3, "the home block parses and says cluster 3");
    st = ods2_scb_parse(image + (size_t)wvol.bitmap_scb_lbn * ODS2_BLOCK_SIZE,
                        ODS2_BLOCK_SIZE, &scb);
    CHECK(st == ODS2_OK && scb.scb_cluster == 3 && scb.scb_volsize == total,
          "the SCB parses and says cluster 3, 3000 blocks");

    m.image = image;
    memcpy(m.bmhdr, image + (size_t)(wvol.hdr_base_lbn + ODS2_FID_BITMAP - 1u) * ODS2_BLOCK_SIZE,
           ODS2_BLOCK_SIZE);
    io.ctx = &m; io.volsize = scb.scb_volsize; io.cluster = scb.scb_cluster;
    io.read = img_read; io.write = img_write;
    nbits = io.volsize / c;
    bmsize = wvol.bitmap_data_blocks * ODS2_BLOCK_SIZE;
    before = malloc(bmsize);
    after = malloc(bmsize);
    memcpy(before, image + (size_t)(wvol.bitmap_scb_lbn + 1u) * ODS2_BLOCK_SIZE, bmsize);

    /* The file's header, its last extent's end as the hint (IO$_WRITEVBLK). */
    hdr_lbn = wvol.hdr_base_lbn + ods2_fid_number(&fid) - 1u;
    memcpy(hdr, image + (size_t)hdr_lbn * ODS2_BLOCK_SIZE, ODS2_BLOCK_SIZE);
    memset(&el, 0, sizeof(el));
    CHECK_EQ(ods2_fh2_map_walk(hdr, ext_cb, &el, NULL), ODS2_OK, "EVAC.DAT's map walks");
    old_hiblk = 0;
    for (e = 0; e < el.n; e++)
        old_hiblk += el.count[e];
    hint = el.n ? el.lbn[el.n - 1] + el.count[el.n - 1] : 0;

    st = ods2_sbm_alloc(&io, blk, 1, hint, 1 + old_hiblk, &lbn, &count);
    CHECK_EQ(st, ODS2_OK, "a 1-block extend is allocated from BITMAP.SYS");
    CHECK_EQ(lbn % c, 0, "the run starts on a cluster boundary");
    CHECK_EQ(count, 3, "the run is one whole cluster");
    CHECK(lbn + count <= total, "the run lies inside the volume");
    CHECK_EQ(ods2_fh2_map_append(hdr, lbn, count), ODS2_OK, "the run is appended to the map");
    st = ods2_fh2_map_vbn(hdr, old_hiblk + 1u, &data_lbn);
    CHECK(st == ODS2_OK && data_lbn >= lbn && data_lbn < lbn + count,
          "the extended VBN resolves to an LBN inside the allocated run");

    /* Bitmap vs map: exactly the run's clusters went free -> allocated. */
    memcpy(after, image + (size_t)(wvol.bitmap_scb_lbn + 1u) * ODS2_BLOCK_SIZE, bmsize);
    for (k = 0; k < nbits; k++) {
        if (bit_free(before, k) != bit_free(after, k)) {
            changed++;
            if (!(bit_free(before, k) && k >= lbn / c && k < (lbn + count) / c))
                agree = 0;
        }
    }
    CHECK(agree && changed == count / c,
          "exactly the run's clusters changed in BITMAP.SYS, free -> allocated");
    memset(&el, 0, sizeof(el));
    (void)ods2_fh2_map_walk(hdr, ext_cb, &el, NULL);
    for (e = 0; e < el.n; e++)
        for (k = el.lbn[e]; k < el.lbn[e] + el.count[e]; k++)
            if (bit_free(after, k / c))
                agree = 0;
    CHECK(agree, "every block EVAC.DAT maps lies in a cluster the bitmap marks allocated");

    printf("--- 3. deallocation gives back whole clusters only ---\n");
    CHECK_EQ(ods2_sbm_free_range(&io, blk, lbn + 1u, count - 1u), ODS2_OK,
             "free a range starting inside the cluster");
    memcpy(after, image + (size_t)(wvol.bitmap_scb_lbn + 1u) * ODS2_BLOCK_SIZE, bmsize);
    CHECK(!bit_free(after, lbn / c), "the partly covered cluster stays allocated");
    CHECK_EQ(ods2_sbm_free_range(&io, blk, lbn, count), ODS2_OK, "free the whole run");
    memcpy(after, image + (size_t)(wvol.bitmap_scb_lbn + 1u) * ODS2_BLOCK_SIZE, bmsize);
    CHECK(memcmp(before, after, bmsize) == 0,
          "BITMAP.SYS is back to exactly its state before the extend");

    free(before);
    free(after);
    free(image);
}

static void part4_edges(void)
{
    struct mem_bm m;
    ods2_sbm_io_t io;
    uint8_t blk[ODS2_BLOCK_SIZE];
    uint32_t lbn = 0, count = 0, k;
    ods2_status_t st;

    printf("--- 4. cluster factor 1 unchanged; a full volume is NOSPACE ---\n");
    m.nblk = 1;
    m.bits = calloc(1, ODS2_BLOCK_SIZE);
    for (k = 100; k < 1000; k++)
        set_bit(m.bits, k, 1);
    io.ctx = &m; io.volsize = 1000; io.cluster = 1;
    io.read = mem_read; io.write = mem_write;
    st = ods2_sbm_alloc(&io, blk, 5, 0, 5, &lbn, &count);
    CHECK(st == ODS2_OK && lbn == 100 && count == 5,
          "cluster factor 1: bit N is LBN N, exactly `need` blocks");
    CHECK_EQ(ods2_sbm_round_blocks(4, 3), 6, "4 blocks round up to 2 clusters of 3");
    CHECK_EQ(ods2_sbm_round_blocks(4, 1), 4, "cluster factor 1 rounds nothing");

    memset(m.bits, 0, ODS2_BLOCK_SIZE);
    io.cluster = 3;
    st = ods2_sbm_alloc(&io, blk, 1, 0, 1, &lbn, &count);
    CHECK_EQ(st, ODS2_ERR_NOSPACE, "no free cluster: ODS2_ERR_NOSPACE, never a run");
    free(m.bits);
}

int main(void)
{
    printf("=== test_ods2_cluster_alloc: storage-bitmap allocation in clusters (vms-563) ===\n");
    part1_incident();
    part2_3_image();
    part4_edges();
    printf("=== test_ods2_cluster_alloc: %s (%d failure%s) ===\n",
           g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
