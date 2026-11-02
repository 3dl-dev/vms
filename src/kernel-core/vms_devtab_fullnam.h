/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_devtab_fullnam.h - a device's FULL name, the one $GETDVI DVI$_FULLDEVNAM,
 * $ALLOC's physical name and SHOW PROCESS's "Devices allocated:" print
 * (rd vms-1f7, the vms-dv1 remainder). Header-only and dependency-free, so the
 * SAME composition the executive runs in devinfo_fill() is unit-tested on the
 * host (tests/libvms/test_devtab_fullnam.c) without a kernel.
 *
 * THE RULE, as measured on OpenVMS VAX V7.3 (tests/lab/captures/
 * fulldevnam-standalone-20261101/, docs/oracle/vax73-device-full-name.md):
 *
 *   1. A disk on a system with a non-zero ALLOCLASS is named by its allocation
 *      class:                         $<alloclass>$<devnam>     _$2$DUA0:
 *   2. Otherwise a unit whose driver set DEVCHAR2 bit DEV$M_NNM (512, "prefix
 *      the device name with the node name") is named by the node:
 *                                     <SCSNODE>$<devnam>        _VAX1$OPA0:
 *   3. Every other unit is its physical name alone:              _NLA0:
 *
 * The node prefix does NOT depend on cluster membership: the capture was taken
 * on a node booted with VAXCLUSTER 0 (F$GETSYI("CLUSTER_MEMBER") FALSE) and
 * still reads _VAX1$OPA0:, the same name the clustered VAX1 prints. What
 * decides it is the unit's own DEV$M_NNM -- set on the terminals, mailboxes,
 * disks, tapes and the printer; clear on NLA0:, the LAN controllers, the NET
 * and BG pseudo-devices (the R2| table in the capture).
 *
 * A name that already carries a '$' (an MSCP-served unit the class driver
 * entered as "$2$DUA2:" or "VAX1$DUA2:") is already full and is returned as is.
 * With no node name or allocation class loaded (no SYSGEN_LOAD yet), nothing
 * is invented: the full name is the physical name.
 *
 * No leading underscore: callers that print one (DVI$_FULLDEVNAM, $ALLOC)
 * add it, the way SHOW PROCESS prints the name without it.
 */
#ifndef _VMS_DEVTAB_FULLNAM_H
#define _VMS_DEVTAB_FULLNAM_H

#define VMS_DEVTAB_DEV_M_NNM   0x00000200u   /* DEV$M_NNM, DEVCHAR2 bit 9 */
#define VMS_DEVTAB_DC_DISK     1u            /* DC$_DISK */

/*
 * Write the full name into out[outsz]. Returns the length written, or -1 (out
 * left as the empty string) when the full name does not fit -- a truncated
 * device name would be a different device, so none is produced.
 */
static inline int vms_devtab_full_name(const char *devnam,
				       unsigned int devclass,
				       unsigned int devchar2,
				       const char *scsnode,
				       unsigned int scsnode_len,
				       unsigned int alloclass,
				       char *out, unsigned int outsz)
{
	char pre[16];
	unsigned int np = 0, i, n = 0;
	const char *p;

	if (!out || outsz == 0u)
		return -1;
	out[0] = '\0';
	if (!devnam)
		return -1;

	for (p = devnam; *p; p++)
		if (*p == '$')
			break;
	if (*p != '$') {
		if (devclass == VMS_DEVTAB_DC_DISK && alloclass != 0u &&
		    alloclass < 1000u) {
			unsigned int a = alloclass, d = 1;
			pre[np++] = '$';
			while (d * 10u <= a)
				d *= 10u;
			for (; d > 0u; d /= 10u)
				pre[np++] = (char)('0' + (a / d) % 10u);
			pre[np++] = '$';
		} else if ((devchar2 & VMS_DEVTAB_DEV_M_NNM) && scsnode &&
			   scsnode_len != 0u && scsnode_len <= 8u) {
			/* upper case, as $GETSYI SYI$_SCSNODE reads it back */
			for (i = 0; i < scsnode_len && scsnode[i] != '\0' &&
				    scsnode[i] != ' '; i++)
				pre[np++] = (scsnode[i] >= 'a' && scsnode[i] <= 'z') ?
					    (char)(scsnode[i] - 'a' + 'A') : scsnode[i];
			if (np != 0u)
				pre[np++] = '$';
		}
	}

	for (i = 0; i < np; i++) {
		if (n + 1u >= outsz)
			goto toolong;
		out[n++] = pre[i];
	}
	for (p = devnam; *p; p++) {
		if (n + 1u >= outsz)
			goto toolong;
		out[n++] = *p;
	}
	out[n] = '\0';
	return (int)n;

toolong:
	out[0] = '\0';
	return -1;
}

#endif /* _VMS_DEVTAB_FULLNAM_H */
