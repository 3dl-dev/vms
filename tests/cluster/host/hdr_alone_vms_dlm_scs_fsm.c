/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_dlm_scs_fsm.c - vms_dlm_scs_fsm.h, alone, from a blank slate
 * (FC-P4.6, on FC-P0.1's pattern).
 *
 * This translation unit's ONLY project #include is vms_dlm_scs_fsm.h. If that
 * header silently relies on something else having been included first -- the
 * FC-P4.5 cat-0x02 codec, the FC-P4.4 proxy seam, vms_cluster.h -- this file
 * fails to compile even though the same header works fine inside
 * vms_dlm_scs_fsm.c, which pulls its dependencies in itself.
 *
 * It also proves the thing the FC-P4.6 groundwork commit was FOR: this header
 * pulls in BOTH vms_dlm_proxy.h (the engine seam) and vms_cluster_codec_dlm.h
 * (the wire opcodes) in one TU, which was impossible while the two families
 * shared short macro names.
 */
#include <stdint.h>
#include <stddef.h>

#include "vms_dlm_scs_fsm.h"

int ovmx_hdr_alone_vms_dlm_scs_fsm(void);
int ovmx_hdr_alone_vms_dlm_scs_fsm(void)
{
	return (int)sizeof(struct dlm_req_fsm) + (int)DLM_REQ_EV__COUNT +
	       (int)DLM_REQ_ST__COUNT + (int)sizeof(struct dlm_req);
}
