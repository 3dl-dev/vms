/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_mscp_cl_fsm.h - MSCP disk-client DISCOVERY FSM (plan item FC-P3.4).
 *
 * SCOPE. The class-driver discovery sequence a joiner runs on its
 * MSCP$DISK connection: SET CONTROLLER CHARACTERISTICS twice, then a GET
 * UNIT STATUS walk with the NEXT-UNIT modifier until an END returns status
 * Unit-Offline (the end-of-list terminator, not an error). DISCOVERY ONLY:
 * no ONLINE, no READ, no WRITE -- design docs/design-faithful-cluster-
 * executive.md and docs/cluster-protocol-spec.md sec 4(n) both ground that a
 * joiner's own MSCP walk during the join does exactly this and nothing
 * more ("Nothing else -- there is no MSCP INIT handshake before it ... and
 * no ONLINE or READ after it. The joiner never mounts or reads the disk
 * during the join."). FC-P3.3 (the join FSM) drives this FSM to enumerate
 * served disks; it does not itself decide when the MSCP$DISK connection is
 * open or closed.
 *
 * THIS FSM BUILDS NO ENVELOPE AND OWNS NO CONNECTION. Every command frame
 * needs the live MSCP$DISK connection's own SCS envelope state (recv_ack,
 * send_seq, the Con.ID pair, the Ethernet/logical addresses) -- READ from
 * the real connection by the caller and passed in as a `struct
 * vms_mscp_link` (design SS3.9 rule 1, and the executive-backed-not-wire-
 * plumbing discipline: never plumb a template or a stale frame's bytes
 * into a new one). This file only ever fills in the two things a
 * disk-client discovery walk actually decides for itself: which MSCP
 * command to send next, and which unit number to query -- both derived
 * from FSM state and the peer's own prior answers, never fabricated.
 *
 * P.TIME (SCC's own live-time field) is likewise caller-supplied: sec 6.16
 * defines it as the host's current VMS absolute time (or 0), reading the
 * clock is a seam call (exec_time_now_vms), and design SS3.9 rule 4 bars a
 * pure _fsm.c TU from calling the seam directly (enforced by
 * tools/ci/cluster_core_includes_gate.sh RULE4) -- so the caller (which has
 * the ops the join FSM already carries) reads it and hands it in.
 *
 * THE WALK, GROUNDED (docs/cluster-protocol-spec.md sec 4(n), the
 * af2-firsttimer-established-20260728.pcap / vax3-2to3-established-
 * 20260730.pcap captures):
 *
 *   1. SET CONTROLLER CHARACTERISTICS, twice -> each answered END 0x84,
 *      status SUCCESS.
 *   2. GET UNIT STATUS with MD.NXU (NEXT-UNIT). The FIRST command seeds
 *      unit word 0x0001 (seeding 0x0000 makes the server answer OFFLINE
 *      immediately and the enumeration terminates after one exchange --
 *      a silent, plausible-looking failure this FSM avoids by construction:
 *      VMS_MSCP_CL_GUS_SEED_UNIT is 1, never 0). Each subsequent command
 *      uses the PREVIOUS END's own returned unit word + 1 -- read from the
 *      peer's answer, never computed from a local counter that ignores it.
 *      The walk ends when an END returns status major Unit-Offline
 *      (VMS_MSCP_ST_OFFLINE): the end-of-list terminator, not an error.
 *
 * THE P.CRF (command reference) COMPOSITION. Sec 5.1 makes the whole
 * 32-bit P.CRF opaque -- "unique, non-zero", echoed verbatim, unique only
 * across the commands outstanding on one connection. The captured joiner's
 * own convention -- a per-command-class constant in the low word and an
 * incrementing message id in the high word -- is OBSERVED VMS behaviour
 * (docs/cluster-protocol-spec.md sec 4(n): "message id -- increments per
 * command, echoed verbatim"; src/vmsscs/include/scs_mscp.h's own citation
 * of the same af2 capture), not a protocol requirement; this FSM reproduces
 * the observation, byte-exact for the first command of each class against
 * the af2 golden vectors (VMS_MSCP_CL_SCC_MSGID0 / VMS_MSCP_CL_GUS_MSGID0),
 * and keeps incrementing thereafter -- never colliding, never a bare zero.
 *
 * WHAT IS DELIBERATELY LEFT TO THE P6.2 CODEC'S OWN, ALREADY-DOCUMENTED
 * GAP. vms_cluster_codec_mscp.h's struct vms_mscp_link doc names one
 * ungrounded span, abs[36,72) content[22:24) among it -- the strawman's
 * "incarnation" byte the af2 golden command frames happen to carry as
 * 0x0001. This FSM does not reach around the frozen P6.2 seam to add it;
 * INV-6 prefers an honest, already-documented omission over a builder
 * quietly source-of-truth-hopping past what the codec it is layered on
 * top of actually grounds. R1's byte-exact assertions are scoped to the
 * MSCP MESSAGE itself (content[58:94), "100% field-built" per
 * src/vmsscs/scs_mscp.c's own division-of-labour comment) for exactly this
 * reason.
 *
 * PURE TU (design SS3.9, gate RULE4): no seam call, no allocation, no
 * clock. Every field this file supplies comes from FSM state (the two
 * message-id counters, the walk cursor) or the caller's own arguments.
 *
 * INCLUDES: kernel-core headers only
 * (tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_MSCP_CL_FSM_H
#define OVMX_VMS_MSCP_CL_FSM_H

#include "vms_cluster_codec_mscp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The P.CRF class-token / message-id seeds, GROUNDED
 * af2-firsttimer-established-20260728.pcap (see file header). class_token
 * is the OVMX-observed low word of P.CRF, msgid0 the seed for the high
 * word; both are a client-side naming convention this FSM reproduces, not
 * a byte this item invents.
 * ------------------------------------------------------------------ */
#define VMS_MSCP_CL_SCC_CLASS  0x0002u
#define VMS_MSCP_CL_GUS_CLASS  0x0001u
#define VMS_MSCP_CL_SCC_MSGID0 0x81a3u
#define VMS_MSCP_CL_GUS_MSGID0 0x7ee2u

/* Compose one P.CRF from the observed (class token, message id) pair --
 * the same arithmetic src/vmsscs/include/scs_mscp.h names
 * SCS_MSCP_CMD_REF(), reproduced here as this item's own, independent
 * expression of the same observed convention. */
#define VMS_MSCP_CL_CMD_REF(class_token, msg_id) \
	((((uint32_t)(uint16_t)(msg_id)) << 16) | (uint32_t)(uint16_t)(class_token))

/* sec 4(n): "The first command seeds unit word 0x0001." Never 0 -- see the
 * file header's "silent, plausible-looking failure" note. */
#define VMS_MSCP_CL_GUS_SEED_UNIT 0x0001u

/* ------------------------------------------------------------------ *
 * The state ladder. One outstanding command at a time: a build_* call is
 * refused (VMS_CODEC_E_CLASS) unless the FSM is in the state that expects
 * it, and an on_*_end call is refused unless the FSM is actually waiting
 * for that class's answer -- so an out-of-order or duplicate response
 * cannot silently advance the walk.
 * ------------------------------------------------------------------ */
enum vms_mscp_cl_state {
	VMS_MSCP_CL_ST_INIT = 0,  /* nothing sent yet: build_scc -> SCC #1  */
	VMS_MSCP_CL_ST_SCC1_SENT, /* SCC #1 outstanding                     */
	VMS_MSCP_CL_ST_SCC1_DONE, /* SCC #1 answered: build_scc -> SCC #2   */
	VMS_MSCP_CL_ST_SCC2_SENT, /* SCC #2 outstanding                     */
	VMS_MSCP_CL_ST_GUS_READY, /* both SCCs done: build_gus -> the walk  */
	VMS_MSCP_CL_ST_GUS_SENT,  /* a GUS command outstanding               */
	VMS_MSCP_CL_ST_DONE,      /* OFFLINE terminator seen: walk complete */
	VMS_MSCP_CL_ST_ERROR      /* an out-of-sequence or mismatched answer*/
};

/*
 * One unit the walk found: every field is read out of the peer's own GUS
 * END message (vms_mscp_gus_end), never fabricated. Reported to the caller
 * on each non-terminal vms_mscp_cl_fsm_on_gus_end() (INV-6: the caller
 * decides what to do with a real served unit; this FSM only enumerates).
 */
struct vms_mscp_cl_unit {
	uint16_t unit;         /* P.UNIT the END actually returned          */
	uint16_t unit_flags;   /* P.UNFL, Table A-5                         */
	uint64_t unit_id;      /* P.UNTI                                    */
	uint32_t media_id;     /* P.MEDI                                    */
	unsigned status_major; /* sec 5.6 split; AVAILABLE on a real unit   */
};

/*
 * struct vms_mscp_cl_fsm - the whole of this FSM's state. No globals
 * (design SS3.9 rule 3): every instance is one caller's one MSCP$DISK
 * discovery walk.
 */
struct vms_mscp_cl_fsm {
	enum vms_mscp_cl_state state;
	uint16_t scc_msgid;      /* next SCC message id to send             */
	uint16_t gus_msgid;      /* next GUS message id to send             */
	uint16_t next_unit;      /* next GUS unit word to query (the walk
				   * cursor: seeded 1, then peer's own
				   * returned unit + 1 each step)             */
	uint32_t pending_cmd_ref; /* the P.CRF of the outstanding command,
				   * checked against the answer's own echo   */
	unsigned units_found;    /* count of non-terminal GUS ENDs consumed */
};

/* Reset to the start of a fresh discovery walk. Builds and sends nothing. */
void vms_mscp_cl_fsm_init(struct vms_mscp_cl_fsm *f);

/* True once the walk has consumed its OFFLINE terminator. */
int vms_mscp_cl_fsm_done(const struct vms_mscp_cl_fsm *f);

/*
 * vms_mscp_cl_fsm_build_scc - build the next outbound SET CONTROLLER
 * CHARACTERISTICS command (state INIT or SCC1_DONE only; any other state
 * is refused with VMS_CODEC_E_CLASS). `link` is the live MSCP$DISK
 * connection's own envelope state (see file header); `ctlr_flags`,
 * `host_timeout` and `time` are the SET CONTROLLER CHARACTERISTICS
 * parameter fields sec 6.16 defines -- all caller-supplied, never a
 * default this file invents. On success writes the full frame (link
 * prefix + MSCP body) to `frame`/`cap`, sets `*written` to the total frame
 * length, advances the per-class message-id counter and the state.
 */
vms_codec_status_t vms_mscp_cl_fsm_build_scc(struct vms_mscp_cl_fsm *f,
					     const struct vms_mscp_link *link,
					     uint16_t ctlr_flags,
					     uint16_t host_timeout,
					     uint64_t time,
					     uint8_t *frame, uint32_t cap,
					     uint32_t *written);

/*
 * vms_mscp_cl_fsm_on_scc_end - consume a received SET CONTROLLER
 * CHARACTERISTICS end message (state SCC1_SENT or SCC2_SENT only). Refuses
 * (VMS_CODEC_E_CLASS) a frame that does not classify as an SCC end, or
 * whose echoed P.CRF does not match the outstanding command. Advances
 * SCC1_SENT -> SCC1_DONE (ready for the second SCC) or SCC2_SENT ->
 * GUS_READY (ready for the GET UNIT STATUS walk).
 */
vms_codec_status_t vms_mscp_cl_fsm_on_scc_end(struct vms_mscp_cl_fsm *f,
					      const uint8_t *frame,
					      uint32_t len);

/*
 * vms_mscp_cl_fsm_build_gus - build the next GET UNIT STATUS command in
 * the NEXT-UNIT walk (state GUS_READY only). `link` as above. The unit
 * word queried is `f->next_unit` (seeded VMS_MSCP_CL_GUS_SEED_UNIT, then
 * the previous END's own returned unit + 1 -- see file header).
 */
vms_codec_status_t vms_mscp_cl_fsm_build_gus(struct vms_mscp_cl_fsm *f,
					     const struct vms_mscp_link *link,
					     uint8_t *frame, uint32_t cap,
					     uint32_t *written);

/*
 * vms_mscp_cl_fsm_on_gus_end - consume a received GET UNIT STATUS end
 * message (state GUS_SENT only). Refuses a frame that does not classify
 * as a GUS end, or whose echoed P.CRF does not match the outstanding
 * command.
 *
 * On status major Unit-Offline: the walk is complete. `*is_terminator` is
 * set to 1, `unit` is left untouched (there is no unit to report -- INV-6:
 * never synthesize one), and state becomes DONE.
 *
 * Otherwise: `*is_terminator` is set to 0, `*unit` is filled from the
 * peer's own answer, `f->next_unit` becomes `unit->unit + 1`, state
 * returns to GUS_READY so the caller can drive the next step of the walk.
 */
vms_codec_status_t vms_mscp_cl_fsm_on_gus_end(struct vms_mscp_cl_fsm *f,
					      const uint8_t *frame, uint32_t len,
					      struct vms_mscp_cl_unit *unit,
					      int *is_terminator);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_MSCP_CL_FSM_H */
