/*
 * scs_cdt.h - SCA Connection Descriptor Tables (CDTs), the Connection
 * Descriptor List (CDL) that addresses them by CONID, and the per-Path-Block
 * connection queue (vms-e1a).
 *
 * WHY THIS EXISTS. Before this module OVMX had no object called "a
 * connection". It had three node-global Con.ID macros in scsd.c
 * (OVMX_LOCAL_CONID / OVMX_JOINER_CONID / SCS_DIR_OVMX_CONID) and a scatter of
 * `int connected` / `uint32_t *_remote_conid` flags inside `struct peer_state`,
 * dispatched by comparing a received Con.ID against a compile-time constant.
 * That is not the architecture: in SCA a connection IS a data structure -- the
 * Connection Block, which the VMS implementation calls a Connection Descriptor
 * Table -- reachable from a 32-bit Connection Identifier through the
 * Connection Descriptor List, and queued to the Path Block of the virtual
 * circuit that carries it. Every SCA rule about connection loss, delivery and
 * SYSAP notification is written in terms of those structures. This module
 * supplies them.
 *
 * SOURCE (public, quoted with page cites -- CLAUDE.md rule 8):
 *   Roy G. Davis, *VAXcluster Principles*, Digital Press 1993, ch. 2.
 *   - CDT contents; two mirror-image CDTs per connection; ALL CDTs for a
 *     circuit queued to its Path Block; the VC-loss error handler stored in
 *     the CDT; the 32-bit CONID and its exchange in CONNECT_REQ /
 *     ACCEPT_REQ                                                       p. 2-28
 *   - The CDL; "the low order 16 bits of the CONID associated with a CDT are
 *     used as an index into the CDL to locate the CDT's address"; the message
 *     and datagram input routines stored in the CDT and supplied as arguments
 *     to the CONNECT and ACCEPT services; destination-CONID delivery  p. 2-29
 *   - CDL sizing: "The number of entries in the CDL is equal to the value of
 *     the SYSGEN parameter SCSCONNCNT plus 200 (100 in earlier versions of
 *     VMS)"; SCSCONNCNT CDTs pre-allocated into the first SCSCONNCNT entries;
 *     released-but-not-deallocated reuse; up to 200 more allocated "as
 *     needed" into the 200 extra entries                               p. 2-30
 *   - "each packet contains source and destination CONIDs ... The source CONID
 *     comes from the local CONID field of that CDT; and the destination CONID
 *     comes from the remote CONID field of the CDT."                   p. 2-35
 *
 * THIS MODULE IS PURE STATE AND DELIVERY. It builds no frame and parses no
 * frame. It is wire-invisible by construction -- see the WIRE VERDICT below.
 *
 * ============================ WIRE VERDICT ================================
 * vms-e1a was dispatched as "wire-visible MAYBE (source CONID)": p. 2-35's
 * both-CONIDs-in-every-packet rule might have required OVMX to fill a source
 * CONID it currently leaves zero. IT DOES NOT. Measured over our own lab
 * captures (formation-ci1-joinwindow.pcap, 3000 frames, and
 * vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap, 19930 frames), classifying
 * every 0x13-format SCS frame by total SCA length and by whether abs 64
 * (remote/destination Con.ID) and abs 68 (local/source Con.ID) are zero:
 *
 *   - EVERY Con.ID-bearing class the VAXen emit carries BOTH Con.IDs nonzero,
 *     with exactly two principled exceptions:
 *   - the 110-byte CONNECT_REQ carries destination Con.ID = 0 (the peer's CDT
 *     does not exist yet -- p. 2-28: the CONIDs are exchanged DURING formation);
 *   - the 66-byte class (0x4b and 0x5b) carries source Con.ID = 0 on the real
 *     VAX wire, in 100% of observed frames (31 of 31 across both captures).
 *
 * OVMX's builders already reproduce all three shapes byte-exactly, because the
 * templates ARE those captured frames: scs_connect.c substitutes both Con.IDs,
 * scs_dir_build_connect_echo (the 66-byte class) deliberately leaves the source
 * Con.ID at the captured zero, and the 94/110/190-byte builders write both.
 * There is therefore NO byte to change and no kill-switch to ship: an
 * OVMX_NO_SRC_CONID switch would have nothing to switch off. Filling the
 * 66-byte class's source Con.ID "because p. 2-35 says so" would have been a
 * REGRESSION away from the observed wire. Ground truth beat the book here, per
 * the source-of-truth rule.
 * ==========================================================================
 *
 * OVMX DESIGN CHOICES (labeled per rule 8 -- the book documents the CONTENTS
 * of a CDT and the CDL indexing rule, not any byte layout; nothing here reaches
 * the wire except the 32-bit CONID value itself):
 *   - CONID composition. The book fixes only that the low 16 bits index the
 *     CDL (p. 2-29) and that the whole 32-bit number uniquely identifies the
 *     CDT (p. 2-28); it does not publish what the high 16 bits are. OVMX uses
 *     a constant recognizable tag, SCS_CDT_CONID_TAG == 0x4F58 ("OX"), so OVMX
 *     connections stand out in a capture. This is deliberately the SAME base
 *     that scsd.c has been putting on the wire since vms-5fe
 *     (SCS_CONNECT_OVMX_CONID_BASE), which means the three Con.IDs OVMX
 *     already ships are legal CDL slots under the p. 2-29 rule with no wire
 *     change: 0x4F580001 -> slot 1, 0x4F580002 -> slot 2, 0x4F580007 -> slot 7.
 *     tests/vmsscs/test_scs_cdt.c pins that correspondence.
 *   - A zero CONID is never allocated. The tag makes every allocated CONID
 *     nonzero, which preserves OVMX's existing use of 0 as the "peer's Con.ID
 *     not yet known" sentinel (scs_connect.h, scs_dir.c).
 *   - Fixed-capacity pools instead of dynamic allocation (same rationale as
 *     scs_config.h): the daemon gets no allocation failure path. The p. 2-30
 *     TWO-TIER structure is preserved exactly -- SCS_CDT_SCSCONNCNT CDTs are
 *     pre-placed in CDL slots 0..SCSCONNCNT-1 at init, and up to
 *     SCS_CDT_CDL_OVERFLOW (200) more are handed out "as needed" into the
 *     extra slots -- because that two-tier shape is the architecture, but the
 *     backing storage is a static array rather than pool allocation.
 *   - SCS_CDT_SCSCONNCNT's DEFAULT VALUE is an OVMX choice, not a VMS
 *     quantity. On VMS it is a SYSGEN parameter; OVMX has no SYSGEN plumbing
 *     for it yet, so it is a compile-time knob. The +200 overflow count is the
 *     book's number (p. 2-30).
 *   - SYSAP input routines take an explicit void* context. VMS passes none
 *     (the CDT address is the context); C callbacks in a daemon need one.
 *   - `conn_state` is an OPAQUE int here. This module stores it and never
 *     interprets it. The state values and every transition between them belong
 *     to the SCS connection state machine, which is deliberately NOT
 *     implemented here. It now EXISTS, in scs_conn.{h,c} (vms-dd5): the values
 *     are enum scs_conn_state and the transitions are the tables of Figures
 *     2-14/2-15/2-16. Nothing below changed to accommodate it.
 *
 * ===== REACHABILITY IN SCSD TODAY -- DO NOT READ A GREEN TEST RUN AS "OVMX
 * IMPLEMENTS THIS" =====
 *
 * This module and tests/vmsscs/test_scs_cdt.c implement and exercise the full
 * p. 2-28..2-30 model.
 *
 * AS OF vms-dd5 THE DAEMON USES IT -- PARTLY. scsd.c now links this module,
 * initializes one node-wide CDL, and allocates a CDT for each of the three
 * connections it forms per peer (SCS$DIRECTORY, the member-opened
 * VMS$VAXcluster, and the joiner-opened VMS$VAXcluster), claiming each at the
 * EXACT node-global Con.ID it already put on the wire via scs_cdl_alloc_conid.
 * Those CDTs carry live connection state driven by scs_conn.c.
 *
 * AS OF vms-7c0 RECEIVED APPLICATION MESSAGES ARE ROUTED THROUGH THE CDL. The
 * paragraph that used to stand here said they were not, and that is no longer
 * the case: scsd.c's receive dispatch decodes the SCS header (src/vmsscs/
 * scs_rx.c), and every frame whose message type is 10 -- the p. 4-13
 * APPLICATION MESSAGE, grounded in docs/cluster-protocol-spec.md sec 4(h)(1b)
 * -- is handed to scs_cdl_deliver_message(), which resolves the destination
 * Con.ID here and calls the CDT's msg_input. The five services install that
 * routine on every CDT they open (scsd.c's four scs_svc_args sites). The
 * VMS$VAXcluster connection-manager dialogue, which used to decide for itself
 * whether a frame was OVMX's by comparing Con.IDs against three macros, IS that
 * routine now. Measured: gcov over tests/vmsscs/test_scsd_wire.c driving real
 * captured frames shows scs_cdl_deliver_message and scs_cdl_resolve at 100%
 * line coverage, entered only from scsd_handle_frame().
 *
 * WHAT IS STILL NOT TRUE, so no one reads more into a green test run than is
 * there:
 *   - scs_cdl_deliver_datagram() STILL HAS NO PRODUCTION CALLER, and that is a
 *     measurement rather than an omission. Over 141 reference-lab pcaps and
 *     981,367 envelope-conformant SCA frames the SCS message-type namespace is
 *     exactly {0..10} -- 0..9 connection control, 10 the application message --
 *     so no application DATAGRAM class has ever been observed, and p. 4-68's
 *     credit==0 rule is necessary rather than sufficient (24.1% of MTYPE-10
 *     frames carry credit 0). OVMX cannot recognise an inbound datagram from
 *     this wire and does not pretend to; see the census in scs_rx.h and the
 *     rx_unknown_mtype counter that will notice the first candidate. THE COUNTS
 *     ABOVE ARE A DATED SNAPSHOT, THE CONCLUSION IS NOT. The reference lab keeps
 *     writing captures, so the corpus GROWS and a later re-derivation will not
 *     reproduce these totals -- a re-run on 2026-08-05 read 154 pcaps /
 *     1,002,247 envelope-conformant frames (942,251 real-VAX-source) and got
 *     MTYPE 0..10 with MTYPE 10 at 923,678 / 23.9% credit-0. A DIFFERENT TOTAL
 *     IS NOT A REGRESSION; a different SHAPE would be. What must still hold on
 *     re-run, and what the code depends on, is: the MTYPE namespace is exactly
 *     {0..10}, MTYPE 10 dominates, and no eleventh value appears. If an MTYPE
 *     outside {0..10} ever shows up, scsd's rx_unknown_mtype counter moves and
 *     the datagram question is finally answerable.
 *   - CONNECTION-CONTROL messages (types 0..9) are still NOT delivered through
 *     the CDL -- they drive the connection state machine, which is right:
 *     p. 2-29's delivery is to a SYSAP, and control messages are SCS's own.
 *   - `struct peer_state`'s connected / dir_connected / joiner_connected
 *     booleans still gate every send. The CDT state is a RECORD of what those
 *     booleans did, not a replacement for them.
 *   - OVMX's Con.IDs are STILL node-global, so only ONE peer's connections can
 *     occupy the three CDL slots. A second peer's connections are not tracked;
 *     scsd.c logs SCSD-W-CONNSLOT and carries on rather than allocating a
 *     Con.ID that differs from the one on the wire.
 *   - msg_input IS installed now (vms-7c0, see above). dgram_input is still
 *     never installed, for the reason measured above -- not for want of a
 *     place to install it.
 *   - vc_loss_handler IS installed now: as of vms-abc, scsd.c's conn_bind()
 *     puts scsd_sysap_vc_loss() on every CDT it creates, so
 *     scs_cdl_vc_loss()'s notification loop notifies somebody. It has TWO
 *     production callers, and both therefore reach that handler:
 *     scs_pb_depart() (vms-17f, the departure sweep, gated by
 *     OVMX_NO_PEER_DEPART) and scs_vc_break() (vms-abc, the p. 2-31
 *     message-guarantee failures, gated by OVMX_NO_VC_BREAK).
 */
#ifndef SCS_CDT_H
#define SCS_CDT_H

#include <stddef.h>
#include <stdint.h>

#include "scs_config.h" /* struct scs_pb, struct scs_sb */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SYSAP name field width. The wire's SYSAP-name fields are 16 bytes,
 * blank-padded and NOT NUL-terminated ("VMS$VAXcluster  ", "SCS$DIRECTORY   ")
 * -- see scs_connect.h [62:78]/[78:94] and scs_dir.h [62:78]. The CDT holds
 * the name as a NUL-terminated C string of that width (+1) so it can be
 * printed; the trailing blanks are preserved exactly as stored.
 */
#define SCS_CDT_SYSAP_NAME_LEN 16

/*
 * Connect data (p. 2-28): "Connect data (if any) provided by a SYSAP to be
 * included in a CONNECT_REQ packet or an ACCEPT_REQ packet". This module only
 * CARRIES it; its content and its placement in a frame are vms-fdd's. The
 * capacity is an OVMX choice.
 */
#define SCS_CDT_CONNECT_DATA_MAX 16

/*
 * CDL sizing, p. 2-30. SCS_CDT_SCSCONNCNT stands in for the SYSGEN parameter
 * SCSCONNCNT (OVMX default, see the header note); SCS_CDT_CDL_OVERFLOW is the
 * book's +200. Total CDL entries = SCSCONNCNT + 200.
 */
#ifndef SCS_CDT_SCSCONNCNT
#define SCS_CDT_SCSCONNCNT 40
#endif
#define SCS_CDT_CDL_OVERFLOW 200
#define SCS_CDL_ENTRIES (SCS_CDT_SCSCONNCNT + SCS_CDT_CDL_OVERFLOW)

/*
 * High 16 bits of every CONID OVMX allocates (OVMX design choice; see the
 * header note). Keep this equal to SCS_CONNECT_OVMX_CONID_BASE >> 16 -- the
 * value scsd.c already puts on the wire. test_scs_cdt.c asserts the equality
 * so the two cannot drift apart silently.
 */
#define SCS_CDT_CONID_TAG 0x4F58u

/*
 * vms-578: THE TAG IS A DEFAULT, NOT A CONSTANT ANY MORE.
 *
 * worktree-760 made the high half of every OVMX Con.ID PER-INCARNATION
 * (scsd.c's ovmx_conid_base(): a clock+pid mix, or OVMX_CONID_BASE). The reason
 * is vms-2f3's: a node that returns must not present the handles its previous
 * incarnation used. That is incompatible with a compile-time tag -- the CDL
 * refused every real Con.ID as "not one this node could have issued", so no
 * connection got a CDT at all and the whole vms-e1a/vms-dd5 layer went dark
 * (MEASURED: 19 assertions in test_scsd_wire.c, every one of them a missing
 * CDT).
 *
 * So the tag becomes SETTABLE, defaulting to 0x4F58. Whoever chooses the
 * incarnation's base calls scs_cdt_set_conid_tag() once at startup; everything
 * else keeps asking. The CHECK is unchanged in strength -- a Con.ID whose high
 * half is not THIS node's is still refused -- it is just asking a variable now.
 */
void scs_cdt_set_conid_tag(uint16_t tag);
uint16_t scs_cdt_conid_tag(void);

/* Compose / decompose a CONID (p. 2-29: the low 16 bits index the CDL). */
#define SCS_CDT_MAKE_CONID(slot) (((uint32_t)scs_cdt_conid_tag() << 16) | ((uint32_t)(slot) & 0xFFFFu))
#define SCS_CDT_CONID_SLOT(conid) ((unsigned)((uint32_t)(conid) & 0xFFFFu))

struct scs_cdt;

/*
 * SYSAP entry points stored in the CDT (p. 2-28 for the VC-loss error handler,
 * p. 2-29 for the message and datagram input routines). All three "are supplied
 * as arguments to the CONNECT and ACCEPT services".
 */
typedef void (*scs_msg_input_fn)(struct scs_cdt *cdt, const void *buf, size_t len, void *ctx);
typedef void (*scs_dgram_input_fn)(struct scs_cdt *cdt, const void *buf, size_t len, void *ctx);
typedef void (*scs_vc_loss_fn)(struct scs_cdt *cdt, void *ctx);

/*
 * vms-1d2 flow-control hooks. Declared here because the CDT carries them
 * (p. 2-45: SCS keeps the per-connection flow-control state in the CDT), but
 * defined and driven entirely by src/vmsscs/scs_credit.c -- see scs_credit.h.
 *
 * struct scs_credit_waiter is the OVMX stand-in for the VMS CDRP that p. 2-45
 * queues to the CDT in a Credit Wait; it is opaque to this module.
 */
struct scs_credit_waiter;
typedef void (*scs_credit_special_fn)(struct scs_cdt *cdt, unsigned credit, void *ctx);

/*
 * struct scs_cdt - Connection Descriptor Table entry: SCS's description of one
 * connection, from this node's point of view. Contents per p. 2-28/2-29.
 *
 * "there are two CDTs associated with an open connection ... the two CDTs are
 * effectively mirror images of each other" (p. 2-28) -- this is OUR half.
 */
struct scs_cdt {
    int in_use; /* 0 = released (p. 2-30: released but NOT deallocated) */

    /*
     * vms-7fe: THIS CDT IS A LISTENING CDT, NOT A CONNECTION.
     *
     * p. 2-48: "Each SDIR contains the CONID of a special 'listening CDT' that
     * is also allocated at this time. Instead of containing the address of a
     * regular message input routine, a listening CDT contains the address of
     * the SYSAP's routine for handling incoming connect requests." So it is a
     * CDT in every structural sense -- it occupies a CDL slot and has a CONID --
     * but it describes no connection: it has no peer, no circuit, no remote
     * CONID and no Figure 2-14 state. Set only by scs_sdir_listen()
     * (src/vmsscs/scs_sdir.c); everything that reports on CONNECTIONS must skip
     * it, which is why the flag is explicit rather than inferred from "pb is
     * NULL and remote_sysap is empty" -- a connection CDT is briefly in that
     * shape too, between allocation and binding.
     */
    int listening;

    /* p. 2-28: names of the local and remote SYSAPs on this connection. */
    char local_sysap[SCS_CDT_SYSAP_NAME_LEN + 1];
    char remote_sysap[SCS_CDT_SYSAP_NAME_LEN + 1];

    /* p. 2-28: "State of the connection." OPAQUE HERE -- vms-dd5 owns the
     * values and every transition; this module only stores and reports it. */
    int conn_state;

    /* p. 2-28: pointer to the System Block for the node at the other end, and
     * to the Path Block for the virtual circuit that supports the connection. */
    struct scs_sb *sb;
    struct scs_pb *pb;

    /* p. 2-28: connect data provided by a SYSAP for CONNECT_REQ / ACCEPT_REQ.
     * Carried, never interpreted (vms-fdd owns the content). */
    uint8_t connect_data[SCS_CDT_CONNECT_DATA_MAX];
    size_t  connect_data_len;

    /* p. 2-28/2-29: the SYSAP entry points. */
    scs_vc_loss_fn     vc_loss_handler;
    scs_msg_input_fn   msg_input;
    scs_dgram_input_fn dgram_input;
    void              *sysap_ctx; /* OVMX design choice, see header note */

    /* p. 2-28: the two CONIDs. `local_conid` identifies THIS CDT and indexes
     * the CDL; `remote_conid` is the peer's, learned during formation from a
     * CONNECT_REQ or ACCEPT_REQ, and is what OVMX writes as the DESTINATION
     * CONID of every message and datagram it sends (p. 2-35). 0 = not yet
     * learned. */
    uint32_t local_conid;
    uint32_t remote_conid;

    /* p. 2-28: "SCA specifies that all CDTs corresponding to connections
     * supported by a virtual circuit be queued to the Path Block corresponding
     * to that circuit." Head of the queue lives in struct scs_pb (scs_config.h). */
    struct scs_cdt *pb_next;
    struct scs_cdt *pb_prev;

    /*
     * vms-76e: the message-service flow control account for this connection.
     * "Since flow control for the SCS message service is done on a per
     * connection basis, SCS maintains this information for each connection in
     * the CDT it uses to describe the connection." (p. 2-45)
     *
     * Owned entirely by src/vmsscs/scs_credit.c -- scs_cdt.c only zeroes these
     * with the rest of the CDT at open and never reads them. See scs_credit.h
     * for the page cites, the grounded wire offset of the credit field, and the
     * honest statement that NOTHING IN scsd.c CALLS ANY OF IT YET.
     */

    /* p. 2-43: number of buffers this node BELIEVES exist on the remote node to
     * receive messages on this connection. Debited per message sent, raised by
     * the credit field of every message received. */
    unsigned send_credit;

    /* p. 2-44: "effectively a mirror image of remote SCS's Send Credit count" --
     * what the remote is believed to think IT may send us. */
    unsigned receive_credit;

    /* p. 2-43: buffers freed locally that the remote does not know about yet;
     * piggybacked into the credit field of the next outbound message and then
     * reset to 0 (p. 2-44). */
    unsigned pending_receive_credit;

    /* p. 2-43/2-45: this connection's share of the port MFREEQ -- the N Send
     * Credits the local SYSAP extended at connection formation. */
    unsigned extended_credits;

    /* p. 2-44: the Minimum Send Credits argument the REMOTE SYSAP passed to
     * CONNECT/ACCEPT; the dangerously-low threshold is compared against it. */
    unsigned remote_min_send_credits;

    /*
     * vms-1d2: the p. 2-45 CREDIT WAIT queue, and the p. 2-44 special credit
     * message emitter. Both are owned entirely by src/vmsscs/scs_credit.c --
     * scs_cdt.c only zeroes them with the rest of the CDT at open and never
     * reads them.
     *
     * "If no Send Credits are available, then this routine temporarily suspends
     * the operation involved by placing it in a Credit Wait. This is done by
     * queuing the CDRP representing the operation to the CDT for the
     * connection." (p. 2-45) -- so the queue head belongs HERE, on the CDT, and
     * not in a side table.
     *
     * The queue is FIFO: "Each of the SCS wait queues described here is a
     * 'first in first out' queue ... Queue priority is based on time spent in
     * the queue." (p. 2-46) `credit_wait_tail` is the OVMX means of keeping it
     * FIFO with an O(1) append; VMS's queue primitives are doubly linked and
     * the book publishes no layout for them.
     *
     * `credit_wait_draining` is an OVMX reentrancy guard, not an SCA concept:
     * a resumed waiter may itself send or receive on this connection, and a
     * nested drain of the same queue would resume waiters out of order.
     */
    struct scs_credit_waiter *credit_wait_head;
    struct scs_credit_waiter *credit_wait_tail;
    unsigned                  credit_wait_depth;
    int                       credit_wait_draining;

    /* p. 2-44: "local SCS immediately sends remote SCS a special credit message
     * containing the local Pending Receive Credit count." OVMX cannot build
     * that frame (its wire class is an open RE gap -- see
     * docs/cluster-protocol-spec.md sec 5), so the CDT carries the SYSAP/port
     * hook that WOULD send it. NULL = nothing is wired, which is the state
     * scsd.c is in today. */
    scs_credit_special_fn special_emit;
    void                 *special_emit_ctx;

    /*
     * vms-b1d: this connection's DATAGRAM buffer account. "The number of
     * datagram buffers requested by the SYSAP is stored in the CDT that
     * describes the connection." (p. 2-42) Owned entirely by
     * src/vmsscs/scs_dgram.c -- scs_cdt.c only zeroes these with the rest of
     * the CDT at open and never reads them. See scs_dgram.h for the page
     * cites, for why the account is on the RECEIVE path and not the send path,
     * and for the honest statement that no production caller routes a datagram
     * through it.
     *
     * NOTE that this is NOT credit. p. 2-42: "SCA does not provide a flow
     * control mechanism for the datagram service." The message account above
     * blocks a sender; this one silently discards a receipt. Do not conflate.
     */

    /* p. 2-42: "the CDT's count of datagram buffers available to this
     * connection" -- decremented on delivery, restored when the SYSAP returns
     * the buffer. Zero means every datagram arriving for this connection is
     * discarded, even with buffers free in the port DFREEQ. */
    unsigned dgram_buffers;

    /* The number requested at CONNECT/ACCEPT (p. 2-42), kept so the run log
     * can show how far the available count has fallen from its extension. */
    unsigned dgram_extended;

    /* INV-6 visibility (an OVMX addition -- see scs_dgram.h). `no_quota` is the
     * p. 2-42 discard made when dgram_buffers is 0 and the buffer goes back to
     * the DFREEQ; the port-wide empty-DFREEQ discard is counted on the PDT. */
    unsigned long dgram_delivered;
    unsigned long dgram_discards_no_quota;
};

/*
 * struct scs_cdl - the Connection Descriptor List: "the system space addresses
 * of all CDTs are kept in a table known as the Connection Descriptor List"
 * (p. 2-29), sized SCSCONNCNT + 200 (p. 2-30).
 *
 * `entry[i]` is the address of the CDT whose CONID has low 16 bits == i, or
 * NULL if no CDT has been placed in that slot. Slots 0..SCSCONNCNT-1 are
 * pre-populated at init with the initial CDT allocation, marked unused; slots
 * SCSCONNCNT..SCSCONNCNT+199 stay NULL until the initial allocation is
 * exhausted and an overflow CDT is handed out (p. 2-30).
 */
struct scs_cdl {
    struct scs_cdt *entry[SCS_CDL_ENTRIES];
    struct scs_cdt  initial[SCS_CDT_SCSCONNCNT];
    struct scs_cdt  overflow[SCS_CDT_CDL_OVERFLOW];
    unsigned        overflow_allocated; /* how many of the 200 have been handed out */
};

/* --- lifecycle ------------------------------------------------------------ */

/*
 * scs_cdl_init - "The CDL is allocated during system initialization ... VMS
 * also allocates and marks as unused SCSCONNCNT CDTs. VMS stores the addresses
 * of these CDTs in the first SCSCONNCNT entries of the CDL." (p. 2-30)
 * No-op if cdl is NULL.
 */
void scs_cdl_init(struct scs_cdl *cdl);

/*
 * scs_cdl_alloc - form a connection: take a free CDT, give it the CONID of its
 * CDL slot, record the local and remote SYSAP names, and queue it to `pb` (p.
 * 2-28). `pb` may be NULL only for a connection not yet bound to a circuit.
 * The CDT's SB pointer is taken from pb->sb.
 *
 * Reuses a released CDT from the initial allocation first (p. 2-30: "As
 * connections are broken, those CDTs are released (but not deallocated) so
 * that they can then be reused"); only when the initial allocation is
 * exhausted does it hand out one of the 200 overflow CDTs.
 *
 * SYSAP names are truncated to SCS_CDT_SYSAP_NAME_LEN; NULL means "unset".
 * Returns NULL if cdl is NULL or the CDL is full.
 */
struct scs_cdt *scs_cdl_alloc(struct scs_cdl *cdl, const char *local_sysap,
                              const char *remote_sysap, struct scs_pb *pb);

/*
 * scs_cdl_alloc_conid - the same, but claiming a SPECIFIC CONID rather than
 * the next free slot. Exists because OVMX has already shipped three fixed
 * Con.IDs on the lab wire (scsd.c's OVMX_LOCAL_CONID / OVMX_JOINER_CONID and
 * scs_dir.h's SCS_DIR_OVMX_CONID); a caller that must keep those exact bytes
 * can claim their slots instead of taking whatever the allocator hands out.
 *
 * Returns NULL if the CONID's tag is not SCS_CDT_CONID_TAG, if its slot is out
 * of range, or if the slot is already occupied by an in-use CDT.
 */
struct scs_cdt *scs_cdl_alloc_conid(struct scs_cdl *cdl, uint32_t conid,
                                    const char *local_sysap, const char *remote_sysap,
                                    struct scs_pb *pb);

/*
 * scs_cdl_release - the connection is broken. Dequeues the CDT from its Path
 * Block and marks it unused, but leaves it in its CDL slot for reuse (p. 2-30:
 * "released (but not deallocated)"). No-op on NULL or an already-free CDT.
 *
 * vms-61b: also RETURNS this connection's share of the port MFREEQ -- the
 * buffers p. 2-43 contributed at connection formation that are STILL SITTING IN
 * the queue, i.e. `receive_credit + pending_receive_credit`, are subtracted from
 * cdt->pb->pdt->mfreeq_count (saturating at 0). CORRECTED vms-096: this used to
 * subtract `extended_credits`, the whole formation deposit, which charges the
 * port a second time for every buffer already dequeued into the SYSAP's hands
 * by mfreeq_take() -- the exact error vms-b1d had already fixed on the DFREEQ
 * side below. Nothing else
 * about the credit account is touched; in particular the CDT's Credit Wait
 * queue is NOT drained here, because struct scs_credit_waiter is opaque to
 * scs_cdt.c. A caller tearing a connection down must call
 * scs_credit_wait_flush() first (scs_depart.c does).
 *
 * vms-b1d: and RETURNS this connection's share of the port DFREEQ, by the same
 * p. 2-43 argument -- `dgram_buffers`, the deposit STILL SITTING IN the queue,
 * is subtracted from cdt->pb->pdt->dfreeq_count (saturating at 0). Buffers the
 * SYSAP still holds (dgram_extended - dgram_buffers) were already dequeued by
 * scs_dgram_port_take and are deliberately NOT returned: they are gone with the
 * connection that held them. Without this the port's datagram account grows on
 * every connect/release cycle -- see the DEPOSIT RETURN note in scs_dgram.h.
 */
void scs_cdl_release(struct scs_cdl *cdl, struct scs_cdt *cdt);

/* --- CDT field setters ---------------------------------------------------- */

/*
 * scs_cdt_set_remote_conid - record the CONID the peer assigned to ITS CDT,
 * learned from a CONNECT_REQ or ACCEPT_REQ (p. 2-28). This is the value that
 * becomes the destination CONID of everything we then send (p. 2-35).
 */
void scs_cdt_set_remote_conid(struct scs_cdt *cdt, uint32_t remote_conid);

/*
 * scs_cdt_set_handlers - the arguments to the CONNECT and ACCEPT services
 * (pp. 2-28..2-29). Any pointer may be NULL. `ctx` is an OVMX addition.
 */
void scs_cdt_set_handlers(struct scs_cdt *cdt, scs_msg_input_fn msg_input,
                          scs_dgram_input_fn dgram_input, scs_vc_loss_fn vc_loss,
                          void *ctx);

/*
 * scs_cdt_set_connect_data - carry the SYSAP-supplied connect data (p. 2-28).
 * Returns 0, or -1 if cdt is NULL, data is NULL with len != 0, or len exceeds
 * SCS_CDT_CONNECT_DATA_MAX. This module never interprets the bytes.
 */
int scs_cdt_set_connect_data(struct scs_cdt *cdt, const void *data, size_t len);

/* Bind (or re-bind) a CDT to a Path Block, moving it between PB queues and
 * re-syncing the SB pointer from pb->sb. pb may be NULL to unbind. */
void scs_cdt_set_pb(struct scs_cdt *cdt, struct scs_pb *pb);

/* --- lookup and delivery -------------------------------------------------- */

/*
 * scs_cdl_lookup - CONID -> CDT. "The low order 16 bits of the CONID
 * associated with a CDT are used as an index into the CDL to locate the CDT's
 * address." (p. 2-29)
 *
 * Returns NULL if cdl is NULL, the slot is empty, the CDT in it is not in use,
 * or the CDT's own local_conid does not equal `conid` -- the last check is what
 * makes the high 16 bits load-bearing: a stale CONID whose slot has since been
 * reused for a different connection does NOT resolve to the new one.
 */
struct scs_cdt *scs_cdl_lookup(const struct scs_cdl *cdl, uint32_t conid);

/* Delivery results. */
enum scs_deliver_result {
    SCS_DELIVER_OK = 0,
    SCS_DELIVER_NO_CDT = -1,      /* destination CONID resolves to no open connection */
    SCS_DELIVER_SRC_MISMATCH = -2,/* source CONID is not this connection's remote CONID */
    SCS_DELIVER_NO_ROUTINE = -3   /* the CDT has no input routine for this packet kind */
};

/*
 * scs_cdl_resolve - THE p. 2-29 destination-CONID resolution, factored out so
 * that every receive entry point resolves identically instead of each
 * reimplementing it: the CDL index lookup plus the p. 2-35 source-CONID
 * refusal. On SCS_DELIVER_OK `*out_cdt` is the connection; on any other result
 * it is NULL. `out_cdt` may be NULL if the caller only wants the verdict.
 *
 * All three PACKET-DELIVERY entry points -- scs_cdl_deliver_message(),
 * scs_cdl_deliver_datagram() and the accounted scs_dgram_cdl_deliver()
 * (scs_dgram.h) -- resolve through THIS function and differ only in what they
 * do with the CDT afterwards. Add a fourth and it resolves here too: two
 * delivery paths that disagree about which connection a packet belongs to
 * would be a real defect, and the point of this function is that they cannot.
 *
 * NOT a claim that this is the only scs_cdl_lookup() call in the tree. scsd.c
 * calls it directly to drive the connection state machine off a received
 * message type; that is an FSM step on an already-classified frame, not packet
 * delivery to a SYSAP, and it applies no source-CONID refusal.
 */
int scs_cdl_resolve(struct scs_cdl *cdl, uint32_t dest_conid, uint32_t src_conid,
                    struct scs_cdt **out_cdt);

/*
 * scs_cdl_deliver_message / scs_cdl_deliver_datagram - the p. 2-29 receive
 * path: "the remote port driver uses the low order 16 bits of the destination
 * CONID as an index into the remote node's CDL ... There it finds the address
 * of the CDT ... The port driver then offsets to a fixed location within the
 * CDT to locate the address of the remote SYSAP's message input routine, and
 * passes the message to that routine. (In the case of a datagram, the port
 * driver would simply fetch the address of the datagram input routine from a
 * different but fixed location in the CDT.)"
 *
 * `src_conid` is the packet's source CONID (p. 2-35: every message and datagram
 * carries both). When it is nonzero and the CDT's remote CONID is already
 * known, a mismatch is REFUSED rather than delivered -- that is the whole point
 * of carrying the source CONID. Pass 0 when the frame class genuinely does not
 * carry one (see the WIRE VERDICT: the 66-byte class carries source CONID 0 on
 * the real VAX wire).
 *
 * Returns an enum scs_deliver_result.
 */
int scs_cdl_deliver_message(struct scs_cdl *cdl, uint32_t dest_conid, uint32_t src_conid,
                            const void *buf, size_t len);
int scs_cdl_deliver_datagram(struct scs_cdl *cdl, uint32_t dest_conid, uint32_t src_conid,
                             const void *buf, size_t len);

/* --- the Path Block connection queue (p. 2-28) ---------------------------- */

/* First CDT queued to this circuit's Path Block; NULL if none. */
struct scs_cdt *scs_pb_first_cdt(const struct scs_pb *pb);

/* Next CDT on the same Path Block; NULL at the end of the queue. */
struct scs_cdt *scs_cdt_next_on_pb(const struct scs_cdt *cdt);

/* How many connections this virtual circuit supports; 0 for NULL. */
unsigned scs_pb_cdt_count(const struct scs_pb *pb);

/*
 * scs_cdl_vc_loss - "If the circuit is broken for any reason, it is then a
 * relatively simple matter to scan this queue to determine which connections
 * have also been lost, and to notify the interested SYSAPs. In fact, the VMS
 * implementation of SCA stores the address of the interested SYSAP's error
 * handler for virtual circuit loss in the CDT itself." (p. 2-28)
 *
 * Scans pb's CDT queue and calls each CDT's vc_loss_handler. Returns the number
 * of CDTs found on the circuit (whether or not they had a handler).
 *
 * It does NOT release the CDTs and does NOT touch conn_state: what a connection
 * becomes after VC loss is the connection state machine's decision (vms-dd5).
 * It must be called BEFORE scs_pb_close(), which returns the Path Block to its
 * pool and would leave every CDT on it holding a dangling pb pointer. Since
 * vms-17f that is enforced, not merely required: scs_pb_close() REFUSES a PB
 * with a non-empty CDT queue (SCS_PB_CLOSE_CONNECTIONS_QUEUED). scs_depart.c's
 * scs_pb_depart() is the one routine that performs the full sequence.
 */
unsigned scs_cdl_vc_loss(struct scs_pb *pb);

/* Number of in-use CDTs in the whole CDL. */
unsigned scs_cdl_in_use_count(const struct scs_cdl *cdl);

/* vms-7fe: mark this CDT as a p. 2-48 LISTENING CDT (see the field). Called by
 * scs_sdir_listen() immediately after allocation, before anything can report on
 * it. cdt_open() clears the flag with the rest of the descriptor, so a released
 * listening CDT reused for a connection is not still marked. */
void scs_cdt_set_listening(struct scs_cdt *cdt, int listening);

#ifdef __cplusplus
}
#endif

#endif /* SCS_CDT_H */
