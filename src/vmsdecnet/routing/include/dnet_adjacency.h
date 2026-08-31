/*
 * dnet_adjacency.h - DECnet Phase IV routing-layer ADJACENCY state machine
 *                    (rung 3 of the DECnet lane, rd vms-b15 / vms-851 /
 *                    epic vms-30e).
 *
 * Consumes the rung-1 Ethernet Endnode Hello codec (dnet_hello.{c,h}) and
 * drives, per neighbour, the Phase IV routing *adjacency* lifecycle:
 *
 *        (no hello / listen timer expiry)
 *     +--------------------------------------------------+
 *     v                                                  |
 *   DOWN --first valid HELLO--> INITIALIZING --two-way--> UP
 *     ^                            |  ^                    |
 *     |                            |  +--one-way HELLO-----+ (stays alive,
 *     +---------listen timer T4 expires------------------+   kept by T4)
 *
 * plus our OWN periodic endnode-HELLO emission cadence on the hello timer T3.
 *
 * This module is a PURE, deterministic state machine + timer engine: no
 * socket, no thread, no allocation, no wall clock, no sleep. The caller
 * injects a monotonic tick (`dnet_tick_t`, conventionally seconds) into every
 * entry point, so the whole lifecycle is unit-testable deterministically and
 * the module serves EITHER engine boundary (the forward-ported in-kernel
 * net/decnet adjacency, or the design sec-4b userspace AF_PACKET fallback) --
 * the same engine-agnostic rationale the rung-1/2 codecs carry
 * (docs/design-decnet-ovmx.md sec 4b: "the L3-L6 VMS surface is identical
 * either way -- only the engine boundary moves"). The socket/datalink driver
 * that actually clocks this SM and puts HELLOs on the wire is a later rung
 * (the AF_PACKET family, rd vms-a1c) and is deliberately NOT built here.
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md Rule 8, HARD; docs/decnet-provenance-
 * register.md).  The state/timer semantics below are derived ONLY from:
 *   (a) the public DEC Digital Network Architecture (DNA) Phase IV Routing
 *       Layer Functional Specification (adjacency up/down, the two-way
 *       reachability handshake, and the listen/holding timer = BCT3MULT x the
 *       neighbour's advertised hello timer), and
 *   (b) the lab-oracle wire specimen captured under rd vms-3be
 *       (docs/decnet-provenance-register.md sec 4.6): the captured endnode
 *       HELLO advertises "hello 15" -- i.e. T3 = 15 s -- which is the ONLY
 *       timer value observed from real VAX wire and is used as the T3 default
 *       here. See the constants below for exactly which value is oracle-
 *       captured and which is spec-derived.
 * No VSI/HPE/DEC source or binary was disassembled, decompiled, or copied.
 */
#ifndef DNET_ADJACENCY_H
#define DNET_ADJACENCY_H

#include <stddef.h>
#include <stdint.h>

#include "dnet_hello.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Injected monotonic clock. The caller advances this however it likes (a
 * monotonic second counter in production; a hand-stepped counter in tests).
 * T3 and the listen timer are expressed in the SAME unit -- conventionally
 * seconds, to match the on-wire "hello <n>" timer field.
 */
typedef uint64_t dnet_tick_t;

/*
 * Timer constants.
 *
 * DNET_T3_DEFAULT (hello timer, seconds) is ORACLE-CAPTURED: the vms-3be lab
 * VAX HELLO advertised "hello 15" (docs/decnet-provenance-register.md sec 4.6,
 * the TIMER field = 0x000f). This is our default own-emission cadence and the
 * fallback we assume for a neighbour that somehow advertised 0.
 *
 * DNET_BCT3MULT_DEFAULT (listen-timer multiplier) is SPEC-DERIVED, NOT oracle-
 * captured: the DNA Phase IV Routing spec sets an adjacency's listen (holding)
 * timer to BCT3MULT x the hello timer, but the vms-3be capture recorded only a
 * live HELLO cadence, never a neighbour timing OUT, so no multiplier value was
 * observed on the wire. 2 is the DNA-documented default and OVMX's chosen
 * default here; it is configurable per instance (dnet_adj_init). Labelled as
 * an OVMX/spec default, never presented as an oracle-observed fact. When a
 * router-timeout specimen is captured, promote this to oracle-grounded.
 */
#define DNET_T3_DEFAULT         15u   /* oracle: vms-3be HELLO "hello 15" */
#define DNET_BCT3MULT_DEFAULT   2u    /* spec/OVMX default (DNA Phase IV); not oracle-captured */

/* Fixed neighbour-table capacity (no allocation; freestanding-friendly). A LAN
 * adjacency set well within Phase IV's practical bounds; OVMX design choice. */
#ifndef DNET_ADJ_MAX_NEIGHBORS
#define DNET_ADJ_MAX_NEIGHBORS  32
#endif

/* Adjacency lifecycle state (DNA Phase IV routing adjacency). */
enum dnet_adj_state {
    DNET_ADJ_DOWN = 0,      /* no adjacency: never heard, or listen timer lapsed */
    DNET_ADJ_INITIALIZING,  /* heard a valid HELLO, two-way not yet confirmed */
    DNET_ADJ_UP             /* two-way reachability confirmed (peer names us) */
};

/* Return codes. */
#define DNET_ADJ_OK       0
#define DNET_ADJ_EINVAL (-1)   /* null argument */
#define DNET_ADJ_EFULL  (-2)   /* neighbour table full, new neighbour dropped */

/* Per-neighbour adjacency record. */
struct dnet_adj_neighbor {
    int             in_use;
    uint8_t         id[DNET_ADDR_LEN];  /* neighbour Ethernet id AA-00-04-00-nn-nn */
    uint16_t        addr;               /* decoded DECnet address (area<<10 | node) */
    enum dnet_adj_state state;
    int             two_way;            /* peer's HELLO named us as its neighbour */
    uint16_t        adv_t3;             /* neighbour's advertised hello timer (s) */
    dnet_tick_t     last_heard;         /* tick of the most recent valid HELLO */
    dnet_tick_t     listen_deadline;    /* tick at which the adjacency times out */
};

/* The adjacency state machine for one local endnode/circuit. */
struct dnet_adjacency {
    uint8_t     my_id[DNET_ADDR_LEN];   /* our own Ethernet id (for the two-way test) */
    uint16_t    t3;                     /* our own hello cadence, seconds */
    uint16_t    bct3mult;               /* listen-timer multiplier */
    dnet_tick_t next_hello_due;         /* tick our next HELLO should be emitted */
    size_t      count;                  /* neighbours currently tracked (in_use) */
    struct dnet_adj_neighbor nbr[DNET_ADJ_MAX_NEIGHBORS];
};

/*
 * Initialise the state machine. `my_id` is our 6-byte Ethernet id (used for the
 * two-way reachability test). `t3` is our own hello cadence in seconds (0 =>
 * DNET_T3_DEFAULT); `bct3mult` is the listen-timer multiplier (0 =>
 * DNET_BCT3MULT_DEFAULT). `now` seeds the emission clock: the first HELLO is due
 * at now + t3. Returns DNET_ADJ_OK, or DNET_ADJ_EINVAL on a null argument.
 */
int dnet_adj_init(struct dnet_adjacency *sm, const uint8_t my_id[DNET_ADDR_LEN],
                  uint16_t t3, uint16_t bct3mult, dnet_tick_t now);

/*
 * Feed a decoded, validated Ethernet Endnode Hello into the SM at time `now`.
 * Creates or refreshes the neighbour, (re)arms its listen timer to
 * now + bct3mult * adv_t3, and advances its adjacency state:
 *   DOWN         -> INITIALIZING (first HELLO) -> UP if two-way holds
 *   INITIALIZING -> UP           when two-way holds
 *   UP           -> stays UP      (kept alive; two-way loss is not a teardown
 *                                  by itself while HELLOs keep arriving)
 * "Two-way" holds when the HELLO's neighbour field names US (h->neighbor ==
 * my_id) -- the DNA reachability handshake: the peer has heard us. When
 * non-NULL, *state_out receives the resulting adjacency state. Returns
 * DNET_ADJ_OK, DNET_ADJ_EFULL if a brand-new neighbour cannot fit, or
 * DNET_ADJ_EINVAL on a null argument.
 */
int dnet_adj_rx_hello(struct dnet_adjacency *sm, dnet_tick_t now,
                      const struct dnet_endnode_hello *h,
                      enum dnet_adj_state *state_out);

/*
 * Advance time to `now` and expire any adjacency whose listen timer has lapsed
 * (now >= listen_deadline): each such neighbour transitions to DNET_ADJ_DOWN.
 * Returns the number of neighbours that transitioned to DOWN on this call
 * (0 if none), or DNET_ADJ_EINVAL (negative) on a null argument.
 */
int dnet_adj_tick(struct dnet_adjacency *sm, dnet_tick_t now);

/* Look up a neighbour by Ethernet id; NULL if not tracked. */
struct dnet_adj_neighbor *dnet_adj_find(struct dnet_adjacency *sm,
                                        const uint8_t id[DNET_ADDR_LEN]);

/* Current adjacency state for `id` (DNET_ADJ_DOWN if not tracked). */
enum dnet_adj_state dnet_adj_state_of(const struct dnet_adjacency *sm,
                                      const uint8_t id[DNET_ADDR_LEN]);

/* --- our own HELLO emission cadence (T3) -------------------------------- */

/* 1 if it is time to emit our next HELLO (now >= next_hello_due), else 0. */
int dnet_adj_hello_due(const struct dnet_adjacency *sm, dnet_tick_t now);

/* The tick at which our next HELLO is due. */
dnet_tick_t dnet_adj_next_hello_tick(const struct dnet_adjacency *sm);

/*
 * Record that we just emitted a HELLO at `now`; reschedules the next emission
 * to now + t3. (The caller decides HOW to put the HELLO on the wire; this only
 * advances the cadence clock.)
 */
void dnet_adj_hello_emitted(struct dnet_adjacency *sm, dnet_tick_t now);

#ifdef __cplusplus
}
#endif

#endif /* DNET_ADJACENCY_H */
