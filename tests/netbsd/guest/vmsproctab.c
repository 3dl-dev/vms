/*
 * vmsproctab.c - OVMX/NetBSD P4-A process-table cross-process test program
 * (rd vms-f8a, epic vms-8e8). The tool the harness runs as SEPARATE PROCESSES
 * to prove the INV-6-decisive property for the process-table facility: a
 * process NAME one process records with $SETPRN is resolvable by a DIFFERENT
 * process's $GETJPI/$PROCESS_SCAN, because the row lives in the executive's
 * KERNEL memory (the shared facility src/kernel-core/vms_proctab.c, compiled
 * into the NetBSD `vms' pseudo-device), not in either process (CLAUDE.md
 * Rule 9).
 *
 * It reaches the in-kernel /dev/vms `vms' pseudo-device THROUGH the NetBSD
 * transport seam (src/libvmssys/kif_transport_netbsd.c), issuing the SAME
 * process-table ioctls the shared facility implements.
 *
 * USAGE (one operation per invocation, so each is its own OS process):
 *   vmsproctab bg <name> <secs>
 *       $SETPRN(name), then sleep(secs) so this process's PCB stays present
 *       in the shared table (still alive, so vms_proc_reap_dead() cannot
 *       reap it) long enough for a DIFFERENT process to look it up.
 *   vmsproctab getjpi_name <name>
 *       $GETJPI selecting VMS_JPI_SEL_PRCNAM=<name>. Prints the resolved
 *       row's pid/uic if found.
 *   vmsproctab procscan_find <name> <maxrows>
 *       Walks $PROCESS_SCAN (index 0..maxrows-1) looking for a row whose
 *       prcnam matches <name> -- the "enumerate and see it" proof.
 *
 * EXIT CODES:
 *   bg:            0 = $SETPRN succeeded and the sleep completed.
 *   getjpi_name:   0 = found a row whose prcnam matches <name>; 1 = not
 *                  found / status failure.
 *   procscan_find: 0 = a scanned row matched <name>; 1 = exhausted the scan
 *                  (or hit maxrows) without a match.
 *   any op:        2 = usage error; 3 = /dev/vms unreachable (honest
 *                  SS$_NOSUCHDEV, the module-absent negative control);
 *                  4 = ioctl delivery failed.
 *
 * The stdout lines are stable, greppable tokens the driver
 * (drive_netbsd_p4a.py) keys on: "PROCTAB SETPRN name=.. status=..",
 * "PROCTAB GETJPI_FOUND name=.. pid=.. uic=0x..",
 * "PROCTAB GETJPI_NOTFOUND name=..",
 * "PROCTAB PROCSCAN_FOUND idx=.. name=.. pid=..".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kif_transport.h"
#include "vms_proctab_nb.h"

enum { OP_BG, OP_GETJPI_NAME, OP_PROCSCAN_FIND };

/* SS$_NOSUCHDEV, matching vms_eflag_nb.h / vms_internal.h's SS__NOSUCHDEV
 * (2680); named locally since this header set does not carry it. */
#define VMS_SS_NOSUCHDEV 2680u

static int
open_or_honest_fail(int *fd_out)
{
	int fd = kif_xport_dev_open();
	if (fd < 0) {
		printf("PROCTAB UNREACHABLE /dev/vms open rc=%d -> honest failure, "
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
		    "usage: %s bg <name> <secs> | getjpi_name <name> | "
		    "procscan_find <name> <maxrows>\n", argv[0]);
		return 2;
	}

	if (strcmp(argv[1], "bg") == 0)             op = OP_BG;
	else if (strcmp(argv[1], "getjpi_name") == 0)   op = OP_GETJPI_NAME;
	else if (strcmp(argv[1], "procscan_find") == 0) op = OP_PROCSCAN_FIND;
	else {
		fprintf(stderr, "%s: unknown op '%s'\n", argv[0], argv[1]);
		return 2;
	}

	if (open_or_honest_fail(&fd) < 0)
		return 3;

	if (op == OP_BG) {
		struct vms_setprn_args sa;
		unsigned int secs;

		if (argc != 4) {
			fprintf(stderr, "usage: %s bg <name> <secs>\n", argv[0]);
			return 2;
		}
		secs = (unsigned int)strtoul(argv[3], NULL, 0);

		memset(&sa, 0, sizeof(sa));
		strncpy(sa.prcnam, argv[2], sizeof(sa.prcnam) - 1);
		rc = kif_xport_ioctl(fd, VMS_IOCTL_SETPRN, &sa);
		if (rc < 0) {
			printf("PROCTAB IOCTLFAIL SETPRN rc=%d (negated errno)\n", rc);
			kif_xport_dev_close(fd);
			return 4;
		}
		printf("PROCTAB SETPRN name=%s status=%u\n", argv[2], sa.status);
		if ((sa.status & 1u) == 0u) {
			kif_xport_dev_close(fd);
			return 6;
		}
		/* Keep this process (and its PCB) alive so a different process's
		 * lookup below can find it -- the cross-process rendezvous window. */
		fflush(stdout);
		sleep(secs);
		kif_xport_dev_close(fd);
		return 0;
	}

	if (op == OP_GETJPI_NAME) {
		struct vms_getjpi_args ga;

		if (argc != 3) {
			fprintf(stderr, "usage: %s getjpi_name <name>\n", argv[0]);
			return 2;
		}
		memset(&ga, 0, sizeof(ga));
		ga.select = VMS_JPI_SEL_PRCNAM;
		strncpy(ga.sel_prcnam, argv[2], sizeof(ga.sel_prcnam) - 1);

		rc = kif_xport_ioctl(fd, VMS_IOCTL_GETJPI, &ga);
		kif_xport_dev_close(fd);
		if (rc < 0) {
			printf("PROCTAB IOCTLFAIL GETJPI rc=%d (negated errno)\n", rc);
			return 4;
		}
		if ((ga.status & 1u) == 0u ||
		    strncmp(ga.info.prcnam, argv[2], sizeof(ga.info.prcnam)) != 0) {
			printf("PROCTAB GETJPI_NOTFOUND name=%s status=%u\n",
			    argv[2], ga.status);
			return 1;
		}
		printf("PROCTAB GETJPI_FOUND name=%s pid=%u uic=0x%08x\n",
		    ga.info.prcnam, ga.info.vms_pid, ga.info.uic);
		return 0;
	}

	/* procscan_find */
	{
		struct vms_procscan_args pa;
		unsigned int maxrows;
		unsigned int i;

		if (argc != 4) {
			fprintf(stderr, "usage: %s procscan_find <name> <maxrows>\n", argv[0]);
			return 2;
		}
		maxrows = (unsigned int)strtoul(argv[3], NULL, 0);

		memset(&pa, 0, sizeof(pa));
		pa.index = 0;
		for (i = 0; i < maxrows; i++) {
			rc = kif_xport_ioctl(fd, VMS_IOCTL_PROCSCAN, &pa);
			if (rc < 0) {
				printf("PROCTAB IOCTLFAIL PROCSCAN rc=%d (negated errno)\n", rc);
				kif_xport_dev_close(fd);
				return 4;
			}
			if ((pa.status & 1u) == 0u) {
				/* SS$_NONEXPR: scan exhausted, no match. */
				break;
			}
			if (strncmp(pa.info.prcnam, argv[2], sizeof(pa.info.prcnam)) == 0) {
				printf("PROCTAB PROCSCAN_FOUND idx=%u name=%s pid=%u\n",
				    i, pa.info.prcnam, pa.info.vms_pid);
				kif_xport_dev_close(fd);
				return 0;
			}
			/* pa.index was advanced by the facility; loop continues. */
		}
		printf("PROCTAB PROCSCAN_NOTFOUND name=%s scanned=%u\n", argv[2], i);
		kif_xport_dev_close(fd);
		return 1;
	}
}
