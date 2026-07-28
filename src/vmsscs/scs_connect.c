/*
 * scs_connect.c - VMS$VAXcluster SCS connect handshake builder/parser
 * (vms-5fe). See scs_connect.h for the full clean-room provenance and the
 * GROUNDED-vs-REPLAYED field breakdown.
 */
#include "scs_connect.h"

#include <string.h>

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
    memcpy(out + 14 + 10, p->src_mac, 6);       /* src logical  (abs 24) = OVMX HW MAC */
    put_le32(out + 14 + 50, remote_conid);      /* Remote Con.ID (abs 64) */
    put_le32(out + 14 + 54, p->local_conid);    /* Local  Con.ID (abs 68) */

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

    return 0;
}
