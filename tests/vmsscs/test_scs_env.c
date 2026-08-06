/*
 * test_scs_env.c - vms-ec7: the SCS message envelope, build and parse.
 *
 * WHAT THIS TEST IS FOR. scs_env.c is now the single place every SCS message
 * OVMX emits gets its inner length, format word, MTYPE, credit and Con.ID pair,
 * and the single place every received frame is envelope-tested. Six builders
 * and the whole receive path go through it, so a one-byte defect here is a
 * one-byte defect on every frame OVMX transmits. The per-class byte-exact tests
 * (test_scs_disc.c, test_scs_mscp.c, test_scs_dir.c, test_scs_connect.c,
 * test_scs_member.c, test_scsd_wire.c) already catch that -- MEASURED: mutating
 * the MTYPE store by +1 reds seven test binaries. This file exists for the
 * things those cannot reach: the refusals, the boundaries, and the round trip
 * as a property rather than as one golden frame.
 *
 * THE GOLDEN BYTES below are the envelope window content[42:58] of real
 * captured frames, byte-exact, one per class OVMX builds. Provenance is the
 * per-builder template each is copied from (all already in the tree, all
 * clean-room lab captures under CLAUDE.md rule 8):
 *   58-content  DISCONNECT_RSP   scs_disc.c   disc_response_tmpl
 *   62-content  DISCONNECT_REQ   scs_disc.c   disc_request_tmpl
 *   66-content  CONNECT_RSP echo scs_dir.c    dir_echo_tmpl
 *   94-content  MSCP SCC command scs_mscp.c   mscp_scc_tmpl
 *  110-content  CONNECT_REQ      scs_connect.c connect_request_tmpl
 *  190-content  add-member config scs_member.c member_config_tmpl
 * The corpus-wide version of this same check -- every envelope-conformant frame
 * in 48 pcaps, 319,575 of them, rebuilt and compared -- is
 * tools/cluster/scs_env_measure.py part (D), gated by ctest -R scs_env_figures.
 */
#include "scs_env.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL %s\n", what);
    }
}

/* content[42:58] of one captured frame per class OVMX builds, and the fields
 * they decode to. */
struct golden {
    const char *name;
    unsigned    sca_len;
    uint8_t     env[SCS_ENV_HDR_END - SCS_ENV_OFF_INNER_LEN];
    uint16_t    mtype;
    uint16_t    credit;
    uint32_t    dest;
    uint32_t    src;
};

static const struct golden goldens[] = {
    { "58-content DISCONNECT_RSP", 58,
      { 0x0e, 0x00, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x05, 0x63, 0x07, 0x00, 0x59, 0x33 },
      7, 0, 0x63050008u, 0x33590007u },
    { "62-content DISCONNECT_REQ", 62,
      { 0x12, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x05, 0x63, 0x07, 0x00, 0x59, 0x33 },
      6, 0, 0x63050008u, 0x33590007u },
    { "66-content CONNECT_RSP echo", 66,
      { 0x16, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x05, 0x63, 0x00, 0x00, 0x00, 0x00 },
      1, 0, 0x63050008u, 0x00000000u },
    { "94-content MSCP SCC command", 94,
      { 0x32, 0x00, 0x04, 0x00, 0x0a, 0x00, 0x01, 0x00,
        0x0a, 0x00, 0x54, 0x35, 0x08, 0x00, 0xd2, 0x8f },
      10, 1, 0x3554000au, 0x8fd20008u },
    { "110-content CONNECT_REQ", 110,
      { 0x42, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0a, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0xc5, 0x62 },
      0, 10, 0x00000000u, 0x62c50009u },
    { "190-content add-member config", 190,
      { 0x92, 0x00, 0x04, 0x00, 0x0a, 0x00, 0x02, 0x00,
        0x09, 0x00, 0xc5, 0x62, 0x08, 0x00, 0x58, 0x33 },
      10, 2, 0x62c50009u, 0x33580008u },
};
#define N_GOLDENS ((int)(sizeof(goldens) / sizeof(goldens[0])))

/* Lay out a synthetic SCA content buffer of `sca_len` bytes carrying the
 * class's own [0:2] length field, then stamp the golden envelope window. */
static void make_content(uint8_t *buf, unsigned sca_len, const uint8_t *env)
{
    memset(buf, 0xEE, sca_len); /* poison: nothing outside [42:58] may be read */
    buf[0] = (uint8_t)((sca_len - 2) & 0xff);
    buf[1] = (uint8_t)(((sca_len - 2) >> 8) & 0xff);
    if (env != NULL) {
        memcpy(buf + SCS_ENV_OFF_INNER_LEN, env,
               SCS_ENV_HDR_END - SCS_ENV_OFF_INNER_LEN);
    }
}

/*
 * THE ROUND TRIP, per class: parse the golden window, then rebuild it from the
 * parsed fields and require the 16 bytes back byte-identical. This is the
 * property the six builders now rely on -- that scs_env_build() is the exact
 * inverse of scs_env_parse() over the wire's own values.
 */
static void test_golden_round_trip(void)
{
    for (int i = 0; i < N_GOLDENS; i++) {
        const struct golden *g = &goldens[i];
        uint8_t buf[256];
        struct scs_env e;
        struct scs_env_fields f;
        char msg[192];

        make_content(buf, g->sca_len, g->env);

        snprintf(msg, sizeof(msg), "%s: parses as envelope-conformant", g->name);
        check(scs_env_parse(buf, g->sca_len, &e) == 0, msg);

        snprintf(msg, sizeof(msg), "%s: total_sca_len == %u", g->name, g->sca_len);
        check(e.total_sca_len == g->sca_len, msg);
        snprintf(msg, sizeof(msg), "%s: inner length == %u (total - 44)",
                 g->name, g->sca_len - 44u);
        check(e.inner_len == (uint16_t)(g->sca_len - 44u), msg);
        snprintf(msg, sizeof(msg), "%s: MTYPE == %u", g->name, g->mtype);
        check(e.mtype == g->mtype, msg);
        snprintf(msg, sizeof(msg), "%s: credit == %u", g->name, g->credit);
        check(e.credit == g->credit, msg);
        snprintf(msg, sizeof(msg), "%s: Con.ID pair", g->name);
        check(e.dest_conid == g->dest && e.src_conid == g->src, msg);
        snprintf(msg, sizeof(msg), "%s: payload_len == total - 58", g->name);
        check(e.payload_len == (size_t)(g->sca_len - SCS_ENV_HDR_END), msg);

        /* Rebuild into a differently-poisoned buffer, from the parsed fields
         * only -- nothing is carried over from the golden bytes. */
        uint8_t out[256];
        make_content(out, g->sca_len, NULL);
        f.mtype = e.mtype;
        f.credit = e.credit;
        f.dest_conid = e.dest_conid;
        f.src_conid = e.src_conid;
        snprintf(msg, sizeof(msg), "%s: scs_env_build returns 0", g->name);
        check(scs_env_build(out, g->sca_len, &f) == 0, msg);
        snprintf(msg, sizeof(msg),
                 "%s: rebuilt content[42:58] is byte-identical to the capture",
                 g->name);
        check(memcmp(out + SCS_ENV_OFF_INNER_LEN, g->env,
                     SCS_ENV_HDR_END - SCS_ENV_OFF_INNER_LEN) == 0, msg);

        /* And the builder touched NOTHING else: the poison outside [42:58]
         * survives. A builder that scribbled would corrupt every captured
         * template it is handed. */
        int clean = 1;
        for (unsigned k = 2; k < SCS_ENV_OFF_INNER_LEN; k++) {
            if (out[k] != 0xEE) {
                clean = 0;
            }
        }
        for (unsigned k = SCS_ENV_HDR_END; k < g->sca_len; k++) {
            if (out[k] != 0xEE) {
                clean = 0;
            }
        }
        snprintf(msg, sizeof(msg),
                 "%s: scs_env_build wrote only [42:58]", g->name);
        check(clean, msg);
    }
}

/*
 * THE REFUSALS. These are what the per-class byte-exact tests cannot reach, and
 * they are the half of the item that matters most on the RECEIVE side: reading
 * an MTYPE out of a frame that has no envelope is exactly the confound
 * docs/design-mscp-direction.md sec 4 records ("printed types 1..22").
 */
static void test_refusals(void)
{
    uint8_t buf[256];
    struct scs_env e;

    /* The 106-content START / config class: [44:46] is a config-round counter
     * (observed 2) and [46:48] is the SCSSYSTEMID, so neither half of the
     * conformance test holds. */
    make_content(buf, 106, NULL);
    buf[SCS_ENV_OFF_INNER_LEN] = 0x02;
    buf[SCS_ENV_OFF_INNER_LEN + 1] = 0x00;   /* inner length 2, not 106-44 */
    buf[SCS_ENV_OFF_FORMAT] = 0x02;
    buf[SCS_ENV_OFF_FORMAT + 1] = 0x00;      /* config round, not 0x0004 */
    buf[SCS_ENV_OFF_MTYPE] = 0x01;
    buf[SCS_ENV_OFF_MTYPE + 1] = 0x04;       /* SCSSYSTEMID 1025, not an MTYPE */
    check(scs_env_parse(buf, 106, &e) == -1,
          "the 106-content START/config class is REFUSED (spec sec 4(h)(1d))");

    /* The undecoded 70-content class: format word wrong, inner length 9..13. */
    make_content(buf, 70, NULL);
    buf[SCS_ENV_OFF_INNER_LEN] = 0x0b;
    buf[SCS_ENV_OFF_INNER_LEN + 1] = 0x00;
    buf[SCS_ENV_OFF_FORMAT] = 0x2f;
    buf[SCS_ENV_OFF_FORMAT + 1] = 0x52;      /* 0x522f */
    check(scs_env_parse(buf, 70, &e) == -1,
          "the 70-content class is REFUSED -- its [44:46] is not 0x0004");

    /* Format word right, inner length wrong: HALF the test must still refuse,
     * or the AND in scs_env_parse() is decoration. */
    make_content(buf, 94, goldens[3].env);
    buf[SCS_ENV_OFF_INNER_LEN] = 0x33;       /* 51, not 50 */
    check(scs_env_parse(buf, 94, &e) == -1,
          "format word right + inner length wrong is REFUSED");

    /* Inner length right, format word wrong. */
    make_content(buf, 94, goldens[3].env);
    buf[SCS_ENV_OFF_FORMAT] = 0x05;
    check(scs_env_parse(buf, 94, &e) == -1,
          "inner length right + format word wrong is REFUSED");

    /* A frame that declares more content than is readable. */
    make_content(buf, 94, goldens[3].env);
    check(scs_env_parse(buf, 93, &e) == -1,
          "a declared length beyond the readable bytes is REFUSED");

    /* Shorter than the envelope itself. */
    make_content(buf, 58, goldens[0].env);
    check(scs_env_parse(buf, SCS_ENV_HDR_END - 1, &e) == -1,
          "a buffer shorter than the 58-byte envelope is REFUSED");

    /* NULL arguments, both directions. */
    check(scs_env_parse(NULL, 94, &e) == -1, "parse refuses a NULL buffer");
    check(scs_env_parse(buf, 94, NULL) == -1, "parse refuses a NULL output");
    struct scs_env_fields f = { 10, 1, 1, 2 };
    check(scs_env_build(NULL, 94, &f) == -1, "build refuses a NULL buffer");
    check(scs_env_build(buf, 94, NULL) == -1, "build refuses NULL fields");
    check(scs_env_build(buf, SCS_ENV_HDR_END - 1, &f) == -1,
          "build refuses a class shorter than the envelope");
    check(scs_env_build(buf, 0x10000, &f) == -1,
          "build refuses a class longer than the u16 SCA length field");
    check(scs_env_build_frame(NULL, 108, &f) == -1,
          "build_frame refuses a NULL frame");
    check(scs_env_build_frame(buf, SCS_ENV_ETH_HDR_LEN, &f) == -1,
          "build_frame refuses a frame with no content");
    check(scs_env_parse_frame(NULL, 108, &e) == -1,
          "parse_frame refuses a NULL frame");
}

/*
 * THE 58-CONTENT BOUNDARY. The shortest envelope class is envelope and nothing
 * else: inner length 14 == exactly [44:58], payload_len 0, payload NULL. A
 * parser that handed out a one-byte payload here would feed a SYSAP input
 * routine a pointer past the frame.
 */
static void test_shortest_class(void)
{
    uint8_t buf[64];
    struct scs_env e;

    make_content(buf, 58, goldens[0].env);
    check(scs_env_parse(buf, 58, &e) == 0, "the 58-content class parses");
    check(e.inner_len == 14, "the 58-content class has inner length 14 (== [44:58])");
    check(e.payload_len == 0, "the 58-content class has no payload");
    check(e.payload == NULL, "a zero-length payload is handed out as NULL");
}

/*
 * THE DISPATCH DECISION (p. 4-15). Total over the whole u16 space; 0..9 route
 * to the connection state machine, 10 to per-connection message input, and
 * everything else to UNKNOWN -- counted, never assumed to be a datagram.
 */
static void test_route(void)
{
    for (unsigned mt = 0; mt <= SCS_ENV_MTYPE_CONTROL_MAX; mt++) {
        check(scs_env_route_for_mtype(mt) == SCS_ENV_ROUTE_CONTROL,
              "MTYPE 0..9 routes to the connection state machine");
    }
    check(scs_env_route_for_mtype(SCS_ENV_MTYPE_APP_MESSAGE) ==
              SCS_ENV_ROUTE_MESSAGE,
          "MTYPE 10 routes to per-connection message input");
    for (unsigned mt = 11; mt < 0x10000u; mt++) {
        if (scs_env_route_for_mtype(mt) != SCS_ENV_ROUTE_UNKNOWN) {
            check(0, "an MTYPE above 10 routes to UNKNOWN");
            return;
        }
    }
    check(1, "every MTYPE above 10 routes to UNKNOWN (all 65,525 of them)");

    /* The route is also filled in by the parser, not only by the predicate. */
    uint8_t buf[256];
    struct scs_env e;
    make_content(buf, 94, goldens[3].env);
    check(scs_env_parse(buf, 94, &e) == 0 && e.route == SCS_ENV_ROUTE_MESSAGE,
          "the parser fills route from the decoded MTYPE");
    make_content(buf, 62, goldens[1].env);
    check(scs_env_parse(buf, 62, &e) == 0 && e.route == SCS_ENV_ROUTE_CONTROL,
          "a DISCONNECT_REQ parses as a control message");

    /* An eleventh value has never been observed. If one ever appears the
     * daemon must COUNT it, not classify it -- so the parser must accept the
     * frame and route it to UNKNOWN rather than refuse it outright. */
    make_content(buf, 94, goldens[3].env);
    buf[SCS_ENV_OFF_MTYPE] = 11;
    check(scs_env_parse(buf, 94, &e) == 0 && e.route == SCS_ENV_ROUTE_UNKNOWN,
          "an unobserved MTYPE parses and routes to UNKNOWN (so it is counted, "
          "never guessed at, and NEVER assumed to be a datagram)");
}

/*
 * THE FRAME-RELATIVE HELPERS. Every builder in the tree addresses from absolute
 * frame offset 0, so the +14 wrappers are the ones actually used; getting the
 * Ethernet-header bias wrong once would shift every field by 14 bytes.
 */
static void test_frame_addressing(void)
{
    uint8_t frame[14 + 256];
    struct scs_env e;
    uint16_t mt = 0xFFFF;

    memset(frame, 0, sizeof(frame));
    frame[12] = 0x60;
    frame[13] = 0x07;
    make_content(frame + SCS_ENV_ETH_HDR_LEN, 94, goldens[3].env);

    check(scs_env_parse_frame(frame, 14 + 94, &e) == 0 &&
              e.mtype == 10 && e.dest_conid == goldens[3].dest,
          "parse_frame reads the envelope at absolute offset 14 + N");
    check(scs_env_mtype_of_frame(frame, 14 + 94, &mt) == 1 && mt == 10,
          "mtype_of_frame agrees with the full parse");

    /* And it REFUSES a non-envelope frame rather than returning a number from
     * an offset that class does not have -- the whole reason it exists. */
    frame[SCS_ENV_ETH_HDR_LEN + SCS_ENV_OFF_FORMAT] = 0x2f;
    mt = 0xFFFF;
    check(scs_env_mtype_of_frame(frame, 14 + 94, &mt) == 0 && mt == 0xFFFF,
          "mtype_of_frame refuses a non-envelope frame and leaves *out alone");

    /* build_frame and build must agree byte for byte. */
    uint8_t a[14 + 128];
    uint8_t b[128];
    struct scs_env_fields f = { 10, 1, 0x11223344u, 0x55667788u };
    memset(a, 0, sizeof(a));
    make_content(a + SCS_ENV_ETH_HDR_LEN, 94, NULL);
    make_content(b, 94, NULL);
    check(scs_env_build_frame(a, 14 + 94, &f) == 0, "build_frame returns 0");
    check(scs_env_build(b, 94, &f) == 0, "build returns 0");
    check(memcmp(a + SCS_ENV_ETH_HDR_LEN, b, 94) == 0,
          "build_frame(frame, 14+n) == build(content, n)");
}

/* Names are for logs; they must never be NULL, and 8/9 must never acquire a
 * name that implies an identification nobody has made. */
static void test_names(void)
{
    check(strcmp(scs_env_mtype_name(SCS_ENV_MTYPE_APP_MESSAGE),
                 "APP_MESSAGE") == 0, "MTYPE 10 renders as APP_MESSAGE");
    check(strcmp(scs_env_mtype_name(SCS_ENV_MTYPE_T8), "type 8") == 0 &&
              strcmp(scs_env_mtype_name(SCS_ENV_MTYPE_T9), "type 9") == 0,
          "MTYPEs 8 and 9 render as bare type numbers -- 8 is the special "
          "credit message and 9 is deliberately unnamed (vms-f03/#128), and a "
          "guessed name here would put a guess in the log");
    check(scs_env_mtype_name(4242) != NULL && scs_env_route_name(99) != NULL,
          "the name helpers never return NULL");
}

int main(void)
{
    test_golden_round_trip();
    test_refusals();
    test_shortest_class();
    test_route();
    test_frame_addressing();
    test_names();

    printf("test_scs_env: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
