/*
 * scs_reason.h - vms-6b3: the 16-bit REJECT/DISCONNECT reason code.
 *
 * THE BOOK (VAXcluster Principles, Davis 1993, p. 2-26):
 *
 *   "When a SYSAP rejects a CONNECT_REQ or explicitly breaks an open
 *    connection, it also has the option of providing the other SYSAP a 16-bit
 *    'reason code' explaining why it did so. In the case of rejecting a
 *    CONNECT_REQ, the reason code is included in the REJECT_REQ packet. When an
 *    open connection is broken, the reason code is included in a disconnect
 *    request packet that is sent from one node to the other." (p. 2-26)
 *
 *   "The DISCONNECT_REQ identifies the connection being broken; and it contains
 *    an optional disconnect reason code if one is specified by the SYSAP."
 *    (p. 2-26)
 *
 * So the chapter grounds THREE things and no more: the field exists, it is
 * 16 bits, and it rides REJECT_REQ and DISCONNECT_REQ. It publishes neither the
 * byte OFFSET nor a single code VALUE. Both of those are OVMX design choices
 * below, and they are LABELED as such -- see "WHAT IS GROUNDED" and
 * docs/cluster-protocol-spec.md sec 5.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS GROUNDED, AND WHAT THE MEASUREMENT ACTUALLY SHOWED (vms-6b3)
 * ---------------------------------------------------------------------------
 *
 * The offset was searched for in our own captures before being chosen. Method,
 * reproducible with tools/cluster/scs_reason_measure.py.
 *
 *   >>> EVERY MEASURED COUNT BELOW IS ON A LINE BEGINNING `CENSUS-`. Those
 *   >>> lines are PARSED, field by field, and compared against the EXPECTED
 *   >>> table in tools/cluster/scs_reason_measure.py by the ctest gate
 *   >>> `scs_reason_figures`, which also parses the same figures out of
 *   >>> docs/cluster-protocol-spec.md. Do not hand-edit a digit on a CENSUS-
 *   >>> line: re-run the script on a lab host and update EXPECTED. Each count
 *   >>> lives on exactly ONE such line, so there is no second copy for a
 *   >>> drifted figure to hide behind. THAT IS THE POINT -- the two defects
 *   >>> review round 2 found were both figures that only a comment carried.
 *   >>>
 *   >>> The remaining numbers in this comment are byte OFFSETS and slot
 *   >>> boundaries, not counts: [46:48], [50:58], [18:50], payload 50 and 58.
 *   >>> Those are grounded in spec sec 4(h)(1a) or pinned by the macros below
 *   >>> (SCS_REASON_PAYLOAD_OFF), and the gate checks them against the
 *   >>> script's own constants rather than against a census.
 *   >>>
 *   >>> AND: THE TWO REFUTED CLAIMS CANNOT BE WRITTEN BACK IN. Both are
 *   >>> quoted -- and killed -- lower down this comment, each inside a
 *   >>> QUARANTINE BLOCK. The gate reds if either is asserted anywhere else in
 *   >>> this file or in the spec, in ANY wording: it matches the claim family
 *   >>> (subject + constancy assertion), not a sentence, and there is no
 *   >>> proximity excuse -- writing the claim next to the word "refuted" does
 *   >>> not license it. If you must write one down, copy the shape of a
 *   >>> quarantine block below; the gate names the exact markers when it reds,
 *   >>> and checks that the block is small, says the claim is refuted, and
 *   >>> holds no CENSUS-/REFUTATION-FACT line. What the gate ACTUALLY kills is
 *   >>> measured, never claimed, by the ctest mutation battery
 *   >>> tests/vmsscs/test_scs_reason_mutants.py -- read its output.
 *
 *   POPULATION RULE. Every SCA frame in the connection-control length classes
 *   (spec sec 4(h)(1a)), from a VMS-origin source MAC (DEC OUI 08-00-2b or the
 *   LAVC logical aa-00-04-00-xx-04) so that no OVMX-emitted frame can be
 *   mistaken for a VMS one, over the whole lab capture set:
 *
 *     CENSUS-P sca_len_classes=62,66,110 pcaps_scanned=47
 *
 *   Message type is payload [46:48]; the Con.ID pair is payload [50:58].
 *
 *   CENSUS-A -- THE TWO FRAMES p. 2-26 SAYS CARRY THE REASON CODE, and the two
 *   16-bit words that follow the Con.ID pair:
 *
 *     CENSUS-A type=4 name=REJECT_REQ     frames=453 pcaps=19
 *     CENSUS-A type=4 off=58 values=0x0000:453
 *     CENSUS-A type=4 off=60 values=0x0001:453
 *     CENSUS-A type=6 name=DISCONNECT_REQ frames=220 pcaps=25
 *     CENSUS-A type=6 off=58 values=0x0000:220
 *     CENSUS-A type=6 off=60 values=0x0000:131,0x0001:89
 *
 *   Read off those six lines: payload [58:60] is 0x0000 in EVERY frame of BOTH
 *   carriers, while payload [60:62] is a constant on REJECT_REQ but VARIES on
 *   DISCONNECT_REQ. So [60:62] is a live, undecoded field and MUST NOT be used.
 *
 *   THE POSITIVE FACT, PINNED. These two lines are parsed and compared against
 *   EXPECTED by the same gate. They are the measurements that KILL the two
 *   claims quarantined below, so a contradicting sentence cannot quietly
 *   coexist with them, and deleting one is as loud as asserting the claim:
 *
 *     REFUTATION-FACT off=60 type=6 distinct_values=2 values=0x0000:131,0x0001:89
 *     REFUTATION-FACT off=58 len=62 type=3 name=ACCEPT_RSP nonzero=62
 *
 *   REFUTED-QUOTE-BEGIN
 *   (An earlier revision of the spec said "nothing after the Con.ID pair
 *   varies". The CENSUS-A off=60 line for type=6 refutes it.)
 *   REFUTED-QUOTE-END
 *
 *   THE SECOND ORACLE AGREES. SDA `SHOW CONNECTIONS` prints a per-CDT field
 *   literally named "Rej/Disconn Reason". Counted out of the captured extract,
 *   not asserted (`cdts` is how many CDTs it printed, `values` the histogram):
 *
 *     CENSUS-D sda_file=sda-scs-extract-vax1.txt cdts=12 values=0:12
 *
 *   So the field is real and named by the VMS oracle, and both the wire and SDA
 *   say every reason code our lab ever produced was ZERO.
 *
 *   CONCLUSION, stated exactly: **the offset is NOT GROUNDED and cannot be,
 *   from the data we hold.** No VMS node in any observed REJECT/DISCONNECT
 *   frame ever set a nonzero reason code, so there is no varying field to
 *   localize.
 *
 * ---------------------------------------------------------------------------
 * WHY payload [58:60] -- AND THE RATIONALE THAT WAS REFUTED (read this)
 * ---------------------------------------------------------------------------
 *
 *   REFUTED-QUOTE-BEGIN
 *   The FIRST revision of this file justified the slot as "the only 16-bit
 *   slot in either frame that is zero in 100% of observed VMS frames". THAT
 *   CLAIM IS FALSE IN BOTH HALVES,
 *   REFUTED-QUOTE-END
 *
 *   ...and both halves are now measured, not asserted -- censuses B and C of
 *   scs_reason_measure.py:
 *
 *   (B) THE SLOT IS NOT DEAD ACROSS THE ENVELOPE. The SAME population rule
 *       applied to the whole connection-control envelope shows payload [58:60]
 *       in live use by neighbouring message types -- including one that shares
 *       the IDENTICAL 62-byte layout with REJECT_REQ and DISCONNECT_REQ. Each
 *       line is `len type name frames pcaps nonzero-at-[58:60]`:
 *
 *     CENSUS-B len=62  type=3  name=ACCEPT_RSP      frames=258  pcaps=33 nonzero58=62
 *     CENSUS-B len=62  type=4  name=REJECT_REQ      frames=453  pcaps=19 nonzero58=0
 *     CENSUS-B len=62  type=6  name=DISCONNECT_REQ  frames=220  pcaps=25 nonzero58=0
 *     CENSUS-B len=66  type=1  name=CONNECT_RSP     frames=778  pcaps=26 nonzero58=0
 *     CENSUS-B len=110 type=0  name=CONNECT_REQ     frames=1101 pcaps=35 nonzero58=809
 *     CENSUS-B len=110 type=2  name=ACCEPT_REQ      frames=324  pcaps=25 nonzero58=101
 *     CENSUS-B len=110 type=10 name=APPLICATION     frames=2889 pcaps=39 nonzero58=2889
 *
 *       ACCEPT_RSP (type=3) is the one to look at: same 62-byte layout as the
 *       two carriers, and it DOES set the slot.
 *
 *   (C) IT IS NOT THE ONLY ALWAYS-ZERO SLOT EITHER. The 16-bit-aligned payload
 *       slots that are 0x0000 in 100% of the frames of that type:
 *
 *     CENSUS-C type=4 zero_slots=28,32,36,48,58
 *     CENSUS-C type=6 zero_slots=28,32,36,48,58
 *     CENSUS-C common_at_or_after_payload=50 zero_slots=58
 *
 *   WHAT SURVIVES, and it is all the placement needs -- two narrower claims,
 *   each of which is exactly one of the CENSUS lines above:
 *
 *     1. CENSUS-A says [58:60] is 0x0000 in 100% of the two frames that CARRY
 *        the reason code. The neighbours' use of the word is irrelevant to a
 *        REJECT_REQ or DISCONNECT_REQ builder, because the field is
 *        per-message-type and those two types are precisely the ones that
 *        leave it zero. An OVMX frame carrying SCS_REASON_NONE there is
 *        therefore byte-identical to what VMS emits, and only a
 *        deliberately-set nonzero code makes OVMX differ.
 *     2. The last CENSUS-C line says it: of the always-zero slots, [58:60] is
 *        the ONLY one at or after payload 50 -- the only one outside the SCS
 *        sequenced-message counter region [18:50], whose low halves
 *        demonstrably vary (the other always-zero slots are that region's high
 *        halves, and would collide with a counter the moment one wraps). It
 *        also sits immediately after the Con.ID pair, which is where p. 2-26
 *        puts the reason code relative to the identification of the connection.
 *
 *   That is a placement argument, NOT a decoding. Nothing above says VMS puts
 *   its reason code at [58:60]; it says nothing we hold contradicts OVMX doing
 *   so, and names exactly what would.
 *
 *   ==> PLACEMENT AT payload [58:60] IS A **LABELED OVMX DESIGN CHOICE**, not a
 *       decoded VMS field. Registered in docs/cluster-protocol-spec.md sec 5.
 *       If a real node is ever observed setting a nonzero reason code, that
 *       observation OVERRIDES this file.
 *
 *   ==> THE CODE VALUES BELOW ARE ALSO A LABELED OVMX DESIGN CHOICE. The
 *       chapter publishes no VMS reason-code namespace and we have never
 *       observed one. SCS_REASON_NONE = 0 is the only value with any external
 *       support at all (it is what every observed frame and every SDA CDT
 *       shows), and it is the DEFAULT: OVMX puts a nonzero code on the wire
 *       only when a caller explicitly asks for one.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS LIVE AND WHAT IS NOT (read before believing anything else)
 * ---------------------------------------------------------------------------
 *
 * RECEIVE SIDE -- LIVE. src/vmsscs/scsd.c decodes the field out of every
 * REJECT_REQ / DISCONNECT_REQ addressed to one of OUR Con.IDs and logs it
 * (SCSD-I-CONNREASON), giving the peer's SDA "Rej/Disconn Reason" a counterpart
 * on our side. Counted in the exit summary. Covered by the four test_reason_*
 * cases in tests/vmsscs/test_scsd_wire.c, which drive scsd_handle_frame() with
 * an UNEDITED real SCS$DIRECTORY dialogue from ovmx-760-MEMBER-achieved ending
 * in a real DISCONNECT_REQ addressed to OVMX's own Con.ID.
 *
 * SEND SIDE -- NO PRODUCTION CALLER. scs_reason_put() is a tested codec with
 * no caller in the shipped daemon, because OVMX has no REJECT_REQ or
 * DISCONNECT_REQ builder at all (see scs_svc.h: both services report their
 * frame unemitted). `struct scs_svc_args.reason` already carries the SYSAP's
 * value into the emit callback, so whoever adds that builder need only call
 * scs_reason_put() on the frame it assembles. Nothing here claims OVMX
 * currently transmits a reason code, because it currently transmits neither
 * frame. Its only caller today is tests/vmsscs/test_scs_reason.c.
 *
 * KILL SWITCH: OVMX_NO_REASON_CODE=1 suppresses BOTH halves -- put() writes no
 * byte and get() reports suppressed (so scsd.c logs nothing). Re-read from the
 * environment on every call, so a test can bracket one call with setenv.
 */
#ifndef OVMX_SCS_REASON_H
#define OVMX_SCS_REASON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The two SCA connection-control message types that carry the field (p. 2-26).
 * The type itself is at payload [46:48] and is GROUNDED -- spec sec 4(h)(1a). */
#define SCS_REASON_MSGTYPE_REJECT_REQ     4u
#define SCS_REASON_MSGTYPE_DISCONNECT_REQ 6u

/* LABELED OVMX DESIGN CHOICE (see the header comment): payload-relative
 * [58:60], i.e. absolute frame [72:74] since the SCA payload starts at 14. */
#define SCS_REASON_PAYLOAD_OFF 58u
#define SCS_REASON_FRAME_OFF   (14u + SCS_REASON_PAYLOAD_OFF)

/* Absolute length of the 62-byte-SCA connection-control frame class. */
#define SCS_REASON_CONNCTL_FRAME_LEN (14u + 62u)

/*
 * The OVMX reason-code namespace. LABELED OVMX DESIGN CHOICE -- no VMS value is
 * published in ch. 2 and none has ever been observed on our wire. Only
 * SCS_REASON_NONE is externally supported (every observed frame; every SDA
 * "Rej/Disconn Reason" field).
 */
enum scs_reason_code {
    SCS_REASON_NONE              = 0,  /* no reason given -- what VMS sends */
    SCS_REASON_NO_SUCH_SYSAP     = 1,  /* target SYSAP name unknown here */
    SCS_REASON_NOT_LISTENING     = 2,  /* known name, LISTEN not invoked (p. 2-22) */
    SCS_REASON_NO_RESOURCES      = 3,  /* no CDT / no credit / pool exhausted */
    SCS_REASON_CONNECT_DATA      = 4,  /* connect data refused (p. 2-25 version check) */
    SCS_REASON_SYSAP_SHUTDOWN    = 5,  /* local SYSAP is going away */
    SCS_REASON_VC_LOST           = 6,  /* circuit loss closed the connection (p. 2-31) */
    SCS_REASON_PEER_DISCONNECT   = 7   /* symmetric answer to a peer DISCONNECT_REQ */
};

/* Human-readable name for a code. Unknown values -- including any a real VMS
 * node might one day send -- read as "UNKNOWN", never as a guess. */
const char *scs_reason_name(uint16_t reason);

/* The kill switch. 1 = the reason-code field is live, 0 = OVMX_NO_REASON_CODE
 * is set. Re-read from the environment on every call. */
int scs_reason_enabled(void);

/* 1 if `msgtype` (the payload [46:48] value) is one of the two frames that
 * carry a reason code, 0 otherwise. */
int scs_reason_carried_by(unsigned msgtype);

/*
 * Stamp `reason` into an assembled connection-control frame, little-endian, at
 * SCS_REASON_FRAME_OFF. `frame`/`len` are the WHOLE Ethernet frame.
 *
 *   1  written
 *   0  suppressed by OVMX_NO_REASON_CODE -- not one byte was touched
 *  -1  NULL frame, frame too short, or `msgtype` does not carry the field
 */
int scs_reason_put(uint8_t *frame, size_t len, unsigned msgtype, uint16_t reason);

/*
 * Decode the reason code out of a received connection-control frame.
 *
 *   1  decoded into *out
 *   0  suppressed by OVMX_NO_REASON_CODE -- *out untouched
 *  -1  NULL argument, frame too short, or `msgtype` does not carry the field
 */
int scs_reason_get(const uint8_t *frame, size_t len, unsigned msgtype, uint16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_SCS_REASON_H */
