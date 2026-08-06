/*
 * scs_env.c - the SCS message envelope: one build path, one parse path.
 * See scs_env.h for the source cites, the conformance test and the list of
 * classes that are deliberately NOT envelope messages.
 */
#include "scs_env.h"

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int scs_env_route_for_mtype(unsigned mtype)
{
    if (mtype == SCS_ENV_MTYPE_APP_MESSAGE) {
        return SCS_ENV_ROUTE_MESSAGE;
    }
    if (mtype <= SCS_ENV_MTYPE_CONTROL_MAX) {
        return SCS_ENV_ROUTE_CONTROL;
    }
    /* Never observed. NOT a datagram -- see scs_env.h. */
    return SCS_ENV_ROUTE_UNKNOWN;
}

const char *scs_env_mtype_name(unsigned mtype)
{
    switch (mtype) {
    case SCS_ENV_MTYPE_CONNECT_REQ:    return "CONNECT_REQ";
    case SCS_ENV_MTYPE_CONNECT_RSP:    return "CONNECT_RSP";
    case SCS_ENV_MTYPE_ACCEPT_REQ:     return "ACCEPT_REQ";
    case SCS_ENV_MTYPE_ACCEPT_RSP:     return "ACCEPT_RSP";
    case SCS_ENV_MTYPE_REJECT_REQ:     return "REJECT_REQ";
    case SCS_ENV_MTYPE_REJECT_RSP:     return "REJECT_RSP";
    case SCS_ENV_MTYPE_DISCONNECT_REQ: return "DISCONNECT_REQ";
    case SCS_ENV_MTYPE_DISCONNECT_RSP: return "DISCONNECT_RSP";
    /* 8 and 9 are observed and UNIDENTIFIED (vms-f03). Deliberately not named:
     * a log line reading "SPECIAL_CREDIT" would put a guess in the record. */
    case SCS_ENV_MTYPE_T8:             return "type 8";
    case SCS_ENV_MTYPE_T9:             return "type 9";
    case SCS_ENV_MTYPE_APP_MESSAGE:    return "APP_MESSAGE";
    default:                           return "?";
    }
}

const char *scs_env_route_name(int route)
{
    switch (route) {
    case SCS_ENV_ROUTE_CONTROL: return "control";
    case SCS_ENV_ROUTE_MESSAGE: return "app-message";
    case SCS_ENV_ROUTE_UNKNOWN: return "unknown-mtype";
    default:                    return "?";
    }
}

int scs_env_build(uint8_t *content, size_t sca_len,
                  const struct scs_env_fields *f)
{
    if (content == NULL || f == NULL) {
        return -1;
    }
    if (sca_len < SCS_ENV_HDR_END || sca_len > 0xFFFFu) {
        return -1;
    }

    /* DERIVED, never copied: the one field a template can be stale about. */
    put_le16(content + SCS_ENV_OFF_INNER_LEN,
             (uint16_t)(sca_len - SCS_ENV_INNER_LEN_BIAS));
    /* A constant on every envelope-conformant frame in the corpus, and half of
     * the conformance test scs_env_parse() applies on the way back in. */
    put_le16(content + SCS_ENV_OFF_FORMAT, (uint16_t)SCS_ENV_FORMAT_WORD);

    put_le16(content + SCS_ENV_OFF_MTYPE, f->mtype);
    put_le16(content + SCS_ENV_OFF_CREDIT, f->credit);
    put_le32(content + SCS_ENV_OFF_DEST_CONID, f->dest_conid);
    put_le32(content + SCS_ENV_OFF_SRC_CONID, f->src_conid);
    return 0;
}

int scs_env_build_frame(uint8_t *frame, size_t frame_len,
                        const struct scs_env_fields *f)
{
    if (frame == NULL || frame_len <= SCS_ENV_ETH_HDR_LEN) {
        return -1;
    }
    return scs_env_build(frame + SCS_ENV_ETH_HDR_LEN,
                         frame_len - SCS_ENV_ETH_HDR_LEN, f);
}

int scs_env_parse(const uint8_t *content, size_t len, struct scs_env *out)
{
    uint16_t total;
    uint16_t inner;

    if (content == NULL || out == NULL) {
        return -1;
    }
    if (len < SCS_ENV_HDR_END) {
        return -1;
    }

    total = (uint16_t)(get_le16(content) + 2u);
    if ((size_t)total > len || total < SCS_ENV_HDR_END) {
        return -1;
    }

    /* THE CONFORMANCE TEST (spec sec 4(h)(1b)). Both halves are load-bearing:
     * the 70-content class satisfies neither, which is exactly what sec
     * 4(h)(1d) requires this parser to reject. */
    if (get_le16(content + SCS_ENV_OFF_FORMAT) != SCS_ENV_FORMAT_WORD) {
        return -1;
    }
    inner = get_le16(content + SCS_ENV_OFF_INNER_LEN);
    if (inner != (uint16_t)(total - SCS_ENV_INNER_LEN_BIAS)) {
        return -1;
    }

    out->total_sca_len = total;
    out->inner_len = inner;
    out->mtype = get_le16(content + SCS_ENV_OFF_MTYPE);
    out->credit = get_le16(content + SCS_ENV_OFF_CREDIT);
    out->dest_conid = get_le32(content + SCS_ENV_OFF_DEST_CONID);
    out->src_conid = get_le32(content + SCS_ENV_OFF_SRC_CONID);
    out->payload_len = (size_t)total - SCS_ENV_HDR_END;
    out->payload = (out->payload_len > 0) ? (content + SCS_ENV_HDR_END) : NULL;
    out->route = scs_env_route_for_mtype(out->mtype);
    return 0;
}

int scs_env_parse_frame(const uint8_t *frame, size_t len, struct scs_env *out)
{
    if (frame == NULL || len <= SCS_ENV_ETH_HDR_LEN) {
        return -1;
    }
    return scs_env_parse(frame + SCS_ENV_ETH_HDR_LEN,
                         len - SCS_ENV_ETH_HDR_LEN, out);
}

int scs_env_mtype_of_frame(const uint8_t *frame, size_t len, uint16_t *out)
{
    struct scs_env e;

    if (out == NULL) {
        return 0;
    }
    if (scs_env_parse_frame(frame, len, &e) != 0) {
        return 0;
    }
    *out = e.mtype;
    return 1;
}
