/*
 * vmseflag.c - OVMX/NetBSD P2c event-flag test program (rd vms-4b4, epic
 * vms-8e8). The tool the harness runs as SEPARATE PROCESSES to prove the
 * INV-6-decisive property: a COMMON event flag set by one process is seen by a
 * DIFFERENT process, because the flag lives in the executive's KERNEL memory
 * (the shared facility src/kernel-core/vms_eflag.c, compiled into the NetBSD
 * `vms' pseudo-device), not in either process (CLAUDE.md Rule 9).
 *
 * It reaches the in-kernel /dev/vms `vms' pseudo-device THROUGH the NetBSD
 * transport seam (src/libvmssys/kif_transport_netbsd.c), and issues the SAME
 * event-flag ioctls the shared facility implements, using the arg structs and
 * IOC_VOID request numbers of the shared contract (vms_eflag_nb.h).
 *
 * VMS-FAITHFUL ASSOCIATION. On OpenVMS a process must $ASCEFC a common event
 * flag cluster before it can $SETEF/$READEF a flag in it; the shared facility
 * enforces exactly that (an unassociated common EFN answers SS$_UNASEFC). So
 * every op here FIRST associates the well-known PERMANENT cluster that contains
 * the requested EFN, then performs the op. Marking the cluster PERMANENT is what
 * makes its flag state outlive the setting process, so a later, different
 * process observes the set -- that persistence is the proof. This is real VMS
 * behaviour, not a per-process shortcut (INV-6).
 *
 * It applies the SAME honest-failure policy the vms_kif policy layer applies: if
 * the transport cannot open the device (kif_xport_dev_open() < 0), that is an
 * honest SS$_NOSUCHDEV -- reported, exit nonzero -- NEVER a fabricated success
 * (INV-6). That is the module-absent negative control.
 *
 * USAGE (one operation per invocation, so each is its own OS process):
 *   vmseflag set   <efn>   $ASCEFC then $SETEF  efn ; print previous-state status
 *   vmseflag clear <efn>   $ASCEFC then $CLREF  efn ; print previous-state status
 *   vmseflag read  <efn>   $ASCEFC then $READEF efn ; print cluster state + bit
 *   vmseflag wait  <efn>   $ASCEFC then $WAITFR efn ; BLOCKS until the flag is
 *                          set by another process, then prints WAITDONE
 *
 * EXIT CODES (chosen so a shell can branch on the outcome):
 *   set/clear:  0 = executive returned success (odd status); 6 = failure status.
 *   read:       0 = the named flag is SET; 10 = it is CLEAR (both are a
 *               SUCCESSFUL read -- the code just encodes the bit for the caller);
 *               6 = the executive returned a failure status.
 *   wait:       0 = the wait completed (flag was set by another process); 6 =
 *               failure status.
 *   any op:     2 = usage error; 3 = /dev/vms unreachable (honest SS$_NOSUCHDEV,
 *               the module-absent negative control); 4 = ioctl delivery failed;
 *               5 = $ASCEFC failed (could not associate the common cluster).
 *
 * The stdout lines are stable, greppable tokens the driver (drive_netbsd_p2c.py)
 * keys on:  "EFLAG SETEF efn=..", "EFLAG SET efn=..", "EFLAG CLEAR efn=..",
 * "EFLAG WAITDONE efn=..".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kif_transport.h"
#include "vms_eflag_nb.h"

enum { OP_SET, OP_CLEAR, OP_READ, OP_WAIT };

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

/*
 * associate_common - $ASCEFC the well-known PERMANENT cluster that contains
 * `efn'. Required before any $SETEF/$READEF/$WAITFR on a common flag (the shared
 * facility answers SS$_UNASEFC otherwise). perm=1 makes the cluster outlive this
 * process, which is what lets a later process observe an earlier set.
 */
static int
associate_common(int fd, unsigned long efn)
{
	struct vms_ef_common_args ca;
	unsigned int base = (efn < 96u) ? 64u : 96u;
	const char *name  = (efn < 96u) ? VMS_P2C_CLUSTER2_NAME : VMS_P2C_CLUSTER3_NAME;
	int rc;

	memset(&ca, 0, sizeof(ca));
	ca.efn  = base;
	ca.perm = 1u;                 /* permanent: state survives process exit */
	ca.prot = 0u;
	strncpy(ca.name, name, sizeof(ca.name) - 1);
	ca.name[sizeof(ca.name) - 1] = '\0';

	rc = kif_xport_ioctl(fd, VMS_IOCTL_ASCEFC, &ca);
	if (rc < 0) {
		printf("EFLAG IOCTLFAIL ASCEFC efn=%u rc=%d (negated errno)\n", base, rc);
		return -1;
	}
	if ((ca.status & 1u) == 0u) {
		printf("EFLAG STATUSFAIL ASCEFC efn=%u status=%u (even/failure)\n",
		    base, ca.status);
		return -1;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	unsigned long efn;
	int op, fd, rc;
	const char *opname;

	if (argc != 3) {
		fprintf(stderr, "usage: %s {set|clear|read|wait} <efn>\n", argv[0]);
		return 2;
	}
	if (strcmp(argv[1], "set") == 0)        { op = OP_SET;   opname = "SETEF";  }
	else if (strcmp(argv[1], "clear") == 0) { op = OP_CLEAR; opname = "CLREF";  }
	else if (strcmp(argv[1], "read") == 0)  { op = OP_READ;  opname = "READEF"; }
	else if (strcmp(argv[1], "wait") == 0)  { op = OP_WAIT;  opname = "WAITFR"; }
	else {
		fprintf(stderr, "%s: unknown op '%s'\n", argv[0], argv[1]);
		return 2;
	}
	efn = strtoul(argv[2], NULL, 0);

	if (open_or_honest_fail(&fd) < 0)
		return 3;

	/* VMS-faithful: associate the common cluster before touching a flag in it. */
	if (associate_common(fd, efn) < 0) {
		kif_xport_dev_close(fd);
		return 5;
	}

	if (op == OP_READ) {
		struct vms_ef_read_args ra;
		unsigned int bit   = (unsigned int)((efn - VMS_EF_COMMON_LO) & 31u);
		int          isset;

		memset(&ra, 0, sizeof(ra));
		ra.efn = (uint32_t)efn;
		rc = kif_xport_ioctl(fd, VMS_IOCTL_READEF, &ra);
		kif_xport_dev_close(fd);
		if (rc < 0) {
			printf("EFLAG IOCTLFAIL %s efn=%lu rc=%d (negated errno)\n",
			    opname, efn, rc);
			return 4;
		}
		if ((ra.status & 1u) == 0u) {
			printf("EFLAG STATUSFAIL %s efn=%lu status=%u (even/failure)\n",
			    opname, efn, ra.status);
			return 6;
		}
		isset = (ra.state & (1u << bit)) != 0;
		printf("EFLAG %s efn=%lu state=0x%08x status=%u\n",
		    isset ? "SET" : "CLEAR", efn, ra.state, ra.status);
		return isset ? 0 : 10;
	}

	if (op == OP_WAIT) {
		struct vms_ef_args wa;

		memset(&wa, 0, sizeof(wa));
		wa.efn = (uint32_t)efn;
		/* Blocks in-kernel (exec_cv_wait) until another process $SETEFs the
		 * flag in this shared common cluster; returns SS$_NORMAL then. */
		rc = kif_xport_ioctl(fd, VMS_IOCTL_WAITFR, &wa);
		kif_xport_dev_close(fd);
		if (rc < 0) {
			printf("EFLAG IOCTLFAIL %s efn=%lu rc=%d (negated errno)\n",
			    opname, efn, rc);
			return 4;
		}
		if ((wa.status & 1u) == 0u) {
			printf("EFLAG STATUSFAIL %s efn=%lu status=%u (even/failure)\n",
			    opname, efn, wa.status);
			return 6;
		}
		printf("EFLAG WAITDONE efn=%lu status=%u\n", efn, wa.status);
		return 0;
	}

	/* set / clear */
	{
		struct vms_ef_args ea;
		unsigned long cmd = (op == OP_SET) ? VMS_IOCTL_SETEF : VMS_IOCTL_CLREF;

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
			printf("EFLAG STATUSFAIL %s efn=%lu status=%u (even/failure)\n",
			    opname, efn, ea.status);
			return 6;
		}
		/* status carries the PREVIOUS state (WASSET=9 / WASCLR=1). */
		printf("EFLAG %s efn=%lu prevstatus=%u (%s)\n",
		    opname, efn, ea.status,
		    ea.status == VMS_SS_WASSET ? "was-set" : "was-clear");
		return 0;
	}
}
