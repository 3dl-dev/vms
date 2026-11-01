/*
 * test_l2_ring.c - host unit test of the executive L2 receive-ring bookkeeping
 * (rd vms-9394, src/kernel-core/vms_l2_ring.h): the SAME header-only index
 * arithmetic the NetBSD vms.kmod runs under its rx spin mutex
 * (src/kernel-netbsd/vms_socket_netbsd.c, exec_l2_recv / ovmx_l2_deliver),
 * driven here without a kernel. The live proof -- a booted OVMX/VAX NETACP
 * receiving DECnet frames through it and SET HOST working both ways with an
 * OVMX/x86 node -- is tests/lab-vax/test_decnet_pair_vax.sh.
 */
#include <stdio.h>
#include <string.h>

#include "vms_l2_ring.h"

static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else { printf("  FAIL: %s\n", m); fail++; } } while (0)

#define CAP 4

int main(void)
{
    struct vms_l2_ring r;
    unsigned int tag[CAP];      /* "frame contents": the sequence number */
    unsigned int slot, i, seq = 0, ok;

    printf("=== test_l2_ring ===\n");

    vms_l2_ring_init(&r, CAP);
    CHECK(r.count == 0 && !vms_l2_ring_pop(&r, &slot),
          "a fresh ring is empty: pop returns nothing (RECV waits, no fabricated frame)");

    /* FIFO order under capacity. */
    for (i = 0; i < 3; i++) {
        slot = vms_l2_ring_push(&r);
        tag[slot] = ++seq;
    }
    ok = 1;
    for (i = 1; i <= 3; i++)
        ok &= vms_l2_ring_pop(&r, &slot) && tag[slot] == i;
    CHECK(ok && r.count == 0, "frames come out in arrival order (1,2,3)");
    CHECK(r.drops == 0, "no frame is dropped while the ring has room");

    /* Overflow: push CAP+2 frames -> the two OLDEST are dropped. */
    for (i = 0; i < CAP + 2; i++) {
        slot = vms_l2_ring_push(&r);
        tag[slot] = ++seq;          /* seq 4..9 */
    }
    CHECK(r.count == CAP, "a full ring holds exactly its capacity");
    CHECK(r.drops == 2, "overflowing by two drops exactly two frames");
    ok = vms_l2_ring_pop(&r, &slot);
    CHECK(ok && tag[slot] == seq - CAP + 1,
          "drop-OLDEST: the oldest survivor is the (CAP)th-newest frame, not the first one pushed");
    ok = 1;
    for (i = 2; i <= CAP; i++)
        ok &= vms_l2_ring_pop(&r, &slot) && tag[slot] == seq - CAP + i;
    CHECK(ok && !vms_l2_ring_pop(&r, &slot), "...then the rest in order, then empty");

    /* Wrap-around: indices stay inside [0, cap) across many cycles. */
    ok = 1;
    for (i = 0; i < 1000; i++) {
        slot = vms_l2_ring_push(&r);
        ok &= slot < CAP;
        if (i % 3 == 0) {
            unsigned int s2;
            ok &= vms_l2_ring_pop(&r, &s2) && s2 < CAP;
        }
    }
    CHECK(ok && r.count <= CAP, "1000 mixed push/pop cycles never index outside the ring");

    /* Ethertype classification (the field the rx path filters on). */
    {
        unsigned char dnet[14] = { 0xAB, 0, 0, 3, 0, 0,  0xAA, 0, 4, 0, 0x2B, 4,  0x60, 0x03 };
        unsigned char scs[14]  = { 0xAB, 0, 4, 1, 0, 0,  0x08, 0, 0x2B, 1, 2, 3,  0x60, 0x07 };
        CHECK(vms_l2_frame_ethertype(dnet) == 0x6003,
              "a DECnet routing frame (AA-00-04-00-2B-04 -> AB-00-00-03-00-00) classifies as 0x6003");
        CHECK(vms_l2_frame_ethertype(scs) == 0x6007,
              "a cluster SCS frame classifies as 0x6007 (left to the cluster path)");
    }

    printf("=== test_l2_ring: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
