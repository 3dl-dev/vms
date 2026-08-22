/*
 * vmsdevalloc.c - OVMX/NetBSD-vax device-allocation ($ALLOC / $DALLOC)
 * cross-process test program (rd vms-618, epic vms-8e8).
 *
 * The tool the harness runs as SEPARATE PROCESSES to prove the INV-6-decisive
 * property of the device-allocation facility: a device one process $ALLOCates
 * is REFUSED to a DIFFERENT process with SS$_DEVALLOC, because the device row,
 * its owner and its allocation live in the EXECUTIVE's kernel memory (the
 * shared facility src/kernel-core/vms_devtab.c, now compiled into the NetBSD
 * `vms' pseudo-device) and not in either process (CLAUDE.md Rule 9 / Rule 11).
 *
 * WHY THAT IS THE DECISIVE CHECK, and not "ALLOCATE returned success". A
 * substrate-local $ALLOC that just answered SS$_NORMAL would pass every
 * single-process assertion and still be a facade -- it is exactly the
 * fabrication class INV-6 exists to kill. Only a SECOND process being told
 * SS$_DEVALLOC about a device the FIRST one holds can distinguish a real
 * executive-resident table from a per-process one. The same reasoning drives
 * the sibling mailbox proof (vmsmbx.c) and the Linux twin
 * tests/qemu/test_kmod_devtab.c.
 *
 * It reaches /dev/vms THROUGH the NetBSD transport seam
 * (src/libvmssys/kif_transport_netbsd.c), issuing the SAME ioctls the shared
 * facility implements (vms_devtab_nb.h -- byte-identical to the Linux
 * src/kernel/vms_ioctl.h contract). One operation per invocation, so every
 * operation is genuinely its own OS process.
 *
 * USAGE:
 *   vmsdevalloc alloc <devnam>
 *       $ALLOC(devnam). Prints the executive's status and exits at once.
 *   vmsdevalloc alloc_hold <devnam> <holdsecs>
 *       $ALLOC(devnam), stay alive <holdsecs> (so a DIFFERENT process can be
 *       refused while this one holds it), then $DALLOC and exit. The explicit
 *       $DALLOC is deliberate: it makes the release observable at a known
 *       instant instead of depending on when the executive reaps a dead PCB.
 *   vmsdevalloc dalloc <devnam>
 *       $DALLOC(devnam). Prints the executive's status.
 *   vmsdevalloc alloc_dalloc <devnam>
 *       $ALLOC then $DALLOC in the one process.
 *
 * STDOUT is stable, greppable tokens the driver keys on:
 *   "DEVALLOC ALLOC devnam=<name> status=<n>"
 *   "DEVALLOC DALLOC devnam=<name> status=<n>"
 * Status is the raw VMS condition value the executive returned (odd = success),
 * printed verbatim -- this program NEVER interprets a failure into a success.
 *
 * EXIT CODES: 0 = the ioctl(s) were delivered and the executive answered (the
 * ANSWER itself is on stdout for the driver to judge -- an expected refusal is
 * not this program's failure); 2 = usage error; 3 = /dev/vms unreachable (the
 * honest SS$_NOSUCHDEV, which is the module-absent negative control);
 * 4 = ioctl delivery failed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kif_transport.h"
#include "vms_devtab_nb.h"

/* Honest device-unreachable verdict, mirroring vmsprobe.c. */
#define OVMX_SS_NOSUCHDEV 2680u

static int
alloc_op(int fd, unsigned long req, const char *devnam, const char *what)
{
	struct vms_alloc_args aa;
	int rc;

	memset(&aa, 0, sizeof(aa));
	strncpy(aa.devnam, devnam, sizeof(aa.devnam) - 1);

	rc = kif_xport_ioctl(fd, req, &aa);
	if (rc < 0) {
		printf("DEVALLOC %s devnam=%s IOCTL_FAILED rc=%d\n",
		    what, devnam, rc);
		return 4;
	}
	printf("DEVALLOC %s devnam=%s status=%u\n", what, devnam,
	    (unsigned)aa.status);
	return 0;
}

int
main(int argc, char **argv)
{
	const char *op, *devnam;
	int fd, r;

	if (argc < 3) {
		fprintf(stderr, "usage: vmsdevalloc "
		    "alloc|dalloc|alloc_dalloc <devnam> | "
		    "alloc_hold <devnam> <holdsecs>\n");
		return 2;
	}
	op = argv[1];
	devnam = argv[2];

	fd = kif_xport_dev_open();
	if (fd < 0) {
		/* Honest failure -- never a fabricated per-process success. */
		printf("DEVALLOC: /dev/vms unreachable (open rc=%d) -> honest "
		    "SS$_NOSUCHDEV (%u); NOT faking success\n", fd,
		    OVMX_SS_NOSUCHDEV);
		return 3;
	}

	if (strcmp(op, "alloc") == 0) {
		r = alloc_op(fd, VMS_IOCTL_ALLOC, devnam, "ALLOC");
	} else if (strcmp(op, "dalloc") == 0) {
		r = alloc_op(fd, VMS_IOCTL_DALLOC, devnam, "DALLOC");
	} else if (strcmp(op, "alloc_dalloc") == 0) {
		r = alloc_op(fd, VMS_IOCTL_ALLOC, devnam, "ALLOC");
		if (r == 0)
			r = alloc_op(fd, VMS_IOCTL_DALLOC, devnam, "DALLOC");
	} else if (strcmp(op, "alloc_hold") == 0) {
		int secs = (argc > 3) ? atoi(argv[3]) : 10;

		r = alloc_op(fd, VMS_IOCTL_ALLOC, devnam, "ALLOC");
		if (r == 0) {
			fflush(stdout);
			if (secs > 0)
				(void)sleep((unsigned)secs);
			r = alloc_op(fd, VMS_IOCTL_DALLOC, devnam, "DALLOC");
		}
	} else {
		fprintf(stderr, "vmsdevalloc: unknown op '%s'\n", op);
		kif_xport_dev_close(fd);
		return 2;
	}

	kif_xport_dev_close(fd);
	fflush(stdout);
	return r;
}
