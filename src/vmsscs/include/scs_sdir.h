/*
 * scs_sdir.h - THE VMS QUEUE OF SCS DIRECTORY ENTRIES (SDIRs) AND THE
 * LISTENING CDTs (vms-7fe).
 *
 * SOURCE: VAXcluster Principles (Davis, 1993) ch. 2, pp. 2-48..2-50 and
 * Figure 2-24 (p. 2-49). Quoted with page cites; the chapter is host-only and
 * is never committed.
 *
 * p. 2-48 is the whole specification of this file:
 *
 *   "VMS integrates its implementation of the SCA list of listening SYSAPs with
 *    its mechanism for delivering connect requests to those SYSAPs. Figure 2-24
 *    shows that this list consists of a queue of SCS Directory Entries (SDIRs).
 *    When a VMS-based SYSAP invokes the LISTEN service, an SDIR containing the
 *    SYSAP's name is allocated and placed in this queue. Each SDIR contains the
 *    CONID of a special 'listening CDT' that is also allocated at this time.
 *    Instead of containing the address of a regular message input routine, a
 *    listening CDT contains the address of the SYSAP's routine for handling
 *    incoming connect requests."
 *
 *   "When a CONNECT_REQ is received on a VAX system, SCS scans this queue
 *    looking for an SDIR containing a SYSAP name matching the target name
 *    contained in the connect request. If none is found, SCS replies with a
 *    CONNECT_RSP containing the 'no such SYSAP' error. If an SDIR with a
 *    matching name is found, SCS uses the CONID in that SDIR to find the target
 *    SYSAP's listening CDT. The connect request is then delivered to the routine
 *    whose address is in the listening CDT (and hence, to the target SYSAP).
 *    SCS on the target node also replies with a CONNECT_RSP indicating that the
 *    CONNECT_REQ has successfully been delivered to the target SYSAP."
 *
 * p. 2-49, on what ACCEPT does:
 *
 *   "if the target SYSAP accepts the connection, a separate CDT (distinct from
 *    the listening CDT) is allocated to describe the new connection ... the
 *    'local CONID' used on the target node to identify the new connection is the
 *    CONID of this separate CDT, and NOT the CONID of the listening CDT."
 *
 * p. 2-50, on the one-at-a-time rule:
 *
 *   "When the connect request is delivered to the target SYSAP, SCS changes the
 *    state of its listening CDT to CONNECT RECEIVED. It remains in that state
 *    until a response is received to either the REJECT_REQ or ACCEPT_REQ, at
 *    which time the listening CDT is returned to the LISTEN state. If another
 *    connect request is received while the listening CDT is in the CONNECT
 *    RECEIVED state, SCS replies with a response that essentially says
 *    'busy ... try again later'. Thus, a SYSAP can handle only one incoming
 *    connect request at a time."
 *
 * ==========================================================================
 * THIS MODULE IS PURE STATE. IT BUILDS NO FRAME AND PARSES NO FRAME.
 * ==========================================================================
 *
 * It owns the queue, the listening CDTs, the scan, and the LISTEN /
 * CONNECT RECEIVED state of each listening CDT. It never transmits: it returns
 * an enum scs_sdir_result and the PORT DRIVER decides what frame that becomes.
 * That is the same p. 2-56 split scs_svc.h draws ("the actual dialogue for
 * connection formation ... is managed by SCS code in the port driver"), and it
 * is what keeps the scsd.c send census (tests/vmsscs/test_scsd_send_sites.py)
 * complete.
 *
 * The ONE exception is that the two REFUSAL STATUS VALUES below are declared
 * here, beside the results they correspond to, because they are the wire
 * meaning of those results. They are OVMX CHOICES -- see the block on them.
 *
 * ==========================================================================
 * WIRE STATUS -- WHAT IS GROUNDED AND WHAT IS AN OVMX CHOICE
 * ==========================================================================
 *
 * GROUNDED (docs/cluster-protocol-spec.md sec 4(h)(2), from our own captures):
 *   - the DIRECTORY LOOKUP miss marker is the literal 16-byte ASCII
 *     "NOT PRESENT HERE" in the lookup response's result field [78:94].
 *     scs_dir.c has shipped it since vms-246 and this item does not change a
 *     byte of it; what changes is WHO DECIDES, which moves from a hardcoded
 *     name compare in scsd.c to a scan of this queue.
 *   - the target SYSAP name of a CONNECT_REQ sits at payload [62:78],
 *     16 bytes, blank padded (sec 4(h)(2), "connect frame (SCA 21): target
 *     SYSAP 'SCS$DIRECTORY   ' (16-byte field [62:78])"). That is the scan key.
 *   - [46:48] == 1 is CONNECT_RSP (sec 4(h)(1a), 16 frames / 16 dialogues,
 *     0 residuals). OVMX's 66-byte CONNECT_RSP builder is
 *     scs_dir_build_connect_echo().
 *
 * NOT GROUNDED -- OVMX CHOICES, LABELLED (rule 8). p. 2-48 and p. 2-50 name a
 * "no such SYSAP" error and a "busy ... try again later" response but publish
 * no code for either, and NEITHER APPEARS ON ANY WIRE WE HOLD: every one of the
 * 16 CONNECT_RSP frames in formation-ci1.pcap is the positive kind (its target
 * was listening), and spec sec 4(h) records the companion word at [48:50] as
 * INFERRED. So:
 *
 *   - OVMX carries its refusal in [48:50], because that is the only field the
 *     positive CONNECT_RSP holds at zero and the only one sec 4(h)(2) even
 *     names as a "companion flag/status word". THE PLACEMENT IS A GUESS.
 *   - The VALUES are invented here. They are NOT VMS status codes, they are not
 *     $SSDEF values, and no capture contains them. A future capture of a real
 *     VAX refusing a connect request SUPERSEDES both, and the spec's sec 5
 *     register carries this as an open gap.
 *
 * BLAST RADIUS OF THE GUESS, measured rather than asserted: in the configuration
 * OVMX actually runs, neither refusal is ever emitted, because the only two
 * CONNECT_REQs the reference VAX addresses to OVMX name SCS$DIRECTORY and
 * VMS$VAXcluster and OVMX LISTENs for both -- both scans HIT and the wire is
 * byte-unchanged. tests/vmsscs/test_scsd_wire.c pins that (the golden
 * SCS$DIRECTORY and member CONNECT_REQ frames produce 0 refusals), and
 * tools/cluster/scsd_wire_diff.sh byte-diffs the whole replay script against
 * the pre-item tree.
 *
 * AND THE TWO VALUES ARE NOT EVEN EQUALLY EXERCISED. 0x0002 (no such SYSAP) is
 * at least emitted by scsd.c under a SYNTHESIZED frame -- test_scsd_wire.c case
 * (2d) substitutes the 16-byte name field of pcap #30 -- so the refusal's frame
 * shape is pinned even though its status value is invented. 0x0003 (busy) IS
 * NEVER EMITTED BY THE DAEMON AT ALL, for the reason DESIGN CHOICE 3 below
 * gives. That is measured rather than predicted: test_scsd_wire.c accumulates
 * scsd.c's sdir_busy_replies across every case in the file and asserts the
 * total is 0, and a live daemon prints busy-sent in its exit summary so the
 * claim also stays falsifiable in the lab. The only thing that reaches the BUSY
 * path is tests/vmsscs/test_scs_sdir.c, at this module's API. Anything in this
 * tree that describes the busy reply should say that.
 *
 * ==========================================================================
 * OVMX DESIGN CHOICES (labelled, rule 8)
 * ==========================================================================
 *
 * 1. THE LISTENING CDTs LIVE IN A RESERVED CONID BAND, slots 32..39. p. 2-48
 *    says a listening CDT is allocated at LISTEN time; it does not say where in
 *    the CDL. OVMX cannot let the generic allocator choose, because
 *    scs_cdl_alloc() hands out the lowest free slot from 1 upward and OVMX has
 *    already SHIPPED three fixed Con.IDs on the lab wire at slots 1, 2 and 7
 *    (scsd.c OVMX_LOCAL_CONID / OVMX_JOINER_CONID, scs_dir.h
 *    SCS_DIR_OVMX_CONID). A listening CDT landing on one of those would make
 *    scs_cdl_alloc_conid() refuse the connection that needs it, and OVMX would
 *    stop answering the member's CONNECT-REQUEST. So listening CDTs are claimed
 *    explicitly at SCS_SDIR_CONID_BASE + i. tests/vmsscs/test_scs_sdir.c pins
 *    that the three shipped Con.IDs are still allocatable after a full queue.
 *
 * 2. THE LISTEN / CONNECT RECEIVED STATE LIVES ON THE SDIR, NOT IN
 *    enum scs_conn_state. Figures 2-14/2-15/2-16 -- which are what
 *    scs_conn.h (vms-dd5) enumerates -- have no LISTEN state; p. 2-50 names one
 *    for the listening CDT only. Rather than invent a twelfth value in another
 *    item's table and risk indexing its transition matrix out of range, the
 *    listening CDT's own conn_state is left at CLOSED and the p. 2-50 state is
 *    kept here. The listening CDT is therefore a descriptor with a handler and
 *    a CONID, exactly as p. 2-48 describes it, and no connection state.
 *
 * 3. RETURN TO LISTEN HAPPENS WHEN THE ANSWER IS EMITTED, NOT WHEN ITS RESPONSE
 *    ARRIVES -- A DELIBERATE DEVIATION FROM p. 2-50, and the reason is honest:
 *    p. 2-50 holds the listening CDT in CONNECT RECEIVED until the ACCEPT_RSP
 *    or REJECT_RSP comes back, because a VMS SYSAP takes time to decide.
 *    OVMX's ACCEPT is SYNCHRONOUS -- scs_svc.h already labels that ("OVMX has
 *    no asynchronous SYSAP to hand it to, so the receive path decides and calls
 *    ACCEPT synchronously"). Emitting the answer is the point at which OVMX's
 *    SYSAP has finished deciding, so that is when the SDIR returns to LISTEN.
 *
 *    /!\ THE SECOND HALF OF THIS JUSTIFICATION WAS REFUTED (vms-70e2) AND HAS
 *    BEEN REMOVED. It read:
 *    REFUTED-QUOTE-BEGIN
 *      "decisively, OVMX HAS NEVER OBSERVED AN ACCEPT_RSP ADDRESSED TO ITSELF
 *       ... A listening CDT that waited for a frame that never comes would
 *       wedge in CONNECT RECEIVED with no timeout"
 *      -- REFUTED by run A1; quoted only to kill it.
 *    REFUTED-QUOTE-END
 *    OVMX HAS observed
 *    one, on this branch, on lab-2 pod vaxlab-4 (run A1, 2026-08-05, capture
 *    vms70e2-A1-lab2-vaxlab4-20260805.pcap): VAX1 answers OVMX's own
 *    SCS$DIRECTORY ACCEPT_REQ with a 62-byte [46:48]=3 ACCEPT_RSP addressed to
 *    the OVMX MAC and carrying OVMX's own Con.ID pair 4F580007/B751000C, 0.5 ms
 *    later, and the daemon logs the matching
 *    "ACCEPT SENT --RCV_ACCEPT_RSP--> OPEN" transition. The same frame is in
 *    run A3. The SYNCHRONOUS-ACCEPT reason above still stands on its own; the
 *    "it never comes" reason is dead and may not be restated. Spec sec 4(O)
 *    carries the measurement and
 *    tests/vmsscs/test_scs_join_capability_figures.py reds if the refuted
 *    sentence returns to this file or to the spec.
 *
 *    THE HONEST CONSEQUENCE, STATED SO NOBODY READS MORE INTO IT: the
 *    CONNECT RECEIVED window is one call deep, so under scsd.c's single
 *    threaded receive loop A SECOND REQUEST CANNOT ARRIVE INSIDE IT, and the
 *    p. 2-50 "busy" reply IS NOT REACHABLE FROM THE DAEMON TODAY. The busy
 *    counter is reported at exit and it is 0. It is reachable through this
 *    module's API -- a connect-request handler that itself receives -- and
 *    tests/vmsscs/test_scs_sdir.c exercises it exactly that way. Do not read
 *    that green test as "scsd.c sends busy replies". It does not.
 *
 * 4. A RETRANSMITTED CONNECT_REQ IS NOT A SECOND REQUESTER. scs_sdir_connect_req
 *    takes the requester's remote CONID; a request arriving in CONNECT RECEIVED
 *    from the SAME remote CONID is the same request again and is delivered
 *    again rather than refused busy. SCA has no retransmit concept here; the
 *    established VAX's retransmit-until-accepted behaviour is our own wire
 *    observation and scs_svc.h already carries it as a labelled OVMX addition
 *    (struct scs_svc_args::retransmit). Without this, item 3's window aside, a
 *    busy reply to a retransmit would break the join OVMX depends on.
 *
 *    THE DAEMON-LEVEL PROOF is test_scsd_wire.c case (2e): it holds
 *    SCS$DIRECTORY in CONNECT RECEIVED for the golden requester and feeds the
 *    golden 0x5b CONNECT_REQ through scsd_handle_frame(), which takes THIS
 *    branch (retransmits++) and not the busy one -- which also pins that
 *    scsd.c passes the frame's own requester Con.ID into the scan. That is a
 *    synthetic arrangement and the case says so; item 3's statement that the
 *    receive loop cannot produce it stands.
 */
#ifndef SCS_SDIR_H
#define SCS_SDIR_H

#include <stddef.h>
#include <stdint.h>

#include "scs_cdt.h" /* struct scs_cdl / struct scs_cdt, SCS_CDT_SYSAP_NAME_LEN */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How many SYSAP names may be queued at once. VMS's queue is dynamically
 * allocated with no published bound (p. 2-48); OVMX uses a fixed array for the
 * same reason every other pool in src/vmsscs/ is fixed -- an executive path
 * gets no allocation failure. OVMX DESIGN CHOICE.
 *
 * It is deliberately the same number as SCS_SVC_MAX_LISTENERS (scs_svc.h):
 * this queue IS that list now, and one bound is less to keep in step than two.
 */
#define SCS_SDIR_MAX 8

/*
 * The reserved listening-CDT CONID band -- see OVMX DESIGN CHOICE 1. Slot
 * 32 + i for i in [0, SCS_SDIR_MAX). Chosen to sit above OVMX's three shipped
 * Con.IDs (slots 1, 2, 7) and below SCS_CDT_SCSCONNCNT (40) so a listening CDT
 * never consumes a p. 2-30 overflow entry.
 */
#define SCS_SDIR_CONID_SLOT_BASE 32u
#define SCS_SDIR_CONID_BASE      SCS_CDT_MAKE_CONID(SCS_SDIR_CONID_SLOT_BASE)

/*
 * ---- THE TWO REFUSAL STATUS WORDS: OVMX CHOICES, NOT GROUNDED ----
 *
 * Written to CONNECT_RSP payload [48:50]. See the WIRE STATUS block above for
 * why the placement is a guess and the values are invented. 0 is the value the
 * captured positive CONNECT_RSP carries and is what OVMX keeps sending on the
 * HIT path, so a successful delivery stays byte-identical.
 */
#define SCS_SDIR_STATUS_OK            0x0000u /* GROUNDED: the golden CONNECT_RSP's [48:50] */
#define SCS_SDIR_STATUS_NO_SUCH_SYSAP 0x0002u /* OVMX CHOICE -- p. 2-48 "no such SYSAP" */
#define SCS_SDIR_STATUS_BUSY          0x0003u /* OVMX CHOICE -- p. 2-50 "busy ... try again later".
                                               * NEVER PUT ON THE WIRE BY scsd.c: see DESIGN CHOICE 3.
                                               * MEASURED: test_scsd_wire.c accumulates scsd.c's
                                               * sdir_busy_replies across every case and asserts the
                                               * total is 0. The only thing that reaches the BUSY path
                                               * at all is test_scs_sdir.c, at this module's API. */

/* p. 2-50's two states of a listening CDT. */
enum scs_sdir_state {
    SCS_SDIR_LISTEN = 0,          /* "returned to the LISTEN state" */
    SCS_SDIR_CONNECT_RECEIVED = 1 /* "SCS changes the state of its listening CDT to CONNECT RECEIVED" */
};

/* What the p. 2-48 scan decided. The port driver turns this into a frame. */
enum scs_sdir_result {
    SCS_SDIR_DELIVERED = 0,     /* match found, handler called; reply CONNECT_RSP (status OK) */
    SCS_SDIR_NO_SUCH_SYSAP = 1, /* "If none is found, SCS replies with a CONNECT_RSP
                                 * containing the 'no such SYSAP' error" (p. 2-48) */
    SCS_SDIR_BUSY = 2,          /* p. 2-50 "busy ... try again later" -- NOT PRODUCIBLE BY
                                 * scsd.c (DESIGN CHOICE 3); reached only through this
                                 * module's API, and only by tests/vmsscs/test_scs_sdir.c */
    SCS_SDIR_BADARG = 3         /* NULL queue/name -- not a wire outcome */
};

const char *scs_sdir_result_name(enum scs_sdir_result r);
const char *scs_sdir_state_name(enum scs_sdir_state s);

/* p. 2-48: "a listening CDT contains the address of the SYSAP's routine for
 * handling incoming connect requests". `remote_conid` is the requester's own
 * handle, carried so a handler can tell requesters apart. */
typedef void (*scs_sdir_connect_req_fn)(const char *local_sysap,
                                        const char *remote_sysap,
                                        uint32_t remote_conid, void *ctx);

/*
 * struct scs_sdir - one SCS Directory Entry. Figure 2-24 draws exactly two
 * fields in the box, SYSAP NAME and CONID, plus the queue links; both are here
 * and nothing else is invented into it.
 */
struct scs_sdir {
    int in_use;
    char sysap[SCS_CDT_SYSAP_NAME_LEN + 1]; /* "SYSAP NAME" (Figure 2-24) */
    uint32_t conid;                         /* "CONID" -- of the listening CDT (Figure 2-24) */

    /* Figure 2-24's queue links. */
    struct scs_sdir *next;
    struct scs_sdir *prev;

    /* p. 2-50's state of the listening CDT -- see OVMX DESIGN CHOICE 2. */
    enum scs_sdir_state state;

    /* Set while state == CONNECT RECEIVED: the requester whose CONNECT_REQ is
     * outstanding (OVMX DESIGN CHOICE 4, the retransmit discriminator). */
    uint32_t pending_remote_conid;
};

/*
 * struct scs_sdir_queue - "VMS QUEUE OF SCS DIRECTORY ENTRIES (LIST OF
 * LISTENING SYSAPS)" (Figure 2-24). `cdl` is where the listening CDTs come
 * from; without one, LISTEN cannot allocate a listening CDT and fails honestly
 * (rule 9 / INV-6 -- there is no "SDIR without a CDT" degraded mode).
 */
struct scs_sdir_queue {
    struct scs_cdl *cdl;
    struct scs_sdir entry[SCS_SDIR_MAX];
    struct scs_sdir *head;
    struct scs_sdir *tail;

    /* Every claim this file makes about what it DID is a number a test reads. */
    unsigned long listens;        /* SDIRs placed in the queue */
    unsigned long scans;          /* p. 2-48 scans performed */
    unsigned long hits;           /* scans that found a matching SDIR */
    unsigned long delivered;      /* connect requests handed to a listening CDT's routine */
    unsigned long no_such_sysap;  /* p. 2-48 refusals */
    unsigned long busy;           /* p. 2-50 refusals. Never moved by scsd.c (DESIGN CHOICE 3,
                                   * measured by test_scsd_wire.c's end-of-run total); moved
                                   * only by test_scs_sdir.c, through this module's API */
    unsigned long retransmits;    /* re-deliveries under OVMX DESIGN CHOICE 4 */
};

/* Zero the queue and point it at the CDL the listening CDTs come from. */
void scs_sdir_queue_init(struct scs_sdir_queue *q, struct scs_cdl *cdl);

/*
 * scs_sdir_listen - p. 2-48: "an SDIR containing the SYSAP's name is allocated
 * and placed in this queue. Each SDIR contains the CONID of a special
 * 'listening CDT' that is also allocated at this time."
 *
 * Both allocations, in that order, or neither. Returns the SDIR, or NULL if
 * the queue is full, the name is already queued, the queue has no CDL, or the
 * listening CDT could not be claimed at its reserved CONID.
 *
 * `on_connect_req` is p. 2-48's "address of the SYSAP's routine for handling
 * incoming connect requests" and may be NULL: a SYSAP with no handler still
 * declares itself listening (which is what the Directory Service answers on),
 * and a delivery to it counts as delivered with nothing to call. scsd.c's two
 * SYSAPs are in exactly that position today.
 */
struct scs_sdir *scs_sdir_listen(struct scs_sdir_queue *q, const char *sysap,
                                 scs_sdir_connect_req_fn on_connect_req, void *ctx);

/*
 * scs_sdir_lookup - THE p. 2-48 SCAN, and the SCS Directory Service's whole
 * question (p. 2-22: "responds to inquires from other nodes wanting to know if
 * particular SYSAP names are in this list"). `sysap` may be a 16-byte
 * blank-padded wire field; trailing blanks and NULs are ignored on both sides.
 * Returns the SDIR or NULL. Counts a scan and, on a match, a hit.
 */
const struct scs_sdir *scs_sdir_lookup(struct scs_sdir_queue *q, const char *sysap);

/* Non-counting variant, for callers that only want the answer (the exit
 * report, scs_svc_listening()). */
const struct scs_sdir *scs_sdir_peek(const struct scs_sdir_queue *q, const char *sysap);

/* The listening CDT an SDIR names, via its CONID (p. 2-48: "SCS uses the CONID
 * in that SDIR to find the target SYSAP's listening CDT"). NULL if the SDIR is
 * not queued or its CDT was released. */
struct scs_cdt *scs_sdir_listening_cdt(const struct scs_sdir_queue *q,
                                       const struct scs_sdir *sdir);

/*
 * scs_sdir_connect_req - the p. 2-48 receive rule, whole:
 *
 *   scan for `target_sysap`
 *     no match                        -> SCS_SDIR_NO_SUCH_SYSAP
 *     match, listening                -> deliver to the listening CDT's routine,
 *                                        move it to CONNECT RECEIVED,
 *                                        return SCS_SDIR_DELIVERED
 *     match, CONNECT RECEIVED, same
 *       remote_conid                  -> deliver again (OVMX CHOICE 4),
 *                                        return SCS_SDIR_DELIVERED
 *     match, CONNECT RECEIVED, other
 *       remote_conid                  -> SCS_SDIR_BUSY (p. 2-50)
 *
 * THE LAST ROW IS UNREACHABLE FROM scsd.c. Its precondition is a listening CDT
 * still in CONNECT RECEIVED when a DIFFERENT requester's frame arrives, and the
 * daemon's receive loop answers synchronously (DESIGN CHOICE 3), so it is never
 * in that state between frames. Measured, not assumed: test_scsd_wire.c sums
 * scsd.c's sdir_busy_replies across every case it runs and asserts the total is
 * 0. tests/vmsscs/test_scs_sdir.c is the only thing that takes that row, and it
 * does so through this API, not through the daemon.
 *
 * `*sdir_out` (may be NULL) receives the matched SDIR on DELIVERED, so the
 * caller can pass it back to scs_sdir_connect_answered().
 */
enum scs_sdir_result scs_sdir_connect_req(struct scs_sdir_queue *q,
                                          const char *target_sysap,
                                          const char *remote_sysap,
                                          uint32_t remote_conid,
                                          const struct scs_sdir **sdir_out);

/*
 * scs_sdir_connect_answered - return the listening CDT to LISTEN.
 *
 * p. 2-50 returns it when the ACCEPT_RSP/REJECT_RSP arrives; OVMX returns it
 * when the ACCEPT/REJECT answer has been emitted (or abandoned). See OVMX
 * DESIGN CHOICE 3 for why, and for the consequence. Call it once per
 * SCS_SDIR_DELIVERED, whatever the port did with the frame. Idempotent.
 */
void scs_sdir_connect_answered(struct scs_sdir_queue *q, const struct scs_sdir *sdir);

/* Queue walk, in Figure 2-24 order. */
const struct scs_sdir *scs_sdir_first(const struct scs_sdir_queue *q);
const struct scs_sdir *scs_sdir_next(const struct scs_sdir *sdir);
unsigned scs_sdir_count(const struct scs_sdir_queue *q);

/*
 * scs_sdir_target_name - lift the target SYSAP name out of a received
 * CONNECT_REQ frame. GROUNDED offset: payload [62:78] == absolute [76:92],
 * 16 bytes, blank padded (docs/cluster-protocol-spec.md sec 4(h)(2)).
 * `frame` points at the Ethernet destination (absolute 0). Writes a
 * NUL-terminated, blank-trimmed name. Returns 0, or -1 if the frame is too
 * short to carry the field.
 *
 * This reads a frame and builds none: keeping it here rather than in the port
 * driver keeps the scan key beside the scan.
 */
int scs_sdir_target_name(const uint8_t *frame, size_t len,
                         char out[SCS_CDT_SYSAP_NAME_LEN + 1]);

/*
 * scs_sdir_enabled - the OVMX_NO_SDIR kill switch. 0 when OVMX_NO_SDIR is set
 * to anything other than "0", in which case the caller must fall back to its
 * pre-vms-7fe behaviour. Re-read on EVERY call (never cached) so a test can
 * bracket one dispatch with setenv/unsetenv, exactly as scs_conn_fsm_enabled()
 * does.
 */
int scs_sdir_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* SCS_SDIR_H */
