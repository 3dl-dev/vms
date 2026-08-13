// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_alloc.c - substrate-neutral ODS-2 storage / FID allocator
 * (rd vms-d69, epic vms-8e8).
 *
 * The storage-bitmap block allocator, the file-header (FID) allocator, the
 * home-block free-count write-back and the per-file block-map growth, promoted
 * (behaviour-preserving) from vmsfs_blkdev.c onto the block/inode seam
 * (README-backend.md §3a). They reach the volume through struct vmsfs_volume
 * and the block bitmap through the vmsfs_bit_* / vmsfs_b* ops -- NO struct inode,
 * NO host superblock, 0 <linux/...> includes.
 *
 * The volume free-count and bitmap are mutated in place; the caller (backend)
 * holds the volume's allocation lock across these, exactly as the former Linux
 * glue held sbi->alloc_lock.
 */

#include "vmsfs_bio.h"

/*
 * Write a single bitmap block back to disk after modifying the in-memory
 * bitmap. bit_pos determines which bitmap block to write.
 */
static int vmsfs_write_bitmap_block(struct vmsfs_volume *vol, uint32_t bit_pos)
{
	uint32_t block_idx = bit_pos / (VMSFS_BLOCK_SIZE * 8);
	uint32_t lbn = vol->bitmap_lbn + block_idx;
	struct vmsfs_bh *bh;

	if (block_idx >= vol->bitmap_blocks)
		return -VMSFS_EINVAL;

	bh = vmsfs_bget(vol->host, lbn);
	if (!bh)
		return -VMSFS_EIO;

	memcpy(vmsfs_bdata(bh),
	       (char *)vol->bitmap + block_idx * VMSFS_BLOCK_SIZE,
	       VMSFS_BLOCK_SIZE);

	vmsfs_bdirty_sync(bh);
	vmsfs_bput(bh);
	return 0;
}

int vmsfs_update_home_block(struct vmsfs_volume *vol)
{
	struct vmsfs_bh *bh;
	struct vmsfs_home_block *hb;

	bh = vmsfs_bget(vol->host, VMSFS_HOME_LBN);
	if (!bh)
		return -VMSFS_EIO;

	hb = vmsfs_bdata(bh);
	hb->hb_free_blocks = vmsfs_cpu_to_le32(vol->free_blocks);
	hb->hb_modified = vmsfs_cpu_to_le64(vmsfs_now_seconds());

	hb->hb_checksum = 0;
	hb->hb_checksum = vmsfs_cpu_to_le32(vmsfs_checksum(hb, sizeof(*hb)));

	vmsfs_bdirty_sync(bh);
	vmsfs_bput(bh);
	return 0;
}

int vmsfs_alloc_block(struct vmsfs_volume *vol, uint32_t *lbn_out)
{
	uint32_t bit;
	int ret;

	if (vol->free_blocks == 0)
		return -VMSFS_ENOSPC;

	bit = vmsfs_bit_find_zero(vol->bitmap, vol->total_blocks, vol->data_lbn);
	if (bit >= vol->total_blocks)
		return -VMSFS_ENOSPC;

	vmsfs_bit_set(vol->bitmap, bit);
	vol->free_blocks--;

	ret = vmsfs_write_bitmap_block(vol, bit);
	if (ret) {
		vmsfs_bit_clear(vol->bitmap, bit);
		vol->free_blocks++;
		return ret;
	}

	*lbn_out = bit;
	return 0;
}

int vmsfs_free_block(struct vmsfs_volume *vol, uint32_t lbn)
{
	int ret;

	if (lbn >= vol->total_blocks)
		return -VMSFS_EINVAL;

	if (!vmsfs_bit_test(vol->bitmap, lbn))
		return 0;  /* already free */

	vmsfs_bit_clear(vol->bitmap, lbn);
	vol->free_blocks++;

	ret = vmsfs_write_bitmap_block(vol, lbn);
	if (ret) {
		vmsfs_bit_set(vol->bitmap, lbn);
		vol->free_blocks--;
		return ret;
	}

	return 0;
}

void vmsfs_free_file_blocks(struct vmsfs_volume *vol,
			    const struct vmsfs_retrieval_ptr *map,
			    uint16_t map_count)
{
	unsigned int i;
	uint32_t j;

	for (i = 0; i < map_count; i++) {
		for (j = 0; j < map[i].rp_count; j++)
			vmsfs_free_block(vol, map[i].rp_lbn + j);
	}
}

int vmsfs_alloc_fid(struct vmsfs_volume *vol, uint32_t *fid_out)
{
	uint32_t fid;

	for (fid = VMSFS_FID_FIRST_USER; fid <= vol->max_files; fid++) {
		struct vmsfs_bh *bh;
		struct vmsfs_file_header *fh;
		uint32_t lbn = vol->index_lbn + fid - 1;

		bh = vmsfs_bget(vol->host, lbn);
		if (!bh)
			continue;

		fh = vmsfs_bdata(bh);

		if (vmsfs_le32_to_cpu(fh->fh_magic) != VMSFS_FH_MAGIC ||
		    !(vmsfs_le16_to_cpu(fh->fh_flags) & VMSFS_FH_INUSE)) {
			vmsfs_bput(bh);
			*fid_out = fid;
			return 0;
		}

		vmsfs_bput(bh);
	}

	return -VMSFS_ENOSPC;
}

int vmsfs_free_fid(struct vmsfs_volume *vol, uint32_t fid)
{
	struct vmsfs_bh *bh;
	uint32_t lbn;

	if (fid < VMSFS_FID_FIRST_USER || fid > vol->max_files)
		return -VMSFS_EINVAL;

	lbn = vol->index_lbn + fid - 1;
	bh = vmsfs_bget(vol->host, lbn);
	if (!bh)
		return -VMSFS_EIO;

	memset(vmsfs_bdata(bh), 0, VMSFS_BLOCK_SIZE);
	vmsfs_bdirty_sync(bh);
	vmsfs_bput(bh);
	return 0;
}

int vmsfs_grow_map(struct vmsfs_volume *vol, struct vmsfs_retrieval_ptr *map,
		   uint16_t *map_count, uint32_t target_vbn, int *blocks_added)
{
	uint32_t next;
	int ret;

	*blocks_added = 0;
	next = vmsfs_next_vbn(map, *map_count);

	while (next <= target_vbn) {
		uint32_t lbn;
		struct vmsfs_bh *bh;

		ret = vmsfs_alloc_block(vol, &lbn);
		if (ret)
			return ret;

		ret = vmsfs_extend_map(map, map_count, lbn);
		if (ret) {
			vmsfs_free_block(vol, lbn);
			return ret;
		}

		/* Zero newly allocated block. */
		bh = vmsfs_bget(vol->host, lbn);
		if (bh) {
			memset(vmsfs_bdata(bh), 0, VMSFS_BLOCK_SIZE);
			vmsfs_bdirty(bh);
			vmsfs_bput(bh);
		}

		(*blocks_added)++;
		next++;
	}

	return 0;
}
