/*
 * dnet_engine.c - DECnet Phase IV routing engine core (rd vms-449d).
 *
 * See dnet_engine.h for the full clean-room provenance and the Rule-1 rationale
 * (the AF_PACKET socket is HIDDEN behind this VMS-faithful routing surface, the
 * same way src/vmsscs/scsd.c hides its SCA raw socket behind SCS). This file is
 * pure logic over frame buffers + an injected clock: it opens no socket, starts
 * no thread, reads no wall clock. The daemon (decnetd.c) owns the datalink.
 *
 * Field ground truth is the vms-3be endnode-HELLO specimen
 * (docs/decnet-provenance-register.md sec 4.6): src node 1.1
 * (aa:00:04:00:01:04) -> dst ab:00:00:03:00:00, ethertype 0x6003, "endnode
 * vers 2 eco 0 ueco 0 blksize 1498 rtr 0.0 hello 15 data 2".
 */
#include "dnet_engine.h"

#include <string.h>
#include <stdio.h>

/* AB-00-00-03-00-00: the Phase IV endnode-HELLO multicast (vms-3be specimen #1
 * destination; DNA-documented). Single storage definition. */
const uint8_t DNET_HELLO_MCAST[DNET_ADDR_LEN] =
    { 0xab, 0x00, 0x00, 0x03, 0x00, 0x00 };

/* The DEC HIORD prefix of a DECnet Phase IV Ethernet id: AA-00-04-00-<LE addr>.
 * (DNA Phase IV address<->id mapping; corroborated by the vms-3be specimen.) */
static const uint8_t DNET_HIORD[4] = { 0xaa, 0x00, 0x04, 0x00 };

int dnet_id_from_addr(unsigned area, unsigned node, uint8_t id_out[DNET_ADDR_LEN])
{
    if (!id_out || area > 63 || node > 1023)
        return DNET_ENGINE_EINVAL;
    uint16_t addr = (uint16_t)((area << 10) | node);
    memcpy(id_out, DNET_HIORD, 4);
    id_out[4] = (uint8_t)(addr & 0xff);         /* LE low byte  */
    id_out[5] = (uint8_t)((addr >> 8) & 0xff);  /* LE high byte */
    return DNET_ENGINE_OK;
}

const char *dnet_addr_str(uint16_t addr, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return "";
    snprintf(buf, cap, "%u.%u", dnet_area_of(addr), dnet_node_of(addr));
    return buf;
}

int dnet_engine_init(struct dnet_engine *e, unsigned area, unsigned node,
                     const char *node_name, const char *device,
                     const char *circuit, const uint8_t hw_mac[DNET_ADDR_LEN],
                     uint16_t t3, uint16_t blksize, dnet_tick_t now)
{
    if (!e || area > 63 || node > 1023)
        return DNET_ENGINE_EINVAL;

    memset(e, 0, sizeof(*e));
    e->addr = (uint16_t)((area << 10) | node);
    if (dnet_id_from_addr(area, node, e->my_id) != DNET_ENGINE_OK)
        return DNET_ENGINE_EINVAL;

    if (node_name && node_name[0]) {
        strncpy(e->node_name, node_name, DNET_NODENAME_MAX);
        e->node_name[DNET_NODENAME_MAX] = '\0';
    }
    if (device && device[0]) {
        strncpy(e->device, device, DNET_DEVNAME_MAX);
        e->device[DNET_DEVNAME_MAX] = '\0';
    } else {
        strcpy(e->device, "EWA0"); /* OVMX default datalink device label */
    }
    if (circuit && circuit[0]) {
        strncpy(e->circuit, circuit, DNET_DEVNAME_MAX);
        e->circuit[DNET_DEVNAME_MAX] = '\0';
    } else {
        /* OVMX design choice (LABELLED): derive the circuit name from the
         * device by inserting a dash before the trailing unit digits, the DEC
         * Ethernet-circuit naming shape (e.g. "EWA0" -> "EWA-0"). Not presented
         * as VMS-authentic beyond that shape. */
        size_t n = strlen(e->device);
        size_t i = n;
        while (i > 0 && e->device[i - 1] >= '0' && e->device[i - 1] <= '9')
            i--;
        if (i > 0 && i < n && i < DNET_DEVNAME_MAX) {
            memcpy(e->circuit, e->device, i);
            e->circuit[i] = '-';
            strncpy(e->circuit + i + 1, e->device + i, DNET_DEVNAME_MAX - i - 1);
            e->circuit[DNET_DEVNAME_MAX] = '\0';
        } else {
            strncpy(e->circuit, e->device, DNET_DEVNAME_MAX);
            e->circuit[DNET_DEVNAME_MAX] = '\0';
        }
    }

    if (hw_mac)
        memcpy(e->hw_mac, hw_mac, DNET_ADDR_LEN);
    e->blksize = blksize ? blksize : 1498; /* vms-3be advertised blksize */

    /* Drive our own emission cadence + the neighbour listen timers off the
     * rung-3 adjacency SM. t3=0 selects the oracle default (15 s). */
    if (dnet_adj_init(&e->adj, e->my_id, t3, 0, now) != DNET_ADJ_OK)
        return DNET_ENGINE_EINVAL;
    return DNET_ENGINE_OK;
}

/* Fill a decoded endnode-HELLO struct with THIS engine's advertised identity:
 * an endnode naming no designated router (rtr 0.0), matching the vms-3be shape. */
static void engine_fill_hello(const struct dnet_engine *e,
                              struct dnet_endnode_hello *h)
{
    memset(h, 0, sizeof(*h));
    h->rflags   = DNET_RFLAG_ENDNODE_HELLO;
    h->version  = 2;                 /* vms-3be: vers 2 */
    h->eco      = 0;
    h->user_eco = 0;
    memcpy(h->id, e->my_id, DNET_ADDR_LEN);
    h->iinfo    = DNET_NODETYPE_ENDNODE;
    h->blksize  = e->blksize;
    /* AREA field: the vms-3be oracle (node 1.1) emits 0 here, not the node's
     * area -- so a faithful endnode HELLO carries 0. Its multi-area semantics
     * are NOT grounded by that single specimen; emitting 0 is what makes our
     * HELLO byte-identical to the captured VAX wire. A multi-area capture would
     * ground a non-zero value (tracked with the live-oracle child, vms-aac0). */
    h->area     = 0;
    /* seed left zero (vms-3be). NEIGHBOR (designated router): "rtr 0.0" is
     * encoded on the wire as the DECnet id of address 0.0 -- the full HIORD
     * prefix AA-00-04-00 followed by the LE address 00:00 (aa:00:04:00:00:00),
     * NOT six zero bytes. That is exactly what the vms-3be VAX put on the wire
     * (specimen #1 offsets 24..29), and matching it makes our HELLO byte-
     * identical to the oracle. */
    (void)dnet_id_from_addr(0, 0, h->neighbor);
    h->timer    = e->adj.t3;         /* our advertised T3 (seconds) */
    h->mpd      = 0;
    h->datalen  = 2;                 /* vms-3be: 2 bytes of test data */
    h->data[0]  = 0xaa;              /* vms-3be test data bytes */
    h->data[1]  = 0xaa;
}

int dnet_engine_build_hello_frame(const struct dnet_engine *e,
                                  uint8_t *frame_out, size_t cap, size_t *len_out)
{
    if (!e || !frame_out)
        return DNET_ENGINE_EINVAL;
    if (cap < (size_t)DNET_ETH_HDRLEN + DNET_ETH_MIN_PAYLOAD)
        return DNET_ENGINE_ENOSPACE;

    /* 14-byte Ethernet II header: dst | src | ethertype (big-endian on wire). */
    memcpy(frame_out, DNET_HELLO_MCAST, DNET_ADDR_LEN);          /* dst */
    memcpy(frame_out + 6, e->my_id, DNET_ADDR_LEN);             /* src */
    frame_out[12] = (uint8_t)((DNET_ETHERTYPE >> 8) & 0xff);    /* 0x60 */
    frame_out[13] = (uint8_t)(DNET_ETHERTYPE & 0xff);          /* 0x03 */

    struct dnet_endnode_hello h;
    engine_fill_hello(e, &h);

    size_t plen = 0;
    int rc = dnet_hello_encode(&h, frame_out + DNET_ETH_HDRLEN,
                               cap - DNET_ETH_HDRLEN, &plen);
    if (rc != DNET_HELLO_OK)
        return (rc == DNET_HELLO_ENOSPACE) ? DNET_ENGINE_ENOSPACE
                                           : DNET_ENGINE_EINVAL;
    if (len_out)
        *len_out = (size_t)DNET_ETH_HDRLEN + plen;
    return DNET_ENGINE_OK;
}

int dnet_engine_rx_frame(struct dnet_engine *e, dnet_tick_t now,
                         const uint8_t *frame, size_t len,
                         uint8_t from_out[DNET_ADDR_LEN],
                         enum dnet_adj_state *state_out)
{
    if (!e || !frame)
        return DNET_ENGINE_EINVAL;
    e->frames_recv++;

    /* Need at least the Ethernet header + a HELLO length prefix. */
    if (len < (size_t)DNET_ETH_HDRLEN + DNET_HELLO_LENPREFIX) {
        e->frames_dropped++;
        return 0;
    }
    /* Ethertype gate (big-endian on the wire). */
    uint16_t etype = (uint16_t)((frame[12] << 8) | frame[13]);
    if (etype != DNET_ETHERTYPE) {
        e->frames_dropped++;
        return 0;
    }
    /* Ignore our own multicast echo: src (frame[6..11]) == our Ethernet id.
     * (An AF_PACKET SOCK_RAW socket sees frames it just sent on the segment.) */
    if (memcmp(frame + 6, e->my_id, DNET_ADDR_LEN) == 0) {
        e->frames_dropped++;
        return 0;
    }

    struct dnet_endnode_hello h;
    int rc = dnet_hello_decode(frame + DNET_ETH_HDRLEN,
                               len - DNET_ETH_HDRLEN, &h, NULL);
    if (rc != DNET_HELLO_OK) {
        e->frames_dropped++;
        return 0;
    }
    /* Only endnode-HELLO control messages drive this endnode's adjacency here
     * (router HELLOs are the sibling codec's job, a later rung). */
    if (h.rflags != DNET_RFLAG_ENDNODE_HELLO) {
        e->frames_dropped++;
        return 0;
    }

    e->hello_recv++;
    if (from_out)
        memcpy(from_out, h.id, DNET_ADDR_LEN);

    enum dnet_adj_state before = dnet_adj_state_of(&e->adj, h.id);
    enum dnet_adj_state after = before;
    if (dnet_adj_rx_hello(&e->adj, now, &h, &after) != DNET_ADJ_OK) {
        /* Table full (EFULL) or bad arg: honestly a non-advance, not a fake. */
        if (state_out)
            *state_out = before;
        return 0;
    }
    if (state_out)
        *state_out = after;
    if (after == DNET_ADJ_UP && before != DNET_ADJ_UP)
        e->adj_up_events++;
    return 1;
}

int dnet_engine_hello_due(const struct dnet_engine *e, dnet_tick_t now)
{
    if (!e)
        return 0;
    return dnet_adj_hello_due(&e->adj, now);
}

void dnet_engine_hello_emitted(struct dnet_engine *e, dnet_tick_t now)
{
    if (!e)
        return;
    dnet_adj_hello_emitted(&e->adj, now);
    e->hello_sent++;
}

int dnet_engine_tick(struct dnet_engine *e, dnet_tick_t now)
{
    if (!e)
        return DNET_ENGINE_EINVAL;
    int gone = dnet_adj_tick(&e->adj, now);
    if (gone > 0)
        e->adj_down_events += (unsigned long)gone;
    return gone;
}

/* --- NSP logical-link data frames (routing-layer carrier for NSP PDUs) ---- */

int dnet_engine_build_data_frame(const struct dnet_engine *e,
                                 const uint8_t dst_id[DNET_ADDR_LEN],
                                 const uint8_t *nsp_pdu, size_t pdu_len,
                                 uint8_t *frame_out, size_t cap, size_t *len_out)
{
    if (!e || !dst_id || (!nsp_pdu && pdu_len) || !frame_out)
        return DNET_ENGINE_EINVAL;
    size_t total = DNET_DATA_NSP_OFF + pdu_len;
    if (total > 0xffff)                       /* length prefix is 16-bit */
        return DNET_ENGINE_EINVAL;
    if (cap < total)
        return DNET_ENGINE_ENOSPACE;

    /* Ethernet II header. */
    memcpy(frame_out, dst_id, DNET_ADDR_LEN);          /* dst = peer id */
    memcpy(frame_out + 6, e->my_id, DNET_ADDR_LEN);    /* src = our id  */
    frame_out[12] = (uint8_t)((DNET_ETHERTYPE >> 8) & 0xff);
    frame_out[13] = (uint8_t)(DNET_ETHERTYPE & 0xff);

    uint8_t *p = frame_out + DNET_ETH_HDRLEN;
    /* Data-link 2-byte LE length prefix = routing header + NSP PDU (everything
     * after the prefix), matching the specimen-#3 framing. */
    uint16_t rlen = (uint16_t)(DNET_DATA_RHDR_LEN + pdu_len);
    p[0] = (uint8_t)(rlen & 0xff);
    p[1] = (uint8_t)((rlen >> 8) & 0xff);
    p += DNET_DATA_LENPREFIX;

    /* 21-byte long-data routing header. */
    p[0] = DNET_RFLAG_LONG_DATA;      /* RFLG */
    p[1] = 0; p[2] = 0;               /* destination reserved */
    memcpy(p + 3, dst_id, DNET_ADDR_LEN);       /* DSTID */
    p[9] = 0; p[10] = 0;              /* source reserved */
    memcpy(p + 11, e->my_id, DNET_ADDR_LEN);    /* SRCID */
    p[17] = 0;                        /* nextl / forwarding */
    p[18] = 0;                        /* visit count */
    p[19] = 0;                        /* service class */
    p[20] = 0;                        /* protocol type */
    p += DNET_DATA_RHDR_LEN;

    if (pdu_len)
        memcpy(p, nsp_pdu, pdu_len);

    if (len_out)
        *len_out = total;
    return DNET_ENGINE_OK;
}

int dnet_engine_parse_data_frame(const uint8_t *frame, size_t len,
                                 uint8_t src_id_out[DNET_ADDR_LEN],
                                 uint8_t dst_id_out[DNET_ADDR_LEN],
                                 const uint8_t **nsp_pdu, size_t *pdu_len)
{
    if (!frame || !nsp_pdu || !pdu_len)
        return DNET_ENGINE_EINVAL;
    if (len < DNET_DATA_NSP_OFF)
        return DNET_ENGINE_EINVAL;
    uint16_t etype = (uint16_t)((frame[12] << 8) | frame[13]);
    if (etype != DNET_ETHERTYPE)
        return DNET_ENGINE_EINVAL;

    const uint8_t *p = frame + DNET_ETH_HDRLEN;
    uint16_t rlen = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    p += DNET_DATA_LENPREFIX;
    if (p[0] != DNET_RFLAG_LONG_DATA)     /* not a long-data frame (e.g. a HELLO) */
        return DNET_ENGINE_EINVAL;
    /* The declared routing length must cover the fixed header and fit the frame. */
    if (rlen < DNET_DATA_RHDR_LEN ||
        (size_t)DNET_ETH_HDRLEN + DNET_DATA_LENPREFIX + rlen > len)
        return DNET_ENGINE_EINVAL;

    if (dst_id_out)
        memcpy(dst_id_out, p + 3, DNET_ADDR_LEN);
    if (src_id_out)
        memcpy(src_id_out, p + 11, DNET_ADDR_LEN);

    *nsp_pdu = p + DNET_DATA_RHDR_LEN;
    *pdu_len = (size_t)(rlen - DNET_DATA_RHDR_LEN);
    return DNET_ENGINE_OK;
}

/* --- single-link connection wrappers (what DECNETD drives) ---------------- */

/* Wrap an NSP message the link FSM produced into a full data frame to the peer
 * identified by the link's remote_node. */
static int engine_wrap_pdu(struct dnet_engine *e, const struct dnet_nsp_msg *msg,
                           uint8_t *frame_out, size_t cap, size_t *len_out)
{
    uint8_t pdu[DNET_NSP_MAX_DATA + 16];
    size_t pdu_len = 0;
    if (dnet_nsp_encode(msg, pdu, sizeof(pdu), &pdu_len) != DNET_NSP_OK)
        return DNET_ENGINE_EINVAL;
    uint8_t dst_id[DNET_ADDR_LEN];
    if (dnet_id_from_addr(dnet_area_of(e->link.remote_node),
                          dnet_node_of(e->link.remote_node),
                          dst_id) != DNET_ENGINE_OK)
        return DNET_ENGINE_EINVAL;
    return dnet_engine_build_data_frame(e, dst_id, pdu, pdu_len,
                                        frame_out, cap, len_out);
}

int dnet_engine_link_open(struct dnet_engine *e,
                          unsigned remote_area, unsigned remote_node,
                          uint16_t local_lla,
                          const uint8_t *conn_data, size_t conn_len,
                          uint16_t segsize, uint8_t services, uint8_t info,
                          uint8_t *frame_out, size_t cap, size_t *len_out,
                          dnet_tick_t now)
{
    if (!e || !frame_out || remote_area > 63 || remote_node > 1023)
        return DNET_ENGINE_EINVAL;
    if (e->link_active)
        return DNET_ENGINE_EINVAL;   /* one active link per instance (rung-2 scope) */
    uint16_t rnode = (uint16_t)((remote_area << 10) | remote_node);
    if (dnet_link_init(&e->link, local_lla, rnode, now) != DNET_LINK_OK)
        return DNET_ENGINE_EINVAL;

    struct dnet_nsp_msg ci;
    if (dnet_link_connect(&e->link, conn_data, conn_len, segsize, services, info,
                          &ci, now) != DNET_LINK_OK)
        return DNET_ENGINE_EINVAL;
    e->link_active = 1;
    return engine_wrap_pdu(e, &ci, frame_out, cap, len_out);
}

int dnet_engine_link_accept(struct dnet_engine *e, uint16_t local_lla,
                            uint8_t *frame_out, size_t cap, size_t *len_out,
                            dnet_tick_t now)
{
    if (!e || !frame_out || !e->link_active)
        return DNET_ENGINE_EINVAL;
    /* If the inbound CI arrived on a fresh link, adopt our chosen logical-link
     * address before confirming. */
    if (e->link.local_addr == 0)
        e->link.local_addr = local_lla;
    struct dnet_nsp_msg cc;
    if (dnet_link_accept(&e->link, &cc, now) != DNET_LINK_OK)
        return DNET_ENGINE_EINVAL;
    return engine_wrap_pdu(e, &cc, frame_out, cap, len_out);
}

int dnet_engine_link_send(struct dnet_engine *e, const uint8_t *data, size_t len,
                          uint8_t *frame_out, size_t cap, size_t *len_out,
                          dnet_tick_t now)
{
    if (!e || !frame_out || !e->link_active)
        return DNET_ENGINE_EINVAL;
    struct dnet_nsp_msg d;
    if (dnet_link_send_data(&e->link, data, len, &d, now) != DNET_LINK_OK)
        return DNET_ENGINE_EINVAL;
    return engine_wrap_pdu(e, &d, frame_out, cap, len_out);
}

int dnet_engine_link_close(struct dnet_engine *e, uint16_t reason,
                           uint8_t *frame_out, size_t cap, size_t *len_out,
                           dnet_tick_t now)
{
    if (!e || !frame_out || !e->link_active)
        return DNET_ENGINE_EINVAL;
    struct dnet_nsp_msg di;
    if (dnet_link_disconnect(&e->link, reason, &di, now) != DNET_LINK_OK)
        return DNET_ENGINE_EINVAL;
    return engine_wrap_pdu(e, &di, frame_out, cap, len_out);
}

int dnet_engine_link_rx(struct dnet_engine *e, dnet_tick_t now,
                        const uint8_t *frame, size_t len,
                        uint8_t *reply_frame, size_t cap, size_t *reply_len,
                        int *has_reply, enum dnet_link_event *event)
{
    if (!e || !frame)
        return DNET_ENGINE_EINVAL;
    if (has_reply)
        *has_reply = 0;
    if (event)
        *event = DNET_LINK_EV_NONE;

    uint8_t src_id[DNET_ADDR_LEN];
    const uint8_t *pdu = NULL;
    size_t pdu_len = 0;
    if (dnet_engine_parse_data_frame(frame, len, src_id, NULL, &pdu, &pdu_len)
            != DNET_ENGINE_OK) {
        e->nsp_frames_dropped++;
        return DNET_ENGINE_OK;
    }
    struct dnet_nsp_msg in;
    if (dnet_nsp_decode(pdu, pdu_len, &in, NULL) != DNET_NSP_OK) {
        e->nsp_frames_dropped++;
        return DNET_ENGINE_OK;
    }

    /* A Connect Initiate may open a fresh link on this engine. */
    if (in.type == DNET_NSP_T_CI && !e->link_active) {
        dnet_link_init(&e->link, 0, dnet_addr_from_id(src_id), now);
        e->link_active = 1;
    }
    if (!e->link_active) {
        e->nsp_frames_dropped++;
        return DNET_ENGINE_OK;
    }
    /* Keep the link's routing peer in sync with the frame we heard it on. */
    if (in.type == DNET_NSP_T_CI)
        e->link.remote_node = dnet_addr_from_id(src_id);

    e->nsp_frames_recv++;
    struct dnet_nsp_msg reply;
    int rep = 0;
    enum dnet_link_event ev = DNET_LINK_EV_NONE;
    dnet_link_rx(&e->link, &in, now, &reply, &rep, &ev);

    if (ev == DNET_LINK_EV_DATA) {
        e->rx_datalen = (in.datalen > DNET_NSP_MAX_DATA)
                            ? DNET_NSP_MAX_DATA : in.datalen;
        if (e->rx_datalen)
            memcpy(e->rx_data, in.data, e->rx_datalen);
    }
    if (event)
        *event = ev;

    if (rep && reply_frame) {
        int rc = engine_wrap_pdu(e, &reply, reply_frame, cap, reply_len);
        if (rc == DNET_ENGINE_OK && has_reply)
            *has_reply = 1;
    }
    return DNET_ENGINE_OK;
}

/* --- VMS-faithful presentation surface ---------------------------------- */

static const char *state_name(enum dnet_adj_state s)
{
    switch (s) {
    case DNET_ADJ_UP:           return "up";
    case DNET_ADJ_INITIALIZING: return "initializing";
    case DNET_ADJ_DOWN:
    default:                    return "down";
    }
}

void dnet_engine_show_executor(const struct dnet_engine *e, FILE *out)
{
    if (!e || !out)
        return;
    char a[8];
    fprintf(out, "Node Volatile Summary\n");
    fprintf(out, "Executor node = %s (%s)\n",
            dnet_addr_str(e->addr, a, sizeof(a)),
            e->node_name[0] ? e->node_name : "");
    fprintf(out, "State                    = on\n");
    /* INV-0: OVMX-branded identification -- never claims "DECnet for OpenVMS". */
    fprintf(out, "Identification           = OVMX DECnet-compatible networking\n");
    fprintf(out, "Type                     = nonrouting IV (endnode)\n");
}

void dnet_engine_show_circuit(const struct dnet_engine *e, FILE *out)
{
    if (!e || !out)
        return;
    char a[8];
    fprintf(out, "Known Circuit Volatile Summary\n");
    fprintf(out, "Circuit = %s\n", e->circuit);
    fprintf(out, "State                    = on\n");
    fprintf(out, "Designated router        = none\n");
    fprintf(out, "Hello timer              = %u\n", (unsigned)e->adj.t3);
    fprintf(out, "Adjacent nodes           = %zu\n", e->adj.count);
    (void)a;
}

void dnet_engine_show_adjacent(const struct dnet_engine *e, FILE *out)
{
    if (!e || !out)
        return;
    char a[8];
    fprintf(out, "Adjacent Node Volatile Summary\n");
    fprintf(out, "Executor node = %s (%s)\n",
            dnet_addr_str(e->addr, a, sizeof(a)),
            e->node_name[0] ? e->node_name : "");
    fprintf(out, "    Node            State         Circuit\n");
    for (size_t i = 0; i < DNET_ADJ_MAX_NEIGHBORS; i++) {
        const struct dnet_adj_neighbor *n = &e->adj.nbr[i];
        if (!n->in_use)
            continue;
        fprintf(out, " %-14s  %-12s  %s\n",
                dnet_addr_str(n->addr, a, sizeof(a)),
                state_name(n->state), e->circuit);
    }
}
