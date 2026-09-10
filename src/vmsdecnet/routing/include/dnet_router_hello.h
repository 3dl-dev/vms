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
 * register.md). A REAL OpenVMS VAX V7.3 router-hello was captured on the lab
 * segment (register sec 4.6a, rd vms-df5): the lone-L1-router (n = 0) routing
 * message is 27 bytes and its RSLIST tail (RSLIST-length = 8, 7 zero Name
 * bytes, RSLIST-count = 0) matches the layout below byte-for-byte -- the codec
 * is now oracle-grounded for that case (test_dnet_router_hello.c round-trips
 * the captured bytes). The layout is also, and was originally, derived from
 * PUBLIC descriptions of the wire format -- the DNA Phase IV protocol bytes
 * themselves, which are factual and uncopyrightable, the same bytes any
 * conformant router emits:
 *   (a) the public DEC Digital Network Architecture (DNA) Phase IV Routing
 *       Layer Functional Specification's Ethernet Router Hello message
 *       definition (routing-message header, node id, node-type/priority, the
 *       AA-00-04-00-xx-yy address <-> node.area mapping already oracle-
 *       corroborated by the sibling endnode-hello codec, AND the trailing
 *       RSLIST/router-list structure: an RSLIST-length byte, a 7-byte reserved
 *       Name field, an RSLIST-count byte, then 7-byte router-list entries);
 *   (b) the public tcpdump `print-decnet.c` decoder (BSD-licensed, widely
 *       reviewed public documentation of live DECnet Phase IV traffic --
 *       NOT VSI/HPE/DEC source), whose `struct ehellomsg` field-for-field
 *       matches this repo's ALREADY oracle-validated endnode-hello codec, and
 *       whose sibling `struct rhellomsg` describes the router-hello fixed part
 *       (its RMF_RHELLO = 013 octal = 0x0b is DNET_RFLAG_ROUTER_HELLO below,
 *       and it confirms AREA->TIMER is DIRECT -- there is no bct3mult byte on
 *       the wire); and
 *   (c) the public Linux kernel `net/decnet/dn_dev.c` `dn_send_router_hello()`
 *       -- an INDEPENDENT, real Phase IV router-hello encoder (GPL). It is used
 *       here ONLY as confirmation of the WIRE-FORMAT bytes of the RSLIST tail,
 *       NOT as a source of code: it emits, after the MPD byte, an RSLIST-length
 *       byte = 8 + 7*n, then 7 reserved zero "Name" bytes, then an RSLIST-count
 *       byte = 7*n, then n 7-byte router-list entries. Those byte values are
 *       the DNA protocol, not an implementation choice; the OVMX codec below is
 *       written independently in this repo's own field-append style and does
 *       NOT mirror the kernel's expression, structure, or naming.
 * No VSI/HPE/DEC source or binary was disassembled, decompiled, or copied.
 *
 * WHY THE RSLIST TAIL IS NOW EXPLICIT (rd vms-df5): an earlier revision of this
 * codec stopped after the MPD byte (an 18-byte fixed message) and carried
 * everything past it as an opaque "E-list" blob. A REAL OpenVMS VAX V7.3
 * rejected that short frame on the wire (routing event 4.4, "packet format
 * error") and never selected OVMX as designated router, because a conformant
 * Phase IV Ethernet router hello ALWAYS carries the RSLIST tail (RSLIST-length,
 * 7-byte Name, RSLIST-count) after MPD -- even for a lone router advertising no
 * other routers (n = 0), where the tail is a fixed RSLIST-length = 8, seven
 * zero Name bytes, and an RSLIST-count = 0. The fixed message is therefore 27
 * bytes, not 18, and this codec now models those fields explicitly.
 *
 * WHAT IS ORACLE-GROUNDED vs SPEC-DERIVED (read before trusting a field):
 *   - The RFLAGS control/type-field ENCODING SCHEME (bit 0 = control message,
 *     bits 1-3 = message type) and the 2-byte "Ethernet DATA LENGTH" framing
 *     prefix are REUSED, unchanged, from the oracle-validated endnode-hello
 *     envelope (dnet_hello.h) -- both are properties of the shared Ethernet
 *     routing-message envelope, not something specific to the hello sub-type.
 *   - PRIORITY, AREA, TIMER, MPD and the fixed-field ORDER are SPEC-DERIVED
 *     (tcpdump rhellomsg + DNA) -- NOT independently oracle-observed for a
 *     router. AREA->TIMER is direct; there is NO bct3mult byte on the wire.
 *   - The RSLIST tail (RSLIST-length = 8 + 7*n, 7-byte reserved Name = 0,
 *     RSLIST-count = 7*n, then n 7-byte router-list entries) is SPEC-DERIVED
 *     from the DNA routing spec and CORROBORATED byte-for-byte by the public
 *     Linux dn_send_router_hello encoder (ref (c) above). The RSLIST-length,
 *     Name and RSLIST-count bytes are modelled explicitly; the individual
 *     7-byte router-list ENTRIES are carried as opaque bytes (their internal
 *     sub-field layout -- id + priority/two-way status -- is not needed for
 *     OVMX to be selected as a designated router by an endnode, and OVMX
 *     advertises n = 0, an honest "no other routers reported", INV-6).
 *   - The n = 0 (lone-router) message IS oracle-grounded: register sec 4.6a
 *     holds the real captured VMS V7.3 router-hello bytes, and the ctest
 *     asserts OVMX's encoder reproduces them exactly. The individual router-
 *     list ENTRY sub-fields (n > 0) remain spec-derived/opaque -- the captured
 *     specimen advertised no other routers, so no entry bytes were observed.
 *
 * On-wire layout of the 802.3/Ethernet data field this codec owns
 * (little-endian; same length-prefixed envelope as dnet_hello.h). Offsets are
 * buffer-relative (off 0 = the 2-byte DATA LENGTH prefix); the routing-message
 * offsets referenced in code start at off 2 = message offset 0:
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
 *   16    1    AREA          reserved (tcpdump: "reserved"); AREA->TIMER direct
 *   17    2    TIMER (LE)    hello timer, seconds
 *   19    1    MPD           reserved / must-be-zero
 *   -- RSLIST tail (always present on a conformant router hello) ------------
 *   20    1    RSLIST LEN    router-list length byte = 8 + RSLIST COUNT
 *   21    7    NAME          reserved, all-zero
 *   28    1    RSLIST COUNT  router-list byte count = 7 * (#router entries)
 *   29  var    RSLIST        router-list entries, 7 bytes each (opaque);
 *                            length = RSLIST COUNT = DATA LENGTH - 27, may be 0
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

/* RSLIST tail geometry (DNA Phase IV; corroborated by the public Linux
 * dn_send_router_hello encoder, see provenance note ref (c)). */
#define DNET_ROUTER_HELLO_NAME_LEN   7     /* reserved Name field width (all-zero) */
#define DNET_ROUTER_HELLO_RSENTRY    7     /* each router-list entry is 7 bytes */
#define DNET_ROUTER_HELLO_RSLIST_HDR 8     /* RSLIST-length base: 1 len + 7 Name (n=0) */

/* Cap on the router-list: the DNA/Linux encoder caps the entry count at 32,
 * i.e. 7*32 = 224 router-list bytes. 224 + DNET_ROUTER_HELLO_RSLIST_HDR = 232
 * still fits the u8 RSLIST-length byte, so no wire field can overflow. */
#define DNET_ROUTER_HELLO_MAX_RSENTRIES  32
#define DNET_ROUTER_HELLO_MAX_RSLIST     (DNET_ROUTER_HELLO_RSENTRY * \
                                          DNET_ROUTER_HELLO_MAX_RSENTRIES) /* 224 */

/* Fixed sizes of the framing prefix (shared with dnet_hello.h) and the fixed
 * part of the router-hello routing message. The fixed part now runs RFLAGS
 * through the RSLIST-count byte inclusive -- i.e. it INCLUDES the RSLIST tail
 * for n = 0 (RSLIST-length + Name[7] + RSLIST-count), and only the variable
 * router-list ENTRIES follow it. */
#define DNET_ROUTER_HELLO_LENPREFIX   DNET_HELLO_LENPREFIX  /* 2 */
#define DNET_ROUTER_HELLO_FIXED_MSG   27   /* rflags..rslist_count inclusive (n=0 tail) */

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
    /* RSLIST tail (see layout above). rslist_len is a DERIVED wire byte
     * (8 + rslist_count); the encoder always recomputes it from rslist_count
     * so an emitted frame is DNA-consistent regardless of the caller's value.
     * On decode it carries the raw wire byte for inspection. */
    uint8_t  rslist_len;                           /* RSLIST-length byte (off 20) */
    uint8_t  name[DNET_ROUTER_HELLO_NAME_LEN];     /* reserved Name field, all-zero */
    uint8_t  rslist_count;                         /* RSLIST-count byte = router-list byte count */
    uint8_t  rslist[DNET_ROUTER_HELLO_MAX_RSLIST]; /* router-list entry bytes (opaque, 7*n) */
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
 * written to *consumed when non-NULL. The router-list ENTRIES (RSLIST COUNT
 * bytes) are stored opaquely in out->rslist with out->rslist_count set from
 * the framed message length (the authoritative router-list byte count).
 * Returns a negative DNET_ROUTER_HELLO_E* on malformed input.
 */
int dnet_router_hello_decode(const uint8_t *buf, size_t len,
                             struct dnet_router_hello *out, size_t *consumed);

/*
 * Encode `msg` into `buf` as the DEC Ethernet Router Hello wire form: the
 * 2-byte DATA LENGTH prefix, the fixed routing message (RFLAGS..MPD), then the
 * RSLIST tail -- RSLIST-length (recomputed as 8 + msg->rslist_count), the
 * 7-byte Name field, RSLIST-count, and msg->rslist_count router-list bytes --
 * then a zero pad up to the Ethernet 46-byte minimum data-field length (same
 * data-link pad behaviour as dnet_hello_encode -- an OVMX encoder choice, not
 * a routing field). Writes the total byte count to *outlen when non-NULL.
 * Returns DNET_ROUTER_HELLO_OK, DNET_ROUTER_HELLO_EBADLEN if rslist_count
 * exceeds the cap, or DNET_ROUTER_HELLO_ENOSPACE if `cap` is too small.
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
