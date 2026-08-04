/*
 * test_scs_svc.c - the five SCS SYSAP services (vms-561).
 *
 * SOURCE for every claim: VAXcluster Principles ch. 2, pp. 2-22..2-27 and
 * p. 2-56. Page cites are on each case.
 *
 * TWO HALVES:
 *
 *  (1) THE SERVICE CONTRACT, case by case -- who allocates a CDT and who does
 *      not (p. 2-56), what CONNECT does when p. 2-47 finds no OPEN circuit,
 *      what ACCEPT does on a re-answer, what the arguments of pp. 2-22..2-23
 *      actually reach, and what the state machine does when a descriptor
 *      cannot be had.
 *
 *  (2) THE TWO-NODE INTEGRATION CASE -- two independent SCS instances, each
 *      with its own configuration database, CDL and service port, forming a
 *      connection, USING it (a message delivered through the p. 2-29
 *      destination-CONID path with the p. 2-43 credit account debited), and
 *      tearing it down through the full Figure 2-16 symmetric disconnect,
 *      ending with both CDLs empty and both ports' buffer deposits back.
 *
 * WHAT IS REAL AND WHAT IS A TEST DOUBLE, stated plainly because this epic has
 * rejected nine items for a claim outrunning its evidence:
 *
 *   REAL (production code, called exactly as the daemon calls it): scs_listen,
 *   scs_connect, scs_accept, scs_reject, scs_disconnect, scs_svc_close_if_closed,
 *   the CDL/CDT allocator, the connection state machine, the configuration
 *   database and its CONFIG_SYS/CONFIG_PATH queries, the credit and datagram
 *   accounts, and the p. 2-29 message delivery path.
 *
 *   A TEST DOUBLE: the WIRE. There is no Ethernet here. Each node's emitter
 *   hands the peer an SCS control message as a C struct, and a `deliver()`
 *   helper turns it into the matching received-message event with
 *   scs_conn_fsm_step(). THAT HELPER IS NOT PRODUCTION AND NOTHING HERE CLAIMS
 *   IT COVERS A PRODUCTION RECEIVE PATH: the daemon's receive dispatch is
 *   scsd.c's, tested in tests/vmsscs/test_scsd_wire.c, and the SYSAP-facing
 *   half of the receive side (symmetric disconnect) is vms-591's item. What
 *   this case proves is what it says: the five SERVICES compose into a complete
 *   connection lifecycle, and the descriptor accounting balances at the end.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scs_svc.h"
#include "scs_credit.h"
#include "scs_dgram.h"
#include "scs_dir.h"     /* SCS_DIR_OVMX_CONID -- a CONID scsd.c ships */

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

/* ==========================================================================
 * A NODE. Everything one SCS instance owns.
 * ========================================================================== */
struct node {
    struct scs_config  cfg;
    struct scs_pdt     pdt;
    struct scs_cdl     cdl;
    struct scs_svc_port port;
    struct scs_pb     *pb;     /* the circuit to the other node */
    uint8_t            sysid[6];
    const char        *name;

    /* what this node's SYSAP saw */
    int      msgs_in;
    int      losses;
    char     last_msg[32];
};

static void node_init(struct node *n, const char *name, uint8_t idbyte)
{
    memset(n, 0, sizeof(*n));
    n->name = name;
    scs_config_init(&n->cfg);
    scs_pdt_init(&n->pdt, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_cdl_init(&n->cdl);
    scs_svc_port_init(&n->port, &n->cdl);
    n->sysid[0] = 0xaa;
    n->sysid[1] = 0x00;
    n->sysid[2] = 0x04;
    n->sysid[3] = 0x00;
    n->sysid[4] = idbyte;
    n->sysid[5] = 0x04;
}

/* Give `n` an OPEN circuit to the node whose System Address is `peer_sysid`,
 * through the production configuration-queue transitions (p. 2-19/2-21). */
static void node_open_circuit(struct node *n, const uint8_t peer_sysid[6],
                              const uint8_t peer_port_addr[6])
{
    n->pb = scs_pb_create(&n->cfg, &n->pdt, peer_port_addr, SCS_PORT_TYPE_ETHERNET);
    if (n->pb == NULL) {
        return;
    }
    (void)scs_pb_learn_system_addr(&n->cfg, n->pb, peer_sysid);
    (void)scs_pb_open(&n->cfg, n->pb);
}

static void sysap_msg_in(struct scs_cdt *cdt, const void *buf, size_t len, void *ctx)
{
    struct node *n = (struct node *)ctx;
    (void)cdt;
    n->msgs_in++;
    size_t cap = (len < sizeof(n->last_msg) - 1) ? len : sizeof(n->last_msg) - 1;
    memcpy(n->last_msg, buf, cap);
    n->last_msg[cap] = '\0';
}

static void sysap_vc_loss(struct scs_cdt *cdt, void *ctx)
{
    struct node *n = (struct node *)ctx;
    (void)cdt;
    n->losses++;
}

/* ==========================================================================
 * THE TEST DOUBLE FOR THE WIRE. One SCS control message in flight.
 * ========================================================================== */
struct ctrl_msg {
    int                  valid;
    enum scs_conn_action action;
    uint32_t             src_conid;  /* sender's local CONID (0 when it has none) */
    uint32_t             dst_conid;  /* receiver's CONID, when the sender knows it */
    uint16_t             reason;
};

static struct ctrl_msg wire[8];
static unsigned wire_depth = 0;

static void wire_reset(void)
{
    memset(wire, 0, sizeof(wire));
    wire_depth = 0;
}

/* The PORT DRIVER of a node: p. 2-56's "the actual dialogue ... is managed by
 * SCS code in the port driver". It builds nothing (there is no wire) but it
 * answers the emit contract exactly as scsd.c's emitters do. */
static int node_emit(void *ctx, struct scs_cdt *cdt, enum scs_conn_action action,
                     const struct scs_svc_args *args, const char **what)
{
    (void)ctx;
    (void)what;
    if (wire_depth >= sizeof(wire) / sizeof(wire[0])) {
        return SCS_SVC_EMIT_REFUSED;
    }
    struct ctrl_msg *m = &wire[wire_depth++];
    m->valid = 1;
    m->action = action;
    m->src_conid = (cdt != NULL) ? cdt->local_conid : 0;
    m->dst_conid = (cdt != NULL) ? cdt->remote_conid : args->remote_conid;
    m->reason = args->reason;
    return SCS_SVC_EMIT_SENT;
}

/* A port that cannot build the frame the machine asked for. */
static int emit_nobuilder(void *ctx, struct scs_cdt *cdt, enum scs_conn_action action,
                          const struct scs_svc_args *args, const char **what)
{
    (void)ctx; (void)cdt; (void)action; (void)args; (void)what;
    return SCS_SVC_EMIT_NOBUILDER;
}

/* A port that refuses (no circuit, p. 2-31). */
static int emit_refused(void *ctx, struct scs_cdt *cdt, enum scs_conn_action action,
                        const struct scs_svc_args *args, const char **what)
{
    (void)ctx; (void)cdt; (void)action; (void)args; (void)what;
    return SCS_SVC_EMIT_REFUSED;
}

/* ==========================================================================
 * (1) THE SERVICE CONTRACT
 * ========================================================================== */

/* p. 2-22: LISTEN "will place the SYSAP's name into a 'list of listening
 * SYSAPs'", and p. 2-22 again: the Directory Service answers "inquires from
 * other nodes wanting to know if particular SYSAP names are in this list". */
static void test_listen_places_the_name_in_the_list(void)
{
    struct node n;
    node_init(&n, "LISTENER", 0x01);

    CHECK(!scs_svc_listening(&n.port, "VMS$VAXcluster"),
          "a fresh port already claims to be listening");
    CHECK(scs_listen(&n.port, "VMS$VAXcluster", NULL, NULL) == SCS_SVC_OK,
          "LISTEN refused a name on an empty list");
    CHECK(scs_svc_listening(&n.port, "VMS$VAXcluster"),
          "LISTEN did not place the name in the list");
    CHECK(!scs_svc_listening(&n.port, "MSCP$DISK"),
          "the list answers yes for a name nobody registered");

    /* A duplicate is a SYSAP bug, and is reported rather than swallowed. */
    CHECK(scs_listen(&n.port, "VMS$VAXcluster", NULL, NULL) == SCS_SVC_NOLISTEN,
          "LISTEN accepted the same name twice");

    /*
     * vms-7fe CORRECTS THIS ASSERTION. It used to read "LISTEN is not one of
     * the services that allocates a CDT", cited to p. 2-56. That over-read the
     * page: p. 2-56 says "The SCS CONNECT and ACCEPT services each result in
     * the allocation of a CDT FOR NEW CONNECTIONS", and a listening CDT is not
     * a connection. p. 2-48 is explicit the other way -- "an SDIR containing the
     * SYSAP's name is allocated and placed in this queue. Each SDIR contains the
     * CONID of a special 'listening CDT' that is also allocated at this time."
     * Source-of-truth order puts the book above a test comment, so the number
     * changes and the claim gets sharper: LISTEN allocates exactly one CDT, it
     * is the LISTENING CDT the SDIR names, and it lives in the reserved band
     * that cannot collide with a connection Con.ID.
     */
    CHECK(scs_cdl_in_use_count(&n.cdl) == 1,
          "LISTEN allocated %u CDT(s) -- p. 2-48 gives it exactly one, the"
          " listening CDT", scs_cdl_in_use_count(&n.cdl));
    {
        const struct scs_sdir *sd = scs_sdir_peek(scs_svc_sdir(&n.port),
                                                  "VMS$VAXcluster");
        CHECK(sd != NULL, "LISTEN queued no SDIR");
        CHECK(sd != NULL && sd->conid == SCS_SDIR_CONID_BASE,
              "the listening CDT is at 0x%08X, expected the reserved 0x%08X",
              sd ? sd->conid : 0u, (unsigned)SCS_SDIR_CONID_BASE);
        struct scs_cdt *lcdt = scs_sdir_listening_cdt(scs_svc_sdir(&n.port), sd);
        CHECK(lcdt != NULL && lcdt->local_conid == sd->conid,
              "the SDIR's CONID does not resolve to a listening CDT through the CDL");
    }

    /* The list fills, and says so rather than overrunning. */
    char nm[8];
    unsigned placed = 0;
    for (unsigned i = 0; i < SCS_SVC_MAX_LISTENERS + 2; i++) {
        snprintf(nm, sizeof(nm), "S%u", i);
        if (scs_listen(&n.port, nm, NULL, NULL) == SCS_SVC_OK) {
            placed++;
        }
    }
    CHECK(placed == SCS_SVC_MAX_LISTENERS - 1,
          "%u further names fit beside the first, expected %u", placed,
          SCS_SVC_MAX_LISTENERS - 1);

    CHECK(scs_listen(NULL, "X", NULL, NULL) == SCS_SVC_BADARG, "NULL port");
    CHECK(scs_listen(&n.port, NULL, NULL, NULL) == SCS_SVC_BADARG, "NULL name");
}

/*
 * p. 2-56: "The SCS CONNECT and ACCEPT services each result in the allocation
 * of a CDT for new connections." Both halves, counted.
 */
static void test_connect_and_accept_allocate_a_cdt(void)
{
    struct node a, b;
    static const uint8_t pa[6] = {0x08, 0x00, 0x2b, 0x00, 0x00, 0x0a};
    static const uint8_t pb_[6] = {0x08, 0x00, 0x2b, 0x00, 0x00, 0x0b};
    node_init(&a, "A", 0x01);
    node_init(&b, "B", 0x02);
    node_open_circuit(&a, b.sysid, pb_);
    node_open_circuit(&b, a.sysid, pa);
    wire_reset();

    struct scs_svc_args args;
    memset(&args, 0, sizeof(args));
    args.local_sysap = "VMS$VAXcluster";
    args.remote_sysap = "VMS$VAXcluster";
    args.target_node = b.sysid;
    args.cfg = &a.cfg;
    args.vc_loss = sysap_vc_loss;
    args.msg_input = sysap_msg_in;
    args.sysap_ctx = &a;
    args.emit = node_emit;

    struct scs_cdt *acdt = NULL;
    CHECK(scs_connect(&a.port, &args, &acdt) == SCS_SVC_OK, "CONNECT refused an open node");
    CHECK(acdt != NULL, "CONNECT allocated no CDT");
    CHECK(scs_cdl_in_use_count(&a.cdl) == 1, "CONNECT left %u CDTs in use",
          scs_cdl_in_use_count(&a.cdl));
    CHECK(a.port.cdts_allocated == 1, "CONNECT counted %lu allocations",
          a.port.cdts_allocated);
    if (acdt != NULL) {
        /* pp. 2-28/2-29: the arguments reach the CDT. */
        CHECK(acdt->vc_loss_handler == sysap_vc_loss, "the VC-loss handler was not stored");
        CHECK(acdt->msg_input == sysap_msg_in, "the message input routine was not stored");
        CHECK(acdt->sysap_ctx == &a, "the SYSAP context was not stored");
        CHECK(strcmp(acdt->local_sysap, "VMS$VAXcluster") == 0, "source SYSAP name");
        CHECK(strcmp(acdt->remote_sysap, "VMS$VAXcluster") == 0, "target SYSAP name");
        /* p. 2-28: the CDT is queued to the Path Block of the circuit that
         * supports it, and p. 2-47 chose that circuit. */
        CHECK(acdt->pb == a.pb, "the CDT is not queued to the selected circuit");
        CHECK(scs_conn_state_of(acdt) == SCS_CONN_CONNECT_SENT,
              "after CONNECT the connection is %s, expected CONNECT SENT",
              scs_conn_state_name(scs_conn_state_of(acdt)));
    }
    CHECK(wire_depth == 1 && wire[0].action == SCS_CONN_ACT_SEND_CONNECT_REQ,
          "CONNECT emitted %u message(s); first was %s", wire_depth,
          wire_depth ? scs_conn_action_name(wire[0].action) : "none");

    /* --- ACCEPT on the other node. --- */
    struct scs_svc_args bargs;
    memset(&bargs, 0, sizeof(bargs));
    bargs.local_sysap = "VMS$VAXcluster";
    bargs.remote_sysap = "VMS$VAXcluster";
    bargs.vc = b.pb;
    bargs.vc_loss = sysap_vc_loss;
    bargs.msg_input = sysap_msg_in;
    bargs.sysap_ctx = &b;
    bargs.remote_conid = wire[0].src_conid;
    bargs.emit = node_emit;

    unsigned before = wire_depth;
    struct scs_cdt *bcdt = NULL;
    CHECK(scs_accept(&b.port, &bargs, &bcdt) == SCS_SVC_OK, "ACCEPT refused");
    CHECK(bcdt != NULL, "ACCEPT allocated no CDT");
    CHECK(scs_cdl_in_use_count(&b.cdl) == 1, "ACCEPT left %u CDTs in use",
          scs_cdl_in_use_count(&b.cdl));
    CHECK(b.port.cdts_allocated == 1, "ACCEPT counted %lu allocations",
          b.port.cdts_allocated);
    if (bcdt != NULL) {
        CHECK(bcdt->remote_conid == wire[0].src_conid,
              "ACCEPT did not record the peer's CONID");
        CHECK(scs_conn_state_of(bcdt) == SCS_CONN_ACCEPT_SENT,
              "after ACCEPT the connection is %s, expected ACCEPT SENT",
              scs_conn_state_name(scs_conn_state_of(bcdt)));
    }
    /* p. 2-23: ACCEPT's node sends the CONNECT_RSP and then the ACCEPT_REQ. */
    CHECK(wire_depth == before + 2, "ACCEPT emitted %u message(s), expected 2",
          wire_depth - before);
    CHECK(wire_depth >= before + 2 &&
              wire[before].action == SCS_CONN_ACT_SEND_CONNECT_RSP &&
              wire[before + 1].action == SCS_CONN_ACT_SEND_ACCEPT_REQ,
          "ACCEPT emitted the wrong pair, or in the wrong order");
}

/*
 * p. 2-56: "The SCS REJECT service does not involve a CDT at all, but merely
 * the sending of a message rejecting a connect request." The claim, as a
 * number: the CDL is untouched.
 */
static void test_reject_allocates_no_cdt(void)
{
    struct node b;
    node_init(&b, "B", 0x02);
    wire_reset();

    struct scs_svc_args args;
    memset(&args, 0, sizeof(args));
    args.local_sysap = "VMS$VAXcluster";
    args.remote_sysap = "VMS$VAXcluster";
    args.remote_conid = 0x63050008u;
    args.reason = 0x1234; /* vms-6b3 owns the value; REJECT only carries it */
    args.emit = node_emit;

    unsigned in_use_before = scs_cdl_in_use_count(&b.cdl);
    CHECK(scs_reject(&b.port, &args) == SCS_SVC_OK, "REJECT refused");
    CHECK(scs_cdl_in_use_count(&b.cdl) == in_use_before,
          "REJECT changed the CDL from %u to %u in use -- p. 2-56 says it does"
          " not involve a CDT at all", in_use_before, scs_cdl_in_use_count(&b.cdl));
    CHECK(b.port.cdts_allocated == 0, "REJECT allocated %lu CDT(s)", b.port.cdts_allocated);
    CHECK(b.port.cdts_released == 0, "REJECT released %lu CDT(s)", b.port.cdts_released);
    /* Figure 2-15: the packet is a REJECT_REQ, and the reason code rides it. */
    CHECK(wire_depth == 1 && wire[0].action == SCS_CONN_ACT_SEND_REJECT_REQ,
          "REJECT emitted %u message(s); first was %s", wire_depth,
          wire_depth ? scs_conn_action_name(wire[0].action) : "none");
    CHECK(wire_depth == 1 && wire[0].reason == 0x1234,
          "REJECT dropped the reason code (0x%04x)", wire_depth ? wire[0].reason : 0);
    CHECK(wire_depth == 1 && wire[0].dst_conid == 0x63050008u,
          "REJECT did not address the requester's CONID");

    /* A port with no builder for a REJECT_REQ -- which is OVMX today -- must be
     * told nothing went out, not that it did. */
    wire_reset();
    args.emit = emit_nobuilder;
    CHECK(scs_reject(&b.port, &args) == SCS_SVC_NOTSENT,
          "REJECT reported success for a frame the port could not build");
    CHECK(b.port.unemitted == 1, "the unbuildable REJECT_REQ was not counted");
}

/*
 * p. 2-47: CONNECT "examines each Path Block in turn until it finds one whose
 * virtual circuit is OPEN". With none, nothing is allocated and nothing is
 * emitted -- a CDT for a request that never left the node would be a lie the
 * CDL carries for the rest of the run.
 */
static void test_connect_without_an_open_circuit_allocates_nothing(void)
{
    struct node a, b;
    static const uint8_t pb_[6] = {0x08, 0x00, 0x2b, 0x00, 0x00, 0x0b};
    node_init(&a, "A", 0x01);
    node_init(&b, "B", 0x02);
    wire_reset();

    struct scs_svc_args args;
    memset(&args, 0, sizeof(args));
    args.local_sysap = "VMS$VAXcluster";
    args.remote_sysap = "VMS$VAXcluster";
    args.target_node = b.sysid;
    args.cfg = &a.cfg;
    args.emit = node_emit;

    /* (a) the node is not in the configuration database at all. */
    struct scs_cdt *cdt = NULL;
    CHECK(scs_connect(&a.port, &args, &cdt) == SCS_SVC_NOVC,
          "CONNECT invented a circuit for an unknown node");
    CHECK(cdt == NULL && scs_cdl_in_use_count(&a.cdl) == 0, "a CDT was allocated anyway");
    CHECK(wire_depth == 0, "CONNECT emitted %u message(s) with no circuit", wire_depth);

    /* (b) the node is known but its circuit is still forming. */
    a.pb = scs_pb_create(&a.cfg, &a.pdt, pb_, SCS_PORT_TYPE_ETHERNET);
    CHECK(a.pb != NULL, "no Path Block");
    if (a.pb != NULL) {
        (void)scs_pb_learn_system_addr(&a.cfg, a.pb, b.sysid);
        scs_pb_set_vc_state(a.pb, SCS_VC_START_SENT);
    }
    CHECK(scs_connect(&a.port, &args, &cdt) == SCS_SVC_NOVC,
          "CONNECT accepted a circuit that is not OPEN");
    CHECK(scs_cdl_in_use_count(&a.cdl) == 0, "a CDT was allocated for a forming circuit");

    /* (c) a NAMED circuit that is not OPEN is refused the same way. p. 2-47
     * permits naming a circuit; it does not permit inventing one. */
    args.vc = a.pb;
    CHECK(scs_connect(&a.port, &args, &cdt) == SCS_SVC_NOVC,
          "CONNECT sent on a NAMED circuit that is not OPEN");
    CHECK(wire_depth == 0, "CONNECT emitted %u message(s) with no OPEN circuit",
          wire_depth);

    /* (d) and when the port REFUSES the frame, the descriptor is unwound. */
    (void)scs_pb_open(&a.cfg, a.pb);
    args.emit = emit_refused;
    CHECK(scs_connect(&a.port, &args, &cdt) == SCS_SVC_NOTSENT,
          "CONNECT reported success for a refused frame");
    CHECK(cdt == NULL, "a refused CONNECT handed back a CDT");
    CHECK(scs_cdl_in_use_count(&a.cdl) == 0,
          "a refused CONNECT left %u CDT(s) in the CDL",
          scs_cdl_in_use_count(&a.cdl));
    CHECK(a.port.refused == 1, "the refusal was not counted");
}

/*
 * A retransmitted CONNECT_REQ, and a re-answered CONNECT_REQ, must each drive
 * ONE connection rather than allocate a parallel one. Both are OVMX rows in
 * scs_conn.c (labeled, not documented), and both are what the lab wire does.
 */
static void test_retransmit_reuses_the_connection(void)
{
    struct node a, b;
    static const uint8_t pa[6] = {0x08, 0x00, 0x2b, 0x00, 0x00, 0x0a};
    static const uint8_t pb_[6] = {0x08, 0x00, 0x2b, 0x00, 0x00, 0x0b};
    node_init(&a, "A", 0x01);
    node_init(&b, "B", 0x02);
    node_open_circuit(&a, b.sysid, pb_);
    node_open_circuit(&b, a.sysid, pa);
    wire_reset();

    struct scs_svc_args args;
    memset(&args, 0, sizeof(args));
    args.local_sysap = "VMS$VAXcluster";
    args.remote_sysap = "VMS$VAXcluster";
    args.target_node = b.sysid;
    args.cfg = &a.cfg;
    args.conid = SCS_DIR_OVMX_CONID; /* a CONID scsd.c really ships */
    args.emit = node_emit;

    struct scs_cdt *c1 = NULL, *c2 = NULL;
    CHECK(scs_connect(&a.port, &args, &c1) == SCS_SVC_OK, "first CONNECT");
    CHECK(scs_connect(&a.port, &args, &c2) == SCS_SVC_OK, "retransmitted CONNECT");
    CHECK(c1 != NULL && c1 == c2, "the retransmit allocated a second CDT");
    CHECK(scs_cdl_in_use_count(&a.cdl) == 1, "%u CDTs in use after a retransmit",
          scs_cdl_in_use_count(&a.cdl));
    CHECK(c1 != NULL && c1->local_conid == SCS_DIR_OVMX_CONID,
          "the requested CONID was not claimed");
    CHECK(c1 != NULL && scs_conn_state_of(c1) == SCS_CONN_CONNECT_SENT,
          "a retransmit moved the state to %s",
          c1 ? scs_conn_state_name(scs_conn_state_of(c1)) : "?");
    CHECK(a.port.illegal == 0, "a retransmit scored %lu illegal event(s)", a.port.illegal);
    CHECK(wire_depth == 2, "%u CONNECT_REQs went out for two calls", wire_depth);

    /* ACCEPT's re-answer: one frame, and it is the ACCEPT_REQ. */
    struct scs_svc_args bargs;
    memset(&bargs, 0, sizeof(bargs));
    bargs.local_sysap = "VMS$VAXcluster";
    bargs.remote_sysap = "VMS$VAXcluster";
    bargs.vc = b.pb;
    /* ACCEPT names the connection it is re-answering by CONID, exactly as
     * scsd.c does (it always claims OVMX_LOCAL_CONID). Without a named CONID
     * there is nothing for the service to recognise and it would allocate. */
    bargs.conid = SCS_CDT_MAKE_CONID(9);
    bargs.remote_conid = SCS_DIR_OVMX_CONID;
    bargs.emit = node_emit;
    struct scs_cdt *bc = NULL;
    CHECK(scs_accept(&b.port, &bargs, &bc) == SCS_SVC_OK, "ACCEPT");
    unsigned after_accept = wire_depth;
    bargs.retransmit = 1;
    struct scs_cdt *bc2 = NULL;
    CHECK(scs_accept(&b.port, &bargs, &bc2) == SCS_SVC_OK, "re-answer");
    CHECK(bc == bc2, "the re-answer allocated a second CDT");
    CHECK(scs_cdl_in_use_count(&b.cdl) == 1, "%u CDTs after a re-answer",
          scs_cdl_in_use_count(&b.cdl));
    CHECK(wire_depth == after_accept + 1,
          "the re-answer emitted %u message(s), expected exactly 1",
          wire_depth - after_accept);
    CHECK(wire_depth == after_accept + 1 &&
              wire[after_accept].action == SCS_CONN_ACT_SEND_ACCEPT_REQ,
          "the re-answer sent the wrong message");
    CHECK(bc != NULL && scs_conn_state_of(bc) == SCS_CONN_ACCEPT_SENT,
          "the re-answer moved the state to %s",
          bc ? scs_conn_state_name(scs_conn_state_of(bc)) : "?");
    CHECK(b.port.illegal == 0, "the re-answer scored %lu illegal event(s)", b.port.illegal);
}

/*
 * A CONID already claimed by a connection on a DIFFERENT circuit is a different
 * connection wearing the same handle. The service must not hand that CDT back
 * -- but it must still emit, because the state machine is a recorder, never a
 * gate. This is scs_cdt.h's node-global-CONID limit, exercised.
 */
static void test_a_taken_conid_is_refused_but_still_sends(void)
{
    struct node a;
    static const uint8_t p1[6] = {0x08, 0x00, 0x2b, 0x00, 0x00, 0x11};
    static const uint8_t p2[6] = {0x08, 0x00, 0x2b, 0x00, 0x00, 0x22};
    static const uint8_t peer1[6] = {0xaa, 0x00, 0x04, 0x00, 0x31, 0x04};
    static const uint8_t peer2[6] = {0xaa, 0x00, 0x04, 0x00, 0x32, 0x04};
    node_init(&a, "A", 0x01);
    wire_reset();

    struct scs_pb *vc1 = scs_pb_create(&a.cfg, &a.pdt, p1, SCS_PORT_TYPE_ETHERNET);
    struct scs_pb *vc2 = scs_pb_create(&a.cfg, &a.pdt, p2, SCS_PORT_TYPE_ETHERNET);
    CHECK(vc1 != NULL && vc2 != NULL, "two Path Blocks");
    if (vc1 == NULL || vc2 == NULL) {
        return;
    }
    (void)scs_pb_learn_system_addr(&a.cfg, vc1, peer1);
    (void)scs_pb_learn_system_addr(&a.cfg, vc2, peer2);
    (void)scs_pb_open(&a.cfg, vc1);
    (void)scs_pb_open(&a.cfg, vc2);

    struct scs_svc_args args;
    memset(&args, 0, sizeof(args));
    args.local_sysap = "VMS$VAXcluster";
    args.remote_sysap = "VMS$VAXcluster";
    args.conid = SCS_DIR_OVMX_CONID;
    args.emit = node_emit;

    struct scs_cdt *c1 = NULL, *c2 = NULL;
    args.vc = vc1;
    CHECK(scs_connect(&a.port, &args, &c1) == SCS_SVC_OK, "first peer");
    args.vc = vc2;
    CHECK(scs_connect(&a.port, &args, &c2) == SCS_SVC_NOCDT,
          "the second peer was given the first peer's CDT");
    CHECK(c2 == NULL, "a CDT came back for the taken CONID");
    CHECK(scs_cdl_in_use_count(&a.cdl) == 1, "%u CDTs in use", scs_cdl_in_use_count(&a.cdl));
    CHECK(wire_depth == 2,
          "%u CONNECT_REQs went out -- the second peer's frame was suppressed by a"
          " bookkeeping shortage", wire_depth);
}

/*
 * DESCRIPTORLESS MODE (an OVMX addition -- see scs_svc.h). With
 * OVMX_NO_CONN_FSM set, nothing is recorded and EVERY frame still goes out,
 * including BOTH of ACCEPT's. Guardrail 23: the switch is RUN and the thing it
 * gates is measured moving, in that order.
 */
static void test_kill_switch_records_nothing_and_sends_everything(void)
{
    struct node b;
    static const uint8_t pa[6] = {0x08, 0x00, 0x2b, 0x00, 0x00, 0x0a};

    struct scs_svc_args args;
    memset(&args, 0, sizeof(args));
    args.local_sysap = "SCS$DIRECTORY";
    args.remote_sysap = "SCS$DIRECTORY";
    args.conid = SCS_DIR_OVMX_CONID;
    args.remote_conid = 0x63050008u;
    args.emit = node_emit;

    /* --- machine ON: the control. --- */
    CHECK(unsetenv("OVMX_NO_CONN_FSM") == 0, "unsetenv");
    node_init(&b, "B", 0x02);
    node_open_circuit(&b, (const uint8_t[6]){0xaa, 0, 4, 0, 0x31, 4}, pa);
    args.vc = b.pb;
    wire_reset();
    struct scs_cdt *on = NULL;
    CHECK(scs_accept(&b.port, &args, &on) == SCS_SVC_OK, "ACCEPT with the machine on");
    unsigned on_frames = wire_depth;
    CHECK(on != NULL, "no CDT with the machine on");
    CHECK(b.port.transitions == 2, "%lu transitions with the machine on, expected 2",
          b.port.transitions);
    CHECK(scs_cdl_in_use_count(&b.cdl) == 1, "%u CDTs with the machine on",
          scs_cdl_in_use_count(&b.cdl));
    CHECK(on_frames == 2, "%u frames with the machine on, expected 2", on_frames);

    /* --- machine OFF: same call, same frames, nothing recorded. --- */
    CHECK(setenv("OVMX_NO_CONN_FSM", "1", 1) == 0, "setenv");
    struct node c;
    node_init(&c, "C", 0x03);
    node_open_circuit(&c, (const uint8_t[6]){0xaa, 0, 4, 0, 0x31, 4}, pa);
    args.vc = c.pb;
    wire_reset();
    struct scs_cdt *off = NULL;
    CHECK(scs_accept(&c.port, &args, &off) == SCS_SVC_OK, "ACCEPT with the machine off");
    CHECK(off == NULL, "the kill switch did not stop CDT allocation");
    CHECK(scs_cdl_in_use_count(&c.cdl) == 0, "the kill switch left %u CDT(s)",
          scs_cdl_in_use_count(&c.cdl));
    CHECK(c.port.transitions == 0, "the kill switch recorded %lu transition(s)",
          c.port.transitions);
    CHECK(wire_depth == on_frames,
          "the kill switch changed the frames: %u with it set, %u without."
          " Descriptorless mode exists so it cannot", wire_depth, on_frames);
    CHECK(wire_depth == 2 &&
              wire[0].action == SCS_CONN_ACT_SEND_CONNECT_RSP &&
              wire[1].action == SCS_CONN_ACT_SEND_ACCEPT_REQ,
          "the kill switch changed WHICH frames went out");
    CHECK(c.port.illegal == 0,
          "the kill switch made ACCEPT score %lu illegal event(s) -- the second"
          " transition lost its starting state", c.port.illegal);
    CHECK(unsetenv("OVMX_NO_CONN_FSM") == 0, "unsetenv");
}

/*
 * DISCONNECT does not close a connection by itself. p. 2-26: "the symmetry that
 * SCA builds into the concept of a connection requires that the other SYSAP
 * respond by also invoking the DISCONNECT service." Releasing the CDT on the
 * first invocation would free a descriptor the peer is still talking to.
 */
static void test_one_disconnect_does_not_release(void)
{
    struct node a, b;
    static const uint8_t pa[6] = {0x08, 0x00, 0x2b, 0x00, 0x00, 0x0a};
    static const uint8_t pb_[6] = {0x08, 0x00, 0x2b, 0x00, 0x00, 0x0b};
    node_init(&a, "A", 0x01);
    node_init(&b, "B", 0x02);
    node_open_circuit(&a, b.sysid, pb_);
    node_open_circuit(&b, a.sysid, pa);
    wire_reset();

    struct scs_svc_args args;
    memset(&args, 0, sizeof(args));
    args.local_sysap = "VMS$VAXcluster";
    args.remote_sysap = "VMS$VAXcluster";
    args.vc = a.pb;
    args.emit = node_emit;
    struct scs_cdt *cdt = scs_cdl_alloc(&a.cdl, "VMS$VAXcluster", "VMS$VAXcluster", a.pb);
    CHECK(cdt != NULL, "no CDT");
    if (cdt == NULL) {
        return;
    }
    scs_conn_fsm_init(cdt);
    /* Put it in OPEN the way Figure 2-14 does, through the machine. */
    (void)scs_conn_fsm_step(cdt, SCS_CONN_EV_SVC_CONNECT);
    (void)scs_conn_fsm_step(cdt, SCS_CONN_EV_RCV_CONNECT_RSP);
    (void)scs_conn_fsm_step(cdt, SCS_CONN_EV_RCV_ACCEPT_REQ);
    CHECK(scs_conn_state_of(cdt) == SCS_CONN_OPEN, "fixture did not reach OPEN");

    args.reason = 0x0042; /* p. 2-26's optional reason code; vms-6b3 owns it */
    CHECK(scs_disconnect(&a.port, cdt, &args) == SCS_SVC_OK, "DISCONNECT refused");
    CHECK(scs_conn_state_of(cdt) == SCS_CONN_DISC_SENT,
          "after DISCONNECT the connection is %s, expected DISC SENT",
          scs_conn_state_name(scs_conn_state_of(cdt)));
    CHECK(cdt->in_use, "DISCONNECT released the CDT on the FIRST invocation --"
                       " p. 2-26 requires the peer's matching DISCONNECT first");
    CHECK(a.port.cdts_released == 0, "%lu release(s) on one DISCONNECT",
          a.port.cdts_released);
    CHECK(wire_depth == 1 && wire[0].action == SCS_CONN_ACT_SEND_DISCONNECT_REQ,
          "DISCONNECT emitted %u message(s)", wire_depth);
    CHECK(wire_depth == 1 && wire[0].reason == 0x0042,
          "DISCONNECT dropped the reason code");

    /* And scs_svc_close_if_closed() refuses to release a connection that is not
     * CLOSED -- it is state-driven, not a rename of "free it now". */
    CHECK(scs_svc_close_if_closed(&a.port, cdt) == 0,
          "close_if_closed released a connection in DISC SENT");
}

/* ==========================================================================
 * (2) THE TWO-NODE INTEGRATION CASE
 * ========================================================================== */

/*
 * The test double for the receive side. NOT PRODUCTION -- see the file header.
 * It turns one emitted control message into the matching received event on the
 * other node, steps that node's machine with the PRODUCTION scs_conn_fsm_step(),
 * emits whatever the machine then requires, and releases the CDT if the
 * connection reached CLOSED (through the production
 * scs_svc_close_if_closed()).
 */
static int action_to_event(enum scs_conn_action act, enum scs_conn_event *ev)
{
    switch (act) {
    case SCS_CONN_ACT_SEND_CONNECT_REQ:    *ev = SCS_CONN_EV_RCV_CONNECT_REQ; return 1;
    case SCS_CONN_ACT_SEND_CONNECT_RSP:    *ev = SCS_CONN_EV_RCV_CONNECT_RSP; return 1;
    case SCS_CONN_ACT_SEND_ACCEPT_REQ:     *ev = SCS_CONN_EV_RCV_ACCEPT_REQ; return 1;
    case SCS_CONN_ACT_SEND_ACCEPT_RSP:     *ev = SCS_CONN_EV_RCV_ACCEPT_RSP; return 1;
    case SCS_CONN_ACT_SEND_REJECT_REQ:     *ev = SCS_CONN_EV_RCV_REJECT_REQ; return 1;
    case SCS_CONN_ACT_SEND_REJECT_RSP:     *ev = SCS_CONN_EV_RCV_REJECT_RSP; return 1;
    case SCS_CONN_ACT_SEND_DISCONNECT_REQ: *ev = SCS_CONN_EV_RCV_DISCONNECT_REQ; return 1;
    case SCS_CONN_ACT_SEND_DISCONNECT_RSP: *ev = SCS_CONN_EV_RCV_DISCONNECT_RSP; return 1;
    case SCS_CONN_ACT_NONE: default: return 0;
    }
}

/* Deliver every queued control message to `dst` (whose CDT is `cdt`), looping
 * until the wire is quiet. Any packet the receiver's machine then owes is put
 * back on the wire for the other direction. */
static void deliver_all(struct node *dst, struct scs_cdt *cdt, struct ctrl_msg *out,
                        unsigned *out_n)
{
    for (unsigned i = 0; i < wire_depth; i++) {
        enum scs_conn_event ev;
        if (!wire[i].valid || !action_to_event(wire[i].action, &ev)) {
            continue;
        }
        struct scs_conn_transition t = scs_conn_fsm_step(cdt, ev);
        if (t.illegal) {
            CHECK(0, "%s: %s in state %s was illegal", dst->name,
                  scs_conn_event_name(ev), scs_conn_state_name(t.from));
        }
        if (t.action != SCS_CONN_ACT_NONE && *out_n < 8) {
            out[*out_n].valid = 1;
            out[*out_n].action = t.action;
            out[*out_n].src_conid = cdt->local_conid;
            out[*out_n].dst_conid = cdt->remote_conid;
            (*out_n)++;
        }
        (void)scs_svc_close_if_closed(&dst->port, cdt);
    }
    wire_reset();
}

static void put_wire(const struct ctrl_msg *src, unsigned n)
{
    wire_reset();
    for (unsigned i = 0; i < n && i < 8; i++) {
        wire[i] = src[i];
    }
    wire_depth = n;
}

static void test_two_nodes_form_use_and_tear_down_a_connection(void)
{
    struct node a, b;
    static const uint8_t pa[6] = {0x08, 0x00, 0x2b, 0x00, 0x00, 0x0a};
    static const uint8_t pb_[6] = {0x08, 0x00, 0x2b, 0x00, 0x00, 0x0b};
    node_init(&a, "A", 0x01);
    node_init(&b, "B", 0x02);
    node_open_circuit(&a, b.sysid, pb_);
    node_open_circuit(&b, a.sysid, pa);
    wire_reset();

    struct ctrl_msg back[8];
    unsigned back_n;

    /* --- LISTEN (p. 2-22). B declares itself willing. --- */
    CHECK(scs_listen(&b.port, "MSCP$DISK", NULL, NULL) == SCS_SVC_OK, "B LISTEN");
    CHECK(scs_svc_listening(&b.port, "MSCP$DISK"), "B is not listening");

    /* --- CONNECT (p. 2-22). A opens the connection, extending 4 message
     * buffers (p. 2-43) and asking for 2 datagram buffers (p. 2-42). --- */
    struct scs_svc_args aargs;
    memset(&aargs, 0, sizeof(aargs));
    aargs.local_sysap = "VMS$DISK_CL_DRVR";
    aargs.remote_sysap = "MSCP$DISK";
    aargs.target_node = b.sysid;
    aargs.cfg = &a.cfg;
    aargs.vc_loss = sysap_vc_loss;
    aargs.msg_input = sysap_msg_in;
    aargs.sysap_ctx = &a;
    aargs.send_credits = 4;
    aargs.min_send_credits = 1;
    aargs.dgram_buffers = 2;
    aargs.emit = node_emit;

    unsigned mfree_a_before = a.pdt.mfreeq_count;
    unsigned dfree_a_before = a.pdt.dfreeq_count;
    struct scs_cdt *acdt = NULL;
    CHECK(scs_connect(&a.port, &aargs, &acdt) == SCS_SVC_OK, "A CONNECT");
    CHECK(acdt != NULL, "A got no CDT");
    if (acdt == NULL) {
        return;
    }
    CHECK(a.pdt.mfreeq_count == mfree_a_before + 4,
          "CONNECT contributed %u message buffers to the MFREEQ, expected 4"
          " (p. 2-43)", a.pdt.mfreeq_count - mfree_a_before);
    CHECK(a.pdt.dfreeq_count == dfree_a_before + 2,
          "CONNECT contributed %u datagram buffers to the DFREEQ, expected 2"
          " (p. 2-42)", a.pdt.dfreeq_count - dfree_a_before);

    /* --- ACCEPT (p. 2-23). B's SYSAP says yes. --- */
    CHECK(wire_depth == 1, "A emitted %u message(s)", wire_depth);
    uint32_t a_conid = wire_depth ? wire[0].src_conid : 0;
    struct scs_svc_args bargs;
    memset(&bargs, 0, sizeof(bargs));
    bargs.local_sysap = "MSCP$DISK";
    bargs.remote_sysap = "VMS$DISK_CL_DRVR";
    bargs.vc = b.pb;
    bargs.vc_loss = sysap_vc_loss;
    bargs.msg_input = sysap_msg_in;
    bargs.sysap_ctx = &b;
    bargs.remote_conid = a_conid;
    bargs.send_credits = 8;
    bargs.min_send_credits = 1;
    bargs.emit = node_emit;

    wire_reset();
    struct scs_cdt *bcdt = NULL;
    CHECK(scs_accept(&b.port, &bargs, &bcdt) == SCS_SVC_OK, "B ACCEPT");
    CHECK(bcdt != NULL, "B got no CDT");
    if (bcdt == NULL) {
        return;
    }

    /* B's two frames reach A: CONNECT_RSP then ACCEPT_REQ. A answers with the
     * ACCEPT_RSP the machine requires, and reaches OPEN. */
    back_n = 0;
    deliver_all(&a, acdt, back, &back_n);
    CHECK(scs_conn_state_of(acdt) == SCS_CONN_OPEN,
          "A is %s after the accept, expected OPEN",
          scs_conn_state_name(scs_conn_state_of(acdt)));
    CHECK(back_n == 1 && back[0].action == SCS_CONN_ACT_SEND_ACCEPT_RSP,
          "A owed %u message(s) after the accept", back_n);

    /* The ACCEPT_RSP reaches B, which reaches OPEN too. */
    put_wire(back, back_n);
    back_n = 0;
    deliver_all(&b, bcdt, back, &back_n);
    CHECK(scs_conn_state_of(bcdt) == SCS_CONN_OPEN,
          "B is %s after the accept response, expected OPEN",
          scs_conn_state_name(scs_conn_state_of(bcdt)));

    /* Both ends know each other's handle -- the connection is bound. */
    scs_cdt_set_remote_conid(acdt, bcdt->local_conid);
    CHECK(acdt->remote_conid == bcdt->local_conid && bcdt->remote_conid == acdt->local_conid,
          "the CONID pair is not bound in both directions");

    /* --- USE IT. A sends B a message: p. 2-43 debits a Send Credit, and
     * p. 2-29 routes the packet to B's message input routine by DESTINATION
     * CONID. Both are production paths.
     *
     * THE ONE HAND-DRIVEN STEP, and it is named rather than hidden: p. 2-43
     * says the Send Credit count a node believes it has comes from the CREDIT
     * FIELD of the peer's ACCEPT_REQ. That field is a wire field, this test has
     * no wire, and OVMX has no builder that would carry it -- so B's grant of
     * the 8 credits it extended is applied here with the production
     * scs_credit_grant_from_peer(). It stands in for a frame field, not for a
     * code path: nothing here claims the daemon reads that field today (it does
     * not -- see scs_credit.h). */
    CHECK(scs_credit_grant_from_peer(acdt, 8) == 0, "the peer's credit grant was refused");
    CHECK(scs_credit_can_send(acdt), "A has no Send Credit to use the connection with");
    unsigned credit_before = acdt->send_credit;
    CHECK(scs_credit_on_send(acdt) == 0, "the send was refused for want of credit");
    CHECK(acdt->send_credit == credit_before - 1,
          "the send did not debit a Send Credit (%u -> %u)", credit_before,
          acdt->send_credit);
    CHECK(scs_cdl_deliver_message(&b.cdl, acdt->remote_conid, acdt->local_conid,
                                  "MSCP CMD", 8) == SCS_DELIVER_OK,
          "the message was not delivered to B");
    CHECK(b.msgs_in == 1, "B's SYSAP received %d message(s)", b.msgs_in);
    CHECK(strcmp(b.last_msg, "MSCP CMD") == 0, "B received '%s'", b.last_msg);

    /* --- TEAR IT DOWN. Figure 2-16, both ends invoking DISCONNECT. --- */
    struct scs_svc_args da = aargs;
    da.reason = 0x0007;
    da.emit = node_emit;
    wire_reset();
    CHECK(scs_disconnect(&a.port, acdt, &da) == SCS_SVC_OK, "A DISCONNECT");
    CHECK(scs_conn_state_of(acdt) == SCS_CONN_DISC_SENT, "A is not in DISC SENT");

    /* B receives the DISCONNECT_REQ: OPEN -> DISC RECEIVED, owing a
     * DISCONNECT_RSP. */
    back_n = 0;
    deliver_all(&b, bcdt, back, &back_n);
    CHECK(scs_conn_state_of(bcdt) == SCS_CONN_DISC_RECEIVED,
          "B is %s, expected DISC RECEIVED", scs_conn_state_name(scs_conn_state_of(bcdt)));
    CHECK(back_n == 1 && back[0].action == SCS_CONN_ACT_SEND_DISCONNECT_RSP,
          "B owed %u message(s)", back_n);

    /* A receives it: DISC SENT -> DISC ACK. */
    put_wire(back, back_n);
    back_n = 0;
    deliver_all(&a, acdt, back, &back_n);
    CHECK(scs_conn_state_of(acdt) == SCS_CONN_DISC_ACK,
          "A is %s, expected DISC ACK", scs_conn_state_name(scs_conn_state_of(acdt)));

    /* B's SYSAP now symmetrically invokes DISCONNECT (p. 2-26). */
    struct scs_svc_args db = bargs;
    db.reason = 0x0007;
    db.emit = node_emit;
    wire_reset();
    CHECK(scs_disconnect(&b.port, bcdt, &db) == SCS_SVC_OK, "B DISCONNECT");
    CHECK(scs_conn_state_of(bcdt) == SCS_CONN_DISC_MATCH,
          "B is %s, expected DISC MATCH", scs_conn_state_name(scs_conn_state_of(bcdt)));
    CHECK(bcdt->in_use, "B released its CDT before the dialogue finished");

    /* A receives the matching DISCONNECT_REQ: DISC ACK -> CLOSED, owing a
     * DISCONNECT_RSP, and A's CDT is released. */
    back_n = 0;
    deliver_all(&a, acdt, back, &back_n);
    CHECK(a.port.cdts_released == 1, "A released %lu CDT(s)", a.port.cdts_released);
    CHECK(scs_cdl_in_use_count(&a.cdl) == 0, "A's CDL still holds %u CDT(s)",
          scs_cdl_in_use_count(&a.cdl));
    CHECK(a.pdt.mfreeq_count == mfree_a_before,
          "A's MFREEQ did not come back to %u (it is %u) -- the p. 2-43 deposit"
          " leaked on release", mfree_a_before, a.pdt.mfreeq_count);
    CHECK(a.pdt.dfreeq_count == dfree_a_before,
          "A's DFREEQ did not come back to %u (it is %u)", dfree_a_before,
          a.pdt.dfreeq_count);
    CHECK(back_n == 1 && back[0].action == SCS_CONN_ACT_SEND_DISCONNECT_RSP,
          "A owed %u message(s) on close", back_n);

    /* B receives it: DISC MATCH -> CLOSED, and B's CDT is released too. */
    put_wire(back, back_n);
    back_n = 0;
    deliver_all(&b, bcdt, back, &back_n);
    CHECK(b.port.cdts_released == 1, "B released %lu CDT(s)", b.port.cdts_released);
    /* vms-7fe: what must go to zero is B's CONNECTION CDTs. The listening CDT
     * B's LISTEN allocated (p. 2-48) is still there and must be -- B is still
     * listening for MSCP$DISK after the connection tears down. */
    CHECK(scs_cdl_in_use_count(&b.cdl) == scs_sdir_count(scs_svc_sdir(&b.port)),
          "B's CDL holds %u CDT(s) beside its %u listening CDT(s)",
          scs_cdl_in_use_count(&b.cdl), scs_sdir_count(scs_svc_sdir(&b.port)));

    /* --- and the whole lifecycle scored no illegal event on either node. --- */
    CHECK(a.port.illegal == 0 && b.port.illegal == 0,
          "illegal events: A %lu, B %lu", a.port.illegal, b.port.illegal);
    /* A DISCONNECT is not a circuit loss: p. 2-28's error handler must NOT have
     * been called on either side. */
    CHECK(a.losses == 0 && b.losses == 0,
          "the VC-loss ERROR handler ran on a graceful disconnect (A %d, B %d)",
          a.losses, b.losses);
}

/* Null/degenerate arguments must be refused, not crash. */
static void test_null_safety(void)
{
    struct scs_svc_args args;
    memset(&args, 0, sizeof(args));
    CHECK(scs_connect(NULL, &args, NULL) == SCS_SVC_BADARG, "CONNECT NULL port");
    CHECK(scs_accept(NULL, &args, NULL) == SCS_SVC_BADARG, "ACCEPT NULL port");
    CHECK(scs_reject(NULL, &args) == SCS_SVC_BADARG, "REJECT NULL port");
    CHECK(scs_disconnect(NULL, NULL, &args) == SCS_SVC_BADARG, "DISCONNECT NULL port");

    struct node n;
    node_init(&n, "N", 0x09);
    CHECK(scs_connect(&n.port, NULL, NULL) == SCS_SVC_BADARG, "CONNECT NULL args");
    CHECK(scs_connect(&n.port, &args, NULL) == SCS_SVC_BADARG,
          "CONNECT named neither a circuit nor a node");
    CHECK(scs_disconnect(&n.port, NULL, &args) == SCS_SVC_BADARG, "DISCONNECT NULL cdt");
    CHECK(scs_svc_close_if_closed(&n.port, NULL) == 0, "close_if_closed NULL cdt");
    CHECK(strcmp(scs_svc_status_name(SCS_SVC_NOVC), "NOVC") == 0, "status name");
}

int main(void)
{
    test_listen_places_the_name_in_the_list();
    test_connect_and_accept_allocate_a_cdt();
    test_reject_allocates_no_cdt();
    test_connect_without_an_open_circuit_allocates_nothing();
    test_retransmit_reuses_the_connection();
    test_a_taken_conid_is_refused_but_still_sends();
    test_kill_switch_records_nothing_and_sends_everything();
    test_one_disconnect_does_not_release();
    test_two_nodes_form_use_and_tear_down_a_connection();
    test_null_safety();

    printf("test_scs_svc: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
