// SPDX-License-Identifier: GPL-2.0
/*
 * vms_frame_compose.c - TEST-ONLY full-frame composer. See
 * vms_frame_compose.h for the contract; this is exactly the demoted
 * vms_cm_link_build() body (FC-P3.1), moved here unchanged by FC-P3.15.
 */

#include "vms_frame_compose.h"

vms_codec_status_t vms_frame_compose_link(const struct vms_cm_link *l,
					  uint8_t *frame, uint32_t cap,
					  uint32_t *written)
{
	struct vms_sca_hdr h;
	vms_wire_buf_t w;
	vms_codec_status_t st;
	uint32_t hdr_written = 0;

	if (l == (const struct vms_cm_link *)0)
		return VMS_CODEC_E_INVAL;

	/* Every frame this composer builds is a 190-byte-content CM message on
	 * the SCS format-0x13 sequenced class (spec sec 4(d)/(g)) -- the caller's
	 * hdr.sca_len_field and hdr.word30 are IGNORED (see the header's field
	 * doc). */
	h = l->hdr;
	h.sca_len_field = (uint16_t)(VMS_CM_SCA_CONTENT - 2u);
	h.word30 = (uint16_t)((uint16_t)VMS_SCS_MT_MSG |
			      ((uint16_t)VMS_SCS_FORMAT_V13 << 8));

	st = vms_sca_hdr_build(&h, frame, cap, &hdr_written);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	/* abs [32,72): zero the whole span first -- the counter-mirror region
	 * spec sec 4(d) itself calls "inferred, not independently confirmed"
	 * stays honest residue, never a guessed value -- then lay down exactly
	 * the GROUNDED fields on top. */
	vms_wire_put_zero(&w, VMS_OFF_SCS_RECV_ACK,
			  VMS_OFF_SYSAP_BODY - VMS_OFF_SCS_RECV_ACK);
	vms_wire_put_le16(&w, VMS_OFF_SCS_RECV_ACK, l->recv_ack);
	vms_wire_put_le16(&w, VMS_OFF_SCS_SEND_SEQ, l->send_seq);
	vms_wire_put_le16(&w, VMS_OFF_CM_LINK_OVRHD, VMS_CM_LINK_OVRHD_VAL);
	vms_wire_put_le32(&w, VMS_OFF_SCS_CONID_REMOTE, l->remote_conid);
	vms_wire_put_le32(&w, VMS_OFF_SCS_CONID_LOCAL, l->local_conid);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_OFF_SYSAP_BODY;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_frame_compose(const struct vms_cm_link *l,
				     const uint8_t body[VMS_CM_BODY_LEN],
				     uint8_t *frame, uint32_t cap,
				     uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;
	uint32_t link_written = 0;

	if (body == (const uint8_t *)0 || frame == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;

	st = vms_frame_compose_link(l, frame, cap, &link_written);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;
	vms_wire_put_bytes(&w, VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN, body);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_CM_FRAME_LEN;
	return VMS_CODEC_OK;
}
