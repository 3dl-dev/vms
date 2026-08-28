/*
 * test_scs_dlm.c - DLM SYSAP message class round-trip + wire-field tests
 * (vms-94c, DLM epic vms-7fa rung 1).
 *
 * The DLM message body [58:138] is a ⚠ LABELLED OVMX design choice (scs_dlm.h
 * class (B)): VSI/HPE do not publish the lock manager's SCS message byte layout.
 * So there is no golden capture to assert against; the acceptance is instead
 *   (1) BUILD -> PARSE round-trips every field losslessly,
 *   (2) the SEMANTIC field VALUES are the authentic $LCKDEF ones (class (A)):
 *       a message carries LCK$K_ modes and LCK$M_ flags unchanged,
 *   (3) the frame is a well-formed SCS application message (MTYPE 10) that the
 *       shared envelope parser accepts, with the fields at the documented
 *       envelope offsets,
 *   (4) malformed inputs are refused, not over-read.
 *
 * Pure state: opens no socket.
 */
#include <stdio.h>
#include <string.h>

#include "scs_dlm.h"
#include "scs_env.h"
#include "lckdef.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            printf("FAIL %s:%d: ", __func__, __LINE__);                     \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* A representative ENQ request: an EX-mode lock on "SYS$SYSDEVICE" with a value
 * block, requester CSID 1025, master unknown (0 => directory-resolve). */
static void fill_enq(struct scs_dlm_msg *m)
{
    memset(m, 0, sizeof(*m));
    m->op = SCS_DLM_OP_ENQ;
    m->mode = LCK$K_EXMODE;
    m->flags = LCK$M_VALBLK | LCK$M_SYSTEM;
    m->req_lkid = 0x00040011u;
    m->master_lkid = 0;
    m->status = 0;
    m->req_csid = 1025;
    m->master_csid = 0;
    m->parent_present = 0;
    for (int i = 0; i < LCK$C_VALBLK_LEN; i++) m->valblk[i] = (uint8_t)(0xA0 + i);
    const char *name = "SYS$SYSDEVICE";
    m->namelen = (uint8_t)strlen(name);
    memcpy(m->resnam, name, m->namelen);
}

static void test_body_roundtrip(void)
{
    struct scs_dlm_msg in, out;
    uint8_t body[SCS_DLM_BODY_LEN];

    fill_enq(&in);
    CHECK(scs_dlm_build_body(&in, body, sizeof(body)) == 0, "build_body failed");
    CHECK(scs_dlm_parse_body(body, sizeof(body), &out) == 0, "parse_body failed");

    CHECK(out.op == in.op, "op %u != %u", out.op, in.op);
    CHECK(out.mode == in.mode, "mode %u != %u", out.mode, in.mode);
    CHECK(out.flags == in.flags, "flags 0x%x != 0x%x", out.flags, in.flags);
    CHECK(out.req_lkid == in.req_lkid, "req_lkid");
    CHECK(out.master_lkid == in.master_lkid, "master_lkid");
    CHECK(out.status == in.status, "status");
    CHECK(out.req_csid == in.req_csid, "req_csid %u", out.req_csid);
    CHECK(out.master_csid == in.master_csid, "master_csid");
    CHECK(out.namelen == in.namelen, "namelen %u != %u", out.namelen, in.namelen);
    CHECK(memcmp(out.valblk, in.valblk, LCK$C_VALBLK_LEN) == 0, "valblk");
    CHECK(memcmp(out.resnam, in.resnam, SCS_DLM_RESNAM_FIELD) == 0, "resnam");
}

/* The authentic $LCKDEF constants must ride the wire UNCHANGED (class (A)). */
static void test_lckdef_values_on_wire(void)
{
    struct scs_dlm_msg m;
    uint8_t body[SCS_DLM_BODY_LEN];
    fill_enq(&m);
    m.mode = LCK$K_PWMODE;         /* 4 */
    m.flags = LCK$M_NOQUEUE | LCK$M_CONVERT; /* 0x0004 | 0x0002 = 0x0006 */
    CHECK(scs_dlm_build_body(&m, body, sizeof(body)) == 0, "build");

    CHECK(body[SCS_DLM_B_MODE] == LCK$K_PWMODE, "mode byte %u", body[SCS_DLM_B_MODE]);
    CHECK(le16(body + SCS_DLM_B_FLAGS) == (LCK$M_NOQUEUE | LCK$M_CONVERT),
          "flags on wire 0x%x", le16(body + SCS_DLM_B_FLAGS));
    /* value-block field width is exactly LCK$C_VALBLK_LEN */
    CHECK(SCS_DLM_B_RESNAM - SCS_DLM_B_VALBLK == LCK$C_VALBLK_LEN,
          "valblk field width %d", SCS_DLM_B_RESNAM - SCS_DLM_B_VALBLK);
}

static void test_frame_roundtrip_and_envelope(void)
{
    struct scs_dlm_params p;
    struct scs_dlm_msg in, out;
    uint8_t frame[SCS_DLM_FRAME_LEN];
    struct scs_dlm_view v;

    memset(&p, 0, sizeof(p));
    uint8_t dmac[6] = {0x08,0x00,0x2b,0x11,0x22,0x33};
    uint8_t smac[6] = {0x08,0x00,0x2b,0x44,0x55,0x66};
    uint8_t slog[6] = {0xaa,0x00,0x04,0x00,0x1a,0x04};
    uint8_t plog[6] = {0xaa,0x00,0x04,0x00,0x01,0x04};
    memcpy(p.dst_mac, dmac, 6);
    memcpy(p.src_mac, smac, 6);
    memcpy(p.src_logical, slog, 6);
    memcpy(p.peer_logical, plog, 6);
    p.remote_conid = 0x0401001Eu; /* peer DLM server handle */
    p.local_conid  = 0x0401000Eu; /* our DLM client handle  */
    p.recv_ack = 0x0021;
    p.send_seq = 0x0022;
    p.incarnation = 3;
    p.credit = 1;

    fill_enq(&in);
    CHECK(scs_dlm_build_frame(&p, &in, frame) == 0, "build_frame failed");

    /* Ethernet + ethertype. */
    CHECK(memcmp(frame + 0, dmac, 6) == 0, "eth dst");
    CHECK(memcmp(frame + 6, smac, 6) == 0, "eth src");
    CHECK(frame[12] == 0x60 && frame[13] == 0x07, "ethertype");

    /* SCS envelope must be conformant and carry MTYPE 10, our Con.IDs, credit. */
    struct scs_env env;
    CHECK(scs_env_parse_frame(frame, sizeof(frame), &env) == 0, "envelope not conformant");
    CHECK(env.mtype == SCS_ENV_MTYPE_APP_MESSAGE, "mtype %u", env.mtype);
    CHECK(env.dest_conid == p.remote_conid, "dest conid 0x%x", env.dest_conid);
    CHECK(env.src_conid == p.local_conid, "src conid 0x%x", env.src_conid);
    CHECK(env.credit == 1, "credit %u", env.credit);
    /* SCA content length field [0:2] == SCA_LEN - 2. */
    CHECK(le16(frame + 14) == SCS_DLM_SCA_LEN - 2, "sca len word %u", le16(frame + 14));
    /* Sequence counters at the documented offsets. */
    CHECK(le16(frame + 14 + 18) == p.recv_ack, "recv_ack");
    CHECK(le16(frame + 14 + 20) == p.send_seq, "send_seq");
    CHECK(le16(frame + 14 + 22) == p.incarnation, "incarnation");
    /* Identity substitutions. */
    CHECK(memcmp(frame + 14 + 2, plog, 6) == 0, "dest logical");
    CHECK(memcmp(frame + 14 + 10, slog, 6) == 0, "src logical");
    /* PPD marker byte is the transport 0x4b, NOT the SCS MTYPE. */
    CHECK(frame[14 + 16] == 0x4b, "ppd marker 0x%x", frame[14 + 16]);

    /* Full parse round-trips the body. */
    CHECK(scs_dlm_parse(frame, sizeof(frame), &v) == 0, "parse failed");
    CHECK(v.scs_mtype == SCS_ENV_MTYPE_APP_MESSAGE, "view mtype");
    CHECK(v.remote_conid == p.remote_conid, "view remote conid");
    CHECK(v.local_conid == p.local_conid, "view local conid");
    out = v.msg;
    CHECK(out.op == in.op, "view op");
    CHECK(out.mode == in.mode, "view mode");
    CHECK(out.flags == in.flags, "view flags");
    CHECK(out.req_csid == in.req_csid, "view req_csid");
    CHECK(out.namelen == in.namelen, "view namelen");
    CHECK(memcmp(out.resnam, in.resnam, in.namelen) == 0, "view resnam");
    CHECK(memcmp(out.valblk, in.valblk, LCK$C_VALBLK_LEN) == 0, "view valblk");
}

/* A GRANT response carries a VMS status and the master's handle. */
static void test_grant_response(void)
{
    struct scs_dlm_msg in, out;
    uint8_t body[SCS_DLM_BODY_LEN];
    memset(&in, 0, sizeof(in));
    in.op = SCS_DLM_OP_GRANT;
    in.mode = LCK$K_EXMODE;
    in.req_lkid = 0x00040011u;   /* echoes the request's handle */
    in.master_lkid = 0x00080002u;
    in.status = 0x00000001u;     /* SS$_NORMAL */
    in.master_csid = 1026;
    CHECK(scs_dlm_build_body(&in, body, sizeof(body)) == 0, "build grant");
    CHECK(le32(body + SCS_DLM_B_STATUS) == 1, "status on wire");
    CHECK(le32(body + SCS_DLM_B_MASTER_LKID) == in.master_lkid, "master lkid on wire");
    CHECK(scs_dlm_parse_body(body, sizeof(body), &out) == 0, "parse grant");
    CHECK(out.op == SCS_DLM_OP_GRANT && out.status == 1, "grant fields");
}

static void test_rejects_malformed(void)
{
    struct scs_dlm_msg m;
    uint8_t body[SCS_DLM_BODY_LEN];
    uint8_t small[SCS_DLM_BODY_LEN - 1];

    fill_enq(&m);
    /* short buffer */
    CHECK(scs_dlm_build_body(&m, small, sizeof(small)) == -1, "short build not refused");
    /* bad op */
    m.op = 99;  CHECK(scs_dlm_build_body(&m, body, sizeof(body)) == -1, "bad op not refused");
    fill_enq(&m);
    /* bad mode */
    m.mode = LCK$K_EXMODE + 1; CHECK(scs_dlm_build_body(&m, body, sizeof(body)) == -1, "bad mode");
    fill_enq(&m);
    /* oversize name */
    m.namelen = SCS_DLM_RESNAM_MAX + 1;
    CHECK(scs_dlm_build_body(&m, body, sizeof(body)) == -1, "oversize name");

    /* parse refuses an oversize namelen field even in a full-size buffer */
    fill_enq(&m);
    CHECK(scs_dlm_build_body(&m, body, sizeof(body)) == 0, "rebuild");
    body[SCS_DLM_B_NAMELEN] = 0xFF;
    CHECK(scs_dlm_parse_body(body, sizeof(body), &m) == -1, "oversize namelen parse");

    /* parse refuses a runt frame */
    uint8_t frame[SCS_DLM_FRAME_LEN];
    struct scs_dlm_view v;
    CHECK(scs_dlm_parse(frame, SCS_DLM_FRAME_LEN - 1, &v) == -1, "runt frame parse");
}

int main(void)
{
    test_body_roundtrip();
    test_lckdef_values_on_wire();
    test_frame_roundtrip_and_envelope();
    test_grant_response();
    test_rejects_malformed();

    printf("test_scs_dlm: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
