/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ods2_cluster_fixture.h - turn a volume the ODS-2 writer just formatted
 * (cluster factor 1) into a volume with cluster factor `c`, for tests of the
 * cluster-aware storage-bitmap allocator (rd vms-563).
 *
 * The writer only formats cluster factor 1 (INITIALIZE /CLUSTER_SIZE is not
 * implemented there). A VAX initializes data disks with larger clusters --
 * VAX1's $2$DUA2 in the vms-5fa fire had cluster size 3 -- and the bug this
 * fixture exists for only shows on such a volume. What this changes, all of it
 * on-disk ODS-2 fact already carried by vmsfs/ods2.h:
 *
 *   - hm2_cluster in both home blocks (LBN 1 and 2), both checksums resealed;
 *   - scb_cluster in BITMAP.SYS's SCB (VBN1), checksum resealed;
 *   - the storage bitmap rewritten so bit k covers LBNs k*c .. k*c+c-1: the
 *     cluster is FREE only when all of its blocks were free, so every block
 *     already in use stays inside an allocated cluster. Bits at and past
 *     volsize / c are ALLOCATED (no such cluster).
 *
 * It is a TEST FIXTURE, not an INITIALIZE: the reserved files and any files
 * written before the conversion keep their cluster-factor-1 extents (not
 * cluster aligned). Allocation and deallocation through the codec on the
 * result follow the cluster rules; that is what the tests check.
 *
 * Shared by tests/ods2/test_ods2_cluster_alloc.c (host) and
 * tests/qemu/mkimage_ods2_cluster3.c (the QEMU fixture for
 * test_syssvc_acp_cluster3).
 */
#ifndef OVMX_TESTS_ODS2_CLUSTER_FIXTURE_H
#define OVMX_TESTS_ODS2_CLUSTER_FIXTURE_H

#include "vmsfs/ods2.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline void ods2fx_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

/* 1 if bit `bit` of the storage bitmap starting at `bm` is set (FREE). */
static inline int ods2fx_bit(const uint8_t *bm, uint32_t bit)
{
    return (bm[bit / 8u] >> (bit % 8u)) & 1u;
}

/* Rewrite the formatted image to cluster factor `c`. 0 on success. */
static inline int ods2_fixture_recluster(uint8_t *image, size_t image_len,
                                         const ods2_wvolume_t *wvol, uint32_t c)
{
    uint32_t nblocks = wvol->nblocks, nbits, k, j, lbn;
    size_t bm_bytes = (size_t)wvol->bitmap_data_blocks * ODS2_BLOCK_SIZE;
    uint8_t *bm, *old;
    uint8_t *scb;

    if (c < 2 || c > 0xFFFFu || (size_t)nblocks * ODS2_BLOCK_SIZE > image_len)
        return -1;
    bm = image + (size_t)(wvol->bitmap_scb_lbn + 1u) * ODS2_BLOCK_SIZE;
    old = malloc(bm_bytes);
    if (!old)
        return -1;
    memcpy(old, bm, bm_bytes);
    memset(bm, 0, bm_bytes);                 /* every bit ALLOCATED ... */
    nbits = nblocks / c;
    for (k = 0; k < nbits; k++) {            /* ... but the wholly free clusters */
        int free = 1;
        for (j = 0; j < c && free; j++)
            free = ods2fx_bit(old, k * c + j);
        if (free)
            bm[k / 8u] |= (uint8_t)(1u << (k % 8u));
    }
    free(old);

    for (lbn = 1; lbn <= 2; lbn++) {         /* primary + alternate home block */
        uint8_t *hb = image + (size_t)lbn * ODS2_BLOCK_SIZE;
        ods2fx_put16(hb + offsetof(ods2_home_t, hm2_cluster), (uint16_t)c);
        ods2fx_put16(hb + offsetof(ods2_home_t, hm2_checksum1), ods2_checksum(hb, 29));
        ods2fx_put16(hb + offsetof(ods2_home_t, hm2_checksum2), ods2_block_checksum(hb));
    }
    scb = image + (size_t)wvol->bitmap_scb_lbn * ODS2_BLOCK_SIZE;
    ods2fx_put16(scb + offsetof(ods2_scb_t, scb_cluster), (uint16_t)c);
    ods2fx_put16(scb + offsetof(ods2_scb_t, scb_checksum), ods2_block_checksum(scb));
    return 0;
}

#endif /* OVMX_TESTS_ODS2_CLUSTER_FIXTURE_H */
