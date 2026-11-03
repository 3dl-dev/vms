/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_master_wire.h - ONE RECEIVED op-0x01/op-0x07, AS THE MASTER IS TOLD
 * OF IT (rd vms-1e58 C4).
 *
 * The translation from a PARSED cat-0x02 request (struct vms_dlm_enq_request,
 * the codec's reading of the frame) to the master-side door's request (struct
 * vms_dlm_master_request, vms_dlm_master.h). It is header-only so that the
 * shipping wire arm (vms_dlm_scs.c, not host-linkable) and the host harnesses
 * that serve a VAX's frame as the master call THE SAME CODE -- the harness used
 * to carry its own copy, and that copy took the request's flags as a PARAMETER
 * while the arm's never set them at all. The ci.6 lab fire found the gap the
 * copy hid: a real VAX's EX/NOQUEUE against an EX this node held was QUEUED,
 * and the VAX's $ENQW completed SS$_NORMAL while this node kept EX.
 *
 * The includer must already have the engine's LCK_M_ vocabulary in scope
 * (vms_internal.h on either substrate): `flags` here is the ENGINE's
 * numbering, which is not LCK$M_'s.
 */
#ifndef VMS_DLM_MASTER_WIRE_H
#define VMS_DLM_MASTER_WIRE_H

#include "vms_cluster_codec_dlm.h"
#include "vms_dlm_master.h"

#ifndef LCK_M_NOQUEUE
#error "vms_dlm_master_wire.h needs the engine's LCK_M_ flags (vms_internal.h)"
#endif

/*
 * The requester's options the master acts on, from the frame's body[28:30].
 *
 * NOQUEUE ONLY, AND ONLY ON A FIRST ENQ. It is the one grounded bit
 * (VMS_DLM_REQ_FLAG_NOQUEUE), and an op-0x01 deny is the one answer shape this
 * node can build from grounded cells (vms_dlm_enq_response_build_notqueued). A
 * CONVERT's deny has a shape of its own whose body[30] has no grounded
 * determinant yet, so a NOQUEUE convert keeps its old path until that answer
 * is grounded -- an honest gap, tracked, not a guessed frame sent to a peer.
 */
static inline uint32_t vms_dlm_master_req_flags(const struct vms_dlm_enq_request *e,
						uint8_t wireop)
{
	if (wireop == VMS_DLM_WIREOP_ENQ &&
	    (e->lck_flags & VMS_DLM_REQ_FLAG_NOQUEUE) != 0u)
		return (uint32_t)LCK_M_NOQUEUE;
	return 0u;
}

/*
 * Fill the master-side request from the PARSED frame and the connection
 * manager's own identification of the sender. `from_csid` is spec §4(a)'s
 * cluster-logical source address -- the one grounded answer to "who is
 * asking"; the DLM body carries no CSID field at all.
 */
static inline void vms_dlm_master_req_from_wire(const struct vms_dlm_enq_request *e,
						uint8_t wireop,
						uint32_t from_csid,
						struct vms_dlm_master_request *out)
{
	uint32_t n = e->name_len;
	uint32_t i;
	uint8_t *p = (uint8_t *)out;

	for (i = 0u; i < (uint32_t)sizeof(*out); i++)
		p[i] = 0u;
	out->op = (wireop == VMS_DLM_WIREOP_CONVERT) ? VMS_DLM_MREQ_CONVERT
						     : VMS_DLM_MREQ_ENQ;
	out->req_csid = from_csid;
	out->req_lkid = e->req_lkid;
	out->master_lkid = e->master_lkid;
	out->lkmode = e->mode;
	out->flags = vms_dlm_master_req_flags(e, wireop);
	/* COUNTED (rd vms-326): the frame states the length at body[47] and a
	 * VMS resource name may contain NULs, so the master is told how many
	 * bytes the name is rather than being left to find a terminator. The
	 * tail stays zero, so a diagnostic printf of an ASCII name is safe. */
	if (n > (uint32_t)sizeof(out->resnam))
		n = (uint32_t)sizeof(out->resnam);
	for (i = 0u; i < n; i++)
		out->resnam[i] = (char)e->name[i];
	out->resnam_len = (uint8_t)n;
	/*
	 * WHICH resource of that name (rd vms-b5b0): body[44:46] UIC group and
	 * body[46] access mode, as the codec read them. `res_ident_valid` is 0
	 * for an op-0x07 CONVERT, whose identity span is stale buffer on a real
	 * frame -- the engine then takes the identity off the LOCK the convert
	 * names, which is an executive read (vms_lock.c).
	 */
	out->res_group = e->res_group;
	out->res_mode = e->res_acmode;
	out->res_ident_valid = e->res_ident_valid;
}

#endif /* VMS_DLM_MASTER_WIRE_H */
