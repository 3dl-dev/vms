/*
 * scs_svc.h - THE FIVE SCS SYSAP SERVICES (vms-561).
 *
 *   LISTEN   CONNECT   ACCEPT   REJECT   DISCONNECT
 *
 * SOURCE: VAXcluster Principles (Davis, 1993) ch. 2, pp. 2-22..2-27 and p. 2-56.
 * Quoted with page cites; the chapter itself is host-only and is never committed.
 *
 * p. 2-56 names the whole job of this file:
 *
 *   "Providing SYSAPs with the calling interface to the LISTEN, CONNECT,
 *    ACCEPT, REJECT, and DISCONNECT services, and implementing those services."
 *
 * ==========================================================================
 * WHAT THIS ITEM IS, AND WHAT IT IS NOT
 * ==========================================================================
 *
 * It is an API SURFACE over machinery that already exists. Everything below is
 * a composition of layers this epic already built and tested:
 *
 *   the CDT/CDL/CONID model .............. vms-e1a  (scs_cdt.h)
 *   the CONNECTION state machine ......... vms-dd5  (scs_conn.h)
 *   CONFIG_SYS / CONFIG_PATH circuit sel . vms-398  (scs_config.h)
 *   credit accounting .................... vms-76e  (scs_credit.h)
 *   the Credit Wait queue ................ vms-1d2  (scs_credit.h)
 *   datagram buffer accounting ........... vms-b1d  (scs_dgram.h)
 *   the ordered teardown ................. vms-17f  (scs_depart.h)
 *
 * NOTHING IS REIMPLEMENTED HERE. This file allocates no memory, builds no
 * frame, parses no frame and opens no socket. It decides WHICH of the above to
 * call, in what order, and it asks a caller-supplied emitter to put the
 * resulting SCS control message on the wire.
 *
 * WHAT IT IS NOT: it is not a fix for the vms-2f3 rejoin failure, and nothing
 * here should be read as claiming to be. It is the calling interface the
 * SYSAP-facing half of SCS was missing, and the place the four sibling items
 * (vms-591 symmetric disconnect, vms-6b3 reason codes, vms-fdd connect data,
 * vms-7fe the SDIR listen queue) attach to.
 *
 * ==========================================================================
 * THE EMITTER, AND WHY THE SERVICES DO NOT BUILD FRAMES
 * ==========================================================================
 *
 * p. 2-56 draws the line for us:
 *
 *   "The SCS CONNECT and ACCEPT services each result in the allocation of a
 *    CDT for new connections. The SCS DISCONNECT service results in releasing
 *    CDTs. Thus, the SYSAP calling interface for each of these services is in
 *    SYS$SCS. However, the actual dialogue for connection formation and
 *    disconnect is managed by SCS code in the port driver."
 *
 * So: descriptor lifecycle and connection state HERE; frame assembly and
 * transmission in the port driver, which for OVMX is src/vmsscs/scsd.c. The
 * services reach it through struct scs_svc_args::emit -- a function the caller
 * supplies that is handed the connection and the enum scs_conn_action the state
 * machine named, and that answers whether a frame actually went out.
 *
 * THIS IS ALSO WHAT KEEPS THE p. 2-31 SEND CHOKE POINT INTACT. scsd.c routes
 * every SCS-layer send through send_frame_vc(), which refuses to transmit
 * without an OPEN virtual circuit, and tests/vmsscs/test_scsd_send_sites.py
 * enforces that mechanically by parsing scsd.c. Because the emitters live in
 * scsd.c and this file contains no transmit primitive at all, migrating the
 * daemon's call sites onto these services does not move a single send out from
 * under that census -- the emitters are named in the SEND SITE TABLE like any
 * other sender.
 *
 * ==========================================================================
 * THE HONEST STATE OF THE FIVE, AS OF THIS ITEM
 * ==========================================================================
 *
 * CONNECT     LIVE. scsd.c's joiner VMS$VAXcluster CONNECT-REQUEST goes out
 *             through it on every join.
 * ACCEPT      LIVE, twice: the SCS$DIRECTORY connect (op=1 CONNECT-ECHO +
 *             op=2 CONNECT-RESPONSE) and the member-opened VMS$VAXcluster
 *             0x4b CONNECT-RESPONSE.
 * LISTEN      LIVE AS OF vms-7fe, and now backed by the p. 2-48 SDIR queue
 *             (scs_sdir.h) with a listening CDT per name. TWO production
 *             readers: the SCS$DIR_LOOKUP responder answers affirmative iff the
 *             queried name is in the queue (the hardcoded VMS$VAXcluster name
 *             compare is gone), and both inbound-CONNECT_REQ paths scan the
 *             queue before accepting. Gated by OVMX_NO_SDIR, which restores the
 *             pre-vms-7fe hardcoded compare and skips the connect scan.
 *             STILL NOT TRUE: no SYSAP supplies a connect-request handler, so
 *             the p. 2-48 delivery calls nothing (see scs_sdir.h).
 * REJECT      NO PRODUCTION CALLER. OVMX accepts every connect request it
 *             understands and ignores the rest; it has never rejected one.
 *             The service exists, allocates nothing, and is exercised only by
 *             tests/vmsscs/test_scs_svc.c.
 * DISCONNECT  LIVE AS OF vms-591, and it is the one entry above whose status
 *             changed from "no production caller". FOUR production callers:
 *             the receive path's answer to a peer DISCONNECT_REQ (the p. 2-26
 *             symmetric own-disconnect), the daemon's shutdown teardown,
 *             scs_svc_disconnect_all(), and (vms-66f round 4) the SCS Process
 *             Poller closing its p. 2-50 cycle. OVMX now BUILDS both DISCONNECT_REQ
 *             and DISCONNECT_RSP from byte-exact captured templates
 *             (src/vmsscs/scs_disc.h) -- including the DISCONNECT_RSP the spec
 *             said did not exist on our wire, which vms-591 found and which
 *             corrects docs/cluster-protocol-spec.md sec 4(h)(1a).
 *             Gated by OVMX_NO_CLEAN_SHUTDOWN=1, which restores the
 *             pre-vms-591 wire exactly (it carried no DISCONNECT frame at all).
 *
 * Do not read "the service exists" as "OVMX disconnects cleanly on the wire"
 * for the four entries above that say otherwise; the counters in struct
 * scs_svc_port say which is which at runtime.
 *
 * ==========================================================================
 * NO KILL SWITCH, AND WHY THAT IS THE RIGHT ANSWER HERE
 * ==========================================================================
 *
 * The standing rule is that a WIRE-VISIBLE change ships an env kill switch. This
 * change is not wire-visible, and that is MEASURED TWICE:
 *
 *   1. BYTE DIFF, deterministic. tools/cluster/scsd_wire_diff.sh compiles the
 *      pre- and post-migration scsd.c into two dump programs -- through the
 *      PRODUCTION transmit path, with sendto() macro-renamed to a capture
 *      function, so the bytes recorded are the bytes handed to the socket --
 *      replays the same captured frames through both, and byte-diffs every
 *      emitted frame. 22 frames, 185 dump lines, IDENTICAL, covering the
 *      SCS$DIRECTORY accept (both of its frames), the member accept and its
 *      re-answer, CONNECT by named circuit / by CONFIG_SYS selection / on
 *      retransmit, the joiner accept with its add-member burst, and the whole
 *      script again under OVMX_NO_CONN_FSM=1.
 *
 *      IT EARNED ITS KEEP ON THE FIRST RUN, which was RED: descriptorless
 *      ACCEPT dropped its second frame, because the second transition had no
 *      state to start from. The shadow state in svc_step() is that bug's fix.
 *
 *   2. LIVE LAB, functional. Both binaries run in turn against a real 2-node
 *      VMScluster (lab-2 pod vaxlab-3, br0, 70 s, --emit-hello --respond
 *      --connect; logs kept at /data/training/vax/k8s-labs/vaxlab-3/logs/
 *      561-scsd-{before,after}.log). Both reach the same join milestones with
 *      the same OVMX Con.IDs -- SCSD-I-DIRCONN (local 0x4F580007),
 *      SCSD-I-CONNREQ (local 0x4F580002, seq 3), SCSD-I-JOINBOUND,
 *      SCSD-I-CMCONFIG (3 frames, VOTES=0) -- the SAME SIX CONNFSM transitions
 *      in the same order, the same single SCSD-W-CONNNOACT (the ACCEPT_RSP OVMX
 *      cannot build), and the same "0 of 2 in-use connection(s) parked off
 *      OPEN". The only differences are the VAX's own per-run Con.IDs and the
 *      received-frame counts (132 vs 134), both per-run values.
 *
 *      WHAT THAT RUN DOES NOT PROVE: it is not a byte comparison (no capture
 *      was diffed) and it is not a rejoin test. It proves the refactored daemon
 *      still forms both connections and drives the add-member burst on a real
 *      cluster, which the deterministic diff alone does not.
 *
 * The item's own constraint says a kill switch is not required if the bytes are
 * provably identical.
 *
 * The switch that DOES gate this code already exists and is unchanged:
 * OVMX_NO_CONN_FSM (vms-dd5). With it set, no CDT is allocated and no state is
 * recorded -- and the services still emit exactly the frames they emitted
 * before, because in that mode they run DESCRIPTORLESS (see below). Adding a
 * second switch that gated the same behaviour would be theatre.
 *
 * ==========================================================================
 * DESCRIPTORLESS MODE -- the one piece of this that is not from the book
 * ==========================================================================
 *
 * OVMX ADDITION, labeled per rule 8. When OVMX_NO_CONN_FSM is set (or the CDL
 * has not been initialized), scsd.c's pre-vms-561 code allocated no CDT, took
 * no transition, and STILL BUILT AND SENT ITS FRAMES -- the emission was
 * outside the state machine entirely. Preserving that exactly is what makes
 * this refactor wire-invisible under the switch as well as without it, so
 * CONNECT and ACCEPT fall back to driving the DOCUMENTED action sequence
 * (looked up from the pure table starting at CLOSED) with no CDT to record it
 * on. Nothing in SCA describes this; it exists so one kill switch keeps meaning
 * exactly what it meant before.
 */
#ifndef SCS_SVC_H
#define SCS_SVC_H

#include <stddef.h>
#include <stdint.h>

#include "scs_cdt.h"    /* struct scs_cdt / struct scs_cdl, the SYSAP entry points */
#include "scs_conn.h"   /* enum scs_conn_action / enum scs_conn_event, the table */
#include "scs_config.h" /* struct scs_pb / struct scs_config, CONFIG_SYS + CONFIG_PATH */
#include "scs_sdir.h"   /* vms-7fe: the p. 2-48 SDIR queue -- LISTEN's actual list */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How many SYSAP names one node may have LISTENing at once.
 *
 * vms-7fe: this is now just the SDIR queue's capacity, spelled here so callers
 * that already used this name keep working. The real list is
 * struct scs_sdir_queue (scs_sdir.h), which allocates a listening CDT per
 * entry exactly as p. 2-48 requires.
 */
#define SCS_SVC_MAX_LISTENERS SCS_SDIR_MAX

/* --- status ---------------------------------------------------------------
 *
 * NOT VMS status codes. These are the service layer's own results; the OVMX
 * daemon logs them and the tests assert on them. (Project rule 4's odd/even VMS
 * convention applies to the sys$/lib$ APIs, not to this internal interface --
 * scs_cdt.c, scs_conn.c and scs_config.c all use plain enums for the same
 * reason.)
 */
enum scs_svc_status {
    SCS_SVC_OK = 0,       /* the service ran; see the port counters for what it emitted */
    SCS_SVC_BADARG,       /* NULL port/args, or a required argument missing */
    SCS_SVC_NOCDT,        /* p. 2-30: no free CDT, or the requested CONID is taken */
    SCS_SVC_NOVC,         /* p. 2-47: no OPEN virtual circuit to the target node */
    SCS_SVC_BADSTATE,     /* the connection state machine has no rule for this service here */
    SCS_SVC_NOLISTEN,     /* LISTEN: the list is full, or the name is already listening */
    SCS_SVC_NOTSENT       /* the port emitted nothing for an action the machine required */
};

const char *scs_svc_status_name(enum scs_svc_status st);

/* --- the port's emitter ---------------------------------------------------
 *
 * Called once per transition whose action is not NONE, BEFORE the transition is
 * committed. The ORDER MATTERS and is not arbitrary: scsd.c's pre-migration
 * code sent the frame first and stepped the machine afterwards, and every
 * conn_step() call site passed the name of the frame it had just put on the
 * wire. Emitting first preserves that, and it is also what lets the emitter
 * report honestly -- it knows whether the circuit accepted the frame.
 *
 * THREE ANSWERS, and the difference between the last two is load-bearing:
 *
 *   SCS_SVC_EMIT_SENT (1)     a frame went out. Counted in port->emitted.
 *   SCS_SVC_EMIT_NOBUILDER(0) the port has no way to build this frame. The
 *                             transition IS committed -- the connection really
 *                             is in the new state, SCA just did not get its
 *                             packet -- and port->unemitted records it. As of
 *                             vms-591 that list is CONNECT_RSP, ACCEPT_RSP,
 *                             REJECT_REQ and REJECT_RSP; DISCONNECT_REQ and
 *                             DISCONNECT_RSP came OFF it and are built by
 *                             src/vmsscs/scs_disc.c (see the DISCONNECT entry
 *                             in the five-service table above, and
 *                             docs/cluster-protocol-spec.md sec 4(h)(1a)).
 *                             They still answer NOBUILDER in exactly two cases,
 *                             both literally true: with OVMX_NO_CLEAN_SHUTDOWN=1
 *                             set, and for a CDT-less connection, whose frame
 *                             would name no Con.ID pair.
 *                             Saying so is the point; pretending a frame went
 *                             out is the LARP failure INV-6 forbids.
 *   SCS_SVC_EMIT_REFUSED(-1)  the port declined -- no OPEN virtual circuit
 *                             (p. 2-31), or the builder rejected its arguments.
 *                             The transition is NOT committed, the remaining
 *                             steps of the service are abandoned, and a CDT
 *                             this call allocated is released again.
 *
 * The 0/-1 split is exactly the pre-vms-561 daemon's own behaviour, preserved:
 * its member-connect branch stepped the machine for a frame it could not build
 * (passing NULL to conn_step) but stepped NOTHING when the send was refused.
 *
 * `what` may be filled with a short human name for the frame, for the caller's
 * own logging. It is not interpreted here.
 */
#define SCS_SVC_EMIT_SENT       1
#define SCS_SVC_EMIT_NOBUILDER  0
#define SCS_SVC_EMIT_REFUSED  (-1)

struct scs_svc_args;

typedef int (*scs_svc_emit_fn)(void *ctx, struct scs_cdt *cdt,
                               enum scs_conn_action action,
                               const struct scs_svc_args *args,
                               const char **what);

/* The routine a listening SYSAP supplies to handle an incoming connect request
 * (p. 2-48: "a listening CDT contains the address of the SYSAP's routine for
 * handling incoming connect requests"). vms-7fe stores it on the listening CDT
 * the SDIR names, and scsd.c's directory and member accept paths now call it.
 * The type is scs_sdir.h's -- one typedef, not two that must be kept in step. */
typedef scs_sdir_connect_req_fn scs_svc_connect_req_fn;

/*
 * struct scs_svc_port - the node's SCS service state. One per node (scsd.c has
 * exactly one, file-static, next to the one CDL it already had).
 *
 * The counters are here because every claim this file makes about what the
 * services DID must be a number a test can read, not a sentence. `unemitted` in
 * particular is the running measure of how far OVMX's frame vocabulary is from
 * SCA's: it counts actions the state machine required and the port could not
 * build.
 */
struct scs_svc_port {
    struct scs_cdl *cdl;                                 /* p. 2-29 */
    struct scs_sdir_queue sdir;                          /* p. 2-48, Figure 2-24 */

    unsigned long connects;      /* scs_connect() calls that reached the machine */
    unsigned long accepts;       /* scs_accept()   ditto */
    unsigned long rejects;       /* scs_reject()   ditto */
    unsigned long disconnects;   /* scs_disconnect() ditto */
    unsigned long cdts_allocated;/* p. 2-56: CONNECT and ACCEPT allocate */
    unsigned long cdts_released; /* p. 2-56: DISCONNECT releases */
    unsigned long delivered;     /* vms-591: received-message events fed to scs_svc_deliver() */
    unsigned long transitions;   /* transitions COMMITTED onto a CDT */
    unsigned long emitted;       /* actions the port put on the wire */
    unsigned long unemitted;     /* actions the port had no builder for */
    unsigned long refused;       /* actions the port declined to transmit */
    unsigned long illegal;       /* (state, service) pairs no rule covers */
};

/*
 * struct scs_svc_args - the argument set of pp. 2-22..2-23, 2-26 and 2-56,
 * gathered into one object. Every field names the page that puts it there.
 *
 * A field a given service does not use is ignored, not rejected: the five
 * services deliberately share one argument object so that a SYSAP (and the four
 * sibling items) fill in what they have.
 */
struct scs_svc_args {
    /* p. 2-22: "the SYSAP in NODE_1 specifies its own name, the name of the
     * target SYSAP with which it is requesting a connection, and the node in
     * which the target SYSAP resides". */
    const char *local_sysap;   /* source SYSAP name */
    const char *remote_sysap;  /* target SYSAP name */
    const uint8_t *target_node; /* the node: its 48-bit SCS System Address (CONFIG_SYS key) */

    /* p. 2-47: CONNECT may be given the circuit, or may be left to select one.
     * `vc` non-NULL names it; `vc` NULL selects with CONFIG_SYS + the OPEN scan
     * over `cfg`, which is scs_config_select_vc(). */
    struct scs_pb *vc;
    struct scs_config *cfg;

    /* p. 2-28: "the address of the interested SYSAP's error handler for virtual
     * circuit loss ... is supplied by a SYSAP as an argument to the VMS
     * implementations of both the CONNECT and ACCEPT services."
     * p. 2-29: "VMS also stores the addresses of a SYSAP's message and datagram
     * input routines in the CDT. These addresses are supplied as arguments to
     * the CONNECT and ACCEPT services." */
    scs_vc_loss_fn     vc_loss;
    scs_msg_input_fn   msg_input;
    scs_dgram_input_fn dgram_input;
    void              *sysap_ctx; /* OVMX addition: the ctx those three are called with */

    /* p. 2-28: connect data carried on CONNECT_REQ / ACCEPT_REQ. CARRIED, NEVER
     * INTERPRETED -- vms-fdd owns what the 16 bytes mean. */
    const void *connect_data;
    size_t      connect_data_len;

    /* p. 2-43/2-44: the credit arguments. `send_credits` is the number of
     * message buffers this SYSAP contributes to the port MFREEQ at formation;
     * `min_send_credits` is the Minimum Send Credits the REMOTE SYSAP passed,
     * against which p. 2-44's dangerously-low test runs. Both 0 = do not touch
     * the credit account (which is what scsd.c passes today). */
    unsigned send_credits;
    unsigned min_send_credits;

    /* p. 2-42: "A SYSAP can optionally request SCS to allocate a number of
     * buffers to handle incoming datagrams received on a particular connection.
     * This is done using an optional argument in the CONNECT and ACCEPT
     * services." 0 = ask for none. */
    unsigned dgram_buffers;

    /* p. 2-28/2-29: the CONIDs. `conid` is an OVMX addition -- claim THIS
     * CONID for the new CDT rather than the next free CDL slot, because OVMX has
     * already shipped three fixed Con.IDs on the lab wire (scs_cdt.h). 0 = take
     * whatever the allocator hands out. `remote_conid` is the peer's handle,
     * known at ACCEPT and at REJECT, not yet known at CONNECT. */
    uint32_t conid;
    uint32_t remote_conid;

    /* p. 2-26: "it contains an optional disconnect reason code if one is
     * specified by the SYSAP", and the same for REJECT (p. 2-24). CARRIED, NEVER
     * INTERPRETED HERE -- vms-6b3 defines the value in scs_reason.h
     * (enum scs_reason_code) and the two bytes it occupies. Nothing in THIS file
     * puts a reason code on any wire; it is handed to args->emit and the emitter
     * stamps it.
     *
     * CORRECTED (vms-096): this used to add "the REJECT_REQ / DISCONNECT_REQ
     * builder OVMX still does not have needs only to call scs_reason_put()".
     * OVMX HAS that builder -- scs_disc_build_request() since vms-591 -- and it
     * DOES call scs_reason_put(). scsd.c drives it with SCS_REASON_SYSAP_SHUTDOWN
     * on clean shutdown and SCS_REASON_PEER_DISCONNECT on the symmetric answer,
     * so nonzero reason codes are on a real VAX's wire. See the SEND SIDE block
     * in scs_reason.h for the census and the design-choice ruling. */
    uint16_t reason;

    /*
     * OVMX ADDITION (labeled, rule 8). SCA does not say what a node does when a
     * CONNECT_REQ it has already accepted arrives again; our lab wire does -- the
     * established VAX retransmits until it accepts the answer, and scsd.c has
     * re-answered every one since vms-c6d. Set this when the SYSAP has already
     * accepted this connection: ACCEPT then drives ONLY the labeled OVMX
     * retransmit row of scs_conn.c (ACCEPT SENT --RCV_CONNECT_REQ--> ACCEPT
     * SENT, repeating the ACCEPT_REQ) instead of re-running the formation pair.
     *
     * IT ONLY RECOGNISES A CONNECTION `conid` NAMES. A re-answer with conid 0
     * has nothing to look up, so ACCEPT allocates a fresh CDT and starts it
     * from ACCEPT SENT. scsd.c always names the CONID (its three are fixed
     * macros), so this is a caveat for future callers, not a live defect.
     */
    int retransmit;

    /* The port driver half (p. 2-56). `emit` may be NULL, in which case every
     * action is counted unemitted and nothing is sent. */
    scs_svc_emit_fn emit;
    void           *emit_ctx;
};

/* --- LISTEN (p. 2-22) -----------------------------------------------------
 *
 * "During a SYSAP's initialization, the SYSAP reaches the point where it is
 *  capable of handling incoming connect requests. When this point is reached,
 *  the SYSAP invokes the LISTEN service. This service will place the SYSAP's
 *  name into a 'list of listening SYSAPs' ..." (p. 2-22)
 *
 * `on_connect_req` is p. 2-48's "SYSAP's routine for handling incoming connect
 * requests"; it may be NULL.
 *
 * WHAT THIS NOW DOES, AND DID NOT BEFORE vms-7fe: it allocates the SDIR and the
 * listening CDT p. 2-48 requires ("an SDIR containing the SYSAP's name is
 * allocated and placed in this queue. Each SDIR contains the CONID of a special
 * 'listening CDT' that is also allocated at this time"). So LISTEN DOES take a
 * CDT -- one per listening name, from a reserved CONID band that cannot collide
 * with the three Con.IDs OVMX ships on the wire (scs_sdir.h, DESIGN CHOICE 1).
 * p. 2-56's "CONNECT and ACCEPT services each result in the allocation of a CDT
 * for new connections" is about CONNECTION CDTs and does not contradict this:
 * a listening CDT is not a connection.
 *
 * Returns SCS_SVC_OK, SCS_SVC_BADARG (NULL port or name), or SCS_SVC_NOLISTEN
 * (the queue is full, the name is already in it -- LISTEN is not idempotent by
 * accident; a duplicate is a SYSAP bug worth seeing -- the port has no CDL, or
 * the reserved listening CONID was already taken).
 */
enum scs_svc_status scs_listen(struct scs_svc_port *port, const char *local_sysap,
                               scs_svc_connect_req_fn on_connect_req, void *ctx);

/* Is `local_sysap` in the list? 0/1. The SCS Directory Service's question
 * (p. 2-22: "The SCS Directory Service is in fact a SYSAP that responds to
 * inquires from other nodes wanting to know if particular SYSAP names are in
 * this list."). Non-counting: it does not move the SDIR scan counters. */
int scs_svc_listening(const struct scs_svc_port *port, const char *local_sysap);

/* The port's SDIR queue -- the p. 2-48 scan, the listening CDTs, and the
 * counters. The mutable form is what the port driver's receive path runs
 * scs_sdir_lookup() / scs_sdir_connect_req() against. */
const struct scs_sdir_queue *scs_svc_sdir(const struct scs_svc_port *port);
struct scs_sdir_queue *scs_svc_sdir_mut(struct scs_svc_port *port);

/* --- CONNECT (pp. 2-22..2-23, 2-47, 2-56) ---------------------------------
 *
 * The ACTIVE half. Allocates a CDT (p. 2-56), records the SYSAP's entry points
 * and optional arguments on it, and drives SCS_CONN_EV_SVC_CONNECT -- which
 * Figure 2-14 takes CLOSED -> CONNECT SENT with the action "send CONNECT_REQ".
 * The emitter is called with that action before the transition commits.
 *
 * CIRCUIT SELECTION (p. 2-47) happens FIRST and can refuse: if `vc` is NULL the
 * circuit is chosen by scs_config_select_vc(cfg, target_node) -- CONFIG_SYS
 * then the OPEN scan -- and if that finds nothing, SCS_SVC_NOVC is returned
 * with NO CDT ALLOCATED and nothing emitted. A named `vc` is checked through
 * CONFIG_PATH the same way and refused if it is not OPEN.
 *
 * IF THE PORT SENDS NOTHING, THE CONNECTION IS UNWOUND: a CDT this call
 * allocated is released again and SCS_SVC_NOTSENT is returned. A connection
 * descriptor for a CONNECT_REQ that never left the node is a lie the CDL would
 * then carry for the rest of the run.
 *
 * RETRANSMIT: calling CONNECT again for a CONID that already has an in-use CDT
 * does not allocate a second one; it re-drives the machine, which for CONNECT
 * SENT is scs_conn.c's labeled OVMX retransmit row. This is how scsd.c's
 * timer-driven CONNECT-REQUEST retransmit reaches the wire.
 *
 * `*cdt_out` (may be NULL) receives the CDT, or NULL in descriptorless mode.
 */
enum scs_svc_status scs_connect(struct scs_svc_port *port,
                                const struct scs_svc_args *args,
                                struct scs_cdt **cdt_out);

/* --- ACCEPT (p. 2-23, 2-56) -----------------------------------------------
 *
 * The PASSIVE half's yes. Allocates a CDT (p. 2-56) and drives the target's two
 * Figure 2-14 transitions in order:
 *
 *   CLOSED      --RCV_CONNECT_REQ--> CONNECT REC   action: send CONNECT_RSP
 *   CONNECT REC --SVC_ACCEPT-------> ACCEPT SENT   action: send ACCEPT_REQ
 *
 * BOTH, in one call, because that is what the wire does and what scsd.c already
 * did: the daemon answers a directory CONNECT-REQUEST with an op=1
 * CONNECT-ECHO followed by an op=2 CONNECT-RESPONSE, which
 * docs/cluster-protocol-spec.md sec 4(h)(1) grounds as exactly that pair. The
 * emitter is called once per action, in that order, so the frames leave in the
 * order they left before.
 *
 * The book has SCS pass the request to the SYSAP and the SYSAP then invoke
 * ACCEPT; OVMX has no asynchronous SYSAP to hand it to, so the receive path
 * decides and calls ACCEPT synchronously. LABELED as a difference here rather
 * than hidden: an OVMX SYSAP cannot currently take time to decide.
 *
 * args->retransmit selects the re-answer path instead (see the field).
 */
enum scs_svc_status scs_accept(struct scs_svc_port *port,
                               const struct scs_svc_args *args,
                               struct scs_cdt **cdt_out);

/* --- REJECT (p. 2-24, 2-56) -----------------------------------------------
 *
 * The PASSIVE half's no, and the one service that touches no descriptor at all:
 *
 *   "The SCS REJECT service does not involve a CDT at all, but merely the
 *    sending of a message rejecting a connect request. Thus, REJECT is
 *    implemented entirely in the port driver." (p. 2-56)
 *
 * So this function ALLOCATES NOTHING, RELEASES NOTHING and never touches the
 * CDL -- tests/vmsscs/test_scs_svc.c asserts scs_cdl_in_use_count() is
 * unchanged across it, which is the p. 2-56 claim as a number. What it does is
 * derive the action from the PURE table (Figure 2-15's CONNECT REC --SVC_REJECT
 * --> REJECT SENT, action send REJECT_REQ) and ask the port to emit it, with
 * args->reason carried through untouched for vms-6b3.
 *
 * There is no CDT, so there is no state to advance: the REJECT SENT state of
 * Figure 2-15 lives in the port driver's own dialogue context, exactly as the
 * quote says. OVMX does not model it, and this is the honest consequence.
 */
enum scs_svc_status scs_reject(struct scs_svc_port *port,
                               const struct scs_svc_args *args);

/* --- DISCONNECT (pp. 2-25..2-27, 2-56) ------------------------------------
 *
 * Drives SCS_CONN_EV_SVC_DISCONNECT on `cdt`. From OPEN that is Figure 2-16's
 * OPEN -> DISC SENT with action send DISCONNECT_REQ; from DISC RECEIVED it is
 * the symmetric answer, DISC RECEIVED -> DISC MATCH. args->reason carries
 * p. 2-26's "optional disconnect reason code".
 *
 * THE CDT IS RELEASED WHEN THE CONNECTION REACHES CLOSED, not when DISCONNECT
 * is invoked. p. 2-26 is explicit that one DISCONNECT does not close anything --
 * "the symmetry that SCA builds into the concept of a connection requires that
 * the other SYSAP respond by also invoking the DISCONNECT service" -- so
 * releasing on invocation would free a descriptor the peer is still talking to.
 * A caller that drives the rest of the dialogue (vms-591 owns it) reaches CLOSED
 * and gets the release through scs_svc_close_if_closed().
 *
 * THE RELEASE ORDER is vms-17f's, minus one step, and the omission is
 * deliberate: Credit Wait flush (p. 2-45), then scs_cdl_release(), which returns
 * this connection's MFREEQ and DFREEQ deposits to the port (p. 2-43). What is
 * NOT done is calling the SYSAP's VC-LOSS handler. That handler is p. 2-28's
 * ERROR handler for a broken circuit; a SYSAP that asked for a disconnect has
 * not lost its circuit, and reporting a loss that did not happen is a lie in the
 * same family as a silent fallback. p. 2-26's own notification for this path is
 * the DISCONNECTED notify bit, which scs_conn.c returns in the transition.
 */
enum scs_svc_status scs_disconnect(struct scs_svc_port *port, struct scs_cdt *cdt,
                                   const struct scs_svc_args *args);

/*
 * scs_svc_close_if_closed - release `cdt` if and only if its connection state is
 * CLOSED. THE release path: Credit Wait flush, then scs_cdl_release() with its
 * MFREEQ/DFREEQ deposit return. Returns 1 if it released, 0 if not.
 *
 * Separate from scs_disconnect() because the transition that reaches CLOSED is
 * usually a RECEIVED message (Figure 2-16: DISC ACK --RCV_DISCONNECT_REQ-->
 * CLOSED, DISC MATCH --RCV_DISCONNECT_RSP--> CLOSED), and the receive dispatch
 * is not one of the five services. vms-591 wired that path; scs_svc_deliver()
 * below is it, and this stays exported because scsd.c also calls it directly.
 */
int scs_svc_close_if_closed(struct scs_svc_port *port, struct scs_cdt *cdt);

/* --- the RECEIVE side (vms-591) -------------------------------------------
 *
 * scs_svc_deliver - apply a RECEIVED SCS control message to `cdt`, EMIT
 * whatever the state machine says that message is answered with, and release
 * the CDT if the transition reached CLOSED.
 *
 * WHY IT HAD TO EXIST. Before vms-591 the daemon's receive path called
 * conn_step(), which RECORDS a transition and emits nothing -- so an arriving
 * DISCONNECT_REQ moved the CDT to DISC RECEIVED and the DISCONNECT_RSP that
 * Figure 2-16 draws on that same arrow was never sent, by construction. The
 * five services all emit through svc_emit(); the receive side had no
 * equivalent, and half of Figure 2-16 is receive-side.
 *
 * It is deliberately NOT a sixth service: p. 2-56 names five, and this is the
 * port driver delivering a message, not a SYSAP invoking anything. It shares
 * the services' emitter, their counters and their emit contract (a REFUSED
 * emit leaves the state untouched and returns SCS_SVC_NOTSENT).
 *
 * `ev` must be one of SCS_CONN_EV_RCV_* -- a local SYSAP invocation belongs to
 * the five services and SCS_CONN_EV_VC_LOST belongs to scs_vc_break(); either
 * one here returns SCS_SVC_BADARG rather than opening a second unaudited entry
 * point into the same machine.
 *
 * `*closed_out` (may be NULL) is set to 1 iff this delivery reached CLOSED and
 * the CDT was released -- which the caller needs, because after that the CDT
 * pointer must not be dereferenced again.
 */
enum scs_svc_status scs_svc_deliver(struct scs_svc_port *port, struct scs_cdt *cdt,
                                    enum scs_conn_event ev,
                                    const struct scs_svc_args *args,
                                    int *closed_out);

/*
 * scs_svc_disconnect_all - invoke DISCONNECT on every in-use connection on
 * circuit `vc` that the state machine has a DISCONNECT rule for, and return
 * how many were driven. `vc` NULL means every circuit.
 *
 * This is what "OVMX can INITIATE a disconnect" means at the port level, and
 * it is what the daemon runs at shutdown, once per peer.
 *
 * WHY IT TAKES A CIRCUIT. The frame is addressed FROM the CDT (its Con.ID
 * pair) but transmitted TO whatever peer the caller's emitter is bound to, so
 * a walk that ignored the circuit would send one peer's DISCONNECT_REQ to
 * another peer's MAC the moment OVMX has two peers.
 *
 * LISTENING CDTs ARE SKIPPED (p. 2-48: they describe no connection).
 * Connections in a state with no (state, SVC_DISCONNECT) row are skipped too,
 * not forced: from CLOSED or from the formation states there is no disconnect
 * to invoke, and from DISC SENT / DISC ACK / DISC MATCH one has already been
 * invoked. The TABLE decides which -- this function asks
 * scs_conn_table_lookup() and never encodes its own list of states, so a table
 * change cannot leave it stale.
 */
unsigned scs_svc_disconnect_all(struct scs_svc_port *port, const struct scs_pb *vc,
                                const struct scs_svc_args *args);

/* Zero the port and point it at a CDL. `cdl` may be NULL (descriptorless). */
void scs_svc_port_init(struct scs_svc_port *port, struct scs_cdl *cdl);

/*
 * Are descriptors available? 0 when OVMX_NO_CONN_FSM is set or the port has no
 * CDL -- the DESCRIPTORLESS mode described in the header note. Read fresh on
 * every call (never cached) so a test can bracket one call with setenv.
 */
int scs_svc_descriptors_available(const struct scs_svc_port *port);

#ifdef __cplusplus
}
#endif

#endif /* SCS_SVC_H */
