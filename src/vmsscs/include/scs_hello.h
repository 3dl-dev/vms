/*
 * scs_hello.h - Build a spec-valid 0x6007 multicast HELLO frame (vms-b62).
 *
 * Reproduces the 134-byte (14-byte Ethernet + 120-byte SCA content)
 * multicast HELLO frame documented in docs/cluster-protocol-spec.md
 * section 4(a) (shared SCA discovery header, abs offsets 14-71) and
 * section 4(b) (HELLO-specific tail, abs offsets 72-133).
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md rule 8 / spec sec 0): every constant
 * byte value baked into scs_hello_build_frame() was read directly off the
 * wire in ~/vax/cluster/captures/scs-idle-baseline.pcap, frames 1 and 4
 * (VAX1 and VAX2's own multicast HELLOs), cross-checked byte-for-byte
 * against each other so the template only contains bytes that are
 * IDENTICAL between two independent real senders (i.e. genuinely constant,
 * not incidentally the same for one node). No VSI/HPE source or binary was
 * read. Fields the spec marks GROUNDED are labeled GROUNDED below; fields
 * the spec marks unknown/inferred are reproduced verbatim as
 * inferred-constant (their wire value, not a guess) per the item's
 * clean-room constraint -- this module invents no new field semantics.
 *
 * Grounded per-sender (identity) fields, filled from scs_hello_params:
 *   - Ethernet dst / SCA dest-group logical addr (abs 0-5, 16-21): the
 *     cluster multicast address for the configured cluster group
 *     (AB-00-04-01-<group><group>, GROUNDED for group=1 against SYSMAN
 *     CONFIGURATION SHOW CLUSTER_AUTHORIZATION -- see
 *     scs_hello_multicast_addr()).
 *   - Ethernet src / SCA src-logical-addr / HELLO-tail HW MAC (abs 6-11,
 *     24-29, 120-125): OVMX has no DECnet-style logical LAVC MAC (that
 *     scheme is unassigned pre-membership, vms-5fe); this builder uses the
 *     emitting interface's real HW MAC for all three fields, matching the
 *     real-VAX pattern observed on VAX2's early-boot HELLO (frame 4 of the
 *     baseline capture) where the same real HW MAC appears at the
 *     HELLO-tail offset. Presenting our real HW MAC at offset 24 as well
 *     (rather than fabricating a logical address we were never assigned)
 *     is the OVMX design choice for this rung; documented, not hidden.
 *   - Node name (abs 40-46): SCSNODE, ASCII, space-padded to exactly 6
 *     bytes with a length-prefix of 6 -- GROUNDED: every real HELLO
 *     observed carries namelen==6 (VAX1/VAX2/VAX3's names are all
 *     space-padded to 6, matching the documented SCSNODE 6-char maximum),
 *     so the field is reproduced as a fixed-width 6-byte name, not a
 *     variable-length one.
 *   - Timer/tick (abs 96-99): unknown/inferred, observed to increase
 *     within a capture. Caller-supplied (e.g. a per-emission counter);
 *     this is a per-sender/per-time field, not an identity field, and is
 *     expected to differ from any specific real capture.
 */
#ifndef SCS_HELLO_H
#define SCS_HELLO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCS_HELLO_SCA_LEN      120  /* total SCA content bytes, GROUNDED (spec sec 2/4b) */
#define SCS_HELLO_FRAME_LEN    134  /* 14-byte Ethernet header + SCS_HELLO_SCA_LEN */
#define SCS_HELLO_NODENAME_LEN 6    /* fixed space-padded node-name field width, GROUNDED */
#define SCS_HELLO_MCAST_GROUP1 1    /* the only grounded cluster group (the reference lab) */

struct scs_hello_params {
    uint8_t  dst_mac[6];      /* Ethernet dst + SCA dest/group logical addr;
                                  the cluster multicast address (GROUNDED for group 1) */
    uint8_t  src_mac[6];      /* Ethernet src + SCA src-logical-addr + HELLO-tail HW MAC;
                                  OVMX's real emitting-interface HW MAC (see header note) */
    char     node_name[SCS_HELLO_NODENAME_LEN + 1]; /* NUL-terminated, <=6 chars, SCSNODE */
    uint32_t timer_tick;      /* caller-supplied per-emission counter, abs offset 96-99 */
};

/*
 * scs_hello_multicast_addr - Compute the cluster multicast MAC for a group.
 *
 * GROUNDED for group==1: AB-00-04-01-01-01, byte-exact against
 * scs-idle-baseline.pcap and SYSMAN CONFIGURATION SHOW
 * CLUSTER_AUTHORIZATION ("Multicast address: AB-00-04-01-01-01"). The
 * mapping of the low two bytes to an arbitrary group number beyond 1 was
 * never observed on the wire (the reference lab only ever ran group 1) --
 * reproducing the repeated-low-byte pattern that IS grounded for group 1
 * is an inferred extrapolation, not a second grounded fact.
 */
void scs_hello_multicast_addr(uint16_t group, uint8_t mac_out[6]);

/*
 * scs_hello_build_frame - Fill out[SCS_HELLO_FRAME_LEN] with a complete
 * Ethernet+SCA multicast HELLO frame per docs/cluster-protocol-spec.md
 * sec 4(a)/4(b), using the identity in *p.
 *
 * Always builds a MULTICAST HELLO (join-nonce=0, directed-flag=0,
 * poller-sweep=0 -- the GROUNDED multicast values from spec sec 4a/4b).
 * Directed HELLOs are out of scope for this item (vms-5fe).
 *
 * Returns 0 on success, -1 if p or out is NULL, or if p->node_name is
 * longer than SCS_HELLO_NODENAME_LEN characters.
 */
int scs_hello_build_frame(const struct scs_hello_params *p,
                           uint8_t out[SCS_HELLO_FRAME_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* SCS_HELLO_H */
