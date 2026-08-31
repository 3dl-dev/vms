/*
 * dnet_hello.h - DECnet Phase IV routing-layer Ethernet Endnode Hello codec
 *                (rung 1 of the DECnet lane, rd vms-851 / epic vms-30e).
 *
 * Encodes and decodes the DECnet Phase IV *Ethernet Endnode Hello* routing
 * message -- the periodic adjacency beacon a Phase IV endnode multicasts
 * (ethertype 0x6003, dst AB-00-00-03-00-00) so neighbouring routers/nodes
 * list it in SHOW ADJACENT/KNOWN NODES. This is the smallest self-contained,
 * oracle-testable slice of the DECnet wire: a pure byte-layout codec with no
 * socket, no allocation, and no kernel/libc dependency beyond memcpy/memset,
 * so it can serve EITHER engine boundary that vms-851's go/no-go settles on
 * (docs/design-decnet-ovmx.md sec 4b: "the L3-L6 VMS surface is identical
 * either way -- only the engine boundary moves"): the forward-ported
 * in-kernel net/decnet validation harness, or the sec-4b userspace
 * NSP/AF_PACKET fallback engine (src/vmsdecnet/{datalink,routing,nsp}). It
 * lives at the routing layer named in design sec 4 ("2. NSP + routing ...
 * Phase IV routing, HELLO").
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md Rule 8, HARD; docs/decnet-provenance-
 * register.md).  The byte layout below is derived ONLY from:
 *   (a) the public DEC Digital Network Architecture (DNA) Phase IV Routing
 *       Layer Functional Specification (the Ethernet Endnode Hello message
 *       format and the AA-00-04-00-xx-yy address <-> node.area mapping), and
 *   (b) the lab-oracle wire specimen captured under rd vms-3be (PR #665) and
 *       committed as a hex dump in docs/decnet-provenance-register.md sec 4.6
 *       ("Hex dump, specimen #1 (HELLO)"): a 46-byte Ethernet payload sent by
 *       lab node VAX1 (OpenVMS VAX V7.3), src aa:00:04:00:01:04 (node 1.1)
 *       -> dst ab:00:00:03:00:00, tcpdump-decoded as
 *       "endnode-hello endnode vers 2 eco 0 ueco 0 src 1.1 blksize 1498
 *        rtr 0.0 hello 15 data 2".
 * No VSI/HPE/DEC source or binary was disassembled, decompiled, or copied.
 * Every field offset here is corroborated by BOTH the public spec layout and
 * that captured specimen (see the field-by-field map in dnet_hello.c). Where
 * the wire carries a data-link artefact the spec does not attribute to the
 * routing message -- the trailing zero pad up to the 46-byte Ethernet minimum
 * frame -- it is reproduced as an OVMX-labelled encoder choice (see
 * dnet_hello_encode), never presented as a routing-layer field.
 *
 * On-wire layout of the 802.3/Ethernet data field this codec owns
 * (little-endian; the DEC "Ethernet padding" framing prefixes the routing
 * message with its length):
 *
 *   off  size  field
 *   ---  ----  --------------------------------------------------------------
 *    0    2    DATA LENGTH (LE) = size of the routing message that follows
 *   -- routing message (DATA LENGTH bytes) ---------------------------------
 *    2    1    RFLAGS        routing flags / control-message type
 *                            (0x0d = control, msg type 6 = endnode hello)
 *    3    1    VERSION       DNA version   (tiver[0])
 *    4    1    ECO           ECO           (tiver[1])
 *    5    1    USER ECO      user ECO      (tiver[2])
 *    6    6    ID            sender's Ethernet id (AA-00-04-00-nn-nn)
 *   12    1    IINFO         info byte; low 2 bits = node type
 *   13    2    BLKSIZE (LE)  max receive block size
 *   15    1    AREA          area field
 *   16    8    SEED          verification seed
 *   24    6    NEIGHBOR      designated-router Ethernet id (0-fill => 0.0)
 *   30    2    TIMER (LE)    hello timer, seconds
 *   32    1    MPD           reserved / must-be-zero
 *   33    1    DATALEN       length of the TEST DATA that follows
 *   34  DATALEN TEST DATA    padding test data
 *   -- data-link minimum-frame pad (encoder choice; see .c) ----------------
 *
 * For specimen #1: DATA LENGTH = 0x0022 (34), DATALEN = 2, so the routing
 * message is 34 bytes, the encoded prefix+message is 36 bytes, and the frame
 * is zero-padded to the 46-byte Ethernet minimum.
 */
#ifndef DNET_HELLO_H
#define DNET_HELLO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wire constants (public DNA Phase IV; corroborated by the vms-3be specimen). */
#define DNET_ETHERTYPE          0x6003  /* DECnet Phase IV ethertype (design sec 2) */
#define DNET_RFLAG_ENDNODE_HELLO 0x0d   /* control msg, type 6 = Ethernet endnode hello */
#define DNET_ETH_MIN_PAYLOAD    46      /* Ethernet minimum data-field length (802.3) */

/* Node type carried in the low 2 bits of IINFO (DNA Phase IV Routing spec). */
#define DNET_NODETYPE_L2ROUTER  1
#define DNET_NODETYPE_L1ROUTER  2
#define DNET_NODETYPE_ENDNODE   3

#define DNET_SEED_LEN           8
#define DNET_ADDR_LEN           6       /* DECnet/Ethernet id length */
#define DNET_HELLO_MAX_DATA     128     /* generous cap on the TEST DATA field */

/* Fixed sizes of the framing prefix and the fixed part of the routing message
 * (everything except the variable TEST DATA). */
#define DNET_HELLO_LENPREFIX    2
#define DNET_HELLO_FIXED_MSG    32      /* rflags..datalen inclusive, no test data */

/* Decoded / to-be-encoded Ethernet Endnode Hello message. */
struct dnet_endnode_hello {
    uint8_t  rflags;                    /* routing flags (DNET_RFLAG_ENDNODE_HELLO) */
    uint8_t  version;                   /* tiver[0] */
    uint8_t  eco;                       /* tiver[1] */
    uint8_t  user_eco;                  /* tiver[2] */
    uint8_t  id[DNET_ADDR_LEN];         /* sender Ethernet id, AA-00-04-00-nn-nn */
    uint8_t  iinfo;                     /* info byte; low 2 bits = node type */
    uint16_t blksize;                   /* max receive block size */
    uint8_t  area;                      /* area field */
    uint8_t  seed[DNET_SEED_LEN];       /* verification seed */
    uint8_t  neighbor[DNET_ADDR_LEN];   /* designated-router Ethernet id */
    uint16_t timer;                     /* hello timer, seconds */
    uint8_t  mpd;                       /* reserved / must-be-zero */
    uint8_t  datalen;                   /* length of test data (<= DNET_HELLO_MAX_DATA) */
    uint8_t  data[DNET_HELLO_MAX_DATA]; /* test data */
};

/* Return codes. */
#define DNET_HELLO_OK            0
#define DNET_HELLO_ETRUNC      (-1)     /* input buffer too short for the message */
#define DNET_HELLO_EBADLEN     (-2)     /* embedded DATA LENGTH inconsistent / oversized */
#define DNET_HELLO_ENOSPACE    (-3)     /* output buffer too small */
#define DNET_HELLO_EINVAL      (-4)     /* null argument */

/*
 * Decode an Ethernet Endnode Hello from `buf` (buf[0..2] = DATA LENGTH LE,
 * then the routing message). Trailing data-link padding beyond the routing
 * message is ignored. On success fills *out and returns DNET_HELLO_OK; the
 * number of bytes the routing message occupied (2 + DATA LENGTH) is written
 * to *consumed when non-NULL. Returns a negative DNET_HELLO_E* on malformed
 * input.
 */
int dnet_hello_decode(const uint8_t *buf, size_t len,
                      struct dnet_endnode_hello *out, size_t *consumed);

/*
 * Encode `msg` into `buf` as the DEC Ethernet Endnode Hello wire form:
 * the 2-byte DATA LENGTH prefix, the routing message, then a zero pad up to
 * the Ethernet 46-byte minimum data-field length. Writes the total byte
 * count to *outlen when non-NULL. Returns DNET_HELLO_OK, or DNET_HELLO_ENOSPACE
 * if `cap` is too small.
 *
 * The trailing zero pad to DNET_ETH_MIN_PAYLOAD is a DATA-LINK behaviour, not
 * a routing field: an Ethernet controller pads any sub-minimum frame with
 * zeros, which is exactly what the vms-3be capture shows. Reproducing it here
 * (labelled OVMX encoder choice, not a VMS-authentic routing field) is what
 * makes the round-trip against the captured 46-byte payload byte-identical.
 */
int dnet_hello_encode(const struct dnet_endnode_hello *msg,
                      uint8_t *buf, size_t cap, size_t *outlen);

/* Derive the 16-bit DECnet node address (area<<10 | node) from a 6-byte
 * AA-00-04-00-nn-nn Ethernet id: the low two bytes are the address, LE. */
uint16_t dnet_addr_from_id(const uint8_t id[DNET_ADDR_LEN]);

/* Split a 16-bit DECnet address into area (bits 15..10) and node (bits 9..0). */
static inline unsigned dnet_area_of(uint16_t addr) { return (addr >> 10) & 0x3F; }
static inline unsigned dnet_node_of(uint16_t addr) { return addr & 0x3FF; }

/* Node type carried in IINFO (low 2 bits); one of DNET_NODETYPE_*. */
static inline unsigned dnet_hello_nodetype(const struct dnet_endnode_hello *m)
{
    return m->iinfo & 0x03u;
}

#ifdef __cplusplus
}
#endif

#endif /* DNET_HELLO_H */
