/*
 * test_scs_dgram.c - SCA datagram buffer management: the per-port DFREEQ and
 * the per-connection datagram buffer count in the CDT (vms-b1d).
 *
 * Asserts the documented mechanism of Roy G. Davis, *VAXcluster Principles*,
 * Digital Press 1993, ch. 2 sec 2.8:
 *   - a SYSAP's CONNECT/ACCEPT buffer request is stored in the CDT and the
 *     buffers go into the DFREEQ of the port supporting the connection (2-42)
 *   - a received datagram dequeues a DFREEQ buffer, then debits the CDT's
 *     count and reaches the datagram input routine                    (2-42)
 *   - CDT count 0 -> the datagram is DISCARDED and the buffer goes BACK to the
 *     DFREEQ, which is what stops one connection eating another's     (2-42)
 *   - DFREEQ empty -> "the port merely discards the datagram"          (2-42)
 *   - the SYSAP returns the buffer (both counts rise) or deallocates it
 *     (neither does)                                                  (2-42)
 *   - a SYSAP can add and remove buffers later, clamped at its own deposit
 *                                                                (2-43 bank)
 *   - the DFREEQ is PER PORT and shared by the connections on it      (2-43)
 *   - every discard is COUNTED, per connection for the no-quota class and per
 *     port for the empty-DFREEQ class, and printed by scs_dgram_report -- the
 *     INV-6 half of the item: silent on the wire must not mean invisible here
 *   - THE ORDER of the debit and the callback: p. 2-42 decrements the CDT's
 *     count "and then" passes the buffer to the input routine. That order is
 *     invisible to an inert SYSAP fake (both orders leave the same final
 *     counts), so it is driven with a RE-ENTRANT one -- see the block above
 *     test_input_routine_sees_the_debit_already_applied
 *   - the accounted and unaccounted CDL receive paths RESOLVE IDENTICALLY,
 *     because they share scs_cdl_resolve() (scs_cdt.h) rather than each
 *     implementing the p. 2-29 lookup
 *   - THE CONNECTION LIFECYCLE: releasing a connection RETURNS its deposit to
 *     the port, because "each person is entitled only to the amount of money
 *     that he or she has on deposit" and a depositor who is gone has none
 *     (2-43). Driven both directly and through vms-17f's departure sweep, the
 *     production caller. Every fixture written before these opened its
 *     connections once and never released one, so none of them could see the
 *     port's account grow across connection churn -- it did, 8/16/24/32 over
 *     four cycles
 *
 * AND the OVMX_NO_DGRAM_ACCOUNTING kill switch, exercised by re-running the
 * whole discard scenario with the switch set and asserting the discard does
 * NOT happen and no count moves.
 *
 * NOT ASSERTED, because it is not true: that anything in SCSD.EXE routes a
 * datagram through this accounting. scs_dgram_cdl_deliver() has no production
 * caller (scs_dgram.h says so in full). A green run here proves the mechanism
 * is correct, not that OVMX exercises it. The one live daemon call site is
 * scs_dgram_report() in the exit summary, which prints zeros -- and THAT call
 * site is exercised for real by test_scsd_wire.c
 * (test_exit_summary_reports_datagram_discards); deleting it from scsd.c reds
 * that test while leaving this file green.
 *
 * NOT ASSERTED EITHER: any send-side debit. p. 2-42 states "SCA does not
 * provide a flow control mechanism for the datagram service"; the DFREEQ is a
 * receive-side account and no send path touches it. See the DIRECTION note in
 * scs_dgram.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scs_depart.h" /* vms-17f's sweep: the live caller of scs_cdl_release */
#include "scs_dgram.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        checks++;                                                                        \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                  \
            printf(__VA_ARGS__);                                                         \
            printf("\n");                                                                \
        }                                                                                \
    } while (0)

/* --- a bench: one port, one or two connections on it (p. 2-43) ------------ */

struct bench {
    struct scs_config cfg;
    struct scs_pdt    pdt;
    struct scs_cdl    cdl;
    struct scs_cdt   *a; /* first connection on the port */
    struct scs_cdt   *b; /* second connection on the SAME port */
};

static const uint8_t mac_a[6]   = {0x08, 0x00, 0x2b, 0xaa, 0xaa, 0xaa};
static const uint8_t mac_b[6]   = {0x08, 0x00, 0x2b, 0xbb, 0xbb, 0xbb};
static const uint8_t sysid_a[6] = {0xaa, 0x00, 0x04, 0x00, 0x0a, 0x04};
static const uint8_t sysid_b[6] = {0xaa, 0x00, 0x04, 0x00, 0x0b, 0x04};

/* What the SYSAP's datagram input routine saw. */
struct sysap {
    unsigned long calls;
    size_t        last_len;
    unsigned char last_first_byte;
};

static void sysap_dgram_input(struct scs_cdt *cdt, const void *buf, size_t len, void *ctx)
{
    (void)cdt;
    struct sysap *s = (struct sysap *)ctx;
    s->calls++;
    s->last_len = len;
    s->last_first_byte = (len > 0 && buf != NULL) ? ((const unsigned char *)buf)[0] : 0;
}

static struct scs_cdt *open_conn(struct bench *b, const uint8_t mac[6], const uint8_t sysid[6],
                                 struct sysap *sysap)
{
    struct scs_pb *pb = scs_pb_create(&b->cfg, &b->pdt, mac, SCS_PORT_TYPE_ETHERNET);
    if (pb == NULL || scs_pb_learn_system_addr(&b->cfg, pb, sysid) == NULL) {
        return NULL;
    }
    if (scs_pb_open(&b->cfg, pb) == SCS_OPEN_ERROR) {
        return NULL;
    }
    struct scs_cdt *cdt = scs_cdl_alloc(&b->cdl, "VMS$VAXcluster  ", "VMS$VAXcluster  ", pb);
    if (cdt != NULL && sysap != NULL) {
        scs_cdt_set_handlers(cdt, NULL, sysap_dgram_input, NULL, sysap);
    }
    return cdt;
}

/* One connection, with a datagram input routine installed. */
static int bench_init(struct bench *b, struct sysap *sysap)
{
    memset(b, 0, sizeof(*b));
    memset(sysap, 0, sizeof(*sysap));
    scs_config_init(&b->cfg);
    scs_pdt_init(&b->pdt, SCS_PORT_TYPE_ETHERNET, 4096);
    scs_cdl_init(&b->cdl);
    b->a = open_conn(b, mac_a, sysid_a, sysap);
    return b->a != NULL;
}

static const unsigned char DG[] = {0xde, 0xad, 0xbe, 0xef};

/* Deliver DG to `cdt`'s connection through the accounted CDL path. The CDT's
 * remote CONID is left 0, so the p. 2-35 source check is not engaged and 0 is
 * the correct src_conid to pass (see scs_cdt.h). */
static int deliver(struct bench *b, struct scs_cdt *cdt)
{
    return scs_dgram_cdl_deliver(&b->cdl, cdt->local_conid, 0, DG, sizeof(DG));
}

/* ========================================================================== */

/*
 * p. 2-42: "The number of datagram buffers requested by the SYSAP is stored in
 * the CDT that describes the connection. The buffers themselves are inserted
 * into the Datagram Free Queue (DFREEQ) associated with the port that supports
 * the connection."
 */
static void test_extend_fills_dfreeq(void)
{
    struct bench b;
    struct sysap s;
    CHECK(bench_init(&b, &s), "bench setup failed");

    CHECK(b.pdt.dfreeq_count == 0, "DFREEQ starts empty, got %u", b.pdt.dfreeq_count);
    CHECK(b.a->dgram_buffers == 0, "CDT count starts 0, got %u", b.a->dgram_buffers);

    CHECK(scs_dgram_extend(b.a, 4) == 0, "extend failed");

    CHECK(b.a->dgram_buffers == 4, "CDT count %u, want 4", b.a->dgram_buffers);
    CHECK(b.a->dgram_extended == 4, "CDT extension %u, want 4", b.a->dgram_extended);
    CHECK(b.pdt.dfreeq_count == 4, "DFREEQ %u, want 4", b.pdt.dfreeq_count);
}

/*
 * p. 2-42: the port "dequeues the buffer at the head of DFREEQ", then SCS
 * "delivers the datagram to the destination SYSAP ... by decrementing the count
 * of datagram buffers available to the connection, and then passing the buffer
 * to the datagram input routine whose address is in the CDT."
 *
 * THE DEBIT IS THE POINT OF THIS ITEM: a datagram that arrives on a connection
 * must move both counts, not sail through unaccounted.
 */
static void test_receive_debits_both_counts(void)
{
    struct bench b;
    struct sysap s;
    CHECK(bench_init(&b, &s), "bench setup failed");
    scs_dgram_extend(b.a, 3);

    CHECK(deliver(&b, b.a) == SCS_DGRAM_DELIVERED, "first datagram not delivered");

    CHECK(b.pdt.dfreeq_count == 2, "DFREEQ %u after 1 receipt, want 2", b.pdt.dfreeq_count);
    CHECK(b.a->dgram_buffers == 2, "CDT count %u after 1 receipt, want 2", b.a->dgram_buffers);
    CHECK(scs_dgram_delivered(b.a) == 1, "delivered counter %lu, want 1",
          scs_dgram_delivered(b.a));

    /* It really reached the SYSAP's datagram input routine, with the bytes. */
    CHECK(s.calls == 1, "input routine calls %lu, want 1", s.calls);
    CHECK(s.last_len == sizeof(DG), "input len %zu, want %zu", s.last_len, sizeof(DG));
    CHECK(s.last_first_byte == 0xde, "input first byte 0x%02x, want 0xde", s.last_first_byte);

    /* Three receipts exhaust the extension exactly. */
    CHECK(deliver(&b, b.a) == SCS_DGRAM_DELIVERED, "second datagram not delivered");
    CHECK(deliver(&b, b.a) == SCS_DGRAM_DELIVERED, "third datagram not delivered");
    CHECK(b.a->dgram_buffers == 0, "CDT count %u after 3, want 0", b.a->dgram_buffers);
    CHECK(b.pdt.dfreeq_count == 0, "DFREEQ %u after 3, want 0", b.pdt.dfreeq_count);
    CHECK(s.calls == 3, "input routine calls %lu, want 3", s.calls);
    CHECK(scs_dgram_discards_no_quota(b.a) == 0, "no discard yet, got %lu",
          scs_dgram_discards_no_quota(b.a));
}

/*
 * p. 2-42: "If the CDT's count of datagram buffers available to this connection
 * is not greater than 0, SCS discards the datagram by inserting the buffer back
 * into the DFREEQ."
 *
 * The distinguishing detail, and the one a mutant that just drops the datagram
 * would miss: the DFREEQ does NOT lose a buffer, because the one the port took
 * goes straight back.
 */
static void test_no_quota_discards_and_returns_the_buffer(void)
{
    struct bench b;
    struct sysap s;
    CHECK(bench_init(&b, &s), "bench setup failed");

    /* Connection A has ONE buffer; the port DFREEQ has three more from B. */
    scs_dgram_extend(b.a, 1);
    struct sysap s_b;
    memset(&s_b, 0, sizeof(s_b));
    b.b = open_conn(&b, mac_b, sysid_b, &s_b);
    CHECK(b.b != NULL, "second connection failed to open");
    scs_dgram_extend(b.b, 3);
    CHECK(b.pdt.dfreeq_count == 4, "shared DFREEQ %u, want 4", b.pdt.dfreeq_count);

    /* A's single buffer is consumed. */
    CHECK(deliver(&b, b.a) == SCS_DGRAM_DELIVERED, "A's first datagram not delivered");
    CHECK(b.a->dgram_buffers == 0, "A count %u, want 0", b.a->dgram_buffers);
    CHECK(b.pdt.dfreeq_count == 3, "DFREEQ %u, want 3", b.pdt.dfreeq_count);

    /* The next one for A is discarded even though the DFREEQ is not empty --
     * "This prevents datagrams received on one connection from consuming
     * datagram buffers contributed to the DFREEQ for other connections." */
    unsigned long s_calls_before = s.calls;
    CHECK(deliver(&b, b.a) == SCS_DGRAM_DISCARD_NO_QUOTA, "A's second datagram not discarded");
    CHECK(s.calls == s_calls_before, "discarded datagram still reached the SYSAP");
    CHECK(scs_dgram_discards_no_quota(b.a) == 1, "A discard counter %lu, want 1",
          scs_dgram_discards_no_quota(b.a));

    /* The buffer went BACK. B's three are untouched, so B still receives. */
    CHECK(b.pdt.dfreeq_count == 3, "DFREEQ %u after discard, want 3 (buffer returned)",
          b.pdt.dfreeq_count);
    CHECK(b.b->dgram_buffers == 3, "B count %u, want 3", b.b->dgram_buffers);
    CHECK(deliver(&b, b.b) == SCS_DGRAM_DELIVERED, "B starved by A's discard");
    CHECK(s_b.calls == 1, "B input routine calls %lu, want 1", s_b.calls);

    /* The counter is PER CONNECTION: A's discard is not charged to B. */
    CHECK(scs_dgram_discards_no_quota(b.b) == 0, "B charged %lu of A's discards",
          scs_dgram_discards_no_quota(b.b));
}

/*
 * p. 2-42: "The possibility exists that the DFREEQ itself is empty when the
 * port receives a datagram. If this is the case, the port merely discards the
 * datagram." A PORT-level drop, counted on the port.
 */
static void test_empty_dfreeq_discards_at_the_port(void)
{
    struct bench b;
    struct sysap s;
    CHECK(bench_init(&b, &s), "bench setup failed");

    /* The connection has a quota on paper but the port's queue is empty --
     * only reachable by removing the buffers, which is exactly the p. 2-43
     * deallocate request. Set the CDT count back up by hand afterwards so the
     * two discard classes are genuinely distinguished: if the port did not
     * check first, this would be delivered. */
    scs_dgram_extend(b.a, 2);
    CHECK(scs_dgram_remove_buffers(b.a, 2) == 2, "remove_buffers did not remove 2");
    CHECK(b.pdt.dfreeq_count == 0, "DFREEQ %u, want 0", b.pdt.dfreeq_count);
    b.a->dgram_buffers = 2; /* deliberately inconsistent: quota yes, queue no */

    CHECK(deliver(&b, b.a) == SCS_DGRAM_DISCARD_DFREEQ_EMPTY,
          "empty DFREEQ did not discard at the port");
    CHECK(s.calls == 0, "datagram reached the SYSAP with an empty DFREEQ");
    CHECK(scs_dgram_dfreeq_empty_discards(&b.pdt) == 1, "port empty-discards %lu, want 1",
          scs_dgram_dfreeq_empty_discards(&b.pdt));

    /* The connection's own count did NOT move: nothing was delivered to it,
     * and the drop is not attributable to it. */
    CHECK(b.a->dgram_buffers == 2, "CDT count %u moved on a port discard", b.a->dgram_buffers);
    CHECK(scs_dgram_discards_no_quota(b.a) == 0, "port discard charged to the connection");
}

/*
 * p. 2-42: "When a SYSAP has finished processing a received datagram, it can
 * request that SCS return the buffer to the DFREEQ and increment the CDT's
 * count of available datagram buffers; or it can simply request that SCS
 * deallocate the buffer."
 */
static void test_release_and_deallocate(void)
{
    struct bench b;
    struct sysap s;
    CHECK(bench_init(&b, &s), "bench setup failed");
    scs_dgram_extend(b.a, 2);

    CHECK(deliver(&b, b.a) == SCS_DGRAM_DELIVERED, "datagram not delivered");
    CHECK(b.a->dgram_buffers == 1 && b.pdt.dfreeq_count == 1, "post-receive counts wrong");

    /* Return: BOTH rise. */
    CHECK(scs_dgram_release_buffer(b.a) == 0, "release failed");
    CHECK(b.a->dgram_buffers == 2, "CDT count %u after release, want 2", b.a->dgram_buffers);
    CHECK(b.pdt.dfreeq_count == 2, "DFREEQ %u after release, want 2", b.pdt.dfreeq_count);

    /* Deallocate: NEITHER rises -- the buffer is gone for good. */
    CHECK(deliver(&b, b.a) == SCS_DGRAM_DELIVERED, "second datagram not delivered");
    CHECK(b.a->dgram_buffers == 1 && b.pdt.dfreeq_count == 1, "post-receive counts wrong");
    CHECK(scs_dgram_deallocate_buffer(b.a) == 0, "deallocate failed");
    CHECK(b.a->dgram_buffers == 1, "CDT count %u after deallocate, want 1 (unchanged)",
          b.a->dgram_buffers);
    CHECK(b.pdt.dfreeq_count == 1, "DFREEQ %u after deallocate, want 1 (unchanged)",
          b.pdt.dfreeq_count);
    CHECK(b.a->dgram_extended == 1, "extension %u after deallocate, want 1",
          b.a->dgram_extended);
}

/*
 * p. 2-43: "If necessary, a SYSAP can request SCS to allocate additional
 * buffers to the DFREEQ. A SYSAP can also request SCS to deallocate buffers for
 * a connection from the DFREEQ. In both cases, SCS will appropriately alter the
 * CDT to reflect the change." -- and the bank rule: "each person is entitled
 * only to the amount of money that he or she has on deposit in the bank."
 */
static void test_add_and_remove_are_clamped_to_the_deposit(void)
{
    struct bench b;
    struct sysap s;
    CHECK(bench_init(&b, &s), "bench setup failed");
    scs_dgram_extend(b.a, 2);

    struct sysap s_b;
    memset(&s_b, 0, sizeof(s_b));
    b.b = open_conn(&b, mac_b, sysid_b, &s_b);
    CHECK(b.b != NULL, "second connection failed to open");
    scs_dgram_extend(b.b, 5);
    CHECK(b.pdt.dfreeq_count == 7, "shared DFREEQ %u, want 7", b.pdt.dfreeq_count);

    CHECK(scs_dgram_add_buffers(b.a, 3) == 3, "add_buffers did not add 3");
    CHECK(b.a->dgram_buffers == 5, "A count %u, want 5", b.a->dgram_buffers);
    CHECK(b.pdt.dfreeq_count == 10, "DFREEQ %u, want 10", b.pdt.dfreeq_count);

    /* A asks to deallocate 99. It has 5. It gets 5 -- and B's 5 stay in the
     * bank: the DFREEQ must not fall to 0. */
    CHECK(scs_dgram_remove_buffers(b.a, 99) == 5, "remove_buffers overshot the deposit");
    CHECK(b.a->dgram_buffers == 0, "A count %u, want 0", b.a->dgram_buffers);
    CHECK(b.pdt.dfreeq_count == 5, "DFREEQ %u, want 5 (B's deposit survives)",
          b.pdt.dfreeq_count);
    CHECK(b.b->dgram_buffers == 5, "B count %u, want 5", b.b->dgram_buffers);
}

/*
 * p. 2-43: "SCA associates a separate DFREEQ with each port ... The datagram
 * buffers allocated for a connection are inserted into the DFREEQ for the port
 * that supports that connection." Two ports, two banks.
 */
static void test_dfreeq_is_per_port(void)
{
    struct scs_config cfg;
    struct scs_pdt    pdt1, pdt2;
    struct scs_cdl    cdl;
    memset(&cfg, 0, sizeof(cfg));
    memset(&pdt1, 0, sizeof(pdt1));
    memset(&pdt2, 0, sizeof(pdt2));
    scs_config_init(&cfg);
    scs_pdt_init(&pdt1, SCS_PORT_TYPE_ETHERNET, 4096);
    scs_pdt_init(&pdt2, SCS_PORT_TYPE_ETHERNET, 4096);
    scs_cdl_init(&cdl);

    struct scs_pb *pb1 = scs_pb_create(&cfg, &pdt1, mac_a, SCS_PORT_TYPE_ETHERNET);
    struct scs_pb *pb2 = scs_pb_create(&cfg, &pdt2, mac_b, SCS_PORT_TYPE_ETHERNET);
    CHECK(pb1 != NULL && pb2 != NULL, "path blocks failed");
    CHECK(scs_pb_learn_system_addr(&cfg, pb1, sysid_a) != NULL, "learn 1 failed");
    CHECK(scs_pb_learn_system_addr(&cfg, pb2, sysid_b) != NULL, "learn 2 failed");
    CHECK(scs_pb_open(&cfg, pb1) != SCS_OPEN_ERROR, "open 1 failed");
    CHECK(scs_pb_open(&cfg, pb2) != SCS_OPEN_ERROR, "open 2 failed");

    struct scs_cdt *c1 = scs_cdl_alloc(&cdl, "SCS$DIRECTORY   ", "SCS$DIRECTORY   ", pb1);
    struct scs_cdt *c2 = scs_cdl_alloc(&cdl, "SCS$DIRECTORY   ", "SCS$DIRECTORY   ", pb2);
    CHECK(c1 != NULL && c2 != NULL, "CDT alloc failed");

    scs_dgram_extend(c1, 6);
    CHECK(pdt1.dfreeq_count == 6, "port 1 DFREEQ %u, want 6", pdt1.dfreeq_count);
    CHECK(pdt2.dfreeq_count == 0, "port 2 DFREEQ %u, want 0 (separate bank)",
          pdt2.dfreeq_count);

    scs_dgram_extend(c2, 2);
    CHECK(pdt1.dfreeq_count == 6, "port 1 DFREEQ %u after port 2 extend, want 6",
          pdt1.dfreeq_count);
    CHECK(pdt2.dfreeq_count == 2, "port 2 DFREEQ %u, want 2", pdt2.dfreeq_count);

    /* Exhausting port 2 through the p. 2-43 deallocate request -- both its
     * connection count and its DFREEQ go to 0, and no hand poke is needed to
     * get there. Port 1's bank is untouched by it. */
    CHECK(scs_dgram_remove_buffers(c2, 2) == 2, "removing port 2's deposit did not remove 2");
    CHECK(c2->dgram_buffers == 0, "port 2 connection count %u after deallocating its whole"
          " deposit, want 0", c2->dgram_buffers);
    CHECK(pdt2.dfreeq_count == 0, "port 2 DFREEQ %u after deallocating its whole deposit,"
          " want 0", pdt2.dfreeq_count);
    CHECK(pdt1.dfreeq_count == 6, "port 1 DFREEQ %u after port 2 deallocated, want 6",
          pdt1.dfreeq_count);
    CHECK(scs_dgram_cdl_deliver(&cdl, c2->local_conid, 0, DG, sizeof(DG))
              == SCS_DGRAM_DISCARD_DFREEQ_EMPTY, "port 2 did not discard");
    CHECK(scs_dgram_dfreeq_empty_discards(&pdt2) == 1, "port 2 empty-discards %lu, want 1",
          scs_dgram_dfreeq_empty_discards(&pdt2));
    CHECK(scs_dgram_dfreeq_empty_discards(&pdt1) == 0, "port 1 charged port 2's discard");
}

/*
 * A datagram addressed to no open connection resolves to nothing, and -- per
 * the ORDERING CAVEAT in scs_dgram.h -- OVMX moves no count for it, because it
 * cannot identify the receiving port without the CDT. Asserted so the
 * divergence from p. 2-42's port-first order is PINNED rather than assumed.
 */
static void test_unknown_conid_moves_nothing(void)
{
    struct bench b;
    struct sysap s;
    CHECK(bench_init(&b, &s), "bench setup failed");
    scs_dgram_extend(b.a, 3);

    CHECK(scs_dgram_cdl_deliver(&b.cdl, 0x4F58BEEFu, 0, DG, sizeof(DG)) == SCS_DGRAM_NO_CDT,
          "unknown CONID did not report NO_CDT");
    CHECK(b.pdt.dfreeq_count == 3, "DFREEQ %u moved for an unknown CONID", b.pdt.dfreeq_count);
    CHECK(scs_dgram_dfreeq_empty_discards(&b.pdt) == 0, "unknown CONID counted a discard");

    /* And the p. 2-35 source-CONID refusal: a datagram whose source is not this
     * connection's remote CONID is not this connection's. */
    scs_cdt_set_remote_conid(b.a, 0x11112222u);
    CHECK(scs_dgram_cdl_deliver(&b.cdl, b.a->local_conid, 0x33334444u, DG, sizeof(DG))
              == SCS_DGRAM_NO_CDT, "source-CONID mismatch was accepted");
    CHECK(b.pdt.dfreeq_count == 3, "DFREEQ moved on a source mismatch");
    CHECK(scs_dgram_delivered(b.a) == 0, "source mismatch counted as delivered");
}

/*
 * A connection with no datagram input routine (p. 2-29) has nowhere to deliver.
 * The buffer must go back rather than vanish, and the connection's quota must
 * not be spent on a datagram nothing consumed. THIS IS THE STATE EVERY OVMX
 * CONNECTION IS IN TODAY -- scsd.c installs no SYSAP.
 */
static void test_no_input_routine_returns_the_buffer(void)
{
    struct bench b;
    struct sysap s;
    CHECK(bench_init(&b, &s), "bench setup failed");
    scs_dgram_extend(b.a, 2);
    scs_cdt_set_handlers(b.a, NULL, NULL, NULL, NULL);

    CHECK(deliver(&b, b.a) == SCS_DGRAM_NO_ROUTINE, "missing input routine not reported");
    CHECK(b.pdt.dfreeq_count == 2, "DFREEQ %u, want 2 (buffer returned)", b.pdt.dfreeq_count);
    CHECK(b.a->dgram_buffers == 2, "CDT count %u, want 2 (nothing consumed)",
          b.a->dgram_buffers);
    CHECK(scs_dgram_delivered(b.a) == 0, "counted as delivered with no input routine");
}

/*
 * INV-6: the discards are silent on the wire and MUST be visible in the run
 * log. Asserts the report actually prints the numbers -- a counter nothing
 * prints is the same invisibility the item exists to remove.
 */
static void test_report_prints_the_counters(void)
{
    struct bench b;
    struct sysap s;
    CHECK(bench_init(&b, &s), "bench setup failed");
    scs_dgram_extend(b.a, 1);

    /* A second connection deposits into the SAME port DFREEQ, so that A's
     * second datagram hits the NO-QUOTA discard and not the empty-DFREEQ one --
     * the two classes must be produced separately to be reported separately. */
    struct sysap s_b;
    memset(&s_b, 0, sizeof(s_b));
    b.b = open_conn(&b, mac_b, sysid_b, &s_b);
    CHECK(b.b != NULL, "second connection failed to open");
    scs_dgram_extend(b.b, 2);

    deliver(&b, b.a);                       /* delivered=1, buffers 1 -> 0 */
    CHECK(deliver(&b, b.a) == SCS_DGRAM_DISCARD_NO_QUOTA, "expected a no-quota discard");
    b.pdt.dfreeq_count = 0;
    CHECK(deliver(&b, b.a) == SCS_DGRAM_DISCARD_DFREEQ_EMPTY, "expected a port discard");

    char  buf[4096];
    memset(buf, 0, sizeof(buf));
    FILE *f = fmemopen(buf, sizeof(buf) - 1, "w");
    CHECK(f != NULL, "fmemopen failed");
    unsigned lines = scs_dgram_report(&b.cdl, f);
    fclose(f);

    CHECK(lines == 2, "report printed %u connection lines, want 2", lines);
    CHECK(strstr(buf, "DFREEQ:") != NULL, "no DFREEQ line in:\n%s", buf);
    CHECK(strstr(buf, "empty-discards=1") != NULL, "empty-discard count missing from:\n%s", buf);
    CHECK(strstr(buf, "DGRAM:") != NULL, "no DGRAM line in:\n%s", buf);
    CHECK(strstr(buf, "delivered=1") != NULL, "delivered count missing from:\n%s", buf);
    CHECK(strstr(buf, "discarded-no-quota=1") != NULL, "discard count missing from:\n%s", buf);
    CHECK(strstr(buf, "buffers=0/1") != NULL, "buffer count missing from:\n%s", buf);
}

/* ==========================================================================
 * THE ORDER OF THE DEBIT AND THE CALLBACK (p. 2-42).
 *
 * "SCS delivers the datagram to the destination SYSAP. This is done by
 * DECREMENTING the count of datagram buffers available to the connection, AND
 * THEN passing the buffer to the datagram input routine whose address is in
 * the CDT." (p. 2-42, emphasis on the sequencing the sentence states.)
 *
 * WHY A SEPARATE FAKE IS NEEDED. `struct sysap` above is INERT: it records the
 * call and returns. Nothing it does can distinguish "debit then call" from
 * "call then debit", because both orders leave the same final counts -- swap
 * the two statements in scs_dgram_deliver() and every assertion above still
 * passes. The order is only observable to a SYSAP that RE-ENTERS the API from
 * inside its own input routine, which is precisely the case the ordering
 * comment in scs_dgram.c is about. This fake does that.
 *
 * Re-entrancy is not hypothetical: SCS calls the input routine synchronously,
 * and a SYSAP is documented (p. 2-42) as calling back into SCS from there to
 * return or deallocate the buffer.
 * ========================================================================== */

enum reentry_mode {
    RE_OBSERVE,   /* just record what the account looked like on entry */
    RE_RELEASE,   /* p. 2-42: return the buffer from inside the input routine */
    RE_REDELIVER  /* a second datagram arrives while the first is being processed */
};

struct reentrant_sysap {
    enum reentry_mode mode;
    unsigned          depth;      /* guards the RE_REDELIVER recursion */
    unsigned long     calls;
    unsigned          seen_buffers[4]; /* cdt->dgram_buffers AS SEEN AT ENTRY */
    int               nested_result;
};

static void sysap_reentrant_input(struct scs_cdt *cdt, const void *buf, size_t len, void *ctx)
{
    struct reentrant_sysap *s = (struct reentrant_sysap *)ctx;
    if (s->calls < 4) {
        s->seen_buffers[s->calls] = cdt->dgram_buffers;
    }
    s->calls++;

    if (s->depth > 0) {
        return; /* one level of re-entry is enough to expose the order */
    }
    s->depth++;
    if (s->mode == RE_RELEASE) {
        scs_dgram_release_buffer(cdt);
    } else if (s->mode == RE_REDELIVER) {
        /* The SCS half only: this models a second datagram whose DFREEQ buffer
         * the port has already dequeued, arriving while the first is still in
         * the input routine. scs_dgram_deliver() is exactly the function whose
         * internal order is under test. */
        s->nested_result = scs_dgram_deliver(cdt, buf, len);
    }
    s->depth--;
}

static struct scs_cdt *reentrant_bench(struct bench *b, struct reentrant_sysap *rs,
                                       enum reentry_mode mode, unsigned buffers)
{
    struct sysap ignored;
    memset(b, 0, sizeof(*b));
    memset(rs, 0, sizeof(*rs));
    rs->mode = mode;
    rs->nested_result = 0x7FFFFFFF; /* not any enum value: proves it was assigned */
    memset(&ignored, 0, sizeof(ignored));
    scs_config_init(&b->cfg);
    scs_pdt_init(&b->pdt, SCS_PORT_TYPE_ETHERNET, 4096);
    scs_cdl_init(&b->cdl);
    b->a = open_conn(b, mac_a, sysid_a, &ignored);
    if (b->a == NULL) {
        return NULL;
    }
    scs_cdt_set_handlers(b->a, NULL, sysap_reentrant_input, NULL, rs);
    scs_dgram_extend(b->a, buffers);
    return b->a;
}

/*
 * (1) THE SYSAP SEES THE DEBIT ALREADY APPLIED. p. 2-42 decrements BEFORE
 * passing the buffer to the input routine, so a SYSAP that reads its own
 * available-buffer count from inside that routine must see the post-debit
 * value. Swap the two statements and this reds.
 */
static void test_input_routine_sees_the_debit_already_applied(void)
{
    struct bench b;
    struct reentrant_sysap rs;
    CHECK(reentrant_bench(&b, &rs, RE_OBSERVE, 3) != NULL, "bench setup failed");

    CHECK(deliver(&b, b.a) == SCS_DGRAM_DELIVERED, "datagram not delivered");
    CHECK(rs.calls == 1, "input routine calls %lu, want 1", rs.calls);
    CHECK(rs.seen_buffers[0] == 2,
          "the input routine saw %u available buffers, want 2 -- the p. 2-42 debit"
          " is applied AFTER the callback, so the SYSAP sees a buffer that is"
          " already spent", rs.seen_buffers[0]);
    CHECK(b.a->dgram_buffers == 2, "CDT count %u after the callback, want 2",
          b.a->dgram_buffers);
}

/*
 * (2) THE ACCOUNT DOES NOT GAIN A BUFFER FROM NOWHERE. This is the consequence
 * the ordering comment in scs_dgram.c names, made into an outcome rather than
 * an assertion about statement order.
 *
 * The connection has EXACTLY ONE buffer. A second datagram is presented while
 * the first is inside the input routine. With the p. 2-42 order the debit is
 * already applied, the second sees a count of 0, and it is DISCARDED for want
 * of quota with its buffer returned to the DFREEQ -- one buffer, one delivery.
 * With the debit moved after the callback the second sees the count still at 1
 * and is DELIVERED: two datagrams delivered against one buffer, and the outer
 * decrement then underflows a count that is already 0.
 */
static void test_reentrant_delivery_cannot_spend_the_same_buffer_twice(void)
{
    struct bench b;
    struct reentrant_sysap rs;
    CHECK(reentrant_bench(&b, &rs, RE_REDELIVER, 1) != NULL, "bench setup failed");

    CHECK(deliver(&b, b.a) == SCS_DGRAM_DELIVERED, "the first datagram was not delivered");

    CHECK(rs.nested_result == SCS_DGRAM_DISCARD_NO_QUOTA,
          "the re-entrant second datagram returned %d, want SCS_DGRAM_DISCARD_NO_QUOTA"
          " (%d) -- one buffer delivered two datagrams",
          rs.nested_result, (int)SCS_DGRAM_DISCARD_NO_QUOTA);
    CHECK(rs.calls == 1,
          "the input routine ran %lu times on a connection with ONE buffer, want 1",
          rs.calls);
    CHECK(scs_dgram_delivered(b.a) == 1,
          "delivered counter %lu against a single buffer, want 1",
          scs_dgram_delivered(b.a));
    CHECK(scs_dgram_discards_no_quota(b.a) == 1,
          "no-quota discard counter %lu, want 1 -- the second datagram was not refused",
          scs_dgram_discards_no_quota(b.a));

    /* And the counts are still sane: the returned buffer is back in the DFREEQ
     * and the connection's count did not underflow. */
    CHECK(b.a->dgram_buffers == 0, "CDT count %u, want 0", b.a->dgram_buffers);
    CHECK(b.pdt.dfreeq_count == 1, "DFREEQ %u, want 1 (the discarded buffer returned)",
          b.pdt.dfreeq_count);
}

/*
 * (3) THE p. 2-42 RELEASE, ISSUED FROM INSIDE THE INPUT ROUTINE -- the exact
 * case the ordering comment in scs_dgram.c describes. The SYSAP must see the
 * debit applied when it releases, or its release is crediting a buffer that
 * was never taken.
 */
static void test_release_from_inside_the_input_routine(void)
{
    struct bench b;
    struct reentrant_sysap rs;
    CHECK(reentrant_bench(&b, &rs, RE_RELEASE, 2) != NULL, "bench setup failed");

    CHECK(deliver(&b, b.a) == SCS_DGRAM_DELIVERED, "datagram not delivered");
    CHECK(rs.calls == 1, "input routine calls %lu, want 1", rs.calls);
    CHECK(rs.seen_buffers[0] == 1,
          "the SYSAP saw %u buffers when it released, want 1 -- it released against"
          " an account that had not yet been debited", rs.seen_buffers[0]);

    /* Net effect of take-debit-release: back where it started. */
    CHECK(b.a->dgram_buffers == 2, "CDT count %u after release, want 2", b.a->dgram_buffers);
    CHECK(b.pdt.dfreeq_count == 2, "DFREEQ %u after release, want 2", b.pdt.dfreeq_count);
}

/*
 * The two CDL receive paths RESOLVE IDENTICALLY, because they resolve through
 * the same function (scs_cdl_resolve, scs_cdt.h). Pinned so that the accounted
 * path cannot drift into accepting a packet the unaccounted one refuses, or
 * vice versa -- the reason the resolution was factored out rather than copied.
 */
static void test_both_receive_paths_resolve_identically(void)
{
    struct bench b;
    struct sysap s;
    CHECK(bench_init(&b, &s), "bench setup failed");
    scs_dgram_extend(b.a, 8);
    scs_cdt_set_remote_conid(b.a, 0x11112222u);

    const uint32_t good = b.a->local_conid;
    struct { uint32_t dest, src; int accepted; } cases[] = {
        { good,        0x11112222u, 1 }, /* matching source CONID */
        { good,        0u,          1 }, /* frame class carries no source CONID */
        { good,        0x33334444u, 0 }, /* p. 2-35 source mismatch */
        { 0x4F58BEEFu, 0x11112222u, 0 }, /* destination resolves to nothing */
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int unacc = scs_cdl_deliver_datagram(&b.cdl, cases[i].dest, cases[i].src,
                                             DG, sizeof(DG));
        int acc = scs_dgram_cdl_deliver(&b.cdl, cases[i].dest, cases[i].src,
                                        DG, sizeof(DG));
        int unacc_ok = (unacc == SCS_DELIVER_OK);
        int acc_ok = (acc != SCS_DGRAM_NO_CDT);
        CHECK(unacc_ok == cases[i].accepted,
              "case %u: unaccounted path returned %d, want %s", i, unacc,
              cases[i].accepted ? "accepted" : "refused");
        CHECK(acc_ok == cases[i].accepted,
              "case %u: accounted path returned %d, want %s", i, acc,
              cases[i].accepted ? "accepted" : "refused");
    }
}

/* ==========================================================================
 * CONNECTION LIFECYCLE -- the deposit has to come BACK (vms-b1d, second pass).
 *
 * Every fixture above this point opens its connections once and never releases
 * one, so none of them could see what happens to the port's account when a
 * connection goes away. It went nowhere: the deposit stayed on the port for
 * ever. Measured on the pre-fix tree, four connect/extend(8)/release cycles on
 * ONE port gave DFREEQ = 8 -> 16 -> 24 -> 32.
 *
 * p. 2-43's bank analogy is the rule being enforced -- "each person is entitled
 * only to the amount of money that he or she has on deposit in the bank" -- and
 * a depositor who no longer exists has no deposit. Same defect class as
 * vms-61b's MFREEQ leak, in the same function (scs_cdl_release), and LIVE
 * rather than latent since vms-17f: the departure sweep releases every CDT on a
 * departing peer's circuit.
 * ========================================================================== */

/*
 * THE PROBE. Four full connect / extend / release cycles on a single port. The
 * assertion that matters is inside the loop: after each release the port is
 * back where it started, so nothing accumulates over the cycles.
 */
static void test_release_returns_the_dfreeq_deposit_to_the_port(void)
{
    struct bench b;
    struct sysap s;
    memset(&b, 0, sizeof(b));
    memset(&s, 0, sizeof(s));
    scs_config_init(&b.cfg);
    scs_pdt_init(&b.pdt, SCS_PORT_TYPE_ETHERNET, 4096);
    scs_cdl_init(&b.cdl);

    for (unsigned cycle = 1; cycle <= 4; cycle++) {
        struct scs_cdt *c = open_conn(&b, mac_a, sysid_a, &s);
        CHECK(c != NULL, "cycle %u: connection did not open", cycle);
        if (c == NULL) {
            return;
        }
        CHECK(scs_dgram_extend(c, 8) == 0, "cycle %u: extend failed", cycle);
        CHECK(b.pdt.dfreeq_count == 8, "cycle %u: DFREEQ %u with one open"
              " connection holding 8, want 8 -- a deposit from an earlier cycle"
              " is still on the port's books", cycle, b.pdt.dfreeq_count);

        struct scs_pb *pb = c->pb;
        scs_cdl_release(&b.cdl, c);
        CHECK(b.pdt.dfreeq_count == 0, "cycle %u: DFREEQ %u after releasing the"
              " only connection on the port, want 0 -- the deposit was not"
              " returned", cycle, b.pdt.dfreeq_count);
        /* The Path Block only closes because the CDT was released first; that
         * ordering is vms-17f's scs_pb_close contract, asserted here so this
         * fixture cannot quietly stop being a lifecycle. */
        CHECK(scs_pb_close(&b.cfg, pb) == SCS_PB_CLOSE_OK,
              "cycle %u: path block would not close after the release", cycle);
    }
}

/*
 * WHICH figure is returned. A connection's `dgram_extended` is what it ever
 * contributed; its `dgram_buffers` is what is STILL SITTING IN the queue.
 * Buffers already dequeued by the port and handed to the SYSAP left the queue
 * when they were taken, so returning `dgram_extended` would credit the port
 * twice -- and with a second connection on the same port, would hand it that
 * connection's buffers.
 *
 * A is given 5 and consumes 3 (the SYSAP holds them: it neither released nor
 * deallocated). B holds 4 on the same port. Depth is 2 + 4 = 6. Releasing A
 * must leave B's 4. Returning `dgram_extended` instead would leave 1.
 */
static void test_release_returns_only_what_is_still_on_deposit(void)
{
    struct bench b;
    struct sysap s;
    CHECK(bench_init(&b, &s), "bench setup failed");

    struct sysap s_b;
    memset(&s_b, 0, sizeof(s_b));
    b.b = open_conn(&b, mac_b, sysid_b, &s_b);
    CHECK(b.b != NULL, "second connection failed to open");
    if (b.b == NULL) {
        return;
    }

    scs_dgram_extend(b.a, 5);
    scs_dgram_extend(b.b, 4);
    CHECK(b.pdt.dfreeq_count == 9, "shared DFREEQ %u, want 9", b.pdt.dfreeq_count);

    for (unsigned i = 0; i < 3; i++) {
        CHECK(deliver(&b, b.a) == SCS_DGRAM_DELIVERED, "A's datagram %u not delivered", i);
    }
    CHECK(s.calls == 3, "A's input routine calls %lu, want 3", s.calls);
    CHECK(b.a->dgram_buffers == 2, "A still on deposit %u, want 2", b.a->dgram_buffers);
    CHECK(b.a->dgram_extended == 5, "A ever contributed %u, want 5", b.a->dgram_extended);
    CHECK(b.pdt.dfreeq_count == 6, "DFREEQ %u with 3 buffers in the SYSAP's hands,"
          " want 6", b.pdt.dfreeq_count);

    scs_cdl_release(&b.cdl, b.a);
    CHECK(b.pdt.dfreeq_count == 4, "DFREEQ %u after releasing A, want 4 -- B's"
          " deposit, and only B's", b.pdt.dfreeq_count);
    CHECK(b.b->dgram_buffers == 4, "B's own count %u was disturbed by A's release,"
          " want 4", b.b->dgram_buffers);

    /* B still receives, out of its own untouched deposit. */
    CHECK(deliver(&b, b.b) == SCS_DGRAM_DELIVERED, "B starved by A's release");
    CHECK(s_b.calls == 1, "B input routine calls %lu, want 1", s_b.calls);
    CHECK(b.pdt.dfreeq_count == 3, "DFREEQ %u after B received, want 3",
          b.pdt.dfreeq_count);
}

/*
 * THE LIVE PATH. vms-17f's departure sweep is what makes this leak reachable in
 * production, so it is driven here rather than only through scs_cdl_release:
 * scs_pb_depart() notifies the SYSAPs, flushes the Credit Waits, releases every
 * CDT on the circuit and closes the Path Block. Its stats block reports the
 * DFREEQ reclaim the same way it reports the MFREEQ one -- measured as the
 * port's depth before minus after, not as what the CDT claimed to owe.
 */
static void test_departure_sweep_returns_the_dfreeq_deposit(void)
{
    struct bench b;
    struct sysap s;
    CHECK(bench_init(&b, &s), "bench setup failed");

    struct scs_pb *pb = b.a->pb;
    CHECK(pb != NULL, "the bench connection has no path block");
    if (pb == NULL) {
        return;
    }
    scs_dgram_extend(b.a, 6);
    CHECK(b.pdt.dfreeq_count == 6, "DFREEQ %u before the departure, want 6",
          b.pdt.dfreeq_count);

    struct scs_depart_stats st;
    enum scs_pb_close_result r = scs_pb_depart(&b.cdl, &b.cfg, pb, &st);
    CHECK(r == SCS_PB_CLOSE_OK, "departure did not close the path block (result %d)",
          (int)r);
    CHECK(st.connections_lost == 1, "departure lost %u connection(s), want 1",
          st.connections_lost);
    CHECK(st.dfreeq_reclaimed == 6, "departure reclaimed %u DFREEQ buffer(s), want 6",
          st.dfreeq_reclaimed);
    CHECK(b.pdt.dfreeq_count == 0, "port DFREEQ %u after the peer departed, want 0",
          b.pdt.dfreeq_count);
}

/*
 * p. 2-42 makes the CONNECT/ACCEPT buffer request a SET, not an add: "the
 * number of datagram buffers requested by the SYSAP is stored in the CDT". So
 * re-extending a connection that is STILL OPEN replaces its deposit, and the
 * old one must leave the port's books first -- which is the ONLY thing the
 * subtraction at the top of scs_dgram_extend() does, now that scs_cdl_release
 * returns the deposit itself. Driven with a second connection holding 2 on the
 * same port so a stacked deposit is arithmetically visible (3+5=8 would show as
 * a depth of 10, not 7).
 */
static void test_re_extension_replaces_the_deposit_it_does_not_stack(void)
{
    struct bench b;
    struct sysap s;
    CHECK(bench_init(&b, &s), "bench setup failed");

    struct sysap s_b;
    memset(&s_b, 0, sizeof(s_b));
    b.b = open_conn(&b, mac_b, sysid_b, &s_b);
    CHECK(b.b != NULL, "second connection failed to open");
    if (b.b == NULL) {
        return;
    }
    scs_dgram_extend(b.b, 2);

    scs_dgram_extend(b.a, 3);
    CHECK(b.pdt.dfreeq_count == 5, "DFREEQ %u after A's first extension, want 5",
          b.pdt.dfreeq_count);

    scs_dgram_extend(b.a, 5);
    CHECK(b.a->dgram_buffers == 5, "A count %u after re-extension, want 5 (SET,"
          " not added)", b.a->dgram_buffers);
    CHECK(b.a->dgram_extended == 5, "A extension %u after re-extension, want 5",
          b.a->dgram_extended);
    CHECK(b.pdt.dfreeq_count == 7, "DFREEQ %u after A re-extended from 3 to 5,"
          " want 7 -- the old deposit stacked instead of being replaced",
          b.pdt.dfreeq_count);
    CHECK(b.b->dgram_buffers == 2, "B's deposit %u disturbed by A's re-extension",
          b.b->dgram_buffers);
}

/*
 * A CDT NOT BOUND TO A PATH BLOCK. p. 2-43 puts the DFREEQ on the PORT and this
 * module reaches the port only through cdt->pb->pdt, so a connection with no
 * Path Block has no account to draw on. scs_dgram_port_take()'s documented
 * NULL-pdt contract and scs_dgram_cdl_deliver()'s treatment of it (only
 * SCS_DGRAM_DISCARD_DFREEQ_EMPTY stops delivery; SCS_DGRAM_NO_CDT from the port
 * half falls through to the connection's own quota) were both written down and
 * neither was driven -- every other fixture here binds every CDT to a port.
 *
 * REACHABILITY, stated rather than implied: scs_cdl_alloc() accepts a NULL Path
 * Block and this is the behaviour it produces. No scsd.c path is known to pass
 * one today -- conn_bind() passes ps->pb -- so this pins an API contract, not
 * an observed production sequence.
 */
static void test_unbound_cdt_has_no_port_to_account_against(void)
{
    CHECK(scs_dgram_port_take(NULL) == SCS_DGRAM_NO_CDT,
          "a NULL port did not report NO_CDT");

    struct bench b;
    struct sysap s;
    CHECK(bench_init(&b, &s), "bench setup failed");
    scs_dgram_extend(b.a, 4); /* a real deposit on the bench port, to be left alone */

    struct sysap s_u;
    memset(&s_u, 0, sizeof(s_u));
    struct scs_cdt *u = scs_cdl_alloc(&b.cdl, "VMS$VAXcluster  ", "VMS$VAXcluster  ", NULL);
    CHECK(u != NULL, "unbound CDT allocation failed");
    if (u == NULL) {
        return;
    }
    CHECK(u->pb == NULL, "the CDT was bound to a path block after all");
    scs_cdt_set_handlers(u, NULL, sysap_dgram_input, NULL, &s_u);

    /* The CDT's own count moves; no port is credited, in particular not the
     * bench port that happens to be the only one around. */
    CHECK(scs_dgram_extend(u, 3) == 0, "extend on an unbound CDT failed");
    CHECK(u->dgram_buffers == 3, "unbound CDT count %u, want 3", u->dgram_buffers);
    CHECK(b.pdt.dfreeq_count == 4, "DFREEQ %u -- an unbound connection credited a"
          " port it is not on, want 4", b.pdt.dfreeq_count);

    /* The accounted receive path runs the port half, gets SCS_DGRAM_NO_CDT
     * because there is no port, and delivers anyway against the connection's
     * own count. Nothing is counted against any port. */
    CHECK(scs_dgram_cdl_deliver(&b.cdl, u->local_conid, 0, DG, sizeof(DG))
              == SCS_DGRAM_DELIVERED, "unbound connection did not receive");
    CHECK(s_u.calls == 1, "unbound input routine calls %lu, want 1", s_u.calls);
    CHECK(u->dgram_buffers == 2, "unbound CDT count %u after delivery, want 2",
          u->dgram_buffers);
    CHECK(b.pdt.dfreeq_count == 4, "DFREEQ %u -- an unbound delivery drew on a"
          " port it is not on, want 4", b.pdt.dfreeq_count);
    CHECK(scs_dgram_dfreeq_empty_discards(&b.pdt) == 0,
          "the bench port was charged an unbound connection's discard");

    /* And releasing it returns a deposit to nobody, quietly. */
    scs_cdl_release(&b.cdl, u);
    CHECK(b.pdt.dfreeq_count == 4, "releasing an unbound connection moved a port's"
          " depth to %u, want 4", b.pdt.dfreeq_count);
    CHECK(b.a->dgram_buffers == 4, "the bound connection's deposit %u was disturbed,"
          " want 4", b.a->dgram_buffers);
}

/*
 * The kill switch. With OVMX_NO_DGRAM_ACCOUNTING=1 the accounting is gone: no
 * count moves, and the datagram that WOULD have been discarded for want of
 * quota is delivered instead. Guardrail 23 in miniature -- the gated behaviour
 * is asserted to be actually suppressed, not merely claimed to be.
 */
static void test_kill_switch(void)
{
    setenv("OVMX_NO_DGRAM_ACCOUNTING", "1", 1);
    scs_dgram_reset_switch_cache();
    CHECK(scs_dgram_enabled() == 0, "switch did not disable the accounting");

    struct bench b;
    struct sysap s;
    CHECK(bench_init(&b, &s), "bench setup failed");

    CHECK(scs_dgram_extend(b.a, 3) == 0, "extend failed under the switch");
    CHECK(b.a->dgram_buffers == 0, "CDT count %u moved under the switch", b.a->dgram_buffers);
    CHECK(b.pdt.dfreeq_count == 0, "DFREEQ %u moved under the switch", b.pdt.dfreeq_count);

    /* Quota 0 and DFREEQ 0 -- with the accounting ON this is the discard case
     * (test_no_quota_discards_and_returns_the_buffer / the port discard). With
     * it OFF the datagram is delivered and nothing is counted. */
    CHECK(deliver(&b, b.a) == SCS_DGRAM_DELIVERED, "switch did not restore delivery");
    CHECK(s.calls == 1, "input routine calls %lu, want 1", s.calls);
    CHECK(scs_dgram_discards_no_quota(b.a) == 0, "discard counted under the switch");
    CHECK(scs_dgram_dfreeq_empty_discards(&b.pdt) == 0, "port discard counted under the switch");
    CHECK(scs_dgram_add_buffers(b.a, 4) == 0, "add_buffers moved under the switch");
    CHECK(scs_dgram_remove_buffers(b.a, 4) == 0, "remove_buffers moved under the switch");
    CHECK(scs_dgram_release_buffer(b.a) == 0, "release failed under the switch");
    CHECK(b.a->dgram_buffers == 0, "release moved a count under the switch");

    unsetenv("OVMX_NO_DGRAM_ACCOUNTING");
    scs_dgram_reset_switch_cache();
    CHECK(scs_dgram_enabled() == 1, "switch did not clear");
}

int main(void)
{
    test_extend_fills_dfreeq();
    test_receive_debits_both_counts();
    test_no_quota_discards_and_returns_the_buffer();
    test_empty_dfreeq_discards_at_the_port();
    test_release_and_deallocate();
    test_add_and_remove_are_clamped_to_the_deposit();
    test_dfreeq_is_per_port();
    test_unknown_conid_moves_nothing();
    test_no_input_routine_returns_the_buffer();
    test_report_prints_the_counters();
    /* The p. 2-42 debit/callback ORDER -- only observable to a SYSAP that
     * re-enters the API from inside its own input routine. */
    test_input_routine_sees_the_debit_already_applied();
    test_reentrant_delivery_cannot_spend_the_same_buffer_twice();
    test_release_from_inside_the_input_routine();
    test_both_receive_paths_resolve_identically();
    /* The CONNECTION LIFECYCLE -- releasing a connection must give the port its
     * datagram deposit back, or the account grows on every connect/release
     * cycle (and vms-17f's departure sweep drives that cycle in production). */
    test_release_returns_the_dfreeq_deposit_to_the_port();
    test_release_returns_only_what_is_still_on_deposit();
    test_departure_sweep_returns_the_dfreeq_deposit();
    test_re_extension_replaces_the_deposit_it_does_not_stack();
    test_unbound_cdt_has_no_port_to_account_against();
    test_kill_switch();

    printf("%s: %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
