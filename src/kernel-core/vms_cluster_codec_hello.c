// SPDX-License-Identifier: GPL-2.0
/*
 * vms_cluster_codec_hello.c - HELLO/SOLICIT typed codec entries (FC-P0.7).
 *
 * Read vms_cluster_codec_hello.h first for the field map and the honesty
 * rule this file follows: every wire byte is either a discovery-family
 * FORMAT MARKER (baked in, exactly as vms_sca_hdr_build bakes in the
 * ethertype) or a value threaded through the caller's typed struct -- never
 * a byte array copied verbatim from a captured VAX's HELLO.
 *
 * Pure, like the parent TU: no state, no allocation, no substrate call, no
 * libc. Every array copy/zero goes through the vms_wire_get_bytes/
 * vms_wire_put_bytes/vms_wire_put_zero primitives the parent TU exports, so
 * this file needs no local byte-move helpers of its own.
 */

#include "vms_cluster_codec_hello.h"

/* ------------------------------------------------------------------ *
 * HELLO (sec 4a shared header + 4b HELLO tail)
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_hello_parse(const uint8_t *frame, uint32_t len,
				   const struct vms_frame_info *fi,
				   struct vms_hello_frame *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;
	uint8_t disc_class;

	if (out == (struct vms_hello_frame *)0 ||
	    fi == (const struct vms_frame_info *)0)
		return VMS_CODEC_E_INVAL;
	if (fi->family != VMS_FFAM_DISCOVERY ||
	    (fi->cls != VMS_FCLS_HELLO && fi->cls != VMS_FCLS_HELLO_PADDED))
		return VMS_CODEC_E_CLASS;

	st = vms_sca_hdr_parse(frame, len, &out->hdr);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_view_init(&v, frame, len);
	if (!vms_wire_view_ok(&v))
		return VMS_CODEC_E_INVAL;

	/* Redundant with the classifier, but a parse function that trusts its
	 * caller's classification without checking is how a class-mismatch
	 * bug turns into wrong data instead of a refusal (INV-6). */
	disc_class = vms_wire_get_u8(&v, VMS_OFF_DISC_CLASS);
	if (vms_wire_view_ok(&v) && disc_class != VMS_DISC_CLASS_HELLO)
		return VMS_CODEC_E_CLASS;

	out->disc.namelen = vms_wire_get_u8(&v, VMS_OFF_DISC_NAMELEN);
	vms_wire_get_bytes(&v, VMS_OFF_DISC_NAME, VMS_HELLO_NODENAME_MAX,
			   out->disc.name);
	/* cap_span / reserved_64: read the real wire bytes verbatim. This is
	 * honest on PARSE (it is real data); the struct does not claim to
	 * know what the bytes MEAN (sec 4a marks both spans unknown/
	 * inferred). */
	vms_wire_get_bytes(&v, VMS_OFF_DISC_CAPSPAN, VMS_DISC_CAPSPAN_LEN,
			   out->disc.cap_span);
	vms_wire_get_bytes(&v, VMS_OFF_DISC_RESERVED64, VMS_DISC_RESERVED64_LEN,
			   out->disc.reserved_64);
	vms_wire_get_bytes(&v, VMS_OFF_DISC_NONCE, VMS_DISC_NONCE_LEN,
			   out->disc.nonce);

	out->incarnation = vms_wire_get_le16(&v, VMS_OFF_HELLO_INCARN);
	out->trailer_9205 = vms_wire_get_le16(&v, VMS_OFF_HELLO_TR9205);
	vms_wire_get_bytes(&v, VMS_OFF_HELLO_TIMER, 6, out->timer_tick);
	vms_wire_get_bytes(&v, VMS_OFF_HELLO_TAILCONST, VMS_HELLO_TAILCONST_LEN,
			   out->tail_const);
	vms_wire_get_bytes(&v, VMS_OFF_HELLO_HWMAC, VMS_ETH_ADDR_LEN,
			   out->hw_mac);
	out->trailer_2600 = vms_wire_get_le16(&v, VMS_OFF_HELLO_TR2600);
	out->poller_sweep = vms_wire_get_le16(&v, VMS_OFF_HELLO_POLLER);
	out->trailer_0064 = vms_wire_get_le16(&v, VMS_OFF_HELLO_TR0064);
	out->trailer_0000 = vms_wire_get_le16(&v, VMS_OFF_HELLO_TR0000);

	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

/* Discovery-family format markers shared by every HELLO/SOLICIT build. */
static void disc_put_format_markers(vms_wire_buf_t *w, uint8_t disc_class)
{
	static const uint8_t prefix[4] = { 0x08, 0x00, 0x00, 0x80 };
	static const uint8_t suffix[3] = { 0x01, 0x00, 0x00 };

	vms_wire_put_bytes(w, VMS_OFF_DISC_PREFIX, 4, prefix);
	vms_wire_put_u8(w, VMS_OFF_DISC_CLASS, disc_class);
	vms_wire_put_bytes(w, VMS_OFF_DISC_SUFFIX, 3, suffix);
}

/* The discovery body shared by HELLO and SOLICIT builds (abs 40-71). */
static void disc_put_body(vms_wire_buf_t *w, const struct vms_disc_body *d)
{
	vms_wire_put_u8(w, VMS_OFF_DISC_NAMELEN, d->namelen);
	vms_wire_put_bytes(w, VMS_OFF_DISC_NAME, VMS_HELLO_NODENAME_MAX, d->name);
	vms_wire_put_bytes(w, VMS_OFF_DISC_CAPSPAN, VMS_DISC_CAPSPAN_LEN,
			   d->cap_span);
	vms_wire_put_bytes(w, VMS_OFF_DISC_RESERVED64, VMS_DISC_RESERVED64_LEN,
			   d->reserved_64);
	vms_wire_put_bytes(w, VMS_OFF_DISC_NONCE, VMS_DISC_NONCE_LEN, d->nonce);
}

vms_codec_status_t vms_hello_build(const struct vms_hello_frame *h,
				   uint8_t *frame, uint32_t cap,
				   uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;
	uint32_t hdr_written = 0;

	if (h == (const struct vms_hello_frame *)0)
		return VMS_CODEC_E_INVAL;

	st = vms_sca_hdr_build(&h->hdr, frame, cap, &hdr_written);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	disc_put_format_markers(&w, VMS_DISC_CLASS_HELLO);
	disc_put_body(&w, &h->disc);

	vms_wire_put_zero(&w, VMS_OFF_HELLO_ZEROPAD1, VMS_HELLO_ZEROPAD1_LEN);
	vms_wire_put_le16(&w, VMS_OFF_HELLO_INCARN, h->incarnation);
	vms_wire_put_le16(&w, VMS_OFF_HELLO_TR9205, h->trailer_9205);
	vms_wire_put_bytes(&w, VMS_OFF_HELLO_TIMER, 6, h->timer_tick);
	vms_wire_put_bytes(&w, VMS_OFF_HELLO_TAILCONST, VMS_HELLO_TAILCONST_LEN,
			   h->tail_const);
	vms_wire_put_zero(&w, VMS_OFF_HELLO_ZEROPAD2, VMS_HELLO_ZEROPAD2_LEN);
	vms_wire_put_bytes(&w, VMS_OFF_HELLO_HWMAC, VMS_ETH_ADDR_LEN, h->hw_mac);
	vms_wire_put_le16(&w, VMS_OFF_HELLO_TR2600, h->trailer_2600);
	vms_wire_put_le16(&w, VMS_OFF_HELLO_POLLER, h->poller_sweep);
	vms_wire_put_le16(&w, VMS_OFF_HELLO_TR0064, h->trailer_0064);
	vms_wire_put_le16(&w, VMS_OFF_HELLO_TR0000, h->trailer_0000);

	if (!vms_wire_buf_ok(&w))
		return w.err;

	if (written != (uint32_t *)0)
		*written = VMS_HELLO_FRAME_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_hello_build_padded(const struct vms_hello_frame *h,
					  uint16_t total_sca_len,
					  uint8_t *frame, uint32_t cap,
					  uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;
	uint32_t frame_len;
	uint32_t base_written = 0;

	if (h == (const struct vms_hello_frame *)0)
		return VMS_CODEC_E_INVAL;
	if (total_sca_len < VMS_HELLO_SCA_LEN ||
	    total_sca_len > VMS_HELLO_PADDED_MAX_SCA)
		return VMS_CODEC_E_INVAL;

	frame_len = (uint32_t)VMS_ETH_HDR_LEN + (uint32_t)total_sca_len;
	if (cap < frame_len)
		return VMS_CODEC_E_RANGE;

	/* Zero the pad tail FIRST -- sec 4(k): every probed size is a genuine
	 * 134-byte HELLO followed by pure zero, never caller-buffer garbage. */
	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;
	vms_wire_put_zero(&w, VMS_HELLO_FRAME_LEN, frame_len - VMS_HELLO_FRAME_LEN);
	if (!vms_wire_buf_ok(&w))
		return w.err;

	st = vms_hello_build(h, frame, cap, &base_written);
	if (st != VMS_CODEC_OK)
		return st;

	/* Rewrite ONLY the SCA length field to encode the padded total (sec 2
	 * length identity: LE16 == total_sca_len - 2), independent of what
	 * h->hdr.sca_len_field carried for the un-padded case. */
	vms_wire_buf_init(&w, frame, cap);
	vms_wire_put_le16(&w, VMS_OFF_SCA_LEN, (uint16_t)(total_sca_len - 2u));
	if (!vms_wire_buf_ok(&w))
		return w.err;

	if (written != (uint32_t *)0)
		*written = frame_len;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * SOLICIT (sec 4c)
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_solicit_parse(const uint8_t *frame, uint32_t len,
				     const struct vms_frame_info *fi,
				     struct vms_solicit_frame *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;
	uint8_t disc_class, devlen;

	if (out == (struct vms_solicit_frame *)0 ||
	    fi == (const struct vms_frame_info *)0)
		return VMS_CODEC_E_INVAL;
	if (fi->family != VMS_FFAM_DISCOVERY || fi->cls != VMS_FCLS_SOLICIT)
		return VMS_CODEC_E_CLASS;

	st = vms_sca_hdr_parse(frame, len, &out->hdr);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_view_init(&v, frame, len);
	if (!vms_wire_view_ok(&v))
		return VMS_CODEC_E_INVAL;

	disc_class = vms_wire_get_u8(&v, VMS_OFF_DISC_CLASS);
	if (vms_wire_view_ok(&v) && disc_class != VMS_DISC_CLASS_SOLICIT)
		return VMS_CODEC_E_CLASS;

	out->disc.namelen = vms_wire_get_u8(&v, VMS_OFF_DISC_NAMELEN);
	vms_wire_get_bytes(&v, VMS_OFF_DISC_NAME, VMS_HELLO_NODENAME_MAX,
			   out->disc.name);
	vms_wire_get_bytes(&v, VMS_OFF_DISC_CAPSPAN, VMS_DISC_CAPSPAN_LEN,
			   out->disc.cap_span);
	vms_wire_get_bytes(&v, VMS_OFF_DISC_RESERVED64, VMS_DISC_RESERVED64_LEN,
			   out->disc.reserved_64);
	vms_wire_get_bytes(&v, VMS_OFF_DISC_NONCE, VMS_DISC_NONCE_LEN,
			   out->disc.nonce);

	devlen = vms_wire_get_u8(&v, VMS_OFF_SOLICIT_DEVLEN);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (devlen > VMS_SOLICIT_DEVSPEC_MAX)
		return VMS_CODEC_E_RANGE;
	out->devspec_len = devlen;
	vms_wire_get_bytes(&v, VMS_OFF_SOLICIT_DEV, devlen, out->devspec);

	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_solicit_build(const struct vms_solicit_frame *s,
				     uint8_t *frame, uint32_t cap,
				     uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;
	uint32_t hdr_written = 0;
	uint32_t trail_off;
	uint32_t total;

	if (s == (const struct vms_solicit_frame *)0)
		return VMS_CODEC_E_INVAL;
	if (s->devspec_len > VMS_SOLICIT_DEVSPEC_MAX)
		return VMS_CODEC_E_INVAL;

	st = vms_sca_hdr_build(&s->hdr, frame, cap, &hdr_written);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	disc_put_format_markers(&w, VMS_DISC_CLASS_SOLICIT);
	disc_put_body(&w, &s->disc);

	vms_wire_put_zero(&w, VMS_OFF_SOLICIT_ZERO, VMS_SOLICIT_ZERO_LEN);
	vms_wire_put_u8(&w, VMS_OFF_SOLICIT_DEVLEN, s->devspec_len);
	vms_wire_put_bytes(&w, VMS_OFF_SOLICIT_DEV, s->devspec_len, s->devspec);

	trail_off = VMS_OFF_SOLICIT_DEV + s->devspec_len;
	vms_wire_put_zero(&w, trail_off, VMS_SOLICIT_TRAILPAD_LEN);

	if (!vms_wire_buf_ok(&w))
		return w.err;

	total = trail_off + VMS_SOLICIT_TRAILPAD_LEN;
	if (written != (uint32_t *)0)
		*written = total;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * Cluster-LOGICAL LAVC address helpers (sec 4a)
 * ------------------------------------------------------------------ */

void vms_cluster_lavc_addr_build(uint16_t sysid, uint8_t out[VMS_ETH_ADDR_LEN])
{
	if (out == (uint8_t *)0)
		return;
	out[0] = VMS_LAVC_PREFIX0;
	out[1] = VMS_LAVC_PREFIX1;
	out[2] = VMS_LAVC_PREFIX2;
	out[3] = VMS_LAVC_PREFIX3;
	out[4] = (uint8_t)(sysid & 0xffu);
	out[5] = (uint8_t)((sysid >> 8) & 0xffu);
}

int vms_cluster_lavc_is_logical(const uint8_t addr[VMS_ETH_ADDR_LEN])
{
	if (addr == (const uint8_t *)0)
		return 0;
	return addr[0] == VMS_LAVC_PREFIX0 && addr[1] == VMS_LAVC_PREFIX1 &&
	       addr[2] == VMS_LAVC_PREFIX2 && addr[3] == VMS_LAVC_PREFIX3;
}

vms_codec_status_t vms_cluster_lavc_sysid(const uint8_t addr[VMS_ETH_ADDR_LEN],
					  uint16_t *out)
{
	if (addr == (const uint8_t *)0 || out == (uint16_t *)0)
		return VMS_CODEC_E_INVAL;
	if (!vms_cluster_lavc_is_logical(addr))
		return VMS_CODEC_E_INVAL;
	*out = (uint16_t)((uint16_t)addr[4] | ((uint16_t)addr[5] << 8));
	return VMS_CODEC_OK;
}
