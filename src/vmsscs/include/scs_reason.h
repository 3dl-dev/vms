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
 * reproducible with tools/cluster/scs_reason_measure.py:
 *
 *   Over ALL 47 pcaps in the lab capture set, every SCA frame whose total SCA
 *   length is 62 (the connection-control class, spec sec 4(h)(1a)) and whose
 *   message type at payload [46:48] is 4 (REJECT_REQ) or 6 (DISCONNECT_REQ),
 *   restricted to VMS-origin source MACs (DEC OUI 08-00-2b or the LAVC logical
 *   aa-00-04-00-xx-04), so that no OVMX-emitted frame can be mistaken for a
 *   VMS one:
 *
 *     REJECT_REQ     453 frames across 19 pcaps
 *     DISCONNECT_REQ 220 frames across 25 pcaps
 *
 *   Per-offset value census over the whole 62-byte payload of those frames:
 *   the only bytes that vary at all are the sequence/ack fields and the Con.ID
 *   pair at payload [50:58]. The four bytes AFTER the Con.ID pair read:
 *
 *     payload [58:60]  0x0000 in 453/453 REJECT_REQ and 220/220 DISCONNECT_REQ
 *     payload [60:62]  0x0001 in 453/453 REJECT_REQ;
 *                      0x0000 (131) / 0x0001 (89) in DISCONNECT_REQ
 *
 *   THE SECOND ORACLE AGREES. SDA `SHOW CONNECTIONS` prints a per-CDT field
 *   literally named "Rej/Disconn Reason"
 *   (captures/sda-scs-extract-vax1.txt): it reads **0 on all 12 CDTs**. So the
 *   field is real and named by the VMS oracle, and both the wire and SDA say
 *   every reason code our lab ever produced was ZERO.
 *
 *   CONCLUSION, stated exactly: **the offset is NOT GROUNDED and cannot be,
 *   from the data we hold.** No VMS node in 673 observed REJECT/DISCONNECT
 *   frames ever set a nonzero reason code, so there is no varying field to
 *   localize. What the measurement DOES buy is a safe placement: payload
 *   [58:60] is the only 16-bit slot in either frame that is zero in 100% of
 *   observed VMS frames, so an OVMX frame carrying SCS_REASON_NONE there is
 *   byte-identical to what VMS emits, and only a deliberately-set nonzero code
 *   makes OVMX differ. Do NOT use payload [60:62]: it is an observed CONSTANT
 *   0x0001 on REJECT_REQ, i.e. a field with a meaning we have not decoded.
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
