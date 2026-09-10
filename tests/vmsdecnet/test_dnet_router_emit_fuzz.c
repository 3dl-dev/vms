/*
 * test_dnet_router_emit_fuzz.c - isolation proof of the DECnet Phase IV ROUTER
 *                                run-mode EMIT path (rd vms-0a9).
 *
 * WHY THIS EXISTS (⭐⭐ never crash a peer): --router mode makes DECNETD emit a
 * spec-faithful Ethernet ROUTER-hello so a real Phase IV ENDNODE selects OVMX as
 * its designated router (the rd vms-aac0 live campaign). The router-hello codec
 * is SPEC-DERIVED -- no oracle wire specimen exists (see dnet_router_hello.h) --
 * so before OVMX ever puts a router-hello on a segment shared with a real VAX,
 * the frame it EMITS must be proven well-formed and safe HERE, entirely in
 * isolation. This test touches NO real node: every byte moves in-process, and
 * the receive side is another OVMX engine.
 *
 * The sibling test_dnet_routing_fuzz.c fuzzes the RECEIVE/decode path; THIS file
 * fuzzes the EMIT path -- the exact bytes dnet_engine_build_router_hello_frame()
 * produces, the endnode DR-selection those bytes drive, and the encoder under
 * adversarial field/cap values.
 *
 * WHAT IT PROVES:
 *   1. ROUND-TRIP: the router-hello DECNETD emits decodes back to every field it
 *      was built with (a real Phase IV endnode must be able to parse it).
 *   2. NODE-TYPE: the emitted IINFO low 2 bits say "L1 router", so an endnode
 *      treats the sender as a designated-router candidate.
 *   3. DR-SELECTION (the routing path): feeding the emitted router-hello into an
 *      OVMX endnode's dnet_engine_rx_frame drives a VALID transition -- the
 *      endnode selects the router and then NAMES it in the rtr/neighbor field of
 *      its own endnode-hello (the field vms-aac0 will watch a real VAX flip),
 *      with higher-priority preemption and the DNA higher-address tie-break.
 *   4. FUZZ-CLEAN under ASan/UBSan (>=100k iters each): the ENCODER never
 *      crashes / over-writes / hits UB under adversarial field values and
 *      boundary output-buffer caps; and the emitted frame, arbitrarily MUTATED,
 *      never crashes the endnode receive path.
 *
 * ANTI-LARP / TEETH (rd vms-abf): built WITH -fsanitize=address,undefined by its
 * CMake target; an OOB/UB is a HARD failure. Encoding is done into an EXACT-size
 * heap buffer (malloc(cap)) so any write at offset >= cap is a real ASan heap-
 * buffer-overflow, and received frames are copied into malloc(n) so any read
 * past the logical length is caught. When OVMX_FUZZ_REQUIRE_SANITIZERS is set
 * (the gated CI leg sets it) a build WITHOUT ASan HARD-FAILS rather than pass
 * toothless. Teeth confirmed out-of-tree: removing the `cap < total` guard from
 * a scratch copy of dnet_router_hello_encode makes this fuzz trip an ASan heap-
 * buffer-overflow within the first few thousand iterations; the guard is present
 * in the shipped codec, so the fuzz stays silent -- teeth without weakening the
 * codec.
 *
 * Clean-room (CLAUDE.md Rule 8): no VSI/HPE/DEC source consulted; a test harness
 * over OVMX's own engine + codecs.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dnet_engine.h"          /* pulls dnet_router_hello.h / dnet_hello.h */

/* rd vms-abf: is AddressSanitizer actually compiled in? The -fsanitize flags are
 * probe-gated in CMake; a CI image lacking the ASan runtime would silently build
 * this crash-vector proof TOOTHLESS while still passing. With
 * OVMX_FUZZ_REQUIRE_SANITIZERS set, a toothless build HARD-FAILS. */
#if defined(__SANITIZE_ADDRESS__)
#  define OVMX_FUZZ_ASAN_ACTIVE 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define OVMX_FUZZ_ASAN_ACTIVE 1
#  else
#    define OVMX_FUZZ_ASAN_ACTIVE 0
#  endif
#else
#  define OVMX_FUZZ_ASAN_ACTIVE 0
#endif

/* Iteration counts (each >= the 100k the item mandates; sized so the whole
 * ctest runs well under 60s even under ASan+UBSan). */
#define FUZZ_ITERS_ENCODE  150000
#define FUZZ_ITERS_RXMUT   150000

#define BASE_SEED  0x0A9C0DEC0FFEE511ULL

static int g_fail = 0;
#define CHECK(cond, msg) do {                                               \
        if (!(cond)) {                                                      \
            printf("  CHECK FAILED: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

/* pure, per-iteration-seeded splitmix64 (libc-rand-independent, replayable). */
static inline uint64_t sm64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Allocate an EXACTLY-n-byte heap buffer so ASan has real teeth (any read/write
 * at offset >= n is a hard heap-buffer-overflow). When src != NULL its n bytes
 * are copied in; when src == NULL the buffer is left uninitialised (an exact-cap
 * output buffer for the encoder). Caller frees. */
static uint8_t *heap_exact(const uint8_t *src, size_t n)
{
    uint8_t *h = (uint8_t *)malloc(n ? n : 1);
    if (h && n && src)
        memcpy(h, src, n);
    return h;
}

/* Stand up an endnode engine (1.<node>) with a fixed hw mac. */
static void mk_endnode(struct dnet_engine *e, unsigned node)
{
    uint8_t hw[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, (uint8_t)node };
    CHECK(dnet_engine_init(e, 1, node, "OVMXE", "EWA0", NULL, hw, 0, 0, 0) == 0,
          "endnode init");
}

/* Stand up a router engine (1.<node>) with a fixed hw mac + priority. */
static void mk_router(struct dnet_engine *e, unsigned node, uint8_t prio)
{
    uint8_t hw[6] = { 0x02, 0x00, 0x00, 0x00, 0x01, (uint8_t)node };
    CHECK(dnet_engine_init(e, 1, node, "OVMXR", "EWA0", NULL, hw, 0, 0, 0) == 0,
          "router init");
    CHECK(dnet_engine_set_router(e, prio) == 0 && dnet_engine_is_router(e),
          "set_router");
}

/* ---- 1+2. the emitted router-hello round-trips + says "router" ----------- */
static void emit_roundtrip_and_nodetype(void)
{
    struct dnet_engine R;
    mk_router(&R, 42, 64);

    uint8_t frame[DNET_FRAME_MAX];
    size_t flen = 0;
    CHECK(dnet_engine_build_router_hello_frame(&R, frame, sizeof frame, &flen)
              == DNET_ENGINE_OK, "build_router_hello_frame ok");

    /* Ethernet header: dst = all-endnodes multicast, src = our id, 0x6003. */
    CHECK(memcmp(frame, DNET_ROUTER_HELLO_MCAST, 6) == 0,
          "eth dst == ab:00:00:04:00:00 (all-endnodes mcast)");
    CHECK(memcmp(frame + 6, R.my_id, 6) == 0, "eth src == our DECnet id");
    CHECK(frame[12] == 0x60 && frame[13] == 0x03, "ethertype 0x6003");

    /* NODE-TYPE: IINFO (payload offset 12) low 2 bits == L1 router. */
    CHECK((frame[DNET_ETH_HDRLEN + 12] & 0x03u) == DNET_NODETYPE_L1ROUTER,
          "emitted IINFO node-type == L1 router (endnode would treat as DR candidate)");

    /* ROUND-TRIP: decode the payload back and check every field against what the
     * engine advertises (a real Phase IV endnode must be able to parse it). */
    struct dnet_router_hello d;
    size_t consumed = 0;
    CHECK(dnet_router_hello_decode(frame + DNET_ETH_HDRLEN,
                                   flen - DNET_ETH_HDRLEN, &d, &consumed)
              == DNET_ROUTER_HELLO_OK, "emitted router-hello decodes");
    CHECK(d.rflags == DNET_RFLAG_ROUTER_HELLO, "rflags == router-hello (0x0b)");
    CHECK(d.version == 2, "version == 2 (oracle DNA version)");
    CHECK(d.eco == 0 && d.user_eco == 0, "eco/user_eco zero");
    CHECK(memcmp(d.id, R.my_id, 6) == 0, "id == our Ethernet id");
    CHECK((d.iinfo & 0x03u) == DNET_NODETYPE_L1ROUTER, "decoded node-type L1 router");
    CHECK(d.blksize == R.blksize, "blksize round-trips");
    CHECK(d.priority == 64, "priority round-trips (advertised 64)");
    CHECK(d.area == 0, "area honest-zero");
    CHECK(d.timer == R.adj.t3, "timer == advertised T3");
    CHECK(d.mpd == 0, "mpd reserved-zero");
    CHECK(d.elist_len == 0, "E-list honest-empty (no fabricated router list)");

    /* A non-router engine must refuse to build a router-hello. */
    struct dnet_engine E;
    mk_endnode(&E, 11);
    CHECK(dnet_engine_build_router_hello_frame(&E, frame, sizeof frame, &flen)
              == DNET_ENGINE_EINVAL, "endnode refuses to emit a router-hello");
}

/* ---- 3. DR-selection: the emitted router-hello drives endnode selection --- */
static void dr_selection_routing_path(void)
{
    struct dnet_engine R, E;
    mk_router(&R, 42, 64);
    mk_endnode(&E, 11);

    uint8_t frame[DNET_FRAME_MAX];
    size_t flen = 0;
    CHECK(dnet_engine_build_router_hello_frame(&R, frame, sizeof frame, &flen)
              == DNET_ENGINE_OK, "R build router-hello");

    /* Feed the EMITTED bytes into the endnode's routing path via an exact-size
     * heap buffer (ASan teeth on the receive decode). */
    uint8_t *hb = heap_exact(frame, flen);
    uint8_t from[6];
    enum dnet_adj_state st = DNET_ADJ_DOWN;
    int rc = dnet_engine_rx_frame(&E, 100, hb, flen, from, &st);
    free(hb);
    CHECK(rc == 1, "endnode accepted the router-hello");
    CHECK(E.have_dr && memcmp(E.dr_id, R.my_id, 6) == 0,
          "endnode SELECTED the router as its designated router");
    CHECK(memcmp(from, R.my_id, 6) == 0, "from_out == router id");

    /* The endnode's own endnode-hello must now NAME the router in rtr (offset 24
     * of the endnode-hello payload) -- the field a real VAX endnode flips. */
    CHECK(dnet_engine_build_hello_frame(&E, frame, sizeof frame, &flen)
              == DNET_ENGINE_OK, "E build endnode-hello after selection");
    CHECK(memcmp(frame + DNET_ETH_HDRLEN + 24, R.my_id, 6) == 0,
          "endnode-hello rtr field now names the selected router");

    /* Higher-priority preemption: a second router with priority 100 wins. */
    struct dnet_engine R2;
    mk_router(&R2, 7, 100);
    CHECK(dnet_engine_build_router_hello_frame(&R2, frame, sizeof frame, &flen)
              == DNET_ENGINE_OK, "R2 build router-hello");
    hb = heap_exact(frame, flen);
    rc = dnet_engine_rx_frame(&E, 101, hb, flen, NULL, NULL);
    free(hb);
    CHECK(rc == 1 && E.have_dr && memcmp(E.dr_id, R2.my_id, 6) == 0,
          "higher-priority router preempts the DR");

    /* Lower-priority router does NOT displace the current DR. */
    struct dnet_engine R3;
    mk_router(&R3, 9, 10);
    CHECK(dnet_engine_build_router_hello_frame(&R3, frame, sizeof frame, &flen)
              == DNET_ENGINE_OK, "R3 build router-hello");
    hb = heap_exact(frame, flen);
    rc = dnet_engine_rx_frame(&E, 102, hb, flen, NULL, NULL);
    free(hb);
    CHECK(rc == 1 && memcmp(E.dr_id, R2.my_id, 6) == 0,
          "lower-priority router does not displace the higher-priority DR");

    /* An endnode-hello (not a router) never sets a DR. */
    struct dnet_engine E2, En;
    mk_endnode(&E2, 20);
    mk_endnode(&En, 21);
    CHECK(dnet_engine_build_hello_frame(&En, frame, sizeof frame, &flen)
              == DNET_ENGINE_OK, "plain endnode-hello built");
    hb = heap_exact(frame, flen);
    rc = dnet_engine_rx_frame(&E2, 100, hb, flen, NULL, NULL);
    free(hb);
    CHECK(!E2.have_dr, "an endnode-hello never selects a DR");
}

/* Fill a dnet_router_hello with fully-adversarial field values from the stream
 * (including elist_len well past the cap, to exercise the EBADLEN guard). */
static void fuzz_fill_router(uint64_t *st, struct dnet_router_hello *r)
{
    r->rflags   = (uint8_t)sm64(st);
    r->version  = (uint8_t)sm64(st);
    r->eco      = (uint8_t)sm64(st);
    r->user_eco = (uint8_t)sm64(st);
    for (int k = 0; k < DNET_ADDR_LEN; k++) r->id[k] = (uint8_t)sm64(st);
    r->iinfo    = (uint8_t)sm64(st);
    r->blksize  = (uint16_t)sm64(st);
    r->priority = (uint8_t)sm64(st);
    r->area     = (uint8_t)sm64(st);
    r->timer    = (uint16_t)sm64(st);
    r->mpd      = (uint8_t)sm64(st);
    /* elist_len across the whole uint8_t range: 0..128 valid, 129..255 -> the
     * EBADLEN reject path. elist[] filled fully so a valid memcpy is in-bounds. */
    r->elist_len = (uint8_t)sm64(st);
    for (int k = 0; k < DNET_ROUTER_HELLO_MAX_ELIST; k++)
        r->elist[k] = (uint8_t)sm64(st);
}

/* ---- 4a. ENCODE fuzz: adversarial fields + boundary output caps ---------- */
static void fuzz_encode(void)
{
    unsigned ok = 0, nospace = 0, badlen = 0;
    for (unsigned i = 0; i < FUZZ_ITERS_ENCODE; i++) {
        uint64_t st = BASE_SEED ^ 0x2222222222222222ULL;
        st += i;                                  /* independent per-iter stream */

        struct dnet_router_hello r;
        memset(&r, 0, sizeof r);
        fuzz_fill_router(&st, &r);

        /* Fuzz the output cap across [0 .. a bit past a maximal frame] so the
         * ENOSPACE bound is exercised at every boundary. Encode into an EXACT-
         * size heap buffer: a write at offset >= cap is a hard ASan overflow. */
        size_t cap = (size_t)(sm64(&st) % 260u);
        uint8_t *buf = heap_exact(NULL, cap);     /* uninitialised cap bytes */
        size_t outlen = 0xdead;
        int rc = dnet_router_hello_encode(&r, buf, cap, &outlen);

        if (rc == DNET_ROUTER_HELLO_OK) {
            ok++;
            CHECK(outlen <= cap, "encode outlen within cap");
            /* The emitted bytes must decode back with the SAME grounded fields
             * (round-trip closure on adversarial input). Decode from an exact
             * copy of exactly outlen bytes. */
            uint8_t *dbuf = heap_exact(buf, outlen);
            struct dnet_router_hello d;
            size_t consumed = 0;
            int drc = dnet_router_hello_decode(dbuf, outlen, &d, &consumed);
            CHECK(drc == DNET_ROUTER_HELLO_OK, "emitted bytes decode");
            if (drc == DNET_ROUTER_HELLO_OK) {
                CHECK(d.rflags == r.rflags && d.version == r.version &&
                      d.priority == r.priority && d.timer == r.timer &&
                      d.iinfo == r.iinfo && d.blksize == r.blksize &&
                      d.elist_len == r.elist_len &&
                      memcmp(d.id, r.id, DNET_ADDR_LEN) == 0 &&
                      (r.elist_len == 0 ||
                       memcmp(d.elist, r.elist, r.elist_len) == 0),
                      "encode->decode round-trips every field");
            }
            free(dbuf);
        } else if (rc == DNET_ROUTER_HELLO_ENOSPACE) {
            nospace++;
        } else if (rc == DNET_ROUTER_HELLO_EBADLEN) {
            badlen++;               /* elist_len past the cap: rejected, no memcpy */
        }
        free(buf);
    }
    printf("  encode fuzz: %u ok, %u nospace, %u badlen over %u iters\n",
           ok, nospace, badlen, FUZZ_ITERS_ENCODE);
    CHECK(ok > 0 && nospace > 0 && badlen > 0,
          "encode fuzz reached ok + ENOSPACE + EBADLEN paths");
}

/* ---- 4b. mutated-emitted-frame receive fuzz: never crash the endnode ------ */
static void fuzz_rx_mutated_emit(void)
{
    /* Build ONE valid emitted router-hello, then feed arbitrarily-mutated copies
     * of it into the endnode receive path. The engine must never crash / OOB /
     * UB on any mutation a wire gremlin could produce. */
    struct dnet_engine R;
    mk_router(&R, 42, 64);
    uint8_t base[DNET_FRAME_MAX];
    size_t blen = 0;
    CHECK(dnet_engine_build_router_hello_frame(&R, base, sizeof base, &blen)
              == DNET_ENGINE_OK, "seed emitted frame for mutation fuzz");

    unsigned accepted = 0, dropped = 0;
    for (unsigned i = 0; i < FUZZ_ITERS_RXMUT; i++) {
        uint64_t st = BASE_SEED ^ 0x3333333333333333ULL;
        st += i;

        /* Copy the frame and apply 1..8 random single-byte pokes, and sometimes
         * truncate/extend the logical length. */
        uint8_t tmp[DNET_FRAME_MAX];
        size_t n = blen;
        memcpy(tmp, base, blen);
        unsigned pokes = 1u + (unsigned)(sm64(&st) % 8u);
        for (unsigned p = 0; p < pokes && n; p++) {
            size_t off = (size_t)(sm64(&st) % n);
            tmp[off] = (uint8_t)sm64(&st);
        }
        /* 1-in-4: change the logical length (truncate or pad within the buffer). */
        if ((sm64(&st) & 3u) == 0) {
            n = (size_t)(sm64(&st) % (blen + 8u));
            if (n > sizeof tmp) n = sizeof tmp;
        }

        /* Exact-size heap copy => ASan teeth on any receive-path over-read. */
        uint8_t *hb = heap_exact(tmp, n);
        struct dnet_engine E;
        mk_endnode(&E, 11);
        uint8_t from[6];
        enum dnet_adj_state stt = DNET_ADJ_DOWN;
        int rc = dnet_engine_rx_frame(&E, 100 + i, hb, n, from, &stt);
        free(hb);
        CHECK(rc == 0 || rc == 1, "rx_frame returns a valid accept/ignore code");
        if (rc == 1) accepted++; else dropped++;
    }
    printf("  rx-mutation fuzz: %u accepted, %u dropped over %u iters\n",
           accepted, dropped, FUZZ_ITERS_RXMUT);
    CHECK(dropped > 0, "mutation fuzz exercised the drop path");
}

int main(void)
{
    printf("test_dnet_router_emit_fuzz: DECnet router-hello EMIT-path isolation "
           "proof (rd vms-0a9)\n");
    printf("  base seed = 0x%016llx (replayable); AddressSanitizer %s\n",
           (unsigned long long)BASE_SEED,
           OVMX_FUZZ_ASAN_ACTIVE ? "ACTIVE (teeth on)" : "NOT active (no teeth)");

    /* rd vms-abf teeth-enforcement: refuse to pass toothless when required. */
    const char *require_san = getenv("OVMX_FUZZ_REQUIRE_SANITIZERS");
    if (require_san && require_san[0] && !OVMX_FUZZ_ASAN_ACTIVE) {
        fprintf(stderr,
                "test_dnet_router_emit_fuzz: FAIL -- OVMX_FUZZ_REQUIRE_SANITIZERS "
                "is set but this binary was built WITHOUT AddressSanitizer, so the "
                "emit-path crash-vector proof has NO TEETH. Refusing to pass "
                "toothless (rd vms-abf).\n");
        return 1;
    }

    emit_roundtrip_and_nodetype();
    dr_selection_routing_path();
    fuzz_encode();
    fuzz_rx_mutated_emit();

    if (g_fail == 0) {
        printf("test_dnet_router_emit_fuzz: ALL CHECKS PASSED "
               "(NO real node touched -- isolation only)\n");
        return 0;
    }
    printf("test_dnet_router_emit_fuzz: %d CHECK(S) FAILED\n", g_fail);
    return 1;
}
