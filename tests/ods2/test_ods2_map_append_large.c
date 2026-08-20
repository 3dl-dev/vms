/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_map_append_large.c - CODEC-LEVEL proof (vms-3a8) that
 * ods2_fh2_map_append() records a contiguous run LONGER THAN 256 blocks as
 * several consecutive FM2 format-1 retrieval pointers, instead of rejecting it.
 *
 * WHY THIS EXISTS. The executive Files-11 ACP's IO$_WRITEVBLK extend path
 * (vms_ioctl_acp_writevb, src/kernel-core/vmsfs_acp.c) allocates a whole
 * implicit extend as ONE contiguous run -- up to a 1 MiB / 2048-block $PUT
 * chunk -- and hands it to ods2_fh2_map_append() to record in the file's FH2
 * retrieval map. A single FM2 format-1 pointer's count field is 8 bits (1..256
 * blocks), so map_append() USED TO reject any run > 256 blocks with
 * ODS2_ERR_ARGS -- which the ACP surfaced as SS$_DEVICEFULL. The result: PRODUCT
 * INSTALL's WRITE of the first kit image larger than 128 KB (AUTHORIZE.EXE)
 * failed with %PCSI-E-WRITE, cascading into every downstream install/boot step.
 *
 * A large contiguous allocation is legally stored in ODS-2 as back-to-back
 * format-1 pointers whose LBNs abut; the reader (ods2_fh2_map_walk) and the
 * ACP's channel-window builder read them back and coalesce the abutting runs, so
 * the file remains one VBN->LBN window turn. This test drives map_append()
 * DIRECTLY (no volume, no fd -- a header built by ods2_fh2_build) and proves,
 * through the reader's own ods2_fh2_map_walk primitive:
 *
 *   1. A run of 600 blocks (> 256) is ACCEPTED and reconstructs to exactly the
 *      600-block contiguous span it was given (the pre-fix ODS2_ERR_ARGS bug).
 *   2. It is stored as MORE THAN ONE pointer, each <= 256 blocks (a genuine
 *      format-1 split, not one out-of-range pointer), yet the walk coalesces
 *      them back into a single contiguous span.
 *   3. An abutting follow-on run coalesces into the last pointer up to the
 *      256-block ceiling and then extends (the vms-401 grow-in-place invariant
 *      still holds across the split boundary).
 *   4. A run whose pointers would overflow the fixed 255-word FH2 map area is an
 *      honest ODS2_ERR_NOSPACE -- never a silent partial map (INV-6).
 *   5. count == 0 is a no-op success; a run past the format-1 LBN ceiling
 *      (>= 2^22) is a fail-honest ODS2_ERR_ARGS.
 */

#include "vmsfs/ods2.h"

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

/* Walk state: collect every retrieval pointer the reader decodes. */
struct walk_state {
    unsigned n;
    uint32_t total_blocks;
    uint32_t max_ptr_count;   /* largest single decoded pointer */
    uint32_t first_lbn;
    int      contiguous;      /* 1 while each pointer abuts the previous */
    uint32_t next_lbn;        /* expected LBN of the next pointer if contiguous */
};

static int walk_cb(const ods2_extent_t *ext, void *ctx)
{
    struct walk_state *w = (struct walk_state *)ctx;
    if (!ext || ext->count == 0)
        return 0;
    if (w->n == 0)
        w->first_lbn = ext->lbn;
    else if (ext->lbn != w->next_lbn)
        w->contiguous = 0;
    if (ext->count > w->max_ptr_count)
        w->max_ptr_count = ext->count;
    w->total_blocks += ext->count;
    w->next_lbn = ext->lbn + ext->count;
    w->n++;
    return 0;
}

static void walk(const uint8_t *hdr, struct walk_state *w)
{
    memset(w, 0, sizeof(*w));
    w->contiguous = 1;
    CHECK_EQ(ods2_fh2_map_walk(hdr, walk_cb, w, NULL), ODS2_OK, "map_walk parses the header");
}

int main(void)
{
    uint8_t hdr[ODS2_BLOCK_SIZE];
    ods2_uic_t owner = { 4, 1 };     /* [1,4] SYSTEM */
    ods2_fid_t backlink = { 4, 4, 0, 0 };
    ods2_status_t st;
    struct walk_state w;

    printf("=== ODS-2 codec: ods2_fh2_map_append large-run split (vms-3a8) ===\n");

    /* A file header with NO initial allocation -- map area empty. */
    memset(hdr, 0, sizeof(hdr));
    st = ods2_fh2_build(hdr, /*fidnum*/100, /*seq*/1, "BIGFILE", /*version*/1,
                        /*filechar*/0, ODS2_FK_DATA_FIX,
                        /*extents*/NULL, /*n_extents*/0, /*data_len*/0,
                        backlink, owner, /*fileprot*/0, /*maxfiles*/200);
    CHECK_EQ(st, ODS2_OK, "ods2_fh2_build a fresh RFM=FIXED header, empty map");

    /* (1) + (2): a 600-block run (> 256) is accepted and split into format-1
     * pointers, reconstructing to exactly the 600-block contiguous span. */
    st = ods2_fh2_map_append(hdr, /*lbn*/1000, /*count*/600);
    CHECK_EQ(st, ODS2_OK, "map_append accepts a 600-block run (pre-fix: ODS2_ERR_ARGS)");
    ods2_fh2_reseal(hdr);

    walk(hdr, &w);
    CHECK_EQ(w.total_blocks, 600, "the 600-block run reconstructs to 600 blocks");
    CHECK_EQ(w.first_lbn, 1000, "the run still starts at LBN 1000");
    CHECK(w.contiguous, "the split pointers reconstruct ONE contiguous span (LBNs abut)");
    CHECK(w.n >= 3, "the run is stored as >= 3 pointers (256+256+88), not one");
    CHECK(w.max_ptr_count <= 256, "no single pointer exceeds the format-1 256-block ceiling");

    /* (3): an abutting follow-on run coalesces into the last (<256) pointer,
     * then extends -- the vms-401 grow-in-place invariant across the split. */
    st = ods2_fh2_map_append(hdr, /*lbn*/1600, /*count*/100);   /* 1000+600 == 1600 */
    CHECK_EQ(st, ODS2_OK, "map_append accepts an abutting 100-block follow-on");
    ods2_fh2_reseal(hdr);

    walk(hdr, &w);
    CHECK_EQ(w.total_blocks, 700, "after the abutting append the span is 700 blocks");
    CHECK_EQ(w.first_lbn, 1000, "the coalesced span still starts at LBN 1000");
    CHECK(w.contiguous, "the whole 700-block span is still ONE contiguous run");

    /* (5): fail-honest edges. count 0 is a no-op success; a run past the
     * format-1 LBN ceiling (>= 2^22) is ODS2_ERR_ARGS. */
    st = ods2_fh2_map_append(hdr, 5000, 0);
    CHECK_EQ(st, ODS2_OK, "map_append(count=0) is a no-op success");
    st = ods2_fh2_map_append(hdr, (1u << 22) - 10u, 100);   /* end >= 2^22 */
    CHECK_EQ(st, ODS2_ERR_ARGS, "a run past the format-1 LBN ceiling is ODS2_ERR_ARGS");

    /* (4): a run whose pointers cannot fit the remaining 255-word map area is an
     * honest ODS2_ERR_NOSPACE. Build a fresh header and fill the map with
     * NON-abutting 256-block runs (each forces a new pointer) until it refuses;
     * the map must fill and never silently drop a pointer. */
    memset(hdr, 0, sizeof(hdr));
    st = ods2_fh2_build(hdr, 101, 1, "FULLMAP", 1, 0, ODS2_FK_DATA_FIX,
                        NULL, 0, 0, backlink, owner, 0, 200);
    CHECK_EQ(st, ODS2_OK, "ods2_fh2_build a second fresh header");
    {
        int nospace = 0;
        uint32_t lbn = 100;
        for (int i = 0; i < 400; i++) {
            /* +512 gap between runs so no coalesce -- each is its own pointer. */
            st = ods2_fh2_map_append(hdr, lbn, 200);
            if (st == ODS2_ERR_NOSPACE) { nospace = 1; break; }
            CHECK_EQ(st, ODS2_OK, "map_append fills the map area one pointer at a time");
            if (g_failures) break;
            lbn += 200 + 512;
        }
        CHECK(nospace, "a full FH2 map area refuses further pointers with ODS2_ERR_NOSPACE (INV-6)");
        ods2_fh2_reseal(hdr);
        walk(hdr, &w);
        CHECK(w.n > 0 && w.total_blocks == w.n * 200u,
              "the filled map still parses clean -- every recorded pointer is intact");
    }

    printf("=== test_ods2_map_append_large: %d failure(s) ===\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
