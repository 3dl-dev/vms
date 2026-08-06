/*
 * scs_config.h - SCA System Blocks, Path Blocks, Port Descriptor Tables and
 * the VMS SCA Configuration Queue (vms-7be).
 *
 * WHY THIS EXISTS. Before this module OVMX described a remote cluster node
 * with an ad-hoc `struct peer_state` keyed by the peer's Ethernet MAC. That is
 * not the architecture: SCA describes remote nodes and virtual circuits with
 * two architecturally defined structures -- the System Block (SB) and the Path
 * Block (PB) -- held in a configuration queue, with circuits still in formation
 * parked on a Port Descriptor Table (PDT). Every SCA rule about rejoin,
 * refresh and re-formation is written in terms of those structures, so a
 * MAC-keyed struct has nowhere to be legibly wrong. This module supplies them.
 *
 * SOURCE (public, quoted with page cites -- CLAUDE.md rule 8):
 *   Roy G. Davis, *VAXcluster Principles*, Digital Press 1993, ch. 2.
 *   - PB contents and the VC states                                pp. 2-11..2-12
 *   - SB contents (CPU/hw rev, OS name+version, 48-bit SCS System
 *     ID, SCS Node Name, 64-bit software incarnation number)             p. 2-16
 *   - "SCA requires that each node in a network maintain an SB for every node
 *     in the network. Consequently, a node must maintain an SB that describes
 *     its own CPU and operating system."                                 p. 2-16
 *   - The System List as queues: SBs for nodes with at least one open VC in a
 *     configuration queue, PBs for open circuits queued to their SB   pp. 2-17..2-19
 *   - Formative PBs queued to a PDT; the open-transition rules and the
 *     REFRESH Note                                                   pp. 2-20..2-21
 *
 * THIS MODULE IS PURE STATE. It builds no frame, parses no frame, and knows
 * nothing about any byte on the wire; it is a refactor of how OVMX REMEMBERS
 * peers, not of what OVMX SAYS to them (vms-7be is wire-invisible by
 * construction).
 *
 * OVMX DESIGN CHOICES (labeled per rule 8 -- the book documents the CONTENTS of
 * these structures, not any byte layout, and the PDT is explicitly "not an
 * architecturally defined data structure", p. 2-21):
 *   - Fixed-capacity pools instead of dynamic allocation, so the daemon has no
 *     allocation failure path (the pools are sized so every PB can hold its own
 *     formative SB). Sizes are compile-time knobs, not VMS quantities.
 *   - Enumerated port-type / port-state values. The book names the categories
 *     (CI port, HSC's CI port, "any type of Ethernet port"; ENABLED vs
 *     MAINTENANCE ENABLED) but publishes no numeric encoding, so these numbers
 *     are OVMX's own and never appear on the wire.
 *   - The remote port address is a 6-byte quantity here because OVMX's only
 *     interconnect is Ethernet, where the port address IS the MAC (p. 2-12:
 *     "the format of a port address is specific to the type of interconnect").
 *   - For NISCA, OVMX learns the remote node's 48-bit SCS System Address from
 *     the src-logical field of a HELLO (aa:00:04:00:<LE16(SCSSYSTEMID)>), i.e.
 *     at PORT DISCOVERY, which is earlier than the START/STACK the book
 *     describes (p. 2-12). The formative SB is therefore created at discovery
 *     and completed as later frames describe the node.
 *
 * NOT IN SCOPE HERE (deliberately, so the refactor stays wire-invisible):
 *   - The masquerade tests of the p. 2-21 footnote. LANDED SINCE, as vms-22e --
 *     see the "ANTI-MASQUERADE TESTS" block further down this file. That IS a
 *     wire-visible change (it can abandon formation) and ships a kill-switch.
 *   - SCS connections / CDTs, which are a layer ABOVE the virtual circuit
 *     (p. 2-11 Figure 2-6: many connections ride one VC). Landed since, as
 *     src/vmsscs/scs_cdt.{h,c} (vms-e1a); the only thing it added to THIS file
 *     is struct scs_pb's cdt_head, the p. 2-28 per-circuit connection queue.
 */
#ifndef SCS_CONFIG_H
#define SCS_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OVMX design choice: pool sizes. SB pool >= PB pool + 1 so that every PB can
 * own a formative SB and the local node can still hold its own SB (p. 2-16). */
#ifndef SCS_CONFIG_MAX_PB
#define SCS_CONFIG_MAX_PB 8
#endif
#ifndef SCS_CONFIG_MAX_SB
#define SCS_CONFIG_MAX_SB (SCS_CONFIG_MAX_PB + 2)
#endif

/* Remote port address width. Ethernet/NISCA: the 6-byte MAC (p. 2-12). */
#define SCS_PORT_ADDR_LEN 6
/* 48-bit SCS System ID / System Address (p. 2-16). */
#define SCS_SYSTEM_ID_LEN 6
/* SCSNODE is at most 6 characters on VMS. */
#define SCS_SB_NODENAME_LEN 6
/* OS name string ("VAX/VMS", "OVMX", ...) -- p. 2-16 "Name and version of the
 * operating system". Length is an OVMX choice; no wire field. */
#define SCS_SB_OSNAME_LEN 15

/*
 * Virtual-circuit states, p. 2-11 ("State of the virtual circuit between the
 * local port and the port at the other end ... OPEN, CLOSED, some stage of
 * formation") and the formation dialogue of pp. 2-12..2-14 (CLOSED -> START
 * SENT -> START RECEIVED -> OPEN). The numeric values are OVMX's own.
 */
enum scs_vc_state {
    SCS_VC_CLOSED = 0,
    SCS_VC_START_SENT = 1,
    SCS_VC_START_RECEIVED = 2,
    SCS_VC_OPEN = 3
};

/*
 * ===== vms-4071: the VC FORMATION dialogue as an explicit state machine =====
 *
 * The three packet classes of the formation dialogue (p. 2-12, Figure 2-7) and
 * the acceptability table of p. 2-14. The event/action enums live HERE, next to
 * enum scs_vc_state, because the Path Block carries the machine's state (the
 * machine itself is scs_vc.h/scs_vc.c -- this header stays pure state).
 *
 * The numeric values are OVMX's own; none of them reaches the wire.
 */
enum scs_vc_event {
    SCS_VC_EV_START = 0, /* START  (Start Virtual Circuit), p. 2-12 */
    SCS_VC_EV_STACK = 1, /* STACK  (Start Acknowledgment), p. 2-12 */
    SCS_VC_EV_ACK   = 2, /* ACK    (Acknowledgment), p. 2-12 */
    /*
     * Any other packet that presupposes an OPEN circuit. OVMX design choice
     * (labeled per rule 8): p. 2-16 says the implied ACK is triggered when "the
     * remote node performs an operation that requires the circuit to be OPEN,
     * and this operation results in a packet being sent to the local node". The
     * book is explicit (p. 2-40) that "the START, STACK, and ACK packets used in
     * virtual circuit formation are examples of datagrams" -- datagrams do NOT
     * require an open circuit -- so OVMX feeds this event ONLY for the SCS
     * sequenced-message classes, never for HELLOs or formation packets. See
     * scs_vc_is_circuit_packet().
     */
    SCS_VC_EV_OTHER = 3
};

/* What the machine tells the port driver to do next. */
enum scs_vc_action {
    SCS_VC_ACT_NONE = 0,
    SCS_VC_ACT_SEND_START = 1,
    SCS_VC_ACT_SEND_STACK = 2,
    SCS_VC_ACT_SEND_ACK = 3,
    SCS_VC_ACT_ABANDON = 4 /* p. 2-14: formation is abandoned */
};

/*
 * struct scs_vc_fsm - the retry/abandon bookkeeping of p. 2-14, carried in the
 * Path Block alongside vc_state.
 *
 * "whenever a port driver sends a START or a STACK to some other port driver
 * during virtual circuit formation, it starts a timer and expects a response.
 * If the timer expires before any response is received, or if the response it
 * receives is 'acceptable' but does not cause the circuit to advance to the
 * next state of formation, SCA requires the port driver to reissue the START or
 * STACK (whichever it last sent). If an 'unacceptable' response is received,
 * SCA requires that formation of the virtual circuit be abandoned." (p. 2-14)
 *
 * OVMX design choice (labeled): the timer is a caller-supplied monotonic
 * millisecond stamp rather than a real timer object, so every transition is
 * deterministic and unit-testable. `abandoned` is a flag rather than a new
 * enum scs_vc_state value: an abandoned circuit is not formed, so its state IS
 * CLOSED (p. 2-11); the flag records WHY it is closed.
 */
struct scs_vc_fsm {
    enum scs_vc_action last_emitted; /* SEND_START or SEND_STACK -- what a timeout reissues */
    int      timer_armed;
    uint64_t timer_ms;               /* monotonic ms at which last_emitted went out */
    unsigned retries;                /* reissues of the CURRENT START/STACK (p. 2-14) */
    int      abandoned;              /* formation abandoned (retry limit, unacceptable resp., or
                                      * vms-22e: a failed p. 2-21 masquerade test) */
    /* Observability only. */
    unsigned long reissues;          /* total reissues over this PB's life */
    unsigned long implied_acks;      /* p. 2-16 implied ACKs honored */
};

/* Remote port type (p. 2-11). OVMX values, not a wire encoding. */
enum scs_port_type {
    SCS_PORT_TYPE_UNKNOWN = 0,
    SCS_PORT_TYPE_CI = 1,      /* CI port common to VAX 11/78x and 86xx */
    SCS_PORT_TYPE_CI_HSC = 2,  /* CI port of an HSC storage controller */
    SCS_PORT_TYPE_ETHERNET = 3 /* any type of Ethernet port (OVMX's only kind) */
};

/* Remote port state (pp. 2-11..2-12): a VAX port's normal state is ENABLED, an
 * HSC's is MAINTENANCE ENABLED. OVMX values, not a wire encoding. */
enum scs_port_state {
    SCS_PORT_STATE_UNKNOWN = 0,
    SCS_PORT_STATE_ENABLED = 1,
    SCS_PORT_STATE_MAINT_ENABLED = 2
};

struct scs_pb;

/*
 * struct scs_sb - System Block: the local node's description of one node
 * (remote or itself). Contents per p. 2-16.
 */
struct scs_sb {
    int in_use;
    int formative; /* built for a circuit still in formation (p. 2-20) */

    uint16_t cpu_type;    /* CPU type of the described node (p. 2-16) */
    uint16_t hw_rev;      /* hardware revision level (p. 2-16) */
    char     os_name[SCS_SB_OSNAME_LEN + 1];   /* operating system name */
    uint16_t os_version;                        /* operating system version */
    uint8_t  system_id[SCS_SYSTEM_ID_LEN];      /* 48-bit SCS System ID (SCSSYSTEMID) */
    char     node_name[SCS_SB_NODENAME_LEN + 1];/* SCS Node Name (SCSNODE) */
    uint64_t incarnation; /* 64-bit software incarnation number (VMS: boot time) */

    /* Head of the queue of PBs for OPEN circuits with this node (p. 2-17). */
    struct scs_pb *pb_head;
    /* Configuration-queue links (p. 2-18 Figure 2-10 is a doubly-linked queue). */
    struct scs_sb *next;
    struct scs_sb *prev;
};

/*
 * struct scs_sb_info - the node description carried by a START/STACK, used to
 * build a formative SB and (per the p. 2-21 Note) to REFRESH an old one.
 */
struct scs_sb_info {
    uint16_t cpu_type;
    uint16_t hw_rev;
    const char *os_name;   /* NULL = leave unset */
    uint16_t os_version;
    uint8_t  system_id[SCS_SYSTEM_ID_LEN];
    const char *node_name; /* NULL = leave unset */
    uint64_t incarnation;
};

struct scs_pdt;
struct scs_cdt; /* vms-e1a: connections queued to this circuit (scs_cdt.h) */

/*
 * struct scs_pb - Path Block: describes one virtual circuit AND the port at the
 * far end of it (p. 2-11). A PB is queued either to a PDT (formative, p. 2-20)
 * or to the SB of the node it reaches (open, p. 2-17).
 */
struct scs_pb {
    int in_use;

    enum scs_vc_state  vc_state;          /* p. 2-11, first item */
    struct scs_vc_fsm  fsm;               /* vms-4071: formation timer/retry/abandon, p. 2-14 */
    enum scs_port_type remote_port_type;  /* p. 2-11 */
    enum scs_port_state remote_port_state;/* pp. 2-11..2-12 */
    uint8_t remote_port_addr[SCS_PORT_ADDR_LEN]; /* p. 2-12 */

    struct scs_sb  *sb;  /* the node this circuit reaches (formative or owning) */
    struct scs_pdt *pdt; /* the LOCAL port this circuit uses (always set) */

    int on_pdt; /* 1 = queued to pdt->formative_head; 0 = queued to sb->pb_head */

    /*
     * vms-e1a: head of the queue of Connection Descriptor Tables for the SCS
     * connections this virtual circuit supports. "SCA specifies that all CDTs
     * corresponding to connections supported by a virtual circuit be queued to
     * the Path Block corresponding to that circuit. If the circuit is broken
     * for any reason, it is then a relatively simple matter to scan this queue
     * to determine which connections have also been lost" (p. 2-28).
     *
     * POPULATED entirely by src/vmsscs/scs_cdt.c -- scs_config.c never inserts
     * into or removes from this queue and never dereferences a CDT. It does
     * READ the head pointer for exactly one purpose (vms-17f/vms-228):
     * scs_pb_close() REFUSES to close a Path Block whose connection queue is not
     * empty, returning SCS_PB_CLOSE_CONNECTIONS_QUEUED.
     *
     * vms-228: p. 2-28 gives the RATIONALE for queueing CDTs to the Path Block
     * (so a broken circuit's lost connections are easy to find), not an
     * ordering MANDATE. The refusal-to-close is an OVMX design choice: without
     * it, the p. 2-28 VC-loss scan (scs_cdl_vc_loss) could run AFTER the
     * circuit structure was gone, leaving the CDTs holding a dangling pb
     * pointer. That used to be a comment; a comment is not enforcement, so it
     * is now a refusal the caller cannot ignore. src/vmsscs/scs_depart.c is the
     * one function that performs the whole sequence in the documented order.
     */
    struct scs_cdt *cdt_head;

    /*
     * vms-22e: which p. 2-21 footnote test rejected this circuit's formative
     * System Block, or SCS_MASQ_PASS (0) if none has. Written only by
     * scs_pb_open; carried here so the port driver can log WHICH test failed
     * without re-running the comparison. Declared as int because
     * enum scs_masquerade_result is defined below this struct.
     */
    int masquerade_fail;

    struct scs_pb *next;
    struct scs_pb *prev;
};

/*
 * struct scs_pdt - Port Descriptor Table: one per LOCAL SCA port. Explicitly a
 * VMS implementation structure, not architecture (p. 2-21). Only the items the
 * book lists that OVMX can honestly fill are carried here.
 */
struct scs_pdt {
    enum scs_port_type port_type;    /* p. 2-21 "Type of port" */
    uint32_t max_xfer_bytes;         /* p. 2-21 max bytes per single operation */
    /* vms-631: p. 2-21 "counters of various port activities" names the CATEGORY
     * this module reserves space for -- these three fields are that reservation,
     * not an implementation of it. Nothing in src/vmsscs writes or reads any of
     * the three (grep confirms); they carry no test. Declared here rather than
     * omitted so a caller that DOES wire up counting has somewhere documented to
     * put it, but until that happens a green test run proves nothing about them. */
    unsigned long frames_sent;       /* UNPOPULATED -- see note above */
    unsigned long frames_received;   /* UNPOPULATED -- see note above */
    unsigned long errors;            /* UNPOPULATED -- see note above */
    struct scs_pb *formative_head;   /* p. 2-21 head of the formative-PB queue */

    /*
     * vms-76e: depth of this port's Message Free Queue. "SCA associates a
     * Message Free Queue with each port. All free message buffers for
     * connections supported by a particular port are kept in the MFREEQ
     * associated with that port." (p. 2-45) VMS keeps the MFREEQ head in the
     * Port Queue Block portion of the PDT; p. 2-45 explicitly permits the
     * port/MFREEQ association to be implementation dependent, and OVMX has no
     * port buffer pool to queue, so this accounts the DEPTH only (OVMX design
     * choice, labeled per rule 8).
     *
     * Owned entirely by src/vmsscs/scs_credit.c -- scs_config.c never reads or
     * writes it, exactly as it never touches scs_pb.cdt_head.
     */
    unsigned mfreeq_count;

    /*
     * vms-b1d: depth of this port's Datagram Free Queue, the DFREEQ. "SCA
     * associates a separate DFREEQ with each port. ... The datagram buffers
     * allocated for a connection are inserted into the DFREEQ for the port
     * that supports that connection." (p. 2-43) VMS keeps the DFREEQ head in
     * the same Port Queue Block portion of the PDT as the MFREEQ head, and
     * p. 2-45 permits that association to be implementation dependent -- so,
     * exactly as for mfreeq_count, OVMX accounts the DEPTH only (OVMX design
     * choice, labeled per rule 8).
     *
     * `dfreeq_empty_discards` counts the p. 2-42 port-level drop: "The
     * possibility exists that the DFREEQ itself is empty when the port
     * receives a datagram. If this is the case, the port merely discards the
     * datagram." That discard is silent on the wire by design; counting it
     * locally is what keeps it from also being invisible (INV-6). It is a
     * PORT counter, not a connection counter, because at that point in the
     * receive path SCS has not yet looked up a CDT.
     *
     * Owned entirely by src/vmsscs/scs_dgram.c -- scs_config.c never reads or
     * writes either field, exactly as it never touches mfreeq_count.
     */
    unsigned      dfreeq_count;
    unsigned long dfreeq_empty_discards;
};

/*
 * struct scs_config - the VMS SCA Configuration Queue (p. 2-17) plus the pools
 * the SBs and PBs are carved from (OVMX design choice, see the header note).
 */
struct scs_config {
    struct scs_sb *sb_head; /* head of the configuration queue */
    struct scs_sb  sb_pool[SCS_CONFIG_MAX_SB];
    struct scs_pb  pb_pool[SCS_CONFIG_MAX_PB];
};

/*
 * ===== REACHABILITY IN SCSD TODAY -- DO NOT READ A GREEN TEST RUN AS "OVMX
 * IMPLEMENTS THIS" =====
 *
 * This module implements the p. 2-20..2-21 STATE-TRANSITION rule set (the SB/PB
 * open/refresh/masquerade rules) and tests/vmsscs/test_scs_config.c exercises
 * all of it. vms-631: it does NOT implement the p. 2-21 "counters of various
 * port activities" -- struct scs_pdt's frames_sent/frames_received/errors
 * fields are declared but unpopulated (see the note at their declaration) --
 * so "implements the full p. 2-20..2-21 rule set" was an overclaim that
 * folded a reserved-but-unwired field group into the same sentence as the
 * transition rules, which really are fully covered. The DAEMON
 * (src/vmsscs/scsd.c) reaches only part of the transition rules:
 *
 *   scs_pb_create / scs_pb_learn_system_addr / scs_pb_open -> SCS_OPEN_NEW_SB
 *       LIVE. Every peer SCSD discovers takes exactly this path.
 *
 *   scs_pb_open -> SCS_OPEN_EXISTING_REFRESHED   (the p. 2-21 rejoin REFRESH)
 *   scs_pb_close
 *       LIVE AS OF vms-17f. Until that item both were structurally unreachable:
 *       SCSD never closed a Path Block, never freed a peer slot, and gated
 *       scs_pb_open behind a !start_acked flag it never reset, so a rejoining
 *       node re-entered the same slot on the same already-open PB. scsd.c now
 *       runs a departure sweep (scsd_peer_departure_sweep) that, when a peer has
 *       been silent past the listen timeout, calls scs_pb_depart() -- the p. 2-28
 *       ordered teardown in src/vmsscs/scs_depart.c -- and releases the peer
 *       slot. The returning node then builds a NEW formative PB and its
 *       scs_pb_open finds the old SB with an empty PB queue, which is exactly the
 *       p. 2-21 Note. tests/vmsscs/test_scsd_wire.c drives that whole sequence
 *       through the daemon's own receive dispatch with captured frames.
 *       This is WIRE-VISIBLE (a returning peer now gets a fresh formation
 *       dialogue instead of silence); OVMX_NO_PEER_DEPART=1 is the kill switch.
 *
 *   scs_pb_open -> SCS_OPEN_EXISTING_SB
 *       UNEXERCISED, not impossible: it needs a peer node presenting two Ethernet
 *       ports (two MACs, one SCS System ID). The reference lab has never presented
 *       one. Note the consequence, which tests/vmsscs/test_scsd_wire.c pins: after
 *       this transition both peers' Path Blocks share ONE System Block, so the
 *       System Address SCSD writes in the SCA peer-logical field is shared state
 *       rather than the per-peer copy it was before vms-7be.
 *
 * Consequence, stated plainly (vms-17f revision): OVMX now implements the rejoin
 * refresh rule AND reaches it from the daemon. What it still does NOT have is a
 * second interconnect, so SCS_OPEN_EXISTING_SB stays unexercised by scsd.c --
 * see the note above it.
 */

/* Result of scs_pb_open (pp. 2-20..2-21). */
enum scs_open_result {
    SCS_OPEN_ERROR = -1,
    SCS_OPEN_NEW_SB = 0,          /* node learned for the first time: SB inserted */
    SCS_OPEN_EXISTING_SB = 1,     /* SB already present, other PBs on it: no refresh */
    SCS_OPEN_EXISTING_REFRESHED = 2, /* SB already present with NO other PB: refreshed */
    /* vms-22e: a p. 2-21 footnote masquerade test failed. NO queue move happened,
     * the circuit is NOT open, and pb->masquerade_fail says which test failed. */
    SCS_OPEN_ABANDONED_MASQUERADE = 3
};

/*
 * ===== ANTI-MASQUERADE TESTS (vms-22e, p. 2-21 footnote) ===================
 *
 * The footnote hangs off the "a System Block corresponding to the remote node is
 * already present in the Configuration Queue" bullet on p. 2-21:
 *
 *   "At this time, special tests are made to ensure that the remote node is not
 *    masquerading as a node already known to the local system. For example, if
 *    the SCS System ID in the formative System Block matches the SCS System ID
 *    in a System Block already in the Configuration Queue, the SCS Node Names
 *    must also match. The converse is also true. If both items match, and if
 *    there is a Path Block already queued to the System Block in the
 *    Configuration Queue, then the 64-bit incarnation numbers must also match.
 *    Virtual circuit formation is abandoned if any of these tests fail."
 *
 * WHY THIS IS NOT A REJOIN FIX. This rule was tested as a hypothesis for the
 * vms-2f3 rejoin failure and REFUTED twice (docs/HANDOFF-vms-2f3.md sec 4M.31):
 * a run presenting the incarnation the peer had already recorded was refused
 * anyway, and independently, the virtual circuit demonstrably FORMS on every
 * refused rejoin (connections open over it), which a failed masquerade test
 * would prevent. This module implements the rule because SCA specifies it, and
 * for no other reason. It fixes nothing.
 *
 * WHEN THE TESTS RUN -- a LABELED READING, not a quotation. The footnote's
 * example is written in the ID-matches direction, but "the converse is also
 * true" describes a formative SB whose NODE NAME matches a queued SB while its
 * System ID does not -- and that case lands in the OTHER p. 2-21 branch
 * ("no other System Block corresponding to the remote node ... learned for the
 * first time"), because the System-ID lookup misses. Running the tests only on
 * the ID-match branch would therefore make the converse clause unreachable. So
 * OVMX runs the whole test set at the single open transition, scanning the
 * entire Configuration Queue, before any queue move. That is a reading of the
 * footnote, not a documented VMS implementation detail.
 *
 * UNKNOWN INPUTS DO NOT FAIL A NODE (INV-6 spirit: never invent). A test whose
 * inputs are not both populated is INDETERMINATE and cannot abandon formation:
 *   - node name: skipped unless BOTH System Blocks carry a non-empty SCS Node
 *     Name. The 0x41 START parser (scs_start_parse / struct scs_start_view) does
 *     not extract the peer's node name today, so no SCSD-built System Block
 *     carries one -- see the REACHABILITY note below.
 *   - incarnation: skipped unless BOTH 64-bit incarnation numbers are non-zero.
 *     vms-7be deliberately left this field unpopulated rather than inventing a
 *     value, and the START parser still does not supply it, so THE INCARNATION
 *     COMPARISON IS UNREACHABLE IN THE DAEMON TODAY. It is implemented and unit
 *     tested so that it is correct the moment the parser supplies the field.
 *     Recorded as a gap in docs/cluster-protocol-spec.md sec 5.
 *
 * REACHABILITY IN SCSD TODAY, stated plainly so a green test run is not misread:
 * scs_pb_open() is reached from scsd.c on every join, so the SCAN runs live --
 * but a comparison needs BOTH sides populated and the formative side never is.
 * Everything SCSD learns about a REMOTE node is its 48-bit System Address
 * (scs_pb_learn_system_addr); node_name is empty and incarnation is 0 on every
 * remote System Block it builds. (SCSD does fill in a node name on its OWN
 * System Block -- see scs_config_insert_sb in scsd.c's main() -- but that is the
 * queued side of the comparison, not the formative one.) All three comparisons
 * are therefore INDETERMINATE in production and OVMX has never abandoned a
 * circuit for masquerade on the wire. THIS PARAGRAPH IS ITSELF ASSERTED, not
 * merely written: test_masquerade_unknown_fields_do_not_convict() in
 * tests/vmsscs/test_scs_config.c and step 0 of
 * test_masquerade_open_is_logged_and_suppresses_vcopen() in
 * tests/vmsscs/test_scsd_wire.c both build a peer the way the receive path does
 * and RED if a node name or an incarnation ever appears on it.
 *
 * The rule itself is exercised by tests/vmsscs/test_scs_config.c against System
 * Blocks whose fields ARE populated -- including against a System Block that is
 * NOT the head of the Configuration Queue (so the queue walk is covered, not
 * just the head), and with the incarnation comparison driven in BOTH directions
 * (so an ordered compare cannot pass for an equality). What the DAEMON does with
 * an abandonment -- log which test failed, and print no STARTDONE/VCOPEN for a
 * circuit it refused -- is exercised by
 * test_masquerade_open_is_logged_and_suppresses_vcopen() in test_scsd_wire.c.
 * Do not read "the tests are implemented and tested" as "OVMX currently detects
 * masqueraders".
 *
 * KILL-SWITCH: OVMX_NO_MASQUERADE_TESTS=1 makes scs_config_masquerade_check()
 * return SCS_MASQ_PASS unconditionally, restoring the pre-vms-22e open
 * transition exactly. Re-read from the environment on every call, so a test can
 * bracket it.
 */
enum scs_masquerade_result {
    SCS_MASQ_PASS = 0,
    /* System IDs match a queued SB, SCS Node Names differ. */
    SCS_MASQ_FAIL_NODE_NAME = 1,
    /* The converse: SCS Node Names match a queued SB, System IDs differ. */
    SCS_MASQ_FAIL_SYSTEM_ID = 2,
    /* Both match, a Path Block is already queued to that SB, incarnations differ. */
    SCS_MASQ_FAIL_INCARNATION = 3
};

/* 1 unless OVMX_NO_MASQUERADE_TESTS=1. Read fresh from the environment. */
int scs_masquerade_tests_enabled(void);

/* Short name of a masquerade result, for logging. Never NULL. */
const char *scs_masquerade_result_name(enum scs_masquerade_result r);

/*
 * scs_config_masquerade_check - run the p. 2-21 footnote tests for `formative`
 * against every System Block in the Configuration Queue. `formative` itself is
 * skipped if it is somehow queued. Returns the FIRST failing test, or
 * SCS_MASQ_PASS if all tests pass or are indeterminate (see the block above).
 * Returns SCS_MASQ_PASS on a NULL argument and when the kill-switch is set.
 *
 * scs_pb_open() calls this itself -- a caller cannot skip it. It is public so
 * the rule can be unit tested directly and so a port driver can pre-check.
 */
enum scs_masquerade_result scs_config_masquerade_check(const struct scs_config *cfg,
                                                       const struct scs_sb *formative);

/* --- lifecycle ------------------------------------------------------------ */

/* Zero a configuration queue and its pools. No-op if cfg is NULL. */
void scs_config_init(struct scs_config *cfg);

/* Initialize a PDT for one local port (empty formative queue). */
void scs_pdt_init(struct scs_pdt *pdt, enum scs_port_type port_type,
                  uint32_t max_xfer_bytes);

/* --- path blocks ---------------------------------------------------------- */

/*
 * scs_pb_create - a local port driver has DISCOVERED a remote port (p. 2-11
 * item 1). Allocate a Path Block describing that port and the circuit forming
 * with it, mark the VC state CLOSED ("When the PB is first initialized, it is
 * marked to indicate that the state of the virtual circuit is CLOSED", p. 2-11)
 * and queue it to the PDT of the local port as a FORMATIVE PB (p. 2-20).
 * Returns NULL if any argument is NULL or the PB pool is exhausted.
 */
struct scs_pb *scs_pb_create(struct scs_config *cfg, struct scs_pdt *pdt,
                             const uint8_t remote_port_addr[SCS_PORT_ADDR_LEN],
                             enum scs_port_type remote_port_type);

/* Set the VC state of a PB (the formation dialogue, pp. 2-12..2-14). Does NOT
 * itself perform the open-transition queue moves -- call scs_pb_open for that. */
void scs_pb_set_vc_state(struct scs_pb *pb, enum scs_vc_state state);

/*
 * scs_pb_attach_formative_sb - a START (or STACK) arrived from the remote node:
 * build the SB describing it and place its address in the Path Block (p. 2-20).
 * If the PB already has a formative SB, that SB is UPDATED in place instead (a
 * retransmitted START carries the same description). If the PB is already open
 * and queued to a non-formative SB, this is a no-op returning that SB.
 * Returns the SB, or NULL on error / SB-pool exhaustion.
 */
struct scs_sb *scs_pb_attach_formative_sb(struct scs_config *cfg, struct scs_pb *pb,
                                          const struct scs_sb_info *info);

/*
 * scs_pb_learn_system_addr - record the remote node's 48-bit SCS System Address
 * (p. 2-16) learned at PORT DISCOVERY rather than from a START.
 *
 * OVMX design choice (labeled per rule 8): on NISCA the remote node's System
 * Address rides the src-logical field of every HELLO
 * (aa:00:04:00:<LE16(SCSSYSTEMID)>), so OVMX knows WHICH node a port belongs to
 * before the START dialogue of p. 2-12 has said anything. This creates the
 * formative SB with only its System Address filled in if the PB has none, or
 * updates the address in place otherwise; the remaining SB contents are filled
 * by scs_pb_attach_formative_sb when a frame actually describes the node.
 * Returns the SB, or NULL on error / SB-pool exhaustion.
 */
struct scs_sb *scs_pb_learn_system_addr(struct scs_config *cfg, struct scs_pb *pb,
                                        const uint8_t system_id[SCS_SYSTEM_ID_LEN]);

/*
 * scs_pb_open - the circuit reached OPEN; perform the p. 2-21 transition.
 *
 *  - No SB for this node in the configuration queue: dequeue the PB from the
 *    PDT, insert the SB into the configuration queue, queue the PB to the SB.
 *    Neither is "formative" any more.               -> SCS_OPEN_NEW_SB
 *  - An SB for this node is already in the queue: queue the formative PB to the
 *    OLD SB and discard the formative SB.           -> SCS_OPEN_EXISTING_SB
 *    THE NOTE (p. 2-21): "If there are no other Path Blocks queued to the old
 *    System Block ... the old System Block is refreshed based on the contents of
 *    the formative System Block before the formative System Block is discarded.
 *    Typically, this happens when the remote node was once in the cluster,
 *    departed, and is now rebooting."               -> SCS_OPEN_EXISTING_REFRESHED
 *
 * Node identity for the lookup is the 48-bit SCS System ID (p. 2-16). Sets the
 * PB's VC state to OPEN. A PB that is already open is left alone
 * (SCS_OPEN_EXISTING_SB). Returns SCS_OPEN_ERROR if pb has no SB attached.
 *
 * vms-22e: BEFORE any queue move, the p. 2-21 footnote masquerade tests run
 * (scs_config_masquerade_check). If one fails, formation is ABANDONED: no queue
 * move happens, the PB stays formative on its PDT with vc_state CLOSED and
 * fsm.abandoned raised, pb->masquerade_fail records which test failed, and
 * SCS_OPEN_ABANDONED_MASQUERADE is returned. Tearing the Path Block down after
 * that is the port driver's business (vms-17f), not this function's.
 */
enum scs_open_result scs_pb_open(struct scs_config *cfg, struct scs_pb *pb);

/* Result of scs_pb_close (vms-17f/vms-228). */
enum scs_pb_close_result {
    SCS_PB_CLOSE_OK = 0,      /* the PB was dequeued and returned to the pool */
    SCS_PB_CLOSE_NOTHING = 1, /* NULL argument, or a PB that was not in use */
    /* REFUSED: connections are still queued to this circuit. p. 2-28 explains
     * WHY the VC-loss scan needs to notify their SYSAPs before the circuit
     * structure goes away; refusing the close until that happens is an OVMX
     * design choice (vms-228), not a mandate the book states. Zeroing the PB
     * here would leave every one of those CDTs holding a dangling pb pointer.
     * Run scs_cdl_vc_loss() + release the CDTs first -- or just call
     * scs_pb_depart() (scs_depart.h), which does it in order. */
    SCS_PB_CLOSE_CONNECTIONS_QUEUED = 2
};

/*
 * scs_pb_close - the circuit went away. Dequeues the PB from whichever queue
 * holds it and returns it to the pool. The SB stays in the configuration queue
 * even with no PBs left: VMS "keeps in a queue the System Blocks for nodes with
 * which it has had at least one open virtual circuit" (p. 2-17) -- that is what
 * makes the p. 2-21 refresh case reachable on a rejoin.
 *
 * REFUSES (and changes nothing) when connections are still queued to the PB --
 * see SCS_PB_CLOSE_CONNECTIONS_QUEUED above. This module cannot notify them
 * itself: the CDT lives one layer up (scs_cdt.c depends on this file, not the
 * other way round), so the refusal is how the ordering is ENFORCED rather than
 * merely documented.
 */
enum scs_pb_close_result scs_pb_close(struct scs_config *cfg, struct scs_pb *pb);

/* --- lookups -------------------------------------------------------------- */

/* Find the PB for a remote port address on a given local port (PDT). Searches
 * the PDT's formative queue AND the open PBs queued to SBs. NULL if unknown.
 * Pass pdt=NULL to search every local port. */
struct scs_pb *scs_config_find_pb(struct scs_config *cfg, const struct scs_pdt *pdt,
                                  const uint8_t remote_port_addr[SCS_PORT_ADDR_LEN]);

/* Find a (non-formative) SB in the configuration queue by 48-bit SCS System ID. */
struct scs_sb *scs_config_find_sb(struct scs_config *cfg,
                                  const uint8_t system_id[SCS_SYSTEM_ID_LEN]);

/*
 * scs_config_insert_sb - insert a fully-known, non-formative SB directly into
 * the configuration queue. Used for the LOCAL node's own SB: "a node must
 * maintain an SB that describes its own CPU and operating system" (p. 2-16),
 * and Figure 2-10 shows VAX_1's own SB in its configuration queue (p. 2-18).
 * Returns the existing SB if one with that System ID is already queued.
 */
struct scs_sb *scs_config_insert_sb(struct scs_config *cfg, const struct scs_sb_info *info);

/* Number of SBs in the configuration queue. */
unsigned scs_config_sb_count(const struct scs_config *cfg);

/* Number of open-circuit PBs queued to an SB (p. 2-17); 0 for NULL. */
unsigned scs_sb_pb_count(const struct scs_sb *sb);

/* Number of formative PBs queued to a PDT (p. 2-21); 0 for NULL. */
unsigned scs_pdt_formative_count(const struct scs_pdt *pdt);

/* Apply an scs_sb_info to an SB (the p. 2-21 "refresh"); no-op on NULL. */
void scs_sb_apply_info(struct scs_sb *sb, const struct scs_sb_info *info);

/* Human-readable VC state name, for logging. Never NULL. */
const char *scs_vc_state_name(enum scs_vc_state state);

/*
 * ===== CONFIG_SYS / CONFIG_PATH (vms-398, p. 2-47) =========================
 *
 * "CONFIG_SYS ... given the SCS System ID of a node, returns ... the SCS
 * System ID, the maximum datagram and message sizes, the type and version of
 * software running on the node, the SCS Node Name, and the address of the
 * first Path Block in the queue of Path Blocks for the node" and "CONFIG_PATH
 * ... given the address of a Path Block, returns ... the state of the
 * virtual circuit, the type and state of the remote port, the address of the
 * System Block ... and the address of the next Path Block in the queue"
 * (p. 2-47). These are READ-ONLY queries over the SB/PB structures above --
 * this module adds no new state and performs no wire I/O.
 *
 * "Address of a Path Block" (p. 2-47) is an OVMX design choice here: a
 * `struct scs_pb *` IS the identifier, exactly as every other lookup function
 * in this header already returns/accepts SB and PB pointers as identifiers
 * (scs_config_find_sb, scs_config_find_pb, pb->sb, pb->next, ...). No new
 * opaque-handle type is introduced.
 *
 * REACHABILITY (measured, not asserted -- the claim above this block about the
 * rest of the module applies here too):
 *   scs_config_select_vc / scs_config_sys
 *       LIVE. src/vmsscs/scsd.c's send_joiner_connect_request() takes either a
 *       named circuit or none, and BOTH of its call sites in the daemon pass
 *       none -- so every joiner CONNECT-REQUEST OVMX transmits picks its
 *       virtual circuit through scs_config_select_vc(), which is CONFIG_SYS
 *       plus the p. 2-47 OPEN scan. If no circuit is OPEN, the daemon logs
 *       SCSD-E-NOVC and sends NOTHING (rule 9 / INV-6: no invented circuit).
 *       tests/vmsscs/test_scsd_wire.c drives that production sender.
 *   scs_config_path
 *       LIVE, on the same path twice over: the OPEN scan inside
 *       scs_config_select_vc() examines each Path Block through it, and the
 *       daemon reads the chosen circuit's remote port address and System Block
 *       back through it to build the frame.
 *
 * HONESTY ON UNPOPULATED FIELDS (do not invent a value, CLAUDE.md rule 8 /
 * INV-6 spirit): scs_sb has NEVER had a max-datagram-size or max-message-size
 * field -- grep src/vmsscs for "datagram" before this file and there is
 * nothing. CONFIG_SYS reports these as 0 with their `have` bit UNSET,
 * always, today. Software type/version and SCS Node Name DO exist as SB
 * fields (os_name/os_version/node_name), but the live daemon has no call path
 * that ever fills them for a REMOTE node: scs_pb_attach_formative_sb (the only
 * function that sets them) is called only from tests, never from scsd.c (see
 * the REACHABILITY note above this comment in the file). scs_config_sys()
 * therefore sets each field's `have` bit from what the SB struct ACTUALLY
 * holds at query time (nonempty string / nonzero version), not from a
 * hardcoded assumption -- so a remote node's software_type/version/node_name
 * read back as empty/0 with `have` unset in every configuration scsd.c builds
 * today, and would read back populated the moment attach_formative_sb gets a
 * real caller (tracked separately, vms-17f is the closest existing item for
 * wiring gaps of this kind; this module does not fix that wiring).
 * test_config_sys_does_not_invent_unknown_fields() queries an SB built the way
 * scsd.c builds one and asserts all three read back ABSENT, so the "do not
 * invent" rule is executable rather than a comment.
 */
enum scs_config_sys_have {
    SCS_CONFIG_SYS_HAVE_MAX_DATAGRAM     = 1u << 0, /* NEVER set: no such SB field exists (see above) */
    SCS_CONFIG_SYS_HAVE_MAX_MESSAGE      = 1u << 1, /* NEVER set: no such SB field exists (see above) */
    SCS_CONFIG_SYS_HAVE_SOFTWARE_TYPE    = 1u << 2, /* set iff sb->os_name[0] != '\0' */
    SCS_CONFIG_SYS_HAVE_SOFTWARE_VERSION = 1u << 3, /* set iff sb->os_version != 0 */
    SCS_CONFIG_SYS_HAVE_NODE_NAME        = 1u << 4  /* set iff sb->node_name[0] != '\0' */
};

/* CONFIG_SYS result (p. 2-47). Zeroed and `have` == 0 when the System ID is
 * not found; `first_pb` is the Path Block identifier of the first PB queued
 * to the SB (NULL if the SB has none), i.e. sb->pb_head. */
struct scs_config_sys_info {
    uint8_t  system_id[SCS_SYSTEM_ID_LEN];      /* echoes the input System ID */
    uint16_t max_datagram_size;                  /* always 0 -- HAVE_MAX_DATAGRAM never set */
    uint16_t max_message_size;                   /* always 0 -- HAVE_MAX_MESSAGE never set */
    char     software_type[SCS_SB_OSNAME_LEN + 1];    /* SB os_name, if HAVE_SOFTWARE_TYPE */
    uint16_t software_version;                         /* SB os_version, if HAVE_SOFTWARE_VERSION */
    char     node_name[SCS_SB_NODENAME_LEN + 1];      /* SB node_name, if HAVE_NODE_NAME */
    struct scs_pb *first_pb;                     /* identifier of the first queued PB, or NULL */
    unsigned have;                                /* bitmask of enum scs_config_sys_have */
};

/*
 * scs_config_sys - CONFIG_SYS query (p. 2-47): find the System Block matching
 * `system_id` in the configuration queue (a formative SB does NOT match --
 * CONFIG_SYS queries the System List, p. 2-17, not in-formation state) and
 * fill *out. Returns 1 if found, 0 if not found or on a NULL argument (out is
 * always zeroed first when out != NULL).
 */
int scs_config_sys(struct scs_config *cfg, const uint8_t system_id[SCS_SYSTEM_ID_LEN],
                   struct scs_config_sys_info *out);

/* CONFIG_PATH result (p. 2-47): the PB's own state plus the identifiers of
 * the SB it is queued to and the next PB in whichever queue holds it (the
 * PDT's formative queue or an SB's open-circuit queue -- CONFIG_PATH does not
 * distinguish the two per the book; `on_formative_queue` says which one). */
struct scs_config_path_info {
    enum scs_vc_state   vc_state;
    enum scs_port_type  remote_port_type;
    enum scs_port_state remote_port_state;
    uint8_t  remote_port_addr[SCS_PORT_ADDR_LEN];
    struct scs_sb *sb;              /* SB this PB is queued to (formative or owning), or NULL */
    struct scs_pb *next;            /* next PB in the same queue, or NULL if last */
    int on_formative_queue;         /* 1 if queued to a PDT (still forming), 0 if queued to an SB */
};

/*
 * scs_config_path - CONFIG_PATH query (p. 2-47): given a Path Block
 * identifier, fill *out with its queryable state. Returns 1 on success, 0 if
 * pb/out is NULL or pb is not a live (in_use) Path Block (out is always
 * zeroed first when out != NULL).
 */
int scs_config_path(const struct scs_pb *pb, struct scs_config_path_info *out);

/*
 * scs_config_select_vc - the CONNECT algorithm's "caller did not name a
 * circuit" path (p. 2-47): "CONNECT ... uses CONFIG_SYS to obtain the address
 * of the first Path Block queued to the System Block for the specified node
 * ... [and] examines each Path Block in turn until it finds one whose virtual
 * circuit is OPEN". Looks up the SB by `system_id` (the CONFIG_SYS step) and
 * examines its PB queue through scs_config_path() for the first PB with
 * vc_state == SCS_VC_OPEN. This is the selection scsd.c's joiner
 * CONNECT-REQUEST performs on every join (see the REACHABILITY note above);
 * a NULL return means CONNECT sends nothing, it does not fall back.
 *
 * OVMX DESIGN CHOICE / DOCUMENTED OMISSION (labeled per rule 8, vms-398
 * constraint): the book also describes a circuit-type preference (e.g. CI
 * over Ethernet) for a node reachable over more than one interconnect
 * (p. 2-47). OVMX's only production interconnect is Ethernet -- scsd.c never
 * opens a second circuit type to the same node (grep confirms no caller drives
 * a mixed-type multi-circuit node in production) -- so that preference has
 * nothing to choose between and is NOT implemented here. This function simply
 * returns the first OPEN PB found scanning the queue from sb->pb_head; if
 * OVMX ever grows a second interconnect type, this is where the preference
 * order would need to be added.
 *
 * Returns the chosen PB, or NULL if the System ID is unknown or none of its
 * queued PBs has vc_state == SCS_VC_OPEN.
 */
struct scs_pb *scs_config_select_vc(struct scs_config *cfg,
                                    const uint8_t system_id[SCS_SYSTEM_ID_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* SCS_CONFIG_H */
