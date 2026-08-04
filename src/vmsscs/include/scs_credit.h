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
 * captures the credit field IS at a single fixed offset for the SCS message
 * classes:
 *
 *   SCS credit field = SCA offset [48:50], LE uint16
 *                    = absolute frame offset [62:64] with the 14-byte Ethernet
 *                      header, i.e. the 2 bytes IMMEDIATELY PRECEDING the
 *                      remote/destination Con.ID at SCA [50:54].
 *
 * ---- HOW TO RE-DERIVE EVERY NUMBER BELOW (a script, not a promise) -------
 *
 *   $ tools/scs_credit_measure.py --quick     # the two grounding captures, ~5s
 *   $ tools/scs_credit_measure.py             # all 47 captures, ~5-10 min
 *
 *   That script re-measures every figure in this WIRE VERDICT from the raw
 *   pcaps and PASS/FAILs each against a checked-in EXPECTED table. Last full
 *   run 2026-08-03 on workshop: 30 checks, 0 failures. It needs the lab-1 captures, which
 *   are host-only and not in git, so ctest cannot run it; what ctest DOES run
 *   (scs_credit_figures) is the cheap half -- it asserts that every number in
 *   EXPECTED still appears verbatim in this header and in
 *   docs/cluster-protocol-spec.md, so the prose cannot drift away from the
 *   measurement even on a machine with no captures. A comment is not evidence;
 *   the script is.
 *
 *   The method it implements, spelled out:
 *
 *   Inputs: the two lab captures
 *       /data/training/vax/cluster/captures/formation-ci1.pcap        (18558 frames)
 *       /data/training/vax/cluster/captures/formation-ci1-joinwindow.pcap (3000 frames)
 *   For each captured Ethernet frame let  sca = frame[14:]  (strip the 14-byte
 *   Ethernet header; SCA offset 0 is absolute offset 14).
 *
 *   >>> TWO DIFFERENT POPULATIONS -- READ WHICH ONE EACH LINE USES. <<<
 *
 *   (A) THE WHOLE SCS FAMILY -- keep a frame if  len(sca) == <class>.  No
 *       marker filter. Evidence line 1 (CONSERVATION) uses THIS, and it MUST:
 *       a debit/credit account only balances if you count every message on the
 *       connection. Of the 20459 190-byte frames in the two captures the marker
 *       split is {0x4B13: 19860, 0x5B13: 591, 0x7B13: 8} -- i.e. ALL of them
 *       are SCS messages of the 0x?B13 family (the header note ~40 lines below
 *       records that 0x5B13 and 0x7B13 carry the same credit field at the same
 *       offset). "All 190-byte frames" and "the whole 0x?B13 family at 190
 *       bytes" are therefore the SAME SET here, and the script asserts that
 *       equality rather than assuming it. Dropping the 591+8 siblings drops
 *       credits that were genuinely granted while still counting the messages
 *       they paid for, and the identity below REFUTES instead of holding. The
 *       measured refutation figures are quoted ONCE, in the CORRECTION section
 *       below -- deliberately not repeated here, so that a drift in one copy
 *       cannot be masked by a stale second copy (the docs gate checks the
 *       figures by value, and duplicates defeat it).
 *
 *   (B) 0x4B13 ONLY -- keep a frame if  len(sca) == <class>  AND
 *       sca[16:18] == 4B 13.  Evidence lines 2 and 3 and the per-class table
 *       use THIS, because those are single-marker value-shape statements, not
 *       accounting identities. 19860 of the 20459 190-byte frames survive it.
 *
 * Three independent lines of evidence, all from those two captures:
 *
 *  1. CONSERVATION over the 190-byte steady-state class. POPULATION (A): ALL
 *     190-byte frames, NO marker filter. Summing [48:50] over every 190-byte
 *     frame a node SENDS, against the number of 190-byte messages it RECEIVED
 *     (nodes are the source MACs aa:00:04:00:01:04 = VAX1 and 08:00:2b:78:56:b9):
 *        formation-ci1:  VAX1 granted 10842 vs peer sent 10842  (delta 0)
 *                        peer granted  6712 vs VAX1 sent  6715  (delta 3)
 *        joinwindow:     VAX1 granted  1601 vs peer sent  1602  (delta 1)
 *                        peer granted  1300 vs VAX1 sent  1300  (delta 0)
 *     All four reproduce EXACTLY under population (A) -- re-run
 *     tools/scs_credit_measure.py --quick. Total credits returned equals total
 *     messages sent to within the end-of-capture tail. That is exactly the
 *     p. 2-43 debit/credit identity and no other field in the header satisfies
 *     it.
 *  2. VALUE SHAPE. POPULATION (B). Over 19860 190-byte 0x4B13 frames the
 *     field takes only {0:5174, 1:10696, 2:2582, 3:1405, 4:3} -- the shape of a
 *     piggybacked Pending Receive Credit in a near-lockstep flow, not a counter.
 *     (Unfiltered, population (A), the same histogram is
 *     {0:5418, 1:11042, 2:2587, 3:1409, 4:3} over 20459 frames -- same shape,
 *     which is the independent reason the siblings belong in line 1.)
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
 * ---- CORRECTION: THE FILTER WAS SCOPED WRONG (adversary-caught) ----------
 *
 * An earlier revision of this block headed the 0x4B13 filter ">>> FILTER,
 * REQUIRED -- the counts below do NOT reproduce without it <<<" and put it
 * above ALL THREE evidence lines. For line 1 that was exactly INVERTED: the
 * filter does not enable the conservation numbers, it DESTROYS them. Applying
 * it to line 1 gives formation-ci1 VAX1 10817 granted vs 10266 peer-sent
 * (delta -551) and peer 6369 vs 6695 (delta +326) -- the debit/credit identity
 * REFUTED. A reader following the old comment literally would have derived a
 * refutation of the primary grounding. The four figures printed in line 1 were
 * always the population-(A) numbers and are correct; only the recorded method
 * was wrong. Hence the (A)/(B) split above.
 *
 * THE OFFSET CONCLUSION IS UNAFFECTED. This was a recorded-method defect, not
 * a wrong verdict: [48:50] is still the credit field, all four conservation
 * figures still reproduce, and lines 2 and 3 are untouched.
 *
 * (One report in that review does not correspond to anything on this branch: a
 * claimed figure "VAX1 granted 6708 vs peer sent 6708 (delta 0)". No revision
 * of this header or of the spec ever contained 6708 -- `git log -S6708` over
 * both files is empty and the first commit already read 10842/10842. The
 * delta-0 partner is and always was 10842 vs 10842. Recorded here so the next
 * reader does not go looking for a number that was never written.)
 *
 * ---- SCOPE OF THE GROUNDING: EVERY ADMITTED CLASS IS MEASURED ------------
 *
 * Offset 48 is asserted only for the SCS *message* length classes admitted by
 * scs_credit_header_offset(), and each one was measured. POPULATION (B)
 * (sca = frame[14:], sca[16:18] == 4B 13), swept over ALL 47 .pcap files in
 * /data/training/vax/cluster/captures/, tabulating the LE u16 at sca[48:50]:
 *
 *     class   n frames   distinct values   max   -> ADMITTED (credit-shaped)
 *        58        1212        2             1
 *        62        1087        1             0
 *        66         944        1             0
 *        86         194        1             1
 *        94        3670        2             1
 *       110        3999        5            10   (the tunables: 1,3,6,8,10)
 *       190      288484        5             4
 *
 *     class   n frames   distinct values   max   -> REFUSED (not credit-shaped)
 *        50          51       47         64897
 *        70         917      752         65447
 *       122           4        3         10709
 *       126           3        3         64891
 *       142           8        8         21361
 *   plus 82/206/242/270/... (the block-data-transfer classes) and the 41-byte
 *   0x48 short, which is too short to reach offset 48 at all.
 *
 * WHY 106 IS NOT HERE (this list previously claimed it, wrongly). An earlier
 * revision of this header listed 106 among the grounded classes. It is not
 * grounded and it is not merely unmeasured -- it is WRONG. Over all 47
 * captures there are ZERO 106-byte SCA frames with the 0x4B13 SCS marker. All
 * 792 106-byte SCA frames that exist carry marker 0x4113: they are the START /
 * config frames of spec sec 4(j), a different protocol layer with no credit
 * field, and sca[48:50] there is a constant 0 in 792/792 frames (part of the
 * config body, not an account). The 106 entry came from misreading the spec's
 * FRAME-length listing of the 0x41 START class as an SCA message class. It has
 * been deleted from the switch, not relabelled: there is nothing to label.
 *
 * WHAT THE LENGTH KEY DOES NOT DO. scs_credit_header_offset() keys on LENGTH
 * ONLY and cannot tell an SCS message from anything else of the same size --
 * the caller must have identified the frame already. Populations (A) and (B)
 * above are grounding methods, not runtime checks. (For completeness: the sibling
 * markers 0x5B13 and 0x7B13 at these same lengths carry the same
 * credit-shaped values -- 190-byte 0x5B13 n=18086 max=3, 0x7B13 n=100 max=3;
 * 110-byte 0x5B13 n=675 max=10, 0x7B13 n=106 max=10 -- so the offset holds
 * across the SCS 0x?B13 family. 0x4113 is the one that does not, and that is
 * exactly the 106 case above.)
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
 * vms-1d2 adds a SECOND, narrower switch -- OVMX_NO_CREDIT_WAIT=1 -- which
 * gates only the Credit Wait queue and the special credit message trigger and
 * leaves the account itself running. See the CREDIT WAIT section below.
 *
 * OVMX DESIGN CHOICES (labeled per rule 8):
 *   - The counts are plain `unsigned` in the CDT, not a VMS-layout structure.
 *     The book documents the CONTENTS of the credit account, never a byte
 *     layout for it; nothing here but the [48:50] field reaches the wire.
 *   - Credit Wait is reported, not queued. **SUPERSEDED BY vms-1d2 -- see the
 *     CREDIT WAIT section below.** vms-76e wrote: "p. 2-45 blocks by queuing
 *     the CDRP to the CDT; OVMX has no CDRP, so scs_credit_on_send() REFUSES
 *     the send and the caller is expected to retry." That is still exactly what
 *     scs_credit_on_send() does, and it is still an honest -1 rather than a
 *     silent success -- but it is no longer the whole mechanism. vms-1d2 adds
 *     the queue (scs_credit_send_or_wait) with struct scs_credit_waiter as the
 *     CDRP stand-in, because refuse-and-retry drops the p. 2-46 FIFO ordering
 *     guarantee. Under OVMX_NO_CREDIT_WAIT=1 the module reverts to this
 *     sentence exactly.
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

/* --- CREDIT WAIT: the queue on the CDT (pp. 2-45..2-46, vms-1d2) ---------- */

/*
 * WHAT vms-76e LEFT UNDONE, AND WHAT vms-1d2 ADDS.
 *
 * vms-76e implemented the p. 2-45 Credit Wait TEST (scs_credit_can_send) and
 * made scs_credit_on_send REFUSE at zero credit, with this design note: "Credit
 * Wait is reported, not queued ... OVMX has no CDRP, so scs_credit_on_send()
 * REFUSES the send and the caller is expected to retry." That is only half the
 * mechanism, and the missing half is the part the book actually specifies:
 *
 *   "If no Send Credits are available, then this routine temporarily suspends
 *    the operation involved by placing it in a Credit Wait. This is done by
 *    queuing the CDRP representing the operation to the CDT for the
 *    connection.
 *    Whenever the Send Credit count in a CDT is increased, the CDT's queue of
 *    waiting CDRPs is examined. If that queue is nonempty, as many waiting
 *    CDRPs as possible are resumed, based on the number of Send Credits
 *    currently available."                                          p. 2-45
 *   "Each of the SCS wait queues described here is a 'first in first out'
 *    queue. The longer a CDRP has been in the queue, the closer it is to the
 *    head of that queue. Queue priority is based on time spent in the queue.
 *    When a resource associated with the queue (e.g., Send Credit, BDT entry,
 *    etc.) becomes available, the CDRP at the head of the queue has priority
 *    for receiving that resource."                                  p. 2-46
 *   "When an operation is suspended due to an SCS Wait, the address of the
 *    instruction at which to resume the operation is kept in the CDRP."
 *                                                                   p. 2-46
 *
 * A refuse-and-retry contract is NOT that: it drops the ordering guarantee
 * (p. 2-46 makes queue position a function of time waited) and it makes the
 * resume edge invisible, so nothing can ever observe "the credit arrived and
 * the starved sender went". This module now queues.
 *
 * SCOPE. Only Credit Wait. The other three VMS SCS Waits on pp. 2-45..2-46 --
 * Pool Wait (CDRP queued to the PDT), BDT Wait, RDT Wait -- are deliberately
 * NOT implemented here; OVMX has no nonpaged pool, no Block Data Transfer
 * table and no Request Descriptor Table to wait on.
 *
 * OVMX DESIGN CHOICES (labeled per rule 8):
 *   - struct scs_credit_waiter stands in for the VMS CDRP. p. 2-46 keeps a
 *     resume PC and "the contents of a couple of registers" in the CDRP; C has
 *     no such thing, so OVMX keeps a function pointer plus a void* context.
 *     The book publishes no CDRP byte layout and none is invented here.
 *   - The waiter is CALLER-OWNED storage. This module allocates nothing (same
 *     rationale as the rest of vmsscs: the daemon gets no allocation failure
 *     path). The caller must keep the waiter alive until it is resumed or
 *     cancelled.
 *   - The queue is singly linked with a tail pointer. VMS uses its
 *     doubly-linked queue primitives; the observable behaviour required by
 *     pp. 2-45..2-46 is FIFO order and O(1) append, which this gives.
 *
 * KILL SWITCH -- OVMX_NO_CREDIT_WAIT=1 (this item's, distinct from vms-76e's
 * OVMX_NO_CREDIT_ACCOUNTING). With it set, BOTH mechanisms vms-1d2 adds are
 * suppressed and the module reverts EXACTLY to the vms-76e behaviour:
 *   - scs_credit_send_or_wait() never queues; at zero credit it returns -1,
 *     which is what scs_credit_on_send() has always done;
 *   - a rise in Send Credit does not examine the queue, so no waiter is ever
 *     resumed;
 *   - scs_credit_on_recv() does not fire the special credit emitter.
 * scs_credit_take_special() is deliberately NOT gated: it is vms-76e's, it
 * emits nothing by itself, and the vms-76e kill-switch test asserts its
 * behaviour. Setting OVMX_NO_CREDIT_ACCOUNTING=1 also suppresses everything
 * here, because with no account there is nothing to wait on.
 *
 * ===== REACHABILITY: THERE IS NO PRODUCTION CALLER. Stated plainly because
 * this epic keeps rejecting claims that outrun their evidence. =====
 *   - Nothing in src/vmsscs/scsd.c calls scs_credit_send_or_wait,
 *     scs_credit_wait_release, scs_credit_set_special_emitter or anything else
 *     declared in this header. vmsscs_credit is still not linked into
 *     scsd_exe (see src/vmsscs/CMakeLists.txt) and `nm SCSD.EXE` shows no
 *     symbol from this header. The daemon is BYTE-UNAFFECTED.
 *   - Therefore NO OVMX SENDER CAN CURRENTLY ENTER A CREDIT WAIT, and OVMX
 *     emits no special credit message. What is proven by the tests is that the
 *     mechanism is correct, not that OVMX runs it.
 *   - The special credit message additionally cannot be BUILT: OVMX has not
 *     grounded which wire frame class carries one. That is filed as an
 *     explicit RE gap in docs/cluster-protocol-spec.md sec 5. This module
 *     decides WHEN one is owed and WHAT count it carries, and hands that to a
 *     hook; it assembles no frame. The 41-byte 0x48 short is NOT it -- sec 4(h)
 *     grounds that class as a strict 1-for-1 sequence ack with no locatable
 *     credit count (622/622 frames), and it is too short to reach SCA
 *     offset 48 anyway.
 *   - Wiring both to the daemon needs the SCS connection state machine
 *     (vms-dd5) and per-peer connection identity, which this item is fenced
 *     out of.
 */

/* scs_credit_send_or_wait() return code: the operation was placed in a Credit
 * Wait on the CDT and will be resumed later (p. 2-45). Distinct from -1, which
 * stays "refused / error", so a caller cannot confuse suspended with failed. */
#define SCS_CREDIT_WAIT (-2)

struct scs_credit_waiter;

/*
 * scs_credit_resume_fn - "the address of the instruction at which to resume the
 * operation" (p. 2-46), as a C callback. `credit` is the value the resumed send
 * MUST place in its header credit field -- exactly what scs_credit_on_send()
 * would have returned, because the resume path performs the same debit and the
 * same piggyback-and-reset.
 */
typedef void (*scs_credit_resume_fn)(struct scs_credit_waiter *w, unsigned credit, void *ctx);

/*
 * struct scs_credit_waiter - one suspended send. OVMX's stand-in for the CDRP
 * that p. 2-45 queues to the CDT. CALLER-OWNED storage; zero it before first
 * use. Fields other than `resume`/`ctx` are owned by this module.
 */
struct scs_credit_waiter {
    scs_credit_resume_fn resume; /* caller sets; may be NULL */
    void                *ctx;    /* caller sets; passed back to resume */

    struct scs_cdt          *cdt;       /* the connection it is queued to */
    struct scs_credit_waiter *next;     /* FIFO link, module-owned */
    int                       queued;   /* 1 while in a Credit Wait */
    int                       resumed;  /* 1 once resumed (never reset here) */
    unsigned                  credit;   /* the piggyback value it was resumed with */
};

/*
 * scs_credit_wait_enabled - 0 when OVMX_NO_CREDIT_WAIT=1 is set in the
 * environment, 1 otherwise. Read once and cached; scs_credit_reset_switch_cache
 * re-reads it along with OVMX_NO_CREDIT_ACCOUNTING.
 */
int scs_credit_wait_enabled(void);

/*
 * scs_credit_send_or_wait - the p. 2-45 send-buffer-allocation routine: "Before
 * actually allocating the buffer, this routine first verifies that at least one
 * Send Credit is available on the connection being used."
 *
 * If a Send Credit is available AND THE CREDIT WAIT QUEUE IS EMPTY, this
 * behaves exactly like scs_credit_on_send: debits one Send Credit and returns
 * (>= 0) the Pending Receive Credit to piggyback, which it resets. `w` is left
 * untouched and unqueued -- the caller sends immediately, on its own stack.
 *
 * THE QUEUE-EMPTY CONDITION IS NOT AN OPTIMISATION, it is p. 2-46: "Queue
 * priority is based on time spent in the queue ... the CDRP at the head of the
 * queue has priority for receiving that resource." A send that arrives while
 * operations are already suspended has spent no time in the queue, so it may
 * not take a credit ahead of them even when one is free -- it goes to the tail
 * like any other. (Free credit and a non-empty queue coexist whenever a resumed
 * operation issues another send: the release pass fixes how many waiters it
 * will resume at the moment the count rises, so a grant larger than the depth
 * at that instant leaves credit behind.)
 *
 * Otherwise -- no Send Credit, or the queue is non-empty -- `w` is appended to
 * the TAIL of this CDT's Credit Wait queue and SCS_CREDIT_WAIT is returned.
 * NOTHING IS SENT and no count moves. `w->resume` is called later, from
 * whatever call increases the Send Credit count.
 *
 * Returns -1 (and queues nothing) if cdt or w is NULL, if w is already queued,
 * or if the Credit Wait kill switch is set and there is no credit -- the
 * vms-76e refusal.
 */
int scs_credit_send_or_wait(struct scs_cdt *cdt, struct scs_credit_waiter *w);

/*
 * scs_credit_wait_depth - number of operations currently in a Credit Wait on
 * this connection. 0 for NULL.
 */
unsigned scs_credit_wait_depth(const struct scs_cdt *cdt);

/*
 * scs_credit_wait_release - "Whenever the Send Credit count in a CDT is
 * increased, the CDT's queue of waiting CDRPs is examined. If that queue is
 * nonempty, as many waiting CDRPs as possible are resumed, based on the number
 * of Send Credits currently available." (p. 2-45)
 *
 * Resumes min(depth, Send Credit) waiters in FIFO order (p. 2-46), debiting one
 * Send Credit per waiter and handing each the piggyback credit its send must
 * carry -- so the FIRST waiter released carries the whole outstanding Pending
 * Receive Credit and the rest carry 0, exactly as a run of scs_credit_on_send
 * calls would. Returns the number resumed.
 *
 * PRODUCTION DOES NOT CALL THIS DIRECTLY. It is called for you by the two
 * functions that raise Send Credit -- scs_credit_grant_from_peer() and
 * scs_credit_on_recv() -- which is what "whenever the Send Credit count is
 * increased" means. It is exported so a caller that manipulates the count by
 * some other route can honour the same rule, and so the drain can be asserted
 * in isolation. A test that calls it BY HAND proves only the drain, never the
 * trigger. BOTH triggers are driven by tests that never call this function:
 * test_credit_wait_released_fifo drives scs_credit_on_recv() and
 * test_credit_wait_released_by_peer_grant drives scs_credit_grant_from_peer().
 *
 * Reentrant calls (a resume callback that receives on the same connection)
 * return 0 rather than draining the queue out of order.
 */
unsigned scs_credit_wait_release(struct scs_cdt *cdt);

/*
 * scs_credit_wait_cancel - remove a waiter from the Credit Wait queue without
 * resuming it (the operation was aborted). Returns 0 if it was dequeued, -1 if
 * cdt or w is NULL or w is not queued on cdt. No credit moves.
 */
int scs_credit_wait_cancel(struct scs_cdt *cdt, struct scs_credit_waiter *w);

/*
 * scs_credit_wait_flush - dequeue EVERY waiter without resuming any, returning
 * how many were dropped. For connection teardown: the book resumes waiters only
 * when credit arrives, and a broken connection will never grant credit again.
 *
 * scs_cdl_release() does NOT call this -- scs_cdt.c does not know about the
 * credit account (see scs_cdt.h) and connection teardown is vms-17f's surface,
 * not this item's. A caller that releases a CDT with waiters still queued
 * abandons them; that is stated here rather than silently handled.
 */
unsigned scs_credit_wait_flush(struct scs_cdt *cdt);

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
 *
 * THIS FUNCTION NEVER QUEUES. It reports the starvation and leaves the caller
 * to decide; a caller that wants the p. 2-45 mechanism -- to be SUSPENDED on
 * the CDT and resumed FIFO when credit arrives -- calls
 * scs_credit_send_or_wait() instead. It also ignores the Credit Wait queue, so
 * calling it while operations are suspended lets this send overtake them; that
 * is why the queue-aware entry point exists.
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

/*
 * scs_credit_set_special_emitter - install the hook that SENDS a special credit
 * message on this connection (vms-1d2). p. 2-44: "Each time the local node
 * receives a message on a connection, it checks to see if the local Receive
 * Credit count for the connection is 'dangerously low'. If it is, and if the
 * local Pending Receive Credit count is greater than 0, local SCS IMMEDIATELY
 * sends remote SCS a special credit message containing the local Pending
 * Receive Credit count."
 *
 * That check is performed inside scs_credit_on_recv() -- "each time the local
 * node receives a message" is a receive-path trigger, not something a caller
 * polls. When it fires, `fn` is called with the count the message must carry,
 * and the Pending Receive Credit is already reset to 0 (p. 2-44) with the
 * Receive Credit raised by the same amount.
 *
 * `fn` may be NULL to unhook. Returns 0, or -1 if cdt is NULL.
 *
 * OVMX CANNOT BUILD THE FRAME. Which wire class carries an SCA special credit
 * message is an open RE gap (docs/cluster-protocol-spec.md sec 5); this hook is
 * where a builder would go, and today nothing installs one. See the
 * reachability note above.
 */
int scs_credit_set_special_emitter(struct scs_cdt *cdt, scs_credit_special_fn fn, void *ctx);

/*
 * scs_credit_set_remote_min_send_credits - record the Minimum Send Credits
 * argument "passed to the CONNECT or ACCEPT service by the remote SYSAP during
 * connection formation" (p. 2-44) in the CDT, independently of
 * scs_credit_extend().
 *
 * scs_credit_extend() already takes it, which covers the case where the local
 * SYSAP extends its buffers knowing the peer's value. It does not cover the
 * ordinary case: OVMX sends CONNECT_REQ (extending its own credits) and only
 * LEARNS the peer's Minimum Send Credits when the ACCEPT_REQ comes back. This
 * setter is that second edge. It changes only the dangerously-low threshold;
 * no count moves and no message is emitted.
 *
 * Returns 0, or -1 if cdt is NULL. A no-op returning 0 under
 * OVMX_NO_CREDIT_ACCOUNTING.
 *
 * NOT WIRE-DERIVED: no capture pins a Minimum Send Credits field. The 110-byte
 * CONNECT_REQ/ACCEPT_REQ credit field at SCA [48:50] carries the EXTENDED Send
 * Credits (grounded -- see the WIRE VERDICT, tunable match), which is a
 * different quantity. Where the remote's Minimum Send Credits rides on the wire
 * is an open gap (docs/cluster-protocol-spec.md sec 5); this call is how a
 * parser would deliver it once it is found.
 */
int scs_credit_set_remote_min_send_credits(struct scs_cdt *cdt, unsigned n);

/* --- the wire field (see the WIRE VERDICT) -------------------------------- */

/*
 * scs_credit_header_offset - offset of the credit field within the SCA content
 * of a message whose total SCA length is `total_sca_len`, or -1 if that length
 * class is not one this module has grounded a credit field for.
 *
 * GROUNDED CLASSES -- and this list contains nothing else. The SCS message
 * classes 58, 62, 66, 86, 94, 110 and 190 SCA bytes. Every one of the seven was
 * measured over our own captures and observed to carry a credit-shaped value at
 * offset 48; see the per-class table in the WIRE VERDICT above for n, distinct
 * values and max. There are NO extrapolated entries: a length class that was
 * not measured is not admitted.
 *
 * Everything else -- notably the block-data-transfer classes, which carry
 * unrelated large values at offset 48; the 41-byte 0x48 short, which is too
 * short to reach offset 48 at all; and the 106-byte class, which is the 0x4113
 * START/config frame and not an SCS message (see "WHY 106 IS NOT HERE") --
 * returns -1 rather than a guess.
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
