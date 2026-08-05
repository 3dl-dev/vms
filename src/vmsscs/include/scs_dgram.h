/*
 * scs_dgram.h - SCA datagram buffer management: the per-port Datagram Free
 * Queue (DFREEQ) and the per-connection count of datagram buffers held in the
 * CDT (vms-b1d).
 *
 * WHY THIS EXISTS. OVMX's datagram path was entirely unaccounted: a datagram
 * either reached a SYSAP input routine or it did not, and nothing anywhere
 * recorded that one had been dropped. SCA does define a buffer-management
 * mechanism for datagrams -- not flow control, a bank account -- and its whole
 * observable behaviour is a DISCARD. A discard that is invisible locally is
 * indistinguishable from a facility that does nothing (CLAUDE.md rule 9 /
 * INV-6), so this module both implements the mechanism and counts every drop.
 *
 * SOURCE (public, quoted with page cites -- CLAUDE.md rule 8):
 *   Roy G. Davis, *VAXcluster Principles*, Digital Press 1993, ch. 2 sec 2.8.
 *   - "Since the delivery of datagrams is on a 'best effort' nonguaranteed
 *     basis, SCA does not provide a flow control mechanism for the datagram
 *     service. However, SCA does define a simple datagram buffer management
 *     mechanism."                                                    p. 2-42
 *   - "A SYSAP can optionally request SCS to allocate a number of buffers to
 *     handle incoming datagrams received on a particular connection. This is
 *     done using an optional argument in the CONNECT and ACCEPT services. The
 *     number of datagram buffers requested by the SYSAP is stored in the CDT
 *     that describes the connection. The buffers themselves are inserted into
 *     the *Datagram Free Queue* (DFREEQ) associated with the port that supports
 *     the connection."                                               p. 2-42
 *   - "When the port receives a datagram, it dequeues the buffer at the head of
 *     DFREEQ, stores the datagram in that buffer, and then passes the buffer to
 *     its port driver. SCS then locates the CDT associated with the destination
 *     CONID in the datagram."                                        p. 2-42
 *   - "If the CDT's count of datagram buffers available to this connection is
 *     greater than 0, SCS delivers the datagram to the destination SYSAP. This
 *     is done by decrementing the count of datagram buffers available to the
 *     connection, and then passing the buffer to the datagram input routine
 *     whose address is in the CDT."                                  p. 2-42
 *   - "If the CDT's count of datagram buffers available to this connection is
 *     not greater than 0, SCS discards the datagram by inserting the buffer
 *     back into the DFREEQ. This prevents datagrams received on one connection
 *     from consuming datagram buffers contributed to the DFREEQ for other
 *     connections."                                                  p. 2-42
 *   - "The possibility exists that the DFREEQ itself is empty when the port
 *     receives a datagram. If this is the case, the port merely discards the
 *     datagram."                                                     p. 2-42
 *   - "When a SYSAP has finished processing a received datagram, it can request
 *     that SCS return the buffer to the DFREEQ and increment the CDT's count of
 *     available datagram buffers; or it can simply request that SCS deallocate
 *     the buffer."                                                   p. 2-42
 *   - "It is the responsibility of the SYSAP to monitor the count of available
 *     datagram buffers for a given connection. If necessary, a SYSAP can
 *     request SCS to allocate additional buffers to the DFREEQ. A SYSAP can
 *     also request SCS to deallocate buffers for a connection from the
 *     DFREEQ."                                                       p. 2-43
 *   - The bank analogy, and "SCA associates a separate DFREEQ with each port.
 *     ... The datagram buffers allocated for a connection are inserted into the
 *     DFREEQ for the port that supports that connection."            p. 2-43
 *   - "SCA permits the association between a port and its MFREEQ and DFREEQ to
 *     be implementation dependent. VMS keeps the heads of these queues for a
 *     given port in the *Port Queue Block* (PQB) portion of the PDT it uses to
 *     describe the port."                                            p. 2-45
 *
 * ==================== DIRECTION: THE ACCOUNT IS ON RECEIVE =================
 *
 * vms-b1d was dispatched as "a connection's datagram SENDS draw from the
 * DFREEQ". THE BOOK REFUTES THAT, and this module implements what the book
 * says instead. p. 2-42 defines DFREEQ buffers as buffers "to handle INCOMING
 * datagrams received on a particular connection", debited when "the port
 * RECEIVES a datagram", and it opens by stating flatly that "SCA does not
 * provide a flow control mechanism for the datagram service" -- the sentence
 * that would have to be false for a send-side quota to exist. The send side is
 * described separately on p. 2-35: a SYSAP sending a datagram "simply allocates
 * a buffer and stores the information to be sent in the buffer" and hands it to
 * a port driver send-datagram routine; no queue is consulted and no count moves.
 *
 * So there is NO SEND-SIDE DFREEQ DEBIT here, and one was not invented. The
 * documented silent discard is real, and it is on the receive path. This is a
 * deliberate, flagged deviation from the item text, not an omission.
 *
 * ============================== WIRE VERDICT ==============================
 *
 * NOT WIRE-VISIBLE. This module is pure local accounting: it builds no frame,
 * parses no frame, changes no transmitted byte, and has no kill-switch
 * obligation on that count. The p. 2-42 "optional argument in the CONNECT and
 * ACCEPT services" by which a SYSAP requests datagram buffers is an ARGUMENT TO
 * A LOCAL SERVICE; no captured field is pinned to it (recorded as a gap in
 * docs/cluster-protocol-spec.md sec 5), so scs_dgram_extend() takes it as an
 * API argument with no wire parser behind it -- exactly as scs_credit.c takes
 * Minimum Send Credits. An OVMX_NO_DGRAM_ACCOUNTING switch is provided anyway,
 * for parity with vms-76e and so a run can be compared with the mechanism off;
 * it gates LOCAL BEHAVIOUR only and no wire byte changes either way.
 *
 * OVMX DESIGN CHOICES (labeled per rule 8):
 *   - The DFREEQ is modeled as a COUNT of free datagram buffers on the PDT
 *     (scs_config.h `dfreeq_count`), not a real buffer queue -- identical
 *     treatment to vms-76e's MFREEQ, and permitted by p. 2-45's
 *     "implementation dependent" clause. OVMX has no port buffer pool to
 *     queue, so what is modeled is the DEPTH and the arithmetic on it. A
 *     consequence: OVMX cannot reproduce WHICH buffer a datagram landed in,
 *     only whether one was available.
 *   - SCS_DGRAM_DEFAULT_BUFFERS's VALUE is an OVMX choice. The book documents
 *     that a SYSAP requests a number and that VMS stores it in the CDT; it
 *     publishes NO default, and no SYSGEN parameter for it appears in the
 *     three the book lists for the message and datagram services (SCSMAXMSG,
 *     SCSMAXDG, SCSRSPCNT -- p. 2-35, all sizes/counts of other things).
 *     Recorded as an inferred/chosen value in docs/cluster-protocol-spec.md
 *     sec 5. Callers should pass an explicit count; the constant exists so
 *     that a caller that does not have one is not silently passing 0.
 *   - Discard counters. The book describes the discards; it does not say VMS
 *     counts them (it does expose datagram ACTIVITY through the SHOW CLUSTER
 *     COUNTERS class, p. 2-45, which is a different quantity). The two
 *     counters here are an OVMX addition demanded by INV-6, kept in the CDT
 *     and PDT beside the counts they shadow.
 *   - The two discard classes are kept SEPARATE (`dgram_discards_no_quota` on
 *     the CDT, `dfreeq_empty_discards` on the PDT) because the book gives them
 *     different actors and different effects: the no-quota discard is made by
 *     SCS and RETURNS the buffer to the DFREEQ, the empty-DFREEQ discard is
 *     made by the port and involves no buffer at all. Summing them would lose
 *     the distinction that tells a per-connection starvation apart from a
 *     port-wide one.
 *
 * ===== DEPOSIT RETURN: WHERE THE ACCOUNT IS CLOSED =====
 *
 * The DFREEQ deposit is returned to the port by scs_cdl_release() (scs_cdt.c),
 * NOT by anything in this file, because releasing a connection is a CDT-layer
 * event and the CDT layer is where the sibling MFREEQ return already lives
 * (vms-61b). p. 2-43's bank analogy is the whole argument: "each person is
 * entitled only to the amount of money that he or she has on deposit", and a
 * depositor who no longer exists has no deposit. The quantity returned is
 * `dgram_buffers` -- the share still sitting IN the queue. Buffers already
 * dequeued by scs_dgram_port_take() and handed to the SYSAP (the difference
 * dgram_extended - dgram_buffers) are NOT returned; they left the queue when
 * they were taken, and returning them here would credit the port twice.
 *
 * THIS IS NOT DECORATIVE. vms-17f made connection release production-reachable:
 * scs_pb_depart() releases every CDT on a departing peer's circuit, so without
 * the return a node that leaves and rejoins inflates the port's datagram
 * account on each cycle and never deflates it. It is the same defect class as
 * vms-61b, in the same function.
 *
 * ===== REACHABILITY IN SCSD TODAY -- DO NOT READ A GREEN TEST RUN AS "OVMX
 * ACCOUNTS ITS DATAGRAMS" =====
 *
 * scs_dgram_cdl_deliver() is the accounted receive path and it has NO
 * PRODUCTION CALLER.
 *
 * It does NOT call scs_cdl_deliver_datagram() -- an earlier revision of this
 * comment said it "wraps" it and that was false. It shares that function's
 * p. 2-29 RESOLUTION (both go through scs_cdl_resolve(), scs_cdt.h) and then
 * diverges, because the p. 2-42 accounting has to sit BETWEEN the resolution
 * and the SYSAP callback -- take a DFREEQ buffer, check the connection's quota,
 * debit it -- and scs_cdl_deliver_datagram() does resolution and callback in
 * one step with no seam to interpose. The two are siblings, not a wrapper and
 * a wrappee.
 *
 * The no-production-caller fact is independent of that and holds for both:
 * vms-e1a's header records scs_cdl_deliver_datagram() as unreachable from
 * scsd.c, which dispatches received frames by comparing Con.IDs against three
 * macros and never routes a datagram through the CDL. No SYSAP is installed, so
 * no dgram_input routine exists to deliver to.
 *
 * What IS wired: SCSD.EXE links this module and its exit summary calls
 * scs_dgram_report(), so every DFREEQ figure and every discard count is in the
 * run log. Today those lines print zeros, and that is the honest state --
 * they are the visibility this item owes, not evidence of traffic.
 */
#ifndef SCS_DGRAM_H
#define SCS_DGRAM_H

#include <stdio.h>

#include "scs_cdt.h" /* struct scs_cdt / struct scs_cdl carry the per-connection count */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SCS_DGRAM_DEFAULT_BUFFERS - the number of datagram buffers a connection gets
 * when its SYSAP does not name one. OVMX VALUE, not a VMS quantity (see the
 * header note); the ROLE -- "the number of datagram buffers requested by the
 * SYSAP is stored in the CDT" -- is the book's (p. 2-42).
 */
#ifndef SCS_DGRAM_DEFAULT_BUFFERS
#define SCS_DGRAM_DEFAULT_BUFFERS 8
#endif

/* Outcome of the p. 2-42 receive path. */
enum scs_dgram_result {
    SCS_DGRAM_DELIVERED = 0,
    /* "If the CDT's count of datagram buffers available to this connection is
     * not greater than 0, SCS discards the datagram by inserting the buffer
     * back into the DFREEQ." (p. 2-42) */
    SCS_DGRAM_DISCARD_NO_QUOTA = -1,
    /* "The possibility exists that the DFREEQ itself is empty when the port
     * receives a datagram. If this is the case, the port merely discards the
     * datagram." (p. 2-42) */
    SCS_DGRAM_DISCARD_DFREEQ_EMPTY = -2,
    /* Not a book state: the CDT is NULL / not bound to a port, or the CONID
     * resolved to nothing. Reported, never silently swallowed. */
    SCS_DGRAM_NO_CDT = -3,
    /* The CDT has no datagram input routine (p. 2-29). The buffer is returned
     * to the DFREEQ exactly as in the no-quota case -- nothing consumed it. */
    SCS_DGRAM_NO_ROUTINE = -4
};

/* --- kill switch (local behaviour only; see the WIRE VERDICT) ------------- */

/* 1 = accounting active; 0 = disabled by OVMX_NO_DGRAM_ACCOUNTING=1. */
int scs_dgram_enabled(void);

/* Re-read the environment on the next query (tests). */
void scs_dgram_reset_switch_cache(void);

/* --- buffer allocation (p. 2-42 / p. 2-43) -------------------------------- */

/*
 * scs_dgram_extend - the p. 2-42 optional CONNECT/ACCEPT argument: the SYSAP
 * requests `n` buffers for incoming datagrams on this connection. The count is
 * stored in the CDT and the buffers are inserted into the DFREEQ of the port
 * that supports the connection (found through cdt->pb->pdt).
 *
 * SETS the connection's count (it is connection formation, not a top-up) and
 * adds the delta to the port DFREEQ. Returns 0, or -1 if cdt is NULL. With the
 * kill switch set, does nothing and returns 0.
 */
int scs_dgram_extend(struct scs_cdt *cdt, unsigned n);

/*
 * scs_dgram_add_buffers / scs_dgram_remove_buffers - p. 2-43: "If necessary, a
 * SYSAP can request SCS to allocate additional buffers to the DFREEQ. A SYSAP
 * can also request SCS to deallocate buffers for a connection from the DFREEQ.
 * In both cases, SCS will appropriately alter the CDT to reflect the change."
 *
 * Removal is clamped at the connection's own count -- a SYSAP cannot deallocate
 * buffers it did not contribute (the bank analogy, p. 2-43: "each person is
 * entitled only to the amount of money that he or she has on deposit").
 * Returns the number actually added/removed, or 0 with the switch set.
 */
unsigned scs_dgram_add_buffers(struct scs_cdt *cdt, unsigned n);
unsigned scs_dgram_remove_buffers(struct scs_cdt *cdt, unsigned n);

/* --- the receive path (p. 2-42) ------------------------------------------- */

/*
 * scs_dgram_port_take - the port's half: "it dequeues the buffer at the head of
 * DFREEQ". Returns 0 if a buffer was taken, or SCS_DGRAM_DISCARD_DFREEQ_EMPTY
 * if the DFREEQ was empty -- in which case the port has already discarded the
 * datagram and pdt->dfreeq_empty_discards has been incremented.
 *
 * Exposed separately because the book's order is port-first: the empty-DFREEQ
 * discard happens BEFORE SCS has looked up any CDT, so it is not attributable
 * to a connection.
 *
 * Returns SCS_DGRAM_NO_CDT for a NULL pdt (there is no port to account
 * against; nothing is counted anywhere rather than counted in the wrong place).
 */
int scs_dgram_port_take(struct scs_pdt *pdt);

/*
 * scs_dgram_deliver - SCS's half, given the CDT the destination CONID resolved
 * to and a buffer already dequeued by scs_dgram_port_take(). Decrements the
 * connection's count and calls the CDT's datagram input routine, or returns
 * the buffer to the DFREEQ and counts a discard.
 *
 * Returns an enum scs_dgram_result.
 */
int scs_dgram_deliver(struct scs_cdt *cdt, const void *buf, size_t len);

/*
 * scs_dgram_cdl_deliver - THE ACCOUNTED RECEIVE PATH, p. 2-42 end to end:
 * resolve the destination CONID through the CDL (p. 2-29, via
 * scs_cdl_resolve()), dequeue from the DFREEQ of the port that supports the
 * connection, then deliver or discard.
 *
 * This is the accounted SIBLING of scs_cdl_deliver_datagram() -- same
 * resolution function, different second half. It does not call that function
 * (see the reachability note above for why it cannot). That one remains
 * available and remains UNACCOUNTED; callers that route datagrams should use
 * this one. Neither has a production caller today.
 *
 * `src_conid` is passed through to the vms-e1a source-CONID check; pass 0 when
 * the frame class does not carry one. Returns an enum scs_dgram_result, or
 * SCS_DGRAM_NO_CDT when the CONID resolves to no open connection or the source
 * CONID does not match the connection's remote CONID.
 *
 * ORDERING CAVEAT, stated rather than papered over. p. 2-42 has the PORT
 * dequeue a buffer BEFORE SCS looks up the CDL, so on VMS a datagram addressed
 * to no open connection still consumes and then releases a DFREEQ buffer.
 * OVMX cannot reproduce that ordering here: it models the DFREEQ as a depth on
 * the PDT and reaches the PDT only THROUGH the resolved CDT (cdt->pb->pdt),
 * because nothing passes a receiving port into this path. So an unresolvable
 * destination CONID returns SCS_DGRAM_NO_CDT and moves NO count -- a real
 * difference from VMS, not an equivalent. Call scs_dgram_port_take() directly
 * from a caller that does know its receiving port to get the book's order.
 */
int scs_dgram_cdl_deliver(struct scs_cdl *cdl, uint32_t dest_conid, uint32_t src_conid,
                          const void *buf, size_t len);

/*
 * scs_dgram_release_buffer - p. 2-42: the SYSAP "can request that SCS return
 * the buffer to the DFREEQ and increment the CDT's count of available datagram
 * buffers". Returns 0, or -1 if cdt is NULL.
 */
int scs_dgram_release_buffer(struct scs_cdt *cdt);

/*
 * scs_dgram_deallocate_buffer - the other p. 2-42 option: the SYSAP "can simply
 * request that SCS deallocate the buffer". The buffer does NOT come back, so
 * neither the DFREEQ nor the connection's count rises; the connection has
 * permanently given up one buffer. Returns 0, or -1 if cdt is NULL.
 */
int scs_dgram_deallocate_buffer(struct scs_cdt *cdt);

/* --- visibility (INV-6) --------------------------------------------------- */

/* Per-connection counters. 0 for a NULL CDT. */
unsigned long scs_dgram_delivered(const struct scs_cdt *cdt);
unsigned long scs_dgram_discards_no_quota(const struct scs_cdt *cdt);

/* Per-port counter: datagrams the port dropped because the DFREEQ was empty. */
unsigned long scs_dgram_dfreeq_empty_discards(const struct scs_pdt *pdt);

/*
 * scs_dgram_report - one SCSD-I-DFREEQ block: the port DFREEQ depth and empty
 * discards for every distinct port reachable from an in-use CDT, then a line
 * per in-use connection with its CONID, its available-buffer count, and its
 * delivered / discarded counts. Returns the number of connection lines
 * printed. No-op returning 0 if cdl or out is NULL.
 */
unsigned scs_dgram_report(const struct scs_cdl *cdl, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* SCS_DGRAM_H */
