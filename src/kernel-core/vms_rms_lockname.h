/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_rms_lockname.h - the RMS FILE-ACCESS lock's resource name and identity,
 * exactly as a real OpenVMS VAX puts them on the cluster wire (rd vms-326).
 *
 * ===========================================================================
 * WHAT IT IS, MEASURED
 *
 * RMS takes one DLM lock per ACCESSED FILE so that two opens of the same file
 * arbitrate (and two opens of DIFFERENT files never do). The name is
 *
 *   name[0:4]    'RMS$'
 *   name[4:6]    the file id's NUMBER, little-endian u16
 *   name[6:8]    its SEQUENCE, little-endian u16
 *   name[8]      its RVN
 *   name[9]      its NMX (the file number's high byte)
 *   name[10]     0x02
 *   name[11:23]  the ODS-2 VOLUME LABEL, 12 bytes, SPACE-padded
 *   name[23:26]  00 00 00
 *
 * -- 26 counted bytes, and the identity that QUALIFIES it (body[44:46] and
 * body[46], which the directory hash is also computed over) is UIC GROUP 0 and
 * ACCESS MODE 1, executive mode.
 *
 * Measured on every committed capture: 1,130 real cat-0x02 requests across the
 * five name-carrying opcodes carry this exact shape and identity, 1,130 of
 * 1,130 (tools/cluster/dlm_rms_name_hash_check.py; the per-byte map is pinned
 * separately by tools/cluster/dlm_rms_name_census.py, which classifies names by
 * ORIGINATOR). 47 distinct such names appear; eight of them, with their capture
 * and frame numbers, are the fixtures of tests/cluster/host/test_rms_lockname.c
 * -- this builder is checked byte-for-byte against real VAX frames, not against
 * its own idea of the form.
 *
 * ===========================================================================
 * WHY THE VOLUME IS IN IT, which is the whole point
 *
 * The DLM is CLUSTER-WIDE. A name built from the file id ALONE makes two
 * different files that happen to share a file id on two members' disks ONE
 * resource: node A writing NETACP.LOG at EX refused node B's read of
 * NETNODE_REMOTE.DAT, and B reported the node as unknown (rd vms-acf3). OVMX's
 * predecessor name ('RMS$<hexfid>.<seq>.<rvn>', src/vmsrms/rms_core.c) was
 * exactly that shape.
 *
 * ===========================================================================
 * AND WHY THERE IS NO FALLBACK FOR A MISSING LABEL (INV-6)
 *
 * `vms_rms_lockname()` REFUSES to build a name without a volume label -- it
 * returns 0 and the caller must not lock. A blank or invented label span would
 * be a volume nobody has, and two nodes that both invented one would collide on
 * a resource that names no file. The label is not a value this code may
 * default: it is read from the executive's own mounted-volume table, and a
 * volume with no label in that table is not a volume a file was opened on.
 *
 * ===========================================================================
 * PURE, AND DELIBERATELY HEADER-ONLY
 *
 * No seam, no allocation, no time, no globals, no wire offsets of its own: it
 * writes a resource NAME, which the DLM codec then places as a counted string.
 * Header-only so the Files-11 ACP (src/kernel-core/vmsfs_acp.c, which holds the
 * file id and the volume label at IO$_ACCESS time and is where the executive
 * takes this lock) and a plain host unit test compile it on identical terms.
 */
#ifndef OVMX_VMS_RMS_LOCKNAME_H
#define OVMX_VMS_RMS_LOCKNAME_H

#include "vms_wire_types.h"   /* fixed-width types, substrate-selected once */

/* The name's length, which is a constant of the form and not a maximum. */
#define VMS_RMS_LOCKNAME_LEN      26u

/* Where the pieces sit, so no caller re-counts them. */
#define VMS_RMS_LOCKNAME_FID_OFF   4u
#define VMS_RMS_LOCKNAME_SEP_OFF  10u
#define VMS_RMS_LOCKNAME_SEP       0x02u
#define VMS_RMS_LOCKNAME_LBL_OFF  11u
#define VMS_RMS_LOCKNAME_LBL_LEN  12u
#define VMS_RMS_LOCKNAME_TAIL_OFF 23u
#define VMS_RMS_LOCKNAME_TAIL_LEN  3u

/*
 * THE IDENTITY THAT QUALIFIES THE NAME (rd vms-b5b0), measured on all 1,130
 * frames. The GROUP is 0 -- a system-wide name, LCK$M_SYSTEM's group -- and the
 * ACCESS MODE is 1, EXECUTIVE: on a real VAX this lock is taken by RMS, which
 * RUNS at executive mode.
 *
 * THAT IS WHY THE EXECUTIVE TAKES THIS LOCK (conductor's ruling, 2026-10-22).
 * OVMX's RMS is a userspace library at access mode 3, and a lock taken at mode
 * 3 is a DIFFERENT RESOURCE from the VAX's: it would not contend with it, so
 * both nodes could hold EX on one file -- worse than the collision above. The
 * answer is NOT to let a user-mode caller claim mode 1 through the $ENQ ABI:
 * that would let any process forge or steal another's RMS file locks, which the
 * VMS protection model does not permit and which privileged-power-in-the-
 * executive forbids. The lock is taken BY THE EXECUTIVE (the ACP, on RMS's
 * behalf), and the executive assigns this identity -- it is never stated by a
 * caller.
 */
#define VMS_RMS_LOCKNAME_GROUP     0u
#define VMS_RMS_LOCKNAME_ACMODE    1u

/*
 * Build the name. Returns VMS_RMS_LOCKNAME_LEN on success, or 0 -- and writes
 * nothing -- when it cannot build the real name: no output room, no label, or a
 * label longer than an ODS-2 label can be. 0 means DO NOT LOCK; there is no
 * second-best name (see the INV-6 note above).
 *
 * `label` is the ODS-2 volume label as the executive's mounted-volume table
 * holds it, `label_len` its significant length (1..12). `out` receives exactly
 * 26 bytes, three of them NUL, which is why every caller must carry the count.
 */
static inline uint32_t vms_rms_lockname(uint16_t fid_num, uint16_t fid_seq,
					uint8_t fid_rvn, uint8_t fid_nmx,
					const char *label, uint32_t label_len,
					uint8_t *out, uint32_t out_cap)
{
	uint32_t i;

	if (out == (uint8_t *)0 || out_cap < VMS_RMS_LOCKNAME_LEN)
		return 0u;
	if (label == (const char *)0 || label_len == 0u ||
	    label_len > VMS_RMS_LOCKNAME_LBL_LEN)
		return 0u;

	out[0] = (uint8_t)'R';
	out[1] = (uint8_t)'M';
	out[2] = (uint8_t)'S';
	out[3] = (uint8_t)'$';
	out[VMS_RMS_LOCKNAME_FID_OFF + 0u] = (uint8_t)(fid_num & 0xffu);
	out[VMS_RMS_LOCKNAME_FID_OFF + 1u] = (uint8_t)((fid_num >> 8) & 0xffu);
	out[VMS_RMS_LOCKNAME_FID_OFF + 2u] = (uint8_t)(fid_seq & 0xffu);
	out[VMS_RMS_LOCKNAME_FID_OFF + 3u] = (uint8_t)((fid_seq >> 8) & 0xffu);
	out[VMS_RMS_LOCKNAME_FID_OFF + 4u] = fid_rvn;
	out[VMS_RMS_LOCKNAME_FID_OFF + 5u] = fid_nmx;
	out[VMS_RMS_LOCKNAME_SEP_OFF] = (uint8_t)VMS_RMS_LOCKNAME_SEP;
	for (i = 0u; i < VMS_RMS_LOCKNAME_LBL_LEN; i++) {
		uint8_t c = (i < label_len) ? (uint8_t)label[i] : (uint8_t)' ';

		/* A label the table holds as a C string ends early: SPACE-pad
		 * from there, which is what the measured form carries. */
		if (c == 0u)
			c = (uint8_t)' ';
		out[VMS_RMS_LOCKNAME_LBL_OFF + i] = c;
	}
	for (i = 0u; i < VMS_RMS_LOCKNAME_TAIL_LEN; i++)
		out[VMS_RMS_LOCKNAME_TAIL_OFF + i] = 0u;
	return VMS_RMS_LOCKNAME_LEN;
}

#endif /* OVMX_VMS_RMS_LOCKNAME_H */
