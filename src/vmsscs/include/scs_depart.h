/*
 * scs_depart.h - PATH-BLOCK TEARDOWN ON PEER DEPARTURE, in the order the book
 * requires (vms-17f).
 *
 * WHY THIS EXISTS. Three modules each own one piece of what has to happen when
 * a remote node goes away, and no single one of them can see all three:
 *
 *   scs_config.c  owns the Path Block and scs_pb_close()          (p. 2-17/2-21)
 *   scs_cdt.c     owns the per-circuit connection queue and the
 *                 VC-loss scan                                     (p. 2-28/2-30)
 *   scs_credit.c  owns the credit account and the Credit Wait queue (p. 2-45)
 *
 * The dependency chain runs config <- cdt <- credit, so the lowest layer -- the
 * one that owns scs_pb_close() -- is precisely the one that cannot call the
 * other two. That is how the vms-228 defect existed: scs_pb_close() zeroed a
 * Path Block without draining its connection queue, and all it could offer
 * instead was a comment saying somebody else must do it first. This module is
 * the somebody else. It sits at the TOP of the chain, so it can run the whole
 * sequence, and scs_pb_close() now REFUSES to run out of order
 * (SCS_PB_CLOSE_CONNECTIONS_QUEUED) rather than trusting a comment.
 *
 * SOURCE (public, quoted with page cites -- CLAUDE.md rule 8):
 *   Roy G. Davis, *VAXcluster Principles*, Digital Press 1993, ch. 2.
 *   - "SCA specifies that all CDTs corresponding to connections supported by a
 *     virtual circuit be queued to the Path Block corresponding to that circuit.
 *     If the circuit is broken for any reason, it is then a relatively simple
 *     matter to scan this queue to determine which connections have also been
 *     lost, and to notify the interested SYSAPs."                     p. 2-28
 *   - CDTs are "released (but not deallocated) so that they can then be reused"
 *                                                                     p. 2-30
 *   - the MFREEQ buffers a connection contributed belong to the PORT   p. 2-43
 *   - a Credit Wait suspends an operation by queuing it to the CDT     p. 2-45
 *   - VMS "keeps in a queue the System Blocks for nodes with which it has had at
 *     least one open virtual circuit" -- so the SB SURVIVES the teardown, which
 *     is what makes the p. 2-21 rejoin REFRESH reachable at all        p. 2-17
 *
 * THIS MODULE BUILDS NO FRAME AND OPENS NO SOCKET. It is nevertheless part of a
 * WIRE-VISIBLE change: it is what lets a returning peer be met with a fresh
 * formation dialogue instead of the silence an already-open Path Block produced.
 * The wire-visible decision -- WHEN a peer counts as departed -- is scsd.c's,
 * gated by the OVMX_NO_PEER_DEPART kill switch declared here.
 */
#ifndef SCS_DEPART_H
#define SCS_DEPART_H

#include <stdint.h>

#include "scs_cdt.h"
#include "scs_config.h"
#include "scs_credit.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ===== THE LISTEN TIMEOUT, AND WHERE ITS NUMBER COMES FROM =====
 *
 * OVMX has to decide when silence means "departed". The book (ch. 2) describes
 * what happens when a circuit is broken but publishes no timer for detecting it
 * -- that lives in the port drivers, which ch. 2 is explicitly not about. So the
 * value below is an OVMX DESIGN CHOICE (rule 8), bounded by measurement rather
 * than invented.
 *
 * MEASURED, from our own lab captures (tools/scs_peer_silence_measure.py
 * re-derives every figure and PASS/FAILs it against a checked-in table; run it
 * against /data/training/vax/cluster/captures/ on a lab host):
 *
 *   LONGEST silence from a HEALTHY peer -- 13 392 frames over 747 s of wire in
 *   three captures where no node left (ovmx-760-persist-10min,
 *   cd0-baseline-current, formation-ci1-joinwindow):                    3.153 s
 *
 *   Silence from a peer that ACTUALLY departed and came back -- VAX2's
 *   drop-and-rejoin in af2-established-rejoin-20260728.pcap, the capture
 *   docs/cluster-protocol-spec.md sec 4(i) is built on:               395.955 s
 *   (in that same capture VAX1, which stayed up throughout, never exceeds
 *   3.12 s -- the two populations do not overlap even inside one window.)
 *
 * Any threshold strictly between those two separates the observed populations.
 * SCS_DEPART_LISTEN_TIMEOUT_DEFAULT_MS is 20 s: the value of the lab's SYSGEN
 * parameter RECNXINTERVAL (20, recorded in docs/cluster-protocol-spec.md sec 3),
 * which is VMS's own "how long may this node be unreachable before it is removed
 * from the cluster" quantity. It is 6.3x the longest healthy silence measured
 * and 20x below the observed departure, so neither population is close to it.
 *
 * NOT A CLAIM THAT VMS USES 20 s FOR THIS. RECNXINTERVAL governs removal after a
 * circuit breaks, not the listen timeout that breaks it; borrowing its value is
 * an OVMX choice made because it is a real quantity from the same system in the
 * right range, not because the book assigns it this role.
 *
 * NOT A CLAIM THAT 3.153 s IS AN UPPER BOUND ON HEALTHY SILENCE EITHER. It is
 * the largest in 747 s of captured wire from a 2-3 node lab. A bigger cluster,
 * a loaded node or a lossy link could exceed it; the margin, not the maximum, is
 * what the choice rests on. If a healthy peer is ever seen departing in a lab
 * run, that measurement -- not this comment -- decides the number.
 */
#define SCS_DEPART_HEALTHY_SILENCE_MAX_MS   3160u   /* measured; see above */
#define SCS_DEPART_OBSERVED_DEPARTURE_MS    395950u /* measured; see above */
#define SCS_DEPART_LISTEN_TIMEOUT_DEFAULT_MS 20000u /* RECNXINTERVAL 20 */

/*
 * scs_depart_enabled - THE KILL SWITCH for the whole wire-visible departure
 * behaviour. 0 when OVMX_NO_PEER_DEPART is set to anything but "0", 1 otherwise.
 * Read fresh on every call so a test can bracket it (guardrail 23); production
 * calls it once per sweep, which is once per main-loop iteration.
 *
 * With the switch set, scs_pb_depart() tears nothing down and scsd.c's sweep
 * declares no peer departed -- i.e. OVMX behaves exactly as it did before
 * vms-17f, pinning the same peer slot and the same already-open Path Block.
 */
int scs_depart_enabled(void);

/*
 * scs_depart_listen_timeout_ms - how long a peer may be silent before it counts
 * as departed. SCS_DEPART_LISTEN_TIMEOUT_DEFAULT_MS unless
 * OVMX_PEER_LISTEN_TIMEOUT_MS names a different value in the environment (used
 * by the lab harness to force a departure inside a short run). A value that
 * would sit at or below the longest healthy silence ever measured
 * (SCS_DEPART_HEALTHY_SILENCE_MAX_MS) is still honoured -- a test and a lab run
 * both need to be able to ask for one -- but scsd.c LOGS when it is in force so
 * no capture is ever read as a spontaneous departure.
 */
uint64_t scs_depart_listen_timeout_ms(void);

/*
 * What one teardown did. Every field is a count, so a caller can log the
 * teardown without re-walking freed structures.
 */
struct scs_depart_stats {
    unsigned connections_lost;   /* CDTs found on the circuit (p. 2-28 scan) */
    unsigned handlers_notified;  /* of those, ones with a SYSAP error handler */
    unsigned waiters_flushed;    /* Credit Wait entries abandoned (p. 2-45) */
    unsigned mfreeq_reclaimed;   /* MFREEQ buffers returned to the port (p. 2-43) */
};

/*
 * scs_pb_depart - the remote node is gone: tear this virtual circuit's Path
 * Block down IN THE DOCUMENTED ORDER.
 *
 *   1. p. 2-28 VC-loss scan: notify every SYSAP with a connection on this
 *      circuit, BEFORE anything is destroyed (scs_cdl_vc_loss).
 *   2. For each connection still on the circuit: abandon its Credit Wait queue
 *      (p. 2-45 -- a broken connection will never grant credit again, so the
 *      waiters can never be resumed) and release the CDT, which returns its
 *      MFREEQ share to the port (p. 2-43) and leaves it in its CDL slot for
 *      reuse (p. 2-30).
 *   3. Only now close the Path Block. The System Block STAYS in the
 *      configuration queue (p. 2-17), which is what makes the returning node's
 *      scs_pb_open() find an SB with an empty PB queue and take the p. 2-21
 *      REFRESH.
 *
 * A SYSAP error handler is entitled to release its own connection in step 1;
 * step 2 re-reads the queue head each time, so that is safe.
 *
 * `cdl` may be NULL (scs_cdl_release ignores it -- the CDT stays in its slot
 * either way). `stats` may be NULL.
 *
 * Returns the scs_pb_close() result. SCS_PB_CLOSE_CONNECTIONS_QUEUED can still
 * come back if a vc_loss handler queued a NEW connection to the dying circuit;
 * that is reported rather than forced, because forcing it would destroy a
 * structure a SYSAP is holding. Returns SCS_PB_CLOSE_NOTHING and does nothing at
 * all when the kill switch is set or when pb is NULL/not in use.
 */
enum scs_pb_close_result scs_pb_depart(struct scs_cdl *cdl, struct scs_config *cfg,
                                       struct scs_pb *pb, struct scs_depart_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* SCS_DEPART_H */
