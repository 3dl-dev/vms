/*
 * scs_connect.c - VMS$VAXcluster SCS connect handshake builder/parser
 * (vms-5fe). See scs_connect.h for the full clean-room provenance and the
 * GROUNDED-vs-REPLAYED field breakdown.
 */
#include "scs_connect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- vms-fdd: SCA connect data (p. 2-25 / p. 2-28) -----------------------
 *
 * Byte-exact to the joiner's VMS$VAXcluster connect data in
 * vax3-2to3-established-join-20260730.pcap -- raw frame 132 (VAX3's
 * CONNECT_REQ) and raw frame 210 (VAX3's ACCEPT_REQ), which carry the SAME 16
 * bytes. That capture is the library's only recording of a real node being
 * admitted to an already-running cluster, i.e. the operation OVMX performs.
 * The full census, the two invariant spans and the honest gap over [98:105]
 * are in the CONNECT DATA verdict in scs_connect.h; re-derive with
 * tools/scs_connect_data_measure.py.
 *
 * NOT invented, NOT an OVMX design choice: every byte below was observed. */
const uint8_t scs_connect_data_vaxcluster[SCS_CONNECT_DATA_LEN] = {
    /* [0:4]  version quad, 148/148 VAX-sourced VMS$VAXcluster connect
     * frames (OVMX's own 55 excluded -- see the guard in scs_connect.h) */
    0x01, 0x1b, 0x01, 0x03,
    /* [4:11] the joiner form (all-zero); what it encodes is an RE gap */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* [11:16] tail, 148/148 VAX-sourced */
    0x08, 0x00, 0x00, 0x06, 0x00
};

static int g_connect_data_enabled = -1;

void scs_connect_data_reset_switch_cache(void)
{
    g_connect_data_enabled = -1;
}

int scs_connect_data_enabled(void)
{
    if (g_connect_data_enabled < 0) {
        const char *v = getenv("OVMX_NO_CONNECT_DATA");
        g_connect_data_enabled = (v != NULL && v[0] == '1' && v[1] == '\0') ? 0 : 1;
    }
    return g_connect_data_enabled;
}

/* Byte-exact 110-byte SCA content of a real CONNECT-REQUEST
 * (formation-ci1-joinwindow.pcap raw frame 47 / SCA#39, VAX1->VAX2).
 * Substituted at build time: dest logical [2:8], src logical [10:16],
 * remote Con.ID [50:54], local Con.ID [54:58]. Every other byte is a
 * REPLAY of the captured frame (see header note). */
static const uint8_t connect_request_tmpl[SCS_CONNECT_SCA_LEN] = {
    /* [0:2]   */ 0x6c, 0x00,
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* dest logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x4b, 0x13,                               /* msgtype 0x4b, format 0x13 (GROUNDED) */
    /* [18:26] */ 0x06, 0x00, 0x07, 0x00, 0x01, 0x00, 0x12, 0x00,
    /* [26:34] */ 0x06, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
    /* [34:42] */ 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02,
    /* [42:50] */ 0x42, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0a, 0x00,
    /* [50:54] */ 0x00, 0x00, 0x00, 0x00,                   /* remote Con.ID (SUBST, 0 for REQUEST) */
    /* [54:58] */ 0x09, 0x00, 0xc5, 0x62,                   /* local Con.ID (SUBST) */
    /* [58:62] */ 0x00, 0x00, 0x01, 0x00,
    /* [62:78] */ 'V','M','S','$','V','A','X','c','l','u','s','t','e','r',' ',' ',
    /* [78:94] */ 'V','M','S','$','V','A','X','c','l','u','s','t','e','r',' ',' ',
    /* [94:98] */ 0x01, 0x1b, 0x01, 0x03,
    /* [98:110]*/ 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x08, 0x00, 0x00, 0x06, 0x00
};

/* Byte-exact 110-byte SCA content of a real CONNECT-RESPONSE/ACCEPT
 * (formation-ci1-joinwindow.pcap raw frame 50 / SCA#42, VAX2->VAX1).
 * Same substitution scheme. Differs from the request template in the
 * (replayed) SCS sequence counters, the connect-state bytes, and the
 * trailing body -- all ungrounded, all replayed. */
static const uint8_t connect_response_tmpl[SCS_CONNECT_SCA_LEN] = {
    /* [0:2]   */ 0x6c, 0x00,
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* dest logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x4b, 0x13,                               /* msgtype 0x4b, format 0x13 (GROUNDED) */
    /* [18:26] */ 0x07, 0x00, 0x08, 0x00, 0x01, 0x00, 0x12, 0x00,
    /* [26:34] */ 0x07, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    /* [34:42] */ 0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02,
    /* [42:50] */ 0x42, 0x00, 0x04, 0x00, 0x02, 0x00, 0x0a, 0x00,
    /* [50:54] */ 0x09, 0x00, 0xc5, 0x62,                   /* remote Con.ID (SUBST, echoed peer's) */
    /* [54:58] */ 0x08, 0x00, 0x58, 0x33,                   /* local Con.ID (SUBST) */
    /* [58:62] */ 0x00, 0x00, 0x00, 0x00,
    /* [62:78] */ 'V','M','S','$','V','A','X','c','l','u','s','t','e','r',' ',' ',
    /* [78:94] */ 'V','M','S','$','V','A','X','c','l','u','s','t','e','r',' ',' ',
    /* [94:98] */ 0x01, 0x1b, 0x01, 0x03,
    /* [98:110]*/ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x06, 0x00
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

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static int build_from_tmpl(const struct scs_connect_params *p,
                           const uint8_t tmpl[SCS_CONNECT_SCA_LEN],
                           uint32_t remote_conid,
                           uint8_t out[SCS_CONNECT_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }

    /* Ethernet header (abs 0-13). */
    memcpy(out + 0, p->dst_mac, 6);
    memcpy(out + 6, p->src_mac, 6);
    out[12] = 0x60;
    out[13] = 0x07;

    /* SCA content (abs 14-123) from the captured template. */
    memcpy(out + 14, tmpl, SCS_CONNECT_SCA_LEN);

    /* Substitute the identity + Con.ID fields (GROUNDED positions). */
    memcpy(out + 14 + 2, p->peer_logical, 6);  /* dest logical (abs 16) */
    memcpy(out + 14 + 10, p->src_logical, 6);   /* src-logical (abs 24) = aa:00:04:00:<sysid>
                                                 * cluster-LOGICAL addr, NOT raw HW MAC (vms-9f3) */
    put_le32(out + 14 + 50, remote_conid);      /* Remote Con.ID (abs 64) */
    put_le32(out + 14 + 54, p->local_conid);    /* Local  Con.ID (abs 68) */

    /* --- vms-c6d: thread the LIVE SCS VC counters (spec sec 4h(4)), replacing
     * the golden template's replayed 6/7 (request) / 7/8 (response). recv_ack at
     * [18:20] repeated at [26:28] and [34:36]; send_seq at [20:22] mirrored at
     * [30:32] (the [20:22]==[30:32] mirror is GROUNDED 17758/17758 frames). The
     * node-incarnation echo at [22:24] is the established-join gate (sec 4i.B),
     * echoing the value the member advertised in its directed-HELLO [78:80]; a 0
     * leaves the template's fresh-contact value 1, preserving byte-exact golden
     * reproduction for the fresh-formation path and the unit tests. Payload
     * offsets are +14 for absolute (payload byte 0 = abs frame 14). */
    put_le16(out + 14 + 18, p->recv_ack);       /* leading counter [18:20] */
    put_le16(out + 14 + 20, p->send_seq);       /* send-seq        [20:22] */
    if (p->incarnation != 0) {
        put_le16(out + 14 + 22, p->incarnation); /* node-incarnation [22:24] */
    }
    put_le16(out + 14 + 26, p->recv_ack);       /* recv_ack mirror [26:28] */
    put_le16(out + 14 + 30, p->send_seq);       /* send-seq mirror [30:32] (== [20:22], GROUNDED) */
    put_le16(out + 14 + 34, p->recv_ack);       /* recv_ack 3rd    [34:36] */

    /* --- vms-fdd: stamp the SCA connect data at [94:110] (abs 108-123).
     * Before this the region was a labeled REPLAY of whichever golden frame
     * the template came from; the CONNECT-REQUEST template is VAX1's, an
     * established MEMBER's frame, so OVMX -- a joiner -- was presenting a
     * member's connect data. Stamping the measured joiner value fixes that
     * for the request and is a no-op for the response (whose template is
     * VAX2's joiner frame and already carries these bytes).
     * OVMX_NO_CONNECT_DATA=1 skips the stamp, restoring the template bytes. */
    if (scs_connect_data_enabled()) {
        memcpy(out + SCS_CONNECT_DATA_ABS_OFF, scs_connect_data_vaxcluster,
               SCS_CONNECT_DATA_LEN);
    }

    return 0;
}

int scs_connect_build_request(const struct scs_connect_params *p,
                              uint8_t out[SCS_CONNECT_FRAME_LEN])
{
    /* CONNECT-REQUEST: remote Con.ID is always 0 (peer's not yet known). */
    return build_from_tmpl(p, connect_request_tmpl, 0, out);
}

int scs_connect_build_response(const struct scs_connect_params *p,
                               uint8_t out[SCS_CONNECT_FRAME_LEN])
{
    if (p == NULL) {
        return -1;
    }
    /* CONNECT-RESPONSE: echo the peer's Con.ID as remote. */
    return build_from_tmpl(p, connect_response_tmpl, p->remote_conid, out);
}

int scs_connect_parse(const uint8_t *frame, size_t len, struct scs_connect_view *v)
{
    if (frame == NULL || v == NULL) {
        return -1;
    }
    /* Need at least the Ethernet header + SCA length + envelope msgtype/format
     * (abs 30-31 = frame index 30-31, so >= 32 bytes). */
    if (len < 32) {
        return -1;
    }

    memset(v, 0, sizeof(*v));

    uint16_t lenword = (uint16_t)(frame[14] | ((uint16_t)frame[15] << 8));
    v->total_sca_len = (uint16_t)(lenword + 2);
    v->msgtype = frame[30];
    v->format = frame[31];

    /* Con.ID pair (abs 64/68) is only grounded for the 110- and 190-byte
     * classes (spec sec 4d/4g); require the frame actually holds those bytes. */
    if ((v->total_sca_len == 110 || v->total_sca_len == 190) && len >= 72) {
        v->has_conid = 1;
        v->remote_conid = get_le32(frame + 64);
        v->local_conid = get_le32(frame + 68);
    }

    /* [46:48] = abs 60:62, the SCA connection-control message type (spec sec
     * 4h(1a)). Filled whenever the frame is long enough to hold it. */
    if (len >= 62) {
        v->conn_msgtype = (uint16_t)(frame[60] | ((uint16_t)frame[61] << 8));
    }

    /* vms-fdd: the peer's connect data. Claimed ONLY for the population it is
     * grounded over -- the 110-byte class, message type CONNECT_REQ or
     * ACCEPT_REQ. The same 110-byte class ALSO carries message type 10, whose
     * [62:78] is binary rather than a SYSAP name and which is not a connect
     * frame, so a length test alone would over-claim. */
    if (scs_connect_data_get(frame, len, v->connect_data) == 0) {
        v->has_connect_data = 1;
    }

    return 0;
}

int scs_connect_data_get(const uint8_t *frame, size_t len,
                         uint8_t out[SCS_CONNECT_DATA_LEN])
{
    if (frame == NULL || out == NULL || len < SCS_CONNECT_FRAME_LEN) {
        return -1;
    }
    /* SCA length word (abs 14) -> total SCA bytes; must be the 110-byte class. */
    uint16_t lenword = (uint16_t)(frame[14] | ((uint16_t)frame[15] << 8));
    if ((uint16_t)(lenword + 2) != SCS_CONNECT_SCA_LEN) {
        return -1;
    }
    /* format constant 0x13 (GROUNDED) + an SCS-message opcode. */
    if (frame[31] != SCS_FORMAT_CONST) {
        return -1;
    }
    if (frame[30] != SCS_MSGTYPE_SEQAPP && frame[30] != 0x5b && frame[30] != 0x7b) {
        return -1;
    }
    uint16_t cmsg = (uint16_t)(frame[60] | ((uint16_t)frame[61] << 8));
    if (cmsg != SCS_CONN_MSGTYPE_CONNECT_REQ && cmsg != SCS_CONN_MSGTYPE_ACCEPT_REQ) {
        return -1;
    }
    memcpy(out, frame + SCS_CONNECT_DATA_ABS_OFF, SCS_CONNECT_DATA_LEN);
    return 0;
}

const char *scs_connect_data_fmt(const uint8_t *cd, char *buf, size_t bufsz)
{
    /* 16*3 hex + "|" + 16 ascii + "|" + NUL = 67. */
    if (buf == NULL || bufsz < 72) {
        return "";
    }
    if (cd == NULL) {
        buf[0] = '\0';
        return buf;
    }
    size_t o = 0;
    for (int i = 0; i < SCS_CONNECT_DATA_LEN; i++) {
        int n = snprintf(buf + o, bufsz - o, "%02x%s", cd[i],
                         i == SCS_CONNECT_DATA_LEN - 1 ? " |" : " ");
        if (n < 0 || (size_t)n >= bufsz - o) {
            buf[bufsz - 1] = '\0';
            return buf;
        }
        o += (size_t)n;
    }
    for (int i = 0; i < SCS_CONNECT_DATA_LEN && o + 2 < bufsz; i++) {
        buf[o++] = (cd[i] >= 32 && cd[i] < 127) ? (char)cd[i] : '.';
    }
    if (o + 1 < bufsz) {
        buf[o++] = '|';
    }
    buf[o] = '\0';
    return buf;
}
