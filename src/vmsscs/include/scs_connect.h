/*
 * scs_connect.h - Build/parse the VMS$VAXcluster SCS connect handshake
 * (vms-5fe), spec sec 4(g) phase 4 (opcode 0x4b, 110-byte SCA class).
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md rule 8 / spec sec 0): the 110-byte SCA
 * body templates baked in here are the byte-exact contents of two real
 * connect frames captured on the reference lab wire and dumped verbatim:
 *   - CONNECT-REQUEST  = formation-ci1-joinwindow.pcap raw frame 47
 *                        (SCA#39, VAX1->VAX2: remote Con.ID 0, local
 *                        0x62C50009)
 *   - CONNECT-RESPONSE = formation-ci1-joinwindow.pcap raw frame 50
 *                        (SCA#42, VAX2->VAX1: remote 0x62C50009 echoed,
 *                        local 0x33580008)
 * No VSI/HPE source or binary was read.
 *
 * GROUNDED fields (spec sec 4d/4g) that this module positions/substitutes:
 *   - abs 30 SCS message-type byte 0x4b, abs 31 format constant 0x13
 *   - abs 64 Remote Con.ID (LE u32), abs 68 Local Con.ID (LE u32)
 *   - abs 76 / abs 92 the two ASCII "VMS$VAXcluster  " SYSAP endpoint names
 *
 * LIVE-THREADED counters (vms-c6d). The SCS sequenced-message counters are NO
 * LONGER replayed: build_from_tmpl substitutes OVMX's LIVE VC send_seq/recv_ack
 * from struct scs_connect_params (recv_ack at [18:20]/[26:28]/[34:36], send_seq
 * at [20:22] mirrored [30:32], node-incarnation echo at [22:24]) -- the same
 * GROUNDED offsets and mechanism as the 0x5b directory exchange (scs_dir.c,
 * spec sec 4h(4)). Baking the golden frame's captured 7/8 counters made the VAX
 * reject OVMX's CONNECT-RESPONSE (its live VC had advanced past the directory
 * phase to a different send_seq) and retransmit the 0x4b forever; threading the
 * live counters is what lets the VAX accept the accept.
 *
 * REPLAYED (ungrounded, spec sec 4g/sec 5) fields still left at their captured
 * values: the connect-state / inner-length body bytes (abs 56-61 and abs
 * 108-123) and the [42:56] region beyond the counters. These are tied to the
 * live channel's message flow and cannot be grounded from passive capture;
 * replaying a real frame's values is the documented lab-shortcut posture
 * (mirrors the nonce replay), NOT a general connect-body implementation. A
 * veracity adversary should treat every non-substituted byte here as a labeled
 * replay.
 *
 * OVMX DESIGN CHOICE (not VMS-authentic, labeled per rule 8): OVMX allocates
 * its own Local Con.ID (SCS_CONNECT_OVMX_CONID_BASE | index). The value is
 * opaque to the peer (it only echoes it back), so any non-colliding 32-bit
 * value is protocol-valid; OVMX uses a recognizable base so it stands out
 * in captures.
 */
#ifndef SCS_CONNECT_H
#define SCS_CONNECT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCS_CONNECT_SCA_LEN   110 /* total SCA content bytes (GROUNDED length class) */
#define SCS_CONNECT_FRAME_LEN 124 /* 14-byte Ethernet header + SCS_CONNECT_SCA_LEN */

#define SCS_MSGTYPE_START     0x41 /* START/config (spec sec 4g phase 2) */
#define SCS_MSGTYPE_DIRLOOKUP 0x5b /* directory lookup (phase 3) */
#define SCS_MSGTYPE_SEQAPP    0x4b /* sequenced-application: connect / VC / DLM data (phase 4) */
#define SCS_MSGTYPE_CREDIT    0x48 /* credit-return short (phase 5) */
#define SCS_FORMAT_CONST      0x13 /* format/version constant (GROUNDED, spec sec 4g) */

/* Recognizable OVMX Con.ID base ("OX" | connection index). OVMX design
 * choice -- opaque to the peer (see header note). */
#define SCS_CONNECT_OVMX_CONID_BASE 0x4F580000u

struct scs_connect_params {
    uint8_t  dst_mac[6];      /* Ethernet dst = peer's observed src MAC */
    uint8_t  src_mac[6];      /* Ethernet src (abs 6) = OVMX HW MAC */
    uint8_t  src_logical[6];  /* SCA src-logical addr [10:16] (abs 24) = aa:00:04:00:<LE16(sysid)>;
                                 the cluster-LOGICAL addr, NOT the raw HW MAC (vms-9f3) */
    uint8_t  peer_logical[6]; /* SCA dest logical addr (abs 16) = peer's advertised logical addr
                                 (its HELLO src-logical field); for a non-DECnet peer this is the
                                 same as dst_mac */
    uint32_t local_conid;     /* OVMX's own Con.ID (goes in the Local Con.ID field) */
    uint32_t remote_conid;    /* peer's Con.ID: 0 for a REQUEST, the peer's own for a RESPONSE */
    /* --- vms-c6d: LIVE SCS VC counters threaded into the 0x4b frame (spec sec
     * 4h(4)); NOT the golden template's replayed values. --- */
    uint16_t recv_ack;        /* [18:20]/[26:28]/[34:36] = OVMX recv_seq (peer's last send_seq) */
    uint16_t send_seq;        /* [20:22] mirrored [30:32] = OVMX's own send_seq for this frame */
    uint16_t incarnation;     /* [22:24] node-incarnation echo (0 => leave the
                               * fresh-golden template value 1; spec sec 4i.B) */
};

/*
 * scs_connect_build_request - Build a CONNECT-REQUEST (remote Con.ID = 0,
 * local Con.ID = p->local_conid). p->remote_conid is ignored (forced 0).
 * Returns 0, or -1 if p or out is NULL.
 */
int scs_connect_build_request(const struct scs_connect_params *p,
                              uint8_t out[SCS_CONNECT_FRAME_LEN]);

/*
 * scs_connect_build_response - Build a CONNECT-RESPONSE/ACCEPT (remote
 * Con.ID = p->remote_conid [the peer's, echoed], local = p->local_conid).
 * Returns 0, or -1 if p or out is NULL.
 */
int scs_connect_build_response(const struct scs_connect_params *p,
                               uint8_t out[SCS_CONNECT_FRAME_LEN]);

/*
 * scs_connect_build_mscp_request - vms-760: Build the JOINER's MSCP$DISK
 * client CONNECT-REQUEST (VMS$DISK_CL_DRVR -> MSCP$DISK), remote Con.ID = 0,
 * local = p->local_conid. Same 110-byte SCA connect class as the
 * VMS$VAXcluster request, but msgtype 0x5b and the MSCP$DISK/VMS$DISK_CL_DRVR
 * SYSAP names + "V5.0" class descriptor. Byte template = the clean 1->2-node
 * formation joiner MSCP$DISK connect (formation-clean-2node.pcap SCA idx35);
 * identity/Con.ID/seq fields are substituted exactly as the VMS$VAXcluster
 * request. The joiner presenting this connection is the GROUNDED predicate the
 * live member requires before it reciprocates the add-member config on the
 * joiner VC (NEW->MEMBER, spec sec 4L(7)). Returns 0, or -1 if p/out is NULL.
 */
int scs_connect_build_mscp_request(const struct scs_connect_params *p,
                                   uint8_t out[SCS_CONNECT_FRAME_LEN]);

/* Read-only view of a received SCS-envelope frame's grounded fields. */
struct scs_connect_view {
    uint16_t total_sca_len;  /* LE u16 at abs 14 + 2 */
    uint8_t  msgtype;        /* abs 30 (0x41/0x5b/0x4b/0x48) */
    uint8_t  format;         /* abs 31 (expect 0x13) */
    uint32_t remote_conid;   /* abs 64 LE u32 (valid only for the 110/190-byte Con.ID classes) */
    uint32_t local_conid;    /* abs 68 LE u32 */
    int      has_conid;      /* 1 if total_sca_len is a Con.ID-bearing class (110 or 190) */
};

/*
 * scs_connect_parse - Parse a received Ethernet+SCA frame's grounded SCS
 * envelope fields into *v. `frame` points at the Ethernet dst (abs 0);
 * `len` is the captured frame length. Fills msgtype/format for any frame
 * with >= abs 32 bytes, and the Con.ID pair when the length class carries
 * one (110 or 190 total SCA bytes, spec sec 4d/4g).
 *
 * Returns 0 on success, -1 if frame/v is NULL or the frame is too short to
 * hold the SCS envelope header.
 */
int scs_connect_parse(const uint8_t *frame, size_t len, struct scs_connect_view *v);

#ifdef __cplusplus
}
#endif

#endif /* SCS_CONNECT_H */
