// SPDX-License-Identifier: GPL-2.0
/*
 * vms_dlm_hash.c - the DLM resource-name hash (rd vms-66fe).
 *
 * The provenance (Baron's Rule-8 ruling on rd vms-dc2), the function itself
 * and the reason nothing routes on it yet are all in vms_dlm_hash.h. Read it
 * first; this file is only the arithmetic.
 *
 * This TU is PURE: no seam call, no allocation, no clock, no library call. It
 * is also deliberately free of any wire-offset knowledge -- the caller hands
 * over already-parsed fields, so the only translation unit that knows where
 * the identity block lives in a frame stays vms_cluster_codec_dlm.c.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */

#include "vms_dlm_hash.h"

/*
 * A 32-bit rotate written so no substrate helper is needed and so a shift of
 * 32 (undefined in C) can never be reached: the amount is masked, and the
 * zero case is answered without shifting by the width.
 */
static uint32_t dlm_hash_rotl32(uint32_t v, unsigned int n)
{
	n &= 31u;
	if (n == 0u)
		return v;
	return (uint32_t)((v << n) | (v >> (32u - n)));
}

/*
 * Longword 0: the identity block ahead of the name, read little-endian --
 * group word, access-mode byte, name-length byte (body[44:48]).
 */
static uint32_t dlm_hash_ident_lw(uint16_t group, uint8_t mode, uint8_t name_len)
{
	return (uint32_t)group
	     | ((uint32_t)mode << 16)
	     | ((uint32_t)name_len << 24);
}

/*
 * The name's `index`-th longword, little-endian, zero-padded past the name's
 * end. Zero padding is not an assumption: it is what the corpus says, over
 * every residue of the length mod 4 (lengths 3, 5, 7, 10, 11, 13, 14, 15, 17,
 * 18, 21, 22, 25, 26, 27 and 30 all reproduce exactly).
 */
static uint32_t dlm_hash_name_lw(const uint8_t *name, uint32_t name_len,
				 uint32_t index)
{
	uint32_t base = index * 4u;
	uint32_t w = 0;
	uint32_t i;

	for (i = 0; i < 4u; i++) {
		if (base + i < name_len)
			w |= (uint32_t)name[base + i] << (8u * i);
	}
	return w;
}

/* The rotate-and-XOR fold, one step per longword. */
static uint32_t dlm_hash_fold(uint16_t group, uint8_t mode,
			      const uint8_t *name, uint32_t name_len)
{
	uint32_t lws = (name_len + 3u) / 4u;
	uint32_t acc;
	uint32_t j;

	acc = dlm_hash_rotl32(dlm_hash_ident_lw(group, mode, (uint8_t)name_len),
			      VMS_DLM_HASH_ROT);
	for (j = 0; j < lws; j++) {
		acc ^= dlm_hash_name_lw(name, name_len, j);
		acc = dlm_hash_rotl32(acc, VMS_DLM_HASH_ROT);
	}
	return acc;
}

enum vms_dlm_hash_status vms_dlm_name_hash(uint16_t group, uint8_t mode,
					   const uint8_t *name,
					   uint32_t name_len,
					   uint32_t *out)
{
	if (name == (const void *)0 || out == (void *)0)
		return VMS_DLM_HASH_E_INVAL;
	if (name_len == 0u || name_len > VMS_DLM_HASH_NAME_MAX)
		return VMS_DLM_HASH_E_RANGE;

	*out = (uint32_t)(dlm_hash_fold(group, mode, name, name_len)
			  * VMS_DLM_HASH_MULT);
	return VMS_DLM_HASH_OK;
}

/*
 * The coverage test, one axis at a time, against the three masks in the header.
 * Nothing is computed: this is a membership test over measured sets.
 */
enum vms_dlm_hash_status vms_dlm_name_hash_coverage(uint16_t group, uint8_t mode,
						    uint32_t name_len)
{
	if (name_len == 0u || name_len > VMS_DLM_HASH_NAME_MAX)
		return VMS_DLM_HASH_E_RANGE;
	if ((VMS_DLM_HASH_LEN_PROVEN & ((uint32_t)1u << name_len)) == 0u)
		return VMS_DLM_HASH_E_COVER;
	if (mode > 7u ||
	    (VMS_DLM_HASH_MODE_PROVEN & ((uint32_t)1u << mode)) == 0u)
		return VMS_DLM_HASH_E_COVER;
	if ((uint32_t)group > VMS_DLM_HASH_GROUP_PROVEN_MAX)
		return VMS_DLM_HASH_E_COVER;
	return VMS_DLM_HASH_OK;
}

enum vms_dlm_hash_status vms_dlm_name_hash_proven(uint16_t group, uint8_t mode,
						  const uint8_t *name,
						  uint32_t name_len,
						  uint32_t *out)
{
	enum vms_dlm_hash_status st;

	if (name == (const void *)0 || out == (void *)0)
		return VMS_DLM_HASH_E_INVAL;
	st = vms_dlm_name_hash_coverage(group, mode, name_len);
	if (st != VMS_DLM_HASH_OK)
		return st;
	return vms_dlm_name_hash(group, mode, name, name_len, out);
}
