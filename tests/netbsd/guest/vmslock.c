/*
 * vmslock.c - OVMX/NetBSD P4-A lock-manager (DLM) cross-process test program
 * (rd vms-f8a, epic vms-8e8). The tool the harness runs as SEPARATE
 * PROCESSES to prove the INV-6-decisive property for the lock manager: a
 * lock one process $ENQs on a named resource genuinely BLOCKS a DIFFERENT
 * process's incompatible $ENQW on the SAME resource until the first process
 * releases it, because the resource database lives in the executive's
 * KERNEL memory (the shared facility src/kernel-core/vms_lock.c, compiled
 * into the NetBSD `vms' pseudo-device), not in either process (CLAUDE.md
 * Rule 9). A per-process userspace fake could report a grant while sharing
 * nothing; a real lock manager cannot, because there is exactly one resource
 * block and it lives in the kernel.
 *
 * It reaches the in-kernel /dev/vms `vms' pseudo-device THROUGH the NetBSD
 * transport seam (src/libvmssys/kif_transport_netbsd.c), issuing the SAME
 * lock ioctls the shared facility implements.
 *
 * USAGE (one operation per invocation, so each is its own OS process):
 *   vmslock hold_release <resnam> <mode> <holdsecs>
 *       $ENQ(resnam, mode, flags=0) -- grants at once since the resource is
 *       free. Holds it for <holdsecs> (this process stays alive so the lock
 *       is not automatically $DEQ'd by process exit), then $DEQs it.
 *   vmslock enqw <resnam> <mode>
 *       $ENQ(resnam, mode, flags=LCK_M_SYNC) -- if incompatible with a
 *       currently granted lock on the resource, this BLOCKS IN THE KERNEL
 *       (exec_cv_wait in the shared facility) until granted. Prints the
 *       granted lock ID once it returns.
 *
 * EXIT CODES:
 *   hold_release: 0 = ENQ granted, hold completed, DEQ succeeded.
 *   enqw:         0 = the (possibly-blocking) ENQW eventually granted.
 *   any op:       2 = usage error; 3 = /dev/vms unreachable (honest
 *                 SS$_NOSUCHDEV, the module-absent negative control);
 *                 4 = ioctl delivery failed; 6 = executive returned a
 *                 failure status.
 *
 * The stdout lines are stable, greppable tokens the driver
 * (drive_netbsd_p4a.py) keys on: "LOCK ENQ GRANTED resnam=.. mode=.. lkid=..",
 * "LOCK DEQ RELEASED lkid=..", "LOCK ENQW GRANTED resnam=.. mode=.. lkid=..".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kif_transport.h"
#include "vms_lock_nb.h"

#define VMS_SS_NOSUCHDEV 2680u

enum { OP_HOLD_RELEASE, OP_ENQW };

static int
open_or_honest_fail(int *fd_out)
{
	int fd = kif_xport_dev_open();
	if (fd < 0) {
		printf("LOCK UNREACHABLE /dev/vms open rc=%d -> honest failure, "
		    "SS$_NOSUCHDEV (%u); NOT faking success\n", fd, VMS_SS_NOSUCHDEV);
		return -1;
	}
	*fd_out = fd;
	return 0;
}

int
main(int argc, char **argv)
{
	int op, fd, rc;

	if (argc < 3) {
		fprintf(stderr,
		    "usage: %s hold_release <resnam> <mode> <holdsecs> | "
		    "enqw <resnam> <mode>\n", argv[0]);
		return 2;
	}

	if (strcmp(argv[1], "hold_release") == 0) op = OP_HOLD_RELEASE;
	else if (strcmp(argv[1], "enqw") == 0)    op = OP_ENQW;
	else {
		fprintf(stderr, "%s: unknown op '%s'\n", argv[0], argv[1]);
		return 2;
	}

	if (open_or_honest_fail(&fd) < 0)
		return 3;

	if (op == OP_HOLD_RELEASE) {
		struct vms_enq_args ea;
		struct vms_deq_args da;
		unsigned int mode, holdsecs;

		if (argc != 5) {
			fprintf(stderr, "usage: %s hold_release <resnam> <mode> <holdsecs>\n",
			    argv[0]);
			kif_xport_dev_close(fd);
			return 2;
		}
		mode = (unsigned int)strtoul(argv[3], NULL, 0);
		holdsecs = (unsigned int)strtoul(argv[4], NULL, 0);

		memset(&ea, 0, sizeof(ea));
		ea.lkmode = mode;
		ea.flags = 0;   /* not sync: the resource is free, grants at once */
		strncpy(ea.resnam, argv[2], sizeof(ea.resnam) - 1);

		rc = kif_xport_ioctl(fd, VMS_IOCTL_ENQ, &ea);
		if (rc < 0) {
			printf("LOCK IOCTLFAIL ENQ resnam=%s rc=%d (negated errno)\n",
			    argv[2], rc);
			kif_xport_dev_close(fd);
			return 4;
		}
		if ((ea.status & 1u) == 0u) {
			printf("LOCK ENQ FAILED resnam=%s mode=%u status=%u\n",
			    argv[2], mode, ea.status);
			kif_xport_dev_close(fd);
			return 6;
		}
		printf("LOCK ENQ GRANTED resnam=%s mode=%u lkid=%u\n",
		    argv[2], mode, ea.lkid);
		fflush(stdout);

		/* Hold the resource: while this process is alive and has not $DEQ'd,
		 * a conflicting request from a DIFFERENT process must block/queue. */
		sleep(holdsecs);

		memset(&da, 0, sizeof(da));
		da.lkid = ea.lkid;
		rc = kif_xport_ioctl(fd, VMS_IOCTL_DEQ, &da);
		kif_xport_dev_close(fd);
		if (rc < 0) {
			printf("LOCK IOCTLFAIL DEQ lkid=%u rc=%d (negated errno)\n",
			    ea.lkid, rc);
			return 4;
		}
		if ((da.status & 1u) == 0u) {
			printf("LOCK DEQ FAILED lkid=%u status=%u\n", ea.lkid, da.status);
			return 6;
		}
		printf("LOCK DEQ RELEASED lkid=%u\n", ea.lkid);
		return 0;
	}

	/* enqw: synchronous, may BLOCK IN THE KERNEL until a conflicting holder
	 * releases -- that in-kernel block/wake, across the process boundary, is
	 * the strongest form of the shared-state proof (mirrors the event-flag
	 * facility's cross-process CV-wait proof, P2c). */
	{
		struct vms_enq_args ea;
		unsigned int mode;

		if (argc != 4) {
			fprintf(stderr, "usage: %s enqw <resnam> <mode>\n", argv[0]);
			kif_xport_dev_close(fd);
			return 2;
		}
		mode = (unsigned int)strtoul(argv[3], NULL, 0);

		memset(&ea, 0, sizeof(ea));
		ea.lkmode = mode;
		ea.flags = LCK_M_SYNC;   /* block in-kernel until granted or deadlocked */
		strncpy(ea.resnam, argv[2], sizeof(ea.resnam) - 1);

		rc = kif_xport_ioctl(fd, VMS_IOCTL_ENQ, &ea);
		kif_xport_dev_close(fd);
		if (rc < 0) {
			printf("LOCK IOCTLFAIL ENQW resnam=%s rc=%d (negated errno)\n",
			    argv[2], rc);
			return 4;
		}
		if ((ea.status & 1u) == 0u) {
			printf("LOCK ENQW FAILED resnam=%s mode=%u status=%u\n",
			    argv[2], mode, ea.status);
			return 6;
		}
		printf("LOCK ENQW GRANTED resnam=%s mode=%u lkid=%u\n",
		    argv[2], mode, ea.lkid);
		return 0;
	}
}
