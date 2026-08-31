/*
 * dnet_engine.h - DECnet Phase IV routing ENGINE (rung 1 of the engine lane,
 *                 rd vms-449d / epic vms-30e).
 *
 * This is the piece that MOVES FRAMES. The three landed codecs are pure,
 * engine-agnostic byte/state libraries -- they build and decode a HELLO
 * (dnet_hello.{c,h}), and drive the per-neighbour adjacency lifecycle
 * (dnet_adjacency.{c,h}) -- but none of them touches a socket, a clock, or the
 * wire. This engine binds them into a live routing endnode: it assembles the
 * full on-wire Ethernet HELLO frame, drives the emission cadence (T3) and the
 * adjacency listen timer on an injected monotonic clock, and consumes received
 * frames to keep the neighbour table.
 *
 * OPERATOR RULING (2026-08-31, rd vms-a1c): the DECnet engine is Option B --
 * a USERSPACE NSP/routing engine over AF_PACKET SOCK_RAW, forking the proven
 * src/vmsscs/ LAVC datalink (scs_datalink.h), NOT an in-kernel AF_DECnet
 * forward-port. See docs/decnet-provenance-register.md sec 6.
 *
 * RULE 1 -- "do it like VMS, or HIDE it." The AF_PACKET raw socket is a hidden
 * mechanism, exactly as src/vmsscs/scsd.c hides its raw SCA socket behind the
 * SCS/NISCA surface. This engine module owns NO socket: it is pure logic over
 * frame buffers + an injected clock (the same discipline as the codecs it
 * consumes), so it is deterministically unit-testable and substrate-neutral.
 * The VMS-visible face it presents is the DECnet routing surface a real NCP
 * user sees -- an executor node (area.node + name + state), a circuit over the
 * datalink device, and a SHOW ADJACENT NODES adjacency table -- NEVER a raw
 * socket or a Linux interface name. The daemon that owns the actual datalink
 * (decnetd.c) is the ONLY place scs_datalink_{open,send,recv}() is called, the
 * same way scsd.c is the only place SCS's datalink is opened.
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md Rule 8, HARD; docs/decnet-provenance-
 * register.md). Everything here derives ONLY from: (a) the public DEC DNA
 * Phase IV Routing Layer spec (the endnode-HELLO wire form, the 0x6003
 * ethertype, the AA-00-04-00-xx-yy address<->id mapping, the AB-00-00-03-00-00
 * Phase IV multicast, the T3 cadence / BCT3MULT listen timer), (b) the vms-3be
 * lab-oracle capture (docs/decnet-provenance-register.md sec 4.6), and (c)
 * OVMX's own scsd.c raw-Ethernet datalink pattern. No VSI/HPE/DEC source or
 * binary was disassembled, decompiled, or copied. Where a value is an OVMX
 * design choice rather than an oracle/spec fact (e.g. the device->circuit name
 * mapping) it is LABELLED as such below, never presented as VMS-authentic.
 */
#ifndef DNET_ENGINE_H
#define DNET_ENGINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "dnet_hello.h"
#include "dnet_adjacency.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Full 14-byte Ethernet II header: dst[6] src[6] ethertype[2]. */
#define DNET_ETH_HDRLEN     14
/* A generous cap for one full Ethernet frame (header + max DECnet payload +
 * slack). Phase IV HELLOs are tiny (60 bytes on the wire); this bounds the
 * receive buffer the daemon hands us. */
#define DNET_FRAME_MAX      1536

/* The Phase IV *endnode* HELLO multicast destination, AB-00-00-03-00-00 --
 * oracle-captured (vms-3be specimen #1 dst) and DNA-documented. Defined in the
 * .c so it has a single storage definition. */
extern const uint8_t DNET_HELLO_MCAST[DNET_ADDR_LEN];

/* NCP node names are 1..6 characters (DNA Phase IV). +1 for the NUL. */
#define DNET_NODENAME_MAX   6
/* VMS device / DECnet circuit name field caps (e.g. "EWA0" / "EWA-0"). */
#define DNET_DEVNAME_MAX    15

/* Engine return codes (distinct namespace from the codecs'). */
#define DNET_ENGINE_OK        0
#define DNET_ENGINE_EINVAL  (-1)   /* null / out-of-range argument */
#define DNET_ENGINE_ENOSPACE (-2)  /* output buffer too small */

/*
 * The live routing endnode. Pure state: no socket, no fd, no thread. The
 * daemon (decnetd.c) owns the datalink and injects the clock; this struct is
 * everything the VMS-visible DECnet surface and the wire logic need.
 */
struct dnet_engine {
    /* --- executor identity (the NCP "executor node") --- */
    uint16_t addr;                        /* DECnet address (area<<10 | node) */
    char     node_name[DNET_NODENAME_MAX + 1]; /* NCP node name, e.g. "OVMX1" */
    uint8_t  my_id[DNET_ADDR_LEN];        /* AA-00-04-00-<LE addr> Ethernet id */
    uint8_t  hw_mac[DNET_ADDR_LEN];       /* the datalink device's real HW MAC */
    uint16_t blksize;                     /* advertised max receive block size */

    /* --- the DECnet circuit over the datalink device (NCP "circuit") ---
     * OVMX design choice (LABELLED, not VMS-authentic): the VMS device name and
     * the circuit name are supplied by the daemon; the executive device-table
     * registration of _NET:/EWA0: over /dev/vms is a later rung (vms-a7e), so
     * these are a presentation label here, honestly scoped. */
    char     device[DNET_DEVNAME_MAX + 1];  /* e.g. "EWA0"  */
    char     circuit[DNET_DEVNAME_MAX + 1]; /* e.g. "EWA-0" */

    /* --- routing/adjacency state machine (rung-3 codec) --- */
    struct dnet_adjacency adj;

    /* --- honest counters (reported on the VMS surface; no fabrication) --- */
    unsigned long hello_sent;
    unsigned long hello_recv;      /* well-formed endnode HELLOs accepted */
    unsigned long frames_recv;     /* every frame handed to rx_frame */
    unsigned long frames_dropped;  /* wrong ethertype / own echo / undecodable */
    unsigned long adj_up_events;
    unsigned long adj_down_events;
};

/*
 * dnet_id_from_addr - build the DECnet Ethernet id AA-00-04-00-<LE addr> from an
 * area.node pair. E.g. area=1 node=1 -> AA-00-04-00-01-04 (the vms-3be node 1.1).
 * Returns DNET_ENGINE_OK, or DNET_ENGINE_EINVAL on a null out / out-of-range
 * area (>63) or node (>1023).
 */
int dnet_id_from_addr(unsigned area, unsigned node, uint8_t id_out[DNET_ADDR_LEN]);

/*
 * dnet_engine_init - stand up the endnode with executor identity area.node,
 * NCP node_name, the VMS device/circuit labels, and the datalink device's real
 * HW MAC (for reporting only). t3 = our HELLO cadence in seconds (0 =>
 * DNET_T3_DEFAULT, the oracle-captured 15 s); blksize = advertised max receive
 * block size (0 => 1498, the vms-3be value). `now` seeds the emission clock.
 *
 * Returns DNET_ENGINE_OK, or DNET_ENGINE_EINVAL on a null/oversized argument.
 * No identity is invented: the caller must pass a real area.node (the daemon
 * fails honestly rather than defaulting one, INV-6 / the scsd resolve_node_
 * identity discipline).
 */
int dnet_engine_init(struct dnet_engine *e, unsigned area, unsigned node,
                     const char *node_name, const char *device,
                     const char *circuit, const uint8_t hw_mac[DNET_ADDR_LEN],
                     uint16_t t3, uint16_t blksize, dnet_tick_t now);

/*
 * dnet_engine_build_hello_frame - assemble the complete on-wire endnode-HELLO
 * Ethernet frame into `frame_out`: a 14-byte Ethernet header (dst = the Phase
 * IV endnode multicast, src = our Ethernet id, ethertype = 0x6003 big-endian)
 * followed by the endnode-HELLO payload from dnet_hello_encode(). The payload
 * advertises our address, node type endnode, blksize, T3 timer, and rtr 0.0
 * (no designated router) -- matching the vms-3be endnode-HELLO shape.
 *
 * Writes the total frame length to *len_out. Returns DNET_ENGINE_OK, or
 * DNET_ENGINE_ENOSPACE if `cap` is too small, or DNET_ENGINE_EINVAL.
 */
int dnet_engine_build_hello_frame(const struct dnet_engine *e,
                                  uint8_t *frame_out, size_t cap, size_t *len_out);

/*
 * dnet_engine_rx_frame - consume one received full Ethernet frame (including the
 * 14-byte header, exactly as AF_PACKET SOCK_RAW / scs_datalink_recv() delivers
 * it) at time `now`. Validates the 0x6003 ethertype, ignores our own multicast
 * echo (src == my_id), decodes the endnode HELLO, and drives the adjacency SM.
 *
 * Returns 1 if a HELLO was accepted and the neighbour SM advanced, 0 if the
 * frame was ignored (wrong ethertype, own echo, or not a decodable endnode
 * HELLO -- frames_dropped is bumped), or DNET_ENGINE_EINVAL on a null argument.
 * When non-NULL, *from_out receives the neighbour's Ethernet id and *state_out
 * its resulting adjacency state. Counters are updated in every case.
 */
int dnet_engine_rx_frame(struct dnet_engine *e, dnet_tick_t now,
                         const uint8_t *frame, size_t len,
                         uint8_t from_out[DNET_ADDR_LEN],
                         enum dnet_adj_state *state_out);

/* T3 cadence: 1 if our next HELLO is due at `now`, else 0. */
int dnet_engine_hello_due(const struct dnet_engine *e, dnet_tick_t now);

/* Record that we emitted a HELLO at `now`: advance the T3 cadence + bump the
 * hello_sent counter. (The daemon decides HOW the frame reached the wire.) */
void dnet_engine_hello_emitted(struct dnet_engine *e, dnet_tick_t now);

/*
 * dnet_engine_tick - advance time to `now`, expiring any adjacency whose listen
 * timer lapsed (each -> DOWN). Returns the number that transitioned to DOWN
 * (also folded into adj_down_events), or DNET_ENGINE_EINVAL on a null argument.
 */
int dnet_engine_tick(struct dnet_engine *e, dnet_tick_t now);

/* --- the VMS-faithful presentation surface (NCP SHOW ..., the hidden-socket
 * face) -------------------------------------------------------------------- */

/* NCP "SHOW EXECUTOR" analogue: executor node = a.n (NAME), State = on, etc. */
void dnet_engine_show_executor(const struct dnet_engine *e, FILE *out);
/* NCP "SHOW CIRCUIT" analogue: the DECnet circuit over the datalink device. */
void dnet_engine_show_circuit(const struct dnet_engine *e, FILE *out);
/* NCP "SHOW ADJACENT NODES" analogue: the live neighbour adjacency table. */
void dnet_engine_show_adjacent(const struct dnet_engine *e, FILE *out);

/* Format a DECnet address as "area.node" into buf (>= 8 bytes). Returns buf. */
const char *dnet_addr_str(uint16_t addr, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* DNET_ENGINE_H */
