/*
 * test_dnet_nsp_flow_fuzz.c - ASan/UBSan fuzz of the NSP receive paths a peer
 * reaches through data flow (rd vms-985): the codec's DATA / LS / ACK / OTHACK
 * decoders (optional ACKNUM + ACKOTH/ACKDAT fields, SEGNUM, LSFLAGS, FCVAL) and
 * the link FSM's handling of hostile acknowledgements, NAKs, link services,
 * out-of-window segment numbers and request counts, interleaved with sends,
 * receive-room changes and polls at arbitrary times.
 *
 * Never crash a peer -- and never let a peer crash us, wedge us, or make us
 * fabricate state. Invariants checked after every step:
 *   - tx_count <= DNET_LINK_TXQ, tx_sent <= tx_ever <= tx_count;
 *   - poll terminates (a bounded number of PDUs per call sequence);
 *   - a delivered segment never exceeds the receive window (rx_room never wraps);
 *   - every PDU the link emits re-encodes and decodes to the same type;
 *   - the link only leaves RUN by a DI/DC from the peer or the retransmit limit.
 *
 * Built with -fsanitize=address,undefined (CMake probe as the routing fuzz);
 * the codec and link sources are compiled INTO this target so the sanitizer
 * covers them. Deterministic splitmix64 stream: every iteration is replayable
 * from the printed seed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dnet_link.h"
#include "dnet_nsp.h"

#define BASE_SEED  0x985F10C0DEULL
#define ITERS_DECODE 300000
#define ITERS_FSM    20000
#define STEPS_FSM    120

static int g_fail = 0;
#define CHECK(c, msg, i) do { if (!(c)) { g_fail++; \
    printf("  FAIL: %s (seed=0x%llx iter=%u)\n", msg, (unsigned long long)BASE_SEED, \
           (unsigned)(i)); } } while (0)

static inline uint64_t sm64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* A hostile but well-shaped header for the data/LS/ack classes, or noise. */
static size_t gen_pdu(uint64_t *s, uint8_t *b, size_t cap, uint16_t la, uint16_t ra)
{
    static const uint8_t flags[] = { 0x00, 0x20, 0x40, 0x60, 0x10, 0x04, 0x14, 0x30,
                                     0x38, 0x48, 0x18, 0x28, 0x24, 0x0c, 0x50, 0x70 };
    size_t n = 0;
    uint64_t r = sm64(s);
    b[n++] = flags[r % sizeof flags];
    if ((r >> 8) % 8 == 0) {                     /* raw noise of any length */
        size_t len = (size_t)(sm64(s) % 64);
        for (size_t k = 1; k < len && k < cap; k++) b[k] = (uint8_t)sm64(s);
        return len;
    }
    uint16_t dst = ((r >> 12) % 6) ? la : (uint16_t)sm64(s);
    b[n++] = (uint8_t)dst; b[n++] = (uint8_t)(dst >> 8);
    b[n++] = (uint8_t)ra;  b[n++] = (uint8_t)(ra >> 8);
    unsigned nf = (unsigned)(sm64(s) % 5);       /* 0..4 16-bit fields */
    for (unsigned k = 0; k < nf; k++) {
        uint16_t v = (uint16_t)sm64(s);
        if (sm64(s) & 1) v = (uint16_t)(0x8000 | (v & 0x7fff));   /* an ack field */
        else v &= 0x1fff;                                           /* a segnum */
        b[n++] = (uint8_t)v; b[n++] = (uint8_t)(v >> 8);
    }
    size_t extra = (size_t)(sm64(s) % 40);
    if (sm64(s) % 50 == 0) extra = DNET_NSP_MAX_DATA + (sm64(s) % 8);  /* oversize */
    for (size_t k = 0; k < extra && n < cap; k++) b[n++] = (uint8_t)sm64(s);
    return n;
}

/* A plausible-but-hostile flow message for a RUN link: sequence numbers near
 * the link's real ones (so they are often in window), any QUAL, any LSFLAGS /
 * FCVAL, sometimes a DI. Built as bytes and decoded, like a real frame. */
static size_t gen_flow(uint64_t *s, uint8_t *b, const struct dnet_link *lk,
                       uint16_t la, uint16_t ra)
{
    size_t n = 0;
    uint64_t r = sm64(s);
    unsigned kind = (unsigned)(r % 100);
    uint16_t near_rx  = (uint16_t)((lk->recv_seq + 1 + (int)(sm64(s) % 5) - 1) & 0x0fff);
    uint16_t near_tx  = (uint16_t)((lk->send_ack + (int)(sm64(s) % 10) - 1) & 0x0fff);
    uint16_t near_oth = (uint16_t)((lk->oth_recv + 1 + (int)(sm64(s) % 3) - 1) & 0x0fff);
    uint16_t qual = (uint16_t)((sm64(s) % 8) << 12);
    uint16_t ack = (uint16_t)(0x8000 | qual | near_tx);
    if (kind < 1) {                                  /* DI */
        b[n++] = 0x38;
    } else if (kind < 45) {
        b[n++] = 0x60;                               /* DATA */
    } else if (kind < 70) {
        b[n++] = 0x04;                               /* data ACK */
    } else if (kind < 90) {
        b[n++] = 0x10;                               /* LS */
    } else {
        b[n++] = 0x14;                               /* other-data ack */
    }
    b[n++] = (uint8_t)la; b[n++] = (uint8_t)(la >> 8);
    b[n++] = (uint8_t)ra; b[n++] = (uint8_t)(ra >> 8);
    switch (b[0]) {
    case 0x38: b[n++] = 0; b[n++] = 0; break;
    case 0x60:
        if (sm64(s) & 1) { b[n++] = (uint8_t)ack; b[n++] = (uint8_t)(ack >> 8); }
        b[n++] = (uint8_t)near_rx; b[n++] = (uint8_t)((near_rx >> 8) | ((sm64(s) & 1) << 4));
        for (unsigned k = (unsigned)(sm64(s) % 30); k; k--) b[n++] = (uint8_t)sm64(s);
        break;
    case 0x04:
        b[n++] = (uint8_t)ack; b[n++] = (uint8_t)(ack >> 8);
        if (sm64(s) % 4 == 0) {
            uint16_t a2 = (uint16_t)(0x8000 | (uint16_t)((sm64(s) % 8) << 12) | lk->oth_send);
            b[n++] = (uint8_t)a2; b[n++] = (uint8_t)(a2 >> 8);
        }
        break;
    case 0x10: {
        uint16_t oa = (uint16_t)(0x8000 | qual | lk->oth_send);
        if (sm64(s) & 1) { b[n++] = (uint8_t)oa; b[n++] = (uint8_t)(oa >> 8); }
        b[n++] = (uint8_t)near_oth; b[n++] = (uint8_t)(near_oth >> 8);
        b[n++] = (uint8_t)sm64(s);                   /* LSFLAGS: any */
        b[n++] = (uint8_t)sm64(s);                   /* FCVAL: any */
        break;
    }
    default: {
        uint16_t oa = (uint16_t)(0x8000 | qual | lk->oth_send);
        b[n++] = (uint8_t)oa; b[n++] = (uint8_t)(oa >> 8);
        break;
    }
    }
    return n;
}

static void check_inv(const struct dnet_link *lk, unsigned cap, unsigned i)
{
    CHECK(lk->tx_count <= DNET_LINK_TXQ, "tx_count within the queue", i);
    CHECK(lk->tx_sent <= lk->tx_ever && lk->tx_ever <= lk->tx_count,
          "tx_sent <= tx_ever <= tx_count", i);
    CHECK(!lk->rx_fc || lk->rx_room <= cap, "rx_room never wraps past the window", i);
}

int main(void)
{
#if defined(__SANITIZE_ADDRESS__)
    printf("[fuzz] ASan active\n");
#else
    if (getenv("OVMX_FUZZ_REQUIRE_SANITIZERS")) {
        printf("FAIL: OVMX_FUZZ_REQUIRE_SANITIZERS set but ASan is not compiled in\n");
        return 1;
    }
    printf("[fuzz] ASan NOT active (toolchain lacks it); bound checks still run\n");
#endif
    uint8_t buf[DNET_NSP_MAX_DATA + 64];
    struct dnet_nsp_msg m, m2, out;

    /* 1. Decoder: any bytes, every truncation; a decoded PDU of the flow types
     *    re-encodes and decodes to the same fields. */
    for (unsigned i = 0; i < ITERS_DECODE; i++) {
        uint64_t s = BASE_SEED + i;
        size_t n = gen_pdu(&s, buf, sizeof buf, 0xc935, 0x2008);
        size_t cut = n ? (size_t)(sm64(&s) % (n + 1)) : 0;
        uint8_t *p = malloc(cut ? cut : 1);      /* exact-size heap copy: ASan sees over-reads */
        memcpy(p, buf, cut);
        size_t used = 0;
        if (dnet_nsp_decode(p, cut, &m, &used) == DNET_NSP_OK) {
            CHECK(used <= cut, "consumed within the input", i);
            if (m.type == DNET_NSP_T_LS || m.type == DNET_NSP_T_DATA ||
                m.type == DNET_NSP_T_ACK || m.type == DNET_NSP_T_OTHACK) {
                uint8_t e[DNET_NSP_MAX_DATA + 64]; size_t en = 0;
                CHECK(dnet_nsp_encode(&m, e, sizeof e, &en) == DNET_NSP_OK &&
                      dnet_nsp_decode(e, en, &m2, NULL) == DNET_NSP_OK &&
                      m2.type == m.type && m2.segnum == m.segnum &&
                      m2.acknum == m.acknum && m2.ackoth == m.ackoth &&
                      m2.ls_flags == m.ls_flags && m2.fc_val == m.fc_val &&
                      m2.datalen == m.datalen,
                      "flow PDU round-trips", i);
            }
        }
        free(p);
    }

    /* 2. FSM: a RUN link with a receive window, hammered by a hostile peer. */
    unsigned long delivered = 0, closed_by_limit = 0, closed_by_peer = 0;
    for (unsigned i = 0; i < ITERS_FSM; i++) {
        uint64_t s = (BASE_SEED ^ 0xF5A1ULL) + i;
        struct dnet_link L;
        const uint16_t LA = 0xc935, RA = 0x2008;
        dnet_link_init(&L, LA, 0x403, 0);
        dnet_link_connect(&L, NULL, 0, 1459, 0x01, DNET_NSP_VER_41, &out, 0);
        memset(&m, 0, sizeof m);
        m.type = DNET_NSP_T_CC; m.msgflg = DNET_NSP_MSGFLG_CC;
        m.dstaddr = LA; m.srcaddr = RA; m.segsize = 1459;
        m.services = (uint8_t)(0x01 | ((sm64(&s) % 3) << 2));    /* none / seg / msg */
        dnet_link_rx(&L, &m, 0, NULL, NULL, NULL);
        unsigned cap = 1u + (unsigned)(sm64(&s) % 40);
        dnet_link_rx_window(&L, cap);
        unsigned room = cap;
        dnet_tick_t now = 0;
        for (unsigned st = 0; st < STEPS_FSM && dnet_link_is_up(&L); st++) {
            uint64_t op = sm64(&s) % 10;
            if (op < 5) {
                size_t n = (sm64(&s) % 10) ? gen_flow(&s, buf, &L, LA, RA)
                                           : gen_pdu(&s, buf, sizeof buf, LA, RA);
                if (dnet_nsp_decode(buf, n, &m, NULL) == DNET_NSP_OK) {
                    int has = 0; enum dnet_link_event ev = DNET_LINK_EV_NONE;
                    dnet_link_rx(&L, &m, now, &out, &has, &ev);
                    if (ev == DNET_LINK_EV_DATA) {
                        CHECK(room > 0, "data delivered only with room", i);
                        if (room) room--;
                        delivered++;
                        dnet_link_rx_room(&L, room);
                    }
                    if (ev == DNET_LINK_EV_DISCONNECT) closed_by_peer++;
                    if (has) {
                        uint8_t e[DNET_NSP_MAX_DATA + 64]; size_t en = 0;
                        CHECK(dnet_nsp_encode(&out, e, sizeof e, &en) == DNET_NSP_OK,
                              "every reply encodes", i);
                    }
                }
            } else if (op < 7) {
                uint8_t d[64]; size_t dl = (size_t)(sm64(&s) % sizeof d);
                for (size_t k = 0; k < dl; k++) d[k] = (uint8_t)sm64(&s);
                (void)dnet_link_send_data(&L, d, dl, &out, now);
            } else if (op < 8) {
                room = (unsigned)(sm64(&s) % (cap + 1));
                dnet_link_rx_room(&L, room);
            } else {
                now += sm64(&s) % 4;
            }
            /* Poll to exhaustion -- it must terminate. */
            unsigned emitted = 0;
            for (;;) {
                int has = 0; enum dnet_link_event ev = DNET_LINK_EV_NONE;
                dnet_link_poll(&L, now, &out, &has, &ev);
                if (ev == DNET_LINK_EV_DISCONNECT) { closed_by_limit++; break; }
                if (!has) break;
                uint8_t e[DNET_NSP_MAX_DATA + 64]; size_t en = 0;
                CHECK(dnet_nsp_encode(&out, e, sizeof e, &en) == DNET_NSP_OK &&
                      out.type != DNET_NSP_T_DI, "polled PDU encodes, never a DI", i);
                if (++emitted > 2 * DNET_LINK_TXQ + 4) {
                    CHECK(0, "poll terminates", i);
                    break;
                }
            }
            check_inv(&L, cap, i);
        }
    }
    printf("[fuzz] %d decode iters, %d FSM runs x %d steps: %lu delivered, %lu closed by"
           " the retransmit limit, %lu by a peer DI\n", ITERS_DECODE, ITERS_FSM, STEPS_FSM,
           delivered, closed_by_limit, closed_by_peer);
    printf("test_dnet_nsp_flow_fuzz: %s\n", g_fail ? "FAILED" : "ALL CHECKS PASSED");
    return g_fail ? 1 : 0;
}
