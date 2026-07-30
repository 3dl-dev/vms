/*
 * scs_member.h - VMS$VAXcluster connection-manager add-member transaction
 * on the 190-byte VMS$VAXcluster VC (vms-224). THE MILESTONE: this is the
 * SYSAP dialogue that promotes OVMX from a bound-but-transient SCS connection
 * into a full cluster MEMBER (a CSB in SDA SHOW CLUSTER, Member State Seq
 * advanced).
 *
 * WHY THIS EXISTS. Everything through the 0x4b CONNECT-RESPONSE (vms-c6d,
 * scs_connect.c) only binds the VMS$VAXcluster Con.ID pair -- the joiner does
 * NOT appear in SHOW CLUSTER as a member until the connection manager's
 * add-member dialogue runs on the 190-byte VC (spec sec 4j). This module
 * builds the frames OVMX (the joiner) must SEND to complete that dialogue.
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md rule 8 / spec sec 0). Every frame is built
 * from a byte-exact template captured off our own reference lab
 * (formation-ci1-joinwindow.pcap, the golden VAX2-joins-VAX1 handshake), with
 * ONLY the GROUNDED fields substituted (identity MACs, the Con.ID pair, the
 * live SCS envelope counters, the SYSAP send/ack-msg# counters, the joiner's
 * VOTES, and -- for op 0x14 -- OVMX's own model string). Ungrounded body bytes
 * (per-boot/timing tokens, the op-0x01 version-token span) are reproduced
 * exactly as observed and labeled, never invented. No VSI/HPE source or binary
 * was read, disassembled, or copied.
 *
 * THE CHECKSUM (spec sec 4j body[6:8]) IS NOT A WALL FOR THE JOINER. The
 * joiner's OWN config messages (op 0x14/0x01/0x02) carry txn=0x0000 AND
 * checksum=0x0000 -- byte-exact in every captured joiner frame -- so OVMX
 * never has to DERIVE a checksum. The (txn,checksum) correlation token appears
 * only on the member-driven op 0x03 (commit) and op 0x05 (lock-rebuild)
 * transactions, and the joiner simply ECHOES the member's token back in its
 * 0x81 response (spec sec 4j: "17/17 responses match their request on
 * (txn,checksum,opcode)"). scs_member_build_response does exactly that echo.
 *
 * THE SYSAP TRANSACTION ENVELOPE (spec sec 4j, body-relative; body[0]=abs 72):
 *   body[0:2]  SYSAP send-msg#   (this sender's monotonic app counter, from 1)
 *   body[2:4]  SYSAP ack-msg#    (acks the peer's highest send-msg# seen)
 *   body[4:6]  transaction number (0 on joiner config; echoed on 0x03/0x05)
 *   body[6:8]  checksum / correlation token (0 on joiner config; echoed)
 *   body[8]    category/flags    (0x01=membership/config; bit 0x80=response)
 *   body[9]    opcode            (0x14 model, 0x01 params, 0x02 config, ...)
 *   body[22:24] VOTES (LE u16, op 0x01 only; OVMX joins non-voting => 0x0000)
 *
 * These are DISTINCT from the SCS envelope counters (spec sec 4h) at SCA
 * offsets [18:20] recv_ack / [20:22] send_seq: the SCS layer sequences the VC,
 * the SYSAP layer sequences the connection-manager dialogue. Both advance
 * independently and in lockstep with their own peer counter.
 */
#ifndef SCS_MEMBER_H
#define SCS_MEMBER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The add-member dialogue rides the 190-byte VMS$VAXcluster VC class (spec
 * sec 4d / Table 2): 190 SCA-content bytes + 14-byte Ethernet header. */
#define SCS_MEMBER_SCA_LEN    190
#define SCS_MEMBER_FRAME_LEN  204

/* SCA-content offset of the SYSAP body (body[0] = SCA offset 58 = abs frame
 * offset 72, the first byte after the Local Con.ID at abs [68:72]). */
#define SCS_MEMBER_BODY_OFF   58

/* SCS envelope (shared with sec 4d/4g/4h) -- SCA-content offsets. */
#define SCS_MEMBER_MSGTYPE    0x4b /* sequenced-application (VC data) [16] */
#define SCS_MEMBER_FORMAT     0x13 /* format constant [17] (GROUNDED) */

/* SYSAP category/flags byte (body[8]). */
#define SCS_MEMBER_CAT_CONFIG   0x01 /* membership / config dialogue */
#define SCS_MEMBER_CAT_ACK      0x04 /* member commit/credit ack */
#define SCS_MEMBER_RESPONSE_BIT 0x80 /* OR'd into body[8] for a response */

/* SYSAP opcodes (body[9]) in the category-0x01 add-member dialogue. */
#define SCS_MEMBER_OP_MODEL   0x14 /* node CPU/model advertisement (len+ASCII) */
#define SCS_MEMBER_OP_PARAMS  0x01 /* cluster parameters (VOTES + version) */
#define SCS_MEMBER_OP_CONFIG  0x02 /* config / topology */
#define SCS_MEMBER_OP_COMMIT  0x03 /* member-driven membership-commit txn */
#define SCS_MEMBER_OP_LOCKRB  0x05 /* member-driven lock/resource rebuild txn */

/* SYSAP-body-relative field offsets (body[0] = SCA offset 58). */
#define SCS_MEMBER_VOTES_BODYOFF 22 /* VOTES LE u16 (abs 94) -- op 0x01 (GROUNDED) */
#define SCS_MEMBER_MODEL_LEN_BODYOFF 16 /* model length prefix (op 0x14) */
#define SCS_MEMBER_RESP_MARK_BODYOFF 18 /* body[18]=0x01 on a 0x81 response (GROUNDED) */

/* OVMX joins non-voting so it can never break VAX quorum (design sec 8). */
#define SCS_MEMBER_VOTES_NONVOTING 0x0000

/*
 * scs_member_params - inputs to build one add-member config frame.
 */
struct scs_member_params {
    uint8_t  dst_mac[6];      /* Ethernet dst = peer/member's Ethernet src MAC */
    uint8_t  src_mac[6];      /* Ethernet src (abs 6) = OVMX HW MAC */
    uint8_t  src_logical[6];  /* SCA src-logical [10:16] (abs 24) = aa:00:04:00:<LE16(sysid)>;
                                 the cluster-LOGICAL addr, NOT the raw HW MAC (vms-9f3) */
    uint8_t  peer_logical[6]; /* SCA dest-logical [2:8] = member's advertised logical addr */
    uint32_t remote_conid;    /* member's VMS$VAXcluster Con.ID (abs 64 [50:54]) */
    uint32_t local_conid;     /* OVMX's VMS$VAXcluster Con.ID (abs 68 [54:58]) */
    uint16_t recv_ack;        /* SCS recv_seq (envelope [18:20]/[26:28]/[34:36]) */
    uint16_t send_seq;        /* SCS send_seq (envelope [20:22] + mirror [30:32]) */
    uint16_t incarnation;     /* envelope [22:24]; 0 => leave template's fresh 1 */
    uint16_t sysap_send_msg;  /* SYSAP body[0:2] (OVMX's own app counter) */
    uint16_t sysap_ack_msg;   /* SYSAP body[2:4] (ack of member's highest send-msg#) */
    uint16_t votes;           /* op 0x01 only: SYSAP body[22:24] (0 = non-voting) */
    const char *model;        /* op 0x14 only: model string; NULL => OVMX default */
    int      config_admission; /* op 0x02 only (vms-760). 0 (default) => reproduce the
                                  FORMATION golden SCA#60 byte-exact. 1 => the variant the
                                  2->3 established-join reference sends to TRIGGER admission
                                  (frame 285): body[10:12]=0x5041 and twelve spaces at
                                  body[40:52]. The two specimens genuinely differ, so this
                                  is a selector between observed variants, not a fix to one
                                  of them. Semantics of both fields are UNGROUNDED -- see
                                  scs_member_build_config() and spec 5(z). */
};

/*
 * scs_member_build_model  - category-0x01 op-0x14 node CPU/model advertisement.
 * scs_member_build_params - category-0x01 op-0x01 cluster-parameters (VOTES).
 * scs_member_build_config - category-0x01 op-0x02 config/topology.
 *
 * Each fills out[SCS_MEMBER_FRAME_LEN] with a complete Ethernet+SCA 190-byte
 * VMS$VAXcluster VC frame per spec sec 4j. Returns 0, or -1 on NULL args (or,
 * for build_model, a model string too long for the body).
 */
int scs_member_build_model(const struct scs_member_params *p,
                           uint8_t out[SCS_MEMBER_FRAME_LEN]);
int scs_member_build_params(const struct scs_member_params *p,
                            uint8_t out[SCS_MEMBER_FRAME_LEN]);
int scs_member_build_config(const struct scs_member_params *p,
                            uint8_t out[SCS_MEMBER_FRAME_LEN]);

/*
 * scs_member_view - decoded view of a received 190-byte VMS$VAXcluster frame.
 */
struct scs_member_view {
    uint16_t total_sca_len;
    uint8_t  msgtype;
    uint8_t  format;
    uint16_t recv_ack;       /* SCS envelope [18:20] */
    uint16_t send_seq;       /* SCS envelope [20:22] */
    uint32_t remote_conid;   /* abs 64 */
    uint32_t local_conid;    /* abs 68 */
    uint8_t  category;       /* body[8] (includes the 0x80 response bit) */
    uint8_t  opcode;         /* body[9] */
    uint16_t sysap_send_msg; /* body[0:2] */
    uint16_t sysap_ack_msg;  /* body[2:4] */
    uint16_t txn;            /* body[4:6] */
    uint16_t checksum;       /* body[6:8] */
    int      is_response;    /* category & 0x80 */
    int      is_member_txn;  /* member-driven commit/lock-rebuild the joiner must
                              * 0x81-respond to: category==0x01 (not response) &&
                              * opcode in {0x03, 0x05} */
};

/*
 * scs_member_parse - classify a received frame as a 190-byte VMS$VAXcluster CM
 * message and fill *v. Returns 0 on a well-formed 190-byte frame, -1 otherwise
 * (wrong length class, too short, or NULL args). Does NOT check the Con.ID pair
 * -- the caller matches the connection.
 */
int scs_member_parse(const uint8_t *frame, size_t len, struct scs_member_view *v);

/*
 * scs_member_build_response - build OVMX's 0x81 response to a member-driven
 * op-0x03/op-0x05 request. The member's SYSAP body is ECHOED (carrying its
 * (txn,checksum) correlation token byte-for-byte, spec sec 4j) with only:
 *   body[0:2] <- p->sysap_send_msg  (OVMX's own app counter)
 *   body[2:4] <- p->sysap_ack_msg   (ack of the member's send-msg#)
 *   body[8]   |= 0x80               (response bit)
 *   body[18]  <- 0x01               (response marker, GROUNDED 0x03+0x05)
 * and a fresh SCS envelope (swapped Con.ID pair + live counters from p).
 * req_frame/req_len are the received member request. Returns 0, or -1 on NULL
 * args / a req_frame that is not a >=204-byte 190-class frame.
 */
int scs_member_build_response(const struct scs_member_params *p,
                              const uint8_t *req_frame, size_t req_len,
                              uint8_t out[SCS_MEMBER_FRAME_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* SCS_MEMBER_H */
