/*
 * vmseflag.c - OVMX/NetBSD P2c event-flag test program (rd vms-4b4, epic
 * vms-8e8). The tool the harness runs as TWO SEPARATE PROCESSES to prove the
 * INV-6-decisive property: a common event flag set by one process is seen by a
 * DIFFERENT process, because the flag lives in the executive's KERNEL memory
 * (src/kernel-netbsd/vms_netbsd.c), not in either process (CLAUDE.md Rule 9).
 *
 * It reaches the in-kernel /dev/vms `vms' pseudo-device THROUGH the NetBSD
 * transport seam (src/libvmssys/kif_transport_netbsd.c, the NetBSD leaf of the
 * P1 kif_transport.h contract), and issues the common-event-flag ioctls defined
 * by the shared /dev/vms contract (vms_eflag_nb.h).
 *
 * It applies the SAME honest-failure policy the vms_kif policy layer applies:
 * if the transport cannot open the device (kif_xport_dev_open() < 0), that is an
 * honest SS$_NOSUCHDEV -- reported, exit nonzero -- NEVER a fabricated success
 * (INV-6). That is the module-absent negative control.
 *
 * USAGE (one operation per invocation, so each is its own OS process):
 *   vmseflag set   <efn>   $SETEF  efn ; print previous-state status
 *   vmseflag clear <efn>   $CLREF  efn ; print previous-state status
 *   vmseflag read  <efn>   $READEF efn ; print cluster state + whether efn set
 *
 * EXIT CODES (chosen so a shell can branch on the outcome):
 *   set/clear:  0 = executive returned success (odd status); 6 = failure status.
 *   read:       0 = the named flag is SET; 10 = it is CLEAR (both are a
 *               SUCCESSFUL read -- the code just encodes the bit for the caller);
 *               6 = the executive returned a failure status.
 *   any op:     2 = usage error; 3 = /dev/vms unreachable (honest SS$_NOSUCHDEV,
 *               the module-absent negative control); 4 = ioctl delivery failed.
 *
 * The stdout lines are stable, greppable tokens the driver (drive_netbsd_p2c.py)
 * keys on:  "EFLAG SET efn=..", "EFLAG CLEAR efn=..", "EFLAG SETEF ..".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kif_transport.h"
#include "vms_eflag_nb.h"

enum { OP_SET, OP_CLEAR, OP_READ };

static int
open_or_honest_fail(int *fd_out)
{
	int fd = kif_xport_dev_open();
	if (fd < 0) {
		/* Honest device-unreachable failure -- the SS$_NOSUCHDEV verdict,
		 * never a faked success (INV-6 / Rule 9). */
		printf("EFLAG UNREACHABLE /dev/vms open rc=%d -> honest failure, "
		    "SS$_NOSUCHDEV (%u); NOT faking success\n", fd, VMS_SS_NOSUCHDEV);
		return -1;
	}
	*fd_out = fd;
	return 0;
}

int
main(int argc, char **argv)
{
	struct vms_eflag_args ea;
	unsigned long efn;
	int op, fd, rc;
	unsigned long cmd;
	const char *opname;

	if (argc != 3) {
		fprintf(stderr, "usage: %s {set|clear|read} <efn>\n", argv[0]);
		return 2;
	}
	if (strcmp(argv[1], "set") == 0)        { op = OP_SET;   opname = "SETEF";  cmd = VMS_IOCTL_SETEF;  }
	else if (strcmp(argv[1], "clear") == 0) { op = OP_CLEAR; opname = "CLREF";  cmd = VMS_IOCTL_CLREF;  }
	else if (strcmp(argv[1], "read") == 0)  { op = OP_READ;  opname = "READEF"; cmd = VMS_IOCTL_READEF; }
	else {
		fprintf(stderr, "%s: unknown op '%s'\n", argv[0], argv[1]);
		return 2;
	}
	efn = strtoul(argv[2], NULL, 0);

	if (open_or_honest_fail(&fd) < 0)
		return 3;

	memset(&ea, 0, sizeof(ea));
	ea.efn = (uint32_t)efn;

	rc = kif_xport_ioctl(fd, cmd, &ea);
	kif_xport_dev_close(fd);

	if (rc < 0) {
		printf("EFLAG IOCTLFAIL %s efn=%lu rc=%d (negated errno)\n",
		    opname, efn, rc);
		return 4;
	}
	if ((ea.status & 1u) == 0u) {
		/* Even status = a VMS failure (e.g. SS$_ILLEFC for an EFN outside the
		 * common range). Report it honestly; do not pretend it worked. */
		printf("EFLAG STATUSFAIL %s efn=%lu status=%u (even/failure)\n",
		    opname, efn, ea.status);
		return 6;
	}

	if (op == OP_READ) {
		unsigned bit   = (unsigned)((efn - VMS_EF_COMMON_LO) & 31u);
		int      isset = (ea.state & (1u << bit)) != 0;
		printf("EFLAG %s efn=%lu state=0x%08x status=%u\n",
		    isset ? "SET" : "CLEAR", efn, ea.state, ea.status);
		return isset ? 0 : 10;
	}

	/* set / clear: status carries the PREVIOUS state (WASSET=9 / WASCLR=1). */
	printf("EFLAG %s efn=%lu prevstatus=%u (%s)\n",
	    opname, efn, ea.status,
	    ea.status == VMS_SS_WASSET ? "was-set" : "was-clear");
	return 0;
}
