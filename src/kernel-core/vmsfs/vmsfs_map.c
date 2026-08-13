// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_map.c - ODS-2 retrieval-map math (VBN <-> LBN translation)
 *
 * A file's data is addressed by 1-based virtual block numbers (VBN). Each
 * retrieval pointer (struct vmsfs_retrieval_ptr, vmsfs_ondisk.h) maps a
 * contiguous run of VBNs to volume logical blocks (LBN):
 *   VBN range [cur .. cur + rp_count - 1]  ->  LBN [rp_lbn .. rp_lbn + rp_count - 1]
 *
 * These functions are pure arithmetic over a file's in-core retrieval-pointer
 * array (the host-endian cache the Linux glue populates from fh_map[] in
 * vmsfs_blkdev_iget). They touch no host object -- no struct inode, no
 * super_block, no buffer_head -- and so were promoted verbatim out of
 * vmsfs_blkdev.c onto the substrate-neutral core (rd vms-544, epic vms-8e8),
 * the ODS-2 analog of the executive facilities extracted onto exec_kbackend.h.
 *
 * OVMX Project - Phase 7: Block-device vmsfs
 */

#include "vmsfs_core.h"

/*
 * vmsfs_vbn_to_lbn - translate a 1-based VBN to its LBN.
 *
 * Walks the retrieval-pointer array accumulating each pointer's block count
 * until the run containing @vbn is found. Returns 0 and stores the LBN in
 * *lbn_out; returns -VMSFS_EIO if @vbn falls beyond the mapped range.
 */
int vmsfs_vbn_to_lbn(const struct vmsfs_retrieval_ptr *map, uint16_t map_count,
                     uint32_t vbn, uint32_t *lbn_out)
{
    uint32_t cur_vbn = 1;
    unsigned int i;

    for (i = 0; i < map_count; i++) {
        uint32_t count = map[i].rp_count;

        if (vbn >= cur_vbn && vbn < cur_vbn + count) {
            *lbn_out = map[i].rp_lbn + (vbn - cur_vbn);
            return 0;
        }
        cur_vbn += count;
    }

    return -VMSFS_EIO;  /* VBN not mapped */
}

/*
 * vmsfs_next_vbn - the next unallocated VBN (sum of all mapped VBNs + 1).
 */
uint32_t vmsfs_next_vbn(const struct vmsfs_retrieval_ptr *map,
                        uint16_t map_count)
{
    uint32_t vbn = 1;
    unsigned int i;

    for (i = 0; i < map_count; i++)
        vbn += map[i].rp_count;
    return vbn;
}

/*
 * vmsfs_extend_map - add one block (@lbn) to a file's retrieval map.
 *
 * Tries a contiguous extension of the last pointer first (the common case for
 * sequentially allocated files); otherwise appends a new pointer slot. Updates
 * *map_count. Returns 0, or -VMSFS_ENOSPC when the pointer array is full.
 */
int vmsfs_extend_map(struct vmsfs_retrieval_ptr *map, uint16_t *map_count,
                     uint32_t lbn)
{
    /* Try contiguous extension of last pointer */
    if (*map_count > 0) {
        struct vmsfs_retrieval_ptr *last = &map[*map_count - 1];

        if (lbn == last->rp_lbn + last->rp_count) {
            last->rp_count++;
            return 0;
        }
    }

    /* Need a new pointer slot */
    if (*map_count >= VMSFS_MAX_RETRIEVAL)
        return -VMSFS_ENOSPC;

    map[*map_count].rp_lbn = lbn;
    map[*map_count].rp_count = 1;
    (*map_count)++;
    return 0;
}
