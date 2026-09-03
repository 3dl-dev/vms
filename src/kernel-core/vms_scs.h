/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_scs.h - System Communication Services: the SYSAP registry, the CDT
 * connection ladder, and credit accounting (FC-P0.1).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.2 (SB/CDT/CDL, the
 * connection FSM, the credit ledger, SCS$DIRECTORY), SS3.4 (data model), SS3.9.
 * Wire spec: docs/cluster-protocol-spec.md SS4(h), SS4(m), SS4(N), SS4(t).
 *
 * THE LAYER IN ONE PARAGRAPH. SCS turns a virtual circuit between two SYSTEMS
 * into named CONNECTIONS between two SYSAPs. It keeps one SB per remote system
 * and one CDT per connection, allocates local Con.IDs, walks the connect verbs,
 * accounts a credit ledger per CDT, and dispatches an arriving message to the
 * CDT's own message-input routine. `SCS$DIRECTORY` is itself a SYSAP over the
 * registry: it answers a lookup with a HIT for a locally registered name and
 * with the literal "NOT PRESENT HERE" otherwise.
 *
 * WHY THE LEDGER IS IN THE INTERFACE. In the strawman daemon the CDL delivery
 * path and the credit accounting were DEAD CODE -- data went around them. This
 * header makes that impossible to repeat: the only way to send is
 * scs_send_msg(), which spends a credit off a real CDT, and the only way to
 * receive is the SYSAP's `message` callback, which is reached through the CDT.
 *
 * WHERE THE IMPLEMENTATION IS (FC-P2.2). This header is the GLUE-facing
 * surface: `struct vms_scs` is the executive object vms_scs.c (FC-P2.4) owns,
 * and the functions below are its entry points. The state machine itself --
 * the SB set, the CDL/CDT ladder, the Con.ID allocator, the credit ledger and
 * the MTYPE dispatch -- is the PURE `struct scs_fsm` in vms_scs_fsm.{c,h},
 * host-testable with no kernel and no wire, exactly as vms_pe_fsm.h is the
 * pure half of vms_pe.h (the E9 precedent, docs/cluster-integration-notes.md).
 * Each function here is a one-line dereference into its scs_fsm_* twin; the
 * names below are NOT redefined there, because `struct vms_scs` is undefined
 * outside vms_scs.c and the two cannot share a name in one TU.
 *
 * THE TWIN OF EACH SERVICE, so the glue is a table and not a search:
 *   scs_sysap_listen   -> scs_fsm_listen        scs_accept  -> scs_fsm_accept
 *   scs_sysap_unlisten -> scs_fsm_unlisten      scs_reject  -> scs_fsm_reject
 *   scs_connect        -> scs_fsm_connect       scs_send_msg-> scs_fsm_send_msg
 *   scs_disconnect     -> scs_fsm_disconnect
 *   scs_return_credit  -> scs_fsm_return_credit
 *   scs_dir_lookup     -> scs_dir_inquire (vms_scs_dir.h -- a DIFFERENT name
 *                         on purpose: `struct scs_dir` and `struct vms_scs`
 *                         cannot share one in a TU that sees both, the same
 *                         reason pe_send_msg/pe_vc_send_msg differ, note E9).
 * FC-P2.3 completed every twin; FC-P2.4 owns vms_scs.c, which is where these
 * names come into existence.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_SCS_H
#define OVMX_VMS_SCS_H

#include "vms_cluster.h"
#include "vms_cluster_snapshot.h"

struct vms_scs;
struct vms_scs_fsm;

/* ==========================================================================
 * 1. The SCS message types
 *
 * PUBLISHED, not inferred: the enum is the one
 * `LIBRARY/MACRO/EXTRACT=$SCSDEF SYS$LIBRARY:LIB.MLB` prints on a VAX -- a
 * published $xxxDEF definition macro, the class of source CLAUDE.md Rule 8
 * explicitly permits (docs/cluster-protocol-spec.md, the PPD/SCSDEF oracle
 * table). It independently confirms values 0..7, which until that extract
 * rested on OVMX's own captures, and extends past 7. It is quoted here rather
 * than re-derived from a pcap.
 * ========================================================================== */
enum scs_mtype {
	SCS_MTYPE_CON_REQ  = 0,
	SCS_MTYPE_CON_RSP  = 1,
	SCS_MTYPE_ACCP_REQ = 2,
	SCS_MTYPE_ACCP_RSP = 3,
	SCS_MTYPE_REJ_REQ  = 4,
	SCS_MTYPE_REJ_RSP  = 5,
	SCS_MTYPE_DISC_REQ = 6,
	SCS_MTYPE_DISC_RSP = 7,
	SCS_MTYPE_CR_REQ   = 8,   /* the credit request of the 8/9 pair */
	SCS_MTYPE_CR_RSP   = 9,
	SCS_MTYPE_APPL_MSG = 10,
	SCS_MTYPE_APPL_DG  = 11,
	SCS_MTYPE__COUNT
};

/* ==========================================================================
 * 2. Timers and the injected ops
 * ========================================================================== */
enum scs_timer {
	SCS_TIMER_CONNECT = 0,   /* a connect verb awaiting its response */
	SCS_TIMER_DISCONNECT = 1,/* a DISCONNECT awaiting its match */
	SCS_TIMER__COUNT
};

struct scs_ops {
	/* Send one SCS message body to a remote system, addressed to the peer's
	 * Con.ID. Production: pe_send_msg. Returns 0 or an SS$_ status. */
	int  (*send)(void *ctx, vms_scs_sysid_t dst, vms_conid_t dst_conid,
		     const uint8_t *body, uint32_t len);
	/* Send a datagram (no connection). Production: pe_send_dg. */
	int  (*send_dg)(void *ctx, vms_scs_sysid_t dst,
			const uint8_t *body, uint32_t len);

	void (*arm_timer)(void *ctx, enum scs_timer which, uint32_t key, uint32_t ms);
	void (*cancel_timer)(void *ctx, enum scs_timer which, uint32_t key);

	uint32_t (*now_ms)(void *ctx);
	void     (*log)(void *ctx, const char *msg);
	void    *(*alloc)(void *ctx, uint32_t n);
	void     (*free)(void *ctx, void *p);

	void *ctx;
};

/* ==========================================================================
 * 3. The event vocabulary (table-driven: handlers[cdt_state][event])
 *
 * One event per connect verb, plus the local requests and the circuit facts.
 * FC-P2.2 owns the table. The CDT STATES the table indexes live in
 * vms_cluster_snapshot.h (enum vms_scs_cdt_state), because SDA prints them and a
 * test asserts on the same values a diagnostic reports.
 * ========================================================================== */
enum scs_event {
	SCS_EV_RX_CON_REQ    = 0,
	SCS_EV_RX_CON_RSP    = 1,
	SCS_EV_RX_ACCP_REQ   = 2,
	SCS_EV_RX_ACCP_RSP   = 3,
	SCS_EV_RX_REJ_REQ    = 4,
	SCS_EV_RX_REJ_RSP    = 5,
	SCS_EV_RX_DISC_REQ   = 6,
	SCS_EV_RX_DISC_RSP   = 7,
	SCS_EV_RX_CR_REQ     = 8,   /* the special credit message */
	SCS_EV_RX_CR_RSP     = 9,
	SCS_EV_RX_APPL_MSG   = 10,
	SCS_EV_RX_APPL_DG    = 11,

	SCS_EV_LOCAL_CONNECT = 12,  /* a SYSAP asked to connect */
	SCS_EV_LOCAL_ACCEPT  = 13,
	SCS_EV_LOCAL_REJECT  = 14,
	SCS_EV_LOCAL_DISCONNECT = 15,
	SCS_EV_LOCAL_SEND    = 16,

	SCS_EV_TIMER_CONNECT = 17,
	SCS_EV_TIMER_DISCONNECT = 18,

	SCS_EV_VC_UP         = 19,  /* the port formed a circuit to this system */
	SCS_EV_VC_DOWN       = 20,  /* ... and lost it: every CDT on it closes */

	SCS_EV__COUNT
};

/* ==========================================================================
 * 4. The SYSAP interface
 *
 * A SYSAP (system application) is a named service -- `VMS$VAXcluster`,
 * `MSCP$DISK`, `SCS$DIRECTORY` -- that LISTENs on its name, ACCEPTs or rejects
 * inbound connects, SENDs on an open connection and RETURNs CREDIT for what it
 * received. In OVMX a SYSAP is an executive component (CNXMAN, the DLM arm, the
 * MSCP server), never a process: nothing in userspace can register one, because
 * a process cannot hold the executive state a SYSAP's answers must project.
 *
 * Names are the 16-byte blank-padded ASCII fields the wire carries; the caller
 * passes exactly VMS_SCS_PROCNAME_LEN bytes and the registry does not
 * NUL-terminate them.
 * ========================================================================== */
/*
 * The one non-status return value connect_req may make: "I have taken the
 * request and will answer later with scs_accept()/scs_reject()". VMS's own
 * shape -- the listening CDT stays in CONNECT RECEIVED until the SYSAP
 * answers (*VAXcluster Principles* ch. 2, "SCS Directory Entries and
 * Listening CDTs"). It is deliberately a value no SS$_ status can collide
 * with (SS$_ statuses are positive odd/even severity-encoded longwords in the
 * low range; this is a distinct sentinel checked BEFORE the status reading).
 */
#define SCS_CONNECT_DEFER (-1)

struct scs_sysap_ops {
	/*
	 * An inbound connect names this SYSAP. Return 0 to ACCEPT (and the CDT
	 * goes to OPEN once the verbs complete), SCS_CONNECT_DEFER to answer
	 * later, or an SS$_ status to REJECT.
	 * `peer` and `peer_conid` identify the requester; `conndata` is the
	 * 16-byte SCA connect-data field (spec SS4(N)), passed through
	 * uninterpreted.
	 *
	 * `local_conid` is the LISTENING CDT's Con.ID, not the connection's:
	 * VMS allocates the connection's own CDT only when the SYSAP ACCEPTS
	 * (ch. 2, "a separate CDT (distinct from the listening CDT) is
	 * allocated ... the local CONID used on the target node is the CONID of
	 * this separate CDT"). The SYSAP learns the connection's Con.ID from
	 * opened(). scs_accept()/scs_reject() take the LISTENING Con.ID, which
	 * is what this callback was handed.
	 */
	int  (*connect_req)(void *ctx, vms_conid_t local_conid,
			    vms_scs_sysid_t peer, vms_conid_t peer_conid,
			    const uint8_t *conndata, uint32_t conndata_len);

	/* The connection reached OPEN (either half). */
	void (*opened)(void *ctx, vms_conid_t local_conid);

	/* One application message arrived on this connection. The callee owns
	 * the bytes only for the duration of the call. Returning nonzero means
	 * "not consumed" and is counted, never silently dropped. */
	int  (*message)(void *ctx, vms_conid_t local_conid,
			const uint8_t *body, uint32_t len);

	/* The connection closed: DISCONNECT matched, rejected, or the circuit
	 * was lost. `reason` is an SS$_ status.
	 *
	 * THIS IS THE `disconnected()` DESIGN SS3.2.5 NAMES. The VC-break
	 * contract ("calls each SYSAP's disconnected(local_conid, reason)") and
	 * this callback are ONE notification, not two: a SYSAP is told once
	 * that a connection is gone, with the reason that took it. The FC-P0.1
	 * spelling is kept because it is the frozen one and because "closed"
	 * covers every way a connection ends -- a matched DISCONNECT, a
	 * REJECT, and a lost path -- which is exactly the set the callee must
	 * handle identically. On a VC break `reason` maps from
	 * SCS_CLOSE_PATHLOST (vms_scs_fsm.h) to SS$_PATHLOST in the glue.
	 *
	 * CONFIRMED (integration note E29, FC-P3.8): CNXMAN's `VMS$VAXcluster`
	 * SYSAP (vms_cnxman.c) is the first SYSAP that acts on `reason`, and it
	 * receives the RAW `enum scs_close_reason` exactly as this note always
	 * said -- never an SS$_ number. It routes SCS_CLOSE_REJECTED to
	 * cnxman_join_rejected() (book p. 2-25's version-gate reject, D12, a
	 * distinct fact from every other close) and every other reason to the
	 * CSB ten-state ladder's own connectivity-lost handling before telling
	 * the join. No wording change to the disposition above was needed.
	 */
	void (*closed)(void *ctx, vms_conid_t local_conid, uint32_t reason);

	/*
	 * OPTIONAL (may be NULL). A message this SYSAP handed to scs_send_msg()
	 * was placed in CREDIT WAIT -- *VAXcluster Principles* ch. 2's own
	 * mechanism: with no Send Credit the operation is queued to the CDT
	 * until the count rises -- and then could NOT be sent, because the
	 * virtual circuit was lost underneath it. `reason` is an SS$_ status
	 * (SS$_PATHLOST for a VC break).
	 *
	 * SCS NEVER RETRIES IT (design SS3.2.5). This callback exists so the
	 * failure is a fact the SYSAP is told, not a message that quietly
	 * evaporated -- and so nothing above SCS can mistake "queued" for
	 * "sent".
	 */
	void (*send_failed)(void *ctx, vms_conid_t local_conid, uint32_t reason);

	void *ctx;
};

/* Register a SYSAP under `name` (VMS_SCS_PROCNAME_LEN bytes, blank-padded).
 * `initial_credits` is what this SYSAP extends to a peer at connect time.
 * Returns 0, or an SS$_ status (already registered, table full). */
int scs_sysap_listen(struct vms_scs *scs, const uint8_t *name,
		     const struct scs_sysap_ops *ops, uint16_t initial_credits);

/* Withdraw a registration; open CDTs on it are disconnected. */
int scs_sysap_unlisten(struct vms_scs *scs, const uint8_t *name);

/* ==========================================================================
 * 5. Connection and data services
 * ========================================================================== */

/* Open a connection from local SYSAP `local_name` to `remote_name` on `dst`.
 * On success *out_conid is the CDT's local Con.ID -- the value the ALLOCATOR
 * minted (spec SS4(t): monotonic, high word reseeded per boot), never a value
 * reflected from the peer. Reflecting a peer's Con.ID is what produced
 * INCONSTATE on a real VAX; the allocator exists so it cannot happen. */
int scs_connect(struct vms_scs *scs, const uint8_t *local_name,
		const uint8_t *remote_name, vms_scs_sysid_t dst,
		vms_conid_t *out_conid);

/* Accept or reject an inbound connect that connect_req deferred. */
int scs_accept(struct vms_scs *scs, vms_conid_t local_conid);
int scs_reject(struct vms_scs *scs, vms_conid_t local_conid, uint32_t reason);

/* Disconnect an open connection (DISCONNECT-REQ; the ladder finishes on the
 * matching response). */
int scs_disconnect(struct vms_scs *scs, vms_conid_t local_conid, uint32_t reason);

/* Send one application message on an open CDT. SPENDS A CREDIT: returns
 * SS$_EXQUOTA-class failure rather than sending when the send credit is
 * exhausted -- the ledger is real, not decorative. */
int scs_send_msg(struct vms_scs *scs, vms_conid_t local_conid,
		 const uint8_t *body, uint32_t len);

/* Return `n` credits the local SYSAP has finished with. */
int scs_return_credit(struct vms_scs *scs, vms_conid_t local_conid, uint16_t n);

/* ==========================================================================
 * 6. Directory service (the SCS$DIRECTORY SYSAP, FC-P2.3)
 * ========================================================================== */

/* Ask `dst` whether it hosts `name`. The answer arrives asynchronously via
 * `cb`: `present` is nonzero for a HIT and zero for the wire's literal
 * "NOT PRESENT HERE" -- there is no third value, and a lookup that never
 * answers times out rather than defaulting to either.
 *
 * IMPLEMENTED (FC-P2.3) as scs_dir_inquire() over `struct scs_dir`
 * (vms_scs_dir.h); this is its glue spelling. "Times out rather than
 * defaulting to either" is executed literally there: an unanswered inquiry is
 * counted and dropped WITHOUT calling `cb`, because reporting silence as
 * absence would fabricate an answer the peer never gave (INV-6). */
typedef void (*scs_dir_result_cb)(void *ctx, vms_scs_sysid_t from,
				  const uint8_t *name, int present);

int scs_dir_lookup(struct vms_scs *scs, vms_scs_sysid_t dst, const uint8_t *name,
		   scs_dir_result_cb cb, void *cb_ctx);

/* ==========================================================================
 * 7. Lifecycle and readback (glue, vms_scs.c -- FC-P2.4)
 * ========================================================================== */
int  vms_scs_start(struct vms_cluster *cl);
void vms_scs_stop(struct vms_cluster *cl);

int vms_scs_snapshot(struct vms_cluster *cl, struct vms_scs_view *out);
int vms_scs_cdt_snapshot(struct vms_cluster *cl, uint32_t index,
			 struct vms_scs_cdt_view *out);

/* ==========================================================================
 * 8. Peer enumeration -- WHICH systems the port has a circuit to (FC-P3.9)
 *
 * SCS never opens a connection on its own; the SYSAP does (design SS3.2.5).
 * But a SYSAP cannot connect to a system it has no way of NAMING, and the only
 * place a peer's SCSSYSTEMID is ever learned is the port's own vc_up -- which
 * SCS records on that system's SB and nothing above SCS could see. This is
 * that seam, and it is the whole of integration note E36's fix: CNXMAN sweeps
 * it on its own beat and allocates a CSB for each system found (p. 7-23's
 * "newly discovered connection manager").
 *
 * Returns SS$_NORMAL and fills *out_sysid for the `index`-th SB with an OPEN
 * circuit; SS$_NOSUCHDEV for a free slot, an SB whose circuit is DOWN, an
 * index past the table, or SCS not started. The caller therefore sweeps the
 * whole 0..VMS_CLUB_MAX_CSB-1 range and SKIPS the refusals -- a down circuit
 * in the middle of the table is not the end of it. Sysids are reported in
 * SB-table order, which is discovery order.
 *
 * INV-6: this reports ONLY systems the PORT genuinely formed a circuit to
 * (scs_fsm_vc_up() sets `sb->vc_up` from vms_pe_fsm.c's vc_notify_up, whose
 * peer_sysid was read off a real received frame). A system that was merely
 * heard from, or whose circuit has since gone down, is not reported.
 *
 * CALLED FROM THE CLUSTER FORK THREAD, like every other service in SS4/SS5/SS6
 * above -- it does NOT take the fork mutex, so a caller that is not on the
 * fork context must take it (the two snapshots in SS7 are the exception, and
 * they say so).
 * ========================================================================== */
int vms_scs_peer_at(struct vms_cluster *cl, uint32_t index,
		    vms_scs_sysid_t *out_sysid);

/* ==========================================================================
 * 9. Block-transfer completions -- the port's THIRD service, routed (FC-P6.3)
 *
 * A block transfer is NOT an SCS message. It names a BUFFER the local port
 * minted, not a Con.ID, so SCS has no CDT to demux it through and nothing to
 * interpret in it: this is a pass-through, and it exists only because the port
 * delivers upward through `struct pe_upper_ops`, which SCS owns (vms_pe.h SS4,
 * whose FC-P2.4 note already said "block_data stays NULL: the port's THIRD
 * service is a SYSAP fact FC-P6.x binds").
 *
 * ONE consumer per node, because a node has one MSCP server. A second call
 * replaces the registration and a NULL `cb` withdraws it; with none registered
 * a completion is COUNTED and dropped, never routed at a guess.
 *
 * `name` is OUR OWN buffer name -- a value this node's port minted and handed
 * to the peer -- so the consumer finds its request by something it created,
 * never by a value the peer chose (INV-6).
 * ========================================================================== */
typedef void (*vms_scs_block_cb)(void *ctx, uint32_t name, uint32_t offset,
				 uint32_t len, uint32_t bytes_remaining);

int vms_scs_set_block_consumer(struct vms_cluster *cl, vms_scs_block_cb cb,
			       void *cb_ctx);

#endif /* OVMX_VMS_SCS_H */
