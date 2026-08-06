/*
 * scs_mscp.h - MSCP-over-SCS disk-client command builders (vms-760).
 *
 * THE GAP THIS CLOSES. Reaching SHOW CLUSTER status NEW as a pure server is not
 * enough for an ESTABLISHED VAX1 to promote OVMX to MEMBER: a real joiner is
 * ALSO a disk CLIENT. After NEW it opens its OWN SCS$DIRECTORY + MSCP$DISK
 * connections BACK to VAX1 and runs MSCP disk discovery -- SET CONTROLLER
 * CHARACTERISTICS then a GET UNIT STATUS enumeration -- proving it can reach the
 * cluster system disk. VAX1 will not admit a member that cannot. This module
 * builds the two MSCP command frames OVMX (the disk client) must SEND on its own
 * MSCP$DISK SCS connection.
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md Rule 8). MSCP (Mass Storage Control Protocol)
 * is a PUBLICLY documented DEC protocol -- opcodes (0x03 GET UNIT STATUS, 0x04
 * SET CONTROLLER CHARACTERISTICS; response = command | 0x80), status majors
 * (0=SUCCESS, 3=UNIT OFFLINE, 4=UNIT AVAILABLE) and the command/response message
 * layout are taken from public MSCP documentation. Every template below is a
 * byte-exact SCA-content capture of the golden JOINER command frames from the
 * reference-lab wire (af2-firsttimer-established-20260728.pcap, the clean-room
 * observation of a first-timer joining an established VAX1). Only GROUNDED fields
 * are substituted (identity MACs / cluster-logical addrs, the SCS envelope
 * counters, the Con.ID pair, and the MSCP command-reference-number / unit word).
 * The MSCP parameter regions (host-time/id quadword, controller-flags) are
 * REPLAYED from the captured frame, never synthesized. No VSI/HPE source or
 * binary was read, disassembled, or copied.
 *
 * THE SCS ENVELOPE is identical to the other 0x4b sequenced-application classes
 * (spec sec 4h): msgtype 0x4b / format 0x13 at [16:18], recv_ack [18:20] mirrored
 * at [26:28]/[34:36], send_seq [20:22] mirrored at [30:32], incarnation echo
 * [22:24], the Con.ID pair at [50:54]/[54:58], and the MSCP body starting at SCA
 * offset 58 (abs frame offset 72). The MSCP command rides that body:
 *
 *   body[0:2]  class token / credits (SCC 0x0002, GUS 0x0001)  \ together the
 *   body[2:4]  message-id  (increments per command; ECHOED back) / 4-byte MSCP
 *                                                                   Command Reference
 *                                                                   Number, an opaque
 *                                                                   correlation token
 *   body[4:6]  unit number word (GUS carries the enumeration unit; SCC 0x0000)
 *   body[6:8]  reserved (0x0000)
 *   body[8]    MSCP opcode (0x04 SCC, 0x03 GUS; END response = opcode | 0x80)
 *   body[9]    flags (0x00)
 *   body[10:12] modifiers (SCC 0x0000; GUS 0x0001 = NEXT-UNIT, drives enumeration)
 *   body[12:36] opcode-specific parameter region (REPLAYED template)
 */
#ifndef SCS_MSCP_H
#define SCS_MSCP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MSCP-over-SCS disk-client command frame: 94 SCA-content bytes + 14-byte
 * Ethernet header (a 108-byte wire frame). */
#define SCS_MSCP_SCA_LEN    94
#define SCS_MSCP_FRAME_LEN  108

/* SCA-content offset of the MSCP body (body[0] = SCA offset 58 = abs 72). */
#define SCS_MSCP_BODY_OFF   58

/* SCS envelope constants (shared with the other 0x4b classes).
 *
 * ⚠ NAMING TRAP, and it is the reason this comment exists: SCS_MSCP_MSGTYPE is
 * the PPD/NISCA marker byte at content [16], NOT the SCS message type. The SCS
 * MTYPE is the LE u16 at content [46:48] and it is 10 on this class -- the
 * p. 4-13 APPLICATION MESSAGE (docs/design-mscp-direction.md sec 1.2, which
 * identifies these very golden frames as MTYPE 10 with credit 1). Since vms-ec7
 * the builder writes it through scs_env_build_frame() as
 * SCS_ENV_MTYPE_APP_MESSAGE; do not add a second spelling of it here. */
#define SCS_MSCP_MSGTYPE    0x4b
#define SCS_MSCP_FORMAT     0x13

/* The credit field ([48:50]) of the golden af2 joiner MSCP commands. A labeled
 * REPLAY, not a computed extension -- see the note at the build site. */
#define SCS_MSCP_ENV_CREDIT 1u

/* MSCP opcodes (body[8]) and the END-response bit (public MSCP). */
#define SCS_MSCP_OP_GET_UNIT_STATUS 0x03
#define SCS_MSCP_OP_SET_CTLR_CHAR   0x04
#define SCS_MSCP_END_BIT            0x80 /* END response opcode = command | 0x80 */

/* MSCP END status major codes (body[10:12] low byte; public MSCP). */
#define SCS_MSCP_ST_SUCCESS   0x0000
#define SCS_MSCP_ST_OFFLINE   0x0003 /* end-of-list terminator on the GUS walk */
#define SCS_MSCP_ST_AVAILABLE 0x0004 /* a real disk unit */

/* GUS NEXT-UNIT modifier (body[10:12]) -- set on every GET UNIT STATUS command;
 * drives the enumeration (next unit = returned unit + 1). */
#define SCS_MSCP_MOD_NEXT_UNIT 0x0001

/* GROUNDED seed tokens from af2-firsttimer-established.pcap (the joiner's first
 * SCC / GUS command-reference-number low/high words). Opaque -- VAX1 echoes them
 * verbatim; OVMX increments the message-id per command. */
#define SCS_MSCP_SCC_CLASS   0x0002u
#define SCS_MSCP_GUS_CLASS   0x0001u
#define SCS_MSCP_SCC_MSGID0  0x81a3u
#define SCS_MSCP_GUS_MSGID0  0x7ee2u

/*
 * scs_mscp_params - inputs to build one MSCP disk-client command frame.
 */
struct scs_mscp_params {
    uint8_t  dst_mac[6];      /* Ethernet dst = member's Ethernet src MAC */
    uint8_t  src_mac[6];      /* Ethernet src (abs 6) = OVMX HW MAC */
    uint8_t  src_logical[6];  /* SCA src-logical [10:16] (abs 24) = aa:00:04:00:<LE16(sysid)> */
    uint8_t  peer_logical[6]; /* SCA dest-logical [2:8] = member's advertised logical addr */
    uint32_t remote_conid;    /* [50:54] member's MSCP$DISK server Con.ID */
    uint32_t local_conid;     /* [54:58] OVMX's MSCP$DISK client Con.ID */
    uint16_t recv_ack;        /* SCS recv_seq (envelope [18:20]/[26:28]/[34:36]) */
    uint16_t send_seq;        /* SCS send_seq (envelope [20:22]/[30:32]) */
    uint16_t incarnation;     /* envelope [22:24]; 0 => leave template's fresh 1 */
    uint16_t class_token;     /* MSCP body[0:2] (SCC 0x0002 / GUS 0x0001) */
    uint16_t msg_id;          /* MSCP body[2:4] (increments per command; echoed) */
    uint16_t unit;            /* MSCP body[4:6] (GUS enumeration unit; 0 for SCC) */
};

/*
 * scs_mscp_build_scc - build the SET CONTROLLER CHARACTERISTICS command
 * (opcode 0x04) OVMX sends as the FIRST MSCP message on its disk-client
 * connection. scs_mscp_build_gus - build a GET UNIT STATUS command (opcode 0x03,
 * NEXT-UNIT modifier) for p->unit. Each fills out[SCS_MSCP_FRAME_LEN] with a
 * complete Ethernet+SCA frame byte-exact to the golden joiner command modulo the
 * substituted GROUNDED fields. Returns 0, or -1 on NULL args.
 */
int scs_mscp_build_scc(const struct scs_mscp_params *p,
                       uint8_t out[SCS_MSCP_FRAME_LEN]);
int scs_mscp_build_gus(const struct scs_mscp_params *p,
                       uint8_t out[SCS_MSCP_FRAME_LEN]);

/*
 * scs_mscp_view - decoded view of a received MSCP-over-SCS frame (command or
 * END response). The caller matches the connection by the Con.ID pair.
 */
struct scs_mscp_view {
    uint16_t total_sca_len;
    /* vms-ec7: the SCS envelope's own two value fields, decoded through the
     * shared path. scs_mtype is the [46:48] SCS message type (10 = the p. 4-13
     * application message on every MSCP frame we hold); credit is [48:50]. */
    uint16_t scs_mtype;      /* [46:48] */
    uint16_t credit;         /* [48:50] */
    uint8_t  msgtype;        /* [16] -- the PPD marker byte, NOT the SCS MTYPE */
    uint8_t  format;         /* [17] */
    uint16_t recv_ack;       /* [18:20] */
    uint16_t send_seq;       /* [20:22] */
    uint32_t remote_conid;   /* [50:54] */
    uint32_t local_conid;    /* [54:58] */
    uint16_t class_token;    /* body[0:2] */
    uint16_t msg_id;         /* body[2:4] */
    uint16_t unit;           /* body[4:6] (END: the returned unit-word) */
    uint8_t  opcode;         /* body[8] (END = command | 0x80) */
    uint16_t status;         /* body[10:12] (END: MSCP status major in low byte) */
    int      is_end;         /* opcode & 0x80 */
};

/*
 * scs_mscp_parse - classify a received frame as an MSCP-over-SCS message and
 * fill *v. Returns 0 on a well-formed frame that holds the MSCP body (>= 84
 * bytes: abs 72 body + status word), -1 otherwise. Does NOT check the Con.ID
 * pair -- the caller matches the connection.
 */
int scs_mscp_parse(const uint8_t *frame, size_t len, struct scs_mscp_view *v);

#ifdef __cplusplus
}
#endif

#endif /* SCS_MSCP_H */
