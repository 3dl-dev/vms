/*
 * scs_dlm.c - Distributed Lock Manager over SCS: the DLM SYSAP message class
 * (vms-94c, DLM epic vms-7fa rung 1).
 *
 * See scs_dlm.h for the full clean-room provenance. In short:
 *   - the SEMANTIC field VALUES (LCK$K_ modes, LCK$M_ flags, the 16-byte value
 *     block) are AUTHENTIC $LCKDEF from lckdef.h (oracle-pinned);
 *   - the BYTE LAYOUT of the DLM body [58:138] is a ⚠ LABELLED OVMX DESIGN
 *     CHOICE, because VSI/HPE do not publish the lock manager's SCS message
 *     byte layout and Rule 8 forbids obtaining it by disassembly.
 *
 * The [0:42] NISCA sequenced-message header and the [42:58] SCS envelope are the
 * SHARED transport, byte-identical to the header every other SYSAP application
 * message rides (scs_mscp.c, the data-phase directory frames). Nothing in that
 * header is DLM-specific: a received frame is told apart as DLM ONLY by the
 * destination Con.ID resolving to the DLM CDT (scs_cdl_deliver_message), never
 * by a byte in the header.
 */
#include "scs_dlm.h"

#include <string.h>

#include "scs_env.h" /* vms-ec7: THE shared SCS message envelope */

/*
 * THE SHARED NISCA/PPD SEQUENCED-MESSAGE HEADER, content [0:58].
 *
 * This is the SAME 0x4b-class sequenced-message header scs_mscp.c replays for an
 * MSCP command -- it is the transport layer, not a DLM artifact. The identity
 * fields ([2:8] dest logical, [10:16] src logical), the sequencing fields
 * ([18:20]/[20:22]/[22:24] and their mirrors) and the [42:58] SCS envelope are
 * all overwritten at build time; the bytes left captured here are the undecoded
 * 0x4b-class PPD/NISCA fields ([8:10], [16:18], [24:26], [36:42]) that are not
 * this module's layer (identical provenance to scs_mscp.c's mscp_sca_hdr). They
 * are left at their observed values so a build that forgets a substitution is
 * caught by the round-trip test rather than silently emitting a zero.
 */
static const uint8_t dlm_sca_hdr[SCS_DLM_BODY_OFF] = {
    /* [0:2]   SCA content length - 2 (derived at build time)               */
    0x88, 0x00,
    /* [2:8]   destination logical address        (substituted)             */
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,
    /* [8:10]  REPLAY -- undecoded 0x4b-class PPD field                     */
    0x01, 0x00,
    /* [10:16] source logical address             (substituted)             */
    0xaa, 0x00, 0x04, 0x00, 0x1a, 0x04,
    /* [16:18] REPLAY -- PPD marker 0x4b / format 0x13 (NOT the SCS MTYPE)  */
    0x4b, 0x13,
    /* [18:24] recv_ack / send_seq / incarnation   (substituted)            */
    0x18, 0x00, 0x19, 0x00, 0x01, 0x00,
    /* [24:26] REPLAY -- undecoded                                          */
    0x12, 0x00,
    /* [26:28] recv_ack mirror                     (substituted)            */
    0x18, 0x00,
    /* [28:30] REPLAY -- undecoded                                          */
    0x00, 0x00,
    /* [30:32] send_seq mirror                     (substituted)            */
    0x19, 0x00,
    /* [32:34] REPLAY -- undecoded                                          */
    0x00, 0x00,
    /* [34:36] recv_ack mirror                     (substituted)            */
    0x18, 0x00,
    /* [36:42] REPLAY -- undecoded 0x4b-class PPD fields                    */
    0x00, 0x00, 0x01, 0x00, 0x00, 0x02,
    /* [42:58] the SCS ENVELOPE -- overwritten by scs_env_build_frame()     */
    0x32, 0x00, 0x04, 0x00, 0x0a, 0x00, 0x01, 0x00,
    0x0a, 0x00, 0x54, 0x35, 0x08, 0x00, 0xd2, 0x8f,
};

static void put_le16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xff);
    dst[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_le32(uint8_t *dst, uint32_t v)
{
    put_le16(dst, (uint16_t)(v & 0xffffu));
    put_le16(dst + 2, (uint16_t)((v >> 16) & 0xffffu));
}

static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)get_le16(src) | ((uint32_t)get_le16(src + 2) << 16);
}

/* ======================= the DLM message body ============================= */

int scs_dlm_build_body(const struct scs_dlm_msg *m, uint8_t *body, size_t body_len)
{
    if (m == NULL || body == NULL || body_len < SCS_DLM_BODY_LEN) {
        return -1;
    }
    /* op must be one of the four rung-1 kinds. */
    if (m->op != SCS_DLM_OP_ENQ && m->op != SCS_DLM_OP_GRANT &&
        m->op != SCS_DLM_OP_DEQ && m->op != SCS_DLM_OP_BLKAST &&
        m->op != SCS_DLM_OP_REBUILD && m->op != SCS_DLM_OP_DLKSRCH) {
        return -1;
    }
    /* mode is an authentic $LCKDEF grant mode, 0..LCK$K_EXMODE. */
    if (m->mode > LCK$K_EXMODE) {
        return -1;
    }
    /* A resource name longer than a $ENQ resource name (31 bytes) is malformed. */
    if (m->namelen > SCS_DLM_RESNAM_MAX) {
        return -1;
    }

    /* Zero first, then write only defined fields, so every reserved byte and the
     * unused name tail is 0 by construction (matches scs_mscp_build_body). */
    memset(body, 0, body_len);

    body[SCS_DLM_B_OP]   = m->op;
    body[SCS_DLM_B_MODE] = m->mode;
    put_le16(body + SCS_DLM_B_FLAGS, m->flags);
    put_le32(body + SCS_DLM_B_REQ_LKID, m->req_lkid);
    put_le32(body + SCS_DLM_B_MASTER_LKID, m->master_lkid);
    put_le32(body + SCS_DLM_B_STATUS, m->status);
    put_le32(body + SCS_DLM_B_REQ_CSID, m->req_csid);
    put_le32(body + SCS_DLM_B_MASTER_CSID, m->master_csid);
    body[SCS_DLM_B_NAMELEN]     = m->namelen;
    body[SCS_DLM_B_PARENT_PRES] = m->parent_present ? 1u : 0u;
    put_le32(body + SCS_DLM_B_PARENT_LKID, m->parent_lkid);
    memcpy(body + SCS_DLM_B_VALBLK, m->valblk, LCK$C_VALBLK_LEN);
    /* Copy only the valid name bytes; the tail stays zero from the memset. */
    memcpy(body + SCS_DLM_B_RESNAM, m->resnam, m->namelen);
    return 0;
}

int scs_dlm_parse_body(const uint8_t *body, size_t body_len, struct scs_dlm_msg *m)
{
    if (body == NULL || m == NULL || body_len < SCS_DLM_BODY_LEN) {
        return -1;
    }
    uint8_t namelen = body[SCS_DLM_B_NAMELEN];
    if (namelen > SCS_DLM_RESNAM_MAX) {
        return -1; /* malformed -- refuse rather than read a bogus length */
    }

    memset(m, 0, sizeof(*m));
    m->op          = body[SCS_DLM_B_OP];
    m->mode        = body[SCS_DLM_B_MODE];
    m->flags       = get_le16(body + SCS_DLM_B_FLAGS);
    m->req_lkid    = get_le32(body + SCS_DLM_B_REQ_LKID);
    m->master_lkid = get_le32(body + SCS_DLM_B_MASTER_LKID);
    m->status      = get_le32(body + SCS_DLM_B_STATUS);
    m->req_csid    = get_le32(body + SCS_DLM_B_REQ_CSID);
    m->master_csid = get_le32(body + SCS_DLM_B_MASTER_CSID);
    m->parent_present = body[SCS_DLM_B_PARENT_PRES] ? 1u : 0u;
    m->parent_lkid = get_le32(body + SCS_DLM_B_PARENT_LKID);
    memcpy(m->valblk, body + SCS_DLM_B_VALBLK, LCK$C_VALBLK_LEN);
    m->namelen = namelen;
    /* The whole fixed 32-byte name field, so the caller sees the padding too. */
    memcpy(m->resnam, body + SCS_DLM_B_RESNAM, SCS_DLM_RESNAM_FIELD);
    return 0;
}

/* ==================== the frame the message rides in ===================== */

int scs_dlm_build_frame(const struct scs_dlm_params *p, const struct scs_dlm_msg *m,
                        uint8_t out[SCS_DLM_FRAME_LEN])
{
    if (p == NULL || m == NULL || out == NULL) {
        return -1;
    }

    /* Ethernet header (abs 0-13). */
    memcpy(out + 0, p->dst_mac, 6);
    memcpy(out + 6, p->src_mac, 6);
    out[12] = 0x60;
    out[13] = 0x07;

    /* Shared NISCA sequenced-message header (abs 14+). */
    memcpy(out + 14, dlm_sca_hdr, SCS_DLM_BODY_OFF);

    /* [0:2] is DERIVED, not replayed: SCA content length counts bytes after it. */
    put_le16(out + 14 + 0, (uint16_t)(SCS_DLM_SCA_LEN - 2));

    /* Identity substitutions (SCA-content offsets + 14). */
    memcpy(out + 14 + 2, p->peer_logical, 6);  /* dest logical [2:8]  */
    memcpy(out + 14 + 10, p->src_logical, 6);  /* src-logical [10:16] */

    /* SCS sequenced-message counters (spec sec 4h): recv_ack at [18:20] repeated
     * at [26:28]/[34:36]; send_seq at [20:22] mirrored at [30:32]. The
     * incarnation echo at [22:24]: 0 leaves the header's fresh-contact value 1. */
    put_le16(out + 14 + 18, p->recv_ack);
    put_le16(out + 14 + 20, p->send_seq);
    if (p->incarnation != 0) {
        put_le16(out + 14 + 22, p->incarnation);
    }
    put_le16(out + 14 + 26, p->recv_ack);
    put_le16(out + 14 + 30, p->send_seq);
    put_le16(out + 14 + 34, p->recv_ack);

    /* vms-ec7: THE SCS MESSAGE ENVELOPE, from the one build path. A DLM message
     * is a p. 4-13 application message: MTYPE 10. A credit of 0 falls back to the
     * replayed header default, matching how scs_mscp.c handles a no-CDT send. */
    {
        struct scs_env_fields env;
        env.mtype = SCS_ENV_MTYPE_APP_MESSAGE;
        env.credit = p->credit;
        env.dest_conid = p->remote_conid;
        env.src_conid = p->local_conid;
        (void)scs_env_build_frame(out, SCS_DLM_FRAME_LEN, &env);
    }

    /* The DLM message body, field by field at its OVMX offsets. */
    return scs_dlm_build_body(m, out + 14 + SCS_DLM_BODY_OFF, SCS_DLM_BODY_LEN);
}

/* ================================ parse ================================== */

int scs_dlm_parse(const uint8_t *frame, size_t len, struct scs_dlm_view *v)
{
    if (frame == NULL || v == NULL) {
        return -1;
    }
    /* The whole frame must be readable: 14 Ethernet + 138 SCA. */
    if (len < (size_t)SCS_DLM_FRAME_LEN) {
        return -1;
    }

    /* vms-ec7: the SCS envelope half of this parse comes from the shared path,
     * which also applies the sec 4(h)(1b) conformance test. A frame that is not
     * envelope-conformant is not a DLM-over-SCS message and is refused here. */
    struct scs_env env;
    if (scs_env_parse_frame(frame, len, &env) != 0) {
        return -1;
    }
    /* A DLM message is always the p. 4-13 application message (MTYPE 10). */
    if (env.mtype != SCS_ENV_MTYPE_APP_MESSAGE) {
        return -1;
    }

    memset(v, 0, sizeof(*v));
    v->total_sca_len = env.total_sca_len;
    v->scs_mtype = env.mtype;
    v->credit = env.credit;
    v->remote_conid = env.dest_conid;
    v->local_conid = env.src_conid;
    v->recv_ack = get_le16(frame + 14 + 18);
    v->send_seq = get_le16(frame + 14 + 20);

    return scs_dlm_parse_body(frame + 14 + SCS_DLM_BODY_OFF, SCS_DLM_BODY_LEN,
                              &v->msg);
}

/* ============================== names ==================================== */

const char *scs_dlm_op_name(uint8_t op)
{
    switch (op) {
    case SCS_DLM_OP_ENQ:    return "ENQ";
    case SCS_DLM_OP_GRANT:  return "GRANT";
    case SCS_DLM_OP_DEQ:    return "DEQ";
    case SCS_DLM_OP_BLKAST: return "BLKAST";
    case SCS_DLM_OP_REBUILD: return "REBUILD";
    case SCS_DLM_OP_DLKSRCH: return "DLKSRCH";
    default:                break;
    }
    return "?";
}

const char *scs_dlm_mode_name(uint8_t mode)
{
    switch (mode) {
    case LCK$K_NLMODE: return "NL";
    case LCK$K_CRMODE: return "CR";
    case LCK$K_CWMODE: return "CW";
    case LCK$K_PRMODE: return "PR";
    case LCK$K_PWMODE: return "PW";
    case LCK$K_EXMODE: return "EX";
    default:           break;
    }
    return "?";
}
