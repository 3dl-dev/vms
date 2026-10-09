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
 * WHAT ROUTES ON IT, AND WHAT MAY NOT (rd vms-b5b0)
 *
 * It routes now. rd vms-c6e leg 2 ran on a real V7.3 VAX with the predictions
 * committed first (tests/lab/captures/vms-c6e-heldout-20261008): 55 of 55
 * pre-registered triples observed on the wire MATCHED, 0 mismatched, plus 541
 * live root values on the same capture. So `dir_resolve` (vms_lock.c) computes
 * the value for a root name this node is the first to touch and routes the
 * request to `ldwv[(value >> 16) mod n]` -- any LOCKDIRWT configuration, a real
 * VAX directory node included.
 *
 * WHAT MAY NOT. A WRONG hash on the wire is not a benign mistake: it makes the
 * directory node scan the wrong chain, conclude the name is unknown, and name
 * the sender as master -- the 35-per-second grant storm in memory
 * cluster-promotion-gap. So an identity OUTSIDE the proven coverage NEVER
 * REACHES A FRAME: `vms_dlm_name_hash_proven()` below is the one entry point
 * routing may use, and §"THE PROVEN COVERAGE" states exactly what it serves.
 *
 * AND WHAT HAPPENS TO THE $ENQ INSTEAD, because this is where the distinction
 * is load-bearing (rd vms-b5b0, the PR #1578 lab regression). Refusing the
 * CALLER is not the same act as withholding a FRAME, and for a while the engine
 * did both: a booted node with a cluster stack bound answered SS$_UNSUPPORTED
 * to its own ACP, 74 file operations failed and STARTUP.COM died on
 * `%RMS-E-FNF ... SYS$STARTUP:VMS$VMS.DAT`. Baron's ruling on rd vms-dc2 had
 * already judged that -- option (B), refusing, is "NEVER" -- so the resource is
 * MASTERED LOCALLY instead (option A, honestly labelled), the fallback is
 * COUNTED (vms_lock_dlm_dir_hash_uncovered) and the first one is ANNOUNCED on
 * the console. Nothing is asserted to anybody: a local grant is not a claim.
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
	VMS_DLM_HASH_E_RANGE  = 2,  /* name_len outside 1..VMS_DLM_HASH_NAME_MAX */
	/*
	 * The arithmetic is defined for this identity but NO VMS NODE HAS EVER
	 * BEEN WATCHED PRODUCING A VALUE FOR ONE LIKE IT -- see §"THE PROVEN
	 * COVERAGE". The answer would be an extrapolation, and this function's
	 * whole value is that it is not one.
	 */
	VMS_DLM_HASH_E_COVER  = 3
};

/* ===========================================================================
 * THE PROVEN COVERAGE, AS DATA
 *
 * Three axes of the identity block, and for each one the set of values a real
 * OpenVMS VAX has been OBSERVED to hash in front of us. The sets below are
 * read off the two proof artifacts and nothing else:
 *
 *   tests/cluster/fixtures/dlm_hash_*.tsv        1216 corpus rows + the
 *                                                m3-soledir forward prediction
 *   tests/lab/captures/vms-c6e-heldout-20261008  the DRIVEN run: 81 triples
 *                                                pre-registered before the VAX
 *                                                locked anything, 55 observed,
 *                                                55 matched, 0 mismatched
 *
 * THE READING OF THE EVIDENCE, STATED ONCE, BECAUSE THE AXES ARE NOT ALIKE.
 * Two of these fields are ENUMERATIONS and one is a MAGNITUDE, and a set of
 * observations means a different thing in each case:
 *
 *   ACCESS MODE and NAME LENGTH are enumerations -- four legal modes, 31 legal
 *     lengths -- so coverage is EXACT-VALUE MEMBERSHIP. An unobserved value of
 *     an enumeration is its own case and nothing about the observed ones
 *     carries to it.
 *   THE UIC GROUP is a magnitude, and rd vms-c6e leg 2 DROVE it as one: the
 *     run pre-registered names locked under UIC groups 1, 300 and 16382
 *     expressly to exercise group bits 0..13, and recorded "group bits 14/15"
 *     as the residual (the run README's table + rd vms-c6e's note). So
 *     coverage is the BIT SPAN of the observed groups -- every bit of a
 *     covered value was observed set -- which is that pre-registered reading
 *     and not a new one invented here.
 *
 * The ctest `cluster_host_test_dlm_hash` DERIVES all three constants from those
 * files under exactly those two rules, and fails if one claims coverage the
 * evidence does not carry or withholds coverage it does
 * (tests/cluster/host/test_dlm_hash.c §coverage). They are measurements, not
 * policy, and must not be widened by hand.
 *
 * WHAT IS OUTSIDE, AND WHY EACH ONE IS A REFUSAL RATHER THAN AN EXTRAPOLATION
 * (docs/design-dlm-name-hash.md §5):
 *
 *   SUPERVISOR MODE (2). Modes 0, 1 and 3 are observed; 2 never is -- no VMS
 *     component seen so far takes a lock at supervisor mode. The arithmetic
 *     would place mode bit 1 exactly where mode 3 places it, but what is
 *     unobserved is not the arithmetic: it is whether VMS QUALIFIES such a
 *     resource with that byte in that field at all.
 *   UIC GROUP >= 16384 (bits 14 and 15 of the group word). A group in the
 *     system range may well be qualified differently (group 0, like a
 *     LCK$M_SYSTEM name) -- unobserved, so unanswered.
 *   NAME LENGTHS 23 AND 29. Every other length 1..31 appears, in the corpus or
 *     in the driven run. These two WERE pre-registered in the driven run and
 *     the VAX did not put them on the wire (the run README's ABSENT list), so
 *     this is the axis where closing the gap is purely a lab errand: lock one
 *     23-character and one 29-character root name on a real VAX, score it with
 *     tools/cluster/dlm_hash/check_heldout_run.py, and widen the mask WITH the
 *     capture.
 *
 * An identity outside these sets is not routed and not refused: the lock
 * engine masters the resource on THIS NODE ONLY, counts it and says so once
 * (vms_lock.c, rd vms-b5b0). That is the pre-proof floor for the narrow set of
 * identities the proof does not cover -- not a new failure mode, and not a
 * cluster-wide claim.
 * =========================================================================== */

/* Bit `mode` set for each access mode observed. Modes 0, 1, 3. */
#define VMS_DLM_HASH_MODE_PROVEN   0x0bu

/* The BIT SPAN of the observed UIC groups (0, 1, 300, 16382 -> bits 0..13), so
 * a covered group is one every set bit of which was observed set. */
#define VMS_DLM_HASH_GROUP_PROVEN_MAX 16383u

/* Bit `name_len` set for each length observed. All of 1..31 but 23 and 29. */
#define VMS_DLM_HASH_LEN_PROVEN    0xdf7ffffeu

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

/*
 * IS THIS IDENTITY INSIDE THE PROVEN COVERAGE? VMS_DLM_HASH_OK yes;
 * VMS_DLM_HASH_E_RANGE for a name length outside 1..31; VMS_DLM_HASH_E_COVER
 * for an identity the three masks above do not carry.
 *
 * Separate from the hash so a diagnostic (and a test) can ask the question
 * without asking for a value, and so the one gate has one name.
 */
enum vms_dlm_hash_status vms_dlm_name_hash_coverage(uint16_t group, uint8_t mode,
						    uint32_t name_len);

/*
 * THE ONE ENTRY POINT ROUTING MAY USE: the coverage test, then the hash.
 * `*out` is written only on VMS_DLM_HASH_OK, so a caller that ignores the
 * status cannot route on an extrapolated value by omission.
 *
 * vms_dlm_name_hash() above stays available UNGATED because reproducing a
 * captured value is how the function is tested and how a capture is scored --
 * both of which must be able to evaluate identities the wire has not shown,
 * precisely to find out whether it ever does. Nothing that builds a frame may
 * call it: tools/ci/cluster_dlm_hash_gate.sh enforces that.
 */
enum vms_dlm_hash_status vms_dlm_name_hash_proven(uint16_t group, uint8_t mode,
						  const uint8_t *name,
						  uint32_t name_len,
						  uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_DLM_HASH_H */
