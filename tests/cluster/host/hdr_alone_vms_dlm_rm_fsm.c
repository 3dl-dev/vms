/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_dlm_rm_fsm.c - vms_dlm_rm_fsm.h, alone, from a blank slate
 * (rd vms-ed5, on FC-P0.1's pattern).
 *
 * This translation unit's ONLY project #include is vms_dlm_rm_fsm.h. If that
 * header silently relies on something else having been included first -- the
 * cat-0x02 codec, the connection manager's body length, or the adoption seam --
 * this file fails to compile even though the same header works fine inside
 * vms_dlm_rm_fsm.c, which pulls its dependencies in itself.
 */
#include <stdint.h>
#include <stddef.h>

#include "vms_dlm_rm_fsm.h"

int ovmx_hdr_alone_vms_dlm_rm_fsm(void);
int ovmx_hdr_alone_vms_dlm_rm_fsm(void)
{
	return (int)sizeof(struct vms_dlm_rm) + (int)VMS_DLM_RM_EV__COUNT +
	       (int)VMS_DLM_RM_ST__COUNT + (int)sizeof(struct vms_dlm_rm_tree) +
	       (int)sizeof(struct vms_dlm_rm_ops);
}
