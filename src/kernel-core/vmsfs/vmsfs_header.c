// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_header.c - substrate-neutral ODS-2 file-header interpretation
 * (rd vms-d69, epic vms-8e8).
 *
 * The pure decode/encode half of the block/inode seam (README-backend.md §3b):
 * these functions translate between the on-disk struct vmsfs_file_header (a
 * 512-byte block, all fields little-endian, XOR-checksummed) and the host-neutral
 * POD struct vmsfs_fh_info. They touch NO host inode and NO buffer I/O -- the
 * backend reads/writes the block, and keeps only the thin `POD <-> struct inode`
 * copy. Extracted verbatim (behaviour-preserving) from vmsfs_blkdev.c's
 * vmsfs_blkdev_iget() / vmsfs_blkdev_flush_inode() and the create/mkdir/rename
 * header writes.
 *
 * 0 <linux/...> includes: endian access + the wall clock come through the
 * vmsfs_bio / vmsfs_backend vocabulary shim.
 */

#include "vmsfs_bio.h"

enum vmsfs_fh_status vmsfs_fh_decode(const void *block512,
				     struct vmsfs_fh_info *out)
{
	const struct vmsfs_file_header *fh = block512;
	uint32_t cksum;
	unsigned int i;

	/* Validate magic. */
	if (vmsfs_le32_to_cpu(fh->fh_magic) != VMSFS_FH_MAGIC)
		return VMSFS_FH_BAD_MAGIC;

	/* Validate checksum. */
	cksum = vmsfs_checksum(fh, sizeof(*fh));
	if (cksum != vmsfs_le32_to_cpu(fh->fh_checksum))
		return VMSFS_FH_BAD_CHECKSUM;

	/* Verify in-use. */
	if (!(vmsfs_le16_to_cpu(fh->fh_flags) & VMSFS_FH_INUSE))
		return VMSFS_FH_NOT_INUSE;

	out->fid        = vmsfs_le32_to_cpu(fh->fh_fid);
	out->size       = vmsfs_le64_to_cpu(fh->fh_size);
	out->created    = vmsfs_le64_to_cpu(fh->fh_created);
	out->modified   = vmsfs_le64_to_cpu(fh->fh_modified);
	out->accessed   = vmsfs_le64_to_cpu(fh->fh_accessed);
	out->backup     = vmsfs_le64_to_cpu(fh->fh_backup);
	out->blocks     = vmsfs_le32_to_cpu(fh->fh_blocks);
	out->parent_fid = vmsfs_le32_to_cpu(fh->fh_parent_fid);
	out->flags      = vmsfs_le16_to_cpu(fh->fh_flags);
	out->version    = vmsfs_le16_to_cpu(fh->fh_version);
	out->protection = vmsfs_le16_to_cpu(fh->fh_protection);
	out->uic_group  = vmsfs_le16_to_cpu(fh->fh_uic_group);
	out->uic_member = vmsfs_le16_to_cpu(fh->fh_uic_member);
	out->link_count = vmsfs_le16_to_cpu(fh->fh_link_count);

	memcpy(out->name, fh->fh_name, sizeof(out->name));
	memcpy(out->type, fh->fh_type, sizeof(out->type));

	/* Cache retrieval pointers (clamped, exactly as the former iget did). */
	out->map_count = vmsfs_le16_to_cpu(fh->fh_map_count);
	if (out->map_count > VMSFS_MAX_RETRIEVAL)
		out->map_count = VMSFS_MAX_RETRIEVAL;
	for (i = 0; i < out->map_count; i++) {
		out->map[i].rp_lbn   = vmsfs_le32_to_cpu(fh->fh_map[i].rp_lbn);
		out->map[i].rp_count = vmsfs_le32_to_cpu(fh->fh_map[i].rp_count);
	}

	return VMSFS_FH_OK;
}

void vmsfs_fh_encode(const struct vmsfs_fh_info *in, void *block512)
{
	struct vmsfs_file_header *fh = block512;
	unsigned int i;

	memset(fh, 0, sizeof(*fh));
	fh->fh_magic      = vmsfs_cpu_to_le32(VMSFS_FH_MAGIC);
	fh->fh_fid        = vmsfs_cpu_to_le32(in->fid);
	fh->fh_size       = vmsfs_cpu_to_le64(in->size);
	fh->fh_created    = vmsfs_cpu_to_le64(in->created);
	fh->fh_modified   = vmsfs_cpu_to_le64(in->modified);
	fh->fh_accessed   = vmsfs_cpu_to_le64(in->accessed);
	fh->fh_backup     = vmsfs_cpu_to_le64(in->backup);
	fh->fh_blocks     = vmsfs_cpu_to_le32(in->blocks);
	fh->fh_parent_fid = vmsfs_cpu_to_le32(in->parent_fid);
	fh->fh_flags      = vmsfs_cpu_to_le16(in->flags);
	fh->fh_version    = vmsfs_cpu_to_le16(in->version);
	fh->fh_protection = vmsfs_cpu_to_le16(in->protection);
	fh->fh_uic_group  = vmsfs_cpu_to_le16(in->uic_group);
	fh->fh_uic_member = vmsfs_cpu_to_le16(in->uic_member);
	fh->fh_link_count = vmsfs_cpu_to_le16(in->link_count);
	fh->fh_map_count  = vmsfs_cpu_to_le16(in->map_count);

	strscpy(fh->fh_name, in->name, sizeof(fh->fh_name));
	strscpy(fh->fh_type, in->type, sizeof(fh->fh_type));

	for (i = 0; i < in->map_count && i < VMSFS_MAX_RETRIEVAL; i++) {
		fh->fh_map[i].rp_lbn   = vmsfs_cpu_to_le32(in->map[i].rp_lbn);
		fh->fh_map[i].rp_count = vmsfs_cpu_to_le32(in->map[i].rp_count);
	}

	fh->fh_checksum = 0;
	fh->fh_checksum = vmsfs_cpu_to_le32(vmsfs_checksum(fh, sizeof(*fh)));
}

void vmsfs_fh_write_meta(void *block512, const struct vmsfs_fh_meta *m)
{
	struct vmsfs_file_header *fh = block512;
	unsigned int i;

	fh->fh_size       = vmsfs_cpu_to_le64(m->size);
	fh->fh_blocks     = vmsfs_cpu_to_le32(m->blocks);
	fh->fh_modified   = vmsfs_cpu_to_le64(vmsfs_now_seconds());
	fh->fh_link_count = vmsfs_cpu_to_le16(m->link_count);
	fh->fh_protection = vmsfs_cpu_to_le16(m->protection);

	fh->fh_map_count = vmsfs_cpu_to_le16(m->map_count);
	for (i = 0; i < m->map_count && i < VMSFS_MAX_RETRIEVAL; i++) {
		fh->fh_map[i].rp_lbn   = vmsfs_cpu_to_le32(m->map[i].rp_lbn);
		fh->fh_map[i].rp_count = vmsfs_cpu_to_le32(m->map[i].rp_count);
	}
	/* Zero remaining map slots. */
	for (; i < VMSFS_MAX_RETRIEVAL; i++) {
		fh->fh_map[i].rp_lbn   = 0;
		fh->fh_map[i].rp_count = 0;
	}

	fh->fh_checksum = 0;
	fh->fh_checksum = vmsfs_cpu_to_le32(vmsfs_checksum(fh, sizeof(*fh)));
}

void vmsfs_fh_write_owner(void *block512, const struct vmsfs_fh_owner *o)
{
	struct vmsfs_file_header *fh = block512;

	fh->fh_protection = vmsfs_cpu_to_le16(o->protection);
	fh->fh_uic_group  = vmsfs_cpu_to_le16(o->uic_group);
	fh->fh_uic_member = vmsfs_cpu_to_le16(o->uic_member);
	fh->fh_modified   = vmsfs_cpu_to_le64(vmsfs_now_seconds());

	fh->fh_checksum = 0;
	fh->fh_checksum = vmsfs_cpu_to_le32(vmsfs_checksum(fh, sizeof(*fh)));
}

/*
 * POSIX-standard permission-bit values (identical on every host this project
 * builds for -- Linux, NetBSD -- so this pure-arithmetic function needs no
 * host <sys/stat.h>/<linux/stat.h> include, matching the vmsfs_backend.h
 * vocabulary discipline the rest of this file follows).
 */
#define VMSFS_MODE_IRUSR 0000400
#define VMSFS_MODE_IWUSR 0000200
#define VMSFS_MODE_IXUSR 0000100
#define VMSFS_MODE_IRGRP 0000040
#define VMSFS_MODE_IWGRP 0000020
#define VMSFS_MODE_IXGRP 0000010
#define VMSFS_MODE_IROTH 0000004
#define VMSFS_MODE_IWOTH 0000002
#define VMSFS_MODE_IXOTH 0000001

uint16_t vmsfs_mode_to_prot(uint32_t unix_mode)
{
	uint16_t prot = 0;
	uint8_t owner = VMSFS_PROT_R | VMSFS_PROT_W | VMSFS_PROT_E | VMSFS_PROT_D;
	uint8_t group = VMSFS_PROT_R | VMSFS_PROT_W | VMSFS_PROT_E | VMSFS_PROT_D;
	uint8_t world = VMSFS_PROT_R | VMSFS_PROT_W | VMSFS_PROT_E | VMSFS_PROT_D;

	if (unix_mode & VMSFS_MODE_IRUSR) owner &= (uint8_t)~VMSFS_PROT_R;
	if (unix_mode & VMSFS_MODE_IWUSR) owner &= (uint8_t)~(VMSFS_PROT_W | VMSFS_PROT_D);
	if (unix_mode & VMSFS_MODE_IXUSR) owner &= (uint8_t)~VMSFS_PROT_E;

	if (unix_mode & VMSFS_MODE_IRGRP) group &= (uint8_t)~VMSFS_PROT_R;
	if (unix_mode & VMSFS_MODE_IWGRP) group &= (uint8_t)~(VMSFS_PROT_W | VMSFS_PROT_D);
	if (unix_mode & VMSFS_MODE_IXGRP) group &= (uint8_t)~VMSFS_PROT_E;

	if (unix_mode & VMSFS_MODE_IROTH) world &= (uint8_t)~VMSFS_PROT_R;
	if (unix_mode & VMSFS_MODE_IWOTH) world &= (uint8_t)~(VMSFS_PROT_W | VMSFS_PROT_D);
	if (unix_mode & VMSFS_MODE_IXOTH) world &= (uint8_t)~VMSFS_PROT_E;

	/* System nibble stays 0 -- SYSTEM always has full access (VMS convention,
	 * matching src/vmsfs/vmsfs_protect.c's vmsfs_mode_to_protection()). */
	prot |= (uint16_t)((uint16_t)owner << VMSFS_PROT_OWN_SHIFT);
	prot |= (uint16_t)((uint16_t)group << VMSFS_PROT_GRP_SHIFT);
	prot |= (uint16_t)((uint16_t)world << VMSFS_PROT_WLD_SHIFT);

	return prot;
}

void vmsfs_fh_write_rename(void *block512, const struct vmsfs_fh_rename *r)
{
	struct vmsfs_file_header *fh = block512;

	memset(fh->fh_name, 0, sizeof(fh->fh_name));
	memset(fh->fh_type, 0, sizeof(fh->fh_type));
	strscpy(fh->fh_name, r->name, sizeof(fh->fh_name));
	strscpy(fh->fh_type, r->type, sizeof(fh->fh_type));
	fh->fh_version    = vmsfs_cpu_to_le16(r->version);
	fh->fh_parent_fid = vmsfs_cpu_to_le32(r->parent_fid);
	fh->fh_modified   = vmsfs_cpu_to_le64(vmsfs_now_seconds());

	fh->fh_checksum = 0;
	fh->fh_checksum = vmsfs_cpu_to_le32(vmsfs_checksum(fh, sizeof(*fh)));
}
