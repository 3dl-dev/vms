/*
 * scs_rx.h - THE RECEIVE-SIDE SCS HEADER: the one place that decides which
 * inbound frames are SYSAP data and which Con.ID they are addressed to
 * (vms-7c0).
 *
 * WHY THIS EXISTS. p. 2-29 says the receive path is a table lookup: "the remote
 * port driver uses the low order 16 bits of the destination CONID as an index
 * into the remote node's CDL ... There it finds the address of the CDT ... The
 * port driver then offsets to a fixed location within the CDT to locate the
 * address of the remote SYSAP's message input routine, and passes the message
 * to that routine."  scs_cdt.c has implemented that lookup since vms-e1a and
 * NOTHING CALLED IT: scsd.c dispatched received frames by comparing a Con.ID
 * against three compile-time macros, per opcode, in six different branches.
 * This module supplies the missing first step -- decode the header, decide the
 * packet's kind, hand out the destination and source Con.IDs -- so that the
 * daemon can do the p. 2-29 lookup instead of the macro comparison.
 *
 * IT PARSES. IT DOES NOT DELIVER, ALLOCATE, LOG OR TRANSMIT. One pure function
 * over a const buffer.
 *
 * ===================== SOURCE AND GROUNDING (rule 8) =====================
 *
 * PUBLIC DOC. Roy G. Davis, *VAXcluster Principles*, Digital Press 1993:
 *   - p. 4-13 defines the SCS MTYPE taxonomy -- application message,
 *     application datagram, SCS control message.
 *   - Figure 4-5 (p. 4-14) shows an MSCP command nested under the SCS header
 *     `CREDIT -- SCS MTYPE, DEST CONID, SRC CONID`, i.e. the two 16-bit fields
 *     share a longword and the two Con.IDs follow.
 *   - p. 4-15 grounds dispatch ON MTYPE into the CDT's message input routine.
 *   - p. 4-68 states the testable rule "This field [credit] must contain a 0 in
 *     datagram packets since messages must be used to extend send credits."
 *
 * OBSERVATION. docs/cluster-protocol-spec.md sec 4(h)(1a)/(1b) grounds the
 * envelope layout, content-relative, for the 58/62/66/86/94/110/190-content
 * classes:
 *
 *     [42:44] inner length  == total_SCA_content - 44
 *     [44:46] format word   == 0x0004
 *     [46:48] MTYPE
 *     [48:50] credit
 *     [50:54] DESTINATION Con.ID   (the sender's CDT "remote Con.ID", p. 2-35)
 *     [54:58] SOURCE Con.ID        (the sender's CDT "local Con.ID",  p. 2-35)
 *     [58:total] the SYSAP payload
 *
 * The first two are used HERE as the envelope-conformance test, which is what
 * keeps the (1d) 70-content class -- a different shape, still undecoded -- from
 * being read with these offsets.
 *
 * ================= THE MESSAGE / DATAGRAM SPLIT: RE-DERIVED =================
 *
 * vms-7e7's rule ("the 0x4b13 / 0x5b13 msgtype word at content [16:18] is the
 * message-vs-datagram distinction") is REFUTED -- see spec sec 4(h)(1c). This
 * module does not inherit it. The classification below was MEASURED fresh for
 * vms-7c0 over the whole reference-lab corpus:
 *
 *   141 pcaps (/data/training/vax/cluster/{captures,captures-lab2,work}),
 *   981,367 envelope-conformant frames. MTYPE census, real-VAX sources only
 *   (Ethernet source OUI 08:00:2b or DECnet-logical aa:00:04), with the share
 *   carrying credit == 0 -- p. 4-68's necessary condition for a datagram:
 *
 *     MTYPE  0  6,084   credit0   0.0%      MTYPE  6    723   credit0 100.0%
 *     MTYPE  1  2,239   credit0 100.0%      MTYPE  7    729   credit0 100.0%
 *     MTYPE  2  1,103   credit0   0.0%      MTYPE  8    609   credit0   0.0%
 *     MTYPE  3    968   credit0 100.0%      MTYPE  9    238   credit0   0.0%
 *     MTYPE  4  1,133   credit0 100.0%      MTYPE 10  907,493  credit0  24.1%
 *     MTYPE  5  4,503   credit0 100.0%
 *
 * TWO RESULTS, AND THE SECOND IS A NEGATIVE THAT MUST NOT BE PAPERED OVER:
 *
 *   1. THE MTYPE NAMESPACE ON THIS WIRE IS EXACTLY {0..10}. 0..9 are the
 *      connection-control messages spec sec 4(h)(1a) already names; 10 is the
 *      p. 4-13 APPLICATION MESSAGE, which sec 4(h)(1b) identifies from the
 *      Figure 4-5 nesting of a real MSCP command. So MTYPE == 10 is the
 *      grounded discriminator for "this frame carries SYSAP data", and it is
 *      what SCS_RX_APP_MESSAGE means below.
 *
 *   2. NO APPLICATION-DATAGRAM MTYPE HAS EVER BEEN OBSERVED. There is no
 *      eleventh value in 981,367 frames. Nor does credit rescue the question:
 *      p. 4-68's rule is NECESSARY, NOT SUFFICIENT (a message that extends zero
 *      credits also carries 0), and 24.1% of MTYPE-10 frames carry credit 0, so
 *      "credit == 0 within MTYPE 10" cannot be the datagram marker either --
 *      that would classify 218,507 frames the corpus gives no reason to call
 *      datagrams.
 *
 * THEREFORE OVMX CANNOT IDENTIFY AN INBOUND APPLICATION DATAGRAM FROM THIS
 * WIRE, and this module does not pretend to: SCS_RX_UNKNOWN_MTYPE is what an
 * MTYPE outside {0..10} decodes to, the daemon COUNTS it, and no code path
 * claims a datagram it cannot recognise. That is why scs_cdl_deliver_datagram()
 * still has no production caller after vms-7c0 while scs_cdl_deliver_message()
 * has one. The honest shape of the remaining work is "find a datagram on a
 * wire, or on a documented one" -- not "pick a value".
 *
 * Re-derive it with the census script in the vms-7c0 branch history, or with
 * any tool that applies the (1b) envelope test and histograms [46:48]; the
 * numbers above are measured from the corpus, not remembered.
 *
 * THE COUNTS ABOVE ARE A DATED SNAPSHOT, THE CONCLUSION IS NOT. The reference
 * lab keeps writing captures, so the corpus GROWS and a later re-derivation
 * will not reproduce these totals -- a re-run on 2026-08-05 read 154 pcaps /
 * 1,002,247 envelope-conformant frames (942,251 real-VAX-source) and got
 * MTYPE 0..10 with MTYPE 10 at 923,678 / 23.9% credit-0. A DIFFERENT TOTAL IS
 * NOT A REGRESSION; a different SHAPE would be. What must still hold on re-run,
 * and what the code depends on, is: the MTYPE namespace is exactly {0..10},
 * MTYPE 10 dominates, and no eleventh value appears. If an MTYPE outside
 * {0..10} ever shows up, scsd's rx_unknown_mtype counter moves and the datagram
 * question below is finally answerable.
 */
#ifndef SCS_RX_H
#define SCS_RX_H

#include <stddef.h>
#include <stdint.h>

#include "scs_env.h" /* THE envelope: offsets, constants, build+parse (vms-ec7) */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * vms-ec7: THE OFFSETS AND CONSTANTS BELOW ARE ALIASES, NOT DEFINITIONS.
 *
 * This module was the tree's first envelope decoder and for a while it was the
 * only place the layout existed in code. Phase A of the MSCP epic made the
 * envelope a layer of its own (scs_env.h) because the SEND side needed the same
 * six fields and had six hand-written copies of them instead. The numbers now
 * live in exactly one place; these names remain so that every existing caller
 * and test keeps compiling, and so that a reader who arrives here is told where
 * the definition went rather than shown a second copy that can drift.
 */
#define SCS_RX_OFF_INNER_LEN  SCS_ENV_OFF_INNER_LEN
#define SCS_RX_OFF_FORMAT     SCS_ENV_OFF_FORMAT
#define SCS_RX_OFF_MTYPE      SCS_ENV_OFF_MTYPE
#define SCS_RX_OFF_CREDIT     SCS_ENV_OFF_CREDIT
#define SCS_RX_OFF_DEST_CONID SCS_ENV_OFF_DEST_CONID
#define SCS_RX_OFF_SRC_CONID  SCS_ENV_OFF_SRC_CONID

/* The SYSAP payload begins here (content-relative): the envelope's last field
 * ends at 58, and the 58-content class -- envelope and nothing else -- has
 * inner length 14, which is exactly [44:58]. */
#define SCS_RX_HDR_END SCS_ENV_HDR_END

/* [44:46] on every envelope-conformant frame in the corpus. */
#define SCS_RX_FORMAT_WORD SCS_ENV_FORMAT_WORD

/* p. 4-13 / spec sec 4(h)(1b): the application-message MTYPE. */
#define SCS_RX_MTYPE_APP_MESSAGE SCS_ENV_MTYPE_APP_MESSAGE

/* The highest connection-control MTYPE spec sec 4(h)(1a) names. */
#define SCS_RX_MTYPE_CONTROL_MAX SCS_ENV_MTYPE_CONTROL_MAX

enum scs_rx_kind {
    /* MTYPE 0..9: an SCA connection-control message. NOT delivered to a SYSAP
     * input routine -- the connection state machine consumes it. */
    SCS_RX_CONTROL = 0,
    /* MTYPE 10: the p. 4-13 application message. This is what p. 2-29 delivers
     * through the CDL to the CDT's message input routine. */
    SCS_RX_APP_MESSAGE = 1,
    /* Any other MTYPE. Never observed (see the census above). Counted, never
     * guessed at -- in particular NEVER assumed to be a datagram. */
    SCS_RX_UNKNOWN_MTYPE = 2
};

struct scs_rx_hdr {
    uint16_t total_sca_len; /* LE16(content[0:2]) + 2 */
    uint16_t inner_len;     /* content [42:44] */
    uint16_t mtype;         /* content [46:48] */
    uint16_t credit;        /* content [48:50] */
    uint32_t dest_conid;    /* content [50:54] -- indexes OUR CDL (p. 2-29) */
    uint32_t src_conid;     /* content [54:58] -- the sender's own handle */
    const uint8_t *payload; /* content + 58, or NULL when payload_len == 0 */
    size_t   payload_len;   /* total_sca_len - 58 */
    int      kind;          /* enum scs_rx_kind */
};

/*
 * scs_rx_parse - decode the sec 4(h)(1b) envelope of one received frame.
 *
 * `content` points at absolute frame offset 14 (the SCA length field) and `len`
 * is the number of bytes readable there. Returns 0 and fills *out only when the
 * frame is ENVELOPE-CONFORMANT:
 *
 *   - at least SCS_RX_HDR_END readable bytes, and
 *   - the declared total SCA content fits within `len`, and
 *   - content[44:46] == 0x0004, and
 *   - content[42:44] == total_sca_len - 44.
 *
 * Returns -1 otherwise, leaving *out untouched. A -1 is not an error: the HELLO
 * (120), the START (106) and the still-undecoded 70-content class all fail this
 * test by design (spec sec 4(h)(1d)), and the caller must keep handling them
 * the way it always has.
 *
 * NOTHING here validates the Con.IDs. Whether a destination Con.ID names one of
 * OUR connections is scs_cdl_lookup()'s decision, and it is the only place that
 * may index the CDL -- see the SECURITY note in scs_cdt.h's lookup contract:
 * the low 16 bits of a PEER-SUPPLIED value select a slot, so the bound check,
 * the slot-in-use check and the full-32-bit identity check all belong to the
 * lookup, not to this parser.
 */
int scs_rx_parse(const uint8_t *content, size_t len, struct scs_rx_hdr *out);

/* Static, non-NULL name for an enum scs_rx_kind value (for logs). */
const char *scs_rx_kind_name(int kind);

#ifdef __cplusplus
}
#endif

#endif /* SCS_RX_H */
