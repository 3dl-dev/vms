/*
 * scs_dir.c - SCS$DIRECTORY connect + SCS$DIR_LOOKUP responder (vms-246).
 * See scs_dir.h for the full clean-room provenance and the
 * GROUNDED-vs-REPLAYED field breakdown.
 */
#include "scs_dir.h"

#include <string.h>

/* --- byte-exact SCA-content templates (payload byte 0 = abs frame 14) --- */

/* SCA#23 CONNECT-ECHO (op=1): VAX2->VAX1, remote=0x63050008 echoed, local=0.
 * formation-ci1-joinwindow.pcap. Substituted at build time: dst-logical
 * [2:8], src-logical [10:16], counters [18:20]/[20:22]/[26:28]/[30:32]/
 * [34:36], remote Con.ID [50:54]. Local [54:58] stays 0 for the echo. */
static const uint8_t dir_echo_tmpl[SCS_DIR_ECHO_SCA_LEN] = {
    /* [0:2]   */ 0x40, 0x00,
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* dst logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x5b, 0x13,                               /* opcode 0x5b, format 0x13 */
    /* [18:20] */ 0x01, 0x00,                               /* recv_ack (SUBST) */
    /* [20:22] */ 0x01, 0x00,                               /* send_seq (SUBST) */
    /* [22:24] */ 0x01, 0x00,
    /* [24:26] */ 0x12, 0x00,
    /* [26:28] */ 0x01, 0x00,                               /* recv_ack mirror (SUBST) */
    /* [28:30] */ 0x00, 0x00,
    /* [30:32] */ 0x01, 0x00,                               /* send_seq mirror (SUBST) */
    /* [32:34] */ 0x00, 0x00,
    /* [34:36] */ 0x01, 0x00,                               /* recv_ack 3rd (SUBST) */
    /* [36:38] */ 0x00, 0x00,
    /* [38:40] */ 0x01, 0x00,
    /* [40:42] */ 0x00, 0x02,
    /* [42:44] */ 0x16, 0x00,                               /* inner length = 22 */
    /* [44:46] */ 0x04, 0x00,
    /* [46:48] */ 0x01, 0x00,                               /* op = 1 */
    /* [48:50] */ 0x00, 0x00,                               /* flag = 0 */
    /* [50:54] */ 0x08, 0x00, 0x05, 0x63,                   /* remote Con.ID (SUBST) */
    /* [54:58] */ 0x00, 0x00, 0x00, 0x00,                   /* local Con.ID = 0 (not yet assigned) */
    /* [58:62] */ 0x00, 0x00, 0x01, 0x00,
    /* [62:66] */ 0x53, 0x43, 0x53, 0x24                    /* 'SCS$' (truncated name in this class) */
};

/* SCA#25 CONNECT-RESPONSE (op=2): remote=0x63050008, local=0x33590007 supplied.
 * Substituted: dst/src logical, counters, remote Con.ID [50:54], local Con.ID
 * [54:58]. name [62:78]='SCS$DIR_LOOKUP  ', result [78:94]='SCS$DIRECTORY   '
 * are replayed byte-exact. */
static const uint8_t dir_resp_tmpl[SCS_DIR_RESP_SCA_LEN] = {
    /* [0:2]   */ 0x6c, 0x00,
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* dst logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x5b, 0x13,
    /* [18:20] */ 0x01, 0x00,                               /* recv_ack (SUBST) */
    /* [20:22] */ 0x02, 0x00,                               /* send_seq (SUBST) */
    /* [22:24] */ 0x01, 0x00,
    /* [24:26] */ 0x12, 0x00,
    /* [26:28] */ 0x01, 0x00,                               /* recv_ack mirror (SUBST) */
    /* [28:30] */ 0x00, 0x00,
    /* [30:32] */ 0x02, 0x00,                               /* send_seq mirror (SUBST) */
    /* [32:34] */ 0x00, 0x00,
    /* [34:36] */ 0x01, 0x00,                               /* recv_ack 3rd (SUBST) */
    /* [36:38] */ 0x00, 0x00,
    /* [38:40] */ 0x01, 0x00,
    /* [40:42] */ 0x00, 0x02,
    /* [42:44] */ 0x42, 0x00,                               /* inner length = 66 */
    /* [44:46] */ 0x04, 0x00,
    /* [46:48] */ 0x02, 0x00,                               /* op = 2 */
    /* [48:50] */ 0x01, 0x00,                               /* flag = 1 */
    /* [50:54] */ 0x08, 0x00, 0x05, 0x63,                   /* remote Con.ID (SUBST) */
    /* [54:58] */ 0x07, 0x00, 0x59, 0x33,                   /* local Con.ID (SUBST) */
    /* [58:62] */ 0x00, 0x00, 0x00, 0x00,
    /* [62:78] */ 'S','C','S','$','D','I','R','_','L','O','O','K','U','P',' ',' ',
    /* [78:94] */ 'S','C','S','$','D','I','R','E','C','T','O','R','Y',' ',' ',' ',
    /* [94:110]*/ ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '
};

/* SCA#31 LOOKUP-RESPONSE (94 bytes): MSCP$TAPE -> "NOT PRESENT HERE".
 * Substituted: dst/src logical, opcode [16], counters, op [46:48], remote
 * [50:54], local [54:58], name [62:78], result [78:94]. */
static const uint8_t dir_lookup_tmpl[SCS_DIR_LOOKUP_SCA_LEN] = {
    /* [0:2]   */ 0x5c, 0x00,
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* dst logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x5b, 0x13,                               /* opcode (SUBST) / format 0x13 */
    /* [18:20] */ 0x03, 0x00,                               /* recv_ack (SUBST) */
    /* [20:22] */ 0x03, 0x00,                               /* send_seq (SUBST) */
    /* [22:24] */ 0x01, 0x00,
    /* [24:26] */ 0x12, 0x00,
    /* [26:28] */ 0x03, 0x00,                               /* recv_ack mirror (SUBST) */
    /* [28:30] */ 0x00, 0x00,
    /* [30:32] */ 0x03, 0x00,                               /* send_seq mirror (SUBST) */
    /* [32:34] */ 0x00, 0x00,
    /* [34:36] */ 0x03, 0x00,                               /* recv_ack 3rd (SUBST) */
    /* [36:38] */ 0x00, 0x00,
    /* [38:40] */ 0x01, 0x00,
    /* [40:42] */ 0x00, 0x02,
    /* [42:44] */ 0x32, 0x00,                               /* inner length = 50 */
    /* [44:46] */ 0x04, 0x00,
    /* [46:48] */ 0x0a, 0x00,                               /* op = 0x0a (SUBST-echo) */
    /* [48:50] */ 0x01, 0x00,                               /* flag = 1 */
    /* [50:54] */ 0x08, 0x00, 0x05, 0x63,                   /* remote Con.ID (SUBST) */
    /* [54:58] */ 0x07, 0x00, 0x59, 0x33,                   /* local Con.ID (SUBST) */
    /* [58:62] */ 0x01, 0x00, 0x00, 0x00,                   /* response marker */
    /* [62:78] */ 'M','S','C','P','$','T','A','P','E',' ',' ',' ',' ',' ',' ',' ',
    /* [78:94] */ 'N','O','T',' ','P','R','E','S','E','N','T',' ','H','E','R','E'
};

/* The AFFIRMATIVE VMS$VAXcluster result descriptor, [78:94] of SCA#38.
 * Reproduced byte-exact as observed; internal semantics NOT grounded
 * (spec sec 4h RE gap (c)). */
static const uint8_t dir_affirmative_result[SCS_DIR_RESULT_LEN] = {
    0x01, 0x1b, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00
};

/* vms-760: OVMX's OWN SCS$DIRECTORY CONNECT-REQUEST (the active joiner opening a
 * directory connection TO the member). 110-byte SCA, byte-exact to the clean
 * joiner's frame (formation-clean-2node.pcap idx25) EXCEPT [8:10] connect-flag
 * = 0x0001 (the golden-lab value; the clean ref carried 0x03e8, a config
 * artifact -- the existing response templates above also use 0x0001). Substituted
 * at build time: dst/src logical, counters, local Con.ID [54:58]. remote [50:54]
 * stays 0 (peer's handle not yet known). name [62:78]="SCS$DIRECTORY   ",
 * operation [78:110]="SCS$DIR_LOOKUP" (blank-padded) are replayed byte-exact. */
static const uint8_t dir_connreq_tmpl[SCS_DIR_RESP_SCA_LEN] = {
    /* [0:2]   */ 0x6c, 0x00,
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* dst logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,                               /* connect flag (golden 0x0001) */
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x5b, 0x13,
    /* [18:20] */ 0x00, 0x00,                               /* recv_ack (SUBST) */
    /* [20:22] */ 0x01, 0x00,                               /* send_seq (SUBST) */
    /* [22:24] */ 0x01, 0x00,                               /* incarnation (SUBST) */
    /* [24:26] */ 0x12, 0x00,
    /* [26:28] */ 0x00, 0x00,                               /* recv_ack mirror (SUBST) */
    /* [28:30] */ 0x00, 0x00,
    /* [30:32] */ 0x01, 0x00,                               /* send_seq mirror (SUBST) */
    /* [32:34] */ 0x00, 0x00,
    /* [34:36] */ 0x00, 0x00,                               /* recv_ack 3rd (SUBST) */
    /* [36:38] */ 0x00, 0x00,
    /* [38:40] */ 0x01, 0x00,
    /* [40:42] */ 0x00, 0x02,
    /* [42:44] */ 0x42, 0x00,                               /* inner length = 66 */
    /* [44:46] */ 0x04, 0x00,
    /* [46:48] */ 0x00, 0x00,                               /* op = 0 (connect) */
    /* [48:50] */ 0x03, 0x00,                               /* flag = 3 */
    /* [50:54] */ 0x00, 0x00, 0x00, 0x00,                   /* remote Con.ID = 0 (not yet known) */
    /* [54:58] */ 0x07, 0x00, 0x00, 0x00,                   /* local Con.ID (SUBST) */
    /* [58:62] */ 0x00, 0x00, 0x01, 0x00,
    /* [62:78] */ 'S','C','S','$','D','I','R','E','C','T','O','R','Y',' ',' ',' ',
    /* [78:94] */ 'S','C','S','$','D','I','R','_','L','O','O','K','U','P',' ',' ',
    /* [94:110]*/ ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '
};

/* vms-760: OVMX's directory LOOKUP-REQUEST (querying the member's directory).
 * 94-byte SCA, byte-exact to the clean joiner's lookup (idx32) EXCEPT [8:10]
 * connect-flag = 0x0001 (golden). Substituted: dst/src logical, counters, remote
 * Con.ID [50:54] (member's directory handle), local [54:58], name [62:78].
 * op[46:48]=0x0a, [58:62] request marker=0, result [78:94]=zeros (a request). */
static const uint8_t dir_lookupreq_tmpl[SCS_DIR_LOOKUP_SCA_LEN] = {
    /* [0:2]   */ 0x5c, 0x00,
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* dst logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,                               /* connect flag (golden 0x0001) */
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x5b, 0x13,
    /* [18:20] */ 0x02, 0x00,                               /* recv_ack (SUBST) */
    /* [20:22] */ 0x03, 0x00,                               /* send_seq (SUBST) */
    /* [22:24] */ 0x01, 0x00,                               /* incarnation (SUBST) */
    /* [24:26] */ 0x12, 0x00,
    /* [26:28] */ 0x02, 0x00,                               /* recv_ack mirror (SUBST) */
    /* [28:30] */ 0x00, 0x00,
    /* [30:32] */ 0x03, 0x00,                               /* send_seq mirror (SUBST) */
    /* [32:34] */ 0x00, 0x00,
    /* [34:36] */ 0x02, 0x00,                               /* recv_ack 3rd (SUBST) */
    /* [36:38] */ 0x00, 0x00,
    /* [38:40] */ 0x01, 0x00,
    /* [40:42] */ 0x00, 0x02,
    /* [42:44] */ 0x32, 0x00,                               /* inner length = 50 */
    /* [44:46] */ 0x04, 0x00,
    /* [46:48] */ 0x0a, 0x00,                               /* op = 0x0a (lookup) */
    /* [48:50] */ 0x00, 0x00,                               /* flag = 0 */
    /* [50:54] */ 0x08, 0x00, 0xdc, 0xe2,                   /* remote Con.ID (SUBST, member's) */
    /* [54:58] */ 0x07, 0x00, 0x00, 0x00,                   /* local Con.ID (SUBST) */
    /* [58:62] */ 0x00, 0x00, 0x00, 0x00,                   /* request marker = 0 */
    /* [62:78] */ 'M','S','C','P','$','T','A','P','E',' ',' ',' ',' ',' ',' ',' ',
    /* [78:94] */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0            /* result = zeros (request) */
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

/* Lay the Ethernet header + template, then substitute the shared envelope
 * (dst/src logical) and the SCS sequence counters. Con.ID/name/result are
 * substituted by the per-class callers afterward. */
static void dir_build_common(const uint8_t *dst_mac, const uint8_t *src_mac,
                             const uint8_t *src_logical,
                             const uint8_t *peer_logical, const uint8_t *tmpl,
                             size_t sca_len, uint16_t recv_ack, uint16_t send_seq,
                             uint16_t incarnation, uint8_t *out)
{
    /* Ethernet header (abs 0-13). */
    memcpy(out + 0, dst_mac, 6);
    memcpy(out + 6, src_mac, 6);
    out[12] = 0x60;
    out[13] = 0x07;

    /* SCA content (abs 14+) from the captured joiner template. */
    memcpy(out + 14, tmpl, sca_len);

    /* Envelope address substitutions (payload-relative + 14). */
    memcpy(out + 14 + 2, peer_logical, 6);  /* dst logical [2:8]  (abs 16) */
    memcpy(out + 14 + 10, src_logical, 6);  /* src-logical [10:16] (abs 24) = aa:00:04:00:<sysid>
                                             * cluster-LOGICAL addr, NOT raw HW MAC (vms-9f3) */

    /* Sequence counters (spec sec 4h(4)): recv_ack at [18:20]/[26:28]/[34:36],
     * send_seq at [20:22] mirrored at [30:32]. */
    put_le16(out + 14 + 18, recv_ack);
    put_le16(out + 14 + 20, send_seq);
    put_le16(out + 14 + 26, recv_ack);
    put_le16(out + 14 + 30, send_seq);
    put_le16(out + 14 + 34, recv_ack);

    /* [22:24] node-incarnation echo (§4i established-join extension of the §4h
     * directory exchange). GROUNDED-BY-OBSERVATION, not from a golden template:
     * on an established-cluster join the member (VAX1) stamps its CURRENT
     * node-incarnation N in [22:24] of its own 0x5b SCS$DIRECTORY connect-
     * request (observed N=3 on the vms-246 lab wire; the fresh-formation golden
     * carries N=1, which is what the baked-in template holds). This is the same
     * per-node incarnation the member advertises in its directed HELLO [78:80]
     * and that §4i.B grounds for the 0x41 START [22:24]. OVMX echoes the value
     * the member itself put on the wire -- never a self-invented constant. A 0
     * here leaves the template's fresh value (1), preserving byte-exact golden
     * reproduction for the fresh-formation path and the unit tests. */
    if (incarnation != 0) {
        put_le16(out + 14 + 22, incarnation);
    }
}

int scs_dir_build_connect_echo(const struct scs_dir_params *p,
                               uint8_t out[SCS_DIR_ECHO_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_echo_tmpl, SCS_DIR_ECHO_SCA_LEN, p->recv_ack, p->send_seq,
                     p->incarnation, out);
    /* Con.ID: remote = peer's handle (echoed), local stays 0 for the echo. */
    put_le32(out + 14 + 50, p->remote_conid);
    return 0;
}

int scs_dir_build_connect_response(const struct scs_dir_params *p,
                                   uint8_t out[SCS_DIR_RESP_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_resp_tmpl, SCS_DIR_RESP_SCA_LEN, p->recv_ack, p->send_seq,
                     p->incarnation, out);
    /* Con.ID pair now bound: remote = peer's handle, local = OVMX's own. */
    put_le32(out + 14 + 50, p->remote_conid);
    put_le32(out + 14 + 54, p->local_conid);
    return 0;
}

int scs_dir_build_lookup_response(const struct scs_dir_lookup_params *p,
                                  uint8_t out[SCS_DIR_LOOKUP_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_lookup_tmpl, SCS_DIR_LOOKUP_SCA_LEN, p->recv_ack, p->send_seq,
                     p->incarnation, out);

    /* Opcode echoes the request (0x5b before the SCS$DIRECTORY connection is up,
     * 0x4b once it is -- see spec sec 4h / 4g phase-3). */
    out[14 + 16] = p->opcode;
    /* Directory-operation field echoes the request's [46:48] (inferred). */
    put_le16(out + 14 + 46, p->op);
    /* Con.ID pair (bound). */
    put_le32(out + 14 + 50, p->remote_conid);
    put_le32(out + 14 + 54, p->local_conid);

    /* Queried SYSAP name echoed into [62:78], 16-byte blank-padded. */
    {
        uint8_t namebuf[SCS_DIR_NAME_LEN];
        memset(namebuf, ' ', sizeof(namebuf));
        /* Bounded copy up to the first NUL or the 16-byte field width. A plain
         * loop (not strnlen) keeps this free of any POSIX feature-macro
         * dependency, so it stays clean under the musl-static build. */
        size_t n = 0;
        while (n < SCS_DIR_NAME_LEN && p->name[n] != '\0') {
            n++;
        }
        memcpy(namebuf, p->name, n);
        memcpy(out + 14 + 62, namebuf, SCS_DIR_NAME_LEN);
    }

    /* Result field [78:94]: affirmative descriptor or the GROUNDED negative
     * marker "NOT PRESENT HERE". */
    if (p->affirmative) {
        memcpy(out + 14 + 78, dir_affirmative_result, SCS_DIR_RESULT_LEN);
    } else {
        memcpy(out + 14 + 78, SCS_DIR_NOT_PRESENT, SCS_DIR_RESULT_LEN);
    }
    return 0;
}

int scs_dir_build_connect_request(const struct scs_dir_params *p,
                                  uint8_t out[SCS_DIR_RESP_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_connreq_tmpl, SCS_DIR_RESP_SCA_LEN, p->recv_ack, p->send_seq,
                     p->incarnation, out);
    /* remote Con.ID stays 0 (member's not yet known); local = OVMX's own. */
    put_le32(out + 14 + 50, 0);
    put_le32(out + 14 + 54, p->local_conid);
    return 0;
}

int scs_dir_build_lookup_request(const struct scs_dir_lookup_params *p,
                                 uint8_t out[SCS_DIR_LOOKUP_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_lookupreq_tmpl, SCS_DIR_LOOKUP_SCA_LEN, p->recv_ack, p->send_seq,
                     p->incarnation, out);
    put_le16(out + 14 + 46, SCS_DIR_OP_LOOKUP);  /* op = 0x0a */
    put_le32(out + 14 + 50, p->remote_conid);    /* member's directory handle */
    put_le32(out + 14 + 54, p->local_conid);     /* OVMX's own handle */
    /* Queried SYSAP name into [62:78], 16-byte blank-padded (as the response). */
    {
        uint8_t namebuf[SCS_DIR_NAME_LEN];
        memset(namebuf, ' ', sizeof(namebuf));
        size_t n = 0;
        while (n < SCS_DIR_NAME_LEN && p->name[n] != '\0') {
            n++;
        }
        memcpy(namebuf, p->name, n);
        memcpy(out + 14 + 62, namebuf, SCS_DIR_NAME_LEN);
    }
    /* result [78:94] stays zeros (a request carries no result). */
    return 0;
}

int scs_dir_parse(const uint8_t *frame, size_t len, struct scs_dir_view *v)
{
    if (frame == NULL || v == NULL) {
        return -1;
    }
    /* Need the Ethernet header + SCA envelope through the Con.ID pair
     * (abs 14 + 58 = 72). */
    if (len < 72) {
        return -1;
    }

    memset(v, 0, sizeof(*v));

    uint16_t lenword = get_le16(frame + 14);
    v->total_sca_len = (uint16_t)(lenword + 2);
    v->opcode = frame[14 + 16];
    v->format = frame[14 + 17];
    v->recv_ack = get_le16(frame + 14 + 18);
    v->send_seq = get_le16(frame + 14 + 20);
    v->op = get_le16(frame + 14 + 46);
    v->flag = get_le16(frame + 14 + 48);
    v->remote_conid = get_le32(frame + 14 + 50);
    v->local_conid = get_le32(frame + 14 + 54);
    v->marker = get_le32(frame + 14 + 58);

    /* Name field [62:78] (abs 76-91). */
    if (len >= 14 + 78) {
        v->has_name = 1;
        memcpy(v->name, frame + 14 + 62, SCS_DIR_NAME_LEN);
        v->name[SCS_DIR_NAME_LEN] = '\0';
    }

    /* Result field [78:94] (abs 92-107). */
    if (len >= 14 + 94) {
        v->has_result = 1;
        v->result_zero = 1;
        for (size_t i = 0; i < SCS_DIR_RESULT_LEN; i++) {
            if (frame[14 + 78 + i] != 0) {
                v->result_zero = 0;
                break;
            }
        }
    }

    /* Classification. A SCS$DIRECTORY CONNECT-REQUEST names "SCS$DIRECTORY" in
     * [62:78] and has not learned OVMX's handle yet (remote_conid == 0). */
    if (v->has_name && v->remote_conid == 0 &&
        memcmp(v->name, "SCS$DIRECTORY", 13) == 0) {
        v->is_dir_connect_request = 1;
    }
    /* A lookup REQUEST carries op==0x0a with a queried name and the [58:62]
     * request marker == 0 (a RESPONSE sets it to 1). NOTE: the result field
     * [78:94] is NOT a reliable discriminator -- the golden capture's requests
     * happened to carry a zero result, but a live VAX's lookup request fills
     * [78:94] with request-context bytes (observed on the vms-246 lab wire), so
     * result_zero would misclassify it. The [58:62] marker is the robust
     * request/response discriminator, grounded on both the golden capture and
     * the live join. */
    if (v->op == SCS_DIR_OP_LOOKUP && v->has_name && v->has_result && v->marker == 0) {
        v->is_lookup_request = 1;
    }

    return 0;
}
