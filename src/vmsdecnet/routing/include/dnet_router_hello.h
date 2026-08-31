/*
 * dnet_router_hello.h - DECnet Phase IV routing-layer Ethernet Router Hello
 *                       codec (rd vms-0aba, sibling of rung 1's endnode-hello
 *                       codec, rd vms-851 / epic vms-30e).
 *
 * Encodes and decodes the DECnet Phase IV *Ethernet Router Hello* routing
 * message -- the periodic adjacency beacon an L1 or L2 Phase IV ROUTER
 * multicasts on Ethernet, as distinct from the Ethernet ENDNODE Hello a
 * non-routing node sends (dnet_hello.{c,h}). Same engine-agnostic, pure
 * byte-layout discipline as the endnode-hello codec: no socket, no
 * allocation, no kernel/libc dependency beyond memcpy/memset, so it links
 * into either engine boundary the vms-851 go/no-go settles on.
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md Rule 8, HARD; docs/decnet-provenance-
 * register.md). Unlike the endnode-hello codec, docs/decnet-provenance-
 * register.md sec 4.6 did NOT capture a Router Hello specimen from the lab
 * oracle (lab-2's VAX1 golden image is an ENDNODE, not a router, and the only
 * oracle wire captured for HELLO traffic is the 16 endnode-hello frames cited
 * there) -- confirmed by search of the register before writing this file.
 * There is therefore NO committed byte-for-byte specimen to round-trip
 * against for this message, and none is fabricated here. The layout below is
 * derived ONLY from:
 *   (a) the public DEC Digital Network Architecture (DNA) Phase IV Routing
 *       Layer Functional Specification's Ethernet Router Hello message
 *       definition (routing-message header, node id, node-type/priority,
 *       and the AA-00-04-00-xx-yy address <-> node.area mapping already
 *       oracle-corroborated by the sibling endnode-hello codec), and
 *   (b) the public tcpdump `print-decnet.c` decoder (BSD-licensed, widely
 *       reviewed public documentation of live DECnet Phase IV traffic --
 *       NOT VSI/HPE/DEC source), whose `struct ehellomsg` field-for-field
 *       matches this repo's ALREADY oracle-validated endnode-hello codec
 *       (eh_flags/eh_vers/eh_eco/eh_ueco/eh_src/eh_info/eh_blksize/eh_area/
 *       eh_seed/eh_router/eh_hello/eh_mpd/eh_data, in that order) -- which is
 *       why its sibling `struct rhellomsg` is trusted here as an accurate
 *       public description of the router-hello wire layout, and why the
 *       control-message type code it names for router hello, RMF_RHELLO =
 *       013 octal = 0x0b, is used as DNET_RFLAG_ROUTER_HELLO below (matching
 *       this item's own title annotation "rflags 0x0b, msg type 5").
 * No VSI/HPE/DEC source or binary was disassembled, decompiled, or copied.
 *
 * WHAT IS ORACLE-GROUNDED vs SPEC-DERIVED (read before trusting a field):
 *   - The RFLAGS control/type-field ENCODING SCHEME (bit 0 = control message,
 *     bits 1-3 = message type) and the 2-byte "Ethernet DATA LENGTH" framing
 *     prefix are REUSED, unchanged, from the oracle-validated endnode-hello
 *     envelope (dnet_hello.h) -- both are documented as properties of the
 *     shared Ethernet routing-message envelope, not something specific to
 *     hello sub-type, so reuse does not extend the oracle claim beyond what
 *     it actually covers.
 *   - PRIORITY, AREA, and the fixed-field ORDER are SPEC-DERIVED (tcpdump
 *     rhellomsg only) -- NOT independently oracle-observed for a router.
 *   - The trailing variable ROUTER LIST / "E-list" (other routers this router
 *     has heard, with their priority and two-way status) is REAL per the DNA
 *     spec but its exact sub-field encoding is NOT reliably public -- even
 *     tcpdump's own decoder declines to decode it in detail. OVMX therefore
 *     carries it as an OPAQUE, uninterpreted trailing byte blob (like the
 *     endnode-hello TEST DATA field) and does NOT fabricate an internal
 *     layout for it. This is a deliberate scope limit, not an oversight.
 *   - There is NO committed router-hello wire specimen (see above): the ctest
 *     for this codec is a decode-then-re-encode SELF round-trip over a
 *     hand-built message, not an oracle byte-identical comparison. Promote
 *     this file's provenance the moment a real router-hello specimen is
 *     captured and committed to the provenance register.
 *
 * On-wire layout of the 802.3/Ethernet data field this codec owns
 * (little-endian; same length-prefixed envelope as dnet_hello.h):
 *
 *   off  size  field
 *   ---  ----  --------------------------------------------------------------
 *    0    2    DATA LENGTH (LE) = size of the routing message that follows
 *   -- routing message (DATA LENGTH bytes) ---------------------------------
 *    2    1    RFLAGS        routing flags / control-message type
 *                            (0x0b = control, msg type 5 = router hello)
 *    3    1    VERSION       DNA version   (tiver[0])
 *    4    1    ECO           ECO           (tiver[1])
 *    5    1    USER ECO      user ECO      (tiver[2])
 *    6    6    ID            sender's Ethernet id (AA-00-04-00-nn-nn)
 *   12    1    IINFO         info byte; low 2 bits = node type (L1/L2 router)
 *   13    2    BLKSIZE (LE)  max receive block size
 *   15    1    PRIORITY      router's designated-router election priority
 *   16    1    AREA          reserved (tcpdump: "reserved")
 *   17    2    TIMER (LE)    hello timer, seconds
 *   19    1    MPD           reserved / must-be-zero
 *   20  var    ELIST         opaque trailing router-list bytes (uninterpreted;
 *                            length = DATA LENGTH - 18, may be zero)
 */
#ifndef DNET_ROUTER_HELLO_H
#define DNET_ROUTER_HELLO_H

#include <stddef.h>
#include <stdint.h>

#include "dnet_hello.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wire constant (public DNA Phase IV; corroborated by tcpdump RMF_RHELLO =
 * 013 octal, and by this item's own title annotation). NOT oracle-captured
 * (see the provenance note above). */
#define DNET_RFLAG_ROUTER_HELLO  0x0b   /* control msg, type 5 = Ethernet router hello */

#define DNET_ROUTER_HELLO_MAX_ELIST  128   /* generous cap on the opaque E-list, OVMX choice */

/* Fixed sizes of the framing prefix (shared with dnet_hello.h) and the fixed
 * part of the router-hello routing message (everything except the trailing
 * opaque E-list). */
#define DNET_ROUTER_HELLO_LENPREFIX   DNET_HELLO_LENPREFIX  /* 2 */
#define DNET_ROUTER_HELLO_FIXED_MSG   18   /* rflags..mpd inclusive, no E-list */

/* Decoded / to-be-encoded Ethernet Router Hello message. */
struct dnet_router_hello {
    uint8_t  rflags;                              /* routing flags (DNET_RFLAG_ROUTER_HELLO) */
    uint8_t  version;                             /* tiver[0] */
    uint8_t  eco;                                  /* tiver[1] */
    uint8_t  user_eco;                             /* tiver[2] */
    uint8_t  id[DNET_ADDR_LEN];                    /* sender Ethernet id, AA-00-04-00-nn-nn */
    uint8_t  iinfo;                                /* info byte; low 2 bits = node type */
    uint16_t blksize;                              /* max receive block size */
    uint8_t  priority;                             /* designated-router election priority */
    uint8_t  area;                                 /* reserved (spec-derived; not oracle-typed) */
    uint16_t timer;                                /* hello timer, seconds */
    uint8_t  mpd;                                  /* reserved / must-be-zero */
    uint8_t  elist_len;                            /* opaque trailing E-list length */
    uint8_t  elist[DNET_ROUTER_HELLO_MAX_ELIST];   /* opaque, uninterpreted router-list bytes */
};

/* Return codes (mirrors dnet_hello.h's DNET_HELLO_* family; kept as a
 * separate namespace since this is a distinct message codec). */
#define DNET_ROUTER_HELLO_OK          0
#define DNET_ROUTER_HELLO_ETRUNC    (-1)   /* input buffer too short for the message */
#define DNET_ROUTER_HELLO_EBADLEN   (-2)   /* embedded DATA LENGTH inconsistent / oversized */
#define DNET_ROUTER_HELLO_ENOSPACE  (-3)   /* output buffer too small */
#define DNET_ROUTER_HELLO_EINVAL    (-4)   /* null argument */

/*
 * Decode an Ethernet Router Hello from `buf` (buf[0..2] = DATA LENGTH LE,
 * then the routing message). Trailing data-link padding beyond the routing
 * message is ignored. On success fills *out and returns DNET_ROUTER_HELLO_OK;
 * the number of bytes the routing message occupied (2 + DATA LENGTH) is
 * written to *consumed when non-NULL. Returns a negative
 * DNET_ROUTER_HELLO_E* on malformed input.
 */
int dnet_router_hello_decode(const uint8_t *buf, size_t len,
                             struct dnet_router_hello *out, size_t *consumed);

/*
 * Encode `msg` into `buf` as the DEC Ethernet Router Hello wire form: the
 * 2-byte DATA LENGTH prefix, the fixed routing message, then the opaque
 * E-list bytes, then a zero pad up to the Ethernet 46-byte minimum data-field
 * length (same data-link pad behaviour as dnet_hello_encode -- an OVMX
 * encoder choice, not a routing field). Writes the total byte count to
 * *outlen when non-NULL. Returns DNET_ROUTER_HELLO_OK, or
 * DNET_ROUTER_HELLO_ENOSPACE if `cap` is too small.
 */
int dnet_router_hello_encode(const struct dnet_router_hello *msg,
                             uint8_t *buf, size_t cap, size_t *outlen);

/* Node type carried in IINFO (low 2 bits); one of DNET_NODETYPE_L1ROUTER /
 * DNET_NODETYPE_L2ROUTER (dnet_hello.h) for a well-formed router hello. */
static inline unsigned dnet_router_hello_nodetype(const struct dnet_router_hello *m)
{
    return m->iinfo & 0x03u;
}

#ifdef __cplusplus
}
#endif

#endif /* DNET_ROUTER_HELLO_H */
