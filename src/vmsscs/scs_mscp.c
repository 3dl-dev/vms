/*
 * scs_mscp.c - MSCP-over-SCS disk-client command builder/parser (vms-760,
 * promoted to a field-based client by vms-533 / MSCP epic Phase B).
 * See scs_mscp.h for the full clean-room provenance.
 *
 * WHAT IS BUILT FROM FIELDS AND WHAT IS STILL A REPLAY -- read this before
 * changing a byte here:
 *
 *   [58:94] THE MSCP MESSAGE -- 100% FIELD-BUILT. Command reference number,
 *           unit, opcode, modifiers and the SET CONTROLLER CHARACTERISTICS
 *           parameter area are laid down at their AA-L619A-TK Table A-6 offsets
 *           from struct scs_mscp_cmd. No captured byte survives in this range.
 *
 *   [42:58] THE SCS ENVELOPE -- built by scs_env_build_frame() (vms-ec7 /
 *           Phase A): derived inner length, format word, MTYPE, credit, Con.ID
 *           pair.
 *
 *   [0:42]  THE SCA/PPD HEADER -- a labeled REPLAY of the golden af2 joiner
 *           frame, with the identity fields ([2:8] dest logical, [10:16] src
 *           logical) and the sequencing fields ([18:20]/[20:22]/[22:24] and
 *           their mirrors at [26:28]/[30:32]/[34:36]) substituted. The bytes
 *           that remain captured are the 0x4b-class PPD/NISCA fields nobody has
 *           decoded yet -- [8:10], [16:18], [24:26], [36:42]. They are NOT this
 *           module's layer and are not Phase B's scope.
 *
 * The golden frames are af2-firsttimer-established-20260728.pcap (a first-timer
 * joining an established VAX1). tests/vmsscs/test_scs_mscp.c holds them and
 * requires the field-built frames to come back byte-identical, which is what
 * makes this refactor provably wire-neutral.
 */
#include "scs_mscp.h"

#include <string.h>

#include "scs_env.h" /* vms-ec7: THE shared SCS message envelope */
#include "scs_cdt.h" /* vms-8de: struct scs_cdt::send_credit, the live credit read */
#include "scs_member.h" /* vms-020: scs_member_vms_time_now() for P.TIME */

/*
 * THE SCA/PPD HEADER of the 94-content MSCP command class: SCA content [0:58],
 * byte-exact from the golden af2 joiner commands. The SCC and GUS captures are
 * IDENTICAL over this range once the identity and sequencing fields are taken
 * out, which is why there is one of these and not two -- the two commands
 * differed only in their MSCP body, and the MSCP body is now built.
 *
 * Positions overwritten at build time are left at their captured values here so
 * that a build that forgets one is caught by the byte-exact test rather than
 * silently emitting a zero.
 */
static const uint8_t mscp_sca_hdr[SCS_MSCP_BODY_OFF] = {
    /* [0:2]   SCA content length - 2 (derived at build time)               */
    0x5c, 0x00,
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

static void put_le64(uint8_t *dst, uint64_t v)
{
    put_le32(dst, (uint32_t)(v & 0xffffffffu));
    put_le32(dst + 4, (uint32_t)((v >> 32) & 0xffffffffu));
}

static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)get_le16(src) | ((uint32_t)get_le16(src + 2) << 16);
}

/* ======================= the MSCP message itself ========================= */

int scs_mscp_build_body(const struct scs_mscp_cmd *c, uint8_t *body,
                        size_t body_len)
{
    if (c == NULL || body == NULL || body_len < SCS_MSCP_BODY_LEN) {
        return -1;
    }
    /* sec 5.1: the command reference number is "a 32-bit, unique, NON-ZERO
     * number identifying a host command". A zero would be indistinguishable
     * from the value an MSCP server supplies in an unsolicited message. */
    if (c->cmd_ref == 0) {
        return -1;
    }
    /* sec 5.1 / Table A-1: OP.END makes a message an END MESSAGE. This module
     * is the disk CLIENT -- it sends commands. Answering commands is Phase D
     * (vms-291) and refusing here is what keeps that boundary honest rather
     * than letting a caller emit a malformed half-response.
     *
     * PHASE D LANDED AND THIS REFUSAL DELIBERATELY STAYS. vms-291 builds end
     * messages in scs_mscp_srv.c with its own builder rather than opening this
     * seam, because a command and an end message DO NOT SHARE A LAYOUT beyond
     * the 12-byte header: the parameter areas come from two different tables
     * (A-6 for commands, A-7 for end messages) and disagree field by field --
     * P.MOD vs P.STS at body[10], P.HTMO at 16 in an SCC command against
     * P.CTMO at 16 in its end message, and the whole GET UNIT STATUS
     * characteristics block that has no command-side counterpart at all. One
     * builder switching on OP.END would have to carry both tables and would
     * make the client able to emit a response by accident. Two builders, one
     * table each. */
    if ((c->opcode & SCS_MSCP_END_BIT) != 0) {
        return -1;
    }

    /* sec 5.2: "class drivers must supply 0 in all reserved fields of messages
     * sent to a controller". Zero first, then write only defined fields -- so
     * every reserved byte and the unused tail of the parameter area is 0 by
     * construction and not by a template's good luck. */
    memset(body, 0, body_len);

    put_le32(body + SCS_MSCP_P_CRF, c->cmd_ref);
    put_le16(body + SCS_MSCP_P_UNIT, c->unit);
    body[SCS_MSCP_P_OPCD] = c->opcode;
    put_le16(body + SCS_MSCP_P_MOD, c->modifiers);

    if (c->opcode == SCS_MSCP_OP_SET_CTLR_CHAR) {
        /* Table A-6 SET CONTROLLER CHARACTERISTICS parameter area (sec 6.16). */
        put_le16(body + SCS_MSCP_P_VRSN, c->scc_version);
        put_le16(body + SCS_MSCP_P_CNTF, c->scc_ctlr_flags);
        put_le16(body + SCS_MSCP_P_HTMO, c->scc_host_timeout);
        put_le64(body + SCS_MSCP_P_TIME, c->scc_time);
    }
    /* GET UNIT STATUS is "standard header only" (sec 6.12): its parameter area
     * is the zero fill above, and that is the whole of it. */
    return 0;
}

int scs_mscp_scc_defaults(struct scs_mscp_cmd *c, uint32_t cmd_ref)
{
    if (c == NULL) {
        return -1;
    }
    memset(c, 0, sizeof(*c));
    c->cmd_ref = cmd_ref;
    c->unit = 0; /* sec 6.16 addresses the CONTROLLER, not a unit */
    c->opcode = SCS_MSCP_OP_SET_CTLR_CHAR;
    c->modifiers = 0; /* sec 6.16 defines no modifiers for this command */
    c->scc_version = SCS_MSCP_SCC_VERSION_HOST;
    c->scc_ctlr_flags = SCS_MSCP_SCC_CTLR_FLAGS;
    c->scc_host_timeout = SCS_MSCP_SCC_HOST_TIMEOUT;
    c->scc_time = scs_member_vms_time_now(); /* vms-020: live host time, see scs_mscp.h */
    return 0;
}

int scs_mscp_gus_defaults(struct scs_mscp_cmd *c, uint32_t cmd_ref, uint16_t unit)
{
    if (c == NULL) {
        return -1;
    }
    memset(c, 0, sizeof(*c));
    c->cmd_ref = cmd_ref;
    c->unit = unit;
    c->opcode = SCS_MSCP_OP_GET_UNIT_STATUS;
    /* MD.NXU (Table A-2, sec 6.12): "returns the next known unit >= the
     * specified unit number". It is what turns single lookups into the walk. */
    c->modifiers = SCS_MSCP_MOD_NEXT_UNIT;
    return 0;
}

/* ==================== the frame the message rides in ===================== */

int scs_mscp_build_command(const struct scs_mscp_params *p,
                           const struct scs_mscp_cmd *c,
                           uint8_t out[SCS_MSCP_FRAME_LEN])
{
    if (p == NULL || c == NULL || out == NULL) {
        return -1;
    }

    /* Ethernet header (abs 0-13). */
    memcpy(out + 0, p->dst_mac, 6);
    memcpy(out + 6, p->src_mac, 6);
    out[12] = 0x60;
    out[13] = 0x07;

    /* SCA header (abs 14+) from the captured 0x4b-class header. */
    memcpy(out + 14, mscp_sca_hdr, SCS_MSCP_BODY_OFF);

    /* [0:2] is DERIVED, not replayed: the SCA content length field counts the
     * bytes after itself. */
    put_le16(out + 14 + 0, (uint16_t)(SCS_MSCP_SCA_LEN - 2));

    /* Identity substitutions (SCA-content offsets + 14). */
    memcpy(out + 14 + 2, p->peer_logical, 6);  /* dest logical [2:8]  (abs 16) */
    memcpy(out + 14 + 10, p->src_logical, 6);  /* src-logical [10:16] (abs 24) */

    /* SCS sequenced-message counters (spec sec 4h): recv_ack at [18:20] repeated
     * at [26:28]/[34:36]; send_seq at [20:22] mirrored at [30:32]. The
     * incarnation echo at [22:24] (established-join gate, sec 4i.B): 0 leaves the
     * header's fresh-contact value 1. */
    put_le16(out + 14 + 18, p->recv_ack);
    put_le16(out + 14 + 20, p->send_seq);
    if (p->incarnation != 0) {
        put_le16(out + 14 + 22, p->incarnation);
    }
    put_le16(out + 14 + 26, p->recv_ack);
    put_le16(out + 14 + 30, p->send_seq);
    put_le16(out + 14 + 34, p->recv_ack);

    /* vms-ec7: THE SCS MESSAGE ENVELOPE, from the one build path -- the inner
     * length (derived), the format word, the MTYPE, the credit field and the
     * Con.ID pair. This is the frame class docs/design-mscp-direction.md sec 1.2
     * identifies: an MSCP command nested under an SCS header, Figure 4-5.
     *
     * CREDIT (vms-8de): a live READ of p->cdt->send_credit when the caller
     * passed a CDT for this connection (the normal scsd.c case, looked up by
     * local_conid) -- the same accounting scsd_credit_stamp_outbound()
     * (vms-aa1) owns; this builder invents no second source of truth. It is
     * still overwritten (via the same read, one debit later) further down the
     * transmit path by scsd_credit_stamp_outbound() on every outbound MTYPE-10
     * frame, so the field written here never reaches the wire as-is -- naming
     * it live here is what makes that overwrite legible instead of invisible.
     * With no CDT (p->cdt == NULL, e.g. a unit test with no CDL), this falls
     * back to SCS_MSCP_ENV_CREDIT, the golden af2 joiner command's [48:50]
     * LABELED REPLAY this field used unconditionally before this item. */
    {
        struct scs_env_fields env;
        env.mtype = SCS_ENV_MTYPE_APP_MESSAGE;
        env.credit = (p->cdt != NULL) ? (uint16_t)p->cdt->send_credit
                                       : SCS_MSCP_ENV_CREDIT;
        env.dest_conid = p->remote_conid;
        env.src_conid = p->local_conid;
        (void)scs_env_build_frame(out, SCS_MSCP_FRAME_LEN, &env);
    }

    /* The MSCP message, field by field at its Table A-6 offsets. */
    return scs_mscp_build_body(c, out + 14 + SCS_MSCP_BODY_OFF,
                               SCS_MSCP_BODY_LEN);
}

int scs_mscp_build_scc(const struct scs_mscp_params *p,
                       uint8_t out[SCS_MSCP_FRAME_LEN])
{
    struct scs_mscp_cmd c;
    if (p == NULL || out == NULL) {
        return -1;
    }
    if (scs_mscp_scc_defaults(&c, p->cmd_ref) != 0) {
        return -1;
    }
    return scs_mscp_build_command(p, &c, out);
}

int scs_mscp_build_gus(const struct scs_mscp_params *p,
                       uint8_t out[SCS_MSCP_FRAME_LEN])
{
    struct scs_mscp_cmd c;
    if (p == NULL || out == NULL) {
        return -1;
    }
    if (scs_mscp_gus_defaults(&c, p->cmd_ref, p->unit) != 0) {
        return -1;
    }
    return scs_mscp_build_command(p, &c, out);
}

/* ============================ status decode ============================== */

unsigned scs_mscp_status_major(uint16_t status)
{
    return (unsigned)(status & SCS_MSCP_ST_MASK);
}

unsigned scs_mscp_status_subcode(uint16_t status)
{
    return (unsigned)(status >> SCS_MSCP_ST_SUB_SHIFT);
}

const char *scs_mscp_status_name(unsigned major)
{
    switch (major & SCS_MSCP_ST_MASK) {
    case SCS_MSCP_ST_SUCCESS:       return "Success";
    case SCS_MSCP_ST_INVALID_CMD:   return "Invalid Command";
    case SCS_MSCP_ST_ABORTED:       return "Command Aborted";
    case SCS_MSCP_ST_OFFLINE:       return "Unit-Offline";
    case SCS_MSCP_ST_AVAILABLE:     return "Unit-Available";
    case SCS_MSCP_ST_MEDIA_FMT_ERR: return "Media Format Error";
    case SCS_MSCP_ST_WRITE_PROT:    return "Write Protected";
    case SCS_MSCP_ST_COMPARE_ERR:   return "Compare Error";
    case SCS_MSCP_ST_DATA_ERR:      return "Data Error";
    case SCS_MSCP_ST_HOST_BUF_ERR:  return "Host Buffer Access Error";
    case SCS_MSCP_ST_CTLR_ERR:      return "Controller Error";
    case SCS_MSCP_ST_DRIVE_ERR:     return "Drive Error";
    case SCS_MSCP_ST_DIAGNOSTIC:    return "internal diagnostic message";
    default:                        break;
    }
    /* Table B-1 leaves 12..30 undefined. Say so; do not invent a name. */
    return "undefined status code";
}

const char *scs_mscp_opcode_name(uint8_t opcode)
{
    switch (opcode & SCS_MSCP_OPCODE_MASK) {
    case SCS_MSCP_OP_GET_UNIT_STATUS: return "GET UNIT STATUS";
    case SCS_MSCP_OP_SET_CTLR_CHAR:   return "SET CONTROLLER CHARACTERISTICS";
    case SCS_MSCP_OP_ONLINE:          return "ONLINE";
    case SCS_MSCP_OP_READ:            return "READ";
    case SCS_MSCP_OP_WRITE:           return "WRITE";
    default:                          break;
    }
    /* Table A-1 has more opcodes than this header defines, on purpose (rule 8:
     * capture-confirmed only). An unrecognised opcode is reported as such. */
    return "opcode not confirmed on our wire";
}

/* ================================ parse ================================== */

int scs_mscp_parse(const uint8_t *frame, size_t len, struct scs_mscp_view *v)
{
    if (frame == NULL || v == NULL) {
        return -1;
    }
    /* The whole sec 5.1 12-byte MSCP header must be readable: 14 Ethernet +
     * 58 SCA header + 12 == 84. */
    if (len < (size_t)(SCS_ENV_ETH_HDR_LEN + SCS_MSCP_BODY_OFF
                       + SCS_MSCP_HDR_LEN)) {
        return -1;
    }

    /* vms-ec7: the SCS envelope half of this parse comes from the shared path,
     * which also applies the sec 4(h)(1b) conformance test. A frame that is not
     * envelope-conformant is not an MSCP-over-SCS message and is refused here
     * rather than decoded with offsets that do not apply to it. */
    struct scs_env env;
    if (scs_env_parse_frame(frame, len, &env) != 0) {
        return -1;
    }

    memset(v, 0, sizeof(*v));
    v->total_sca_len = env.total_sca_len;
    v->scs_mtype = env.mtype;
    v->credit = env.credit;
    v->remote_conid = env.dest_conid;
    v->local_conid = env.src_conid;
    /* The PPD/NISCA marker + format bytes at content [16:18] -- NOT the SCS
     * message type; see the naming trap in scs_mscp.h. */
    v->msgtype = frame[30];
    v->format = frame[31];
    v->recv_ack = get_le16(frame + 32);
    v->send_seq = get_le16(frame + 34);

    const uint8_t *body = frame + SCS_ENV_ETH_HDR_LEN + SCS_MSCP_BODY_OFF;
    v->cmd_ref = get_le32(body + SCS_MSCP_P_CRF);
    v->unit = get_le16(body + SCS_MSCP_P_UNIT);
    v->opcode = body[SCS_MSCP_P_OPCD];
    v->base_opcode = (uint8_t)(v->opcode & SCS_MSCP_OPCODE_MASK);
    v->is_end = (v->opcode & SCS_MSCP_END_BIT) ? 1 : 0;

    /* sec 5.1: body[9] and body[10:12] carry flags+status one way and
     * reserved+modifiers the other. Decode ONLY the pair the direction selects
     * -- a status read off a command message is a number with no meaning. */
    if (v->is_end) {
        v->end_flags = body[SCS_MSCP_P_FLGS];
        v->status = get_le16(body + SCS_MSCP_P_STS);
        v->status_major = (uint8_t)scs_mscp_status_major(v->status);
        v->status_subcode = (uint16_t)scs_mscp_status_subcode(v->status);
    } else {
        v->modifiers = get_le16(body + SCS_MSCP_P_MOD);
    }
    return 0;
}
