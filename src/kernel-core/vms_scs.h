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
struct scs_sysap_ops {
	/*
	 * An inbound connect names this SYSAP. Return 0 to ACCEPT (and the CDT
	 * goes to OPEN once the verbs complete) or an SS$_ status to REJECT.
	 * `peer` and `peer_conid` identify the requester; `conndata` is the
	 * 16-byte SCA connect-data field (spec SS4(N)), passed through
	 * uninterpreted.
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
	 * was lost. `reason` is an SS$_ status. */
	void (*closed)(void *ctx, vms_conid_t local_conid, uint32_t reason);

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
 * answers times out rather than defaulting to either. */
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

#endif /* OVMX_VMS_SCS_H */
