/*
 * test_scs_sdir.c - vms-7fe: the SDIR queue and the listening CDTs
 * (VAXcluster Principles pp. 2-48..2-50, Figure 2-24).
 *
 * SCOPE HONESTY, first, because this epic keeps rejecting tests that read as
 * coverage of something they do not reach:
 *
 *   - Everything here exercises src/vmsscs/scs_sdir.c through its public API.
 *     No case reaches into a struct to set a state by hand; the p. 2-50
 *     CONNECT RECEIVED transitions in these tests are all performed by
 *     scs_sdir_connect_req() itself.
 *   - The p. 2-50 BUSY reply is exercised HERE and NOWHERE ELSE, because
 *     scsd.c cannot reach it. scs_sdir.h DESIGN CHOICE 3 explains why: OVMX
 *     returns the listening CDT to LISTEN as soon as the answer is emitted, so
 *     under the daemon's single-threaded receive loop the CONNECT RECEIVED
 *     window is one call deep and a second request cannot land inside it. The
 *     case below gets inside it the only way anything can -- a connect-request
 *     handler that itself receives -- which is the module's real code path and
 *     is NOT how the daemon behaves. DO NOT READ THIS FILE AS "OVMX SENDS BUSY
 *     REPLIES". The daemon's exit summary reports busy-sent=0 and that is the
 *     truth.
 *   - The DAEMON-side behaviour (the SCS$DIR_LOOKUP responder answering from
 *     this queue, the connect scan on both inbound CONNECT_REQ paths, the
 *     refusal frame, and the OVMX_NO_SDIR bracket) is in
 *     tests/vmsscs/test_scsd_wire.c, driven through scsd_handle_frame().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scs_cdt.h"
#include "scs_sdir.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        checks++;                                               \
        if (!(cond)) {                                          \
            failures++;                                         \
            printf("FAIL %s:%d: ", __func__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

/* OVMX's three shipped Con.IDs, spelled here rather than included from scsd.c:
 * these values are on the lab wire and a listening CDT must never take one. */
#define WIRE_CONID_MEMBER (SCS_CDT_MAKE_CONID(1))
#define WIRE_CONID_JOINER (SCS_CDT_MAKE_CONID(2))
#define WIRE_CONID_DIR    (SCS_CDT_MAKE_CONID(7))

struct world {
    struct scs_cdl cdl;
    struct scs_sdir_queue q;
};

static void world_init(struct world *w)
{
    memset(w, 0, sizeof(*w));
    scs_cdl_init(&w->cdl);
    scs_sdir_queue_init(&w->q, &w->cdl);
}

/* ==========================================================================
 * (1) p. 2-48: LISTEN allocates an SDIR AND a listening CDT, and queues it
 * ========================================================================== */

static void handler_noop(const char *l, const char *r, uint32_t c, void *ctx)
{
    (void)l; (void)r; (void)c; (void)ctx;
}

static void test_listen_allocates_an_sdir_and_a_listening_cdt(void)
{
    struct world w;
    world_init(&w);

    int marker = 0;
    struct scs_sdir *s = scs_sdir_listen(&w.q, "VMS$VAXcluster", handler_noop, &marker);
    CHECK(s != NULL, "LISTEN queued no SDIR");
    if (s == NULL) {
        return;
    }

    /* Figure 2-24 draws exactly two fields in the SDIR box. Both are filled. */
    CHECK(strcmp(s->sysap, "VMS$VAXcluster") == 0,
          "the SDIR carries '%s', not the SYSAP name", s->sysap);
    CHECK(s->conid != 0, "the SDIR carries no listening-CDT CONID");

    /* "Each SDIR contains the CONID of a special 'listening CDT' that is also
     * allocated at this time." (p. 2-48) -- so the CONID must RESOLVE. */
    struct scs_cdt *lcdt = scs_sdir_listening_cdt(&w.q, s);
    CHECK(lcdt != NULL, "the SDIR's CONID resolves to no CDT in the CDL");
    CHECK(lcdt != NULL && lcdt->in_use, "the listening CDT is not in use");
    CHECK(lcdt != NULL && lcdt->local_conid == s->conid,
          "the CDT at the SDIR's CONID carries a different CONID");
    CHECK(scs_cdl_lookup(&w.cdl, s->conid) == lcdt,
          "the listening CDT is not reachable the p. 2-29 way, by CONID");

    /* It is marked as NOT a connection, which is what keeps every
     * connection-level report from counting it (scs_cdt.h). */
    CHECK(lcdt != NULL && lcdt->listening,
          "the listening CDT is not marked listening -- reports will count it"
          " as a connection parked off OPEN");
    CHECK(lcdt != NULL && lcdt->pb == NULL && lcdt->remote_conid == 0,
          "the listening CDT describes a peer or a circuit; it describes neither");

    /* "Instead of containing the address of a regular message input routine, a
     * listening CDT contains the address of the SYSAP's routine for handling
     * incoming connect requests." (p. 2-48) The routine is stored, and the
     * SYSAP context with it. */
    CHECK(lcdt != NULL && lcdt->msg_input != NULL,
          "the connect-request routine was not stored on the listening CDT");
    CHECK(lcdt != NULL && lcdt->sysap_ctx == &marker,
          "the SYSAP context was not stored on the listening CDT");

    /* It is IN THE QUEUE (Figure 2-24), findable by name. */
    CHECK(scs_sdir_count(&w.q) == 1, "the queue holds %u entries, expected 1",
          scs_sdir_count(&w.q));
    CHECK(scs_sdir_first(&w.q) == s, "the SDIR is not at the head of the queue");
    CHECK(scs_sdir_peek(&w.q, "VMS$VAXcluster") == s, "the queue cannot find it by name");

    /* A 16-byte blank-padded wire field finds it too -- that is how the
     * responder queries it. */
    CHECK(scs_sdir_peek(&w.q, "VMS$VAXcluster  ") == s,
          "a blank-padded wire name does not match the queued name");
    CHECK(scs_sdir_peek(&w.q, "MSCP$DISK") == NULL,
          "the queue answers yes for a name nobody registered");

    /* Duplicate LISTEN is refused rather than queueing a second answer. */
    CHECK(scs_sdir_listen(&w.q, "VMS$VAXcluster", NULL, NULL) == NULL,
          "LISTEN queued the same name twice");
    CHECK(scs_sdir_count(&w.q) == 1, "the duplicate grew the queue");

    /* No CDL means no listening CDT, so no SDIR -- INV-6, no degraded mode. */
    struct scs_sdir_queue orphan;
    scs_sdir_queue_init(&orphan, NULL);
    CHECK(scs_sdir_listen(&orphan, "X", NULL, NULL) == NULL,
          "LISTEN queued an SDIR with no listening CDT behind it");
    CHECK(scs_sdir_listen(&w.q, NULL, NULL, NULL) == NULL, "NULL name accepted");
    CHECK(scs_sdir_listen(NULL, "X", NULL, NULL) == NULL, "NULL queue accepted");
}

/* ==========================================================================
 * (2) THE RESERVED CONID BAND -- scs_sdir.h OVMX DESIGN CHOICE 1.
 *
 * This is the wire-safety assertion of the whole item. scs_cdl_alloc() hands
 * out the lowest free slot from 1 upward; OVMX has SHIPPED Con.IDs at slots 1,
 * 2 and 7. If a listening CDT took one, scs_cdl_alloc_conid() would refuse the
 * connection that needs it and OVMX would stop answering the member's
 * CONNECT-REQUEST -- a join failure caused by adding a listener.
 * ========================================================================== */
static void test_listening_cdts_cannot_take_a_shipped_conid(void)
{
    struct world w;
    world_init(&w);

    char nm[8];
    unsigned placed = 0;
    for (unsigned i = 0; i < SCS_SDIR_MAX + 3; i++) {
        snprintf(nm, sizeof(nm), "S%u", i);
        if (scs_sdir_listen(&w.q, nm, NULL, NULL) != NULL) {
            placed++;
        }
    }
    CHECK(placed == SCS_SDIR_MAX, "the queue took %u names, expected %u",
          placed, SCS_SDIR_MAX);

    /* Every listening CDT sits in the reserved band, in order. */
    unsigned i = 0;
    for (const struct scs_sdir *s = scs_sdir_first(&w.q); s != NULL;
         s = scs_sdir_next(s), i++) {
        CHECK(s->conid == SCS_CDT_MAKE_CONID(SCS_SDIR_CONID_SLOT_BASE + i),
              "listening CDT %u is at 0x%08X, outside the reserved band", i, s->conid);
    }

    /* AND THE THREE SHIPPED CON.IDs ARE STILL ALLOCATABLE with the queue full. */
    CHECK(scs_cdl_alloc_conid(&w.cdl, WIRE_CONID_MEMBER, "VMS$VAXcluster", NULL, NULL) != NULL,
          "a full SDIR queue blocked the member connection's shipped Con.ID");
    CHECK(scs_cdl_alloc_conid(&w.cdl, WIRE_CONID_JOINER, "VMS$VAXcluster", NULL, NULL) != NULL,
          "a full SDIR queue blocked the joiner connection's shipped Con.ID");
    CHECK(scs_cdl_alloc_conid(&w.cdl, WIRE_CONID_DIR, "SCS$DIRECTORY", NULL, NULL) != NULL,
          "a full SDIR queue blocked the SCS$DIRECTORY connection's shipped Con.ID");
}

/* ==========================================================================
 * (3) p. 2-48: the scan. Hit delivers; miss is "no such SYSAP".
 * ========================================================================== */

static struct {
    unsigned calls;
    char local[SCS_CDT_SYSAP_NAME_LEN + 1];
    uint32_t remote_conid;
    void *ctx;
} seen;

static void handler_record(const char *l, const char *r, uint32_t c, void *ctx)
{
    (void)r;
    seen.calls++;
    strncpy(seen.local, l ? l : "", SCS_CDT_SYSAP_NAME_LEN);
    seen.local[SCS_CDT_SYSAP_NAME_LEN] = '\0';
    seen.remote_conid = c;
    seen.ctx = ctx;
}

static void test_connect_req_hit_delivers_and_miss_is_no_such_sysap(void)
{
    struct world w;
    world_init(&w);
    memset(&seen, 0, sizeof(seen));

    int sysap = 0;
    struct scs_sdir *s = scs_sdir_listen(&w.q, "VMS$VAXcluster", handler_record, &sysap);
    CHECK(s != NULL, "LISTEN failed");

    /* MISS. "If none is found, SCS replies with a CONNECT_RSP containing the
     * 'no such SYSAP' error." (p. 2-48) */
    const struct scs_sdir *out = (const struct scs_sdir *)1;
    CHECK(scs_sdir_connect_req(&w.q, "MSCP$DISK", "VMS$DISK_CL_DRVR", 0x1111, &out) ==
              SCS_SDIR_NO_SUCH_SYSAP,
          "a request for an unlisted SYSAP was not refused");
    CHECK(out == NULL, "the miss handed back an SDIR");
    CHECK(seen.calls == 0, "the miss called a SYSAP handler");
    CHECK(w.q.no_such_sysap == 1, "the miss was not counted");
    CHECK(s->state == SCS_SDIR_LISTEN, "the miss moved a listening CDT's state");

    /* HIT. "The connect request is then delivered to the routine whose address
     * is in the listening CDT (and hence, to the target SYSAP)." (p. 2-48)
     * The 16-byte blank-padded wire form of the name must hit too. */
    out = NULL;
    CHECK(scs_sdir_connect_req(&w.q, "VMS$VAXcluster  ", "VMS$VAXcluster", 0x2222, &out) ==
              SCS_SDIR_DELIVERED,
          "a request for a listed SYSAP was not delivered");
    CHECK(out == s, "the hit handed back the wrong SDIR");
    CHECK(seen.calls == 1, "the handler ran %u times, expected 1", seen.calls);
    CHECK(strcmp(seen.local, "VMS$VAXcluster") == 0,
          "the handler was told it is '%s'", seen.local);
    CHECK(seen.remote_conid == 0x2222, "the handler was given remote Con.ID 0x%08X",
          seen.remote_conid);
    CHECK(seen.ctx == &sysap, "the handler got the wrong context");

    /* "SCS changes the state of its listening CDT to CONNECT RECEIVED."
     * (p. 2-50) -- performed by the call above, not by this test. */
    CHECK(s->state == SCS_SDIR_CONNECT_RECEIVED,
          "after delivery the listening CDT is %s, expected CONNECT RECEIVED",
          scs_sdir_state_name(s->state));

    /* "...at which time the listening CDT is returned to the LISTEN state." */
    scs_sdir_connect_answered(&w.q, s);
    CHECK(s->state == SCS_SDIR_LISTEN, "the listening CDT did not return to LISTEN");
    scs_sdir_connect_answered(&w.q, s); /* idempotent */
    CHECK(s->state == SCS_SDIR_LISTEN, "a second answer disturbed the state");

    /* A listener with NO handler still listens: the Directory Service answers
     * on the NAME, and delivery has simply nothing to call. That is exactly
     * scsd.c's position today. */
    CHECK(scs_sdir_listen(&w.q, "SCS$DIRECTORY", NULL, NULL) != NULL, "LISTEN failed");
    unsigned before = seen.calls;
    CHECK(scs_sdir_connect_req(&w.q, "SCS$DIRECTORY", NULL, 0x3333, NULL) ==
              SCS_SDIR_DELIVERED,
          "a handler-less listener refused a connect request");
    CHECK(seen.calls == before, "a handler-less delivery called somebody else's handler");
}

/* ==========================================================================
 * (4) p. 2-50: one connect request at a time -- and the retransmit exemption.
 *
 * See the SCOPE HONESTY note at the top: the reentrant handler is how this
 * module's BUSY path is reached, and scsd.c does not reach it.
 * ========================================================================== */

static struct scs_sdir_queue *reentrant_q = NULL;
static enum scs_sdir_result reentrant_second;
static enum scs_sdir_result reentrant_same;
static unsigned reentrant_depth = 0;

static void handler_reenters(const char *l, const char *r, uint32_t c, void *ctx)
{
    (void)l; (void)r; (void)ctx;
    if (reentrant_depth++ > 0) {
        return; /* the inner deliveries must not recurse forever */
    }
    /* We are INSIDE the delivery, so the listening CDT is in CONNECT RECEIVED
     * right now -- which is the only moment p. 2-50's rule has anything to say.
     * A DIFFERENT requester must be told to try again later. */
    reentrant_second = scs_sdir_connect_req(reentrant_q, "VMS$VAXcluster",
                                            NULL, c + 1, NULL);
    /* The SAME requester is a retransmit, not a second SYSAP (OVMX DESIGN
     * CHOICE 4), and must still be delivered. */
    reentrant_same = scs_sdir_connect_req(reentrant_q, "VMS$VAXcluster",
                                          NULL, c, NULL);
}

static void test_one_connect_request_at_a_time(void)
{
    struct world w;
    world_init(&w);
    reentrant_q = &w.q;
    reentrant_depth = 0;
    reentrant_second = SCS_SDIR_DELIVERED;
    reentrant_same = SCS_SDIR_NO_SUCH_SYSAP;

    struct scs_sdir *s = scs_sdir_listen(&w.q, "VMS$VAXcluster", handler_reenters, NULL);
    CHECK(s != NULL, "LISTEN failed");

    CHECK(scs_sdir_connect_req(&w.q, "VMS$VAXcluster", NULL, 0x4444, NULL) ==
              SCS_SDIR_DELIVERED,
          "the first request was not delivered");

    /* p. 2-50: "If another connect request is received while the listening CDT
     * is in the CONNECT RECEIVED state, SCS replies with a response that
     * essentially says 'busy ... try again later'." */
    CHECK(reentrant_second == SCS_SDIR_BUSY,
          "a second concurrent requester got %s, expected BUSY",
          scs_sdir_result_name(reentrant_second));
    CHECK(w.q.busy == 1, "the busy reply was not counted (%lu)", w.q.busy);

    /* OVMX DESIGN CHOICE 4: the same requester again is a retransmit. */
    CHECK(reentrant_same == SCS_SDIR_DELIVERED,
          "a RETRANSMIT from the same requester got %s -- a busy reply here would"
          " break the join, because the established VAX retransmits its"
          " CONNECT-REQUEST until it accepts our answer",
          scs_sdir_result_name(reentrant_same));
    CHECK(w.q.retransmits == 1, "the retransmit was not counted (%lu)", w.q.retransmits);
    CHECK(w.q.busy == 1, "the retransmit was ALSO counted busy");

    /* Once answered, the next requester is served normally. */
    scs_sdir_connect_answered(&w.q, s);
    reentrant_depth = 1; /* stop the handler recursing again */
    CHECK(scs_sdir_connect_req(&w.q, "VMS$VAXcluster", NULL, 0x5555, NULL) ==
              SCS_SDIR_DELIVERED,
          "a requester was refused after the listening CDT returned to LISTEN");
    CHECK(w.q.busy == 1, "a busy reply was issued from the LISTEN state");
}

/* ==========================================================================
 * (5) p. 2-49: ACCEPT allocates a SEPARATE CDT, and the local CONID that goes
 *     on the wire is the NEW one -- "and NOT the CONID of the listening CDT".
 * ========================================================================== */
static void test_accept_uses_a_conid_distinct_from_the_listening_cdt(void)
{
    struct world w;
    world_init(&w);

    struct scs_sdir *s = scs_sdir_listen(&w.q, "VMS$VAXcluster", NULL, NULL);
    CHECK(s != NULL, "LISTEN failed");
    struct scs_cdt *lcdt = scs_sdir_listening_cdt(&w.q, s);

    CHECK(scs_sdir_connect_req(&w.q, "VMS$VAXcluster", NULL, 0x6666, NULL) ==
              SCS_SDIR_DELIVERED, "delivery failed");

    /* The SYSAP accepts: a separate CDT is allocated for the connection. */
    struct scs_cdt *ccdt = scs_cdl_alloc(&w.cdl, "VMS$VAXcluster", "VMS$VAXcluster", NULL);
    CHECK(ccdt != NULL, "ACCEPT could not allocate a connection CDT");
    CHECK(ccdt != lcdt, "ACCEPT reused the LISTENING CDT for the new connection");
    CHECK(ccdt != NULL && lcdt != NULL && ccdt->local_conid != lcdt->local_conid,
          "the new connection's local CONID (0x%08X) is the listening CDT's"
          " -- p. 2-49 says it must NOT be",
          ccdt ? ccdt->local_conid : 0u);
    CHECK(ccdt != NULL && !ccdt->listening,
          "the connection CDT is marked as a listening CDT");
    /* Both are live at once: the SYSAP is still listening while it serves. */
    CHECK(lcdt != NULL && lcdt->in_use, "the listening CDT was consumed by the ACCEPT");
    CHECK(scs_cdl_in_use_count(&w.cdl) == 2,
          "expected the listening CDT and the connection CDT, CDL holds %u",
          scs_cdl_in_use_count(&w.cdl));
}

/* ==========================================================================
 * (6) The scan key: the GROUNDED target SYSAP name at payload [62:78].
 * ========================================================================== */
static void test_target_name_is_lifted_from_the_grounded_offset(void)
{
    /* Byte-exact SCA#21 (the golden SCS$DIRECTORY CONNECT-REQUEST from
     * formation-ci1-joinwindow.pcap) with a 14-byte Ethernet header, the same
     * payload tests/vmsscs/test_scs_dir.c carries. Its [62:78] reads
     * "SCS$DIRECTORY   ". */
    static const uint8_t sca21[110] = {
        0x6c,0x00,0xaa,0x00,0x04,0x00,0x02,0x04,0x01,0x00,0xaa,0x00,0x04,0x00,0x01,0x04,
        0x5b,0x13,0x00,0x00,0x01,0x00,0x01,0x00,0x12,0x00,0x00,0x00,0x00,0x00,0x01,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x02,0x42,0x00,0x04,0x00,0x00,0x00,
        0x03,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x05,0x63,0x00,0x00,0x01,0x00,0x53,0x43,
        0x53,0x24,0x44,0x49,0x52,0x45,0x43,0x54,0x4f,0x52,0x59,0x20,0x20,0x20,0x53,0x43,
        0x53,0x24,0x44,0x49,0x52,0x5f,0x4c,0x4f,0x4f,0x4b,0x55,0x50,0x20,0x20,0x20,0x20,
        0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20
    };
    uint8_t frame[14 + sizeof(sca21)];
    memset(frame, 0, 14);
    memcpy(frame + 14, sca21, sizeof(sca21));

    char name[SCS_CDT_SYSAP_NAME_LEN + 1];
    CHECK(scs_sdir_target_name(frame, sizeof(frame), name) == 0, "name lift failed");
    CHECK(strcmp(name, "SCS$DIRECTORY") == 0,
          "lifted '%s' from [62:78], expected 'SCS$DIRECTORY' (spec sec 4(h)(2))", name);

    /* A frame too short to carry the field must say so rather than read past. */
    CHECK(scs_sdir_target_name(frame, 40, name) == -1, "a short frame was accepted");
    CHECK(scs_sdir_target_name(NULL, sizeof(frame), name) == -1, "NULL frame accepted");
}

/* ==========================================================================
 * (7) The kill switch, read fresh on every call.
 * ========================================================================== */
static void test_kill_switch_is_read_fresh(void)
{
    CHECK(unsetenv("OVMX_NO_SDIR") == 0, "unsetenv failed");
    CHECK(scs_sdir_enabled() == 1, "the SDIR path is off with the switch unset");
    CHECK(setenv("OVMX_NO_SDIR", "0", 1) == 0, "setenv failed");
    CHECK(scs_sdir_enabled() == 1, "OVMX_NO_SDIR=0 turned the SDIR path off");
    CHECK(setenv("OVMX_NO_SDIR", "1", 1) == 0, "setenv failed");
    CHECK(scs_sdir_enabled() == 0, "OVMX_NO_SDIR=1 did not turn the SDIR path off");
    CHECK(unsetenv("OVMX_NO_SDIR") == 0, "unsetenv failed");
    CHECK(scs_sdir_enabled() == 1, "the switch did not come back on when cleared");
}

int main(void)
{
    printf("=== test_scs_sdir (vms-7fe): SDIR queue + listening CDTs ===\n");
    test_listen_allocates_an_sdir_and_a_listening_cdt();
    test_listening_cdts_cannot_take_a_shipped_conid();
    test_connect_req_hit_delivers_and_miss_is_no_such_sysap();
    test_one_connect_request_at_a_time();
    test_accept_uses_a_conid_distinct_from_the_listening_cdt();
    test_target_name_is_lifted_from_the_grounded_offset();
    test_kill_switch_is_read_fresh();
    printf("test_scs_sdir: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
