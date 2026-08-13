// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_dirscan.c - substrate-neutral ODS-2 directory-block scanner
 * (rd vms-d69, epic vms-8e8).
 *
 * The directory lookup / version-resolution / entry add+remove / emptiness
 * scanner, promoted (behaviour-preserving) from vmsfs_blkdev.c onto the
 * block/inode seam (README-backend.md §3b). These walk a directory's data
 * blocks via its in-core retrieval map + vmsfs_bget and make every ODS-2
 * decision (version resolution, case-blind name match, free-slot placement).
 *
 * What stays in the backend: struct-inode mutation (a directory that grew a
 * block returns *blocks_added so the backend bumps i_size/i_blocks and flushes
 * its header) and the readdir dir_emit emission (the backend owns that loop and
 * calls the two per-entry helpers here for the ODS-2 decode + display format).
 *
 * 0 <linux/...> includes: block access + endian come through the vmsfs_bio /
 * vmsfs_backend vocabulary shim; version/map/name logic is the seam-#1 core.
 */

#include "vmsfs_bio.h"

int vmsfs_dir_resolve(struct vmsfs_volume *vol,
		      const struct vmsfs_retrieval_ptr *map, uint16_t map_count,
		      int case_blind, const char *name, uint32_t *fid_out)
{
	char base[VMSFS_MAX_FILENAME + 1];
	int req_version;
	int ret;
	uint32_t vbn;
	uint32_t best_fid = 0;
	uint16_t best_version = 0;

	*fid_out = 0;

	/* Parse requested name for version. */
	ret = vmsfs_parse_version(name, base, sizeof(base), &req_version);
	if (ret)
		return ret;

	/* Scan all data blocks of this directory. */
	for (vbn = 1; ; vbn++) {
		uint32_t lbn;
		struct vmsfs_bh *bh;
		char *blk;
		unsigned int j;

		ret = vmsfs_vbn_to_lbn(map, map_count, vbn, &lbn);
		if (ret)
			break;  /* no more mapped blocks */

		bh = vmsfs_bget(vol->host, lbn);
		if (!bh)
			break;
		blk = vmsfs_bdata(bh);

		for (j = 0; j < VMSFS_DIR_PER_BLOCK; j++) {
			struct vmsfs_dir_entry *de;
			unsigned int off = j * VMSFS_DIR_ENTRY_SIZE;
			uint32_t de_fid;
			uint16_t de_ver;
			char de_base[VMSFS_MAX_FILENAME + 1];
			int de_parsed_ver;

			if (off + VMSFS_DIR_ENTRY_SIZE > VMSFS_BLOCK_SIZE)
				break;

			de = (struct vmsfs_dir_entry *)(blk + off);
			de_fid = vmsfs_le32_to_cpu(de->de_fid);

			if (de_fid == 0)
				continue;  /* free slot */

			de_ver = vmsfs_le16_to_cpu(de->de_version);

			/*
			 * de_name contains "NAME.TYPE". Parse out the base for
			 * comparison. For directories, de_name has no semicolon
			 * or version.
			 */
			ret = vmsfs_parse_version(de->de_name, de_base,
						  sizeof(de_base), &de_parsed_ver);
			if (ret) {
				/* If parse fails, try direct comparison. */
				if (vmsfs_name_match(de->de_name, de->de_name_len,
						     base, strlen(base),
						     case_blind)) {
					best_fid = de_fid;
					best_version = de_ver;
					if (req_version != 0) {
						vmsfs_bput(bh);
						goto found;
					}
				}
				continue;
			}

			/* Compare base names. */
			if (!vmsfs_name_match(de_base, strlen(de_base),
					      base, strlen(base), case_blind))
				continue;

			/* Version matching. */
			if (req_version != 0) {
				/* Exact version requested. */
				if (de_ver == req_version) {
					best_fid = de_fid;
					best_version = de_ver;
					vmsfs_bput(bh);
					goto found;
				}
			} else {
				/* Find highest version. */
				if (de_ver > best_version || best_fid == 0) {
					best_fid = de_fid;
					best_version = de_ver;
				}
			}
		}

		vmsfs_bput(bh);
	}

found:
	*fid_out = best_fid;
	(void)best_version;
	return 0;
}

uint16_t vmsfs_dir_highest_version(struct vmsfs_volume *vol,
				   const struct vmsfs_retrieval_ptr *map,
				   uint16_t map_count, int case_blind,
				   const char *base, size_t base_len)
{
	uint16_t highest = 0;
	uint32_t vbn;
	int ret;

	for (vbn = 1; ; vbn++) {
		uint32_t lbn;
		struct vmsfs_bh *bh;
		char *blk;
		unsigned int j;

		ret = vmsfs_vbn_to_lbn(map, map_count, vbn, &lbn);
		if (ret)
			break;

		bh = vmsfs_bget(vol->host, lbn);
		if (!bh)
			break;
		blk = vmsfs_bdata(bh);

		for (j = 0; j < VMSFS_DIR_PER_BLOCK; j++) {
			struct vmsfs_dir_entry *de;
			unsigned int off = j * VMSFS_DIR_ENTRY_SIZE;
			uint16_t de_ver;

			de = (struct vmsfs_dir_entry *)(blk + off);

			if (vmsfs_le32_to_cpu(de->de_fid) == 0)
				continue;

			de_ver = vmsfs_le16_to_cpu(de->de_version);

			if (vmsfs_name_match(de->de_name, de->de_name_len,
					     base, base_len, case_blind)) {
				if (de_ver > highest)
					highest = de_ver;
			}
		}

		vmsfs_bput(bh);
	}

	return highest;
}

int vmsfs_dir_add_entry(struct vmsfs_volume *vol, struct vmsfs_retrieval_ptr *map,
			uint16_t *map_count, uint32_t fid, const char *name,
			uint8_t name_len, uint16_t version, int *blocks_added)
{
	uint32_t vbn;
	int ret;

	*blocks_added = 0;

	/* Scan existing blocks for a free slot. */
	for (vbn = 1; ; vbn++) {
		uint32_t lbn;
		struct vmsfs_bh *bh;
		char *blk;
		unsigned int j;

		ret = vmsfs_vbn_to_lbn(map, *map_count, vbn, &lbn);
		if (ret)
			break;  /* no more mapped blocks */

		bh = vmsfs_bget(vol->host, lbn);
		if (!bh)
			return -VMSFS_EIO;
		blk = vmsfs_bdata(bh);

		for (j = 0; j < VMSFS_DIR_PER_BLOCK; j++) {
			struct vmsfs_dir_entry *de;
			unsigned int off = j * VMSFS_DIR_ENTRY_SIZE;

			de = (struct vmsfs_dir_entry *)(blk + off);

			if (vmsfs_le32_to_cpu(de->de_fid) == 0) {
				/* Found free slot. */
				memset(de, 0, VMSFS_DIR_ENTRY_SIZE);
				de->de_fid = vmsfs_cpu_to_le32(fid);
				de->de_version = vmsfs_cpu_to_le16(version);
				/* Clamp name_len to avoid overflowing de_name[]. */
				if (name_len > sizeof(de->de_name) - 1)
					name_len = sizeof(de->de_name) - 1;
				de->de_name_len = name_len;
				memcpy(de->de_name, name, name_len);
				de->de_name[name_len] = '\0';

				vmsfs_bdirty_sync(bh);
				vmsfs_bput(bh);
				return 0;
			}
		}

		vmsfs_bput(bh);
	}

	/* No free slot found -- extend directory with a new data block. */
	{
		uint32_t new_lbn;
		struct vmsfs_bh *bh;
		struct vmsfs_dir_entry *de;
		char *blk;

		ret = vmsfs_alloc_block(vol, &new_lbn);
		if (ret)
			return ret;

		ret = vmsfs_extend_map(map, map_count, new_lbn);
		if (ret) {
			vmsfs_free_block(vol, new_lbn);
			return ret;
		}

		/*
		 * The directory grew by one block: the backend bumps the
		 * directory inode's i_blocks/i_size and flushes its header
		 * (which now carries the extended retrieval map). The former
		 * in-place flush lived here; moving it to the backend keeps this
		 * scanner free of struct inode. Under the caller's allocation
		 * lock the ordering relative to the entry write below is not
		 * observable.
		 */
		*blocks_added = 1;

		/* Zero the new block and write first entry. */
		bh = vmsfs_bget(vol->host, new_lbn);
		if (!bh)
			return -VMSFS_EIO;
		blk = vmsfs_bdata(bh);

		memset(blk, 0, VMSFS_BLOCK_SIZE);
		de = (struct vmsfs_dir_entry *)blk;
		de->de_fid = vmsfs_cpu_to_le32(fid);
		de->de_version = vmsfs_cpu_to_le16(version);
		/* Clamp name_len to avoid overflowing de_name[]. */
		if (name_len > sizeof(de->de_name) - 1)
			name_len = sizeof(de->de_name) - 1;
		de->de_name_len = name_len;
		memcpy(de->de_name, name, name_len);
		de->de_name[name_len] = '\0';

		vmsfs_bdirty_sync(bh);
		vmsfs_bput(bh);
		return 0;
	}
}

int vmsfs_dir_remove_entry(struct vmsfs_volume *vol,
			   const struct vmsfs_retrieval_ptr *map,
			   uint16_t map_count, uint32_t fid, uint16_t version)
{
	uint32_t vbn;
	int ret;

	for (vbn = 1; ; vbn++) {
		uint32_t lbn;
		struct vmsfs_bh *bh;
		char *blk;
		unsigned int j;

		ret = vmsfs_vbn_to_lbn(map, map_count, vbn, &lbn);
		if (ret)
			return -VMSFS_ENOENT;

		bh = vmsfs_bget(vol->host, lbn);
		if (!bh)
			return -VMSFS_EIO;
		blk = vmsfs_bdata(bh);

		for (j = 0; j < VMSFS_DIR_PER_BLOCK; j++) {
			struct vmsfs_dir_entry *de;
			unsigned int off = j * VMSFS_DIR_ENTRY_SIZE;

			de = (struct vmsfs_dir_entry *)(blk + off);

			if (vmsfs_le32_to_cpu(de->de_fid) == fid &&
			    (version == 0 ||
			     vmsfs_le16_to_cpu(de->de_version) == version)) {
				memset(de, 0, VMSFS_DIR_ENTRY_SIZE);
				vmsfs_bdirty_sync(bh);
				vmsfs_bput(bh);
				return 0;
			}
		}

		vmsfs_bput(bh);
	}
}

int vmsfs_dir_is_empty(struct vmsfs_volume *vol,
		       const struct vmsfs_retrieval_ptr *map, uint16_t map_count)
{
	uint32_t vbn;
	int ret;

	for (vbn = 1; ; vbn++) {
		uint32_t lbn;
		struct vmsfs_bh *bh;
		char *blk;
		unsigned int j;

		ret = vmsfs_vbn_to_lbn(map, map_count, vbn, &lbn);
		if (ret)
			break;

		bh = vmsfs_bget(vol->host, lbn);
		if (!bh)
			break;
		blk = vmsfs_bdata(bh);

		for (j = 0; j < VMSFS_DIR_PER_BLOCK; j++) {
			struct vmsfs_dir_entry *de =
				(struct vmsfs_dir_entry *)(blk +
							   j * VMSFS_DIR_ENTRY_SIZE);

			if (vmsfs_le32_to_cpu(de->de_fid) != 0) {
				vmsfs_bput(bh);
				return 0;
			}
		}

		vmsfs_bput(bh);
	}

	return 1;
}

int vmsfs_dir_entry_decode(const void *block512, unsigned int slot,
			   struct vmsfs_dirent_view *out)
{
	const char *blk = block512;
	const struct vmsfs_dir_entry *de =
		(const struct vmsfs_dir_entry *)(blk + slot * VMSFS_DIR_ENTRY_SIZE);

	out->fid = vmsfs_le32_to_cpu(de->de_fid);
	out->version = vmsfs_le16_to_cpu(de->de_version);
	out->name_len = de->de_name_len;
	out->name = de->de_name;
	return out->fid != 0;
}

int vmsfs_dir_format_name(const struct vmsfs_dirent_view *e, char *buf,
			  size_t size, int *is_dir)
{
	/*
	 * Versioned files display as "NAME.TYPE;VER"; unversioned entries
	 * (directories, version 0) display as just "NAME". Mirrors the former
	 * readdir formatting exactly, so d_type maps *is_dir -> DT_DIR/DT_REG.
	 */
	if (e->version > 0) {
		*is_dir = 0;
		return snprintf(buf, size, "%.*s;%u",
				e->name_len, e->name, e->version);
	}

	*is_dir = 1;
	return snprintf(buf, size, "%.*s", e->name_len, e->name);
}
