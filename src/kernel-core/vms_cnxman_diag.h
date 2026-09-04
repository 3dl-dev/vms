/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_diag.h - the JOIN's executive-resident TRANSITION RING (E69).
 *
 * Integration note E69 (docs/cluster-integration-notes.md). The connection
 * manager runs inside the executive and the executive has no console log
 * (src/ovmx_init/ovmx_init.c:1399 -- its output is not OPA0:), so THREE
 * consecutive promotion walls (E67, E68 and the E68 re-fire) were diagnosed
 * from a pcap, by luck, and the remaining drive gap shows NOTHING on the
 * wire: OVMX receives the members' PARAMS and never answers with its own
 * MODEL/PARAMS burst, and no frame anywhere says why. This file is the
 * instrument that makes the join FSM's own runtime visible.
 *
 * ===========================================================================
 * WHAT IT IS, AND THE THREE PROPERTIES THAT MAKE IT SAFE
 *
 * A fixed-size, wrap-around ring of one record per FACT the join really
 * handled: every [state][event] dispatch (INCLUDING the empty table cells the
 * FSM counts as `ignored_events`), every arrival that never reached the table
 * at all, and every message body this node handed to SCS -- with the gate that
 * refused it when it did not go.
 *
 *  1. IT IS OBSERVABILITY ONLY. Nothing in this file, and nothing any caller
 *     does with it, changes a state, a timer, a counter or a byte on the wire.
 *     A ring that is absent (`NULL`) or disabled records nothing and the join
 *     behaves identically -- which is what makes it safe to leave recording ON
 *     through a live-cluster run.
 *
 *  2. IT FABRICATES NOTHING (INV-6). Every field of every record is a value
 *     the executive really held at that instant: the FSM's own `state` before
 *     and after the handler, the shared `enum cnxman_event` the dispatcher
 *     really indexed with, the (category, opcode) READ BACK OUT of the body
 *     that was actually passed to SCS (vms_cm_body_kind(), never the builder's
 *     intent), and the op's own return code. A field the executive does not
 *     hold for a given record kind is an explicit zero with a named kind
 *     beside it, never a plausible value -- the same discipline the CSB view
 *     applies to an unlearned CSID.
 *
 *  3. IT IS ALLOCATION-FREE AND CLOCK-FREE ON THE RECORDING PATH. Recording is
 *     a bounded store into a caller-owned buffer. This TU calls no seam
 *     function, takes no lock and reads no clock: the timestamp is passed IN
 *     by the caller from its own injected `ops->now_ms`, exactly as every pure
 *     cluster FSM already gets its time. Serialization is the CALLER's, and in
 *     production it already exists -- every join event is dispatched from the
 *     one cluster fork context under the fork mutex (design SS3.3, FC-P0.5),
 *     and the diagnostics ioctl takes that same mutex to read. So there is no
 *     new lock, no new ordering, and no new context.
 *
 * ===========================================================================
 * WHAT IT IS NOT
 *
 * It is NOT a log of the wire. A pcap already shows what left the interface;
 * what no pcap can show is a message this node never sent, and the reason it
 * did not. That is what the EMIT record with a `gate` other than SENT carries,
 * and it is the single most important thing this ring exists to report.
 *
 * It is NOT cluster state. Nothing here may be rendered as membership, quorum
 * or a lock. A record says "this happened"; it never says "this is true now".
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 * PURE TU: no seam call, no allocation, no clock, no globals -- so it runs
 * identically in both kmods, in the R1 host tests and in the rung-2 simulator.
 */
#ifndef OVMX_VMS_CNXMAN_DIAG_H
#define OVMX_VMS_CNXMAN_DIAG_H

#include "vms_cluster.h"
#include "vms_cnxman.h"

/* ==========================================================================
 * 1. How much history the executive keeps
 *
 * A POWER OF TWO, so the wrap is a mask and never a division on a VAX kernel
 * stack. 256 records is the whole of a join drive plus its member-driven tail
 * several times over (the E68 re-fire's entire CM dialogue was under 40
 * events), and at 32 bytes a record the ring is 8 KB -- allocated ONCE, beside
 * the connection manager, never on the recording path. Repetition does NOT eat
 * it: an identical fact repeated back to back coalesces into the record it
 * repeats (SS4b), which is what keeps a once-a-second watchdog from erasing the
 * join drive it is waiting on.
 * ========================================================================== */
#define CNXMAN_DIAG_SLOTS      256u
#define CNXMAN_DIAG_SLOT_MASK  (CNXMAN_DIAG_SLOTS - 1u)

/* ==========================================================================
 * 2. The three kinds of record
 *
 * ORDERING. A DISPATCH record is written when the handler RETURNS, so any EMIT
 * records the handler produced carry LOWER sequence numbers and print first: a
 * transition reads as "the messages it sent, then the transition that closed
 * it". Recording the dispatch first would mean recording a `new_state` the
 * handler had not chosen yet, which is the one thing a transcript may not do.
 *
 * Three, because a join stalls in exactly three different ways and a reader
 * has to be able to tell them apart at a glance:
 *   - the event reached the table and the table had nothing to say (DISPATCH,
 *     handled = 0);
 *   - the event never reached the table (ARRIVAL, with the reason);
 *   - the message was built but never left (EMIT, with the gate).
 * ========================================================================== */
enum cnxman_diag_kind {
	CNXMAN_DIAG_K_DISPATCH = 0, /* one [state][event] table dispatch      */
	CNXMAN_DIAG_K_ARRIVAL  = 1, /* a fact arrived at an entry point       */
	CNXMAN_DIAG_K_EMIT     = 2, /* a body was handed to SCS -- or was not */
	CNXMAN_DIAG_K__COUNT
};

/*
 * ARRIVAL reasons. Each one is a place a real fact can stop short of the
 * [state][event] table, and each is a different diagnosis.
 */
enum cnxman_diag_reason {
	CNXMAN_DIAG_R_NONE       = 0,  /* nothing further to say              */
	CNXMAN_DIAG_R_DISPATCHED = 1,  /* handed on to the table (a DISPATCH
					* record follows immediately)         */
	CNXMAN_DIAG_R_UNPARSED   = 2,  /* the codec could not classify it     */
	CNXMAN_DIAG_R_NOT_MINE   = 3,  /* a CM frame no join cell owns        */
	CNXMAN_DIAG_R_PEER_ACK   = 4,  /* cat-0x04: counted, never answered   */
	CNXMAN_DIAG_R_ACCEPTED   = 5,  /* an inbound connect was TAKEN        */
	CNXMAN_DIAG_R_REFUSED    = 6,  /* ... refused: no CSB for that system */
	CNXMAN_DIAG_R_MSCP_REJ   = 7,  /* the peer rejected our disk client   */
	CNXMAN_DIAG_R_CM_REJ     = 8,  /* ... our VMS$VAXcluster connect      */
	CNXMAN_DIAG_R_NOT_OURS   = 9,  /* a Con.ID this join never opened     */
	CNXMAN_DIAG_R_CDT_OPEN   = 10, /* glue: a connection WE opened is up  */
	CNXMAN_DIAG_R_CM_ACCEPT  = 11, /* glue: a member opened one TO us     */
	CNXMAN_DIAG_R_CDT_CLOSED = 12, /* glue: a connection went away        */
	/*
	 * glue: SCS REFUSED a send, with the two codes the executive really
	 * produced for it (E70). Written by cnxman_jop_send_msg() the instant
	 * the refusal is returned and BEFORE the join records its own EMIT
	 * record, so the transcript reads "why it was refused, then which
	 * message was refused".
	 *
	 * The EMIT record already carries the SS$_ status and the Con.ID; this
	 * record carries what that status cannot say, because scs_glue_status()
	 * is many-to-one (vms_scs.h SS5, scs_send_refusal): `rc` is the SCS
	 * layer's own `enum scs_err` and `aux` is the PORT's own return for
	 * that same send, verbatim, or 0 when the port was not the refuser.
	 * Both are reads of live CDT state, not a diagnosis composed here.
	 */
	CNXMAN_DIAG_R_SEND_REFUSED = 13,
	/*
	 * ------------------------------------------------------------------
	 * WHICH refusal it was -- one NAMED reason per cause, never a packed
	 * sub-code (E70's second half)
	 * ------------------------------------------------------------------
	 * The record above carries SCS's own reason and the port's return, and
	 * for a transport refusal that return is still many-to-one: the port's
	 * NOCIRCUIT and RINGFULL both map to SS$_DEVOFFLINE, its BADFRAME and
	 * TOOBIG both to SS$_BADPARAM. "No circuit to that system" and "the
	 * unacked ring is full" are completely different defects, so the
	 * transcript must not leave them merged.
	 *
	 * They are separate REASONS rather than bits packed into `aux` for two
	 * reasons. First, this record's own rule (SS3) is that `aux` carries ONE
	 * fact and is never a composed value -- a bitfield would be exactly the
	 * composed value that rule forbids, and would need a decoder ring on a
	 * lab console at 2 a.m. Second, a reason is what the dumper RENDERS, so
	 * `grep CNXTRACE` shows the word `port-nocredit` with no decoding at
	 * all.
	 *
	 * On each of these `rc` is the PORT's own `enum pe_vc_send_status`,
	 * verbatim (so the record is self-contained and machine-checkable), and
	 * `aux` is the ONE live counter or state that explains that cause:
	 *
	 *   PORT_NOCIRCUIT  aux = the circuit's LIVE `enum vms_pe_vc_state`, or
	 *                   CNXMAN_DIAG_NO_VC when the port holds no circuit
	 *                   object for that system at all
	 *   PORT_NOCREDIT   aux = pe_vc.send_refused_credit -- how many sends
	 *                   this circuit has now refused for want of the
	 *                   PEER'S grant (the grant itself is the port view's)
	 *   PORT_RINGFULL   aux = pe_vc.send_refused_ring
	 *   PORT_BADFRAME   aux = 0: the port kept no count of unsendable
	 *                   frames, and an invented one would be worse
	 *   PORT_TXFAIL     aux = 0, likewise -- the interface refused it
	 *
	 * A refusal that was NOT the port's gets CDT_NOT_SENDABLE instead, with
	 * `rc` = the connection's LIVE `enum vms_scs_cdt_state` (which is how a
	 * peer's DISCONNECT that moved the CDT to DISC RCVD under an
	 * originating SYSAP shows up) and `aux` = its LIVE Send Credit.
	 */
	CNXMAN_DIAG_R_PORT_NOCIRCUIT = 14,
	CNXMAN_DIAG_R_PORT_NOCREDIT  = 15,
	CNXMAN_DIAG_R_PORT_RINGFULL  = 16,
	CNXMAN_DIAG_R_PORT_BADFRAME  = 17,
	CNXMAN_DIAG_R_PORT_TXFAIL    = 18,
	CNXMAN_DIAG_R_CDT_NOT_SENDABLE = 19,
	/*
	 * glue: SCS refused to OPEN a connection (E71). `rc` is the executive's
	 * own SS$_ status for that connect and `aux` is the destination
	 * SCSSYSTEMID, truncated to its low 32 bits like every other identity
	 * here. There is deliberately no companion record naming the refusing
	 * LAYER: a refused connect leaves no CDT to ask (vms_cnxman.c's
	 * cnxman_jop_connect explains why the port's own last refusal may not
	 * belong to it).
	 *
	 * It is written before the join is told, so a transcript reads "the
	 * connect was refused, then what the join did about it" -- which on the
	 * live join-e70refire run was the one fact nothing recorded anywhere.
	 */
	CNXMAN_DIAG_R_CONNECT_REFUSED = 20,
	CNXMAN_DIAG_R__COUNT
};

/* `aux` on a PORT_NOCIRCUIT record when the port holds NO circuit object for
 * that system: chosen outside `enum vms_pe_vc_state` so it can never be
 * mistaken for a state, and reported rather than omitted because "there is no
 * circuit at all" is a stronger fact than any state. */
#define CNXMAN_DIAG_NO_VC 0xffffffffu

/*
 * EMIT gates -- WHY a built body did or did not reach SCS. This enum is the
 * answer to E69's own question ("exactly where the MODEL/PARAMS emit is gated
 * off"), so each value names one concrete precondition of join_emit_cm().
 */
enum cnxman_diag_gate {
	CNXMAN_DIAG_G_SENT    = 0, /* SCS accepted the body                   */
	CNXMAN_DIAG_G_NO_CSB  = 1, /* no CSB for the target: no envelope to
				    * stamp, so nothing is originated (INV-6) */
	CNXMAN_DIAG_G_NO_CONN = 2, /* this join holds no OPEN VMS$VAXcluster
				    * connection (`cm_open` clear)            */
	CNXMAN_DIAG_G_NO_OPS  = 3, /* no send_msg op is bound                 */
	CNXMAN_DIAG_G_REFUSED = 4, /* SCS refused the send; its rc is in `rc` */
	CNXMAN_DIAG_G_CODEC   = 5, /* the codec refused to BUILD the body --
				    * there is no body, so cat/op stay 0      */
	CNXMAN_DIAG_G_SKEW    = 6, /* E77: the CSB's dialogue state belongs to a
				    * DIFFERENT Con.ID than the one this join
				    * would send on, so stamping it would assert
				    * a conversation that did not happen. `aux`
				    * carries the Con.ID the join holds.      */
	CNXMAN_DIAG_G__COUNT
};

/* `event` when the record is not about one of the shared events. Chosen at the
 * top of the byte so it can never collide with a future enum cnxman_event
 * member; the renderer prints it as "-". */
#define CNXMAN_DIAG_EV_NONE 0xffu

/* `cat` on an EMIT record that is NOT a VMS$VAXcluster body: the MSCP
 * disk-client commands share the same emit path and the same ring, and their
 * `op` is the real P.OPCD byte read back through the MSCP codec. */
#define CNXMAN_DIAG_CAT_MSCP 0xffu

/* ==========================================================================
 * 3. The record -- a fixed-width ABI struct
 *
 * It crosses /dev/vms to the userland dumper, so it obeys
 * vms_cluster_snapshot.h's rule 3: no 64-bit scalar, 4-byte alignment
 * everywhere, identical layout on LP64 x86_64/aarch64 and ILP32 VAX. A
 * 48-bit SCSSYSTEMID does not fit `aux` and is therefore NOT carried whole --
 * `aux` says which 32 bits it holds, and the reader must not read it as an
 * identity.
 * ========================================================================== */
struct cnxman_diag_rec {
	uint32_t seq;       /* strictly monotonic across the ring's whole life;
			     * a GAP in this column IS the wrap, and the count
			     * of lost records is recorded + count arithmetic  */
	uint32_t t_ms;      /* the caller's own ops->now_ms() at the FIRST
			     * occurrence of this fact; 0 when no clock op is
			     * bound (honest, not "t=0")                      */
	uint32_t t_last_ms; /* ... and at the LAST -- equal to t_ms unless
			     * `repeat` is nonzero                            */
	uint32_t repeat;    /* HOW MANY MORE TIMES the identical fact happened
			     * back to back (see COALESCING, SS4b). 0 means it
			     * happened exactly once.                         */
	uint8_t  kind;      /* enum cnxman_diag_kind                          */
	uint8_t  state;     /* enum cnxman_join_state ON ENTRY                 */
	uint8_t  new_state; /* ... and after the handler ran (== state for the
			     * kinds that run no handler)                     */
	uint8_t  event;     /* enum cnxman_event, or CNXMAN_DIAG_EV_NONE       */
	uint8_t  detail;    /* DISPATCH: 1 = a table cell fired, 0 = EMPTY CELL
			     *           (the FSM's own `ignored_events`)
			     * ARRIVAL:  enum cnxman_diag_reason
			     * EMIT:     enum cnxman_diag_gate                 */
	uint8_t  cat;       /* EMIT: the category byte READ BACK from the body
			     * really passed to SCS; CNXMAN_DIAG_CAT_MSCP for
			     * an MSCP command. 0 on the other kinds.          */
	uint8_t  op;        /* EMIT: the opcode byte, same provenance          */
	uint8_t  rx;        /* enum cnxman_join_rx the dispatch answered       */
	int32_t  rc;        /* the op's own return: 0 = accepted, else the
			     * refusal the injected op reported               */
	uint32_t aux;       /* The ONE extra fact this record kind carries, and
			     * WHICH it is depends on `kind`/`event`/`detail`:
			     *   Con.ID          connection facts + every EMIT
			     *   the learned CSID          CSID_LEARNED
			     *   low 32 bits of a peer's SCSSYSTEMID
			     *                            CM_ACCEPTED (a
			     *                            correlation aid only:
			     *                            half an identity is
			     *                            NOT an identity)
			     *   1 = HIT / 0 = "NOT PRESENT HERE"  DIR_RESULT
			     *   (category << 8) | opcode  ARRIVAL not-mine /
			     *                            peer-ack
			     *   the body length           ARRIVAL unparsed
			     *   the PORT's own refusal code, verbatim
			     *                            ARRIVAL send-refused
			     *   the ONE live counter or state behind the named
			     *   refusal (see SS2's PORT_* block)
			     *                            ARRIVAL port-* /
			     *                            cdt-not-sendable
			     * 0 everywhere else, and never a composed value  */
};
_Static_assert(sizeof(struct cnxman_diag_rec) == 32,
	       "cnxman_diag_rec is a cross-substrate ABI struct");

/* ==========================================================================
 * 4. The ring itself
 *
 * Owned by whoever allocates it (production: struct vms_cnxman, one
 * exec_zalloc at CLUSTER_START; host tests: the stack). This TU never
 * allocates and never frees.
 * ========================================================================== */
struct cnxman_diag_ring {
	uint8_t  enabled;   /* 0 records nothing and costs one branch         */
	uint8_t  pad0[3];
	uint32_t recorded;  /* SLOTS ever taken -- the seq of the next one    */
	struct cnxman_diag_rec slot[CNXMAN_DIAG_SLOTS];
};

/* ==========================================================================
 * 4b. COALESCING -- why an identical fact does not take a new slot
 *
 * THE PROBLEM IT SOLVES, measured. The join watchdog re-arms itself once a
 * second and never abandons a join (CNXMAN_JOIN_WATCH_MS; p. 2-51's "the
 * poller REPEATS"), so a join that is WAITING -- exactly the case this
 * instrument exists to explain -- offers one identical
 * [state][TIMER_JOIN] dispatch every second, forever. Against a plain
 * wrap-around ring that is ~256 seconds to total amnesia: the lab's dump
 * happens ~600 s into a run, by which time the join drive itself -- the
 * first few dozen records, the ONLY ones that carry the answer -- would have
 * been overwritten by nothing but watchdog ticks.
 *
 * THE RULE. A record whose every meaningful field equals the PREVIOUS
 * record's does not take a new slot: the previous record's `repeat` is
 * incremented and its `t_last_ms` advanced. Only CONSECUTIVE identical facts
 * coalesce, so nothing that happened between two ticks is ever hidden -- one
 * different fact in the middle starts a new record and the run ends there.
 *
 * WHAT IT COSTS, AND WHY THAT IS HONEST (INV-6). The individual timestamps of
 * the collapsed repeats are not kept -- their FIRST and LAST are, and their
 * exact COUNT is. Nothing is invented and nothing is silently dropped: a
 * reader sees "this transition happened 587 times between t and t'", which is
 * a stronger statement than 587 identical lines, and the seq column stays
 * unambiguous because it counts SLOTS, not facts.
 * ========================================================================== */

/* ==========================================================================
 * 5. Lifecycle + recording
 * ========================================================================== */

/* Zero the ring and set whether it records. A NULL ring is a legitimate
 * wiring (diagnostics off) and every function below tolerates it. */
void cnxman_diag_init(struct cnxman_diag_ring *r, int enabled);

/* Turn recording on or off without losing what is already held. */
void cnxman_diag_enable(struct cnxman_diag_ring *r, int enabled);

/*
 * The three recorders. Each offers exactly one record -- which either takes a
 * slot or coalesces into the previous one (SS4b) -- and returns nothing: a
 * diagnostic that could fail would be a second thing to diagnose.
 */
void cnxman_diag_dispatch(struct cnxman_diag_ring *r, uint32_t t_ms,
			  uint8_t state, uint8_t new_state, uint8_t event,
			  int handled, uint8_t rx, uint32_t aux);

void cnxman_diag_arrival(struct cnxman_diag_ring *r, uint32_t t_ms,
			 uint8_t state, uint8_t event, uint8_t reason,
			 int32_t rc, uint32_t aux);

void cnxman_diag_emit(struct cnxman_diag_ring *r, uint32_t t_ms,
		      uint8_t state, uint8_t cat, uint8_t op, uint8_t gate,
		      int32_t rc, uint32_t aux);

/* ==========================================================================
 * 6. Readback -- OLDEST FIRST
 * ========================================================================== */

/* How many records the ring HOLDS right now (recorded, capped at the ring
 * size). Never more than CNXMAN_DIAG_SLOTS. */
uint32_t cnxman_diag_count(const struct cnxman_diag_ring *r);

/* How many SLOTS were ever taken. `recorded - count` is exactly how many the
 * wrap dropped, which is why both are reported rather than one. It is NOT the
 * number of facts: a coalesced record carries its own `repeat` (SS4b). */
uint32_t cnxman_diag_recorded(const struct cnxman_diag_ring *r);

/*
 * Copy record `i` of the held history, 0 being the OLDEST record still held.
 * Returns 1 on success, 0 when `i` is past the end (and `out` is untouched):
 * a reader walking off the end gets a refusal, never a zero-filled record it
 * could mistake for a transition that happened.
 */
int cnxman_diag_get(const struct cnxman_diag_ring *r, uint32_t i,
		    struct cnxman_diag_rec *out);

/* ==========================================================================
 * 7. The projection VMS_IOCTL_CLUSTER_DIAG_JOIN carries
 *
 * One window into the ring plus the join's own LIVE state, taken together
 * under the fork mutex so the two cannot disagree -- the same shape and the
 * same discipline as cnxman_get_club()'s CLUB view (vms_cluster_snapshot.h's
 * three rules; no 64-bit scalar anywhere below).
 *
 * 32 records a call: 1056 bytes with the args wrapper, comfortably under
 * NetBSD's one-page IOCPARM_MAX, so the same pre-copy _IOWR path every other
 * cluster diagnostic rides. A caller walks the ring by re-issuing with
 * `first` advanced.
 * ========================================================================== */
#define CNXMAN_DIAG_ROWS 32u

struct cnxman_diag_view {
	uint32_t count;          /* records the ring HOLDS right now         */
	uint32_t recorded;       /* SLOTS ever taken; recorded - count is
				  * exactly what the wrap dropped. NOT the
				  * fact count -- see each row's `repeat`   */
	uint32_t first;          /* which held record row[0] is (the caller's
				  * requested index, clamped)               */
	uint32_t n_rows;         /* rows really filled -- 0 at the end       */
	uint8_t  join_state;     /* enum cnxman_join_state, LIVE             */
	uint8_t  join_failure;   /* enum cnxman_join_failure, LIVE           */
	uint8_t  enabled;        /* the ring is recording                    */
	uint8_t  pad0;
	uint32_t ignored_events; /* the join's own [state][event]-empty-cell
				  * counter, for cross-checking the ring     */
	struct cnxman_diag_rec row[CNXMAN_DIAG_ROWS];
};
_Static_assert(sizeof(struct cnxman_diag_view) == 1048,
	       "cnxman_diag_view is a cross-substrate ABI struct");

/*
 * Project the connection manager's ring + live join state (FC-P3.8's
 * cnxman_get_club() sibling; implemented in vms_cnxman.c, which is where the
 * fork mutex lives). SS$_NOSUCHDEV with an all-zero view when the connection
 * manager is not up -- never a placeholder transcript (INV-6).
 */
struct vms_cluster;
int cnxman_get_join_diag(struct vms_cluster *cl, uint32_t first,
			 struct cnxman_diag_view *out);

/* ==========================================================================
 * 8. Names
 *
 * The strings the ring's own vocabulary renders as. The userland dumper
 * carries its own copy (it cannot link kernel-core), and the host test
 * test_cnxman_diag.c compares the two tables ordinal by ordinal so the copies
 * cannot drift.
 * ========================================================================== */
const char *cnxman_diag_kind_name(uint8_t kind);
const char *cnxman_diag_reason_name(uint8_t reason);
const char *cnxman_diag_gate_name(uint8_t gate);
const char *cnxman_diag_rx_name(uint8_t rx);
/* Renders enum cnxman_event (vms_cnxman.h), plus CNXMAN_DIAG_EV_NONE. */
const char *cnxman_diag_event_name(uint8_t event);

/* ==========================================================================
 * 9. The PORT's refusal, in this ring's vocabulary (E70)
 *
 * A caller that has just been told the PORT refused a send takes the port's
 * own readback (`struct pe_vc_send_refusal`, vms_pe_fsm.h) and asks these two
 * which record to write: the NAMED reason for that cause, and the ONE live
 * number that explains it (SS2's PORT_* block lists them per reason).
 *
 * PURE and TOTAL. Neither reads the port, neither can fail, and a code this
 * vocabulary has not grown yet still gets a real record -- the honest
 * catch-all, with the port's verbatim number carried in the record's `rc`.
 * They live in this TU because it owns `enum cnxman_diag_reason`, and because
 * the glue that calls them is not host-linkable while these are.
 * ========================================================================== */
struct pe_vc_send_refusal;

enum cnxman_diag_reason cnxman_diag_port_reason(int32_t code);
uint32_t cnxman_diag_port_aux(const struct pe_vc_send_refusal *p);

#endif /* OVMX_VMS_CNXMAN_DIAG_H */
