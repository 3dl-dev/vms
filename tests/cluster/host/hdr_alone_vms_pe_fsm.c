/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_pe_fsm.c - vms_pe_fsm.h, alone, from a blank slate (FC-P0.8).
 *
 * Same discipline as hdr_alone_vms_cluster.c: the ONLY project #include is the
 * header under test, so this proves it drags in nothing but the other pure
 * kernel-core headers and the codec. A compile-only object; nothing here runs.
 */
#include <stdint.h>
#include <stddef.h>

#include "vms_pe_fsm.h"

int ovmx_hdr_alone_vms_pe_fsm(void);
int ovmx_hdr_alone_vms_pe_fsm(void)
{
	return (int)sizeof(struct pe_fsm) +
	       (int)sizeof(struct pe_channel) +
	       (int)sizeof(struct pe_identity) +
	       (int)sizeof(struct pe_channel_rec);
}
