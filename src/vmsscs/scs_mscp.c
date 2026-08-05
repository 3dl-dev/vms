/*
 * scs_mscp.c - MSCP-over-SCS disk-client command builders/parser (vms-760).
 * See scs_mscp.h for the full clean-room provenance and the GROUNDED-vs-REPLAYED
 * field breakdown.
 *
 * Templates below are byte-exact 94-byte SCA-content captures of the golden
 * JOINER MSCP command frames from af2-firsttimer-established-20260728.pcap (a
 * first-timer joining an established VAX1). Substituted at build time (GROUNDED
 * positions only): dst logical [2:8], src logical [10:16], the SCS counters
 * [18:20]/[20:22] + their mirrors, the incarnation echo [22:24], the Con.ID pair
 * [50:54]/[54:58], and the MSCP command-reference-number (body[0:4]) + unit word
 * (body[4:6]). Every other byte -- the MSCP opcode/flags/modifiers and the
 * parameter region -- is a labeled REPLAY of the captured joiner command.
 */
#include "scs_mscp.h"

#include <string.h>

/* SET CONTROLLER CHARACTERISTICS (opcode 0x04) -- golden joiner SCC command
 * (af2 rel~143.89574). body param region [12:36] = MSCP-version/controller-flags
 * + the 8-byte host time/id quadword c00b28751902bc00 + zero pad (REPLAYED). */
static const uint8_t mscp_scc_tmpl[SCS_MSCP_SCA_LEN] = {
    0x5c, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x00, 0xaa, 0x00,
    0x04, 0x00, 0x1a, 0x04, 0x4b, 0x13, 0x18, 0x00, 0x19, 0x00, 0x01, 0x00,
    0x12, 0x00, 0x18, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x18, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x32, 0x00, 0x04, 0x00, 0x0a, 0x00,
    0x01, 0x00, 0x0a, 0x00, 0x54, 0x35, 0x08, 0x00, 0xd2, 0x8f, 0x02, 0x00,
    0xa3, 0x81, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xd0, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x0b, 0x28, 0x75, 0x19, 0x02,
    0xbc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* GET UNIT STATUS (opcode 0x03, NEXT-UNIT modifier) -- golden joiner GUS command
 * (af2 rel~143.89732). body param region [12:36] = all zeros (REPLAYED). */
static const uint8_t mscp_gus_tmpl[SCS_MSCP_SCA_LEN] = {
    0x5c, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x00, 0xaa, 0x00,
    0x04, 0x00, 0x1a, 0x04, 0x4b, 0x13, 0x1a, 0x00, 0x1b, 0x00, 0x01, 0x00,
    0x12, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0x1a, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x32, 0x00, 0x04, 0x00, 0x0a, 0x00,
    0x01, 0x00, 0x0a, 0x00, 0x54, 0x35, 0x08, 0x00, 0xd2, 0x8f, 0x01, 0x00,
    0xe2, 0x7e, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

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
 * build_common - lay down the Ethernet header + a 94-byte SCA template, then
 * substitute the shared SCS envelope (dst/src logical, live counters + mirrors,
 * incarnation echo), the Con.ID pair, and the MSCP command-reference-number
 * (class token + message-id) and unit word. The opcode/flags/modifiers and the
 * parameter region stay as the captured template. SCA-content offset == out+14.
 */
static void build_common(const struct scs_mscp_params *p, const uint8_t *tmpl,
                         uint8_t *out)
{
    /* Ethernet header (abs 0-13). */
    memcpy(out + 0, p->dst_mac, 6);
    memcpy(out + 6, p->src_mac, 6);
    out[12] = 0x60;
    out[13] = 0x07;

    /* SCA content (abs 14+) from the captured joiner template. */
    memcpy(out + 14, tmpl, SCS_MSCP_SCA_LEN);

    /* Identity substitutions (SCA-content offsets + 14). */
    memcpy(out + 14 + 2, p->peer_logical, 6);  /* dest logical [2:8]  (abs 16) */
    memcpy(out + 14 + 10, p->src_logical, 6);  /* src-logical [10:16] (abs 24) */

    /* SCS sequenced-message counters (spec sec 4h): recv_ack at [18:20] repeated
     * at [26:28]/[34:36]; send_seq at [20:22] mirrored at [30:32]. The
     * incarnation echo at [22:24] (established-join gate, sec 4i.B): 0 leaves the
     * template's fresh-contact value 1. */
    put_le16(out + 14 + 18, p->recv_ack);
    put_le16(out + 14 + 20, p->send_seq);
    if (p->incarnation != 0) {
        put_le16(out + 14 + 22, p->incarnation);
    }
    put_le16(out + 14 + 26, p->recv_ack);
    put_le16(out + 14 + 30, p->send_seq);
    put_le16(out + 14 + 34, p->recv_ack);

    /* Con.ID pair: remote at [50:54] (abs 64), local at [54:58] (abs 68). */
    put_le32(out + 14 + 50, p->remote_conid);
    put_le32(out + 14 + 54, p->local_conid);

    /* MSCP command-reference-number: class token body[0:2] + message-id body[2:4]
     * (body[0] = SCA offset 58). VAX1 echoes both verbatim in its END. */
    put_le16(out + 14 + SCS_MSCP_BODY_OFF + 0, p->class_token);
    put_le16(out + 14 + SCS_MSCP_BODY_OFF + 2, p->msg_id);
    /* MSCP unit-number word body[4:6] (GUS enumeration unit; 0 for SCC). */
    put_le16(out + 14 + SCS_MSCP_BODY_OFF + 4, p->unit);
}

int scs_mscp_build_scc(const struct scs_mscp_params *p,
                       uint8_t out[SCS_MSCP_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    build_common(p, mscp_scc_tmpl, out);
    return 0;
}

int scs_mscp_build_gus(const struct scs_mscp_params *p,
                       uint8_t out[SCS_MSCP_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    build_common(p, mscp_gus_tmpl, out);
    return 0;
}

int scs_mscp_parse(const uint8_t *frame, size_t len, struct scs_mscp_view *v)
{
    if (frame == NULL || v == NULL) {
        return -1;
    }
    /* Need the Ethernet header + SCA envelope through the MSCP status word:
     * abs 72 body + body[10:12] = abs 84. */
    if (len < 84) {
        return -1;
    }

    memset(v, 0, sizeof(*v));
    uint16_t lenword = get_le16(frame + 14);
    v->total_sca_len = (uint16_t)(lenword + 2);
    v->msgtype = frame[30];
    v->format = frame[31];
    v->recv_ack = get_le16(frame + 32);
    v->send_seq = get_le16(frame + 34);
    v->remote_conid = get_le32(frame + 64);
    v->local_conid = get_le32(frame + 68);

    const uint8_t *body = frame + 72; /* MSCP body[0] = abs 72 */
    v->class_token = get_le16(body + 0);
    v->msg_id = get_le16(body + 2);
    v->unit = get_le16(body + 4);
    v->opcode = body[8];
    v->status = get_le16(body + 10);
    v->is_end = (v->opcode & SCS_MSCP_END_BIT) ? 1 : 0;
    return 0;
}
