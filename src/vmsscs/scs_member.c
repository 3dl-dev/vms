/*
 * scs_member.c - VMS$VAXcluster connection-manager add-member transaction
 * builder/parser (vms-224). See scs_member.h for the full clean-room
 * provenance and the GROUNDED-vs-REPLAYED field breakdown.
 *
 * Templates below are byte-exact 190-byte SCA-content captures of the golden
 * JOINER (VAX2) add-member frames from formation-ci1-joinwindow.pcap:
 *   op 0x14  = SCA#48 (VAX2->VAX1, sendmsg=1)
 *   op 0x01  = SCA#49 (VAX2->VAX1, sendmsg=2, VOTES 0)
 *   op 0x02  = SCA#60 (VAX2->VAX1, sendmsg=3)
 * Substituted at build time (GROUNDED positions only): dst logical [2:8], src
 * logical [10:16], SCS counters [18:24]/[26:28]/[30:32]/[34:36], the Con.ID
 * pair [50:54]/[54:58], the SYSAP send/ack-msg# body[0:4], the op-0x14 model
 * string, and the op-0x01 VOTES body[22:24]. Every other byte is a labeled
 * REPLAY of the captured joiner frame (per-boot/version tokens included).
 */
#include "scs_member.h"

#include <string.h>

/* op 0x14 model advertisement -- golden SCA#48 (VAX2->VAX1). */
static const uint8_t member_model_tmpl[SCS_MEMBER_SCA_LEN] = {
    0xbc, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x00, 0xaa, 0x00,
    0x04, 0x00, 0x02, 0x04, 0x4b, 0x13, 0x0a, 0x00, 0x0a, 0x00, 0x01, 0x00,
    0x12, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x0a, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x92, 0x00, 0x04, 0x00, 0x0a, 0x00,
    0x00, 0x00, 0x09, 0x00, 0xc5, 0x62, 0x08, 0x00, 0x58, 0x33, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x14, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x15, 0x56, 0x41, 0x58, 0x73, 0x65, 0x72, 0x76, 0x65, 0x72,
    0x20, 0x33, 0x39, 0x30, 0x30, 0x20, 0x53, 0x65, 0x72, 0x69, 0x65, 0x73,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* op 0x01 cluster-parameters -- golden SCA#49 (VAX2->VAX1, VOTES 0). */
static const uint8_t member_params_tmpl[SCS_MEMBER_SCA_LEN] = {
    0xbc, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x00, 0xaa, 0x00,
    0x04, 0x00, 0x02, 0x04, 0x4b, 0x13, 0x0a, 0x00, 0x0b, 0x00, 0x01, 0x00,
    0x12, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x0a, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x92, 0x00, 0x04, 0x00, 0x0a, 0x00,
    0x00, 0x00, 0x09, 0x00, 0xc5, 0x62, 0x08, 0x00, 0x58, 0x33, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x50,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x80, 0x4a, 0x3f, 0x0e, 0x57, 0x9f, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2a, 0x00, 0x0e, 0x00,
    0x00, 0x00, 0x56, 0x37, 0x2e, 0x33, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* op 0x02 config/topology -- golden SCA#60 (VAX2->VAX1). */
static const uint8_t member_config_tmpl[SCS_MEMBER_SCA_LEN] = {
    0xbc, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x00, 0xaa, 0x00,
    0x04, 0x00, 0x02, 0x04, 0x4b, 0x13, 0x0f, 0x00, 0x10, 0x00, 0x01, 0x00,
    0x12, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x0f, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x92, 0x00, 0x04, 0x00, 0x0a, 0x00,
    0x02, 0x00, 0x09, 0x00, 0xc5, 0x62, 0x08, 0x00, 0x58, 0x33, 0x03, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* OVMX's own CPU/model advertisement string (op 0x14). NOT a VAXserver: OVMX
 * announces itself honestly (INV-0 trademark ceiling / never-lie-to-the-metal).
 * It is a display-only field in SHOW CLUSTER, so an OVMX-labeled string is both
 * legal and better milestone proof that OVMX -- not a faked VAX -- joined. */
#define OVMX_MODEL_STRING "OVMX Cluster Node"

static void put_le16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xff);
    dst[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_le32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xff);
    dst[1] = (uint8_t)((v >> 8) & 0xff);
    dst[2] = (uint8_t)((v >> 16) & 0xff);
    dst[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

/*
 * build_common - lay down the Ethernet header + a 190-byte SCA template, then
 * substitute the shared SCS envelope (dst/src logical, live counters), the
 * Con.ID pair, and the SYSAP send/ack-msg# header. Caller substitutes the
 * op-specific body fields afterward. SCA-content offset == out+14+off.
 */
static void build_common(const struct scs_member_params *p, const uint8_t *tmpl,
                         uint8_t *out)
{
    /* Ethernet header (abs 0-13). */
    memcpy(out + 0, p->dst_mac, 6);
    memcpy(out + 6, p->src_mac, 6);
    out[12] = 0x60;
    out[13] = 0x07;

    /* SCA content (abs 14+) from the captured joiner template. */
    memcpy(out + 14, tmpl, SCS_MEMBER_SCA_LEN);

    /* Identity substitutions (SCA-content offsets + 14). */
    memcpy(out + 14 + 2, p->peer_logical, 6);  /* dest logical [2:8]  (abs 16) */
    memcpy(out + 14 + 10, p->src_logical, 6);   /* src-logical [10:16](abs 24) = aa:00:04:00:<sysid>
                                                 * cluster-LOGICAL addr, NOT raw HW MAC (vms-9f3) */

    /* SCS sequenced-message counters (spec sec 4h): recv_ack at [18:20]
     * repeated at [26:28]/[34:36]; send_seq at [20:22] mirrored at [30:32]
     * (the [20:22]==[30:32] mirror is GROUNDED 17758/17758 frames). The
     * incarnation echo at [22:24] (the established-join gate, sec 4i.B): 0
     * leaves the template's fresh-contact value 1. */
    put_le16(out + 14 + 18, p->recv_ack);
    put_le16(out + 14 + 20, p->send_seq);
    if (p->incarnation != 0) {
        put_le16(out + 14 + 22, p->incarnation);
    }
    put_le16(out + 14 + 26, p->recv_ack);
    put_le16(out + 14 + 30, p->send_seq);
    put_le16(out + 14 + 34, p->recv_ack);

    /* Con.ID pair (spec sec 4d): remote at [50:54] (abs 64), local at [54:58]
     * (abs 68). */
    put_le32(out + 14 + 50, p->remote_conid);
    put_le32(out + 14 + 54, p->local_conid);

    /* SYSAP transaction envelope send/ack-msg# (body[0:2]/[2:4]; body[0] =
     * SCA offset 58). */
    put_le16(out + 14 + SCS_MEMBER_BODY_OFF + 0, p->sysap_send_msg);
    put_le16(out + 14 + SCS_MEMBER_BODY_OFF + 2, p->sysap_ack_msg);
}

int scs_member_build_model(const struct scs_member_params *p,
                           uint8_t out[SCS_MEMBER_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }

    const char *model = (p->model != NULL) ? p->model : OVMX_MODEL_STRING;
    size_t mlen = strlen(model);
    /* The length-prefixed model string sits at body[16]; the body is 132 bytes
     * (SCA [58:190]), so the string + its 1-byte length prefix must fit in
     * body[16..131] = 116 bytes -> string <= 115. */
    if (mlen > 115) {
        return -1;
    }

    build_common(p, member_model_tmpl, out);

    /* Substitute OVMX's own model string: zero the old field, then write the
     * length prefix + ASCII (op 0x14, spec sec 4j). body[16] = SCA offset
     * 58+16=74; absolute = out+14+74. */
    uint8_t *mfield = out + 14 + SCS_MEMBER_BODY_OFF + SCS_MEMBER_MODEL_LEN_BODYOFF;
    /* Clear the template's "VAXserver 3900 Series" + any trailing bytes up to
     * the end of the body so no stale characters leak. */
    memset(mfield, 0, (size_t)(SCS_MEMBER_SCA_LEN - (SCS_MEMBER_BODY_OFF + SCS_MEMBER_MODEL_LEN_BODYOFF)));
    mfield[0] = (uint8_t)mlen;
    memcpy(mfield + 1, model, mlen);

    return 0;
}

int scs_member_build_params(const struct scs_member_params *p,
                            uint8_t out[SCS_MEMBER_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }

    build_common(p, member_params_tmpl, out);

    /* VOTES at body[22:24] (abs 94), LE u16 -- GROUNDED across four vote
     * configurations (spec sec 4j). OVMX joins non-voting => 0. */
    put_le16(out + 14 + SCS_MEMBER_BODY_OFF + SCS_MEMBER_VOTES_BODYOFF, p->votes);

    return 0;
}

int scs_member_build_config(const struct scs_member_params *p,
                            uint8_t out[SCS_MEMBER_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }

    build_common(p, member_config_tmpl, out);

    /* vms-760: body[10:12] and body[40:52] are left ZERO.
     * An earlier revision replayed 0x5041 and twelve 0x20 spaces here, copied
     * from the one admission specimen we had. A survey of every op-0x02 in the
     * capture library retired that: 9 of 12 genuine VMS specimens carry zeros in
     * both places and are acked identically, and the 3 outliers hold printable
     * digraphs ("AP", "IS") and ASCII spaces -- stale buffer contents, not data.
     * We do not reproduce another implementation's uninitialised memory. */
    return 0;
}

int scs_member_parse(const uint8_t *frame, size_t len, struct scs_member_view *v)
{
    if (frame == NULL || v == NULL) {
        return -1;
    }
    if (len < SCS_MEMBER_FRAME_LEN) {
        return -1;
    }

    uint16_t lenword = get_le16(frame + 14);
    uint16_t total = (uint16_t)(lenword + 2);
    if (total != SCS_MEMBER_SCA_LEN) {
        return -1;
    }

    memset(v, 0, sizeof(*v));
    v->total_sca_len = total;
    v->msgtype = frame[30];       /* SCA offset 16 */
    v->format = frame[31];        /* SCA offset 17 */
    v->recv_ack = get_le16(frame + 32); /* SCA [18:20] */
    v->send_seq = get_le16(frame + 34); /* SCA [20:22] */
    v->remote_conid = get_le32(frame + 64);
    v->local_conid = get_le32(frame + 68);

    const uint8_t *body = frame + 72; /* SYSAP body[0] = abs 72 */
    v->sysap_send_msg = get_le16(body + 0);
    v->sysap_ack_msg = get_le16(body + 2);
    v->txn = get_le16(body + 4);
    v->checksum = get_le16(body + 6);
    v->category = body[8];
    v->opcode = body[9];
    v->is_response = (v->category & SCS_MEMBER_RESPONSE_BIT) ? 1 : 0;

    /* A member-driven transaction the joiner must 0x81-respond to: a
     * category-0x01 (non-response) commit (0x03) or lock-rebuild (0x05). */
    v->is_member_txn = (!v->is_response &&
                        (v->category & 0x7f) == SCS_MEMBER_CAT_CONFIG &&
                        (v->opcode == SCS_MEMBER_OP_COMMIT ||
                         v->opcode == SCS_MEMBER_OP_LOCKRB));
    return 0;
}

int scs_member_build_ack(const struct scs_member_params *p,
                         uint8_t out[SCS_MEMBER_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }

    /* Any 190-byte template supplies the correct constant envelope span;
     * build_common substitutes identity, Con.ID pair and live counters. */
    build_common(p, member_config_tmpl, out);

    uint8_t *body = out + 72;
    /* A cat-0x04 ack is header-only: everything past the category/opcode is
     * payload we do not carry. Zero it rather than inherit template bytes. */
    memset(body + 4, 0, SCS_MEMBER_SCA_LEN - SCS_MEMBER_BODY_OFF - 4);
    put_le16(body + 0, p->sysap_send_msg);
    put_le16(body + 2, p->sysap_ack_msg);
    body[8] = SCS_MEMBER_CAT_ACK; /* 0x04 */
    body[9] = 0x00;               /* see header: real VMS carries residue here */
    return 0;
}

int scs_member_build_barrier(const struct scs_member_params *p,
                             uint32_t epoch, uint32_t step,
                             uint8_t out[SCS_MEMBER_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    build_common(p, member_config_tmpl, out);

    uint8_t *body = out + 72;
    memset(body + 4, 0, SCS_MEMBER_SCA_LEN - SCS_MEMBER_BODY_OFF - 4);
    put_le16(body + 0, p->sysap_send_msg);
    put_le16(body + 2, p->sysap_ack_msg);
    put_le16(body + 4, p->txn);
    put_le16(body + 6, p->checksum);
    body[8] = SCS_MEMBER_CAT_CONFIG;
    body[9] = SCS_MEMBER_OP_BARRIER;
    /* body[10:12] left zero -- residue in the specimens that carry anything. */
    put_le32(body + SCS_MEMBER_EPOCH_BODYOFF, epoch);
    put_le32(body + SCS_MEMBER_STEP_BODYOFF, step);
    return 0;
}

int scs_member_build_token_response(const struct scs_member_params *p,
                                    const uint8_t *req_frame, size_t req_len,
                                    uint8_t out[SCS_MEMBER_FRAME_LEN])
{
    if (p == NULL || req_frame == NULL || out == NULL) {
        return -1;
    }
    if (req_len < SCS_MEMBER_FRAME_LEN) {
        return -1;
    }
    uint16_t lenword = get_le16(req_frame + 14);
    if ((uint16_t)(lenword + 2) != SCS_MEMBER_SCA_LEN) {
        return -1;
    }

    /* vms-760: base this on the PARAMS template, not the config one. The real
     * cat-0x86 response carries the responder's OWN node-parameter block, and
     * that block is structurally the one we already send in our op-0x01 PARAMS
     * message -- byte-for-byte the same shape at the same offsets:
     *   body[72:76] = 0x00000010, body[76:80] = 0x00000001,
     *   body[88:96] = "V7.3    "
     * (ref frame 673 vs the golden op-0x01; the spans that differ, body[64:72]
     * and body[80:88], are per-node/per-boot values, and ours are our own).
     * Sending an EMPTY body here left the coordinator holding the barrier: it
     * answered our step-5 request and then never released step 5 (d94-e11
     * frames 1297 -> 1299/1301 -> nothing). */
    build_common(p, member_params_tmpl, out);

    uint8_t *body = out + 72;
    const uint8_t *rbody = req_frame + 72;
    put_le16(body + 0, p->sysap_send_msg);
    put_le16(body + 2, p->sysap_ack_msg);
    body[4] = rbody[4]; body[5] = rbody[5]; /* txn      -- carried verbatim */
    body[6] = rbody[6]; body[7] = rbody[7]; /* checksum -- carried verbatim */
    body[8] = (uint8_t)(rbody[8] | SCS_MEMBER_RESPONSE_BIT);
    body[9] = rbody[9];
    return 0;
}

int scs_member_build_response(const struct scs_member_params *p,
                              const uint8_t *req_frame, size_t req_len,
                              uint8_t out[SCS_MEMBER_FRAME_LEN])
{
    if (p == NULL || req_frame == NULL || out == NULL) {
        return -1;
    }
    if (req_len < SCS_MEMBER_FRAME_LEN) {
        return -1;
    }
    uint16_t lenword = get_le16(req_frame + 14);
    if ((uint16_t)(lenword + 2) != SCS_MEMBER_SCA_LEN) {
        return -1;
    }

    /* Start from the member_config template (any 190-byte template gives the
     * correct constant envelope span [36:50]); build_common overwrites all the
     * substituted fields, and we then echo the member's SYSAP body. */
    build_common(p, member_config_tmpl, out);

    /* Echo the member's entire SYSAP body (carries txn+checksum byte-for-byte,
     * spec sec 4j), then apply the response transform. body[0] = abs 72. */
    uint8_t *obody = out + 72;
    const uint8_t *rbody = req_frame + 72;
    memcpy(obody, rbody, SCS_MEMBER_SCA_LEN - SCS_MEMBER_BODY_OFF); /* 132 bytes */

    /* SYSAP header: our own send-msg#, ack of the member's send-msg#. */
    put_le16(obody + 0, p->sysap_send_msg);
    put_le16(obody + 2, p->sysap_ack_msg);
    /* txn (body[4:6]) + checksum (body[6:8]) stay echoed from the request. */
    obody[8] = (uint8_t)(rbody[8] | SCS_MEMBER_RESPONSE_BIT); /* set response bit */
    /* opcode body[9] stays echoed. */
    obody[SCS_MEMBER_RESP_MARK_BODYOFF] = 0x01; /* response marker (GROUNDED 0x03+0x05+0x09) */
    /* vms-760: the echo takes exactly THREE mutations, not two. body[55] is
     * cleared as well -- GROUNDED on 6/6 responses across 5 captures and 3
     * different responder nodes (rdiff: "differs at 3 offsets: b[8], b[18],
     * b[55]", nothing else). In the requests it held 0x0e / 0x0a / 0x06, which
     * tracks the member set; whatever it means, the responder zeroes it. */
    obody[55] = 0x00;

    return 0;
}
