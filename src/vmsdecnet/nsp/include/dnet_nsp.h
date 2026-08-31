/*
 * dnet_nsp.h - DECnet Phase IV NSP (Network Services Protocol) transport codec
 *              (rung 2 of the DECnet lane, rd vms-6986 / epic vms-30e).
 *
 * Encodes and decodes the NSP transport-layer messages that ride on top of the
 * Phase IV routing layer (rung 1, src/vmsdecnet/routing/dnet_hello.*): the
 * logical-link establishment, data, acknowledgement and teardown PDUs that
 * carry a DECnet task-to-task connection (SET HOST, FAL, task-to-task $QIO).
 *
 * Like the rung-1 HELLO codec this is a pure byte-layout library: no socket, no
 * allocation, and no kernel/libc dependency beyond memcpy/memset. That keeps it
 * engine-agnostic -- it links equally into the forward-ported in-kernel
 * net/decnet validation harness or the design sec-4b userspace NSP/AF_PACKET
 * fallback engine (docs/design-decnet-ovmx.md sec 4: "the L3-L6 VMS surface is
 * identical either way -- only the engine boundary moves"). It sits at the NSP
 * sublayer named in design sec 4 ("2. NSP + routing ... logical links, flow
 * control").
 *
 * LAYER BOUNDARY (important). This codec owns the NSP transport PDU ONLY -- from
 * the MSGFLG byte onward. It does NOT own the Phase IV routing header (the
 * long/short data-packet header with the D-ID/S-ID Ethernet addresses) that
 * wraps the NSP PDU on the wire, nor the 2-byte data-link length prefix; those
 * belong to the routing/datalink rungs (dnet_hello.* is that family's first
 * codec). A caller decodes the routing header, then hands the NSP PDU slice to
 * dnet_nsp_decode. This matches the design's layering (NSP above routing).
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md Rule 8, HARD; docs/decnet-provenance-
 * register.md). The byte layout below is derived ONLY from:
 *   (a) the public DEC Digital Network Architecture (DNA) Phase IV NSP
 *       Functional Specification (message formats, MSGFLG classification,
 *       little-endian DSTADDR/SRCADDR logical-link addresses, the Connect
 *       Initiate SERVICES/INFO/SEGSIZE fields), and
 *   (b) the lab-oracle wire specimen captured under rd vms-3be (PR #665) and
 *       committed as a hex dump in docs/decnet-provenance-register.md sec 4.6
 *       ("Hex dump, specimen #3 (NSP Connect Initiate)"): a 53-byte routing
 *       frame from lab node VAX1 (OpenVMS VAX V7.3, aa:00:04:00:01:04, node 1.1)
 *       to VAX2 (aa:00:04:00:02:04, node 1.2), tcpdump-decoded as
 *       "1.1 > 1.2 51 conn-initiate 8193>0 ver 4.1 segsize 1459", whose NSP PDU
 *       (frame offset 0x18) is the ground-truth Connect Initiate this codec
 *       round-trips byte-identical (see the field map in dnet_nsp.c).
 * No VSI/HPE/DEC source or binary was disassembled, decompiled, or copied.
 *
 * ORACLE COVERAGE. The committed register carries exactly ONE NSP specimen, a
 * Connect Initiate. The capture never completed the handshake (VAX2's DECnet
 * permanent database was unconfigured in the golden image -- register sec 4.6),
 * so no Connect Confirm / Data / Data-Ack / Disconnect was observed. Therefore:
 *   - Connect Initiate     -> ORACLE-VERIFIED (byte-identical to specimen #3).
 *   - Connect Confirm, Data segment, Data Acknowledgement, Disconnect Initiate
 *                          -> SPEC-DERIVED, NOT oracle-verified: implemented
 *                             from the public DNA Phase IV NSP spec and proven
 *                             only by codec self round-trip (encode->decode->
 *                             encode). No specimen bytes are fabricated for them.
 *
 * On-wire NSP PDU layouts (little-endian scalars). "opaque data" is the
 * higher-layer (Session Control) payload, which NSP carries but does not parse.
 *
 *   Connect Initiate (MSGFLG 0x18) / Connect Confirm (0x28):
 *     off  size  field
 *      0    1    MSGFLG      0x18 CI / 0x28 CC
 *      1    2    DSTADDR LE  destination logical-link address (0 in a fresh CI)
 *      3    2    SRCADDR LE  source logical-link address
 *      5    1    SERVICES    flow-control option byte
 *      6    1    INFO        NSP version (low 2 bits: 0=3.2 1=3.1 2=4.0 3=4.1)
 *      7    2    SEGSIZE LE  requested max segment size
 *      9   var   DATA        connect data / session-control payload (opaque)
 *
 *   Data segment (MSGFLG class 0x00; e.g. 0x60 = BOM+EOM single segment):
 *      0    1    MSGFLG      data flags (bit5 0x20 BOM, bit6 0x40 EOM)
 *      1    2    DSTADDR LE
 *      3    2    SRCADDR LE
 *     [2]        ACKNUM  LE  OPTIONAL piggyback ack, present iff bit15 (0x8000)
 *                            of the field is set (NSP QUAL bit)
 *      -    2    SEGNUM  LE  segment number (low 12 bits) + BOM/EOM/DLY flags
 *      -   var   DATA        segment data (opaque)
 *
 *   Data Acknowledgement (MSGFLG class 0x04; 0x04 data-ack):
 *      0    1    MSGFLG      0x04
 *      1    2    DSTADDR LE
 *      3    2    SRCADDR LE
 *      5    2    ACKNUM  LE  acknowledgement field (bit15 QUAL, bit12 NAK)
 *     [2]        ACKOTH  LE  OPTIONAL cross-subchannel (other-data) ack
 *
 *   Disconnect Initiate (MSGFLG 0x38) / Disconnect Confirm (MSGFLG 0x48):
 *      0    1    MSGFLG      0x38 DI / 0x48 DC
 *      1    2    DSTADDR LE
 *      3    2    SRCADDR LE
 *      5    2    REASON  LE  disconnect reason code (DC: 42 = disconnect complete)
 *      7   var   DATA        disconnect data / session-control (opaque; DC: none)
 *
 * Disconnect Confirm (DC) is the responder's terminal acknowledgement of a
 * Disconnect Initiate: the DI sender waits for it before the logical link is
 * fully closed (DNA Phase IV NSP disconnect handshake). SPEC-DERIVED, NOT
 * oracle-verified -- the vms-3be capture never completed a link so no DC was
 * observed; its layout mirrors DI (REASON + optional data) per the public NSP
 * spec, proven only by codec self round-trip. No specimen bytes are fabricated.
 */
#ifndef DNET_NSP_H
#define DNET_NSP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MSGFLG classification (public DNA Phase IV NSP spec). The two bits <3:2>
 * select the top-level class; within the control class the high bits <6:4>
 * select the specific control message.
 */
#define DNET_NSP_CLASS_MASK     0x0c
#define DNET_NSP_CLASS_DATA     0x00    /* data segment */
#define DNET_NSP_CLASS_ACK      0x04    /* acknowledgement */
#define DNET_NSP_CLASS_CTL      0x08    /* control (CI/CC/DI/DC/...) */

#define DNET_NSP_CTL_SUBMASK    0x70    /* control subtype selector (bits 6:4) */

/* Concrete MSGFLG values used by this codec. */
#define DNET_NSP_MSGFLG_DATA    0x60    /* data segment, BOM+EOM (single segment) */
#define DNET_NSP_MSGFLG_ACK     0x04    /* data acknowledgement */
#define DNET_NSP_MSGFLG_CI      0x18    /* connect initiate  (oracle-verified) */
#define DNET_NSP_MSGFLG_CC      0x28    /* connect confirm   (spec-derived) */
#define DNET_NSP_MSGFLG_DI      0x38    /* disconnect initiate (spec-derived) */
#define DNET_NSP_MSGFLG_DC      0x48    /* disconnect confirm  (spec-derived) */

/* A standard NSP disconnect-complete reason code (DNA Phase IV NSP), carried in
 * the DC REASON field when a Disconnect Initiate is confirmed normally. */
#define DNET_NSP_REASON_DISC_COMPLETE  42

/* Field-level bits. */
#define DNET_NSP_DATA_BOM       0x20    /* MSGFLG: beginning of message */
#define DNET_NSP_DATA_EOM       0x40    /* MSGFLG: end of message */
#define DNET_NSP_ACK_QUAL       0x8000  /* ACK field present / valid (bit 15) */

/* INFO byte NSP version encoding (low 2 bits). */
#define DNET_NSP_VER_32         0
#define DNET_NSP_VER_31         1
#define DNET_NSP_VER_40         2
#define DNET_NSP_VER_41         3

/* Decoded message discriminator. */
enum dnet_nsp_type {
    DNET_NSP_T_DATA = 1,    /* data segment */
    DNET_NSP_T_ACK,         /* data acknowledgement */
    DNET_NSP_T_CI,          /* connect initiate */
    DNET_NSP_T_CC,          /* connect confirm */
    DNET_NSP_T_DI,          /* disconnect initiate */
    DNET_NSP_T_DC           /* disconnect confirm */
};

/* Generous cap on the opaque higher-layer payload an NSP PDU can carry. */
#define DNET_NSP_MAX_DATA       1024

/* Decoded / to-be-encoded NSP message (a flat tagged record). Only the fields
 * relevant to `type` are meaningful; the rest are zero after a decode. */
struct dnet_nsp_msg {
    uint8_t  type;          /* enum dnet_nsp_type */
    uint8_t  msgflg;        /* raw MSGFLG byte (preserved for exact round-trip) */
    uint16_t dstaddr;       /* destination logical-link address (LE) */
    uint16_t srcaddr;       /* source logical-link address (LE) */

    /* Connect Initiate / Connect Confirm */
    uint8_t  services;      /* SERVICES byte (flow-control option) */
    uint8_t  info;          /* INFO byte (NSP version in low 2 bits) */
    uint16_t segsize;       /* requested segment size */

    /* Data segment */
    uint16_t segnum;        /* SEGNUM field (segment no. + BOM/EOM/DLY flags) */

    /* Piggyback / acknowledgement fields */
    uint8_t  has_acknum;    /* data segment or data-ack: ACKNUM present */
    uint16_t acknum;        /* ACKNUM field (raw, incl QUAL/NAK bits) */
    uint8_t  has_ackoth;    /* data-ack: second (other-data) ack present */
    uint16_t ackoth;        /* cross-subchannel other-data ack (raw) */

    /* Disconnect Initiate */
    uint16_t reason;        /* disconnect reason code */

    /* Opaque higher-layer payload: CI/CC connect data, DI disconnect data, or
     * DATA-segment data. */
    uint16_t datalen;
    uint8_t  data[DNET_NSP_MAX_DATA];
};

/* Return codes (mirror the rung-1 dnet_hello codec's convention). */
#define DNET_NSP_OK             0
#define DNET_NSP_ETRUNC       (-1)      /* input buffer too short for the PDU */
#define DNET_NSP_EBADLEN      (-2)      /* declared/derived length inconsistent */
#define DNET_NSP_ENOSPACE     (-3)      /* output buffer too small */
#define DNET_NSP_EINVAL       (-4)      /* null argument or bad struct field */
#define DNET_NSP_EBADTYPE     (-5)      /* unrecognised / unsupported MSGFLG */

/*
 * Decode a single NSP transport PDU from `buf` (which must start at the MSGFLG
 * byte -- the caller has already stripped the routing header and length
 * prefix). On success fills *out and returns DNET_NSP_OK; the number of bytes
 * the PDU occupied is written to *consumed when non-NULL. Returns a negative
 * DNET_NSP_E* on malformed input. Never reads past buf[len-1].
 */
int dnet_nsp_decode(const uint8_t *buf, size_t len,
                    struct dnet_nsp_msg *out, size_t *consumed);

/*
 * Encode `msg` into `buf` as an NSP transport PDU (MSGFLG onward -- no routing
 * header, no length prefix). Writes the byte count to *outlen when non-NULL.
 * Returns DNET_NSP_OK, DNET_NSP_ENOSPACE if `cap` is too small, or
 * DNET_NSP_EINVAL/EBADTYPE/EBADLEN on a malformed message.
 */
int dnet_nsp_encode(const struct dnet_nsp_msg *msg,
                    uint8_t *buf, size_t cap, size_t *outlen);

/* NSP version carried in the INFO byte (low 2 bits); one of DNET_NSP_VER_*. */
static inline unsigned dnet_nsp_version(const struct dnet_nsp_msg *m)
{
    return m->info & 0x03u;
}

#ifdef __cplusplus
}
#endif

#endif /* DNET_NSP_H */
