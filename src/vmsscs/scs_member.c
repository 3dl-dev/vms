/*
 * scs_member.c - VMS$VAXcluster connection-manager add-member transaction
 * builder/parser (vms-224). See scs_member.h for the full clean-room
 * provenance and the GROUNDED-vs-REPLAYED field breakdown.
 *
 * Templates below are byte-exact 190-byte SCA-content captures of the golden
 * JOINER (VAX2) add-member frames from formation-ci1-joinwindow.pcap:
 *   op 0x14  = SCA#48 (VAX2->VAX1, sendmsg=1)
 *   op 0x01  = SCA#49 (VAX2->VAX1, sendmsg=2, VOTES 0)
 *   op 0x02  = SCA#60 (VAX2->VAX1, sendmsg=3)
 * Substituted at build time (GROUNDED positions only): dst logical [2:8], src
 * logical [10:16], SCS counters [18:24]/[26:28]/[30:32]/[34:36], the Con.ID
 * pair [50:54]/[54:58], the SYSAP send/ack-msg# body[0:4], the op-0x14 model
 * string, and the op-0x01 VOTES body[22:24]. Every other byte is a labeled
 * REPLAY of the captured joiner frame (per-boot/version tokens included).
 */
#include "scs_member.h"

#include <string.h>
#include <time.h>

#include "scs_env.h" /* vms-ec7: THE shared SCS message envelope */

/* op 0x14 model advertisement -- golden SCA#48 (VAX2->VAX1). */
static const uint8_t member_model_tmpl[SCS_MEMBER_SCA_LEN] = {
    0xbc, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x00, 0xaa, 0x00,
    0x04, 0x00, 0x02, 0x04, 0x4b, 0x13, 0x0a, 0x00, 0x0a, 0x00, 0x01, 0x00,
    0x12, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x0a, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x92, 0x00, 0x04, 0x00, 0x0a, 0x00,
    0x00, 0x00, 0x09, 0x00, 0xc5, 0x62, 0x08, 0x00, 0x58, 0x33, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x14, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x15, 0x56, 0x41, 0x58, 0x73, 0x65, 0x72, 0x76, 0x65, 0x72,
    0x20, 0x33, 0x39, 0x30, 0x30, 0x20, 0x53, 0x65, 0x72, 0x69, 0x65, 0x73,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* op 0x01 cluster-parameters -- golden SCA#49 (VAX2->VAX1, VOTES 0). */
static const uint8_t member_params_tmpl[SCS_MEMBER_SCA_LEN] = {
    0xbc, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x00, 0xaa, 0x00,
    0x04, 0x00, 0x02, 0x04, 0x4b, 0x13, 0x0a, 0x00, 0x0b, 0x00, 0x01, 0x00,
    0x12, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x0a, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x92, 0x00, 0x04, 0x00, 0x0a, 0x00,
    0x00, 0x00, 0x09, 0x00, 0xc5, 0x62, 0x08, 0x00, 0x58, 0x33, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x50,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x80, 0x4a, 0x3f, 0x0e, 0x57, 0x9f, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2a, 0x00, 0x0e, 0x00,
    0x00, 0x00, 0x56, 0x37, 0x2e, 0x33, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* op 0x02 config/topology -- golden SCA#60 (VAX2->VAX1). */
static const uint8_t member_config_tmpl[SCS_MEMBER_SCA_LEN] = {
    0xbc, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x00, 0xaa, 0x00,
    0x04, 0x00, 0x02, 0x04, 0x4b, 0x13, 0x0f, 0x00, 0x10, 0x00, 0x01, 0x00,
    0x12, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x0f, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x92, 0x00, 0x04, 0x00, 0x0a, 0x00,
    0x02, 0x00, 0x09, 0x00, 0xc5, 0x62, 0x08, 0x00, 0x58, 0x33, 0x03, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* OVMX's own CPU/model advertisement string (op 0x14). NOT a VAXserver: OVMX
 * announces itself honestly (INV-0 trademark ceiling / never-lie-to-the-metal).
 * It is a display-only field in SHOW CLUSTER, so an OVMX-labeled string is both
 * legal and better milestone proof that OVMX -- not a faked VAX -- joined. */
#define OVMX_MODEL_STRING "OVMX Cluster Node"

static void put_le16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xff);
    dst[1] = (uint8_t)((v >> 8) & 0xff);
}

/*
 * scs_member_vms_time_now - the current time as a VMS 64-bit absolute time:
 * 100 ns intervals since 17-NOV-1858 00:00:00. The offset from the POSIX epoch
 * is 3506716800 seconds.
 *
 * This MUST be live. Shipping a replayed constant here is what bugchecked VAX3
 * and VAX1 (see scs_member_build_token_response): the peers exchange times whose
 * high longword reads ~0x00bc03xx, and the frozen value we sent was ~26 years
 * adrift of the cluster's era.
 */
uint64_t scs_member_vms_time_now(void)
{
    return ((uint64_t)time(NULL) + 3506716800ULL) * 10000000ULL;
}

/*
 * scs_member_close_is_resource - does this cat-0x06 request name a lock
 * RESOURCE in ASCII at body[48:]? The two variants take DIFFERENT responses.
 * The membership-close carries 00 01 04 00 there; the resource variant carries
 * a printable name (e.g. "F11B$aSYSDSK1", "DTI$SYSTEM$VAX2").
 */
int scs_member_close_is_resource(const uint8_t *rbody)
{
    if (rbody == NULL) {
        return 0;
    }
    for (int i = 48; i < 56; i++) {
        if (rbody[i] < 0x20 || rbody[i] > 0x7e) {
            return 0;
        }
    }
    return 1;
}

static void put_le64(uint8_t *dst, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        dst[i] = (uint8_t)((v >> (8 * i)) & 0xff);
    }
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

/*
 * build_common - lay down the Ethernet header + a 190-byte SCA template, then
 * substitute the shared SCS envelope (dst/src logical, live counters), the
 * Con.ID pair, and the SYSAP send/ack-msg# header. Caller substitutes the
 * op-specific body fields afterward. SCA-content offset == out+14+off.
 */
static void build_common(const struct scs_member_params *p, const uint8_t *tmpl,
                         uint16_t credit, uint8_t *out)
{
    struct scs_env_fields env;

    /* Ethernet header (abs 0-13). */
    memcpy(out + 0, p->dst_mac, 6);
    memcpy(out + 6, p->src_mac, 6);
    out[12] = 0x60;
    out[13] = 0x07;

    /* SCA content (abs 14+) from the captured joiner template. */
    memcpy(out + 14, tmpl, SCS_MEMBER_SCA_LEN);

    /* Identity substitutions (SCA-content offsets + 14). */
    memcpy(out + 14 + 2, p->peer_logical, 6);  /* dest logical [2:8]  (abs 16) */
    memcpy(out + 14 + 10, p->src_logical, 6);   /* src-logical [10:16](abs 24) = aa:00:04:00:<sysid>
                                                 * cluster-LOGICAL addr, NOT raw HW MAC (vms-9f3) */

    /* SCS sequenced-message counters (spec sec 4h): recv_ack at [18:20]
     * repeated at [26:28]/[34:36]; send_seq at [20:22] mirrored at [30:32]
     * (the [20:22]==[30:32] mirror is GROUNDED 17758/17758 frames). The
     * incarnation echo at [22:24] (the established-join gate, sec 4i.B): 0
     * leaves the template's fresh-contact value 1. */
    put_le16(out + 14 + 18, p->recv_ack);
    put_le16(out + 14 + 20, p->send_seq);
    if (p->incarnation != 0) {
        put_le16(out + 14 + 22, p->incarnation);
    }
    put_le16(out + 14 + 26, p->recv_ack);
    put_le16(out + 14 + 30, p->send_seq);
    put_le16(out + 14 + 34, p->recv_ack);

    /* vms-ec7: THE SCS MESSAGE ENVELOPE, from the one build path -- inner
     * length (derived), format word, MTYPE, credit, Con.ID pair.
     *
     * MTYPE 10 is GROUNDED for this whole class and it is the finding that
     * unified the envelope: docs/design-mscp-direction.md sec 1.1 measured
     * 173,927 of 173,927 190-content add-member frames in the work/ corpus
     * carrying MTYPE 10, inner length 146 -- the same p. 4-13 application
     * message as the MSCP command frames, not a class of its own. The value was
     * already in every template; the builder now says it.
     *
     * CREDIT is a labeled REPLAY of the per-template captured value (0 on the
     * model/params classes, 2 on the config class). On this class it is also
     * the field scsd_credit_stamp_outbound() (vms-aa1) overwrites with the
     * connection's live Pending Receive Credit on the way out, so the value
     * written here is what goes on the wire only when OVMX_NO_CREDIT_ACCOUNTING
     * is set. Naming it is what makes that overwrite legible. */
    env.mtype = SCS_ENV_MTYPE_APP_MESSAGE;
    env.credit = credit;
    env.dest_conid = p->remote_conid;
    env.src_conid = p->local_conid;
    (void)scs_env_build_frame(out, SCS_MEMBER_FRAME_LEN, &env);

    /* SYSAP transaction envelope send/ack-msg# (body[0:2]/[2:4]; body[0] =
     * SCA offset 58). */
    put_le16(out + 14 + SCS_MEMBER_BODY_OFF + 0, p->sysap_send_msg);
    put_le16(out + 14 + SCS_MEMBER_BODY_OFF + 2, p->sysap_ack_msg);
}

int scs_member_build_model(const struct scs_member_params *p,
                           uint8_t out[SCS_MEMBER_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }

    const char *model = (p->model != NULL) ? p->model : OVMX_MODEL_STRING;
    size_t mlen = strlen(model);
    /* The length-prefixed model string sits at body[16]; the body is 132 bytes
     * (SCA [58:190]), so the string + its 1-byte length prefix must fit in
     * body[16..131] = 116 bytes -> string <= 115. */
    if (mlen > 115) {
        return -1;
    }

    build_common(p, member_model_tmpl, SCS_MEMBER_ENV_CREDIT_MODEL, out);

    /* Substitute OVMX's own model string: zero the old field, then write the
     * length prefix + ASCII (op 0x14, spec sec 4j). body[16] = SCA offset
     * 58+16=74; absolute = out+14+74. */
    uint8_t *mfield = out + 14 + SCS_MEMBER_BODY_OFF + SCS_MEMBER_MODEL_LEN_BODYOFF;
    /* Clear the template's "VAXserver 3900 Series" + any trailing bytes up to
     * the end of the body so no stale characters leak. */
    memset(mfield, 0, (size_t)(SCS_MEMBER_SCA_LEN - (SCS_MEMBER_BODY_OFF + SCS_MEMBER_MODEL_LEN_BODYOFF)));
    mfield[0] = (uint8_t)mlen;
    memcpy(mfield + 1, model, mlen);

    return 0;
}

int scs_member_build_params(const struct scs_member_params *p,
                            uint8_t out[SCS_MEMBER_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }

    build_common(p, member_params_tmpl, SCS_MEMBER_ENV_CREDIT_PARAMS, out);

    /* VOTES at body[22:24] (abs 94), LE u16 -- GROUNDED across four vote
     * configurations (spec sec 4j). OVMX joins non-voting => 0. */
    put_le16(out + 14 + SCS_MEMBER_BODY_OFF + SCS_MEMBER_VOTES_BODYOFF, p->votes);

    /*
     * vms-e81: THE MEMBER FORM. Everything above this point is the JOINER form,
     * and that is correct -- while OVMX is joining, it IS a joiner.
     *
     * The bug this fixes is that OVMX never stopped. Its op 0x01 as a sitting
     * MEMBER was 131 of 132 bytes identical to the op 0x01 a node emits while it
     * is not in a cluster at all, and the single differing byte is one sec 4(j)
     * already classifies as per-boot noise. A census of both captures splits
     * every op 0x01 in the corpus into 6 member frames and 6 joiner frames with
     * ZERO residuals, and OVMX sent the joiner value in every distinguishing
     * field.
     *
     * What that says on the wire is not a cosmetic difference. A newcomer asks
     * each member "what cluster are you in?" and OVMX answered: no cluster, zero
     * members, no formation time, no transition time, admitted 1-JAN-2001 --
     * while VAX1 and VAX2, in the same second, answered "3 members, state-seq 4,
     * formed 02:02:11.14, last transition 02:05:31.79", that last transition
     * being OVMX's OWN admission. The newcomer could not close its view of the
     * cluster and never issued its add-member request, to anyone, for 678 s.
     *
     * The grounding for each field is a controlled variation INSIDE one capture:
     * VAX2 says member_count=2 at +2.5 s and 3 at +53.9 s, the only change being
     * OVMX's admission at +9.5 s.
     *
     * RULE 10 -- cluster_formed and last_transition are CLUSTER-WIDE FACTS.
     * They must be COPIED verbatim from a member's own op 0x01 and never
     * computed here. This function does not know them; it only places them.
     */
    if (p->is_member) {
        uint8_t *b = out + 14 + SCS_MEMBER_BODY_OFF;
        b[12] = 0x21;                              /* member flag (joiner: 0x00) */
        b[82] = 0x2b;                              /* tracks the same split (joiner: 0x2a) */
        put_le16(b + 18, p->member_count);         /* live member count */
        put_le32(b + 44, (uint32_t)p->member_count + 1u); /* state-seq = count + 1, 4/4 */
        put_le64(b + 28, p->cluster_formed);       /* copied, never computed */
        put_le64(b + 36, p->last_transition);      /* copied, never computed */
        put_le64(b + 64, p->own_admission);        /* ours; the joiner sentinel is
                                                    * exactly 2001-01-01 00:00:00 */
        /* vms-2f3 2026-08-01 -- DO NOT "FIX" THE JOINER SENTINEL. It is not a
         * placeholder we invented and it is not honesty debt.
         *
         * body[64:72] is the REPORTING NODE'S OWN ADMISSION TIME, and while a
         * node is itself being admitted the value real VMS puts there is
         * literally 00 80 4a 3f 0e 57 9f 00 = 0x009f570e3f4a8000 = 1-JAN-2001
         * 00:00:00.00. Census over every op 0x01 in the capture library splits
         * by ROLE, not by vendor: real VAX3 sends the sentinel 69x and real
         * VAX2/VX3 20x -- as joiners -- and live 2026 values as established
         * members; VAX1, which is never a joiner in this library, never sends
         * it. VX3/1050 sends the sentinel on its first join AND on both of its
         * rejoins and is admitted every time. The flip to a live value is
         * exactly simultaneous with body[12] 0x00 -> 0x21. No specimen anywhere
         * shows a member-form frame with the sentinel or a joiner-form frame
         * with a live value.
         *
         * Corroborated independently by SDA (captures/sda-scs-extract-vax1.txt):
         * VAX1's CSB Ref. time == the CLUB's Founding Time; VAX2's == the CLUB's
         * Last time stamp -- i.e. each node's own admission instant.
         *
         * So the sentinel is correct as a joiner, this live value is correct as
         * a member, and this field is NOT a suspect in the rejoin refusal. A
         * full 8-byte-window scan of the 132-byte body found no OTHER
         * timestamp-shaped value: only [28:36], [36:44] and [64:72] exist, and
         * all three are handled. That closes the op 0x01 half of vms-70c. */
    }

    return 0;
}

int scs_member_build_config(const struct scs_member_params *p,
                            uint8_t out[SCS_MEMBER_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }

    build_common(p, member_config_tmpl, SCS_MEMBER_ENV_CREDIT_CONFIG, out);

    /* vms-760: body[10:12] and body[40:52] are left ZERO.
     * An earlier revision replayed 0x5041 and twelve 0x20 spaces here, copied
     * from the one admission specimen we had. A survey of every op-0x02 in the
     * capture library retired that: 9 of 12 genuine VMS specimens carry zeros in
     * both places and are acked identically, and the 3 outliers hold printable
     * digraphs ("AP", "IS") and ASCII spaces -- stale buffer contents, not data.
     * We do not reproduce another implementation's uninitialised memory. */

    /*
     * vms-2f3: THE REJOIN FORM. A returning node's op 0x02 is a DIFFERENT SHAPE
     * from a first-timer's, and OVMX has always sent the first-timer's.
     *
     * Census, `cat 0x01 op 0x02` admission requests, body offsets:
     *
     *   capture                             [20:22] [22:24]  [28:36]
     *   vax3-2to3 #285      VAX3 FIRST join    0       0      zero
     *   formation-ci1 #67   VAX2 FIRST join    0       0      zero
     *   crash-rejoin #1297  VAX3 REJOIN        1     1025     004af82e3605bc00
     *   af2-established-rejoin #2923           1     1025     c01f673a9400bc00
     *   af2-firsttimer x3 rejoins              1     1025     (same)
     *   e81-bystander-ADDITION #2969           1     1025     e05a627a4b03bc00
     *   OVMX, every run ever                   0       0      zero
     *
     * Zero residuals: 6 captures, 2 real joiner nodes, 3 lab generations.
     *
     * WHAT THE FIELDS ARE -- grounded against the peer's own oracle, not
     * inferred. SDA `ANALYZE/SYSTEM` -> `SHOW CLUSTER` on VAX1 and on VAX3
     * renders the Cluster Block (CLUB) as:
     *
     *     Found Node SYSID     000000000401        <- 0x0401 = 1025 = [22:24]
     *     Founding Time          1-AUG-2026
     *                              12:03:09        <- = [28:36] decoded
     *
     * and `004af82e3605bc00` decodes as a VMS absolute-time quadword to exactly
     * 1-AUG-2026 12:03:09.60. So [22:24] is the CLUB's founding-node SCSSYSTEMID
     * and [28:36] is the CLUB's founding time -- a CLUSTER-scoped fact, NOT this
     * node's incarnation. (The same quadword appears in every member's own
     * op 0x01 body[28:36] and in the class-0x03 removal op 0x08 that removed a
     * node with a completely different incarnation. One value per cluster.)
     *
     * RULE 10 APPLIES, exactly as it does to op 0x01: both values are CLUSTER
     * facts and are COPIED from what the members told us. This function does not
     * know them and never computes them; it only places them. The caller learns
     * `cluster_formed` from a member's op 0x01 body[28:36] (we already parse it
     * and have been discarding it here) and `founding_sysid` by identifying the
     * founding node -- the one member whose own admission time body[64:72]
     * EQUALS the founding time, VAX1 and only VAX1 in the corpus, matching SDA's
     * Found Node SYSID with zero residuals.
     *
     * [36:40] reads 9 / 3 / 2 across the real rejoin specimens and is NOT
     * determined -- not the epoch, not a CSID. It stays zero rather than
     * carrying a guess. Real rejoins also put twelve 0x20 spaces at [40:52];
     * 9 of 12 genuine specimens carry zeros there and are acked identically
     * (see above), so that stays zero too. Both are one-line variables for a
     * later run, deliberately not moved in the same experiment as this.
     *
     * MUST STAY CONDITIONAL. A genuinely fresh identity has to keep sending
     * zeros, because every real FIRST join does.
     */
    if (p->rejoin) {
        uint8_t *b = out + 14 + SCS_MEMBER_BODY_OFF;
        put_le16(b + 20, 1);
        put_le16(b + 22, p->founding_sysid);
        put_le64(b + 28, p->cluster_formed);
    }

    return 0;
}

int scs_member_parse(const uint8_t *frame, size_t len, struct scs_member_view *v)
{
    if (frame == NULL || v == NULL) {
        return -1;
    }
    if (len < SCS_MEMBER_FRAME_LEN) {
        return -1;
    }

    uint16_t lenword = get_le16(frame + 14);
    uint16_t total = (uint16_t)(lenword + 2);
    if (total != SCS_MEMBER_SCA_LEN) {
        return -1;
    }

    memset(v, 0, sizeof(*v));
    v->total_sca_len = total;
    v->msgtype = frame[30];       /* SCA offset 16 */
    v->format = frame[31];        /* SCA offset 17 */
    v->recv_ack = get_le16(frame + 32); /* SCA [18:20] */
    v->send_seq = get_le16(frame + 34); /* SCA [20:22] */
    v->remote_conid = get_le32(frame + 64);
    v->local_conid = get_le32(frame + 68);

    const uint8_t *body = frame + 72; /* SYSAP body[0] = abs 72 */
    v->sysap_send_msg = get_le16(body + 0);
    v->sysap_ack_msg = get_le16(body + 2);
    v->txn = get_le16(body + 4);
    v->checksum = get_le16(body + 6);
    v->category = body[8];
    v->opcode = body[9];
    v->is_response = (v->category & SCS_MEMBER_RESPONSE_BIT) ? 1 : 0;

    /* A member-driven transaction the joiner must 0x81-respond to: a
     * category-0x01 (non-response) commit (0x03) or lock-rebuild (0x05). */
    v->is_member_txn = (!v->is_response &&
                        (v->category & 0x7f) == SCS_MEMBER_CAT_CONFIG &&
                        (v->opcode == SCS_MEMBER_OP_COMMIT ||
                         v->opcode == SCS_MEMBER_OP_LOCKRB));
    return 0;
}

int scs_member_build_ack(const struct scs_member_params *p,
                         uint8_t out[SCS_MEMBER_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }

    /* Any 190-byte template supplies the correct constant envelope span;
     * build_common substitutes identity, Con.ID pair and live counters. */
    build_common(p, member_config_tmpl, SCS_MEMBER_ENV_CREDIT_CONFIG, out);

    uint8_t *body = out + 72;
    /* A cat-0x04 ack is header-only: everything past the category/opcode is
     * payload we do not carry. Zero it rather than inherit template bytes. */
    memset(body + 4, 0, SCS_MEMBER_SCA_LEN - SCS_MEMBER_BODY_OFF - 4);
    put_le16(body + 0, p->sysap_send_msg);
    put_le16(body + 2, p->sysap_ack_msg);
    body[8] = SCS_MEMBER_CAT_ACK; /* 0x04 */
    body[9] = 0x00;               /* see header: real VMS carries residue here */
    return 0;
}

int scs_member_build_barrier(const struct scs_member_params *p,
                             uint32_t epoch, uint32_t step,
                             uint8_t out[SCS_MEMBER_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    build_common(p, member_config_tmpl, SCS_MEMBER_ENV_CREDIT_CONFIG, out);

    uint8_t *body = out + 72;
    memset(body + 4, 0, SCS_MEMBER_SCA_LEN - SCS_MEMBER_BODY_OFF - 4);
    put_le16(body + 0, p->sysap_send_msg);
    put_le16(body + 2, p->sysap_ack_msg);
    put_le16(body + 4, p->txn);
    put_le16(body + 6, p->checksum);
    body[8] = SCS_MEMBER_CAT_CONFIG;
    body[9] = SCS_MEMBER_OP_BARRIER;
    /* body[10:12] left zero -- residue in the specimens that carry anything. */
    put_le32(body + SCS_MEMBER_EPOCH_BODYOFF, epoch);
    put_le32(body + SCS_MEMBER_STEP_BODYOFF, step);
    return 0;
}

int scs_member_build_token_response(const struct scs_member_params *p,
                                    const uint8_t *req_frame, size_t req_len,
                                    uint8_t out[SCS_MEMBER_FRAME_LEN])
{
    if (p == NULL || req_frame == NULL || out == NULL) {
        return -1;
    }
    if (req_len < SCS_MEMBER_FRAME_LEN) {
        return -1;
    }
    uint16_t lenword = get_le16(req_frame + 14);
    if ((uint16_t)(lenword + 2) != SCS_MEMBER_SCA_LEN) {
        return -1;
    }

    /* vms-760: BUILD THIS FRESH. It previously started from the PARAMS template,
     * which made body[10:132] BYTE-IDENTICAL to our own op-0x01 PARAMS body --
     * verified, zero differing offsets. That frame KILLED TWO REAL VAXES:
     * VAX3 bugchecked INCONSTATE 0.27 ms after receiving ours and VAX1
     * INVEXCEPTN 0.18 ms after; VAX2, which never received one, survived
     * (captures/ovmx-760-relay-crash-20260730.pcap f1351 -> VAX3 dead at f1355,
     * f1379 -> VAX1 dead at f1380).
     *
     * The real joiner's close differs from its OWN PARAMS at 17 offsets
     * (ref f673 vs f143). The reasoning "the close carries our node-parameter
     * block, and PARAMS already carries that block, so reuse it" was a
     * STRUCTURAL GUESS, and the bytes say it is wrong. Worst of the leftovers:
     * body[64:72] must be a LIVE VMS absolute time -- the reference sends one
     * matching the peers' era (high longword ~0x00bc03xx) while we shipped a
     * replayed constant ~26 years off.
     *
     * Everything not named below is ZERO. Do not reintroduce a template here. */
    build_common(p, member_config_tmpl, SCS_MEMBER_ENV_CREDIT_CONFIG, out);

    uint8_t *body = out + 72;
    const uint8_t *rbody = req_frame + 72;
    memset(body, 0, SCS_MEMBER_SCA_LEN - SCS_MEMBER_BODY_OFF);

    put_le16(body + 0, p->sysap_send_msg);
    put_le16(body + 2, p->sysap_ack_msg);
    body[4] = rbody[4]; body[5] = rbody[5]; /* txn      -- carried verbatim */
    body[6] = rbody[6]; body[7] = rbody[7]; /* checksum -- carried verbatim */
    body[8] = (uint8_t)(rbody[8] | SCS_MEMBER_RESPONSE_BIT);
    body[9] = rbody[9];

    if (scs_member_close_is_resource(rbody)) {
        /* Variant B -- the request names a lock RESOURCE in ASCII at body[48:].
         * Members answer these with body[24] = 0x04 and nothing else
         * (ref f339 -> f342, f716 -> f728). Using variant A's block here would
         * be the same over-generalisation that caused the crash. */
        body[24] = 0x04;
        return 0;
    }

    /* Variant A -- the membership-close proper (ref f671 -> f673). */
    put_le16(body + 10, 0x0003);
    put_le16(body + 24, 0x0003);
    put_le32(body + 44, 0x00000003);
    put_le32(body + 48, 0x0000006e); /* 110 -- value NOT explained; replayed */
    put_le64(body + 64, scs_member_vms_time_now());
    put_le32(body + 72, 0x00000010);
    put_le32(body + 76, 0x00000001);
    body[82] = 0x2b;
    put_le16(body + 84, 0x678d);
    memcpy(body + 88, "V7.3    ", 8);
    return 0;
}

/*
 * scs_member_build_dlm_response - answer a category-0x02 op-0x0d DLM
 * lock-resource rebuild record.
 *
 * GROUNDED to an unusual degree: this recipe reconstructs 1367 of 1367 real
 * responses byte-for-byte, from FOUR different responder nodes across two
 * captures (vax3-2to3-established-join: joiner 216/216, VAX2 201/201, VAX1
 * 29/29; af2-firsttimer-established: joiner 921/921). Zero residuals.
 *
 * It is a VERBATIM echo plus exactly four edits -- and critically it does NOT
 * take the two cat-0x01 mutations. Those offsets land INSIDE the payload here:
 *   body[18] is the 2nd byte of the L1 region;
 *   body[55] is the 8TH BYTE OF THE LOCK RESOURCE NAME for every observed
 *            name length (13..24).
 * Applying them corrupted every record we answered -- e.g. "CACHE$cmSYSDSK1"
 * went out as "CACHE$c\0SYSDSK1" -- and VAX1 and VAX3 both took a fatal
 * LOCKMGRERR. The in-capture control is decisive: across the SAME milliseconds
 * VAX1 and VAX3 exchanged the same records with each OTHER correctly and
 * neither crashed. The only variable was our mutation.
 *
 * The echo acknowledges the COORDINATOR'S OWN RECORD coming back with a result
 * code; it asserts nothing about locks WE hold. That is why a joiner holding no
 * locks can answer all 216 of them truthfully -- the earlier worry that echoing
 * "claims lock state we do not have" was misplaced. What was wrong was the
 * corruption, not the echoing.
 */
int scs_member_build_dlm_response(const struct scs_member_params *p,
                                  const uint8_t *req_frame, size_t req_len,
                                  uint8_t out[SCS_MEMBER_FRAME_LEN])
{
    if (p == NULL || req_frame == NULL || out == NULL) {
        return -1;
    }
    if (req_len < SCS_MEMBER_FRAME_LEN) {
        return -1;
    }
    uint16_t lenword = get_le16(req_frame + 14);
    if ((uint16_t)(lenword + 2) != SCS_MEMBER_SCA_LEN) {
        return -1;
    }

    build_common(p, member_config_tmpl, SCS_MEMBER_ENV_CREDIT_CONFIG, out);

    uint8_t *obody = out + 72;
    const uint8_t *rbody = req_frame + 72;
    memcpy(obody, rbody, SCS_MEMBER_SCA_LEN - SCS_MEMBER_BODY_OFF); /* VERBATIM */

    put_le16(obody + 0, p->sysap_send_msg);
    put_le16(obody + 2, p->sysap_ack_msg);
    obody[8] = (uint8_t)(rbody[8] | SCS_MEMBER_RESPONSE_BIT); /* 0x02 -> 0x82 */
    /* body[34]: MANDATORY, written unconditionally. The requests carried 0xf9
     * (209), 0x00 (3), 0x20 (2), 0x72 (1), 0xbc (1) and EVERY response carried
     * 0xf9 -- in two specimens it lands mid-ASCII, which proves it is a
     * fixed-offset stamp and not a payload field. INFERRED to be a per-opcode
     * result code; op 0x01/0x07/0x15 responses use 0xfa. DO NOT generalise this
     * value, or this whole shape, to another cat-0x02 opcode. */
    obody[SCS_MEMBER_DLM_RESULT_BODYOFF] = SCS_MEMBER_DLM_RESULT_0D;
    /* Everything else -- txn/cksum at [4:8], opcode at [9], the invariants at
     * [12:16], the L1 region, and the resource name at [47 : 47+1+len] -- is
     * echoed byte for byte. Mutating ANY of it is LOCKMGRERR. */
    return 0;
}

int scs_member_build_response(const struct scs_member_params *p,
                              const uint8_t *req_frame, size_t req_len,
                              uint8_t out[SCS_MEMBER_FRAME_LEN])
{
    if (p == NULL || req_frame == NULL || out == NULL) {
        return -1;
    }
    if (req_len < SCS_MEMBER_FRAME_LEN) {
        return -1;
    }
    uint16_t lenword = get_le16(req_frame + 14);
    if ((uint16_t)(lenword + 2) != SCS_MEMBER_SCA_LEN) {
        return -1;
    }

    /* Start from the member_config template (any 190-byte template gives the
     * correct constant envelope span [36:50]); build_common overwrites all the
     * substituted fields, and we then echo the member's SYSAP body. */
    build_common(p, member_config_tmpl, SCS_MEMBER_ENV_CREDIT_CONFIG, out);

    /* Echo the member's entire SYSAP body (carries txn+checksum byte-for-byte,
     * spec sec 4j), then apply the response transform. body[0] = abs 72. */
    uint8_t *obody = out + 72;
    const uint8_t *rbody = req_frame + 72;
    memcpy(obody, rbody, SCS_MEMBER_SCA_LEN - SCS_MEMBER_BODY_OFF); /* 132 bytes */

    /* SYSAP header: our own send-msg#, ack of the member's send-msg#. */
    put_le16(obody + 0, p->sysap_send_msg);
    put_le16(obody + 2, p->sysap_ack_msg);
    /* txn (body[4:6]) + checksum (body[6:8]) stay echoed from the request. */
    obody[8] = (uint8_t)(rbody[8] | SCS_MEMBER_RESPONSE_BIT); /* set response bit */
    /* opcode body[9] stays echoed. */
    /* vms-e4b: op 0x0f ECHOES body[18]; it does not force it to 1.
     *
     * Two independent censuses looked contradictory and are not. The first found
     * one real 0x0f response with body[18] == 1 (crash capture f1367->f1368) and
     * generalised "0x0f takes the marker". The second found six genuine VMS 0x0f
     * responses that all leave body[18] == 0. The reconciliation is that f1367's
     * REQUEST already carried body[18] == 1 -- so both samples are an echo, and
     * neither is a node setting the byte.
     *
     * Which reading we adopt is not a coin toss. Echoing writes nothing of our
     * own into someone else's message; forcing overwrites a byte we cannot
     * justify. And the only frames in the entire capture library that set
     * body[18] on an 0x0f response are OVMX's own, inside
     * ovmx-760-relay-crash-20260730.pcap -- the capture where VAX3 took
     * INCONSTATE and VAX1 took INVEXCEPTN. That is not proof it was the cause,
     * but it is not evidence for keeping it either. */
    if (rbody[9] != SCS_MEMBER_OP_0F) {
        obody[SCS_MEMBER_RESP_MARK_BODYOFF] = 0x01; /* response marker (GROUNDED 0x03+0x05+0x09) */
    }
    /* vms-e81 CORRECTION: body[55]=0x00 is op-0x09-SPECIFIC, not a general
     * cat-0x01 rule. A whole-capture census of every responder shows op 0x03 and
     * op 0x05 responses ECHO body[55] verbatim (their requests carried 0x54 and
     * the responses kept it); only op 0x09 clears it (0x0e -> 0x00, on both VAX1
     * and VAX3). The earlier "exactly THREE mutations" reading came from a
     * sample that happened to be op-0x09-heavy.
     *
     * Zeroing it on 0x03/0x05 was tolerated -- the coordinator accepted those
     * responses and OVMX still reached MEMBER -- but "tolerated" is not
     * "correct", and this is a byte we were overwriting in someone else's
     * message for no grounded reason. */
    if (rbody[9] == SCS_MEMBER_OP_XITION) {
        obody[55] = 0x00;
    }

    return 0;
}
