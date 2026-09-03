// SPDX-License-Identifier: GPL-2.0
/*
 * vms_mscp_cl_fsm.c - MSCP disk-client DISCOVERY FSM (FC-P3.4).
 *
 * Read vms_mscp_cl_fsm.h first: the walk, the P.CRF composition and the
 * scope boundary (discovery only, no ONLINE/READ/WRITE) are documented
 * there. This file is the state ladder's behaviour: every frame this file
 * builds goes through vms_cluster_codec_mscp.h (FC-P6.2) -- no raw wire
 * offset here (design SS3.9 rule 2) -- and every field it fills in comes
 * from FSM state or a caller argument, never a template (INV-6).
 */

#include "vms_mscp_cl_fsm.h"

/* ==========================================================================
 * Small shared helpers
 * ========================================================================== */

/* This TU calls no library function (a pure TU builds on the host too,
 * where the substrate's own memset is not in scope -- the same discipline
 * vms_cnxman_barrier_fsm.c's barrier_bzero() already established). */
static void mscp_cl_bzero(void *p, uint32_t n)
{
	uint8_t *o = (uint8_t *)p;
	uint32_t i;

	for (i = 0; i < n; i++)
		o[i] = 0u;
}

/* Build the abs[0,72) link prefix for a 94-content command frame. Every
 * command this FSM sends (SCC or GUS) shares the same SCA-content length,
 * so this is the one link-prefix shape the whole walk ever needs. */
static vms_codec_status_t mscp_cl_build_link(const struct vms_mscp_link *link,
					     uint8_t *frame, uint32_t cap,
					     uint32_t *link_written)
{
	if (link == (const struct vms_mscp_link *)0)
		return VMS_CODEC_E_INVAL;
	return vms_mscp_link_build(link, VMS_MSCP_CMD_SCA_LEN, frame, cap,
				   link_written);
}

/* Classify a received frame and refuse anything that is not the expected
 * MSCP end class -- an unmatched class is not this walk's answer and must
 * not be applied to it. */
static vms_codec_status_t mscp_cl_classify_end(const uint8_t *frame,
					       uint32_t len,
					       enum vms_mscp_class want)
{
	struct vms_frame_info fi;
	enum vms_mscp_class cls;
	vms_codec_status_t st;

	if (frame == (const uint8_t *)0)
		return VMS_CODEC_E_INVAL;

	mscp_cl_bzero(&fi, (uint32_t)sizeof(fi));
	st = vms_frame_classify(frame, len, &fi);
	if (st != VMS_CODEC_OK)
		return VMS_CODEC_E_CLASS;

	st = vms_mscp_classify(frame, len, &fi, &cls);
	if (st != VMS_CODEC_OK)
		return st;
	if (cls != want)
		return VMS_CODEC_E_CLASS;
	return VMS_CODEC_OK;
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

void vms_mscp_cl_fsm_init(struct vms_mscp_cl_fsm *f)
{
	if (f == (struct vms_mscp_cl_fsm *)0)
		return;
	mscp_cl_bzero(f, (uint32_t)sizeof(*f));
	f->state = VMS_MSCP_CL_ST_INIT;
	f->scc_msgid = (uint16_t)VMS_MSCP_CL_SCC_MSGID0;
	f->gus_msgid = (uint16_t)VMS_MSCP_CL_GUS_MSGID0;
	f->next_unit = (uint16_t)VMS_MSCP_CL_GUS_SEED_UNIT;
}

int vms_mscp_cl_fsm_done(const struct vms_mscp_cl_fsm *f)
{
	return f != (const struct vms_mscp_cl_fsm *)0 &&
	       f->state == VMS_MSCP_CL_ST_DONE;
}

/* ==========================================================================
 * SET CONTROLLER CHARACTERISTICS, twice
 * ========================================================================== */

vms_codec_status_t vms_mscp_cl_fsm_build_scc(struct vms_mscp_cl_fsm *f,
					     const struct vms_mscp_link *link,
					     uint16_t ctlr_flags,
					     uint16_t host_timeout,
					     uint64_t time,
					     uint8_t *frame, uint32_t cap,
					     uint32_t *written)
{
	struct vms_mscp_scc_cmd c;
	uint32_t link_written = 0, body_written = 0;
	uint32_t cmd_ref;
	vms_codec_status_t st;
	enum vms_mscp_cl_state next_state;

	if (f == (struct vms_mscp_cl_fsm *)0 || frame == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;

	if (f->state == VMS_MSCP_CL_ST_INIT)
		next_state = VMS_MSCP_CL_ST_SCC1_SENT;
	else if (f->state == VMS_MSCP_CL_ST_SCC1_DONE)
		next_state = VMS_MSCP_CL_ST_SCC2_SENT;
	else
		return VMS_CODEC_E_CLASS;

	st = mscp_cl_build_link(link, frame, cap, &link_written);
	if (st != VMS_CODEC_OK)
		return st;

	cmd_ref = VMS_MSCP_CL_CMD_REF(VMS_MSCP_CL_SCC_CLASS, f->scc_msgid);

	mscp_cl_bzero(&c, (uint32_t)sizeof(c));
	c.hdr.cmd_ref = cmd_ref;
	c.hdr.unit = 0u; /* sec 6.16: SCC addresses the controller, not a unit */
	c.version = 0u;  /* sec 6.16: host MUST supply 0 (VMS_MSCP_SCC_VERSION_HOST) */
	c.ctlr_flags = ctlr_flags;
	c.host_timeout = host_timeout;
	c.time = time;

	st = vms_mscp_scc_cmd_build(&c, frame, cap, &body_written);
	if (st != VMS_CODEC_OK)
		return st;

	f->scc_msgid = (uint16_t)(f->scc_msgid + 1u);
	f->pending_cmd_ref = cmd_ref;
	f->state = next_state;

	if (written != (uint32_t *)0)
		*written = VMS_OFF_SYSAP_BODY + body_written;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_mscp_cl_fsm_on_scc_end(struct vms_mscp_cl_fsm *f,
					      const uint8_t *frame,
					      uint32_t len)
{
	struct vms_mscp_scc_end e;
	vms_codec_status_t st;
	enum vms_mscp_cl_state next_state;

	if (f == (struct vms_mscp_cl_fsm *)0)
		return VMS_CODEC_E_INVAL;

	if (f->state == VMS_MSCP_CL_ST_SCC1_SENT)
		next_state = VMS_MSCP_CL_ST_SCC1_DONE;
	else if (f->state == VMS_MSCP_CL_ST_SCC2_SENT)
		next_state = VMS_MSCP_CL_ST_GUS_READY;
	else
		return VMS_CODEC_E_CLASS;

	st = mscp_cl_classify_end(frame, len, VMS_MSCP_CLS_SCC_END);
	if (st != VMS_CODEC_OK)
		return st;

	st = vms_mscp_scc_end_parse(frame, len, &e);
	if (st != VMS_CODEC_OK)
		return st;

	/* sec 5.1: P.CRF is echoed verbatim. An answer to a DIFFERENT
	 * command is not this step's answer -- refuse rather than advance
	 * the walk on a stale or misrouted response. */
	if (e.eh.hdr.cmd_ref != f->pending_cmd_ref)
		return VMS_CODEC_E_CLASS;

	f->state = next_state;
	return VMS_CODEC_OK;
}

/* ==========================================================================
 * GET UNIT STATUS, the NEXT-UNIT walk
 * ========================================================================== */

vms_codec_status_t vms_mscp_cl_fsm_build_gus(struct vms_mscp_cl_fsm *f,
					     const struct vms_mscp_link *link,
					     uint8_t *frame, uint32_t cap,
					     uint32_t *written)
{
	struct vms_mscp_gus_cmd c;
	uint32_t link_written = 0, body_written = 0;
	uint32_t cmd_ref;
	vms_codec_status_t st;

	if (f == (struct vms_mscp_cl_fsm *)0 || frame == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;
	if (f->state != VMS_MSCP_CL_ST_GUS_READY)
		return VMS_CODEC_E_CLASS;

	st = mscp_cl_build_link(link, frame, cap, &link_written);
	if (st != VMS_CODEC_OK)
		return st;

	cmd_ref = VMS_MSCP_CL_CMD_REF(VMS_MSCP_CL_GUS_CLASS, f->gus_msgid);

	mscp_cl_bzero(&c, (uint32_t)sizeof(c));
	c.hdr.cmd_ref = cmd_ref;
	c.hdr.unit = f->next_unit; /* the walk cursor -- see file header */
	c.modifiers = VMS_MSCP_MOD_NEXT_UNIT;

	st = vms_mscp_gus_cmd_build(&c, frame, cap, &body_written);
	if (st != VMS_CODEC_OK)
		return st;

	f->gus_msgid = (uint16_t)(f->gus_msgid + 1u);
	f->pending_cmd_ref = cmd_ref;
	f->state = VMS_MSCP_CL_ST_GUS_SENT;

	if (written != (uint32_t *)0)
		*written = VMS_OFF_SYSAP_BODY + body_written;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_mscp_cl_fsm_on_gus_end(struct vms_mscp_cl_fsm *f,
					      const uint8_t *frame, uint32_t len,
					      struct vms_mscp_cl_unit *unit,
					      int *is_terminator)
{
	struct vms_mscp_gus_end e;
	vms_codec_status_t st;

	if (f == (struct vms_mscp_cl_fsm *)0 || is_terminator == (int *)0)
		return VMS_CODEC_E_INVAL;
	if (f->state != VMS_MSCP_CL_ST_GUS_SENT)
		return VMS_CODEC_E_CLASS;

	st = mscp_cl_classify_end(frame, len, VMS_MSCP_CLS_GUS_END);
	if (st != VMS_CODEC_OK)
		return st;

	st = vms_mscp_gus_end_parse(frame, len, &e);
	if (st != VMS_CODEC_OK)
		return st;

	if (e.eh.hdr.cmd_ref != f->pending_cmd_ref)
		return VMS_CODEC_E_CLASS;

	/* sec 4(n): "the walk ends when an END returns status OFFLINE, which
	 * is the end-of-list terminator, not an error." */
	if (e.eh.status_major == (unsigned)VMS_MSCP_ST_OFFLINE) {
		*is_terminator = 1;
		f->state = VMS_MSCP_CL_ST_DONE;
		return VMS_CODEC_OK;
	}

	*is_terminator = 0;
	if (unit != (struct vms_mscp_cl_unit *)0) {
		unit->unit = e.eh.hdr.unit;
		unit->unit_flags = e.unit_flags;
		unit->unit_id = e.unit_id;
		unit->media_id = e.media_id;
		unit->status_major = e.eh.status_major;
	}

	/* "each subsequent command uses the previous END's returned unit
	 * word + 1" -- read from the peer's own answer, never a local
	 * counter that ignores it. */
	f->next_unit = (uint16_t)(e.eh.hdr.unit + 1u);
	f->units_found++;
	f->state = VMS_MSCP_CL_ST_GUS_READY;
	return VMS_CODEC_OK;
}
