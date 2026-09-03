/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_codec_blk.h - the SCA BLOCK DATA TRANSFER header, as a typed
 * codec family (plan item FC-P6.1; design
 * docs/design-faithful-cluster-executive.md sec 3.2 "the three port services",
 * sec 3.9 rule 2 "no raw wire offset outside a codec TU").
 *
 * WHY ITS OWN FAMILY FILE. Block transfer is the PORT's third service, beside
 * the datagram and the sequenced message -- not an SCS message class and not a
 * SYSAP body. Its 28-byte header occupies the SAME frame span (abs 56..83) an
 * SCS message's own 56-71 envelope would, which is exactly why a
 * block-transfer frame FAILS the SCS envelope conformance test and must never
 * be handed to an SCS parser. Giving it the sibling-file treatment every other
 * harvest family got (hello / vc / scs / cm / dlm / mscp) keeps that
 * distinction structural instead of a comment.
 *
 * CLEAN-ROOM PROVENANCE (Rule 8). The field table below is transcribed from
 * this project's own clean-room OBSERVATION of a real VAX serving a disk to
 * another real VAX (lab-2 `vaxlab-9`, 2026-08-06, the `vms291-mount-A` /
 * `vms291-control-B` / `vms291-boot-C` capture set) as it is RECORDED IN THE
 * TREE at docs/design-mscp-direction.md, "Phase D part 1's lab capture -- SCA
 * block data transfer, DECODED". No VSI/HPE/DEC binary was disassembled and no
 * VMS source was read; the named-buffer MECHANISM itself is described (without
 * bytes) in the public *VAXcluster Principles* pp. 2-32..2-41, and the MSCP
 * buffer descriptor that carries the buffer name is public AA-L619A-TK
 * Table A-6 offset 16.
 *
 * ***  THE TWO UNGROUNDED WORDS -- READ THIS BEFORE ADDING A DECODE  ***
 *
 * The header's `+4` and `+6` words are OBSERVED, NOT UNDERSTOOD. What the
 * capture establishes, and the ONLY thing it establishes, is:
 *
 *     +4  held one value for the whole life of one connection (9 on one
 *         connection, 13 on another). It is NOT a message type despite
 *         resembling one at that offset.
 *     +6  held one value across every frame of a single transfer, and a
 *         different one on the next transfer.
 *
 * Neither the rule that PRODUCES those values nor their meaning is known, and
 * docs/design-mscp-direction.md lists both under "Still ungrounded, do not
 * build on". So this codec:
 *
 *   - NAMES THEM AFTER THEIR OFFSET (`obs_w4`, `obs_w6`) and never after a
 *     meaning. A field called `conn_const`/`xfer_const` would be asserting the
 *     decode this file is refusing to make.
 *   - BAKES IN NOTHING. There is no `#define VMS_BLK_W4_OBSERVED 9`. The 9 and
 *     the 13 belonged to two particular connections of one particular pair of
 *     VAXen; shipping either as a constant would put a captured node's private
 *     value on OVMX's wire, which is the placeholder INV-6 exists to forbid.
 *   - CARRIES THEM THROUGH UNCHANGED. Build takes them from the caller; parse
 *     hands them back. Carrying a value through unchanged is not the same
 *     claim as decoding it. The port (vms_pe_fsm.c) fills them ONLY from a
 *     value it LEARNED off a received block-transfer frame on that very
 *     circuit, and otherwise writes an explicit zero and counts it -- the same
 *     honest-absence shape pe_fsm_send_last_gasp already uses for an absent
 *     cluster nonce.
 *
 * THE HEADER (abs 56..83; the offsets are the field table recorded in
 * docs/design-mscp-direction.md, one row per line):
 *
 *   +0   4  destination connection ID (the same value the MSCP envelope for
 *           this connection carries)
 *   +4   2  OBSERVED, NOT DECODED -- see above
 *   +6   2  OBSERVED, NOT DECODED -- see above
 *   +8   4  bytes remaining in THIS transfer, INCLUDING this frame's own
 *           data (counts DOWN)
 *   +12  4  source buffer name
 *   +16  4  destination offset within the destination buffer
 *   +20  4  destination buffer name
 *   +24  4  source offset
 *   +28  N  the data (N may be 0 -- see WRITE, below)
 *
 * TWO FRAMING FACTS the manual does not give and the capture does:
 *
 *   READ streams standalone block frames and PIGGYBACKS the transfer's final,
 *   possibly-partial chunk into the SAME Ethernet frame as the MSCP end
 *   message, past the length that end message's own inner header declares.
 *   The five READ-END SCA content lengths recorded from the capture --
 *   118/194/448/630/1142 -- are each exactly (58 + 32) + 28 + {0, 76, 330,
 *   512, 1024}: the 90-content end message, this 28-byte header, and a data
 *   tail. vms_blk_trailer_build()/vms_blk_trailer_parse() are that shape, and
 *   the parse side takes the frame's REAL received length because a
 *   block-transfer message carries no length field of its own.
 *
 *   WRITE is a two-frame request/response whose two 28-byte headers are
 *   BYTE-IDENTICAL; only the PRESENCE OF DATA distinguishes them. So a parser
 *   keying on the header alone cannot tell the halves apart, and
 *   vms_blk_frame_parse() does not try to: it reports `data_len`, which is 0
 *   for the header-only half and is a legitimate state, not a parse failure.
 *
 * WHAT THIS FILE DELIBERATELY DOES NOT DO: identify the class. See
 * vms_blk_frame_structural_ok() -- structure alone CANNOT decide whether a
 * received sequenced frame is a block transfer, and the port's real
 * discriminator is a positive lookup of the destination buffer NAME in its own
 * registered-buffer table (executive state), not a guess about bytes.
 */
#ifndef OVMX_VMS_CLUSTER_CODEC_BLK_H
#define OVMX_VMS_CLUSTER_CODEC_BLK_H

#include "vms_cluster_codec.h"
#include "vms_cluster_codec_hello.h"   /* VMS_HELLO_PADDED_MAX_SCA: the one
					 * NISCS_MAX_PKTSZ grounding in tree */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * sec 1  Geometry
 * ------------------------------------------------------------------ */

/*
 * abs 56 == SCA content offset 42 -- the same span an SCS message's own
 * 56-71 envelope occupies (VMS_OFF_SCSCTRL_INNERLEN is 56 for exactly that
 * reason). That collision is the wire's, not a choice made here.
 */
#define VMS_BLK_HDR_OFF   56u
#define VMS_BLK_HDR_LEN   28u
#define VMS_BLK_DATA_OFF  (VMS_BLK_HDR_OFF + VMS_BLK_HDR_LEN)   /* abs 84  */

/* Field offsets, ABSOLUTE (the codec's own vocabulary; no caller ever adds
 * VMS_BLK_HDR_OFF to a number itself -- design sec 3.9 rule 2). */
#define VMS_OFF_BLK_DEST_CONID (VMS_BLK_HDR_OFF +  0u)
#define VMS_OFF_BLK_OBS_W4     (VMS_BLK_HDR_OFF +  4u)
#define VMS_OFF_BLK_OBS_W6     (VMS_BLK_HDR_OFF +  6u)
#define VMS_OFF_BLK_REMAINING  (VMS_BLK_HDR_OFF +  8u)
#define VMS_OFF_BLK_SRC_NAME   (VMS_BLK_HDR_OFF + 12u)
#define VMS_OFF_BLK_DST_OFFSET (VMS_BLK_HDR_OFF + 16u)
#define VMS_OFF_BLK_DST_NAME   (VMS_BLK_HDR_OFF + 20u)
#define VMS_OFF_BLK_SRC_OFFSET (VMS_BLK_HDR_OFF + 24u)

/*
 * The frame cap. Spec sec 1 Table 2 records the block-transfer class capping
 * out "at exactly NISCS_MAX_PKTSZ=1498, GROUNDED against the SYSGEN tunable",
 * which is the SAME tunable VMS_HELLO_PADDED_MAX_SCA already carries for the
 * sec 4(k) padded HELLO. Derived from it rather than re-stated, so there is one
 * place in the tree where that number lives.
 */
#define VMS_BLK_FRAME_MAX  (VMS_ETH_HDR_LEN + VMS_HELLO_PADDED_MAX_SCA)
#define VMS_BLK_DATA_MAX   (VMS_BLK_FRAME_MAX - VMS_BLK_DATA_OFF)

/* ------------------------------------------------------------------ *
 * sec 2  The typed header
 * ------------------------------------------------------------------ */

struct vms_blk_hdr {
	uint32_t dest_conid;       /* +0                                    */
	uint16_t obs_w4;           /* +4  OBSERVED, NOT DECODED (file hdr)  */
	uint16_t obs_w6;           /* +6  OBSERVED, NOT DECODED (file hdr)  */
	uint32_t bytes_remaining;  /* +8  counts DOWN, includes this frame  */
	uint32_t src_name;         /* +12 source buffer NAME                */
	uint32_t dst_offset;       /* +16 offset in the destination buffer  */
	uint32_t dst_name;         /* +20 destination buffer NAME           */
	uint32_t src_offset;       /* +24 offset in the source buffer       */
};

/*
 * vms_blk_hdr_build_at / vms_blk_hdr_build - lay the 28 bytes down at `at`
 * (the trailer case, where `at` is the length of the message already built)
 * or at the class's own VMS_BLK_HDR_OFF. Neither zero-fills anything outside
 * the 28 bytes: the port owns abs 0..55 and builds it through its own codec
 * entry before calling this.
 *
 * VMS_CODEC_E_INVAL for a NULL argument, VMS_CODEC_E_SHORT when `cap` cannot
 * hold `at`..`at`+27.
 */
vms_codec_status_t vms_blk_hdr_build_at(const struct vms_blk_hdr *h,
					uint8_t *frame, uint32_t cap,
					uint32_t at);
vms_codec_status_t vms_blk_hdr_build(const struct vms_blk_hdr *h,
				     uint8_t *frame, uint32_t cap);

/* The mirror pair. `at` is where the header starts. */
vms_codec_status_t vms_blk_hdr_parse_at(const uint8_t *frame, uint32_t len,
					uint32_t at, struct vms_blk_hdr *out);
vms_codec_status_t vms_blk_hdr_parse(const uint8_t *frame, uint32_t len,
				     struct vms_blk_hdr *out);

/* ------------------------------------------------------------------ *
 * sec 3  A whole block-transfer frame
 * ------------------------------------------------------------------ */

/*
 * `data`/`data_len` is what was ACTUALLY PRESENT in the bytes handed in --
 * never a declared length, because this message class declares none.
 * data_len == 0 with data == NULL is a real, valid state: it is WRITE's
 * header-only half.
 */
struct vms_blk_view {
	struct vms_blk_hdr hdr;
	const uint8_t *data;
	uint32_t       data_len;
};

/*
 * vms_blk_frame_parse - parse a standalone block-transfer frame. `len` MUST be
 * the frame's REAL, received length. Class-gated only on the frame being a
 * sequenced SCS frame at all (VMS_FCAP_SEQ): see
 * vms_blk_frame_structural_ok() for why nothing stronger is possible here.
 */
vms_codec_status_t vms_blk_frame_parse(const uint8_t *frame, uint32_t len,
				       const struct vms_frame_info *fi,
				       struct vms_blk_view *out);

/*
 * vms_blk_frame_structural_ok - 1 iff `frame` COULD be a block-transfer frame:
 * it is long enough to hold the header, and abs 58..59 is not the SCS
 * envelope's GROUNDED format word 0x0004 (in a block-transfer frame those two
 * bytes are the upper half of the 32-bit destination connection ID).
 *
 * ***  THIS IS NOT A CLASS TEST AND MUST NOT BE USED AS ONE.  ***  It is a
 * NEGATIVE, and spec sec 4(h)(1d) already names one other class -- the
 * 70-content one -- that also fails the 0x0004 test while being something else
 * entirely. A receiver that routed on this alone would hand undecoded SCS
 * frames to a block-transfer path.
 *
 * The REAL discriminator is a positive fact the executive holds: a received
 * frame is a block transfer for this node when its DESTINATION BUFFER NAME
 * matches a buffer this port actually registered. That is the correlation the
 * capture grounds ("the name in the READ/WRITE command is exactly the name
 * that appears in the block-transfer frames"), and pe_blk_rx_try()
 * (vms_pe_fsm.c) implements exactly it, with this function as the cheap
 * structural precondition in front of the lookup.
 */
int vms_blk_frame_structural_ok(const uint8_t *frame, uint32_t len);

/* ------------------------------------------------------------------ *
 * sec 4  The READ end-message piggyback (the capture's TRAP 1)
 * ------------------------------------------------------------------ */

/*
 * vms_blk_trailer_build - append a block-transfer header plus `data_len` data
 * bytes to an already-built frame of length `frame_len`, and report the new
 * total in *total_out. It does NOT touch the frame's SCA length field: fixing
 * that up is vms_scs_seq_envelope_fixup_len()'s job and the caller does it
 * once, after this, with the real total (the port does exactly that).
 *
 * `data` may be NULL iff `data_len` is 0 -- the header-only trailer, which is
 * the recorded 118-content READ end (90 + 28 + 0).
 */
vms_codec_status_t vms_blk_trailer_build(const struct vms_blk_hdr *h,
					 const uint8_t *data, uint32_t data_len,
					 uint8_t *frame, uint32_t cap,
					 uint32_t frame_len,
					 uint32_t *total_out);

/*
 * vms_blk_trailer_parse - TRAP 1's receive side. `frame_len` is the frame's
 * REAL received length; `inner_frame_len` is where the inner (end) message
 * stops -- i.e. the total length that message's OWN builder would have
 * produced.
 *
 * THE POINT OF THIS FUNCTION IS ITS `frame_len` ARGUMENT. A caller that
 * derived its bound from the inner message's declared length instead would see
 * no trailer, ever -- which is TRAP 1 exactly as a real receive path hits it.
 *
 * *out is zeroed and data_len left 0 (NOT an error) when frame_len <=
 * inner_frame_len, i.e. no trailer is present. VMS_CODEC_E_SHORT when a
 * trailer IS present but is too short to hold even its 28-byte header.
 */
vms_codec_status_t vms_blk_trailer_parse(const uint8_t *frame,
					 uint32_t frame_len,
					 uint32_t inner_frame_len,
					 struct vms_blk_view *out);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_CLUSTER_CODEC_BLK_H */
