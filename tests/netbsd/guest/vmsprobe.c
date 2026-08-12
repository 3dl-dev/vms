/*
 * vmsprobe.c - OVMX/NetBSD P2b userspace probe (rd vms-bfe, epic vms-8e8).
 *
 * Reaches the in-kernel /dev/vms `vms' pseudo-device THROUGH the NetBSD
 * transport seam (src/libvmssys/kif_transport_netbsd.c, the NetBSD leaf of the
 * P1 kif_transport.h contract), issues the version/ping ioctl defined by the
 * shared /dev/vms contract (vms_ping.h), and asserts the executive's answer.
 *
 * It applies the SAME honest-failure policy the vms_kif policy layer applies:
 * if the transport cannot open the device (kif_xport_dev_open() < 0), that is an
 * honest SS$_NOSUCHDEV -- the probe reports it and exits nonzero. It NEVER
 * fabricates a per-process success (INV-6, CLAUDE.md Rule 9). That is the whole
 * point of the negative control: with the module not loaded, /dev/vms does not
 * exist, and this path must fire.
 *
 * P2b is a minimal vertical slice, so the probe drives the transport seam
 * directly and inlines that one policy decision rather than compiling the full
 * Linux-freestanding vms_kif.c on NetBSD (a policy-layer port is P2c).
 *
 * Exit codes: 0 = PING OK. Nonzero = a specific, honest failure (see below).
 */

#include <stdio.h>
#include <string.h>

#include "kif_transport.h"
#include "vms_ping.h"

int
main(void)
{
	struct vms_ping_args pa;
	int fd, rc;

	fd = kif_xport_dev_open();
	if (fd < 0) {
		/* Honest device-unreachable failure -- the SS$_NOSUCHDEV verdict,
		 * never a faked success (INV-6 / Rule 9). */
		printf("PROBE: /dev/vms unreachable (open rc=%d) -> honest failure, "
		    "SS$_NOSUCHDEV (%u); NOT faking success\n", fd, VMS_SS_NOSUCHDEV);
		return 3;
	}

	memset(&pa, 0, sizeof(pa));
	pa.magic = VMS_PING_REQ;

	rc = kif_xport_ioctl(fd, VMS_IOCTL_PING, &pa);
	kif_xport_dev_close(fd);

	if (rc < 0) {
		printf("PROBE: ping ioctl failed (rc=%d, negated errno)\n", rc);
		return 4;
	}
	if (pa.magic != VMS_PING_ACK) {
		printf("PROBE: bad ack cookie 0x%08x (want 0x%08x)\n",
		    pa.magic, VMS_PING_ACK);
		return 5;
	}
	if ((pa.status & 1u) == 0u) {
		printf("PROBE: executive returned failure status %u (even)\n",
		    pa.status);
		return 6;
	}
	if (pa.substrate != VMS_SUBSTRATE_NETBSD) {
		printf("PROBE: unexpected substrate %u (want NetBSD=%u)\n",
		    pa.substrate, VMS_SUBSTRATE_NETBSD);
		return 7;
	}
	if (pa.abi_version != VMS_PING_ABI_VERSION) {
		printf("PROBE: ABI mismatch %u (want %u)\n",
		    pa.abi_version, VMS_PING_ABI_VERSION);
		return 8;
	}

	printf("PROBE: PING OK -- ack=0x%08x abi=%u substrate=NetBSD status=%u\n",
	    pa.magic, pa.abi_version, pa.status);
	return 0;
}
