/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_codec_hello.h - HELLO/SOLICIT typed codec entries (plan item
 * FC-P0.7; design docs/design-faithful-cluster-executive.md sec 3.2.1,
 * "Honest identity in the HELLO software field", memory
 * honest-os-identity-broadcast).
 *
 * This is the FIRST per-family harvest file layered on top of the frozen
 * vms_cluster_codec.{c,h} (FC-P0.6). It follows the plan's own convention
 * that each harvest item (P1.1 VC, P2.1 SCS, P3.1 CM, P4.5 DLM, P6.2 MSCP)
 * "teaches the codec more typed fields" -- the shared TU stays FROZEN and
 * untouched; every family gets its OWN header/source pair so the harvest
 * items can land in parallel without serializing on one file. Only
 * tests/cluster/host/CMakeLists.txt (and later src/kernel/Makefile,
 * distro Kbuild, NetBSD SRCS once FC-P0.9 links this into vms.ko) need a
 * one-line addition per family.
 *
 * SCOPE (spec sec 4(a) shared discovery header, 4(b) HELLO tail, 4(c)
 * SOLICIT, 4(k) padded directed HELLO). Every offset used here is one of
 * the VMS_OFF_* constants already declared in vms_cluster_codec.h plus the
 * VMS_OFF_HELLO_ and VMS_OFF_SOLICIT_ constants this file adds -- still,
 * per design sec 3.9 rule 2, NO raw byte offset outside a codec TU.
 *
 * THE HONESTY RULE THIS FILE EXISTS TO ENFORCE (INV-6 + the
 * honest-os-identity-broadcast ruling). The retired strawman's HELLO builder
 * baked two spans -- the abs 47-63 "capability/version-ish" span (spec sec
 * 4(a) marks it present but does NOT publish its meaning) and the abs 64-67
 * span (spec marks "unknown") -- as `static const` byte arrays copied
 * verbatim from a captured VAX's own HELLO. That is a replayed capture
 * constant: it silently asserts OVMX shares a specific VAX build's
 * capability/version bytes, which is not a fact this codec is entitled to
 * assert. Every field in `struct vms_hello_frame` below -- including the
 * ones the spec calls "constant" -- is instead a CALLER-SUPPLIED value
 * (mirroring the vms_sca_hdr_build precedent, where even the "observed
 * constant" abs-22 word -- the cluster group number, rd vms-b34 -- is
 * threaded through the struct, never baked in).
 * PARSING a real captured frame into this struct is honest (it is reading
 * real wire data); BUILDING one for OVMX's own transmit path must get every
 * byte from the executive's real state, never from this codec's memory of
 * somebody else's capture. Only the discovery-family FORMAT markers (the
 * ethertype-equivalent bytes that a receiver uses to recognise the class:
 * the 08-00-00-80/01-00-00 sandwich and the message-class byte) are baked
 * in here, exactly as vms_sca_hdr_build already bakes in VMS_SCA_ETHERTYPE.
 */
#ifndef OVMX_VMS_CLUSTER_CODEC_HELLO_H
#define OVMX_VMS_CLUSTER_CODEC_HELLO_H

#include "vms_cluster_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Field widths and absolute offsets this item teaches the codec
 * (spec sec 4(a)/4(b)/4(c)/4(k))
 * ------------------------------------------------------------------ */

#define VMS_HELLO_NODENAME_MAX     6u   /* SCSNODE, space-padded (sec 4a)   */
#define VMS_DISC_CAPSPAN_LEN      17u   /* abs 47-63, "version-ish",        */
					 /* structure UNGROUNDED (sec 4a)    */
#define VMS_DISC_RESERVED64_LEN    4u   /* abs 64-67, spec marks "unknown"  */
#define VMS_DISC_NONCE_LEN         4u   /* abs 68-71, connect/join nonce    */
#define VMS_HELLO_TAILCONST_LEN   10u   /* abs 102-111 (sec 4b)             */

#define VMS_HELLO_SCA_LEN        120u   /* full HELLO SCA content (sec 4b)  */
#define VMS_HELLO_FRAME_LEN     (VMS_ETH_HDR_LEN + VMS_HELLO_SCA_LEN) /* 134 */

/* The SECOND discovery revision (rd vms-0f8): SCA content 114, frame 128. */
#define VMS_HELLO_C3_SCA_LEN     114u
#define VMS_HELLO_C3_FRAME_LEN  (VMS_ETH_HDR_LEN + VMS_HELLO_C3_SCA_LEN) /* 128 */

#define VMS_HELLO_PADDED_MAX_SCA  1500u /* NISCS_MAX_PKTSZ 1498+2 (sec 4k)  */
#define VMS_HELLO_PADDED_MAX_FRAME (VMS_ETH_HDR_LEN + VMS_HELLO_PADDED_MAX_SCA)

/* SOLICIT: only one specimen is grounded (devspec_len==9, "_$2$DUA0:",
 * trailing pad 6); the cap below is a generous bound, not a second grounded
 * fact -- see the vms_solicit_build() doc comment. */
#define VMS_SOLICIT_DEVSPEC_MAX   64u
#define VMS_SOLICIT_TRAILPAD_LEN   6u   /* GROUNDED for the one specimen only */

/* abs offsets shared by HELLO and SOLICIT (sec 4a, abs 32-71). */
#define VMS_OFF_DISC_NAMELEN      40u
#define VMS_OFF_DISC_NAME         41u
#define VMS_OFF_DISC_CAPSPAN      47u
#define VMS_OFF_DISC_RESERVED64   64u
#define VMS_OFF_DISC_NONCE        68u

/* abs offsets of the HELLO-specific tail (sec 4b, abs 72-133). */
#define VMS_OFF_HELLO_ZEROPAD1    72u   /* abs 72-91, zero padding          */
#define VMS_HELLO_ZEROPAD1_LEN    20u
#define VMS_OFF_HELLO_INCARN      92u
#define VMS_OFF_HELLO_TR9205      94u
#define VMS_OFF_HELLO_TIMER       96u   /* 6-byte LIVE 100ns tick           */
#define VMS_OFF_HELLO_TAILCONST  102u
#define VMS_OFF_HELLO_ZEROPAD2   112u   /* abs 112-119, zero padding        */
#define VMS_HELLO_ZEROPAD2_LEN     8u
#define VMS_OFF_HELLO_HWMAC      120u
#define VMS_OFF_HELLO_TR2600     126u
#define VMS_OFF_HELLO_POLLER     128u
#define VMS_OFF_HELLO_TR0064     130u
#define VMS_OFF_HELLO_TR0000     132u

/* abs offsets of the SOLICIT-specific tail (sec 4c, abs 72-91+). */
#define VMS_OFF_SOLICIT_ZERO      72u   /* abs 72-75, zero                  */
#define VMS_SOLICIT_ZERO_LEN       4u
#define VMS_OFF_SOLICIT_DEVLEN    76u
#define VMS_OFF_SOLICIT_DEV       77u

/* ------------------------------------------------------------------ *
 * The discovery-family body shared by HELLO and SOLICIT (abs 32-71,
 * minus the format-marker bytes abs 32-35/37-39/36 which the class-
 * specific builders below bake in, exactly as they bake in the ethertype).
 * ------------------------------------------------------------------ */
struct vms_disc_body {
	uint8_t namelen;                      /* abs 40, <= VMS_HELLO_NODENAME_MAX */
	uint8_t name[VMS_HELLO_NODENAME_MAX]; /* abs 41.., ASCII space-padded      */
	uint8_t cap_span[VMS_DISC_CAPSPAN_LEN];      /* abs 47-63, UNGROUNDED --
							 caller's own honest bytes,
							 never a replayed capture  */
	uint8_t reserved_64[VMS_DISC_RESERVED64_LEN]; /* abs 64-67, "unknown"     */
	uint8_t nonce[VMS_DISC_NONCE_LEN];    /* abs 68-71, connect/join nonce     */
};

/* ------------------------------------------------------------------ *
 * THE TWO DISCOVERY REVISIONS (rd vms-0f8)
 *
 * OVMX's whole discovery corpus was harvested from OpenVMS VAX **V7.3**
 * nodes. A real OpenVMS VAX **V5.5-2H4** node speaks a second revision of the
 * SAME HELLO. Both are described HERE, as a table, because the difference is
 * DATA -- five field values and one absent tail -- and not a different
 * handshake.
 *
 * THE MEASUREMENT (the one specimen census this revision rests on; raw bytes
 * in tests/lab/captures/vms147-browser-nodea-vaxc-20260922/hub-frames.json,
 * SHA-256 in docs/clean-room/reference-captures.sha256). 53 consecutive
 * multicast HELLOs from `VAXC`, an unmodified OpenVMS VAX V5.5-2H4 system
 * disk, against 7 from OVMX in the same capture:
 *
 *   abs    V7.3 / OVMX (class 0x05)        V5.5-2H4 (class 0x03)
 *   ----   -----------------------------   -------------------------------
 *   14-15  76 00  -> SCA content 120       70 00  -> SCA content 114
 *   22-23  01 00                           01 01
 *   32-35  08 00 00 80                     08 00 00 80        IDENTICAL
 *   36     05                              03
 *   37-39  01 00 00                        01 00 00           IDENTICAL
 *   40-46  namelen + SCSNODE               namelen + SCSNODE  SAME SHAPE
 *   47-67  disc-format span, ...18 03 ..   disc-format span, ...10 03 ..
 *   68-71  join nonce                      join nonce         SAME SHAPE
 *   72-91  zero                            zero               IDENTICAL
 *   92-93  incarnation                     incarnation        SAME SHAPE
 *   94-95  92 05                           90 05
 *   96-101 live 48-bit tick                live 48-bit tick   SAME SHAPE
 *   102-111 bc 00 03 58 51 41 00 00 00 00  identical          IDENTICAL
 *   112-119 zero                           zero               IDENTICAL
 *   120-125 sender's HW MAC                sender's HW MAC    SAME SHAPE
 *   126-127 26 00                          21 00
 *   128-133 poller/0064/0000 tail          ABSENT (frame ends at 127)
 *
 * Across the 53 V5.5 frames, the ONLY bytes that change are abs 96-101 --
 * the same live tick the V7.3 revision carries at the same offset, walking
 * monotonically upward. That is what makes this a HELLO and not an unknown
 * message: same family sandwich, same node-name field, same sender HW MAC,
 * same live tick, emitted periodically to the cluster's multicast group.
 *
 * WHAT IS *NOT* CLAIMED (Rule 8 / INV-6). No meaning is assigned to the value
 * 0x03, to 0x0101, to 0x0590 or to 0x0021. Nothing here says "0x03 means
 * version 3". The differing words are carried as CALLER-SUPPLIED struct
 * fields exactly as the 0x05 revision's already are -- so the only byte this
 * codec bakes in per revision is the class byte itself, which is a format
 * marker on the same footing as the ethertype. The executive LEARNS a peer's
 * revision and its marker words off that peer's own real frame
 * (vms_pe_fsm.c), never from a table in this file.
 * ------------------------------------------------------------------ */
enum vms_hello_rev {
	/* The sec 4(a)+4(b) revision. MUST stay 0: a zeroed
	 * struct vms_hello_frame is the V7.3 revision, so every pre-vms-0f8
	 * caller keeps its exact behaviour with no edit. */
	VMS_HELLO_REV_C05 = 0,
	VMS_HELLO_REV_C03,          /* class 0x03 / 114-content (vms-0f8)   */
	VMS_HELLO_REV__COUNT
};

struct vms_hello_rev_desc {
	uint8_t     rev;          /* enum vms_hello_rev                     */
	uint8_t     disc_class;   /* abs 36 format marker for this revision */
	uint16_t    sca_content;  /* the ONE observed content length        */
	uint16_t    frame_len;    /* 14 + sca_content                       */
	uint8_t     has_tail128;  /* 1 iff abs 128-133 exists               */
	uint8_t     has_padded;   /* 1 iff a sec 4(k) padded size-verify
				   * frame has been OBSERVED in this
				   * revision. 0 is not "cannot" -- it is
				   * "never seen", and the port declines to
				   * probe rather than extrapolate one.    */
	const char *name;         /* stable id for tests and honest logs    */
};

/* NULL for an out-of-range revision. Pure. */
const struct vms_hello_rev_desc *vms_hello_rev_lookup(uint8_t rev);

/*
 * The revision a CLASSIFIED frame belongs to, or NULL if `fi` names no
 * HELLO class at all. This is the only mapping from the frame-class registry
 * into the revision table -- callers never switch on the class themselves.
 */
const struct vms_hello_rev_desc *
vms_hello_rev_for_class(const struct vms_frame_info *fi);

/* ------------------------------------------------------------------ *
 * HELLO (sec 4a + 4b). One struct/builder covers multicast, directed, and
 * the port-level "last gasp" (spec sec 4(O.30)): those are all the SAME
 * wire shape, differing only in the values placed in hdr.dst_lavc,
 * hdr.word30, and disc.nonce -- there is no separate wire structure for
 * either, so there is no separate builder either.
 * ------------------------------------------------------------------ */
struct vms_hello_frame {
	struct vms_sca_hdr   hdr;   /* abs 0-31 (vms_cluster_codec.h)          */
	struct vms_disc_body disc;  /* abs 32-71, minus the format markers     */
	uint8_t  revision;          /* enum vms_hello_rev. 0 == VMS_HELLO_REV_ */
				     /* C05, so a zeroed struct is the sec 4(b) */
				     /* revision and every pre-vms-0f8 caller   */
				     /* is unchanged. On PARSE this is what the */
				     /* wire said; on BUILD it selects which    */
				     /* revision to emit.                       */
	uint16_t incarnation;       /* abs 92-93 (sec 4b/4i.B)                 */
	uint16_t trailer_9205;      /* abs 94-95                               */
	uint8_t  timer_tick[6];     /* abs 96-101, LIVE 48-bit LE 100ns tick -- */
				     /* caller's real clock, never a frozen     */
				     /* snapshot (sec 4b/4k)                    */
	uint8_t  tail_const[VMS_HELLO_TAILCONST_LEN]; /* abs 102-111           */
	uint8_t  hw_mac[VMS_ETH_ADDR_LEN]; /* abs 120-125                     */
	uint16_t trailer_2600;      /* abs 126-127                             */
	/* The abs 128-133 tail exists ONLY in VMS_HELLO_REV_C05. On a C03
	 * frame these three words are NOT on the wire: parse leaves them zero
	 * and build never emits them. A caller that needs to know whether a
	 * zero here means "the peer sent zero" or "the revision has no such
	 * field" reads `revision` -- it is never guessed at. */
	uint16_t poller_sweep;      /* abs 128-129, C05 only                   */
	uint16_t trailer_0064;      /* abs 130-131, C05 only                   */
	uint16_t trailer_0000;      /* abs 132-133, C05 only                   */
};

/*
 * vms_hello_parse - decode a HELLO (any of multicast/directed/last-gasp/
 * padded, EITHER revision) into typed fields. Honest by construction: every
 * field is read off the real wire through the bounds-checked view; nothing is
 * asserted beyond the revision's own content length (the padded tail past
 * abs 134 is not decoded -- sec 4(k) grounds it as pure zero pad, not further
 * structure). `out->revision` records which revision the wire actually was.
 */
vms_codec_status_t vms_hello_parse(const uint8_t *frame, uint32_t len,
				   const struct vms_frame_info *fi,
				   struct vms_hello_frame *out);

/*
 * vms_hello_build - encode a HELLO from *h in the revision `h->revision`
 * names: 134 bytes for VMS_HELLO_REV_C05, 128 for VMS_HELLO_REV_C03. Bakes
 * in ONLY the discovery-family format markers (08-00-00-80 / 01-00-00 / the
 * revision's class byte); every other byte -- INCLUDING the ones the spec
 * calls "constant" and the three that DIFFER between revisions (abs 22, 94,
 * 126) -- comes from *h. `h->disc.cap_span` and `.reserved_64` MUST be the
 * executive's own honest values (or an honest zero if not yet known); this
 * function does not supply a default.
 *
 * Refuses (E_INVAL) a frame whose `hdr.sca_len_field` does not encode the
 * revision's content length: a HELLO that contradicts itself about its own
 * size is not a frame this codec will put on a wire.
 */
vms_codec_status_t vms_hello_build(const struct vms_hello_frame *h,
				   uint8_t *frame, uint32_t cap,
				   uint32_t *written);

/*
 * vms_hello_build_padded - the sec 4(k) NISCA channel packet-size
 * verification frame: a genuine `vms_hello_build()` frame whose SCA length
 * field is rewritten to `total_sca_len` and whose tail (abs 134..) is
 * zero-padded out to that length. GROUNDED: every padded specimen at the
 * probed sizes (1500/1069/853) is byte-identical to the plain directed
 * HELLO in [0,134) apart from the length field; this function reproduces
 * exactly that shape and nothing else (sec 4(k) is silent on the 745-byte
 * class's ~55-byte non-zero blob, so this codec does not claim to build
 * that size).
 *
 * `total_sca_len` must be in [VMS_HELLO_SCA_LEN, VMS_HELLO_PADDED_MAX_SCA].
 *
 * REVISION: VMS_HELLO_REV_C05 only. The sec 4(k) size ladder was measured
 * entirely on the 0x05 revision; no padded class-0x03 frame has ever been
 * observed, so a C03 *h is REFUSED (E_INVAL) rather than extrapolated.
 */
vms_codec_status_t vms_hello_build_padded(const struct vms_hello_frame *h,
					  uint16_t total_sca_len,
					  uint8_t *frame, uint32_t cap,
					  uint32_t *written);

/* ------------------------------------------------------------------ *
 * SOLICIT (sec 4c). Boot-time satellite disk-server discovery.
 * ------------------------------------------------------------------ */
struct vms_solicit_frame {
	struct vms_sca_hdr   hdr;    /* abs 0-31                              */
	struct vms_disc_body disc;   /* abs 32-71, minus the format markers   */
	uint8_t devspec_len;         /* abs 76, <= VMS_SOLICIT_DEVSPEC_MAX    */
	uint8_t devspec[VMS_SOLICIT_DEVSPEC_MAX]; /* abs 77.., ASCII          */
};

vms_codec_status_t vms_solicit_parse(const uint8_t *frame, uint32_t len,
				     const struct vms_frame_info *fi,
				     struct vms_solicit_frame *out);

/*
 * vms_solicit_build - encode a SOLICIT frame. Bakes in the discovery-family
 * format markers (class 0x02) plus the abs 72-75 zero span and the
 * VMS_SOLICIT_TRAILPAD_LEN trailing zero pad, both GROUNDED against the one
 * spec-composed specimen (devspec_len==9). A devspec of a different length
 * is built with the SAME fixed trailing-pad width; that generalisation is
 * NOT independently grounded (only one devspec length has ever been
 * observed), and is documented as such -- not a second grounded fact.
 */
vms_codec_status_t vms_solicit_build(const struct vms_solicit_frame *s,
				     uint8_t *frame, uint32_t cap,
				     uint32_t *written);

/* ------------------------------------------------------------------ *
 * Cluster-LOGICAL LAVC address helpers (spec sec 4a: aa:00:04:00:<LE16
 * (sysid)>). Not HELLO-specific -- every discovery/SCS/CM frame's src/dst
 * logical fields use this same encoding -- but this is the first item to
 * need it, so it lands here; a later harvest item may move it without a
 * behaviour change if a shared home becomes clearer.
 * ------------------------------------------------------------------ */
#define VMS_LAVC_PREFIX0 0xaau
#define VMS_LAVC_PREFIX1 0x00u
#define VMS_LAVC_PREFIX2 0x04u
#define VMS_LAVC_PREFIX3 0x00u

/* Build aa:00:04:00:<LE16(sysid)> into out[VMS_ETH_ADDR_LEN]. Pure. */
void vms_cluster_lavc_addr_build(uint16_t sysid, uint8_t out[VMS_ETH_ADDR_LEN]);

/* 1 iff addr[0..3] == aa:00:04:00 (a cluster-LOGICAL address, not a raw HW MAC). */
int vms_cluster_lavc_is_logical(const uint8_t addr[VMS_ETH_ADDR_LEN]);

/*
 * Extract the LE16 sysid from a logical address. Returns VMS_CODEC_E_INVAL
 * (and leaves *out untouched) if the address does not carry the aa:00:04:00
 * prefix -- INV-6: never hand back a sysid parsed from a non-logical address.
 */
vms_codec_status_t vms_cluster_lavc_sysid(const uint8_t addr[VMS_ETH_ADDR_LEN],
					  uint16_t *out);

/* ------------------------------------------------------------------ *
 * HELLO multicast group address: AB-00-04-01-<LE16(group + 0x100)>
 * (rd vms-147). The group number is CLUSTER_AUTHORIZE's cluster group
 * (vms_cluster.h params.auth_group, loaded off CLUSTER_AUTHORIZE.DAT at
 * boot) -- a real, per-cluster config value, never a hardcoded constant
 * (E53).
 *
 * THE +0x100 IS NOT AN OVMX INVENTION AND NOT A GUESS. It is what real
 * OpenVMS computes, derived clean-room (Rule 8) from VMS's OWN PRINTED
 * MAPPING plus two independent on-wire observations -- no VSI source, no
 * disassembly, no algorithm recomputed from a binary:
 *
 *   group 1    -> AB-00-04-01-01-01   OpenVMS VAX V7.3, lab VAX1, printed by
 *                                     VMS itself: SYSMAN> CONFIGURATION SHOW
 *                                     CLUSTER_AUTHORIZATION ->
 *                                       "Node VAX1: Cluster group number: 1"
 *                                       "Multicast address: AB-00-04-01-01-01"
 *                                     (~/vax/cluster/captures/
 *                                      sda-scs-extract-vax1.txt:423-424;
 *                                      docs/cluster-protocol-spec.md sec 3)
 *   group 257  -> AB-00-04-01-01-02   OpenVMS VAX V5.5-2H4 (browser-demo Node
 *                                     C, CLUSTER_CONFIG group 257), observed
 *                                     transmitting 0x6007 HELLOs to that
 *                                     address on the demo's in-page hub
 *   group 2026 -> AB-00-04-01-EA-08   OpenVMS Alpha V8.4 (lab-alpha ALPHA1,
 *                                     CLUSTER_CONFIG_LAN group number 2026),
 *                                     tests/lab-alpha/README.md:184/317/351
 *
 * 1+0x100=0x0101 -> LE 01 01; 257+0x100=0x0201 -> LE 01 02;
 * 2026+0x100=0x08EA -> LE EA 08. Three VMS versions, two architectures, one
 * arithmetic. The three points are what make it a DERIVATION rather than a
 * pattern: a plain LE16(group) fits group 1 alone and is WRONG for the other
 * two (it was OVMX's bug through V0.7 -- it pointed a group-257 node at the
 * group-1 address, rd vms-147); OR/XOR of 0x100 are refuted by group 257.
 *
 * Range: VMS cluster group numbers are 1..4095, so the addend never carries
 * out of the 16-bit field. Group 0 is "no group configured" (vms_pe.c
 * pe_hello_multicast) -- not a VMS-assignable group, and what VMS would do
 * with one is UNOBSERVED; the same arithmetic is applied so the executive has
 * exactly one derivation, and the port says out loud that nobody chose it.
 * ------------------------------------------------------------------ */
#define VMS_HELLO_MCAST_PREFIX0 0xabu
#define VMS_HELLO_MCAST_PREFIX1 0x00u
#define VMS_HELLO_MCAST_PREFIX2 0x04u
#define VMS_HELLO_MCAST_PREFIX3 0x01u

/* The addend VMS applies to the group number before writing it LE into the
 * last two bytes (see the three oracles above). */
#define VMS_HELLO_MCAST_GROUP_BIAS 0x0100u

/* Build ab:00:04:01:<LE16(group + VMS_HELLO_MCAST_GROUP_BIAS)> into
 * out[VMS_ETH_ADDR_LEN]. Pure. */
void vms_cluster_hello_mcast_build(uint16_t group, uint8_t out[VMS_ETH_ADDR_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_CLUSTER_CODEC_HELLO_H */
