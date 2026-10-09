/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_hash.h - the VMS DLM resource-name hash: the 32-bit value a
 * directory lookup carries, computed rather than only learned (rd vms-66fe).
 *
 * ===========================================================================
 * PROVENANCE, AND WHY THIS FILE IS ALLOWED TO EXIST (Rule 8, rd vms-dc2)
 *
 * Every $ENQ for a root resource in a VMScluster must reach that resource's
 * DIRECTORY node, `vector[(value >> 16) mod n]` (vms_dlm_ldwv.h). The value is
 * a hash of the resource's identity, and OpenVMS does not publish the
 * arithmetic: *VAXcluster Principles* describes the hash only abstractly
 * (p. 6-49). Until 2026-10-08 OVMX therefore never computed it -- it learned
 * the value off the wire for names a VMS node had already looked up
 * (vms_lock.c `hash_known`), and for a name OVMX touched FIRST it had no value
 * at all and mastered locally.
 *
 * Baron's ruling on rd vms-dc2 (2026-10-08) extends Rule 8's scope exactly far
 * enough to close that gap and no further: OVMX MAY determine this function
 * BLACK-BOX from the (name, value) pairs VMS itself broadcasts IN THE CLEAR on
 * every directory lookup, and from NOTHING ELSE. Never from a VSI/HPE binary,
 * a disassembly, a listing or source. The determination that produced the
 * constants below used 963 such pairs -- values real OpenVMS VAX nodes put in
 * cat-0x02 op-0x01 request bodies at body[128:132] -- and was then PROVEN on
 * 253 further pairs held out of it before the work began.
 *
 *   corpus + frozen split  tests/cluster/fixtures/dlm_hash_*.tsv
 *                          tests/cluster/fixtures/README-dlm-hash-corpus.md
 *   method, hypothesis by hypothesis, and how each was eliminated
 *                          docs/design-dlm-name-hash.md
 *
 * ===========================================================================
 * THE FUNCTION
 *
 * The hashed object is the resource-identity block exactly as it rides the
 * wire (vms_cluster_codec_dlm.h, struct vms_dlm_res_ident): a group word, an
 * access-mode byte, a name-length byte, then the name. Read as
 * little-endian 32-bit longwords that is
 *
 *     w[0] = group | (mode << 16) | (name_len << 24)       <- body[44:48]
 *     w[1..] = the name, zero-padded to a longword boundary <- body[48:...]
 *
 * and the hash is a rotate-and-XOR fold of those longwords, scaled by one
 * 32-bit multiplier:
 *
 *     acc = 0
 *     for each w:  acc = ROTL32(acc ^ w, 9)
 *     value = (acc * 0xA53F19B7) mod 2^32
 *
 * Two properties of that shape are worth naming because they are what made it
 * findable, and what makes the implementation below trivially testable:
 *
 *   - the fold is GF(2)-LINEAR, so flipping one name bit flips exactly one
 *     accumulator bit (measured: 561 of 561 rectangles in a two-byte-varying
 *     family closed exactly);
 *   - the final multiply is the ONLY place carries enter, so flipping
 *     accumulator bit j moves the value by exactly +/- (0xA53F19B7 << j).
 *
 * ===========================================================================
 * WHAT THIS FILE DOES *NOT* DO
 *
 * It does not route anything. Wiring a computed value into `dir_resolve` is a
 * separate item (rd vms-b5b0) that lands only after the fresh-lab held-out
 * proof (rd vms-c6e leg 2), because a WRONG hash on the wire is not a benign
 * mistake: it makes the directory node scan the wrong chain, conclude the name
 * is unknown, and name the sender as master -- the 35-per-second grant storm
 * in memory cluster-promotion-gap. Until then OVMX still only asserts values
 * it learned (INV-6), and this TU is reached only by its tests.
 *
 * SUB-RESOURCES ARE OUT OF SCOPE. A sub-resource's value is a property of its
 * parent too (rd vms-4fb finding 3), so no (name -> value) pair can be learned
 * for one and none was used here. This function answers for ROOT resources;
 * a caller holding a nonzero parent must not use it.
 */
#ifndef OVMX_VMS_DLM_HASH_H
#define OVMX_VMS_DLM_HASH_H

#include "vms_wire_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The name field is 31 bytes on the wire (vms_cluster_codec_dlm.h). */
#define VMS_DLM_HASH_NAME_MAX   31u

/*
 * The two determined constants. They are DATA, derived from the corpus named
 * in this file's provenance note; a change to either invalidates every row of
 * the held-out proof, so neither may be "tuned".
 */
#define VMS_DLM_HASH_ROT        9u
#define VMS_DLM_HASH_MULT       0xa53f19b7u

enum vms_dlm_hash_status {
	VMS_DLM_HASH_OK       = 0,
	VMS_DLM_HASH_E_INVAL  = 1,  /* null name or null out                  */
	VMS_DLM_HASH_E_RANGE  = 2   /* name_len outside 1..VMS_DLM_HASH_NAME_MAX */
};

/*
 * The hash of a ROOT resource identity. `*out` is written ONLY on
 * VMS_DLM_HASH_OK: "this node could not compute a value" and "the value is 0"
 * are different facts and only one of them may ever reach the wire (INV-6).
 *
 * Pure: no allocation, no clock, no seam call, no library call -- so it runs
 * identically in vms.ko, in vms.kmod, in the host unit tests and in the
 * rung-2 N-node simulator.
 */
enum vms_dlm_hash_status vms_dlm_name_hash(uint16_t group, uint8_t mode,
					   const uint8_t *name,
					   uint32_t name_len,
					   uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_DLM_HASH_H */
