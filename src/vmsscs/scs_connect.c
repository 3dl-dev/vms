/*
 * scs_connect.c - VMS$VAXcluster SCS connect handshake builder/parser
 * (vms-5fe). See scs_connect.h for the full clean-room provenance and the
 * GROUNDED-vs-REPLAYED field breakdown.
 */
#include "scs_connect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scs_env.h" /* vms-ec7: THE shared SCS message envelope */

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
 * NOT invented, NOT an OVMX design choice: every byte below was observed.
 * OBSERVED IN ONE VMS BUILD, THOUGH -- the whole capture library is a single
 * OpenVMS VAX V7.3 installation under 3 system roots on 1 disk image, so
 * "148/148" below means "no counterexample in that installation", not
 * agreement between independent VMS systems. See the attestation note in
 * scs_connect.h and the standing limit in spec sec 5. It is the reason OVMX
 * decodes the peer's version claim and acts on none of it. */
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

/* Byte-exact 110-byte SCA content of the JOINER's MSCP$DISK client
 * CONNECT-REQUEST (VMS$DISK_CL_DRVR -> MSCP$DISK), captured from the clean
 * 1->2-node formation (formation-clean-2node.pcap SCA idx35, joiner->member).
 * Same connect class as connect_request_tmpl; the substituted fields are
 * identical (dest logical [2:8], src logical [10:16], remote Con.ID [50:54],
 * local Con.ID [54:58], and the live SCS seq counters). Everything else is a
 * REPLAY of the captured frame, including the msgtype 0x5b at [16], the
 * MSCP-specific [8:10]=0x03e8 and [58:62] connect-class fields, the
 * 'MSCP$DISK' target [62:78] / 'VMS$DISK_CL_DRVR' requester [78:94] SYSAP
 * names, and the ASCII 'V5.0          + ' class descriptor [94:110]. The
 * ungrounded constants are replayed, not synthesized (clean-room: observe +
 * public docs only, CLAUDE.md Rule 8). vms-760. */
static const uint8_t mscp_connect_request_tmpl[SCS_CONNECT_SCA_LEN] = {
    /* [0:2]   */ 0x6c, 0x00,
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x4c, 0x04,       /* dest logical (SUBST) */
    /* [8:10]  */ 0xe8, 0x03,                               /* MSCP connect-class field (REPLAY) */
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x4d, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x5b, 0x13,                               /* msgtype 0x5b, format 0x13 */
    /* [18:26] */ 0x04, 0x00, 0x06, 0x00, 0x01, 0x00, 0x12, 0x00,
    /* [26:34] */ 0x04, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    /* [34:42] */ 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02,
    /* [42:50] */ 0x42, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0a, 0x00,
    /* [50:54] */ 0x00, 0x00, 0x00, 0x00,                   /* remote Con.ID (SUBST, 0 for REQUEST) */
    /* [54:58] */ 0x08, 0x00, 0x62, 0x4e,                   /* local Con.ID (SUBST) */
    /* [58:62] */ 0x02, 0x00, 0x01, 0x00,                   /* MSCP connect-class field (REPLAY) */
    /* [62:78] */ 'M','S','C','P','$','D','I','S','K',' ',' ',' ',' ',' ',' ',' ',
    /* [78:94] */ 'V','M','S','$','D','I','S','K','_','C','L','_','D','R','V','R',
    /* [94:110]*/ 'V','5','.','0',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ','+',' '
};

static void put_le16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xff);
    dst[1] = (uint8_t)((v >> 8) & 0xff);
}

/*
 * vms-578: `stamp_connect_data` -- WHOSE connect data goes at [94:110].
 *
 * vms-fdd stamps the measured VMS$VAXcluster joiner connect data there for the
 * VMS$VAXcluster connect classes. p. 2-25 makes that region PER-SYSAP ("up to
 * 16 bytes ... passed to the destination SYSAP"), and the vms-760 MSCP$DISK
 * template carries a DIFFERENT, equally grounded value in it -- the ASCII class
 * descriptor 'V5.0          + ' replayed from formation-clean-2node SCA idx35.
 * Stamping VMS$VAXcluster's bytes over it would present MSCP$DISK's connect
 * with the connection manager's connect data, which appears on no capture.
 * Measured: test_build_mscp_request's byte-exact check against idx35 fails with
 * the stamp on and passes with it off, and that is the check that caught it.
 */
static int build_from_tmpl(const struct scs_connect_params *p,
                           const uint8_t tmpl[SCS_CONNECT_SCA_LEN],
                           uint32_t remote_conid,
                           uint16_t mtype,
                           int stamp_connect_data,
                           uint8_t out[SCS_CONNECT_FRAME_LEN])
{
    struct scs_env_fields env;

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
    /* vms-ec7: THE SCS MESSAGE ENVELOPE, from the one build path -- inner
     * length (derived), format word, MTYPE, credit, Con.ID pair.
     *
     * THE MTYPE IS THE POINT. These three templates are the CONNECT_REQ (0) and
     * ACCEPT_REQ (2) of Figure 2-14, and until this item the builder never
     * wrote the field that says so: the value came along inside whichever
     * captured template the caller picked, so a builder could be handed the
     * wrong template and emit a well-formed message of the wrong kind with
     * nothing in the program able to notice. It is now an argument.
     *
     * CREDIT: a LABELED REPLAY of the templates' [48:50], which is 10 in all
     * three. p. 2-43 makes this field the number of Send Credits the SYSAP is
     * EXTENDING at connection formation, and spec sec 4(g) matches the observed
     * values {1,3,6,8,10} to the SYSGEN tunables -- so 10 is a real extension
     * OVMX makes, not padding. It is NOT yet computed from the CDT's account;
     * scs_credit_extend() is where that would come from, and wiring it is a
     * wire-visible change this refactor deliberately does not make.
     */
    env.mtype = mtype;
    env.credit = SCS_CONNECT_ENV_CREDIT;
    env.dest_conid = remote_conid;
    env.src_conid = p->local_conid;
    if (scs_env_build_frame(out, SCS_CONNECT_FRAME_LEN, &env) != 0) {
        return -1;
    }

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
    if (stamp_connect_data && scs_connect_data_enabled()) {
        memcpy(out + SCS_CONNECT_DATA_ABS_OFF, scs_connect_data_vaxcluster,
               SCS_CONNECT_DATA_LEN);
    }

    return 0;
}

int scs_connect_build_request(const struct scs_connect_params *p,
                              uint8_t out[SCS_CONNECT_FRAME_LEN])
{
    /* CONNECT-REQUEST: remote Con.ID is always 0 (peer's not yet known). */
    return build_from_tmpl(p, connect_request_tmpl, 0,
                           SCS_CONN_MSGTYPE_CONNECT_REQ, 1, out);
}

int scs_connect_build_response(const struct scs_connect_params *p,
                               uint8_t out[SCS_CONNECT_FRAME_LEN])
{
    if (p == NULL) {
        return -1;
    }
    /* CONNECT-RESPONSE: echo the peer's Con.ID as remote. */
    return build_from_tmpl(p, connect_response_tmpl, p->remote_conid,
                           SCS_CONN_MSGTYPE_ACCEPT_REQ, 1, out);
}

int scs_connect_build_mscp_request(const struct scs_connect_params *p,
                                   uint8_t out[SCS_CONNECT_FRAME_LEN])
{
    /* MSCP$DISK CONNECT-REQUEST: remote Con.ID always 0 (peer's not yet known). */
    /* vms-578: MSCP$DISK carries its OWN [94:110]; do NOT stamp. */
    return build_from_tmpl(p, mscp_connect_request_tmpl, 0,
                           SCS_CONN_MSGTYPE_CONNECT_REQ, 0, out);
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

    /* vms-a61: Con.ID pair (abs 64/68 = content[50:58]) is grounded on the
     * SHARED ENVELOPE for ANY envelope-conformant frame (scs_env.h: "[50:54]
     * DESTINATION Con.ID / [54:58] SOURCE Con.ID", fixed offsets, no length
     * dependence) -- NOT only for the 110- and 190-byte classes this used to
     * special-case. That length pair was never the grounding; it was every
     * class this parser's callers happened to be handed at the time it was
     * written. scs_env_parse_frame() is the conformance test itself (inner
     * length == total-44 AND format word == 0x0004), which is a STRICTER and
     * more principled admission than the two-length allowlist it replaces:
     * every frame the old test admitted is envelope-conformant (110/190 are
     * two of the seven conformant classes), so nothing the old code accepted
     * is now refused.
     *
     * WHAT THIS WIDENS: the 58/62/66/86/94-content classes, which the old
     * length test excluded even though their Con.ID pair sits at the exact
     * same fixed offset and is exactly as grounded. A caller in scsd.c that
     * gates a CONNECT-RESPONSE emission on `has_conid` (branch (c)) will now
     * see it set for those classes too where before it did not -- this is a
     * behaviour change on frames scsd.c did not previously act on this way,
     * not a decode change: the field itself is unchanged and unconditionally
     * correct for every class in the conformant population. */
    struct scs_env cev;
    int conformant = (scs_env_parse_frame(frame, len, &cev) == 0);
    if (conformant) {
        v->has_conid = 1;
        v->remote_conid = cev.dest_conid; /* content[50:54], == old abs-64 read */
        v->local_conid = cev.src_conid;   /* content[54:58], == old abs-68 read */
    }

    /* vms-a61: [46:48] = abs 60:62, the SCA connection-control message type
     * (spec sec 4h(1a)) -- same field scs_env.h calls MTYPE. This used to be
     * an open-coded `frame[60] | frame[61]<<8` behind only a bare `len>=62`
     * check, the same unsound-read shape the has_conid widening above and
     * scsd.c's vms-ec7/vms-a61 migrations removed elsewhere: `len>=62` proves
     * the two bytes are READABLE, not that they are the MTYPE field of an
     * actual SCS message -- a frame of some other class that happens to be
     * >=62 bytes has two bytes there too, meaning something else. Read it
     * from the SAME parsed envelope as the Con.ID pair above, and only when
     * conformant. INERT today (conn_msgtype has no production consumer --
     * only the two asserts in test_scs_connect.c), flagged in the vms-ec7
     * post-merge audit and fixed here as the same bug class this item's
     * other two bullets fix. */
    if (conformant) {
        v->conn_msgtype = cev.mtype; /* content[46:48], == old abs-60 read */
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
