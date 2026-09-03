/* SPDX-License-Identifier: GPL-2.0 */
/*
 * replay.h - the FC-P1.5 pcap replay driver: feed captured VAX frames to ONE
 * simulated OVMX `pe` instance and assert its emitted frames against the
 * reference joiner's shape, allowlist membership, and send_seq contiguity.
 *
 * Design docs/design-faithful-cluster-executive.md §3.9 rung 2: "feed the
 * captured VAX frames to one simulated OVMX instance and assert its emitted
 * frames against the reference joiner's" (shape + allowlist), plan row
 * FC-P1.5. test_pe_formation.c already does exactly this at the raw
 * pe_fsm_rx/pe_fake_ops level for ONE frame class (HELLO channel formation);
 * this file is the GENERIC, reusable driver -- any ordered sequence of
 * captured/decoded specimens, through the FC-P1.4 sim harness -- that the
 * CMakeLists here already reserves a place for ("FC-P1.5's pcap replay
 * driver (replay.c) joins cluster_sim_core's source list on the same
 * terms").
 *
 * STRUCTURE-TOLERANT, NOT BYTE-IDENTICAL. OVMX legitimately diverges from the
 * captured reference node: its own SCSNODE/software version/hardware type
 * (the honest-identity ruling, sim_node.h §1), its own send_seq numbering,
 * LOCKDIRWT=0, and so on. This driver therefore never byte-diffs an emitted
 * frame against a captured one. What it asserts is STRUCTURE:
 *
 *   1. SHAPE           every emitted frame classifies, through the real
 *                       codec, as a GROUNDED class with a coherent §2 length
 *                       identity.
 *   2. ALLOWLIST        every emitted SCS_MSG-class frame's (category,
 *                       opcode) is a row some GROUNDED allowlist table
 *                       carries -- OVMX answered nothing ungrounded (spec
 *                       §4(p), the rule that saved two VAXes).
 *   3. SEQ CONTIGUITY   OVMX's own send_seq values, across everything it
 *                       emitted on this one circuit, advance by exactly one
 *                       with no gap and no duplicate.
 *
 * WHAT FEEDS IT. `docs/clean-room/tools` decoders and the `.spec` files
 * under `tests/cluster/host/fixtures/` are the SAME clean-room specimen
 * chain the R1 codec/pe tests already use (cluster_fixture.h) -- this
 * driver takes raw bytes and a label, so a caller loads fixtures however
 * the R1 tests do and hands the bytes straight through; it adds no new
 * decoder and no new fixture format.
 *
 * HOST-ONLY, never compiled into vms.ko or the NetBSD kmod (same footing as
 * the rest of tests/cluster/sim/).
 */
#ifndef OVMX_SIM_REPLAY_H
#define OVMX_SIM_REPLAY_H

#include <stddef.h>
#include <stdint.h>

#include "sim.h"
#include "vms_cluster_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A generous ceiling: a START-phase join replay is a handful of frames, and
 * a run that needs more is a scenario error the caller sees as -1, never a
 * silent truncation. */
#define REPLAY_MAX_EMITTED 64u

/* One captured/decoded specimen to feed in. `label` is carried only for a
 * report line -- never compared, never a gate. */
struct vms_replay_input {
	const uint8_t *bytes;
	uint32_t       len;
	const char    *label;
};

/* One frame OVMX itself transmitted in response, in emission order. */
struct vms_replay_emitted {
	uint8_t  bytes[SIM_FRAME_MAX];
	uint32_t len;
};

struct vms_replay_result {
	struct vms_replay_emitted frame[REPLAY_MAX_EMITTED];
	uint32_t n;
};

/*
 * Feed `in[0..n_in)` into `node`'s pe_fsm_rx, IN ORDER -- the SAME call
 * sim_dispatch_delivery makes, so this is "the same path sim_lan delivers
 * on" and not a bespoke injection route. After EACH frame, every frame the
 * node's own `ops->send` handed to the virtual LAN (sim_ops_send ->
 * sim_lan_xmit) is drained out of `s->lan`'s queue, in emission order, and
 * appended to `out`. `node` must already be attached to `s` and booted.
 *
 * Returns 0 on success, -1 if `out` would overflow REPLAY_MAX_EMITTED (a
 * scenario needing more headroom, reported rather than silently truncated).
 */
int vms_replay_drive(struct sim *s, struct sim_node *node,
		     const struct vms_replay_input *in, uint32_t n_in,
		     struct vms_replay_result *out);

/*
 * Assertion 1: SHAPE. Classifies every emitted frame through the real codec
 * and counts how many are NOT a grounded, non-UNKNOWN class with a coherent
 * §2 length identity (EXACT or the documented RUNT_PAD -- never MISMATCH).
 * Returns 0 iff every frame shaped. Appends one line per failure to
 * `report` (may be NULL, `report_cap` 0).
 */
uint32_t vms_replay_check_shape(const struct vms_replay_result *r,
				char *report, size_t report_cap);

/*
 * Assertion 2: ALLOWLIST. For every emitted frame that classifies as
 * VMS_FCLS_SCS_MSG (the only class carrying a SYSAP category/opcode
 * envelope, §4(d)), strips the response bit (§4(j)) and looks the
 * (category, opcode) pair up across ALL of `tables`. This driver has no
 * per-connection SYSAP/CDT context to pick a single table the way the
 * executive would -- "grounded in ANY known SYSAP's allowlist" is what it
 * can honestly check at this rung; it is a deliberately looser check than
 * the per-SYSAP one test_codec_cm.c already proves, documented as such
 * rather than silently claimed to be it. A class with no SYSAP envelope
 * carries no allowlist obligation and is skipped (counted in `*skipped`,
 * may be NULL). Returns the count that failed to match any table.
 */
uint32_t vms_replay_check_allowlist(const struct vms_replay_result *r,
				    const struct vms_wire_allow_table *const *tables,
				    uint32_t n_tables, uint32_t *skipped,
				    char *report, size_t report_cap);

/*
 * Assertion 3: SEQ CONTIGUITY. Every VMS_FFAM_SCS-family class the registry
 * publishes carries VMS_FCAP_SEQ (vms_cluster_codec.c's class table);  this
 * walks every emitted frame's send_seq, in emission order, and asserts each
 * is exactly one more than the last -- no gap, no duplicate, no reorder, on
 * the single circuit this replay drives. Returns 1 iff contiguous (or fewer
 * than 2 sequenced frames were emitted -- nothing to be discontiguous
 * about), 0 otherwise, with the break reported.
 */
int vms_replay_check_seq_contiguous(const struct vms_replay_result *r,
				    char *report, size_t report_cap);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_SIM_REPLAY_H */
