/*
 * scs_connect.h - Build/parse the VMS$VAXcluster SCS connect handshake
 * (vms-5fe), spec sec 4(g) phase 4 (opcode 0x4b, 110-byte SCA class).
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md rule 8 / spec sec 0): the 110-byte SCA
 * body templates baked in here are the byte-exact contents of two real
 * connect frames captured on the reference lab wire and dumped verbatim:
 *   - CONNECT-REQUEST  = formation-ci1-joinwindow.pcap raw frame 47
 *                        (SCA#39, VAX1->VAX2: remote Con.ID 0, local
 *                        0x62C50009)
 *   - CONNECT-RESPONSE = formation-ci1-joinwindow.pcap raw frame 50
 *                        (SCA#42, VAX2->VAX1: remote 0x62C50009 echoed,
 *                        local 0x33580008)
 * No VSI/HPE source or binary was read.
 *
 * GROUNDED fields (spec sec 4d/4g) that this module positions/substitutes:
 *   - abs 30 SCS message-type byte 0x4b, abs 31 format constant 0x13
 *   - abs 64 Remote Con.ID (LE u32), abs 68 Local Con.ID (LE u32)
 *   - abs 76 / abs 92 the two ASCII "VMS$VAXcluster  " SYSAP endpoint names
 *
 * LIVE-THREADED counters (vms-c6d). The SCS sequenced-message counters are NO
 * LONGER replayed: build_from_tmpl substitutes OVMX's LIVE VC send_seq/recv_ack
 * from struct scs_connect_params (recv_ack at [18:20]/[26:28]/[34:36], send_seq
 * at [20:22] mirrored [30:32], node-incarnation echo at [22:24]) -- the same
 * GROUNDED offsets and mechanism as the 0x5b directory exchange (scs_dir.c,
 * spec sec 4h(4)). Baking the golden frame's captured 7/8 counters made the VAX
 * reject OVMX's CONNECT-RESPONSE (its live VC had advanced past the directory
 * phase to a different send_seq) and retransmit the 0x4b forever; threading the
 * live counters is what lets the VAX accept the accept.
 *
 * REPLAYED (ungrounded, spec sec 4g/sec 5) fields still left at their captured
 * values: the connect-state / inner-length body bytes (abs 56-61) and the
 * [42:56] region beyond the counters. These are tied to the live channel's
 * message flow and cannot be grounded from passive capture; replaying a real
 * frame's values is the documented lab-shortcut posture (mirrors the nonce
 * replay), NOT a general connect-body implementation. A veracity adversary
 * should treat every non-substituted byte here as a labeled replay.
 *
 * (abs 108-123 USED to be on that replay list. vms-fdd took it off: it is the
 * SCA connect-data field, and it is now measured, named and stamped. See the
 * CONNECT DATA verdict below.)
 *
 * =====================================================================
 * CONNECT DATA -- THE 16-BYTE SCA FIELD (vms-fdd, spec sec 4n)
 * =====================================================================
 *
 * VAXcluster Principles p. 2-25: the initiating SYSAP may supply up to 16 bytes
 * of connect data in CONNECT_REQ and the target SYSAP up to 16 in ACCEPT_REQ;
 * "this option is used to limit which versions of VMS can coexist... When two
 * Connection Managers form a connection with each other, they use this data to
 * effectively identify to each other which version of VMS each is associated
 * with. If the target does not approve of the source Connection Manager VMS
 * version, it rejects the request." p. 2-28 puts the field in the CDT.
 *
 * THE POPULATION IS VAX-ONLY -- OVMX'S OWN FRAMES ARE NOT EVIDENCE ABOUT VMS.
 * The lab captures are taken on a LAN where OVMX itself is a talker, and a
 * quarter of the library's connect frames were TRANSMITTED BY OVMX: 466 of
 * 1891 excluded, 55 of them VMS$VAXcluster. Counting those would be circular: the
 * census could not then tell "every VMS node does this" from "we do this, and
 * so do the VAXes we recorded alongside us". Every figure below is therefore
 * measured over VAX-SOURCED FRAMES ONLY, split on the Ethernet source MAC.
 * OVMX is identified by the hwmac scsd itself logs (it never spoofs its
 * source: scsd.c takes it from SIOCGIFHWADDR), and that blocklist is backed by
 * a structural rule -- a real lab VAX sources from 08:00:2b (DEC OUI) or
 * aa:00:04 (DECnet logical), so any other source, e.g. a locally-administered
 * Linux tap MAC, is not a VAX. The measure script FAILS on any source it
 * cannot place, so a future OVMX MAC reds it rather than rejoining the sample.
 * OVMX-sourced frames remain valid evidence about ONE thing -- what OVMX's own
 * encoder emits -- and the script reports that population separately.
 *
 * A MAC IS NOT A NODE -- HOW MANY SOURCES AGREE IS COUNTED FROM IDENTITY.
 * The source MAC is the right axis for "is this frame ours" and the WRONG axis
 * for "how many independent nodes agree", and an earlier revision of this
 * comment used it for both. Both directions of that error are real here:
 * VAX1 sources from TWO MACs (08:00:2b:4a:b7:15, its DEC NIC address, and
 * aa:00:04:00:01:04, the DECnet logical address that replaces it once DECnet
 * starts), while 08:00:2b:78:56:b9 was reconfigured across reboots and carries
 * THREE identities (VAX2 1026, VX3 1050, ZK 1099). So the counts below are
 * derived from the frames instead: every connect frame carries its sender's own
 * LAVC address at [10:16] as aa:00:04:00:NN:04, NN = SCSSYSTEMID & 1023 (spec
 * sec 4g, 0 residuals and 0 mismatches over the VAX population), and NN is
 * resolved to an ASCII node name through the 106-byte START frames. Two counts
 * follow, and they are not interchangeable:
 *   - NODE IDENTITIES: distinct cluster members. 5 here.
 *   - HARDWARE SOURCES: connected components of the MAC <-> identity graph,
 *     i.e. distinct lab machines. 3 here -- {VAX1}, {VAX3}, {VAX2, VX3, ZK}.
 * Every "independent sources agree" claim uses the HARDWARE count, because VX3
 * and ZK are the same reconfigured box as VAX2 and are not independent
 * observations of VMS behaviour. THE INDEPENDENCE FIGURE DROPPED: this comment
 * previously said "4 independent nodes", which was the MAC count; it is 3.
 * (A by-product is a second check on the population split -- the node numbers
 * the VAX population emits and the ones OVMX emits are disjoint sets.)
 *
 * WHERE IT IS -- GROUNDED. The field is the LAST 16 payload bytes of the
 * 110-byte connect class, [94:110] payload-relative (abs 108-123), directly
 * after the two 16-byte ASCII SYSAP name fields [62:78] and [78:94] that spec
 * sec 4h(2) already grounds. The 110-byte class is exactly the two connect
 * messages: over every lab capture its connection-control message type
 * ([46:48], spec sec 4h(1a)) reads {0: 1101, 2: 324, 10: 2889} VAX-sourced,
 * and all 1425 VAX type-0/type-2 frames carry an ASCII SYSAP name at [62:78]
 * with 0 residuals while the type-10 frames carry binary there. So the field
 * is claimed for CONNECT_REQ and ACCEPT_REQ only.
 *
 * WHAT IS IN IT -- GROUNDED, and it is per-SYSAP, not per-node. Census of
 * [94:110] over 48 pcaps, 1425 VAX-sourced connect frames, keyed on the local
 * SYSAP name:
 *
 *     MSCP$DISK           809 frames, 1 distinct  ASCII "V5.0          + "
 *     SCS$DIRECTORY       201 frames, 1 distinct  16 ASCII spaces
 *     SCS$DIR_LOOKUP      134 frames, 1 distinct  16 ASCII spaces
 *     SCA$TRANSPORT        32 frames, 2 distinct  02 02 01 03 ...
 *     VMS$DISK_CL_DRVR    101 frames, 5 distinct  00 00 04 a0 ...
 *     VMS$VAXcluster      148 frames, 5 distinct  01 1b 01 03 ...
 *
 * MSCP$DISK is the decisive one: a printable ASCII version string, "V5.0", in
 * the connect data of the disk-server SYSAP -- p. 2-25's "which version" read
 * straight off the wire, in the field this module now names.
 *
 * THE VMS$VAXcluster VALUE -- what is invariant. Across ALL 148 VAX-sourced
 * VMS$VAXcluster connect frames, every boot and every capture we hold (all
 * VAX/VMS V7.3), broken out by node identity:
 *
 *     VAX1 74   VAX2 32   VAX3 36   VX3 3   ZK 3     = 148 frames
 *     5 node identities, on 3 independent hardware sources
 *
 *     [94:98]   == 01 1b 01 03        148/148, 0 residuals
 *     [105:110] == 08 00 00 06 00     148/148, 0 residuals
 * The seven bytes in between, [98:105], take 5 values and are NOT grounded --
 * see the RE gap below and spec sec 5.
 *
 * THE VALUE OVMX SENDS -- and why this specific one. OVMX joins an existing
 * cluster. The only capture in the library of a REAL node doing that is
 * vax3-2to3-established-join-20260730.pcap (spec sec 1), and in it the joiner
 * VAX3 (08:00:2b:11:22:33) emits ONE connect-data value for BOTH message types
 * -- raw frame 132, its VMS$VAXcluster CONNECT_REQ to VAX1, and raw frame 210,
 * its ACCEPT_REQ answering VAX2's CONNECT_REQ:
 *
 *     01 1b 01 03 00 00 00 00 00 00 00 08 00 00 06 00
 *
 * That is scs_connect_data_vaxcluster[] below, byte for byte, and it is what
 * OVMX stamps in both builders -- because OVMX occupies exactly VAX3's role in
 * exactly that exchange. The established MEMBERS in the same capture emit a
 * DIFFERENT value (VAX1 raw frame 136, VAX2 raw frame 208, both
 * 01 1b 01 03 01 00 01 00 02 00 01 08 00 00 06 00), which is the contrast the
 * decode test asserts. Both ends of that contrast are real VAXes; OVMX is
 * present in the specimen as a bystander (209 SCA frames) but sources no
 * VMS$VAXcluster connect frame in it, and the measure script asserts that.
 *
 * NOT JUST THE SPECIMEN. Two frames would be thin evidence for a value a peer
 * is documented to reject on, so the adopted value is separately attested:
 * 40 VAX-sourced VMS$VAXcluster connect frames carry it, 38 of them OUTSIDE
 * the specimen, from 5 distinct node identities on 3 independent hardware
 * sources, across 18 captures. The measure script pins all five counts. (The
 * earlier "3 distinct VAX nodes" here was a source-MAC count; it coincided
 * with the hardware count by accident, not by derivation.)
 *
 * RE GAP, STATED (spec sec 5): what [98:105] ENCODES is unknown. All 5 values
 * it takes over the 148 VAX-sourced frames, exhaustively:
 *
 *     00 00 00 00 00 00 00   40 frames   the joining form
 *     01 00 01 00 02 00 01   59 frames
 *     01 00 01 00 03 00 01   37 frames
 *     01 00 01 00 01 00 01   11 frames
 *     01 00 00 00 02 00 01    1 frame    does NOT fit the shape below
 *
 * The nodes that emit the all-zero form are the ones joining. Four of the five
 * fit 01 00 01 00 NN 00 01 with NN in {1,2,3}, and "NN = the count of cluster
 * members the sender currently sees" fits every capture and is the best
 * reading -- but it is INFERRED, not grounded; the fifth value does not even
 * fit the shape, and one frame is too few to say whether it is a sixth state
 * or a transient. Nothing here depends on any of it: OVMX copies a real
 * joiner's bytes rather than computing them. It is NOT the member-state
 * sequence (af2-established-rejoin runs Member State Seq 2->3->4 while VAX1
 * sends NN=1 throughout) and NOT the node number (VAX1, node 1, sends NN=2 in
 * the 2-member specimen). OVMX therefore cannot yet generate a connect data
 * for a role it has not observed.
 *
 * RE-DERIVE ALL OF THE ABOVE: tools/scs_connect_data_measure.py (lab host; the
 * captures are host-only and not in git). Last run 2026-08-05: 57 checks, 0
 * failures. `ctest -R scs_connect_data_figures` needs no captures -- it asserts
 * these figures still appear verbatim here and in the spec.
 *
 * KILL SWITCH: OVMX_NO_CONNECT_DATA=1 suppresses the stamp, leaving the
 * captured template's own bytes in place -- i.e. exactly the pre-vms-fdd wire.
 * That is wire-visible in the CONNECT-REQUEST (whose golden template is VAX1's,
 * a MEMBER's frame, so its [98:105] reads 01 00 01 00 01 00 01) and a no-op in
 * the CONNECT-RESPONSE (whose golden template is VAX2's, a joiner's frame, so
 * it already carries the stamped value). Both directions are asserted in
 * tests/vmsscs/test_scs_connect.c.
 *
 * OVMX DESIGN CHOICE (not VMS-authentic, labeled per rule 8): OVMX allocates
 * its own Local Con.ID (SCS_CONNECT_OVMX_CONID_BASE | index). The value is
 * opaque to the peer (it only echoes it back), so any non-colliding 32-bit
 * value is protocol-valid; OVMX uses a recognizable base so it stands out
 * in captures.
 */
#ifndef SCS_CONNECT_H
#define SCS_CONNECT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCS_CONNECT_SCA_LEN   110 /* total SCA content bytes (GROUNDED length class) */
#define SCS_CONNECT_FRAME_LEN 124 /* 14-byte Ethernet header + SCS_CONNECT_SCA_LEN */

#define SCS_MSGTYPE_START     0x41 /* START/config (spec sec 4g phase 2) */
#define SCS_MSGTYPE_DIRLOOKUP 0x5b /* directory lookup (phase 3) */
#define SCS_MSGTYPE_SEQAPP    0x4b /* sequenced-application: connect / VC / DLM data (phase 4) */
#define SCS_MSGTYPE_CREDIT    0x48 /* credit-return short (phase 5) */
#define SCS_FORMAT_CONST      0x13 /* format/version constant (GROUNDED, spec sec 4g) */

/* Recognizable OVMX Con.ID base ("OX" | connection index). OVMX design
 * choice -- opaque to the peer (see header note). */
#define SCS_CONNECT_OVMX_CONID_BASE 0x4F580000u

/* --- vms-fdd: SCA connect data (p. 2-25 / p. 2-28), see the header verdict. */
#define SCS_CONNECT_DATA_LEN     16  /* p. 2-25 "up to 16 bytes" */
#define SCS_CONNECT_DATA_OFF     94  /* SCA payload-relative (GROUNDED) */
#define SCS_CONNECT_DATA_ABS_OFF (14 + SCS_CONNECT_DATA_OFF) /* abs 108 */

/* SCA connection-control message types at payload [46:48] (spec sec 4h(1a)).
 * Only these two carry connect data. */
#define SCS_CONN_MSGTYPE_CONNECT_REQ 0
#define SCS_CONN_MSGTYPE_ACCEPT_REQ  2

/* The VMS$VAXcluster connect data OVMX presents in both CONNECT_REQ and
 * ACCEPT_REQ: byte-exact to the joiner's value in the established-join
 * specimen (see the header verdict). */
extern const uint8_t scs_connect_data_vaxcluster[SCS_CONNECT_DATA_LEN];

/*
 * scs_connect_data_enabled - 0 when OVMX_NO_CONNECT_DATA=1 is set, in which
 * case the builders leave the captured template's own bytes at [94:110]
 * (the pre-vms-fdd wire). Cached; call the reset for a test bracket.
 */
int scs_connect_data_enabled(void);
void scs_connect_data_reset_switch_cache(void);

/*
 * scs_connect_data_get - decode the peer's connect data out of a received
 * Ethernet+SCA frame. Copies SCS_CONNECT_DATA_LEN bytes into `out` and
 * returns 0 only when the frame is a 110-byte-class SCS message
 * (format 0x13, opcode 0x4b/0x5b/0x7b) whose connection-control message type
 * is CONNECT_REQ or ACCEPT_REQ -- the ONLY population the field is grounded
 * for. Returns -1 otherwise (and leaves `out` untouched).
 */
int scs_connect_data_get(const uint8_t *frame, size_t len,
                         uint8_t out[SCS_CONNECT_DATA_LEN]);

/*
 * scs_connect_data_fmt - render 16 connect-data bytes as
 * "xx xx .. xx |ascii|" into `buf` for logging. `bufsz` must be >= 72.
 * Returns buf.
 */
const char *scs_connect_data_fmt(const uint8_t *cd, char *buf, size_t bufsz);

struct scs_connect_params {
    uint8_t  dst_mac[6];      /* Ethernet dst = peer's observed src MAC */
    uint8_t  src_mac[6];      /* Ethernet src (abs 6) = OVMX HW MAC */
    uint8_t  src_logical[6];  /* SCA src-logical addr [10:16] (abs 24) = aa:00:04:00:<LE16(sysid)>;
                                 the cluster-LOGICAL addr, NOT the raw HW MAC (vms-9f3) */
    uint8_t  peer_logical[6]; /* SCA dest logical addr (abs 16) = peer's advertised logical addr
                                 (its HELLO src-logical field); for a non-DECnet peer this is the
                                 same as dst_mac */
    uint32_t local_conid;     /* OVMX's own Con.ID (goes in the Local Con.ID field) */
    uint32_t remote_conid;    /* peer's Con.ID: 0 for a REQUEST, the peer's own for a RESPONSE */
    /* --- vms-c6d: LIVE SCS VC counters threaded into the 0x4b frame (spec sec
     * 4h(4)); NOT the golden template's replayed values. --- */
    uint16_t recv_ack;        /* [18:20]/[26:28]/[34:36] = OVMX recv_seq (peer's last send_seq) */
    uint16_t send_seq;        /* [20:22] mirrored [30:32] = OVMX's own send_seq for this frame */
    uint16_t incarnation;     /* [22:24] node-incarnation echo (0 => leave the
                               * fresh-golden template value 1; spec sec 4i.B) */
};

/*
 * scs_connect_build_request - Build a CONNECT-REQUEST (remote Con.ID = 0,
 * local Con.ID = p->local_conid). p->remote_conid is ignored (forced 0).
 * Returns 0, or -1 if p or out is NULL.
 */
int scs_connect_build_request(const struct scs_connect_params *p,
                              uint8_t out[SCS_CONNECT_FRAME_LEN]);

/*
 * scs_connect_build_response - Build a CONNECT-RESPONSE/ACCEPT (remote
 * Con.ID = p->remote_conid [the peer's, echoed], local = p->local_conid).
 * Returns 0, or -1 if p or out is NULL.
 */
int scs_connect_build_response(const struct scs_connect_params *p,
                               uint8_t out[SCS_CONNECT_FRAME_LEN]);

/* Read-only view of a received SCS-envelope frame's grounded fields. */
struct scs_connect_view {
    uint16_t total_sca_len;  /* LE u16 at abs 14 + 2 */
    uint8_t  msgtype;        /* abs 30 (0x41/0x5b/0x4b/0x48) */
    uint8_t  format;         /* abs 31 (expect 0x13) */
    uint32_t remote_conid;   /* abs 64 LE u32 (valid only for the 110/190-byte Con.ID classes) */
    uint32_t local_conid;    /* abs 68 LE u32 */
    int      has_conid;      /* 1 if total_sca_len is a Con.ID-bearing class (110 or 190) */
    /* --- vms-fdd: the peer's SCA connect data, valid only when
     * has_connect_data == 1 (110-byte class, message type CONNECT_REQ or
     * ACCEPT_REQ). conn_msgtype is [46:48] and is filled whenever the frame
     * reaches that offset. */
    uint16_t conn_msgtype;
    uint8_t  connect_data[SCS_CONNECT_DATA_LEN];
    int      has_connect_data;
};

/*
 * scs_connect_parse - Parse a received Ethernet+SCA frame's grounded SCS
 * envelope fields into *v. `frame` points at the Ethernet dst (abs 0);
 * `len` is the captured frame length. Fills msgtype/format for any frame
 * with >= abs 32 bytes, and the Con.ID pair when the length class carries
 * one (110 or 190 total SCA bytes, spec sec 4d/4g).
 *
 * Returns 0 on success, -1 if frame/v is NULL or the frame is too short to
 * hold the SCS envelope header.
 */
int scs_connect_parse(const uint8_t *frame, size_t len, struct scs_connect_view *v);

#ifdef __cplusplus
}
#endif

#endif /* SCS_CONNECT_H */
