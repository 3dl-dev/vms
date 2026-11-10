/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_tt_oper.c - vms_tt_oper.h, alone, from a blank slate (rd
 * vms-896d). vms_tt.c includes it into both substrates' modules, so it must
 * drag in nothing and warn about nothing under -Wall -Wextra in a TU that uses
 * none of it. A compile-only object; nothing here runs.
 */
#include "vms_tt_oper.h"

int ovmx_hdr_alone_vms_tt_oper(void);
int ovmx_hdr_alone_vms_tt_oper(void)
{
	return (int)VMS_TT_OPER_HOLD;
}
