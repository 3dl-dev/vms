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

/* vms-760: OVMX's directory op=3 CONNECT-CONFIRM (62-byte SCA). Byte-exact to
 * the clean joiner's confirm (formation-clean-2node.pcap SCA idx26) EXCEPT
 * [8:10] connect-flag = 0x0001 (golden, matching the other OVMX dir templates;
 * the clean ref carried 0x03e8, a config artifact). Substituted at build time:
 * dst/src logical, counters, remote Con.ID [50:54], local Con.ID [54:58].
 * op[46:48]=3 and marker[58:62]=0x00010000 are baked in. NO SYSAP names (the
 * frame ends at the marker). */
static const uint8_t dir_confirm_tmpl[SCS_DIR_CONFIRM_SCA_LEN] = {
    /* [0:2]   */ 0x3c, 0x00,
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* dst logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,                               /* connect flag (golden 0x0001) */
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x5b, 0x13,
    /* [18:20] */ 0x02, 0x00,                               /* recv_ack (SUBST) */
    /* [20:22] */ 0x02, 0x00,                               /* send_seq (SUBST) */
    /* [22:24] */ 0x01, 0x00,                               /* incarnation (SUBST) */
    /* [24:26] */ 0x12, 0x00,
    /* [26:28] */ 0x02, 0x00,                               /* recv_ack mirror (SUBST) */
    /* [28:30] */ 0x00, 0x00,
    /* [30:32] */ 0x02, 0x00,                               /* send_seq mirror (SUBST) */
    /* [32:34] */ 0x00, 0x00,
    /* [34:36] */ 0x02, 0x00,                               /* recv_ack 3rd (SUBST) */
    /* [36:38] */ 0x00, 0x00,
    /* [38:40] */ 0x01, 0x00,
    /* [40:42] */ 0x00, 0x02,
    /* [42:44] */ 0x12, 0x00,                               /* inner length = 18 */
    /* [44:46] */ 0x04, 0x00,
    /* [46:48] */ 0x03, 0x00,                               /* op = 3 (connect-confirm) */
    /* [48:50] */ 0x00, 0x00,                               /* flag = 0 */
    /* [50:54] */ 0x08, 0x00, 0xdc, 0xe2,                   /* remote Con.ID (SUBST, member's) */
    /* [54:58] */ 0x07, 0x00, 0x00, 0x00,                   /* local Con.ID (SUBST) */
    /* [58:62] */ 0x00, 0x00, 0x01, 0x00                    /* marker = 0x00010000 */
};

/*
 * vms-e81: the op-5 CONFIRM5 template -- 58 SCA bytes, NOT 62.
 *
 * It is tempting to build this as "the op-3 confirm with the opcode changed",
 * and that would have been wrong in the way this project keeps getting caught.
 * A 336-frame census (every op 5 in the capture library, across 4 sender nodes
 * and 15 captures) says op 5 differs from op 3 in FOUR places, not one:
 *
 *   [0:2]   0x003c -> 0x0038   outer length word   (SCA 62 -> 58)
 *   [42:44] 0x0012 -> 0x000e   inner length word   (18 -> 14)
 *   [46:48] 0x0003 -> 0x0005   opcode
 *   [58:62] the trailing marker word is ABSENT -- the frame ENDS at 58
 *
 * Copying the op-3 template and patching only the opcode would have emitted 62
 * bytes declaring 60, i.e. a frame four bytes longer than it claims. That is the
 * exact shape of the malformed op-7 that stalled the whole join for three
 * sessions: DERIVE LENGTH WORDS FROM WHAT YOU EMIT, NEVER INHERIT THEM. A peer
 * drops the over-long frame as a runt and the next frame dies on the sequence
 * gap -- in silence, because there is no NAK anywhere in this protocol.
 *
 * [16] is 0x4b (SEQAPP), NOT a mirror of the op-4 being answered: 86 of the
 * observed pairs answer a 0x5b op-4 with a 0x4b op-5, and all three op-5s a real
 * VAX has ever sent AT OVMX are 0x4b.
 */
static const uint8_t dir_confirm5_tmpl[SCS_DIR_CONFIRM5_SCA_LEN] = {
    /* [0:2]   */ 0x38, 0x00,                               /* outer length = 56 (SCA 58) */
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* dst logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,                               /* connect flag (336/336) */
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x4b, 0x13,                               /* SEQAPP -- never mirrors the op-4 */
    /* [18:20] */ 0x02, 0x00,                               /* recv_ack (SUBST) */
    /* [20:22] */ 0x02, 0x00,                               /* send_seq (SUBST) */
    /* [22:24] */ 0x01, 0x00,                               /* incarnation (SUBST) -- OUR OWN */
    /* [24:26] */ 0x12, 0x00,
    /* [26:28] */ 0x02, 0x00,                               /* recv_ack mirror (SUBST) */
    /* [28:30] */ 0x00, 0x00,
    /* [30:32] */ 0x02, 0x00,                               /* send_seq mirror (SUBST) */
    /* [32:34] */ 0x00, 0x00,
    /* [34:36] */ 0x02, 0x00,                               /* recv_ack 3rd (SUBST) */
    /* [36:38] */ 0x00, 0x00,
    /* [38:40] */ 0x01, 0x00,
    /* [40:42] */ 0x00, 0x02,
    /* [42:44] */ 0x0e, 0x00,                               /* inner length = 14 */
    /* [44:46] */ 0x04, 0x00,
    /* [46:48] */ 0x05, 0x00,                               /* op = 5 (MSCP connect-CONFIRM5) */
    /* [48:50] */ 0x00, 0x00,                               /* flag = 0 (336/336) */
    /* [50:54] */ 0x08, 0x00, 0xdc, 0xe2,                   /* remote Con.ID (SUBST, peer's) */
    /* [54:58] */ 0x07, 0x00, 0x00, 0x00                    /* local Con.ID (SUBST, ours) */
    /* NO marker word -- the frame ends here. */
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

    /* Result field [78:94]. Per-name selection (vms-760):
     *   - negative (p->affirmative == 0): the GROUNDED literal "NOT PRESENT HERE"
     *   - MSCP$DISK HIT: the queried NAME echoed, 16-byte blank-padded. GROUNDED
     *     byte-exact (af2-firsttimer-established.pcap: OVMX's MSCP$DISK lookup
     *     RESPONSE result@92 == 'MSCP$DISK       '). This DIFFERS from the
     *     VMS$VAXcluster HIT, so the affirmative descriptor is NOT one-size.
     *   - any other affirmative (VMS$VAXcluster): the SCA#38 descriptor blob.
     * The MSCP$DISK-name test uses a 9-char prefix compare so the caller may pass
     * either a NUL- or blank-terminated name. */
    if (!p->affirmative) {
        memcpy(out + 14 + 78, SCS_DIR_NOT_PRESENT, SCS_DIR_RESULT_LEN);
    } else if (memcmp(p->name, "MSCP$DISK", 9) == 0) {
        /* result == the queried name, same 16-byte blank-padded form as [62:78]. */
        memcpy(out + 14 + 78, out + 14 + 62, SCS_DIR_RESULT_LEN);
    } else {
        memcpy(out + 14 + 78, dir_affirmative_result, SCS_DIR_RESULT_LEN);
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

int scs_dir_build_connect_confirm(const struct scs_dir_params *p,
                                  uint8_t out[SCS_DIR_CONFIRM_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_confirm_tmpl, SCS_DIR_CONFIRM_SCA_LEN, p->recv_ack,
                     p->send_seq, p->incarnation, out);
    /* Con.ID pair now bound: remote = member's dir handle, local = OVMX's own. */
    put_le32(out + 14 + 50, p->remote_conid);
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

int scs_dir_build_mscp_echo(const struct scs_dir_params *p,
                            uint8_t out[SCS_DIR_ECHO_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    /* Same 66-byte SCA class as the directory CONNECT-ECHO. */
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_echo_tmpl, SCS_DIR_ECHO_SCA_LEN, p->recv_ack, p->send_seq,
                     p->incarnation, out);
    /* remote = member's MSCP client handle (echoed); local stays 0 for the echo. */
    put_le32(out + 14 + 50, p->remote_conid);
    /* vms-760 delta (1): opcode [16] = 0x4b (data-phase; the VC to OVMX is up),
     * NOT the 0x5b the directory-echo template carries. GROUNDED from the pcap. */
    out[14 + 16] = SCS_MSGTYPE_SEQAPP;
    /* vms-760 delta (2): the truncated SYSAP-name tail [62:66] = 'MSCP' (the
     * 66-byte SCA window clips 'MSCP$DISK' after 4 bytes), NOT the template's
     * 'SCS$'. GROUNDED from the pcap. */
    memcpy(out + 14 + 62, "MSCP", 4);
    return 0;
}

int scs_dir_build_vc_echo(const struct scs_dir_params *p,
                          uint8_t out[SCS_DIR_ECHO_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    /* vms-760: the joiner's op=1 CONNECT-ECHO answering the MEMBER-opened
     * VMS$VAXcluster VC (af2-firsttimer-established VC pair, ~143.7586). Same
     * 66-byte SCA as the MSCP echo; the only delta is the truncated SYSAP-name
     * tail [62:66] = 'VMS$' ("VMS$VAXcluster" clipped to the 66-byte window).
     * Every accept in this protocol echoes op=1 before its op=2/op=4 response. */
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_echo_tmpl, SCS_DIR_ECHO_SCA_LEN, p->recv_ack, p->send_seq,
                     p->incarnation, out);
    put_le32(out + 14 + 50, p->remote_conid); /* member's VC handle (echoed) */
    out[14 + 16] = SCS_MSGTYPE_SEQAPP;         /* opcode 0x4b (data-phase) */
    memcpy(out + 14 + 62, "VMS$", 4);          /* GROUNDED name tail */
    return 0;
}

int scs_dir_build_mscp_accept(const struct scs_dir_params *p,
                              uint8_t out[SCS_DIR_CONFIRM_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    /* Structurally the op=3 dir CONNECT-CONFIRM (same 62-byte SCA, opcode 0x5b,
     * marker 0x00010000, no SYSAP names). */
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_confirm_tmpl, SCS_DIR_CONFIRM_SCA_LEN, p->recv_ack,
                     p->send_seq, p->incarnation, out);
    /* Con.ID pair bound: remote = member's MSCP client handle, local = OVMX's
     * fresh MSCP server handle (the admission act for OUR server connection). */
    put_le32(out + 14 + 50, p->remote_conid);
    put_le32(out + 14 + 54, p->local_conid);
    /* vms-760: the SINGLE fixed-byte delta vs the confirm -- op [46:48] = 4. */
    put_le16(out + 14 + 46, SCS_DIR_OP_ACCEPT);
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

/*
 * scs_dir_build_mscp_confirm5 - answer a peer's op-4 ACCEPT4 with an op-5
 * CONFIRM5, completing the "form B" accept of a connection WE opened.
 *
 * There are two accept forms on an MSCP$DISK connection and OVMX only ever
 * implemented half of each:
 *   form A: op 0 -> op 1 -> op 2 RESPONSE -> op 3 CONFIRM      (we handle this)
 *   form B: op 0 -> op 1 -> op 4 ACCEPT4  -> op 5 CONFIRM5     (we EMIT op 4 as
 *           a server, but could not CONSUME one as a client)
 *
 * The consequence was not a missing feature, it was a wedged node: when VAX3
 * answered our connect with an op-4 we silently dropped it, then retransmitted
 * the same request 60 times over 178 s with a frozen send_seq.
 *
 * Grounded on 336 op-5 frames from 4 sender nodes across 15 captures, including
 * three a real VAX sent AT OVMX. Con.ID convention is identical to the op-3
 * confirm: [50] = the peer's handle, taken from the op-4's [54]; [54] = our own,
 * the handle we put in our op-0. Nothing follows an op-5 -- across all 336 the
 * Con.ID pair never appears again (334 silent, 2 retransmits), so OVMX owes the
 * peer nothing further on that connection.
 */
int scs_dir_build_mscp_confirm5(const struct scs_dir_params *p,
                                uint8_t out[SCS_DIR_CONFIRM5_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_confirm5_tmpl, SCS_DIR_CONFIRM5_SCA_LEN, p->recv_ack,
                     p->send_seq, p->incarnation, out);
    put_le32(out + 14 + 50, p->remote_conid); /* peer's handle, from its op-4 [54] */
    put_le32(out + 14 + 54, p->local_conid);  /* ours, the one we sent in our op-0 */
    return 0;
}
