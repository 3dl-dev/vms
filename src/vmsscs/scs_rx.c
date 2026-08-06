/*
 * scs_rx.c - the receive-side SCS header decoder. See scs_rx.h for the source
 * cites, the measured MTYPE census and the honest statement about datagrams.
 *
 * vms-ec7: the decode itself now lives in scs_env.c, which the SEND side shares.
 * What remains here is the receive-side VIEW of it -- struct scs_rx_hdr and the
 * enum scs_rx_kind names its callers use. The mapping below is written out
 * value by value rather than cast, so that a future change to either enum is a
 * compile-time decision instead of a silent renumbering.
 */
#include "scs_rx.h"

#include "scs_env.h"

const char *scs_rx_kind_name(int kind)
{
    switch (kind) {
    case SCS_RX_CONTROL:       return "control";
    case SCS_RX_APP_MESSAGE:   return "app-message";
    case SCS_RX_UNKNOWN_MTYPE: return "unknown-mtype";
    default:                   return "?";
    }
}

static int kind_for_route(int route)
{
    switch (route) {
    case SCS_ENV_ROUTE_CONTROL: return SCS_RX_CONTROL;
    case SCS_ENV_ROUTE_MESSAGE: return SCS_RX_APP_MESSAGE;
    case SCS_ENV_ROUTE_UNKNOWN:
    default:                    return SCS_RX_UNKNOWN_MTYPE;
    }
}

int scs_rx_parse(const uint8_t *content, size_t len, struct scs_rx_hdr *out)
{
    struct scs_env e;

    if (out == NULL) {
        return -1;
    }
    if (scs_env_parse(content, len, &e) != 0) {
        return -1;
    }

    out->total_sca_len = e.total_sca_len;
    out->inner_len = e.inner_len;
    out->mtype = e.mtype;
    out->credit = e.credit;
    out->dest_conid = e.dest_conid;
    out->src_conid = e.src_conid;
    out->payload = e.payload;
    out->payload_len = e.payload_len;
    out->kind = kind_for_route(e.route);
    return 0;
}
