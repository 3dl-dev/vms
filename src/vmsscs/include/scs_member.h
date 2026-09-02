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
/* vms-ec7: the credit field ([48:50]) each captured 190-content template
 * carries. LABELED REPLAYS of the golden joiner frames, not computed
 * extensions. This class is MTYPE 10, so scsd_credit_stamp_outbound() (vms-aa1)
 * overwrites the field with the connection's live Pending Receive Credit on the
 * transmit path unless OVMX_NO_CREDIT_ACCOUNTING is set. */
#define SCS_MEMBER_ENV_CREDIT_MODEL  0u
#define SCS_MEMBER_ENV_CREDIT_PARAMS 0u
#define SCS_MEMBER_ENV_CREDIT_CONFIG 2u

#define SCS_MEMBER_SCA_LEN    190
#define SCS_MEMBER_FRAME_LEN  204

/* SCA-content offset of the SYSAP body (body[0] = SCA offset 58 = abs frame
 * offset 72, the first byte after the Local Con.ID at abs [68:72]). */
#define SCS_MEMBER_BODY_OFF   58

/* SCS envelope (shared with sec 4d/4g/4h) -- SCA-content offsets. */
#define SCS_MEMBER_MSGTYPE    0x4b /* sequenced-application (VC data) [16] */
/*
 * vms-2f3: THE RETRANSMIT FORM. 0x7b is 0x5b with the retransmit bit set, and
 * this file's sibling already grounded it for another SYSAP --
 * scs_dir.h:SCS_DIR_OPCODE_RETX, "its retransmit form (spec sec 4h)".
 *
 * WHY IT MATTERS HERE, AND WHY IT IS REJOIN-SHAPED. When a peer's connection
 * manager does not get a response it expects, PEDRIVER retransmits the message
 * with this msgtype. OVMX's CM gate accepted 0x4b and 0x5b and DISCARDED 0x7b
 * before any handler ran -- so every retransmission of a CM message was
 * invisible to us, the peer never got its answer, and it retransmitted again on
 * a timer that backs off to its ceiling.
 *
 * Measured, run d94-s3E (a refused rejoin): all three peers send
 * `cat 0x01 op 0x01` in MEMBER form (body[12]=0x21) as msgtype 0x7b, twice
 * each, ~3 s apart -- exactly the collapsed retransmit interval SCACP reports.
 * The matched fresh join d94-s7A contains ZERO 0x7b frames, because nothing
 * needed retransmitting there. That is the whole rejoin asymmetry: a fresh
 * identity never triggers a retransmission, and a returning one is deaf to
 * every retransmission from the first one onward.
 *
 * This is the SAME BUG the 0x5b comment below describes, one msgtype further
 * along, and it produces the same signature the peer side already showed us:
 * "the coordinator waited forever for a response it was never going to get and
 * the CSB stayed 'open'" -- SDA reported exactly that, state `open` with zero
 * flags, for every refused rejoin in session k.
 */
#define SCS_MEMBER_MSGTYPE_RETX 0x7b
#define SCS_MEMBER_MSGTYPE_ALT 0x5b /* vms-760: the SAME 190-byte CM class is also sent
                                     * with the establishing-phase msgtype, even on a
                                     * long-established VC (ref frame 217; the op 0x09
                                     * that ends the add-member transaction, d94-e3
                                     * frame 1212). Message class is the length + Con.ID
                                     * pair -- never gate a CM handler on 0x4b alone. */
#define SCS_MEMBER_FORMAT     0x13 /* format constant [17] (GROUNDED) */

/* SYSAP category/flags byte (body[8]). */
#define SCS_MEMBER_CAT_CONFIG   0x01 /* membership / config dialogue */
#define SCS_MEMBER_CAT_ACK      0x04 /* member commit/credit ack */
#define SCS_MEMBER_CAT_DLM      0x02 /* distributed lock manager. During a join the
                                      * coordinator replays lock-resource records as
                                      * token-correlated cat-0x02 transactions and
                                      * gates the barrier on them being answered. */
#define SCS_MEMBER_CAT_MEMBERSHIP 0x06 /* vms-760: the category the coordinator uses to
                                        * CLOSE the add-member transaction; token-correlated
                                        * and echoed exactly like category 0x01
                                        * (ref frames 671 -> 673, 2185 -> 2186) */
#define SCS_MEMBER_RESPONSE_BIT 0x80 /* OR'd into body[8] for a response */

/* SYSAP opcodes (body[9]) in the category-0x01 add-member dialogue. */
#define SCS_MEMBER_OP_MODEL   0x14 /* node CPU/model advertisement (len+ASCII) */
#define SCS_MEMBER_OP_PARAMS  0x01 /* cluster parameters (VOTES + version) */
#define SCS_MEMBER_OP_CONFIG  0x02 /* config / topology */
#define SCS_MEMBER_OP_COMMIT  0x03 /* member-driven membership-commit txn */
#define SCS_MEMBER_OP_LOCKRB  0x05 /* member-driven lock/resource rebuild txn */
#define SCS_MEMBER_OP_MEMBERSHIP 0x06 /* coordinator's post-commit membership burst;
                                       * txn=0, NOT token-correlated -- acknowledged
                                       * with a category-0x04 ack, never 0x81-echoed
                                       * (vms-760, ref frames 303-312 -> 313/314/315) */

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
    /* vms-e81: MEMBER-STATE block for op 0x01. All zero => the JOINER form, which
     * is what the baked template carries and what OVMX must send while it is
     * still joining. Populated only once OVMX is admitted -- see
     * scs_member_build_params() for why sending the joiner form as a member
     * stalls a newcomer indefinitely. */
    int      is_member;       /* body[12]=0x21 + body[82]=0x2b instead of 0x00/0x2a */
    uint16_t member_count;    /* body[18:20]; body[44:48] is this + 1 */
    uint64_t cluster_formed;  /* body[28:36]  VMS quadword, COPIED from a member */
    uint64_t last_transition; /* body[36:44]  VMS quadword, COPIED from a member */
    uint64_t own_admission;   /* body[64:72]  VMS quadword, OUR OWN admission */
    uint16_t txn;             /* body[4:6] on a transaction WE initiate (op 0x0b) */
    uint16_t checksum;        /* body[6:8] on a transaction WE initiate (op 0x0b) */
    const char *model;        /* op 0x14 only: model string; NULL => OVMX default */
    /* vms-2f3: op 0x02 REJOIN form. Zero => the FIRST-JOIN form, which is what
     * the baked template carries and what a genuinely new identity must send.
     * See scs_member_build_config() for the census that separates the two. */
    int      rejoin;          /* body[20:22]=1 -- "I hold prior cluster state" */
    uint16_t founding_sysid;  /* body[22:24]  the founding node's SCSSYSTEMID */
    /* body[28:36] reuses cluster_formed above -- the same COPIED quadword. */
    /* vms-e15: body[36:40] -- the membership GENERATION ordinal a rejoiner
     * carries. NON-ZERO on 4/4 real crash-rejoin specimens (9/2/2/3), ZERO on
     * 3/3 first-joins; the member SILENTLY DROPS a rejoin op 0x02 that leaves it
     * zero (d94-a4rejoin2 frame 356). The exact value is an OVMX-tracked ordinal
     * (spec sec 5) -- the member does not validate it (the four specimens carry
     * four different values and all succeed), only its non-zero presence on a
     * rejoin. Zero => leave body[36:40] zero (the first-join form / kill-switch). */
    uint32_t rejoin_generation;
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
    uint16_t votes;          /* body[22:24] VOTES (op 0x01 only; 0 otherwise).
                              * GROUNDED LE u16 across four vote configs (spec
                              * sec 4j) -- consumed by the quorum model (vms-7a9). */
    int      has_votes;      /* 1 iff this is a cat-0x01 op-0x01 params frame
                              * (so votes==0 means genuinely non-voting, not
                              * "field absent"). */
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

/*
 * scs_member_build_ack - build a category-0x04 SYSAP ACKNOWLEDGEMENT.
 *
 * A cat-0x04 message carries NO payload: its whole content is the SYSAP header
 * pair (our send-msg#, and an ack-msg# naming the highest peer send-msg# we
 * have taken). The coordinator emits a burst of category-0x01 opcode-0x06
 * messages during the add-member transaction and expects the joiner to keep
 * acknowledging them with these; a joiner that stays silent stalls the
 * transaction with its CSB left open.
 *
 * GROUNDED, vax3-2to3-established-join-20260730.pcap: VAX2 sends op 0x06
 * sm=9..18 (frames 303-312) and VAX3 answers with cat-0x04 frames 313 (sm=9
 * am=11), 314 (sm=10 am=14), 315 (sm=11 am=17) -- roughly one ack per three
 * messages, each naming a higher watermark.
 *
 * BODY[9] IS NOT MEANINGFUL. Real VMS acks carry 0x49/0x44/0x00/0x02 there and
 * the bytes after it are visibly a stale buffer: frame 313 reads
 * `04 49 "IR_LOOKUP  SCS$DIRECTORY"` -- a leftover "DIR_LOOKUP SCS$DIRECTORY"
 * whose first two bytes were overwritten by the category and opcode. VMS accepts
 * any value because it never reads it. OVMX writes 0x00 and zero payload: we do
 * not reproduce another implementation's uninitialised memory.
 */
int scs_member_build_ack(const struct scs_member_params *p,
                         uint8_t out[SCS_MEMBER_FRAME_LEN]);

/*
 * scs_member_build_token_response - respond to a token-correlated request by
 * carrying its (txn,checksum) and category|0x80 on an OTHERWISE EMPTY body.
 *
 * This is NOT scs_member_build_response. That one echoes the requester's whole
 * 132-byte SYSAP body, which is right for the category-0x01 transactions (spec
 * sec 4j: responses match their request on (txn,checksum,opcode)) but WRONG for
 * the category-0x06 request that closes the add-member transaction.
 *
 * GROUNDED, vax3-2to3-established-join-20260730.pcap frames 671 -> 673. The
 * request body carries live cluster structures -- Con.IDs (`eb a4 0a 00 e3 18`),
 * the cluster id (`e0 5a 62 7a`) and more. The real response does NOT send them
 * back: body[8]=0x86, body[9]=0x00, the token at body[4:8] carried verbatim, and
 * the payload rebuilt from zero.
 *
 * WHY THIS FUNCTION EXISTS AT ALL: echoing the payload instead CRASHED the peer.
 * VAX1 took a fatal bugcheck -- `INCONSTATE, Inconsistent I/O data base` -- when
 * OVMX replied to a category-0x06 request with its own I/O structures reflected
 * back at it. Reusing a transform across a category where it does not apply is
 * exactly the "plausible handler for a condition VMS never faces" failure mode;
 * the response shape is per-category and must be grounded per category.
 *
 * vms-760 UPDATE -- the note below this line used to say those fields were left
 * zero "because their meaning is not grounded". Leaving them zero was not the
 * problem; building the frame from the op-0x01 PARAMS template WAS. That made
 * body[10:132] byte-identical to our own PARAMS body and KILLED TWO REAL VAXES
 * (VAX3 INCONSTATE, VAX1 INVEXCEPTN, each within 0.3 ms of receiving ours).
 * The response is now built FRESH, all-zero except the fields the reference
 * carries, including a LIVE VMS time at body[64:72] -- a replayed timestamp
 * ~26 years off the cluster's era is among the prime suspects for the bugcheck.
 * There are TWO variants; scs_member_close_is_resource() picks between them.
 */
int scs_member_build_token_response(const struct scs_member_params *p,
                                    const uint8_t *req_frame, size_t req_len,
                                    uint8_t out[SCS_MEMBER_FRAME_LEN]);

/* vms-760: the DLM lock-resource rebuild record replayed during barrier step 5.
 * It is the ONLY cat-0x02 opcode that occurs during a join (216/216 in the
 * reference). Its response is a VERBATIM echo + body[34] -- and it must NOT take
 * the cat-0x01 body[18]/body[55] mutations, which land inside the L1 region and
 * the lock RESOURCE NAME respectively. See scs_member_build_dlm_response(). */
#define SCS_MEMBER_OP_DLM_REBUILD    0x0d
#define SCS_MEMBER_DLM_RESULT_BODYOFF 34   /* result-code stamp, fixed offset */
#define SCS_MEMBER_DLM_RESULT_0D      0xf9 /* the value for op 0x0d ONLY */

int scs_member_build_dlm_response(const struct scs_member_params *p,
                                  const uint8_t *req_frame, size_t req_len,
                                  uint8_t out[SCS_MEMBER_FRAME_LEN]);

/* vms-db20: originate the joiner's SCS$DIRECTORY / SYS$SYS_ID self-registration
 * (cat 0x02 op 0x0d, null value block) toward the coordinator during an
 * add-transition -- the one cat-0x02 frame a real joiner emits. body[4:8] are
 * minted by the caller (opaque, echoed by the coordinator); body[58:60] is the
 * node's own SCSSYSTEMID from the source logical. See scs_member.c. */
int scs_member_build_dlm_selfreg(const struct scs_member_params *p,
                                 uint8_t out[SCS_MEMBER_FRAME_LEN]);

/* The honest NULL-mode (NL) DLM registration OVMX originates for a resource a
 * non-coordinator member showed it in an op-0d rebuild record (db20-b, vms-7e2):
 * cat 0x02 op 0x01 ENQ, mode hard-pinned NL (holds nothing), resource name from
 * the shown record, dir_hash an honest 0. The respond-to-rebuild frame VAX1
 * granted 48/48. See scs_member.c. */
int scs_member_build_dlm_nl_enq(const struct scs_member_params *p,
                                uint16_t dir_hash,
                                const char *resname, uint8_t namelen,
                                uint8_t out[SCS_MEMBER_FRAME_LEN]);

/* The joiner's rebuild-COMPLETION pair driven to the coordinator after the
 * self-registration: cat 0x02 op 0x04 (completion), then op 0x03 COMMIT. OVMX
 * registers NOTHING HELD (resname + per-lock handles zeroed) -- the honest
 * "rebuild contribution complete, I hold nothing" signal (vms-cn3). See
 * scs_member.c for the INV-6 guardrail. */
int scs_member_build_dlm_op04(const struct scs_member_params *p, uint32_t lkid,
                              uint8_t out[SCS_MEMBER_FRAME_LEN]);
int scs_member_build_dlm_commit(const struct scs_member_params *p, uint32_t lkid,
                                uint8_t out[SCS_MEMBER_FRAME_LEN]);

/* The FULL per-lock DLM record OVMX carries in its op-01 ENQ registration (Layer
 * 3, vms-74f) -- every field a REAL attribute of the standing lock OVMX genuinely
 * holds, in OVMX's own executive encoding (INV-6, no fabrication). A skeletal
 * record is dropped by the coordinator (run 9c); the coordinator needs the full
 * per-lock context every real ENQ carries. */
struct scs_dlm_reg_fields {
    uint32_t req_lkid;    /* body[4:8]   OVMX's own lock handle */
    uint32_t dir_csid;    /* body[20:24] the resource's directory-master csid (OVMX's dir_csid) */
    uint32_t lock_id;     /* body[24:28] OVMX's unique per-lock id */
    uint16_t flags;       /* body[28:30] lock flags */
    uint8_t  mode;        /* body[30]    granted mode (NL for the volume presence lock) */
    uint32_t lockmgmt;    /* body[32:36] lock-mgmt count/flags word */
};

/* The cat 0x02 op 0x01 ENQ that REGISTERS one of OVMX's REAL standing system
 * locks to the coordinator (Layer 3, vms-74f): the full per-lock record from
 * OVMX's genuine lock state. See scs_member.c. */
int scs_member_build_dlm_reg_enq(const struct scs_member_params *p,
                                 const char *resname, uint8_t namelen,
                                 const struct scs_dlm_reg_fields *f,
                                 uint8_t out[SCS_MEMBER_FRAME_LEN]);

/* Current time as a VMS 64-bit absolute time (100 ns since 17-NOV-1858). */
uint64_t scs_member_vms_time_now(void);

/* Does this cat-0x06 request name a lock RESOURCE in ASCII at body[48:]?
 * The membership-close and the resource variant take DIFFERENT responses. */
int scs_member_close_is_resource(const uint8_t *rbody);

/* vms-760: ops the NON-COORDINATOR members send the joiner once the coordinator
 * has relayed the addition. None of these appears before the relay, which is why
 * they were never grounded until OVMX first reached this phase. */
#define SCS_MEMBER_OP_RELAY 0x12 /* the coordinator's relay of a new member. A
                                  * response must OVERWRITE body[20:24] with a
                                  * small integer -- the request carries the
                                  * PEER'S live CSID/cluster id there and echoing
                                  * it back reflects its own handle at it
                                  * (ref f286->f288 0x20400212 -> 0x00000003;
                                  * f404->f406 0x050007A2 -> 0x00000004). The
                                  * value tracks the post-transition member count
                                  * in both specimens -- INFERRED, 2 specimens. */
#define SCS_MEMBER_OP_0F    0x0f /* meaning UNKNOWN. Role slot 0x30, and it occurs
                                  * ONLY inside a class-0x03 (remove-a-failed-node)
                                  * transition, one step before the op 0x08. The
                                  * response is a plain echo + the response bit and
                                  * NOTHING ELSE -- notably body[18] is ECHOED, not
                                  * forced to 1. See scs_member_build_response(). */
#define SCS_MEMBER_OP_08    0x08 /* the class-0x03 transition-open (role slot 0x40,
                                  * tag 0x0340) -- the same slot op 0x09 fills for a
                                  * class-0x02 add. Plain echo + all mutations
                                  * (crash capture f1372->f1373; 6/6 genuine VMS
                                  * responses across the capture library). */
#define SCS_MEMBER_OP_DEPART 0x0d /* the class-0x04 transition-open (role slot 0x40,
                                  * tag 0x0440), sent by the LEAVING node itself
                                  * right after its op 0x12 announce and op 0x03
                                  * commit, and answered in ~0.5 ms with the same
                                  * echo recipe. GROUNDED 3/3 request/response pairs
                                  * (af2-established-rejoin f1950->f1951,
                                  * af2-firsttimer f1830->f1831 and f19435->f19437).
                                  * NOTE the cat-0x02 op 0x0d is a completely
                                  * different message (the DLM rebuild record) --
                                  * the response shape is keyed per CATEGORY and
                                  * these two must never be conflated. */

/*
 * vms-e4b: the ROLE SLOT and the TRANSITION CLASS -- body[16] and body[17].
 *
 * A whole-library census (26 captures) settles what these two bytes are, and
 * refutes the working hypothesis they were introduced under.
 *
 * body[16] is a stable ROLE SLOT. 0x10 announce/relay, 0x20 commit, 0x30 the
 * op-0x0f step, 0x40 transition-open, 0x60 barrier go. It partitions perfectly
 * by role across every capture with zero residuals.
 *
 * body[17] is NOT a generation -- the transition EPOCH is where sec 4(j)/4(p)
 * already put it, body[12:16] as an LE u32. Decisive: af2-firsttimer contains
 * six successive transitions whose epoch runs 3,4,6,7,9,11 (monotone) while
 * body[17] runs 0x04,0x04,0x02,0x04,0x02,0x02 -- it goes DOWN. body[17] is the
 * CLASS of the transition:
 *
 *   0x02 ADD a member      -- has the op 0x05/0x06 lock-DB push AND the barrier
 *   0x03 REMOVE a failed member -- has the extra op 0x0f step AND the barrier,
 *                             but no 0x05/0x06
 *   0x04 a node announces its OWN departure -- 0x12, 0x03, 0x0d, 0x0a and then
 *                             nothing. NO barrier at all: an 0x81/0x0b carrying
 *                             class 0x04 does not exist in any capture.
 *
 * And therefore the transition-open opcode does NOT vary by generation. The
 * 0x09 -> 0x08 -> 0x0d triple that looked like a generation sequence is the
 * three CLASSES, which merely happen to look like consecutive small integers.
 * af2-firsttimer runs three successive ADD transitions at epochs 6, 9 and 11 and
 * the open is op 0x09 / tag 0x0240 every single time (54/54 library-wide).
 *
 * DO NOT key the response allowlist on the role slot. Role 0x20 alone spans op
 * 0x03 and 0x05 (echo), op 0x06 (cat-0x04 ack, NEVER an echo -- 7882 frames) and
 * the joiner's own op 0x02; keying on it would fire a 132-byte echo at the whole
 * op-0x06 burst. Role tags do not exist at all on cat 0x02 or cat 0x06 -- the
 * categories that previously bugchecked real VAXes. The key stays
 * (SYSAP, category, opcode); the role slot is a corroborating cross-check and
 * the place we read the class from.
 */
#define SCS_MEMBER_ROLE_RELAY     0x10 /* body[16] on op 0x12 (and on 0x81/0x0b) */
#define SCS_MEMBER_ROLE_COMMIT    0x20 /* body[16] on op 0x03 / 0x05 / 0x06      */
#define SCS_MEMBER_ROLE_0F        0x30 /* body[16] on op 0x0f                    */
#define SCS_MEMBER_ROLE_XITION    0x40 /* body[16] on op 0x09 / 0x08 / 0x0d      */
#define SCS_MEMBER_ROLE_GO        0x60 /* body[16] on op 0x0a                    */
/* vms-2f3: the coordinator's transition ABORT, broadcast to every participant
 * as `cat 0x01 op 0x04` with body[16]=0x50 and body[17]=the class it is
 * abandoning. GROUNDED to the millisecond in d94-r1B: three copies leave the
 * coordinator 1.2 ms after the last op-0x05 lock-rebuild echo -- one to each
 * participant, on each one's own Con.ID pair -- and the VMS console prints
 * `Node VAX3 (csid 00010007) aborted VAXcluster state transition` in the same
 * hundredth of a second. It is NOT a timeout: it replaces the op 0x09
 * transition-open that a SUCCESSFUL join receives ~27 ms after the identical
 * round (d94-r2A #1248).
 *
 * DISTINCT FROM the op 0x04 that §3.4 of the handoff warns about: that one is
 * role 0x00 / class 0x00 on a NON-VMS$VAXcluster Con.ID -- a different SYSAP's
 * opcode 4 -- and it appears in successful joins. Match on the role slot, and
 * never on the opcode alone. txn=0, so per spec sec 4(r) it takes no response;
 * this is a notification we must LOG, not answer. */
#define SCS_MEMBER_ROLE_ABORT     0x50 /* body[16] on the cat-0x01 op 0x04 abort */
#define SCS_MEMBER_OP_ABORT       0x04 /* transition ABORT (with ROLE_ABORT only) */
#define SCS_MEMBER_CLASS_ADD      0x02 /* body[17]: add a member    (barrier)    */
#define SCS_MEMBER_CLASS_REMOVE   0x03 /* body[17]: remove a failed one (barrier)*/
#define SCS_MEMBER_CLASS_DEPART   0x04 /* body[17]: self-departure  (NO barrier) */
#define SCS_MEMBER_ROLE_BODYOFF   16   /* body[16] role slot                     */
#define SCS_MEMBER_CLASS_BODYOFF  17   /* body[17] transition class              */

/* The cluster-wide state-transition barrier (vms-760). */
#define SCS_MEMBER_OP_BARRIER      0x0b /* joiner-INITIATED barrier step */
#define SCS_MEMBER_OP_BARRIER_REL  0x0c /* coordinator's release of step N */
#define SCS_MEMBER_OP_XITION       0x09 /* coordinator opens the transition */
#define SCS_MEMBER_OP_XITION_GO    0x0a /* coordinator says "start the barrier" */
#define SCS_MEMBER_BARRIER_STEPS   12   /* invariant across 6 joins / 4 clusters */
#define SCS_MEMBER_XITION_TAG      0x0240 /* body[16:18] on the op 0x09 */
#define SCS_MEMBER_BARRIER_TAG     0x0260 /* body[16:18] on the op 0x0a that STARTS
                                           * the barrier for a class-0x02 ADD. The
                                           * tag is (class << 8) | role, so the
                                           * class-0x03 REMOVE start is 0x0360 and
                                           * the class-0x04 self-departure is 0x0460.
                                           * vms-e4b: 0x0360 MUST arm the barrier
                                           * too -- a removal runs the same 12 steps
                                           * and a silent member strands it. 0x0460
                                           * must NOT: a departure has no barrier. */
#define SCS_MEMBER_BARRIER_TAG_REM 0x0360 /* class-0x03 barrier start (see above) */
#define SCS_MEMBER_EPOCH_BODYOFF   12   /* body[12:16] transition epoch (LE u32) */
#define SCS_MEMBER_STEP_BODYOFF    16   /* body[16:20] barrier step index (LE u32) */

/*
 * scs_member_build_barrier - build one joiner-initiated barrier step,
 * category 0x01 opcode 0x0b.
 *
 * THE BARRIER IS CLUSTER-WIDE AND THE JOINER IS A REQUIRED PARTICIPANT. The
 * coordinator runs the same 12-step dialogue with EVERY member and releases
 * step N to nobody until every member has sent its step-N request. Grounded in
 * the reference: at step 5 it held our completed request for 89 ms and released
 * 0x0c#5 to both members 0.8 ms after the last member caught up; 0x0c#N never
 * precedes 0x0b#N anywhere (72 ordered pairs, 0 residuals). A joiner that never
 * sends 0x0b leaves the barrier permanently one member short -- which is why
 * omitting it does not merely fail to join, it makes the cluster time out,
 * print "%CNXMAN, aborting VAXcluster state transition", and DROP its healthy
 * members. Exactly 12 steps, indices 1..12, in 6 joins across 4 clusters and 3
 * different joiner nodes.
 *
 * The body is minimal by design and that minimal form is proven acceptable: one
 * real VMS joiner sends an entirely zero body apart from the four fields below
 * and the coordinator answers normally. Other joiners leave lock-resource names
 * and other stale text in body[20:132]; coordinator behaviour is identical, so
 * that span is residue and we send zeros.
 *
 *   body[4:6]   our own per-VC transaction-context id
 *   body[6:8]   our own per-VC counter, +1 per transaction we initiate
 *   body[12:16] the transition EPOCH, copied from the coordinator's op 0x09
 *   body[16:20] the step index N, LE u32, 1..12
 */
int scs_member_build_barrier(const struct scs_member_params *p,
                             uint32_t epoch, uint32_t step,
                             uint8_t out[SCS_MEMBER_FRAME_LEN]);

/*
 * scs_member_build_depart - vms-ab1 (spec 4(O.29)): the class-0x04 SELF-DEPARTURE
 * transition-open, category 0x01 opcode 0x0d (OP_DEPART), role slot body[16]=0x40
 * (ROLE_XITION), transition class body[17]=0x04 (CLASS_DEPART). This is the CM
 * message a LEAVING node emits to announce its OWN departure so the coordinator
 * REMOVES its Cluster System Block immediately rather than holding it in a
 * reconnect/long_break WAIT for RECNXINTERVAL (VAXcluster Principles p. 7-29;
 * spec 4(O.28)). OVMX has always PARSED this inbound (scsd.c class-0x04 handler,
 * GROUNDED 3/3 request/response pairs) but never EMITTED its own; before vms-ab1
 * OVMX's departure was only a per-connection SCS DISCONNECT.
 *
 * MINIMAL BODY BY DESIGN, and the minimal form is what the ack/barrier builders
 * already prove acceptable: everything past the class byte is ZERO. This is a
 * transition-OPEN and the post-transition facts (new member bitmap, transition
 * time) are the COORDINATOR's to compute when it runs the removal, NEVER the
 * leaver's to assert (Rule 10). Do NOT reintroduce a node-parameter template
 * here -- the vms-760 token_response crash bugchecked two real VAXes precisely
 * because it carried a PARAMS-derived live-node-parameter body.
 *
 *   body[4:6]   our own per-VC transaction-context id
 *   body[6:8]   our own per-VC counter, +1 per transaction we initiate
 *   body[12:16] the transition EPOCH, echoed from the coordinator (held, not
 *               computed); 0 if none was ever learned (residue, as the barrier)
 *   body[16]    role slot 0x40 (ROLE_XITION)
 *   body[17]    transition class 0x04 (CLASS_DEPART)
 */
int scs_member_build_depart(const struct scs_member_params *p,
                            uint32_t epoch,
                            uint8_t out[SCS_MEMBER_FRAME_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* SCS_MEMBER_H */
