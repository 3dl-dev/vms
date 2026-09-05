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
 * constant" connect_flag is threaded through the struct, never baked in).
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
 * HELLO (sec 4a + 4b). One struct/builder covers multicast, directed, and
 * the port-level "last gasp" (spec sec 4(O.30)): those are all the SAME
 * wire shape, differing only in the values placed in hdr.dst_lavc,
 * hdr.word30, and disc.nonce -- there is no separate wire structure for
 * either, so there is no separate builder either.
 * ------------------------------------------------------------------ */
struct vms_hello_frame {
	struct vms_sca_hdr   hdr;   /* abs 0-31 (vms_cluster_codec.h)          */
	struct vms_disc_body disc;  /* abs 32-71, minus the format markers     */
	uint16_t incarnation;       /* abs 92-93 (sec 4b/4i.B)                 */
	uint16_t trailer_9205;      /* abs 94-95                               */
	uint8_t  timer_tick[6];     /* abs 96-101, LIVE 48-bit LE 100ns tick -- */
				     /* caller's real clock, never a frozen     */
				     /* snapshot (sec 4b/4k)                    */
	uint8_t  tail_const[VMS_HELLO_TAILCONST_LEN]; /* abs 102-111           */
	uint8_t  hw_mac[VMS_ETH_ADDR_LEN]; /* abs 120-125                     */
	uint16_t trailer_2600;      /* abs 126-127                             */
	uint16_t poller_sweep;      /* abs 128-129                             */
	uint16_t trailer_0064;      /* abs 130-131                             */
	uint16_t trailer_0000;      /* abs 132-133                             */
};

/*
 * vms_hello_parse - decode a HELLO (any of multicast/directed/last-gasp/
 * padded) into typed fields. Honest by construction: every field is read
 * off the real wire through the bounds-checked view; nothing is asserted
 * beyond VMS_HELLO_SCA_LEN (the padded tail past abs 134 is not decoded --
 * sec 4(k) grounds it as pure zero pad, not further structure).
 */
vms_codec_status_t vms_hello_parse(const uint8_t *frame, uint32_t len,
				   const struct vms_frame_info *fi,
				   struct vms_hello_frame *out);

/*
 * vms_hello_build - encode a 134-byte HELLO from *h. Bakes in ONLY the
 * discovery-family format markers (08-00-00-80 / 01-00-00 / class 0x05);
 * every other byte -- INCLUDING the ones the spec calls "constant" --
 * comes from *h. `cap.disc.cap_span` and `.reserved_64` MUST be the
 * executive's own honest values (or an honest zero if not yet known);
 * this function does not supply a default.
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
 * HELLO multicast group address (spec sec 4a: AB-00-04-01-<LE16(group)>).
 * The group number is CLUSTER_AUTHORIZE's cluster group (vms_cluster.h
 * params.auth_group, loaded off CLUSTER_AUTHORIZE.DAT at boot) -- a real,
 * per-cluster config value, never a hardcoded constant (E53). Same LE16
 * convention as the LAVC prefix above (out[4]=low byte, out[5]=high byte),
 * confirmed against the lab VAX cluster's observed group 257 (0x0101) ->
 * ab:00:04:01:01:01 on the wire.
 * ------------------------------------------------------------------ */
#define VMS_HELLO_MCAST_PREFIX0 0xabu
#define VMS_HELLO_MCAST_PREFIX1 0x00u
#define VMS_HELLO_MCAST_PREFIX2 0x04u
#define VMS_HELLO_MCAST_PREFIX3 0x01u

/* Build ab:00:04:01:<LE16(group)> into out[VMS_ETH_ADDR_LEN]. Pure. */
void vms_cluster_hello_mcast_build(uint16_t group, uint8_t out[VMS_ETH_ADDR_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_CLUSTER_CODEC_HELLO_H */
