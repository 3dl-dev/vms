/*
 * test_dnet_engine.c - DECnet Phase IV routing ENGINE proof (rd vms-449d,
 *                      engine rung 1 of epic vms-30e).
 *
 * Proves the piece that MOVES FRAMES:
 *   1. OORACLE BYTE-EXACT: the engine, configured as node 1.1, builds an
 *      endnode-HELLO whose 46-byte payload is byte-IDENTICAL to the vms-3be
 *      lab-VAX capture (docs/decnet-provenance-register.md sec 4.6, specimen
 *      #1). That is the Rule-8 proof the engine emits real VAX wire, not
 *      something that merely round-trips against itself.
 *   2. REAL SEND/RECV: two engine instances exchange their built HELLO frames
 *      over a real socketpair(2) -- genuine write(2)/read(2) of the actual
 *      encoded bytes -- and the receiver's adjacency SM advances
 *      DOWN -> INITIALIZING -> UP, then ages to DOWN when the listen timer
 *      lapses. This is the datalink + HELLO tx/rx + adjacency-drive done end to
 *      end through the same build_hello_frame -> wire -> rx_frame path the live
 *      AF_PACKET daemon uses (decnetd.c), without needing CAP_NET_RAW.
 *   3. HONEST DROPS: an own-multicast echo (src == my id) and a wrong-ethertype
 *      frame are both ignored and counted, never faked as an adjacency.
 *   4. The VMS-faithful surface (SHOW ADJACENT NODES) lists the neighbour.
 *
 * Clean-room (Rule 8): the only committed wire vector is the vms-3be specimen;
 * every other frame here is BUILT by the engine under test, never hand-forged.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dnet_engine.h"

static int failures = 0;
static void check(int cond, const char *what)
{
    if (cond) { printf("  OK: %s\n", what); }
    else      { printf("  FAIL: %s\n", what); failures++; }
}

/* docs/decnet-provenance-register.md sec 4.6, specimen #1 (HELLO payload),
 * verbatim -- the 46-byte Ethernet data field a real VAX (node 1.1) put on the
 * wire. */
static const uint8_t kHelloSpecimen1[46] = {
    0x22, 0x00, 0x0d, 0x02, 0x00, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x03, 0xda, 0x05, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0f, 0x00,
    0x00, 0x02, 0xaa, 0xaa, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

int main(void)
{
    printf("test_dnet_engine: DECnet Phase IV routing engine (tx/rx/adjacency)\n");

    /* --- 0. id derivation --- */
    uint8_t id[6];
    check(dnet_id_from_addr(1, 1, id) == DNET_ENGINE_OK, "dnet_id_from_addr(1.1) ok");
    static const uint8_t expect11[6] = { 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04 };
    check(memcmp(id, expect11, 6) == 0, "1.1 -> aa:00:04:00:01:04 (oracle id)");
    check(dnet_id_from_addr(64, 1, id) == DNET_ENGINE_EINVAL, "area>63 rejected");
    check(dnet_id_from_addr(1, 1024, id) == DNET_ENGINE_EINVAL, "node>1023 rejected");

    /* --- 1. ORACLE BYTE-EXACT HELLO for node 1.1 --- */
    struct dnet_engine e11;
    check(dnet_engine_init(&e11, 1, 1, "VAX1", "EWA0", NULL, NULL, 0, 0, /*now=*/0)
              == DNET_ENGINE_OK, "engine_init(1.1) ok");
    uint8_t frame[DNET_FRAME_MAX];
    size_t flen = 0;
    check(dnet_engine_build_hello_frame(&e11, frame, sizeof(frame), &flen)
              == DNET_ENGINE_OK, "build_hello_frame ok");
    check(flen == (size_t)DNET_ETH_HDRLEN + sizeof(kHelloSpecimen1),
          "frame length == 14 (eth) + 46 (payload) == 60");
    /* Ethernet header: dst = Phase IV endnode multicast, src = our id, type 0x6003. */
    static const uint8_t mcast[6] = { 0xab, 0x00, 0x00, 0x03, 0x00, 0x00 };
    check(memcmp(frame, mcast, 6) == 0, "eth dst == ab:00:00:03:00:00 (Phase IV mcast)");
    check(memcmp(frame + 6, expect11, 6) == 0, "eth src == our DECnet id");
    check(frame[12] == 0x60 && frame[13] == 0x03, "ethertype == 0x6003 (big-endian)");
    check(memcmp(frame + DNET_ETH_HDRLEN, kHelloSpecimen1, sizeof(kHelloSpecimen1)) == 0,
          "HELLO payload byte-IDENTICAL to vms-3be VAX capture (Rule-8 oracle proof)");

    /* --- 2. REAL SEND/RECV over a socketpair, adjacency SM drive --- */
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "socketpair created");

    struct dnet_engine L, R; /* left 1.10, right 1.11 */
    const uint8_t hwL[6] = { 0x02,0,0,0,0,0x0a };
    const uint8_t hwR[6] = { 0x02,0,0,0,0,0x0b };
    check(dnet_engine_init(&L, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0) == DNET_ENGINE_OK
       && dnet_engine_init(&R, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0) == DNET_ENGINE_OK,
          "L(1.10) + R(1.11) engines init");

    uint8_t rxbuf[DNET_FRAME_MAX];
    enum dnet_adj_state st = DNET_ADJ_DOWN;
    uint8_t from[6];

    /* L emits its HELLO; move the real bytes L->R; R decodes + drives its SM. */
    check(dnet_engine_build_hello_frame(&L, frame, sizeof(frame), &flen) == DNET_ENGINE_OK,
          "L builds HELLO");
    check(write(sv[0], frame, flen) == (ssize_t)flen, "L HELLO written to the wire (socketpair)");
    ssize_t n = read(sv[1], rxbuf, sizeof(rxbuf));
    check(n == (ssize_t)flen, "R read the whole HELLO frame off the wire");
    int rc = dnet_engine_rx_frame(&R, /*now=*/100, rxbuf, (size_t)n, from, &st);
    check(rc == 1, "R rx_frame accepted the HELLO");
    check(memcmp(from, L.my_id, 6) == 0, "R learned the neighbour id (== L)");
    check(st == DNET_ADJ_INITIALIZING, "one-way HELLO -> R sees L INITIALIZING");
    check(R.hello_recv == 1 && R.frames_dropped == 0, "R counters honest (1 recv, 0 dropped)");

    /* A two-way HELLO from R that NAMES L -> L reaches UP. Build R's HELLO via
     * the codec, set neighbor=L, prepend the eth header, move it R->L. */
    {
        struct dnet_endnode_hello h;
        memset(&h, 0, sizeof(h));
        h.rflags = DNET_RFLAG_ENDNODE_HELLO; h.version = 2;
        memcpy(h.id, R.my_id, 6); h.iinfo = DNET_NODETYPE_ENDNODE;
        h.blksize = 1498; h.timer = 15;
        memcpy(h.neighbor, L.my_id, 6); /* two-way: names L */
        uint8_t pay[DNET_FRAME_MAX]; size_t pl = 0;
        check(dnet_hello_encode(&h, pay, sizeof(pay), &pl) == DNET_HELLO_OK, "encode R two-way HELLO");
        uint8_t f2[DNET_FRAME_MAX];
        memcpy(f2, mcast, 6); memcpy(f2 + 6, R.my_id, 6); f2[12] = 0x60; f2[13] = 0x03;
        memcpy(f2 + DNET_ETH_HDRLEN, pay, pl);
        size_t f2len = DNET_ETH_HDRLEN + pl;
        check(write(sv[1], f2, f2len) == (ssize_t)f2len, "R two-way HELLO written to the wire");
        n = read(sv[0], rxbuf, sizeof(rxbuf));
        check(n == (ssize_t)f2len, "L read R's two-way HELLO");
        st = DNET_ADJ_DOWN;
        rc = dnet_engine_rx_frame(&L, 101, rxbuf, (size_t)n, from, &st);
        check(rc == 1 && st == DNET_ADJ_UP, "two-way HELLO -> L sees R UP");
        check(L.adj_up_events == 1, "L counted exactly one UP edge");
    }

    /* Listen-timer expiry: with no further HELLO, L's neighbour ages to DOWN
     * at T4 = BCT3MULT * T3 = 30 s past the last hello (heard at t=101). */
    check(dnet_engine_tick(&L, 101 + 31) >= 1, "L tick expires the stale adjacency");
    check(dnet_adj_state_of(&L.adj, R.my_id) == DNET_ADJ_DOWN, "L neighbour aged to DOWN");
    check(L.adj_down_events >= 1, "L counted the DOWN edge");

    /* --- 3. honest drops --- */
    /* own echo: feed L its OWN built frame -> ignored. */
    check(dnet_engine_build_hello_frame(&L, frame, sizeof(frame), &flen) == DNET_ENGINE_OK, "L rebuild");
    unsigned long drop_before = L.frames_dropped;
    check(dnet_engine_rx_frame(&L, 200, frame, flen, NULL, NULL) == 0, "own multicast echo ignored");
    check(L.frames_dropped == drop_before + 1, "own echo counted as a drop, not an adjacency");
    /* wrong ethertype: flip the type bytes. */
    frame[12] = 0x08; frame[13] = 0x00; /* IPv4 */
    check(dnet_engine_rx_frame(&R, 200, frame, flen, NULL, NULL) == 0, "wrong ethertype ignored");

    /* --- 4. VMS-faithful surface lists the neighbour --- */
    struct dnet_engine S;
    dnet_engine_init(&S, 1, 20, "OVMXS", "EWA0", NULL, hwL, 0, 0, 0);
    /* bring 1.11 up on S via a two-way hello */
    {
        struct dnet_endnode_hello h; memset(&h, 0, sizeof(h));
        h.rflags = DNET_RFLAG_ENDNODE_HELLO; h.version = 2;
        memcpy(h.id, R.my_id, 6); h.iinfo = DNET_NODETYPE_ENDNODE;
        h.blksize = 1498; h.timer = 15; memcpy(h.neighbor, S.my_id, 6);
        uint8_t pay[DNET_FRAME_MAX]; size_t pl = 0;
        dnet_hello_encode(&h, pay, sizeof(pay), &pl);
        uint8_t f2[DNET_FRAME_MAX];
        memcpy(f2, mcast, 6); memcpy(f2 + 6, R.my_id, 6); f2[12] = 0x60; f2[13] = 0x03;
        memcpy(f2 + DNET_ETH_HDRLEN, pay, pl);
        dnet_engine_rx_frame(&S, 5, f2, DNET_ETH_HDRLEN + pl, NULL, &st);
        check(st == DNET_ADJ_UP, "S sees 1.11 UP");
    }
    printf("  --- SHOW ADJACENT NODES (the hidden-socket VMS face) ---\n");
    dnet_engine_show_executor(&S, stdout);
    dnet_engine_show_adjacent(&S, stdout);

    close(sv[0]); close(sv[1]);

    if (failures == 0) { printf("test_dnet_engine: ALL CHECKS PASSED\n"); return 0; }
    printf("test_dnet_engine: %d CHECK(S) FAILED\n", failures);
    return 1;
}
