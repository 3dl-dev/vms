/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_acp_serve.h - the ODS-2 ACP's SERVED-VOLUME view (plan item FC-P6.3).
 *
 * WHAT THIS IS AND WHY IT IS ITS OWN HEADER. The MSCP disk server
 * (src/kernel-core/vms_mscp_srv.c) has to answer a remote class driver's
 * GET UNIT STATUS / ONLINE / READ / WRITE about volumes THIS executive really
 * holds -- design docs/design-faithful-cluster-executive.md's own decision row
 * 8: "the served unit must be the same volume the ACP mounts, or a VAX MOUNT
 * and OVMX see different data". The volumes live in vmsfs_acp.c's
 * executive-global mounted-volume table, whose `struct vms_acp_volume` is
 * private to that file; this header is the ONE read-only projection of it,
 * defined apart from both sides so neither has to include the other:
 *
 *   - vmsfs_acp.c fills it (it owns the table and its lock);
 *   - vms_mscp_srv.c reads it, and it is a CLUSTER core TU, which the CI gate
 *     tools/ci/cluster_core_includes_gate.sh restricts to exec_kbackend.h,
 *     vms_internal.h and src/kernel-core headers -- so the projection has to be
 *     a kernel-core header, which is exactly what this is.
 *
 * INV-6: EVERY FIELD IS A VALUE $MOUNT VALIDATED. `volsize` came off the SCB,
 * `backing_*` from the executive's own block-device resolution, `volname` off
 * the home block. A unit that is not a mounted volume is NOT reported at all --
 * there is no "probably serveable" row, and the enumeration simply ends.
 */
#ifndef OVMX_VMS_ACP_SERVE_H
#define OVMX_VMS_ACP_SERVE_H

#if defined(OVMX_CLUSTER_HOST)
#  include <stdint.h>
#  include <stddef.h>
#else
#  include "vms_internal.h"
#endif

/*
 * The canonical VMS unit-name width. It is the SAME 16 both substrates'
 * VMS_DEVNAM_SIZE is (src/kernel/vms_ioctl.h, src/kernel-netbsd/vms_acp_nb.h),
 * restated here rather than imported because this header must also compile in
 * the host rung, where neither of those is on the include path. A
 * _Static_assert in vmsfs_acp.c -- which sees both -- holds the two together,
 * so a future widening cannot silently truncate a unit name here.
 */
#define VMS_ACP_SERVE_DEVNAM_MAX 16u

/* The ODS-2 label field width, NUL-terminated (12 significant + terminator). */
#define VMS_ACP_SERVE_VOLNAME_MAX 13u

/*
 * One mounted ODS-2 volume, as a server sees it.
 */
struct vms_acp_volume_info {
	char     devnam[VMS_ACP_SERVE_DEVNAM_MAX];   /* "DKA0:" -- canonical  */
	char     volname[VMS_ACP_SERVE_VOLNAME_MAX]; /* ODS-2 label, NUL-term */
	uint8_t  pad0[3];
	uint32_t backing_major;   /* the REAL block device $MOUNT bound       */
	uint32_t backing_minor;
	uint32_t volsize;         /* SCB volume size, 512-byte blocks         */
	uint32_t cluster;         /* SCB storage-bitmap cluster factor        */
	uint16_t struclev;        /* home/SCB structure level (0x0201)        */
	uint8_t  read_only;       /* the volume's own write-protection state  */
	uint8_t  pad1;
};

/*
 * vms_acp_volume_at - project the `index`-th MOUNTED volume.
 *
 * Returns SS$_NORMAL and fills *out for a live row; SS$_NOSUCHDEV once the
 * index is past the end (the ordinary end of a sweep, not an error);
 * SS$_BADPARAM for a NULL argument. Takes the mounted-volume table's own lock,
 * so it MUST NOT be called with that lock already held.
 *
 * WHY AN INDEX AND NOT A LIST. The caller (the MSCP server's unit refresh)
 * wants a stable, allocation-free sweep it can run on its own beat; an index
 * walk gives it that without exporting the table's list head or its lock.
 * A volume dismounted between two calls simply is not reported -- which is the
 * truth at the moment of the call.
 *
 * ON `read_only`. This is the volume's OWN write-protection state as the
 * executive holds it. OVMX's $MOUNT has no /NOWRITE operator surface yet, so
 * today it is genuinely 0 for every mounted volume, and the MSCP server says so
 * on the wire by NOT asserting UF.WPH -- an honest absence, never a guess in
 * either direction. The field exists here (rather than being invented at the
 * server) so that when MOUNT/NOWRITE lands there is exactly ONE place the fact
 * comes from.
 */
uint32_t vms_acp_volume_at(uint32_t index, struct vms_acp_volume_info *out);

#endif /* OVMX_VMS_ACP_SERVE_H */
