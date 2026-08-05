/*
 * scs_rx.c - the receive-side SCS header decoder. See scs_rx.h for the source
 * cites, the measured MTYPE census and the honest statement about datagrams.
 */
#include "scs_rx.h"

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *scs_rx_kind_name(int kind)
{
    switch (kind) {
    case SCS_RX_CONTROL:       return "control";
    case SCS_RX_APP_MESSAGE:   return "app-message";
    case SCS_RX_UNKNOWN_MTYPE: return "unknown-mtype";
    default:                   return "?";
    }
}

int scs_rx_parse(const uint8_t *content, size_t len, struct scs_rx_hdr *out)
{
    if (content == NULL || out == NULL) {
        return -1;
    }
    if (len < SCS_RX_HDR_END) {
        return -1;
    }

    uint16_t total = (uint16_t)(get_le16(content) + 2u);
    if ((size_t)total > len || total < SCS_RX_HDR_END) {
        return -1;
    }

    /* The sec 4(h)(1b) envelope test. Both halves are load-bearing: the
     * 70-content class satisfies neither (its [42:44] is 9..13, never
     * total-44, and its [44:46] is 0x522f / 0x532f / 0x2abe / ...), which is
     * exactly what sec 4(h)(1d) requires this parser to reject. */
    if (get_le16(content + SCS_RX_OFF_FORMAT) != SCS_RX_FORMAT_WORD) {
        return -1;
    }
    uint16_t inner = get_le16(content + SCS_RX_OFF_INNER_LEN);
    if (inner != (uint16_t)(total - 44u)) {
        return -1;
    }

    out->total_sca_len = total;
    out->inner_len = inner;
    out->mtype = get_le16(content + SCS_RX_OFF_MTYPE);
    out->credit = get_le16(content + SCS_RX_OFF_CREDIT);
    out->dest_conid = get_le32(content + SCS_RX_OFF_DEST_CONID);
    out->src_conid = get_le32(content + SCS_RX_OFF_SRC_CONID);
    out->payload_len = (size_t)total - SCS_RX_HDR_END;
    out->payload = (out->payload_len > 0) ? (content + SCS_RX_HDR_END) : NULL;

    if (out->mtype == SCS_RX_MTYPE_APP_MESSAGE) {
        out->kind = SCS_RX_APP_MESSAGE;
    } else if (out->mtype <= SCS_RX_MTYPE_CONTROL_MAX) {
        out->kind = SCS_RX_CONTROL;
    } else {
        /* Never observed. NOT a datagram -- see scs_rx.h. */
        out->kind = SCS_RX_UNKNOWN_MTYPE;
    }
    return 0;
}
