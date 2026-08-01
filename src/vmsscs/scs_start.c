/*
 * scs_start.c - VMS$VAXcluster phase-2 START/config (opcode 0x41) builder,
 * parser, and SCS sequenced-message counter state machine (vms-21e).
 * See scs_start.h for the full clean-room provenance and the
 * GROUNDED-vs-REPLAYED field breakdown.
 */
#include "scs_start.h"

#include <string.h>

/* Byte-exact 106-byte SCA content of a real JOINER round-0 START/config
 * (formation-ci1-joinwindow.pcap raw frame 24 / SCA#16, VAX2->VAX1:
 * SCSSYSTEMID 1026, node "VAX2    ", config-round 0). Substituted at build
 * time: dest logical [2:8], src logical [10:16], SCS counters [18:24] +
 * mirror [30:32], config-round [44:46], SCSSYSTEMID [46:48], node name
 * [90:98]. Every other byte is a labeled REPLAY of the captured joiner frame
 * (see scs_start.h): the [54:58] 0x0240/0x00d8 pair and the per-boot
 * incarnation tokens [66:71]/[98:104]. */
static const uint8_t scs_start_tmpl[SCS_START_SCA_LEN] = {
    0x68, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x00, 0xaa, 0x00,  /* [0:12] */
    0x04, 0x00, 0x02, 0x04, 0x41, 0x13, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,  /* [12:24] */
    0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,  /* [24:36] */
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x00, 0x00, 0x02, 0x04,  /* [36:48] */
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x40, 0x02, 0xd8, 0x00, 0x56, 0x4d,  /* [48:60] */
    0x53, 0x20, 0x56, 0x37, 0x2e, 0x33, 0xbb, 0x8e, 0x67, 0x7a, 0x94, 0x00,  /* [60:72] */
    0xbc, 0x00, 0x56, 0x41, 0x58, 0x20, 0x06, 0x00, 0x00, 0x0a, 0x00, 0x00,  /* [72:84] */
    0x00, 0x00, 0x00, 0x00, 0x77, 0x00, 0x56, 0x41, 0x58, 0x32, 0x20, 0x20,  /* [84:96] */
    0x20, 0x20, 0x40, 0x2a, 0xd3, 0x55, 0x96, 0x00, 0xbc, 0x00,             /* [96:106] */
};

/* Byte-exact 46-byte SCA content of the real JOINER round-2 ACK
 * (formation-ci1-joinwindow.pcap raw frame 28 / SCA#20, VAX2->VAX1). Inner
 * length [42:44]=2, config-round [44:46]=2. Same substitution scheme for the
 * envelope + counters; no identity body. */
static const uint8_t scs_start_ack_tmpl[SCS_START_ACK_SCA_LEN] = {
    0x2c, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x00, 0xaa, 0x00,  /* [0:12] */
    0x04, 0x00, 0x02, 0x04, 0x41, 0x13, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,  /* [12:24] */
    0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,  /* [24:36] */
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00,             /* [36:46] */
};

static void put_le16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xff);
    dst[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_le64(uint8_t *dst, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        dst[i] = (uint8_t)((v >> (8 * i)) & 0xff);
    }
}

static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

/* --- SCS sequenced-message counter state machine --- */

void scs_seq_init(struct scs_seq_state *s)
{
    if (s == NULL) {
        return;
    }
    s->send_seq = 1; /* a fresh VC's first sequenced message is #1 (GROUNDED joiner value) */
    s->recv_seq = 0;
}

void scs_seq_note_recv(struct scs_seq_state *s, uint16_t peer_send_seq)
{
    if (s == NULL) {
        return;
    }
    if (peer_send_seq > s->recv_seq) {
        s->recv_seq = peer_send_seq;
    }
}

uint16_t scs_seq_advance(struct scs_seq_state *s)
{
    if (s == NULL) {
        return 0;
    }
    uint16_t cur = s->send_seq;
    s->send_seq = (uint16_t)(s->send_seq + 1);
    return cur;
}

/* Lay down the Ethernet header + SCA template, then substitute the shared
 * envelope fields (dest/src logical, counters). Caller substitutes the
 * class-specific fields (round/sysid/name) afterward. */
static void build_common(const struct scs_start_params *p, const uint8_t *tmpl,
                         size_t sca_len, uint8_t *out)
{
    /* Ethernet header (abs 0-13). */
    memcpy(out + 0, p->dst_mac, 6);
    memcpy(out + 6, p->src_mac, 6);
    out[12] = 0x60;
    out[13] = 0x07;

    /* SCA content (abs 14+) from the captured joiner template. */
    memcpy(out + 14, tmpl, sca_len);

    /* Identity + counter substitutions (payload-relative offset + 14). */
    memcpy(out + 14 + 2, p->peer_logical, 6);  /* dest logical  [2:8]  (abs 16) */
    memcpy(out + 14 + 10, p->src_logical, 6);   /* src-logical [10:16](abs 24) = aa:00:04:00:<sysid>
                                                 * cluster-LOGICAL addr, NOT raw HW MAC (vms-9f3) */
    put_le16(out + 14 + 18, p->recv_ack);       /* leading counter [18:20] (0 during START) */
    put_le16(out + 14 + 20, p->send_seq);       /* SCS send-seq    [20:22] */
    put_le16(out + 14 + 22, p->incarnation);    /* node-incarnation [22:24] -- echoes the member's
                                                 * directed-HELLO [78:80]; the established-join gate
                                                 * (spec sec 4i.B). 1 for a fresh contact, so this
                                                 * matched send_seq=1 in every fresh-formation
                                                 * specimen -- it is the incarnation, not send_seq. */
    put_le16(out + 14 + 30, p->send_seq);       /* send-seq mirror [30:32] (== [20:22], GROUNDED) */
}

int scs_start_build(const struct scs_start_params *p,
                    uint8_t out[SCS_START_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    size_t namelen = strnlen(p->node_name, SCS_START_NODENAME_LEN + 1);
    if (namelen > SCS_START_NODENAME_LEN) {
        return -1;
    }

    build_common(p, scs_start_tmpl, SCS_START_SCA_LEN, out);

    /* Class-specific: config-round, SCSSYSTEMID, node name. */
    put_le16(out + 14 + 44, p->config_round);   /* config-round counter [44:46] */
    put_le16(out + 14 + 46, p->scssystemid);    /* SCSSYSTEMID (LE u16) [46:48] */

    /* Software-version [58:66]: overwrite the golden template's "VMS V7.3" with
     * OVMX's OWN identity (vms-d94). This is the SHOW CLUSTER SOFTWARE column;
     * OVMX advertises what it IS, not a VMS masquerade (authenticity INV-0). The
     * member does not validate it -- live-verified OVMX reaches status NEW with
     * it. SCS_START_SW_VERSION is a fixed 8-byte string (no NUL on the wire). */
    memcpy(out + 14 + 58, SCS_START_SW_VERSION, SCS_START_NODENAME_LEN);

    /* THIS SYSTEM'S INCARNATION [66:74] -- a VMS absolute-time quadword, live
     * per boot (vms-2f3). The template replays the captured joiner's own value,
     * 0x00bc00947a678ebb = 26-JUL-2026 14:35:33.59, and shipping it verbatim
     * made OVMX the only node on the wire whose incarnation never changed.
     *
     * GROUNDED that this is the field: SDA on the real VAX1 renders OVMX's CSB
     * as "Incarnation 26-JUL-2026 14:35:33" -- the template bytes to the second
     * -- while every real peer's CSB carries its own boot time (VAX3:
     * "Incarnation 1-AUG-2026 00:02:21"). OVMX transmits this quadword nowhere
     * else.
     *
     * WHY IT IS LOAD-BEARING, not cosmetic: a peer distinguishes a REBOOTED
     * system from one whose virtual circuit merely broke by the incarnation it
     * advertises. Holding a CSB in state wait/long_break for a crash-removed
     * OVMX and then seeing a returning OVMX present the IDENTICAL incarnation,
     * VAX1 reused the SAME System Block (SDA: "SB address 87A0C580", byte-equal
     * across the dead-CSB dump and the rejoin attempt) -- and with it the old
     * VC sequence state ("Next seq. number 0003 / Last ack. seq num 0000",
     * Resend queue non-empty) while OVMX had reset its own VC to send_seq=1.
     * The transition it opened for us was then abandoned: "timed-out lost
     * connection to node OVMXC2" on all three peers.
     *
     * A 0 leaves the replayed template bytes in place. That is the control arm
     * for the experiment only -- the shipped default is live. */
    if (p->incarnation_time != 0) {
        put_le64(out + 14 + 66, p->incarnation_time);
    }

    /* MESSAGE TIMESTAMP [98:106] -- the OTHER quadword in this frame, and a
     * different kind of field: not identity, but when this frame was composed.
     * GROUNDED live-rolling on the reference wire (each real peer carries 2-3
     * distinct values inside one capture; one of VAX3's matches to the second
     * the OPCOM line it printed as it built the frame). The template's replayed
     * value is 26-JUL-2026 14:48:50.66, exactly 13m17s after the template's
     * incarnation -- the captured node's boot-to-this-frame gap, which is
     * meaningless for us.
     *
     * Its ROLE is NOT grounded and OVMX does not claim one. What is grounded is
     * the negative: no real node ever sends a stale one, and OVMX sent a
     * six-day-old value on every frame of every run. 0 leaves the template
     * bytes (control arm). */
    if (p->message_time != 0) {
        put_le64(out + 14 + 98, p->message_time);
    }
    {
        /* Node name: fixed 8-byte, blank-padded, left-justified [90:98]
         * (GROUNDED distinct encoding vs. HELLO's length-prefixed form). */
        uint8_t namebuf[SCS_START_NODENAME_LEN];
        memset(namebuf, ' ', sizeof(namebuf));
        memcpy(namebuf, p->node_name, namelen);
        memcpy(out + 14 + 90, namebuf, SCS_START_NODENAME_LEN);
    }

    return 0;
}

int scs_start_build_ack(const struct scs_start_params *p,
                        uint8_t out[SCS_START_ACK_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }

    build_common(p, scs_start_ack_tmpl, SCS_START_ACK_SCA_LEN, out);

    /* The ACK's config-round is always 2 (the template already carries it;
     * assert it explicitly so the field is intentional, not incidental). */
    put_le16(out + 14 + 44, SCS_START_ACK_ROUND);

    return 0;
}

int scs_start_parse(const uint8_t *frame, size_t len, struct scs_start_view *v)
{
    if (frame == NULL || v == NULL) {
        return -1;
    }
    /* Need the Ethernet header + the SCA envelope through the config-round
     * (abs 14+44 = 58). */
    if (len < 60) {
        return -1;
    }
    if (frame[30] != SCS_START_OPCODE) {
        return -1;
    }

    memset(v, 0, sizeof(*v));

    uint16_t lenword = get_le16(frame + 14);
    v->total_sca_len = (uint16_t)(lenword + 2);
    v->opcode = frame[30];
    v->format = frame[31];
    v->recv_ack = get_le16(frame + 14 + 18);
    v->send_seq = get_le16(frame + 14 + 20);
    v->config_round = get_le16(frame + 14 + 44);

    if (v->total_sca_len == SCS_START_SCA_LEN && len >= 14 + 48) {
        v->is_ack = 0;
        v->scssystemid = get_le16(frame + 14 + 46);
    } else {
        v->is_ack = 1; /* the 46-byte round-2 ack carries no identity body */
    }

    return 0;
}
