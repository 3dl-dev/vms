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

#include "scs_env.h"    /* vms-ec7: THE shared SCS message envelope */
#include "scs_reason.h" /* SCS_REASON_PAYLOAD_OFF -- one definition, not two */

/*
 * vms-ec7: the envelope fields of the two teardown classes, named.
 *
 * Before this item the MTYPE and the credit of a DISCONNECT frame were whatever
 * byte the captured template carried at [46:48] and [48:50]: the builder never
 * wrote either, so the program could not say which SCA message it was emitting.
 * Both are now explicit inputs to scs_env_build(), and both are the SAME VALUES
 * the templates carry -- the frames are byte-identical, which is what
 * test_scs_disc.c's byte-exact checks against the captured SCA content assert.
 *
 * CREDIT 0 IS GROUNDED, not a default: types 6 and 7 carry credit 0 in 100% of
 * the 5,257-frame real-VAX population (scs_rx.h census; spec sec 4(h)(1c)),
 * which is the structural signature p. 4-68 predicts for a class that does not
 * extend send credits.
 */
#define SCS_DISC_ENV_CREDIT 0u

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
                        size_t sca_len, uint16_t mtype, uint8_t *out)
{
    uint8_t *pl;
    struct scs_env_fields env;

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

    /* [0:2] is the SCA length field, (total SCA content - 2). DERIVED --
     * recomputed, never copied. It belongs to the frame class, not to the SCS
     * envelope, so it stays here; the inner length that used to sit next to it
     * moved into scs_env_build() below, which derives it the same way for every
     * class in the tree. */
    put_le16(pl + 0, (uint16_t)(sca_len - 2));

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

    /* vms-ec7: THE SCS MESSAGE ENVELOPE, from the one build path. This replaces
     * the inner-length store above and the Con.ID pair below, and additionally
     * writes the two fields this builder never wrote at all -- the MTYPE that
     * names the message and the credit field. */
    env.mtype = mtype;
    env.credit = SCS_DISC_ENV_CREDIT;
    env.dest_conid = p->remote_conid;
    env.src_conid = p->local_conid;
    if (scs_env_build(pl, sca_len, &env) != 0) {
        return -1;
    }
    return 0;
}

int scs_disc_build_request(const struct scs_disc_params *p,
                           uint8_t out[SCS_DISC_REQ_FRAME_LEN])
{
    if (build_common(p, disc_request_tmpl, SCS_DISC_REQ_SCA_LEN,
                     SCS_DISC_MSGTYPE_REQ, out) != 0) {
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
    return build_common(p, disc_response_tmpl, SCS_DISC_RSP_SCA_LEN,
                        SCS_DISC_MSGTYPE_RSP, out);
}

int scs_disc_match_get(const uint8_t *frame, size_t len, uint16_t *out)
{
    uint16_t msgtype;

    if (frame == NULL || out == NULL || len < SCS_DISC_REQ_FRAME_LEN) {
        return 0;
    }
    /* vms-ec7: the MTYPE comes from the shared envelope, which ALSO applies the
     * sec 4(h)(1b) conformance test. The open-coded frame[14+46] read this
     * replaces would happily return a "message type" from a frame that has no
     * envelope at all -- the 70-content class reads 1..22 there. */
    if (!scs_env_mtype_of_frame(frame, len, &msgtype)) {
        return 0;
    }
    if (msgtype != SCS_DISC_MSGTYPE_REQ) {
        return 0;
    }
    *out = (uint16_t)((unsigned)frame[SCS_DISC_MATCH_FRAME_OFF] |
                      ((unsigned)frame[SCS_DISC_MATCH_FRAME_OFF + 1] << 8));
    return 1;
}
