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
 *
 * AND the OVMX_NO_DGRAM_ACCOUNTING kill switch, exercised by re-running the
 * whole discard scenario with the switch set and asserting the discard does
 * NOT happen and no count moves.
 *
 * NOT ASSERTED, because it is not true: that anything in SCSD.EXE routes a
 * datagram through this accounting. scs_dgram_cdl_deliver() has no production
 * caller (scs_dgram.h says so in full). A green run here proves the mechanism
 * is correct, not that OVMX exercises it. The one live daemon call site is
 * scs_dgram_report() in the exit summary, which prints zeros.
 *
 * NOT ASSERTED EITHER: any send-side debit. p. 2-42 states "SCA does not
 * provide a flow control mechanism for the datagram service"; the DFREEQ is a
 * receive-side account and no send path touches it. See the DIRECTION note in
 * scs_dgram.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    /* Exhausting port 2 does not touch port 1's empty-discard counter. */
    c2->dgram_buffers = 0;
    scs_dgram_remove_buffers(c2, 0);
    pdt2.dfreeq_count = 0;
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
    test_kill_switch();

    printf("%s: %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
