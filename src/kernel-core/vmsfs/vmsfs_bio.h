/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmsfs_bio.h - the OVMX ODS-2 block/inode seam (seam #2): substrate-neutral
 * contract for the storage allocator, the directory-block scanner and the
 * file-header decode/encode (rd vms-d69, epic vms-8e8).
 *
 * This is the header the block-coupled ODS-2 core files (vmsfs_alloc.c,
 * vmsfs_dirscan.c, vmsfs_header.c) are written against. It declares:
 *
 *   1. `struct vmsfs_volume` -- a host-neutral volume descriptor (geometry +
 *      the storage bitmap + an opaque `host` handle). The Linux backend embeds
 *      one in its `vmsfs_sb_info` (host = the super_block); a NetBSD backend
 *      embeds one in its mount. The core reaches all volume geometry through it
 *      and never names a host superblock/mount struct.
 *
 *   2. The block-buffer / bitmap / endian / wall-clock OPS the core calls
 *      (vmsfs_bget / vmsfs_bdata / vmsfs_bdirty[_sync] / vmsfs_bput /
 *      vmsfs_bit_* / vmsfs_le*_to_cpu / vmsfs_cpu_to_le* / vmsfs_now_seconds).
 *      Their concrete realization is provided by the build-time-selected
 *      backend header (via vmsfs_backend.h) -- on Linux, trivial forwarders to
 *      sb_bread / brelse / mark_buffer_dirty / find_next_zero_bit / le*_to_cpu /
 *      ktime_get_real_seconds in vmsfs_backend_linux.h.
 *
 *   3. The ODS-2 file-header INTERPRETATION seam: a POD `struct vmsfs_fh_info`
 *      and the pure vmsfs_fh_decode / vmsfs_fh_encode (+ the two partial-update
 *      writers used by flush and rename). The backend keeps only the thin
 *      `POD <-> host inode` copy.
 *
 *   4. The pure ODS-2 ALGORITHM prototypes the backend calls: the allocator,
 *      the directory-block scanner, and the per-entry decode/format helpers the
 *      backend's readdir emission uses.
 *
 * Clean-room (CLAUDE.md Rule 8): this contract and the algorithms behind it are
 * OVMX's own code over PUBLIC, documented host-kernel APIs. No VSI/HPE/Linux/
 * NetBSD source is copied.
 */

#ifndef OVMX_VMSFS_BIO_H
#define OVMX_VMSFS_BIO_H

#include "vmsfs_core.h"    /* fixed-width types + backend ops (via vmsfs_backend.h),
                            * struct vmsfs_retrieval_ptr + name limits, the
                            * seam-#1 version/map/name algorithms */

/*
 * Opaque buffer handle. The core holds only `struct vmsfs_bh *` and passes it
 * back to the vmsfs_b* ops; the backend realizes it (Linux: struct buffer_head).
 * Also forward-declared in the backend header so its ops can be inlined there.
 */
struct vmsfs_bh;

/* ================================================================
 * The volume descriptor (embedded by each backend's mount object)
 *
 * `host` is the backend's own volume/superblock handle, passed opaquely to
 * vmsfs_bget(); the core never dereferences it. Every other field is the
 * host-neutral ODS-2 volume geometry the allocator needs, laid out exactly like
 * the Linux glue's former flat vmsfs_sb_info fields so the extraction is
 * behaviour-preserving. `free_blocks` is mutated in place by the allocator and
 * read back by the backend's statfs.
 * ================================================================ */
struct vmsfs_volume {
	void          *host;          /* opaque backend volume handle (Linux: sb) */
	unsigned long *bitmap;        /* cached storage bitmap (one bit per block) */
	uint32_t       bitmap_lbn;    /* start LBN of the on-disk bitmap */
	uint32_t       bitmap_blocks; /* number of bitmap blocks */
	uint32_t       index_lbn;     /* start LBN of the file-header (index) area */
	uint32_t       max_files;     /* max file headers (FIDs) on the volume */
	uint32_t       data_lbn;      /* start LBN of the data area */
	uint32_t       total_blocks;  /* total blocks on the volume */
	uint32_t       free_blocks;   /* current free-block count (mutated here) */
};

/* ================================================================
 * File-header interpretation (vmsfs_header.c)
 * ================================================================ */

/*
 * Decoded, host-neutral view of an ODS-2 file header (struct vmsfs_file_header).
 * Carries every meaningful header field so vmsfs_fh_encode() is a faithful
 * writer; the on-disk magic/reserved/checksum are supplied by encode itself.
 */
struct vmsfs_fh_info {
	uint32_t fid;
	uint64_t size;
	uint64_t created;
	uint64_t modified;
	uint64_t accessed;
	uint64_t backup;
	uint32_t blocks;
	uint32_t parent_fid;
	uint16_t flags;
	uint16_t version;
	uint16_t protection;
	uint16_t uic_group;
	uint16_t uic_member;
	uint16_t link_count;
	uint16_t map_count;              /* clamped to VMSFS_MAX_RETRIEVAL by decode */
	char     name[40];              /* == sizeof(fh_name) */
	char     type[40];              /* == sizeof(fh_type) */
	struct vmsfs_retrieval_ptr map[VMSFS_MAX_RETRIEVAL];
};

/* Result of decoding+validating a file-header block. */
enum vmsfs_fh_status {
	VMSFS_FH_OK = 0,
	VMSFS_FH_BAD_MAGIC,     /* fh_magic != VMSFS_FH_MAGIC */
	VMSFS_FH_BAD_CHECKSUM,  /* stored checksum mismatch */
	VMSFS_FH_NOT_INUSE,     /* header slot is free (VMSFS_FH_INUSE clear) */
};

/*
 * Validate (magic, checksum, in-use) and decode a 512-byte file-header block
 * into @out. On any non-OK return @out is not fully populated; the backend maps
 * the status to its own errno + diagnostic (the original iget's EIO/EIO/ENOENT).
 */
enum vmsfs_fh_status vmsfs_fh_decode(const void *block512,
				     struct vmsfs_fh_info *out);

/*
 * Encode @in as a full file header into a 512-byte block (zero-fills, writes
 * magic + every field + the retrieval map, then re-checksums). Used to create a
 * fresh header (file create / directory mkdir).
 */
void vmsfs_fh_encode(const struct vmsfs_fh_info *in, void *block512);

/*
 * Partial header write-back for the inode-flush path: read-modify-write of the
 * size / blocks / link-count / protection / retrieval-map fields (plus a
 * refreshed modification time) on an EXISTING header block, then re-checksum.
 * All other on-disk fields are preserved. @map may be NULL iff @map_count == 0.
 */
struct vmsfs_fh_meta {
	uint64_t size;
	uint32_t blocks;
	uint16_t link_count;
	uint16_t protection;
	uint16_t map_count;
	const struct vmsfs_retrieval_ptr *map;
};
void vmsfs_fh_write_meta(void *block512, const struct vmsfs_fh_meta *m);

/*
 * Partial header write-back for the rename path: rewrite name / type / version /
 * parent FID (plus a refreshed modification time) on an EXISTING header block,
 * then re-checksum. All other on-disk fields are preserved.
 */
struct vmsfs_fh_rename {
	const char *name;   /* fh_name  (already split, uppercased) */
	const char *type;   /* fh_type */
	uint16_t    version;
	uint32_t    parent_fid;
};
void vmsfs_fh_write_rename(void *block512, const struct vmsfs_fh_rename *r);

/* ================================================================
 * Storage / FID allocator + block-map growth (vmsfs_alloc.c)
 *
 * The volume free-count and storage bitmap are mutated in place; the caller
 * (backend) holds the volume's allocation lock across these, exactly as the
 * former Linux glue held sbi->alloc_lock. Returns 0 or a negative VMSFS_E* .
 * ================================================================ */

/* Allocate one free volume block, mark it used, persist the bitmap block. */
int vmsfs_alloc_block(struct vmsfs_volume *vol, uint32_t *lbn_out);

/* Free one volume block, persist the bitmap block. */
int vmsfs_free_block(struct vmsfs_volume *vol, uint32_t lbn);

/* Free every data block named by a file's retrieval map. */
void vmsfs_free_file_blocks(struct vmsfs_volume *vol,
			    const struct vmsfs_retrieval_ptr *map,
			    uint16_t map_count);

/* Find a free file-header slot (FID). */
int vmsfs_alloc_fid(struct vmsfs_volume *vol, uint32_t *fid_out);

/* Release a file-header slot (zero the header block on disk). */
int vmsfs_free_fid(struct vmsfs_volume *vol, uint32_t fid);

/* Write the volume's current free-block count into the on-disk home block. */
int vmsfs_update_home_block(struct vmsfs_volume *vol);

/*
 * Grow a file's block map so VBNs 1..target_vbn are all allocated, zero-filling
 * each newly allocated block. *blocks_added receives the number of blocks
 * actually added (even on a mid-way error), so the backend can bump its inode's
 * block count. @map / @map_count are the file's in-core retrieval array.
 */
int vmsfs_grow_map(struct vmsfs_volume *vol, struct vmsfs_retrieval_ptr *map,
		   uint16_t *map_count, uint32_t target_vbn, int *blocks_added);

/* ================================================================
 * Directory-block scanner (vmsfs_dirscan.c)
 *
 * These walk a directory's data blocks via the retrieval map + vmsfs_bget and
 * make every ODS-2 decision (version resolution, case-blind name match, free-
 * slot placement). The backend keeps struct-inode mutation and the readdir
 * dir_emit emission (which is why readdir stays in the backend and calls the
 * two per-entry helpers at the bottom).
 * ================================================================ */

/*
 * Resolve a (possibly versioned) @name in a directory to a FID. *fid_out is 0
 * when no entry matches. Returns 0, or a negative VMSFS_E* only when @name is
 * not a parseable VMS filename.
 */
int vmsfs_dir_resolve(struct vmsfs_volume *vol,
		      const struct vmsfs_retrieval_ptr *map, uint16_t map_count,
		      int case_blind, const char *name, uint32_t *fid_out);

/* Highest existing version of @base in the directory (0 if none). */
uint16_t vmsfs_dir_highest_version(struct vmsfs_volume *vol,
				   const struct vmsfs_retrieval_ptr *map,
				   uint16_t map_count, int case_blind,
				   const char *base, size_t base_len);

/*
 * Add an entry (fid, name, version) to the directory. Reuses a free slot if one
 * exists; otherwise allocates a new directory data block and extends @map /
 * @map_count. *blocks_added receives 1 iff a new block was added (so the backend
 * bumps the directory inode's size/blocks and flushes its header), else 0.
 */
int vmsfs_dir_add_entry(struct vmsfs_volume *vol, struct vmsfs_retrieval_ptr *map,
			uint16_t *map_count, uint32_t fid, const char *name,
			uint8_t name_len, uint16_t version, int *blocks_added);

/*
 * Remove the entry matching @fid (and @version, or any version when @version==0)
 * from the directory. Returns 0, or -VMSFS_ENOENT / -VMSFS_EIO.
 */
int vmsfs_dir_remove_entry(struct vmsfs_volume *vol,
			   const struct vmsfs_retrieval_ptr *map,
			   uint16_t map_count, uint32_t fid, uint16_t version);

/* Nonzero iff the directory holds no in-use entries. */
int vmsfs_dir_is_empty(struct vmsfs_volume *vol,
		       const struct vmsfs_retrieval_ptr *map, uint16_t map_count);

/*
 * Per-entry helpers for the backend's readdir loop (the backend owns the
 * map-walk + dir_emit; these own the ODS-2 decode + display-name format).
 */
struct vmsfs_dirent_view {
	uint32_t    fid;
	uint16_t    version;
	uint8_t     name_len;
	const char *name;   /* points into the caller's 512-byte block buffer */
};

/*
 * Decode the directory entry at @slot of a 512-byte directory block into @out.
 * Returns 1 if the slot is in use (fid != 0), 0 if it is a free slot.
 */
int vmsfs_dir_entry_decode(const void *block512, unsigned int slot,
			   struct vmsfs_dirent_view *out);

/*
 * Format the display name for a decoded entry ("NAME.TYPE;VER" for a versioned
 * file, "NAME" for an unversioned directory) into @buf. Sets *is_dir (1 for the
 * unversioned/directory form). Returns the snprintf length (as the backend's
 * validity check expects).
 */
int vmsfs_dir_format_name(const struct vmsfs_dirent_view *e, char *buf,
			  size_t size, int *is_dir);

#endif /* OVMX_VMSFS_BIO_H */
