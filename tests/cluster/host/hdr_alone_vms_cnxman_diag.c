/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_cnxman_diag.c - vms_cnxman_diag.h, alone, from a blank slate
 * (E69).
 *
 * Same discipline as hdr_alone_vms_cluster.c: the ONLY project #include is the
 * header under test. A compile-only object; nothing here runs. It also touches
 * both _Static_assert'ed ABI structs, so a layout drift on either substrate is
 * a build failure here as well as in the ioctl header's own asserts.
 */
#include <stdint.h>
#include <stddef.h>

#include "vms_cnxman_diag.h"

int ovmx_hdr_alone_vms_cnxman_diag(void);
int ovmx_hdr_alone_vms_cnxman_diag(void)
{
	return (int)sizeof(struct cnxman_diag_rec) +
	       (int)sizeof(struct cnxman_diag_ring) +
	       (int)sizeof(struct cnxman_diag_view) +
	       (int)CNXMAN_DIAG_K__COUNT +
	       (int)CNXMAN_DIAG_R__COUNT +
	       (int)CNXMAN_DIAG_G__COUNT;
}
