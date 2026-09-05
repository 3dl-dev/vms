/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_scs_dir.h - the SCS Directory Service (`SCS$DIRECTORY`) and the SCS
 * Process Poller (`SCS$DIR_LOOKUP`): the two SYSAPs that let a node ask
 * another node whether a named SYSAP is listening there (plan item FC-P2.3).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.2 (SCS$DIRECTORY over
 * the SYSAP registry), SS3.9 (pure FSM, injected ops, no globals).
 * Wire spec: docs/cluster-protocol-spec.md SS4(h)(2)/(2a) (the lookup body,
 * the `"NOT PRESENT HERE"` literal, the request/response marker), SS4(L) (the
 * join's directory phase, both drive directions).
 * Published book: *VAXcluster Principles* (Davis 1993) SS2.11, pp. 2-48..2-51,
 * which is the whole model below and is cited by page in the .c file.
 *
 * ---------------------------------------------------------------------------
 * THE SERVICE IN ONE PARAGRAPH (p. 2-50)
 *
 * "SCA requires that there be an SCS Directory Service on each node that
 * answers 'Yes' or 'No' when asked if a particular SYSAP name is present in
 * its list of listening SYSAPs." That list is the SDIR queue LISTEN builds
 * (vms_scs_fsm.h SS7), so a HIT here is a read of the ONE registry and never a
 * second name store: the directory cannot answer "Yes" about a SYSAP that is
 * not registered at the instant it answers, which is exactly the guarantee the
 * asking node acts on when it opens a connection next (INV-6).
 *
 * ---------------------------------------------------------------------------
 * THE CONNECTION IS TRANSIENT, AND THAT IS THE PROTOCOL (p. 2-51)
 *
 * "Periodically, the Process Poller on VAX_A connects to the Directory Service
 * on NODE_X, and the Directory Service accepts the connection. The Process
 * Poller then sends messages ... After the Process Poller has received replies
 * to all of its inquiries, the Process Poller and Directory Service disconnect
 * from each other." So the client half here is a per-system lifecycle --
 * IDLE -> CONNECTING -> OPEN -> CLOSING -> IDLE -- driven by whether any
 * inquiry is still outstanding, NOT a connection held open for the life of the
 * cluster. Spec SS4(L)'s established-join capture shows the same shape from
 * both sides: the member opens a directory connection to the joiner, asks its
 * three names, and the connection does not survive the round.
 *
 * ---------------------------------------------------------------------------
 * WHAT A HIT CARRIES, AND WHAT THIS FILE REFUSES TO INVENT
 *
 * The 16-byte result field distinguishes the two answers, and only one of its
 * values is grounded: the literal `"NOT PRESENT HERE"` means No (spec
 * SS4(h)(2), directly observed ASCII). Everything else means Yes, and the
 * INTERNAL semantics of an affirmative result are spec SS4(h)(2) RE gap (c) --
 * NOT grounded. Two affirmative shapes are on record from real VAXes: an
 * `MSCP$DISK` hit echoing the queried name blank-padded, and a `VMS$VAXcluster`
 * hit carrying a 16-byte binary descriptor nobody has decoded.
 *
 * So this file NEVER bakes in a captured descriptor. A HIT carries:
 *   1. the 16 bytes the SYSAP THAT OWNS THE NAME declared through
 *      scs_fsm_sysap_set_dir_data() -- a read of real registry state, the
 *      route by which (say) the Connection Manager will supply its own
 *      version descriptor when that field is grounded; or
 *   2. failing that, the REGISTERED NAME ITSELF, which is a read of the same
 *      registry entry and reproduces the one affirmative specimen whose bytes
 *      are explainable (the `MSCP$DISK` hit).
 * Copying the undecoded `VMS$VAXcluster` blob out of a capture would be a
 * template constant asserted as executive state, which is the failure mode
 * INV-6 exists to stop.
 *
 * ---------------------------------------------------------------------------
 * A TIMED-OUT INQUIRY IS NOT A "No"
 *
 * scs_dir_result_cb is called with `present` nonzero for a HIT and zero for
 * the wire's literal negative -- both are answers a peer really gave. An
 * inquiry that is never answered is COUNTED and dropped, and the callback is
 * NOT invoked: reporting silence as absence would fabricate a peer's answer,
 * and the book's own poller reports nothing on absence either (p. 2-51,
 * "SYSAP_B is not notified about the absence of SYSAP_W"). The poll repeats;
 * that is the recovery.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_SCS_DIR_H
#define OVMX_VMS_SCS_DIR_H

#include "vms_scs_fsm.h"

/* ==========================================================================
 * 1. The two SYSAP names (PUBLISHED, p. 2-51: "The SCS Directory Service is
 * called SCS$DIRECTORY, and the SCS Process Poller is called SCS$DIR_LOOKUP")
 * -- 16-byte BLANK-padded fields, the shape spec SS4(h)(2) observes on the
 * wire ("SCS$DIRECTORY   " in the 16-byte field at payload [62:78]).
 * ========================================================================== */
extern const uint8_t scs_dir_name_directory[VMS_SCS_PROCNAME_LEN];
extern const uint8_t scs_dir_name_lookup[VMS_SCS_PROCNAME_LEN];

/* Blank-pad an ASCII SYSAP name into the 16-byte wire field. NUL padding is a
 * different name to a real VAX, so this exists rather than each caller
 * re-deciding. Truncates nothing: a name longer than the field is refused. */
int scs_dir_name_pad(uint8_t *out, const char *ascii);

/* ==========================================================================
 * 2. The injected ops -- this module's ONLY route to the world
 *
 * `sysap_lookup` is the REGISTRY read (scs_fsm_sysap_lookup); the rest are the
 * SCS services (scs_fsm_connect / _send_msg / _return_credit / _disconnect).
 * The glue binds them in one place; nothing here holds a struct scs_fsm.
 * ========================================================================== */
struct scs_dir_ops {
	/* Is `name` in the local list of listening SYSAPs? 0 = yes and *out
	 * filled from the SDIR entry; non-zero = no. */
	int (*sysap_lookup)(void *ctx, const uint8_t *name,
			    struct scs_sysap_info *out);

	/* Open a directory connection to `dst` as SCS$DIR_LOOKUP. `sysap` is
	 * this module's client callback table. */
	int (*connect)(void *ctx, vms_scs_sysid_t dst,
		       const struct scs_sysap_ops *sysap,
		       uint16_t initial_credits, vms_conid_t *out_conid);

	/* Send one directory message body (SCS_DIR_BODY_LEN bytes). */
	int (*send)(void *ctx, vms_conid_t conid,
		    const uint8_t *body, uint32_t len);

	int (*return_credit)(void *ctx, vms_conid_t conid, uint16_t n);
	int (*disconnect)(void *ctx, vms_conid_t conid);

	uint32_t (*now_ms)(void *ctx);
	void     (*log)(void *ctx, const char *msg);

	void *ctx;
};

/* ==========================================================================
 * 3. The client-half state machine (p. 2-51's transient connection)
 * ========================================================================== */
enum scs_dir_state {
	SCS_DIR_IDLE = 0,      /* no directory connection to this system     */
	SCS_DIR_CONNECTING,    /* our CONNECT is outstanding                 */
	SCS_DIR_OPEN,          /* inquiries may go out                       */
	SCS_DIR_CLOSING,       /* the round is done; the connection is going */
	SCS_DIR_STATE__COUNT
};

enum scs_dir_event {
	SCS_DIR_EV_LOOKUP = 0, /* a local SYSAP asked about a name           */
	SCS_DIR_EV_OPENED,     /* SCS says the connection reached OPEN       */
	SCS_DIR_EV_RESPONSE,   /* a directory RESPONSE arrived on it         */
	SCS_DIR_EV_CLOSED,     /* SCS says the connection is gone            */
	SCS_DIR_EV_TICK,       /* the clock moved (scs_dir_tick)             */
	SCS_DIR_EV__COUNT
};

const char *scs_dir_state_name(enum scs_dir_state s);

/* ==========================================================================
 * 4. The objects (the caller owns every table; this module allocates nothing)
 * ========================================================================== */

/* One remote system's transient directory connection. */
struct scs_dir_peer {
	uint8_t         in_use;
	uint8_t         state;        /* enum scs_dir_state                  */
	uint8_t         pad0[2];
	vms_scs_sysid_t sysid;
	vms_conid_t     conid;        /* 0 until SCS mints one               */
	uint32_t        outstanding;  /* inquiries SENT and not yet answered */
	uint32_t        queued;       /* inquiries not yet on the wire       */
	uint32_t        rounds;       /* transient connections completed     */
};

/* One inquiry: a name, who to tell, and when to stop waiting. */
struct scs_dir_inquiry {
	uint8_t           in_use;
	uint8_t           sent;
	uint8_t           pad0[2];
	uint32_t          peer_index;
	uint8_t           name[VMS_SCS_PROCNAME_LEN];
	scs_dir_result_cb cb;
	void             *cb_ctx;
	uint32_t          deadline_ms;
};

/*
 * Tunables. `lookup_timeout_ms` is an OVMX DESIGN VALUE, labelled as one: no
 * capture measures a directory-inquiry timeout, and PRCPOLINTERVAL (the
 * published poll period, p. 2-51) governs how often a poll REPEATS, not how
 * long one inquiry waits. The reference exchanges are machine-speed -- the
 * established-join capture answers all three lookups inside 3 ms -- so a
 * multi-second wait is a backstop, not a schedule.
 *
 * `credits` is what EACH half extends on a directory connection.
 *   - GROUNDED byte-exact for the CLIENT: spec SS4(h)(2a)'s frame table dumps
 *     the poller's `SCS$DIRECTORY` CONNECT_REQ and reads `[48:50]=3`, and
 *     SS4(d) pins that field as the Send Credits the connecting SYSAP extends.
 *   - The ACCEPTOR's value is NOT isolated (SS4(d)'s per-SYSAP list gives
 *     "SCS$DIRECTORY 3, SCS$DIR_LOOKUP 1" but its labelling convention is in
 *     tension with the byte-exact frame above, so which of the two the
 *     ACCEPT_REQ carries cannot be read off it). OVMX extends the same 3 from
 *     both halves and SAYS SO rather than picking the unattributed 1: at 3 a
 *     reference round of inquiries completes with each answer piggybacking the
 *     buffer the previous one freed and the p. 2-44 special credit message
 *     never firing, which is what the reference wire shows (SS4(h)(1g): zero
 *     type-8 frames in 440 367 at the published SCSFLOWCUSH default).
 */
#define SCS_DIR_LOOKUP_TIMEOUT_MS_DEFAULT 5000u
#define SCS_DIR_CREDITS_DEFAULT              3u

struct scs_dir_cfg {
	uint32_t lookup_timeout_ms;
	uint16_t credits;
	uint16_t pad0;
};

struct scs_dir {
	const struct scs_dir_ops *ops;
	struct scs_dir_cfg        cfg;

	/* The two SYSAP tables the glue hands to scs_fsm_listen()/_connect(). */
	struct scs_sysap_ops      server_ops;
	struct scs_sysap_ops      client_ops;

	struct scs_dir_peer      *peers;
	uint32_t                  n_peers;
	struct scs_dir_inquiry   *inq;
	uint32_t                  n_inq;

	/* ---- counters, every one a real event ---- */
	uint32_t srv_connects;        /* inbound directory connects accepted */
	uint32_t srv_hits;            /* answers this node gave as "Yes"     */
	uint32_t srv_misses;          /* ... and as "NOT PRESENT HERE"       */
	uint32_t srv_send_failed;     /* an answer SCS would not take        */
	uint32_t cli_rounds;          /* transient connections completed     */
	uint32_t cli_hits;
	uint32_t cli_misses;
	uint32_t cli_timeouts;        /* inquiries that got NO answer        */
	uint32_t cli_abandoned;       /* inquiries a lost circuit took       */
	uint32_t cli_connect_failed;
	uint32_t rx_malformed;        /* not a directory body at all         */
	uint32_t rx_unmatched;        /* an answer no inquiry was waiting on */
	uint32_t rx_wrong_role;       /* a request to the client half, or an
				       * answer to the server half            */
	uint32_t no_inquiry_slot;
	uint32_t no_peer_slot;
	uint32_t ignored_events;      /* [state][event] with no edge         */

	/* Scratch for ONE message body; never on the stack (the VAX kernel
	 * stack is small), never shared with a second in-flight build. */
	uint8_t  bodybuf[SCS_DIR_BODY_LEN];
};

/* ==========================================================================
 * 5. Lifecycle and binding
 * ========================================================================== */

/* Zero the context, bind the ops, build the two SYSAP tables. */
int scs_dir_init(struct scs_dir *d, const struct scs_dir_ops *ops);

int scs_dir_bind_peers(struct scs_dir *d, struct scs_dir_peer *p, uint32_t n);
int scs_dir_bind_inquiries(struct scs_dir *d, struct scs_dir_inquiry *q,
			   uint32_t n);

/* `cfg` NULL restores the documented defaults. */
void scs_dir_set_cfg(struct scs_dir *d, const struct scs_dir_cfg *cfg);

/* The tables the glue registers with SCS:
 *   scs_fsm_listen(f, scs_dir_name_directory, scs_dir_server_ops(d), n);
 * and the client table scs_dir passes to ops->connect itself. */
const struct scs_sysap_ops *scs_dir_server_ops(struct scs_dir *d);
const struct scs_sysap_ops *scs_dir_client_ops(struct scs_dir *d);

/* ==========================================================================
 * 6. The client service (vms_scs.h's scs_dir_lookup, at the pure level)
 *
 * Ask `dst` whether it hosts `name`. The answer arrives through `cb` with
 * `present` nonzero for a HIT and zero for the wire's literal negative. A
 * lookup that is never answered TIMES OUT and does not call back -- see the
 * file header. Opens the transient connection if there is not one already,
 * and closes it once nothing is outstanding.
 * ========================================================================== */
int scs_dir_inquire(struct scs_dir *d, vms_scs_sysid_t dst, const uint8_t *name,
		   scs_dir_result_cb cb, void *cb_ctx);

/* The clock moved: expire overdue inquiries and close a finished round. The
 * glue calls this from the same fork/callout beat the rest of the cluster
 * timers run on; nothing here reads a clock of its own. */
void scs_dir_tick(struct scs_dir *d);

/* Readback: the peer row for `sysid`, or NULL. */
struct scs_dir_peer *scs_dir_peer_by_sysid(struct scs_dir *d,
					   vms_scs_sysid_t sysid);

#endif /* OVMX_VMS_SCS_DIR_H */
