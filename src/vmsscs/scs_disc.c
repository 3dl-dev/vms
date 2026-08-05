/*
 * scs_disc.c - vms-591: the DISCONNECT_REQ / DISCONNECT_RSP builders.
 *
 * Provenance, the measurement that grounds both frames, the [60:62] matching
 * flag, the replayed-vs-substituted split and the kill switch are all in
 * scs_disc.h. Read that first.
 *
 * Re-derive every figure quoted there with tools/cluster/scs_disc_measure.py
 * on a host with the lab captures.
 */
#include "scs_disc.h"

#include <stdlib.h>
#include <string.h>

#include "scs_reason.h" /* SCS_REASON_PAYLOAD_OFF -- one definition, not two */

/* ------------------------------------------------------------------ *
 * The two byte-exact captured templates (SCA content only, no Ethernet
 * header). formation-ci1.pcap, VAX2 -> VAX1, one teardown dialogue.
 * ------------------------------------------------------------------ */

/* DISCONNECT_REQ: raw frame 64 (SCA #56). This is the MATCHING half of the
 * dialogue, so its [60:62] reads 0x0001; the builder overwrites it either way. */
static const uint8_t disc_request_tmpl[SCS_DISC_REQ_SCA_LEN] = {
    0x3c, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,   /* [00] len=0x003c, dest logical */
    0x01, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,   /* [08] 0x0001, src logical */
    0x4b, 0x13, 0x0e, 0x00, 0x0f, 0x00, 0x01, 0x00,   /* [16] opcode, format, ack, seq, incarn */
    0x12, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x0f, 0x00,   /* [24] NISCS_LAN_OVRHD, ack, -, seq */
    0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x01, 0x00,   /* [32] -, ack, -, 01 00 */
    0x00, 0x02, 0x12, 0x00, 0x04, 0x00, 0x06, 0x00,   /* [40] 00 02, inner=0x12, 04 00, msgtype 6 */
    0x00, 0x00, 0x08, 0x00, 0x05, 0x63, 0x07, 0x00,   /* [48] 00 00, remote Con.ID, local... */
    0x59, 0x33, 0x00, 0x00, 0x01, 0x00                /* [56] ...Con.ID, reason=0, matching=1 */
};

/* DISCONNECT_RSP: raw frame 63 (SCA #55). Identical envelope, 4 bytes shorter,
 * inner length 0x000e instead of 0x0012, message type 7 instead of 6. */
static const uint8_t disc_response_tmpl[SCS_DISC_RSP_SCA_LEN] = {
    0x38, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,   /* [00] len=0x0038, dest logical */
    0x01, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,   /* [08] 0x0001, src logical */
    0x4b, 0x13, 0x0d, 0x00, 0x0e, 0x00, 0x01, 0x00,   /* [16] opcode, format, ack, seq, incarn */
    0x12, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x0e, 0x00,   /* [24] NISCS_LAN_OVRHD, ack, -, seq */
    0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x01, 0x00,   /* [32] -, ack, -, 01 00 */
    0x00, 0x02, 0x0e, 0x00, 0x04, 0x00, 0x07, 0x00,   /* [40] 00 02, inner=0x0e, 04 00, msgtype 7 */
    0x00, 0x00, 0x08, 0x00, 0x05, 0x63, 0x07, 0x00,   /* [48] 00 00, remote Con.ID, local... */
    0x59, 0x33                                        /* [56] ...Con.ID. The frame ENDS here. */
};

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

int scs_disc_enabled(void)
{
    const char *e = getenv("OVMX_NO_CLEAN_SHUTDOWN");
    if (e == NULL || e[0] == '\0') {
        return 1;
    }
    return (e[0] == '0' && e[1] == '\0') ? 1 : 0;
}

/*
 * The common half. `sca_len` selects the class; everything in here is at the
 * same payload offset in both frames, which is the census's own finding.
 */
static int build_common(const struct scs_disc_params *p, const uint8_t *tmpl,
                        size_t sca_len, uint8_t *out)
{
    uint8_t *pl;

    if (p == NULL || out == NULL) {
        return -1;
    }
    /* THE KILL SWITCH IS ENFORCED HERE, not at the call site, so that no
     * present or future caller can emit a DISCONNECT frame around it. */
    if (!scs_disc_enabled()) {
        return -1;
    }

    memcpy(out + 0, p->dst_mac, 6);
    memcpy(out + 6, p->src_mac, 6);
    out[12] = 0x60;
    out[13] = 0x07;
    memcpy(out + 14, tmpl, sca_len);
    pl = out + 14;

    /* Identity (spec sec 4g: the cluster-LOGICAL addresses, not the HW MACs). */
    memcpy(pl + 2, p->peer_logical, 6);
    memcpy(pl + 10, p->src_logical, 6);

    /* DERIVED lengths -- recomputed, never copied. [0:2] is the SCA length
     * field, which is (total SCA content - 2); [42:44] is the sec 4h inner
     * length, payload_len - 44. Both agree with the captured templates. */
    put_le16(pl + 0, (uint16_t)(sca_len - 2));
    put_le16(pl + 42, (uint16_t)(sca_len - 44));

    /* Live SCS VC counters (spec sec 4h(4)); the [20:22]==[30:32] mirror is
     * GROUNDED, and the three recv_ack copies are the same field three times. */
    put_le16(pl + 18, p->recv_ack);
    put_le16(pl + 20, p->send_seq);
    if (p->incarnation != 0) {
        put_le16(pl + 22, p->incarnation);
    }
    put_le16(pl + 26, p->recv_ack);
    put_le16(pl + 30, p->send_seq);
    put_le16(pl + 34, p->recv_ack);

    /* The Con.ID pair (spec sec 4d/4g/4h, same offsets as every other
     * connection-control frame). */
    put_le32(pl + 50, p->remote_conid);
    put_le32(pl + 54, p->local_conid);
    return 0;
}

int scs_disc_build_request(const struct scs_disc_params *p,
                           uint8_t out[SCS_DISC_REQ_FRAME_LEN])
{
    if (build_common(p, disc_request_tmpl, SCS_DISC_REQ_SCA_LEN, out) != 0) {
        return -1;
    }
    /* [58:60] the reason code, written by vms-6b3's OWN CODEC -- not by a
     * second copy of the same two stores. That matters for more than tidiness:
     * scs_reason_put() enforces OVMX_NO_REASON_CODE, so the reason-code kill
     * switch gates this frame too, and the template's own 0x0000 stands (which
     * is what every VMS DISCONNECT_REQ we hold carries). Writing the bytes
     * directly here would have left vms-6b3's switch unable to gate the one
     * frame OVMX actually puts a reason code on. */
    (void)scs_reason_put(out, SCS_DISC_REQ_FRAME_LEN,
                         SCS_REASON_MSGTYPE_DISCONNECT_REQ, p->reason);
    /* [60:62] the matching flag -- newly grounded, see scs_disc.h. */
    put_le16(out + SCS_DISC_MATCH_FRAME_OFF,
             p->matching ? SCS_DISC_MATCH_MATCHING : SCS_DISC_MATCH_INITIAL);
    return 0;
}

int scs_disc_build_response(const struct scs_disc_params *p,
                            uint8_t out[SCS_DISC_RSP_FRAME_LEN])
{
    /* No reason code and no matching flag: the 58-byte class ENDS at the
     * Con.ID pair. Writing either would be inventing a field. */
    return build_common(p, disc_response_tmpl, SCS_DISC_RSP_SCA_LEN, out);
}

int scs_disc_match_get(const uint8_t *frame, size_t len, uint16_t *out)
{
    unsigned msgtype;

    if (frame == NULL || out == NULL || len < SCS_DISC_REQ_FRAME_LEN) {
        return 0;
    }
    msgtype = (unsigned)frame[14 + 46] | ((unsigned)frame[14 + 47] << 8);
    if (msgtype != SCS_DISC_MSGTYPE_REQ) {
        return 0;
    }
    *out = (uint16_t)((unsigned)frame[SCS_DISC_MATCH_FRAME_OFF] |
                      ((unsigned)frame[SCS_DISC_MATCH_FRAME_OFF + 1] << 8));
    return 1;
}
