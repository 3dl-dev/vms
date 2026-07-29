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

/*
 * scs_hello_build_directed_frame - Build a DIRECTED HELLO (vms-5fe), the
 * spec sec 4(b) "directed form": the channel-formation reply a node sends
 * point-to-point to a specific peer whose directed HELLO it just received.
 *
 * Identical to the multicast HELLO template (scs_hello_build_frame) except
 * for the four fields that distinguish a directed HELLO on the wire, taken
 * from a real VAX2->VAX1 directed HELLO (scs-idle-baseline.pcap frame 2,
 * decoded byte-exact):
 *
 *   - Ethernet dst (abs 0-5) and SCA dest logical addr (abs 16-21) = the
 *     peer's MAC. Pass the exact Ethernet SOURCE address of the directed
 *     HELLO you are replying to (a real HW MAC for a non-DECnet node like
 *     VAX2, or the peer's logical LAVC addr for a DECnet node like VAX1);
 *     the real wire mirrors abs 0 into abs 16, which this builder reproduces.
 *   - join nonce (abs 68-71): 4 wire-order bytes. GROUNDED as present +
 *     cross-boot-stable (spec sec 4a/4g); the VALUE is REPLAYED for a known
 *     cluster (the lab's ee-05-39-5b, group 1) -- NOT a general credential
 *     impl (see spec sec 4g "credential question", tracked as vms-732).
 *   - directed-HELLO flag / node-incarnation counter (abs 92-93) = the
 *     `incarnation` argument, LE u16. This field carries the incarnation
 *     number the sender attributes to the peer (spec sec 4b / 4i.B): 1 on a
 *     fresh/first contact, 2,3,... on successive re-forms. The joiner echoes
 *     the value the member advertised in ITS directed HELLO here (read off
 *     the wire, never hard-coded); for a first contact that is 1, matching
 *     every fresh-formation specimen. GROUNDED (spec sec 4b/4i.B).
 *   - poller-sweep marker (abs 128-129) = 0x001F (=31, SDA SHOW PORTS
 *     'Poller Sweep 31'). GROUNDED (spec sec 4b).
 *
 * The per-frame word at abs 30-31 is set to a directed value (0xb300)
 * observed on real directed HELLOs; its semantics are ungrounded (spec sec
 * 4a offset-30), so this is a documented REPLAY of an observed constant,
 * not a grounded field.
 *
 * p supplies OVMX's own identity (src_mac, node_name, timer_tick) exactly
 * as for the multicast builder; p->dst_mac is IGNORED (peer_mac is used).
 *
 * Returns 0 on success, -1 if any pointer arg is NULL or node_name is
 * longer than SCS_HELLO_NODENAME_LEN.
 */
int scs_hello_build_directed_frame(const struct scs_hello_params *p,
                                    const uint8_t peer_mac[6],
                                    const uint8_t nonce[4],
                                    uint16_t incarnation,
                                    uint8_t out[SCS_HELLO_FRAME_LEN]);

/*
 * --- vms-9f3: NISCA channel packet-size verification (padded directed HELLO,
 * spec sec 4k) ---
 *
 * Before an ESTABLISHED VAX1 will open a joining node's CSB and drive the
 * sec 4j add-member commit, the LAN channel must prove it can carry a
 * full-size packet. VAX1 sends the joiner a DIRECTED HELLO (sec 4b)
 * zero-padded up to NISCS_MAX_PKTSZ and retransmits it (1500 -> 1069 -> 853
 * -> 745, ~6 s apart) until the joiner RECIPROCATES with its own padded HELLO
 * on the reverse channel. That reciprocal is the "ack" (GROUNDED sec 4k: the
 * golden formation shows exactly ONE padded HELLO each direction, then the
 * join proceeds; an unpadded 120-byte HELLO does NOT satisfy it).
 *
 * A padded HELLO is byte-for-byte a standard 120-byte-SCA directed HELLO
 * (built by scs_hello_build_directed_frame) whose SCA content is zero-extended
 * to `total_sca_len`, with ONLY the SCA length field at abs 14-15 changed to
 * encode the larger total (LE16 = total_sca_len - 2). The pad tail (abs 134..)
 * is pure zeros -- no new field content (GROUNDED sec 4k: retransmits differ
 * only in the timer, and the smaller sizes are byte-identical prefixes of the
 * 1500-byte frame apart from the length field).
 */
#define SCS_HELLO_NISCS_MAX_PKTSZ  1498 /* SYSGEN NISCS_MAX_PKTSZ, GROUNDED (spec sec 3) */
#define SCS_HELLO_PADDED_MAX_SCA   1500 /* max total SCA content = NISCS_MAX_PKTSZ + 2, GROUNDED (sec 4k) */
#define SCS_HELLO_PADDED_MAX_FRAME (14 + SCS_HELLO_PADDED_MAX_SCA) /* 1514 bytes on the wire */

/*
 * scs_hello_build_padded_directed_frame - Build a padded directed HELLO
 * (spec sec 4k) of `total_sca_len` total SCA content bytes into out.
 *
 * Lays down the ordinary directed HELLO (scs_hello_build_directed_frame, with
 * identical p / peer_mac / nonce / incarnation semantics), zero-extends the SCA
 * content to `total_sca_len`, and rewrites ONLY the SCA length field at abs
 * 14-15 to encode the padded total. The resulting frame is 14 + total_sca_len
 * bytes on the wire; the on-wire length is written to *frame_len_out.
 *
 * `total_sca_len` must be in [SCS_HELLO_SCA_LEN (120), SCS_HELLO_PADDED_MAX_SCA
 * (1500)]; out_cap must be >= 14 + total_sca_len. A total_sca_len of 120 yields
 * a plain directed HELLO (degenerate case, no pad).
 *
 * Returns 0 on success; -1 if any pointer arg is NULL, node_name is too long,
 * total_sca_len is out of range, or out_cap is too small.
 */
int scs_hello_build_padded_directed_frame(const struct scs_hello_params *p,
                                          const uint8_t peer_mac[6],
                                          const uint8_t nonce[4],
                                          uint16_t incarnation,
                                          uint16_t total_sca_len,
                                          uint8_t *out, size_t out_cap,
                                          size_t *frame_len_out);

/* The reference-lab (cluster group 1) join nonce, wire order (abs 68-71).
 * REPLAYED for the known lab cluster only -- see the build-directed note
 * and spec sec 4(g). */
#define SCS_HELLO_LAB_NONCE_BYTES { 0xee, 0x05, 0x39, 0x5b }

#ifdef __cplusplus
}
#endif

#endif /* SCS_HELLO_H */
