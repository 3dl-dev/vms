/*
 * scs_credit.h - SCA message flow control: the Send / Receive / Pending
 * Receive Credit debit-credit system carried per connection in the CDT
 * (vms-76e).
 *
 * WHY THIS EXISTS. OVMX already had something it called "credit": scs_vc.c's
 * strict 1-for-1 `0x48` credit-return emitter (vms-691). That is an ad-hoc
 * ACK generator -- it counts nothing, it holds no per-connection state, and
 * it cannot answer "may this connection send a message right now?". SCA's
 * actual flow control is a debit/credit account per CONNECTION, held in the
 * CDT, with three counts on each side and a piggybacked grant in every
 * message header. This module is that account.
 *
 * SOURCE (public, quoted with page cites -- CLAUDE.md rule 8):
 *   Roy G. Davis, *VAXcluster Principles*, Digital Press 1993, ch. 2, sec 2.8.
 *   - "SCA does provide flow control for the message service based on a
 *     debit/credit system. When a connection is formed ... the local SYSAP
 *     requests SCS to allocate a certain number of buffers to receive incoming
 *     messages from the remote SYSAP. These buffers are inserted into the
 *     Message Free Queue (MFREEQ) associated with the port that supports the
 *     connection. Suppose that number is 10; then the local SYSAP is said to
 *     have extended 10 Send Credits to the remote SYSAP."            p. 2-43
 *   - "Each time the remote SYSAP wishes to send a message ... remote SCS
 *     verifies that it has at least 1 Send Credit associated with the
 *     connection. If it does, remote SCS decrements its Send Credit count and
 *     sends the message. If it doesn't, remote SCS blocks the transmission by
 *     queuing the CDRP ... until the Send Credit count is again greater
 *     than 0."                                                       p. 2-43
 *   - "The difference between these two numbers, 3, is referred to as the
 *     Pending Receive Credit count associated by local SCS with the
 *     connection."                                                   p. 2-43
 *   - "In each message packet is certain header information that includes a
 *     'credit' field. When the local SYSAP sends a message to the remote
 *     SYSAP, local SCS copies the local Pending Receive Credit count into this
 *     credit field, and then resets to 0 the local Pending Receive Credit
 *     count. When the message is received by the remote node, remote SCS adds
 *     the content of the credit field to the Send Credit count it associates
 *     with the connection, and then delivers the message to the remote
 *     SYSAP."                                                 pp. 2-43..2-44
 *   - The worked example and its simultaneous-send variation (10 extended, 3
 *     sent, Send Credit 7, Pending Receive 3, reverse message restores 10;
 *     variation ends at 9).                                          p. 2-44
 *   - "Local SCS maintains a Receive Credit count for the connection that is
 *     effectively a mirror image of remote SCS's Send Credit count."  p. 2-44
 *   - Special credit message; "SCA defines the local Receive Credit count for
 *     a connection to be dangerously low if it is less than the Minimum Send
 *     Credits argument passed to the CONNECT or ACCEPT service by the remote
 *     SYSAP ... The VMS implementation ... considers [it] dangerously low if
 *     it is less than the sum of the local SYSGEN parameter SCSFLOWCUSH and
 *     the remote value for Minimum Send Credits."                     p. 2-44
 *   - "SCS on each node maintains Send Credit, Receive Credit, and Pending
 *     Receive Credit counts for the connection"; "SCA associates a Message
 *     Free Queue with each port"; "a given connection's share of buffers in a
 *     MFREEQ is limited by the data in the CDT for that connection"; "SCS
 *     maintains this information for each connection in the CDT"; VMS keeps
 *     the MFREEQ/DFREEQ heads in the Port Queue Block portion of the PDT.
 *                                                                    p. 2-45
 *   - Credit Wait: "Before actually allocating the buffer, this routine first
 *     verifies that at least one Send Credit is available on the connection
 *     being used. If no Send Credits are available, then this routine
 *     temporarily suspends the operation ... Whenever the Send Credit count in
 *     a CDT is increased, the CDT's queue of waiting CDRPs is examined."
 *                                                                    p. 2-45
 *
 * ================= WIRE VERDICT: THE CREDIT FIELD IS GROUNDED =============
 *
 * The dispatch flagged this item wire-visible and required the credit field's
 * offset to be grounded from OUR OWN captures or else labelled an OVMX design
 * choice. docs/cluster-protocol-spec.md sec 4(g) had previously reported the
 * credit value as a grounded NUMBER but an ungrounded OFFSET ("its offset
 * shifts between message classes, so it is not pinned to a single fixed
 * field"). That negative is now SUPERSEDED. Re-measured here over our own lab
 * captures -- /data/training/vax/cluster/captures/formation-ci1.pcap (18558
 * frames) and formation-ci1-joinwindow.pcap (3000 frames) -- the credit field
 * IS at a single fixed offset for the SCS message classes:
 *
 *   SCS credit field = SCA offset [48:50], LE uint16
 *                    = absolute frame offset [62:64] with the 14-byte Ethernet
 *                      header, i.e. the 2 bytes IMMEDIATELY PRECEDING the
 *                      remote/destination Con.ID at SCA [50:54].
 *
 * Three independent lines of evidence, all from those two captures:
 *
 *  1. CONSERVATION over the 190-byte steady-state class. Summing [48:50] over
 *     every 190-byte frame a node SENDS, against the number of 190-byte
 *     messages it RECEIVED:
 *        formation-ci1:  VAX1 granted 10842 vs peer sent 10842  (delta 0)
 *                        peer granted  6712 vs VAX1 sent  6715  (delta 3)
 *        joinwindow:     VAX1 granted  1601 vs peer sent  1602  (delta 1)
 *                        peer granted  1300 vs VAX1 sent  1300  (delta 0)
 *     Total credits returned equals total messages sent to within the
 *     end-of-capture tail. That is exactly the p. 2-43 debit/credit identity
 *     and no other field in the header satisfies it.
 *  2. VALUE SHAPE. Over 19860 190-byte frames the field takes only
 *     {0:5174, 1:10696, 2:2582, 3:1405, 4:3} -- the shape of a piggybacked
 *     Pending Receive Credit in a near-lockstep flow, not a counter.
 *  3. TUNABLE MATCH at connection formation. In the 110-byte
 *     CONNECT_REQ/ACCEPT_REQ class the same field carries the number of Send
 *     Credits that SYSAP is extending, byte-exact to TWO DIFFERENT SYSGEN
 *     parameters in the same capture:
 *        VMS$VAXcluster  <-> VMS$VAXcluster   : 10 = CLUSTER_CREDITS
 *        MSCP$DISK       -> VMS$DISK_CL_DRVR  : accept carries 8 = MSCP_CREDITS
 *        SCS$DIRECTORY 3 / SCS$DIR_LOOKUP 1 / SCA$TRANSPORT 6
 *     A single offset matching two distinct tunables, on two distinct SYSAP
 *     pairs, is not coincidence.
 *
 * SCOPE OF THE GROUNDING. Offset 48 is asserted only for the SCS *message*
 * length classes listed in scs_credit_header_offset(). The block-data-transfer
 * classes (70/82/206/270/398/462/526/... byte SCA) carry large values there --
 * a different layout -- and are deliberately REFUSED rather than guessed at.
 * That is the residue of truth in the old sec 4(g) note.
 *
 * ================== REACHABILITY: WHAT IS LIVE AND WHAT IS NOT ============
 *
 * READ THIS BEFORE CREDITING A GREEN TEST RUN AS "OVMX DOES FLOW CONTROL".
 *
 *   - NOTHING IN scsd.c CALLS THIS MODULE. src/vmsscs/scsd.c does not link
 *     vmsscs_credit and does not route through CDTs at all (see the same
 *     admission at the end of scs_cdt.h): its connections are still node-global
 *     Con.ID macros over `struct peer_state`. The daemon is BYTE-UNAFFECTED by
 *     this file. `nm SCSD.EXE` shows no scs_credit_* symbol.
 *   - CONSEQUENTLY OVMX STILL PUTS NO LIVE CREDIT ON THE WIRE. Every frame OVMX
 *     emits carries whatever value its captured template had at [48:50]; this
 *     module changes not one transmitted byte. scs_credit_stamp_header() and
 *     scs_credit_read_header() exist, are unit-tested against real captured
 *     frames, and have ZERO production callers today.
 *   - The 0x48 "credit-return" emitter in scs_vc.c is untouched and is NOT this
 *     mechanism. It is a 1-for-1 ACK; the 41-byte 0x48 class is too short to
 *     even contain SCA offset 48. Whether the 0x48 short is SCA's "special
 *     credit message" (p. 2-44) is NOT established and is not claimed here.
 *     NAME COLLISION WARNING: `nm SCSD.EXE` DOES show one `scs_credit_*`
 *     symbol -- `scs_credit_build`, scs_vc.c's 0x48 frame builder, which
 *     predates this module (vms-691). No symbol declared in THIS header
 *     appears in the daemon.
 *   - Wiring this to the daemon needs the connection state machine (vms-dd5)
 *     and per-peer connection identity, which this item is fenced out of.
 *
 * PER THE DISPATCH: this is implemented for CORRECTNESS. It is NOT a fix for
 * the vms-2f3 rejoin failure and is not offered as one -- HANDOFF sec 4M.22
 * killed credit as a rejoin cause with a matched control (the peer's own
 * bookkeeping reads "Send Credit Q. empty" on the stuck CDT).
 *
 * KILL SWITCH. Setting OVMX_NO_CREDIT_ACCOUNTING=1 in the environment disables
 * the whole account: extends grant nothing, sends never debit and never
 * piggyback (they report 0 credit and leave Pending Receive Credit alone),
 * receives never add, and scs_credit_stamp_header() leaves the frame byte
 * unmodified. See tests/vmsscs/test_scs_credit.c, which runs the p. 2-44
 * worked example a second time with the switch set and asserts every count
 * stays at its initial value.
 *
 * OVMX DESIGN CHOICES (labeled per rule 8):
 *   - The counts are plain `unsigned` in the CDT, not a VMS-layout structure.
 *     The book documents the CONTENTS of the credit account, never a byte
 *     layout for it; nothing here but the [48:50] field reaches the wire.
 *   - Credit Wait is reported, not queued. p. 2-45 blocks by queuing the CDRP
 *     to the CDT; OVMX has no CDRP, so scs_credit_on_send() REFUSES the send
 *     and the caller is expected to retry. Nothing here silently succeeds --
 *     a refused send is an honest -1 (CLAUDE.md rule 9 / INV-6).
 *   - SCS_CREDIT_SCSFLOWCUSH's default is an OVMX value, not a VMS quantity.
 *     On VMS it is a SYSGEN parameter; OVMX has no SYSGEN plumbing for it, so
 *     it is a compile-time knob. Its ROLE in the dangerously-low test is the
 *     book's (p. 2-44).
 *   - The MFREEQ is modeled as a COUNT of free message buffers on the PDT
 *     (scs_config.h `mfreeq_count`), not a real buffer queue. p. 2-45 states
 *     the port/MFREEQ association is "implementation dependent"; OVMX has no
 *     port buffer pool to queue, so it accounts the depth only.
 */
#ifndef SCS_CREDIT_H
#define SCS_CREDIT_H

#include <stddef.h>
#include <stdint.h>

#include "scs_cdt.h" /* struct scs_cdt carries the three counts */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SCS_CREDIT_SCSFLOWCUSH - stands in for the local SYSGEN parameter
 * SCSFLOWCUSH used in the p. 2-44 dangerously-low test. OVMX default value
 * (see the header note); the role is the book's.
 */
#ifndef SCS_CREDIT_SCSFLOWCUSH
#define SCS_CREDIT_SCSFLOWCUSH 2
#endif

/*
 * The credit field's position inside the SCA content (NOT the Ethernet frame).
 * See the WIRE VERDICT above for the grounding. Add 14 for an absolute offset
 * into a captured frame.
 */
#define SCS_CREDIT_FIELD_SCA_OFFSET 48
#define SCS_CREDIT_FIELD_LEN        2

/*
 * scs_credit_enabled - 0 when OVMX_NO_CREDIT_ACCOUNTING=1 is set in the
 * environment, 1 otherwise. The value is read once and cached.
 */
int scs_credit_enabled(void);

/*
 * scs_credit_reset_switch_cache - re-read OVMX_NO_CREDIT_ACCOUNTING. Exists so
 * a test can flip the kill switch inside one process; production never calls it.
 */
void scs_credit_reset_switch_cache(void);

/* --- connection formation (p. 2-43) --------------------------------------- */

/*
 * scs_credit_extend - the local SYSAP extends `n` Send Credits to the remote
 * SYSAP by contributing `n` message buffers to the MFREEQ of the port that
 * supports the connection (p. 2-43). Records `n` as this connection's share of
 * that MFREEQ (p. 2-45) and sets the local Receive Credit to `n`, the mirror of
 * the Send Credit count the remote will associate with the connection (p. 2-44).
 * Pending Receive Credit starts at 0.
 *
 * `remote_min_send_credits` is the Minimum Send Credits argument the REMOTE
 * SYSAP passes to CONNECT/ACCEPT; it is what the p. 2-44 dangerously-low test
 * compares the LOCAL Receive Credit against.
 *
 * The MFREEQ depth is added to cdt->pb->pdt->mfreeq_count when the CDT is bound
 * to a Path Block with a port; an unbound CDT still gets its counts (the
 * accounting is per connection, p. 2-45).
 *
 * Returns 0, or -1 if cdt is NULL. A no-op returning 0 when the kill switch is
 * set.
 */
int scs_credit_extend(struct scs_cdt *cdt, unsigned n, unsigned remote_min_send_credits);

/*
 * scs_credit_grant_from_peer - record that the REMOTE SYSAP extended `n` Send
 * Credits to us, learned from the credit field of its CONNECT_REQ or
 * ACCEPT_REQ (see the WIRE VERDICT: that is what the 110-byte class carries).
 * Sets the local Send Credit count to `n`.
 *
 * Returns 0, or -1 if cdt is NULL. A no-op returning 0 under the kill switch.
 */
int scs_credit_grant_from_peer(struct scs_cdt *cdt, unsigned n);

/* --- send path (pp. 2-43..2-45) ------------------------------------------- */

/*
 * scs_credit_can_send - the p. 2-45 Credit Wait test: 1 if at least one Send
 * Credit is available on the connection, 0 if the operation would have to be
 * suspended. Always 1 under the kill switch (accounting disabled). 0 for NULL.
 */
int scs_credit_can_send(const struct scs_cdt *cdt);

/*
 * scs_credit_on_send - send one message on this connection. Debits one Send
 * Credit (p. 2-43) and returns the value the caller MUST place in the outgoing
 * header's credit field: the local Pending Receive Credit count, which is reset
 * to 0 (p. 2-44). The local Receive Credit is raised by the same amount, since
 * it mirrors the Send Credit count the remote is about to compute (p. 2-44).
 *
 * Returns the credit value to piggyback (>= 0), or -1 if cdt is NULL or the
 * connection has no Send Credit and the send must go into a Credit Wait -- in
 * which case NOTHING is modified. Under the kill switch it always returns 0 and
 * modifies nothing.
 */
int scs_credit_on_send(struct scs_cdt *cdt);

/* --- receive path (pp. 2-43..2-44) ---------------------------------------- */

/*
 * scs_credit_on_recv - one message arrived on this connection carrying
 * `credit` in its header credit field. Adds `credit` to the Send Credit count
 * (p. 2-43/2-44) and debits one Receive Credit for the message buffer this
 * message consumed (p. 2-44).
 *
 * Returns 0, or -1 if cdt is NULL. A no-op returning 0 under the kill switch.
 */
int scs_credit_on_recv(struct scs_cdt *cdt, unsigned credit);

/*
 * scs_credit_release_buffer - the local SYSAP finished with a received message
 * and released its buffer back to SCS: the buffer returns to the port MFREEQ
 * and the Pending Receive Credit count rises by one (p. 2-43: "the difference
 * between these two numbers"). Returns 0, or -1 if cdt is NULL. No-op under the
 * kill switch.
 */
int scs_credit_release_buffer(struct scs_cdt *cdt);

/* --- the second mechanism: special credit messages (p. 2-44) -------------- */

/*
 * scs_credit_is_dangerously_low - the p. 2-44 test, in the VMS form: the LOCAL
 * Receive Credit count is dangerously low when it is less than the sum of the
 * local SCSFLOWCUSH and the REMOTE SYSAP's Minimum Send Credits. 0 for NULL or
 * under the kill switch.
 */
int scs_credit_is_dangerously_low(const struct scs_cdt *cdt);

/*
 * scs_credit_take_special - "If it is, and if the local Pending Receive Credit
 * count is greater than 0, local SCS immediately sends remote SCS a special
 * credit message containing the local Pending Receive Credit count. Of course,
 * local SCS also resets the local Pending Receive Credit count to 0 when it
 * does so." (p. 2-44)
 *
 * Returns the credit count to put in the special credit message and clears
 * Pending Receive Credit (raising Receive Credit by the same amount, the same
 * mirror rule as scs_credit_on_send), or -1 if no special credit message is
 * due -- Receive Credit is not dangerously low, Pending Receive Credit is 0,
 * cdt is NULL, or the kill switch is set. Nothing is modified when it
 * returns -1.
 *
 * NOTE: OVMX does not know which wire frame class an SCA special credit message
 * is. This function decides WHETHER one is owed and WHAT it would carry; it
 * builds nothing. See the reachability note.
 */
int scs_credit_take_special(struct scs_cdt *cdt);

/* --- the wire field (see the WIRE VERDICT) -------------------------------- */

/*
 * scs_credit_header_offset - offset of the credit field within the SCA content
 * of a message whose total SCA length is `total_sca_len`, or -1 if that length
 * class is not one this module has grounded a credit field for.
 *
 * Grounded classes: the SCS message classes 58, 62, 66, 86, 94, 106, 110 and
 * 190 SCA bytes. Everything else -- notably the block-data-transfer classes,
 * which carry unrelated large values at offset 48, and the 41-byte 0x48 short,
 * which is too short to reach offset 48 at all -- returns -1 rather than a
 * guess.
 */
int scs_credit_header_offset(uint16_t total_sca_len);

/*
 * scs_credit_read_header - read the credit field out of a received frame's SCA
 * content. `sca` points at SCA offset 0 (absolute frame offset 14) and
 * `sca_len` is the SCA content length. Returns 0 and stores the value, or -1 if
 * the arguments are NULL, the length class has no grounded credit field, or the
 * buffer is too short. Independent of the kill switch: reading is never wrong.
 */
int scs_credit_read_header(const uint8_t *sca, size_t sca_len, uint16_t *out);

/*
 * scs_credit_stamp_header - write `credit` into an outbound frame's credit
 * field, same addressing as scs_credit_read_header. Returns 0, or -1 on NULL /
 * an unsupported length class / a short buffer / a value above 0xFFFF.
 *
 * UNDER THE KILL SWITCH THIS WRITES NOTHING and returns 0 -- the frame keeps
 * whatever the caller had put there. That is the wire-visible half of
 * OVMX_NO_CREDIT_ACCOUNTING.
 *
 * HAS NO PRODUCTION CALLER TODAY. See the reachability note: no OVMX builder
 * stamps a live credit, so no byte OVMX transmits is changed by this item.
 */
int scs_credit_stamp_header(uint8_t *sca, size_t sca_len, unsigned credit);

#ifdef __cplusplus
}
#endif

#endif /* SCS_CREDIT_H */
