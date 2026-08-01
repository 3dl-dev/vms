/*
 * scs_dir.h - Build/parse the SCS$DIRECTORY connect + SCS$DIR_LOOKUP
 * name-resolution exchange (vms-246), spec sec 4(h) (opcode 0x5b, and the
 * 0x4b sequenced-application form the lookups switch to once the
 * SCS$DIRECTORY connection is established).
 *
 * WHY THIS EXISTS. After the phase-2 0x41 START completes (vms-21e) and the
 * NISCA VC is sustained by credit-returns (vms-691), the ESTABLISHED node
 * runs a directory exchange BEFORE it will open the VMS$VAXcluster 0x4b
 * connection: it (a) opens an SCS$DIRECTORY SCS connection to the joiner and
 * (b) queries the joiner's directory ("who on your node serves MSCP$TAPE /
 * MSCP$DISK / VMS$VAXcluster?"). OVMX is the joiner, so OVMX must RESPOND:
 * bind the SCS$DIRECTORY Con.ID pair and answer each SYSAP-name lookup so the
 * established node resolves VMS$VAXcluster on OVMX and proceeds to send OVMX
 * the VMS$VAXcluster 0x4b CONNECT-REQUEST (item vms-c6d).
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md rule 8 / spec sec 0). The SCA-content
 * templates baked in here are the byte-exact contents of the real JOINER
 * (VAX2) directory-phase frames captured on our own reference-lab wire and
 * dumped verbatim from formation-ci1-joinwindow.pcap:
 *   - CONNECT-ECHO     = SCA#23 (66-byte 0x5b, VAX2->VAX1: remote 0x63050008
 *                        echoed, local 0 not-yet-assigned, op[46:48]=1)
 *   - CONNECT-RESPONSE = SCA#25 (110-byte 0x5b, VAX2->VAX1: remote 0x63050008,
 *                        local 0x33590007 supplied, op=2, name SCS$DIR_LOOKUP,
 *                        result SCS$DIRECTORY)
 *   - LOOKUP-RESPONSE  = SCA#31 (94-byte 0x5b, MSCP$TAPE -> "NOT PRESENT HERE")
 *                        with the affirmative result descriptor lifted from
 *                        SCA#38 (94-byte 0x4b, VMS$VAXcluster resolved).
 * No VSI/HPE source or binary was read.
 *
 * GROUNDED fields this module positions/substitutes (spec sec 4h, all
 * payload-relative; absolute = 14 + payload offset):
 *   - [16] opcode (0x5b, or 0x4b once the SCS$DIRECTORY connection is up),
 *     [17] format constant 0x13 (GROUNDED 605/605)
 *   - [18:20]/[26:28]/[34:36] acknowledged sequence (OVMX recv_seq),
 *     [20:22]/[30:32] send-seq (OVMX send_seq, mirror GROUNDED)
 *   - [42:44] inner length = payload_len - 44 (GROUNDED)
 *   - [50:54] remote Con.ID (the peer's SCS$DIRECTORY handle, learned),
 *     [54:58] local Con.ID (OVMX's own SCS$DIRECTORY handle) -- SAME offsets
 *     as the sec 4g phase-4 / sec 4d Con.ID pair
 *   - [62:78] queried SYSAP name (16-byte blank-padded field)
 *   - [78:94] 16-byte result field: the literal "NOT PRESENT HERE" on a
 *     negative lookup (GROUNDED, spec sec 4h(2))
 *
 * REPLAYED / observed-not-grounded (labeled per rule 8, left at captured
 * values):
 *   - the [46:48] directory-operation field and [48:50] companion flag
 *     (spec sec 4h RE gap (a): value<->operation mapping inferred). OVMX
 *     echoes the request's op into its response.
 *   - the AFFIRMATIVE lookup result descriptor for VMS$VAXcluster,
 *     [78:94] = 01 1b 01 03 00*10 06 00 (spec sec 4h RE gap (c): "the
 *     affirmative lookup result encoding ... no separate status/handle-return
 *     field was isolated"). This 16-byte value is reproduced BYTE-EXACT as
 *     observed in SCA#38; its internal semantics are NOT grounded. It is a
 *     replay of our own lab wire, honestly labeled -- not a decoded schema.
 *   - the [24:26]=18 (NISCS_LAN_OVRHD), [38:40], [40:42], [44:46] and the
 *     [58:62] request/response marker (00000000 in a request, 00000001 in a
 *     response) -- all reproduced as observed.
 *   - [22:24] node-incarnation (spec sec 4i established-join extension of the
 *     sec 4h directory exchange). The fresh-formation golden carries the
 *     constant 0x0001; on an ESTABLISHED-cluster join the member stamps its
 *     current node-incarnation N here (observed N=3 on the vms-246 lab wire in
 *     VAX1's own 0x5b SCS$DIRECTORY connect-request; same value it advertises
 *     in its directed HELLO [78:80] and stamps in the 0x41 START [22:24], sec
 *     4i.B). OVMX ECHOES the value the member itself put on the wire (never a
 *     self-invented constant) into scs_dir_params.incarnation; a 0 leaves the
 *     template's fresh value 1. GROUNDED-BY-OBSERVATION and confirmed on the
 *     live oracle: with N echoed, VAX1 binds the SCS$DIRECTORY CDT (SDA SHOW
 *     CONNECTIONS: OVMX::SCS$DIRECTORY, Remote Con.ID = OVMX handle) and
 *     advances to the VMS$VAXcluster 0x4b connect
 *     (captures/vms246-scsdir-0x4b-reached-20260728.pcap); without it, VAX1
 *     silently discarded OVMX's op=1/op=2 responses and retransmitted forever.
 *
 * OVMX DESIGN CHOICE (not VMS-authentic, labeled): OVMX allocates its own
 * SCS$DIRECTORY Local Con.ID (SCS_DIR_OVMX_CONID). Opaque to the peer (it only
 * echoes it back), so any non-colliding 32-bit value is protocol-valid.
 */
#ifndef SCS_DIR_H
#define SCS_DIR_H

#include <stddef.h>
#include <stdint.h>

#include "scs_connect.h" /* SCS_MSGTYPE_*, SCS_FORMAT_CONST, SCS_CONNECT_OVMX_CONID_BASE */

#ifdef __cplusplus
extern "C" {
#endif

#define SCS_DIR_OPCODE        0x5b /* SCS$DIRECTORY connect / lookup (spec sec 4h) */
#define SCS_DIR_OPCODE_RETX   0x7b /* its retransmit form (spec sec 4h) */
#define SCS_DIR_OP_LOOKUP     0x0a /* [46:48] operation value seen on every name lookup (inferred) */

/* SCA-content and full-frame lengths of the three joiner templates. */
#define SCS_DIR_ECHO_SCA_LEN     66
#define SCS_DIR_ECHO_FRAME_LEN   80  /* 14 Eth hdr + 66 */
#define SCS_DIR_RESP_SCA_LEN     110
#define SCS_DIR_RESP_FRAME_LEN   124 /* 14 + 110 */
#define SCS_DIR_LOOKUP_SCA_LEN   94
#define SCS_DIR_LOOKUP_FRAME_LEN 108 /* 14 + 94 */

/* Recognizable OVMX SCS$DIRECTORY Con.ID ("OX" base | 7). OVMX design choice,
 * opaque to the peer (see header note). Distinct from OVMX's VMS$VAXcluster
 * handle (SCS_CONNECT_OVMX_CONID_BASE | 1). */
#define SCS_DIR_OVMX_CONID (SCS_CONNECT_OVMX_CONID_BASE | 0x0007u)

/* 16-byte SYSAP-name field width [62:78] and result field width [78:94]. */
#define SCS_DIR_NAME_LEN   16
#define SCS_DIR_RESULT_LEN 16

/* The grounded negative-lookup result marker (spec sec 4h(2)). */
#define SCS_DIR_NOT_PRESENT "NOT PRESENT HERE" /* exactly 16 chars, no NUL on the wire */

/*
 * scs_dir_params - inputs common to the SCS$DIRECTORY connect responses.
 */
struct scs_dir_params {
    uint8_t  dst_mac[6];      /* Ethernet dst = peer's observed Ethernet src MAC */
    uint8_t  src_mac[6];      /* Ethernet src + SCA src-logical [10:16] = OVMX HW MAC */
    uint8_t  peer_logical[6]; /* SCA dest-logical [2:8] = peer's advertised logical addr */
    uint32_t remote_conid;    /* [50:54] peer's SCS$DIRECTORY handle (learned) */
    uint32_t local_conid;     /* [54:58] OVMX's own SCS$DIRECTORY handle */
    uint16_t recv_ack;        /* [18:20]/[26:28]/[34:36] = OVMX recv_seq */
    uint16_t send_seq;        /* [20:22]/[30:32] = OVMX send_seq for this frame */
    uint16_t incarnation;     /* [22:24] node-incarnation echo (0 => leave the
                               * fresh-golden template value 1). See note below. */
};

/*
 * scs_dir_lookup_params - inputs to a directory lookup RESPONSE.
 */
struct scs_dir_lookup_params {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint8_t  peer_logical[6];
    uint32_t remote_conid;    /* peer's SCS$DIRECTORY handle */
    uint32_t local_conid;     /* OVMX's SCS$DIRECTORY handle */
    uint16_t recv_ack;
    uint16_t send_seq;
    uint8_t  opcode;          /* echo the request opcode into [16] (0x5b or 0x4b) */
    uint16_t op;              /* echo the request [46:48] directory-operation value */
    uint16_t incarnation;     /* [22:24] node-incarnation echo (0 => template 1) */
    char     name[SCS_DIR_NAME_LEN]; /* queried SYSAP name, echoed into [62:78] (blank-padded) */
    int      affirmative;     /* 1 => VMS$VAXcluster descriptor into [78:94]; 0 => NOT PRESENT HERE */
};

/*
 * scs_dir_build_connect_echo - Build the joiner's op=1 CONNECT-ECHO (SCA#23):
 * echoes the peer's SCS$DIRECTORY handle into remote [50:54], local still 0.
 * Returns 0, or -1 if p/out is NULL.
 */
int scs_dir_build_connect_echo(const struct scs_dir_params *p,
                               uint8_t out[SCS_DIR_ECHO_FRAME_LEN]);

/*
 * scs_dir_build_connect_response - Build the joiner's op=2 CONNECT-RESPONSE
 * (SCA#25): remote [50:54] = peer's handle, local [54:58] = OVMX's own handle
 * (the admission act binds the pair). Returns 0, or -1 if p/out is NULL.
 */
int scs_dir_build_connect_response(const struct scs_dir_params *p,
                                   uint8_t out[SCS_DIR_RESP_FRAME_LEN]);

/*
 * scs_dir_build_lookup_response - Build a 94-byte directory lookup RESPONSE:
 * echoes p->name into [62:78] and writes [78:94] = the VMS$VAXcluster
 * affirmative descriptor (p->affirmative != 0) or the GROUNDED literal
 * "NOT PRESENT HERE" (p->affirmative == 0). Returns 0, or -1 if p/out is NULL.
 */
int scs_dir_build_lookup_response(const struct scs_dir_lookup_params *p,
                                  uint8_t out[SCS_DIR_LOOKUP_FRAME_LEN]);

/* Read-only view of a received directory (0x5b / 0x4b-directory) frame. */
struct scs_dir_view {
    uint16_t total_sca_len;  /* LE u16 at abs 14 + 2 */
    uint8_t  opcode;         /* [16] (0x5b/0x7b/0x4b) */
    uint8_t  format;         /* [17] (expect 0x13) */
    uint16_t recv_ack;       /* [18:20] peer's ack of OVMX */
    uint16_t send_seq;       /* [20:22] peer's send-seq */
    uint16_t op;             /* [46:48] directory-operation field */
    uint16_t flag;           /* [48:50] companion flag */
    uint32_t remote_conid;   /* [50:54] */
    uint32_t local_conid;    /* [54:58] */
    uint32_t marker;         /* [58:62] request(0x00000000)/response(0x00000001) marker */
    int      has_name;       /* 1 if the frame is long enough to hold [62:78] */
    char     name[SCS_DIR_NAME_LEN + 1]; /* [62:78] queried SYSAP name, NUL-terminated */
    int      has_result;     /* 1 if the frame is long enough to hold [78:94] */
    int      result_zero;    /* 1 if [78:94] is all-zero (observed on golden requests only) */
    int      is_dir_connect_request; /* name=="SCS$DIRECTORY" && remote_conid==0 */
    int      is_lookup_request;      /* op==0x0a && has_name && marker==0 (request) */
};

/*
 * scs_dir_parse - Parse a received Ethernet+SCA frame's directory fields.
 * `frame` points at the Ethernet dst (abs 0); `len` is the frame length.
 * Fills opcode/format/counters/op/flag/Con.IDs for any frame >= abs 60, and
 * the name/result fields when the length carries them. Sets the
 * is_dir_connect_request / is_lookup_request classification flags.
 *
 * Returns 0 on success, -1 if frame/v is NULL or the frame is too short to
 * hold the directory envelope through the Con.ID pair (abs 72).
 */
int scs_dir_parse(const uint8_t *frame, size_t len, struct scs_dir_view *v);

#ifdef __cplusplus
}
#endif

#endif /* SCS_DIR_H */
