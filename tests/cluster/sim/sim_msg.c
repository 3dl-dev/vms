/* SPDX-License-Identifier: GPL-2.0 */
/* sim_msg.c - the stand-in upper layer. See sim_msg.h for what it may and may
 * not assert. */

#include <string.h>

#include "sim.h"
#include "sim_msg.h"

#include "vms_cluster_codec.h"
#include "vms_cluster_codec_vc.h"

/*
 * The harness's own Con.ID for a node, in the shape §4(d) grounds (a longword
 * whose low half indexes a table and whose high half is a reuse tag). It is a
 * TEST identity: no CDT exists until FC-P2.2, and inventing a plausible-looking
 * one would be a claim this stack cannot support. The 0x51_4d prefix spells
 * "SIM" badly on purpose, so a Con.ID in a dump is obviously the harness's.
 */
static uint32_t sim_conid(const struct sim_node *n)
{
	return 0x514d0000u | (uint32_t)n->index;
}

/*
 * Build one 190-content sequenced message, addressed from the CIRCUIT's own
 * learned addressing. Returns its length, or 0 if there is no such circuit
 * (and then nothing is sent -- the harness does not invent an address).
 */
static uint32_t sim_build_msg(struct sim_node *from, uint16_t to_sysid,
			      uint32_t dst_conid, uint32_t src_conid,
			      uint8_t *out, uint32_t cap)
{
	struct vms_scs_addr a;
	struct vms_sca_hdr h;
	struct vms_frame_info fi;
	vms_wire_buf_t w;
	uint32_t written = 0u;

	if (cap < SIM_MSG_LEN)
		return 0u;
	if (pe_vc_addr(&from->fsm, (vms_scs_sysid_t)to_sysid, &a) != 0)
		return 0u;

	memset(out, 0, SIM_MSG_LEN);
	memset(&h, 0, sizeof(h));
	memcpy(h.eth_dst, a.dst_mac, 6);
	memcpy(h.eth_src, a.src_mac, 6);
	memcpy(h.dst_lavc, a.dst_logical, 6);
	memcpy(h.src_lavc, a.src_logical, 6);
	h.sca_len_field = (uint16_t)(SIM_MSG_SCA - 2u);
	h.connect_flag = 0x0001u;
	h.word30 = (uint16_t)(VMS_SCS_MT_MSG |
			      ((uint16_t)VMS_SCS_FORMAT_V13 << 8));
	if (vms_sca_hdr_build(&h, out, cap, &written) != VMS_CODEC_OK)
		return 0u;
	if (vms_frame_classify(out, SIM_MSG_LEN, &fi) != VMS_CODEC_OK)
		return 0u;

	/* The Con.ID pair through the codec's OWN named offsets and its own
	 * bounds-checked primitive -- never a literal offset (design §3.9
	 * rule 2). The transport sequence fields are deliberately left alone:
	 * pe_vc_send_frame stamps them from real circuit state. */
	vms_wire_buf_init(&w, out, SIM_MSG_LEN);
	vms_wire_put_le32(&w, VMS_OFF_SCS_CONID_REMOTE, dst_conid);
	vms_wire_put_le32(&w, VMS_OFF_SCS_CONID_LOCAL, src_conid);
	if (!vms_wire_buf_ok(&w))
		return 0u;
	return SIM_MSG_LEN;
}

uint32_t sim_send_msgs(struct sim *s, struct sim_node *from,
		       struct sim_node *to, uint32_t count)
{
	uint8_t frame[SIM_MSG_LEN];
	uint32_t i, accepted = 0u;

	(void)s;
	if (from == NULL || to == NULL || from == to)
		return 0u;

	for (i = 0; i < count; i++) {
		uint32_t len = sim_build_msg(from, to->cfg.sysid,
					     sim_conid(to), sim_conid(from),
					     frame, sizeof(frame));
		int rc;

		from->msgs_offered++;
		if (len == 0u) {
			from->msgs_refused++;
			from->last_send_status = PE_VC_SEND_NOCIRCUIT;
			continue;
		}
		rc = pe_vc_send_frame(&from->fsm,
				      (vms_scs_sysid_t)to->cfg.sysid, frame,
				      len);
		from->last_send_status = rc;
		if (rc == PE_VC_SEND_OK) {
			from->msgs_accepted++;
			accepted++;
		} else {
			from->msgs_refused++;
		}
	}
	return accepted;
}
