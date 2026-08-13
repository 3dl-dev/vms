/*
 * vmsmbx.c - OVMX/NetBSD P4-A mailbox cross-process test program (rd
 * vms-f8a, epic vms-8e8). The tool the harness runs as SEPARATE PROCESSES to
 * prove the INV-6-decisive property for the mailbox facility: a message one
 * process writes into a PERMANENT mailbox (MBAn:) is read back byte-for-byte
 * by a DIFFERENT process that $ASSIGNs the SAME device name, because the
 * mailbox and its message queue live in the executive's KERNEL memory (the
 * shared facility src/kernel-core/vms_mbx.c, compiled into the NetBSD `vms'
 * pseudo-device), not in either process (CLAUDE.md Rule 9).
 *
 * It reaches the in-kernel /dev/vms `vms' pseudo-device THROUGH the NetBSD
 * transport seam (src/libvmssys/kif_transport_netbsd.c), issuing the SAME
 * mailbox ioctls the shared facility implements. VMS_MBX_IOCTL_MAXLEN
 * (4096) message buffers push VMS_IOCTL_MBX_WRITE/READ past NetBSD's
 * one-page IOCPARM_MAX, so those two ride the IOC_VOID big-io path
 * (vms_mbx_nb.h's COPY MODEL note) -- transparent to this userspace tool,
 * which just calls kif_xport_ioctl() the same way for every op.
 *
 * USAGE (one operation per invocation, so each is its own OS process):
 *   vmsmbx create_hold <holdsecs>
 *       $CREMBX(permanent=0) -- a TEMPORARY mailbox, the same oracle
 *       Linux's own cross-process mbx proofs use (test_syssvc_mbx_crossproc.c,
 *       test_kmod_mbx.c): creating a PERMANENT mailbox needs the PRMMBX
 *       privilege, which a default-privilege test process does not hold on
 *       EITHER substrate -- test_kmod_mbx.c asserts that failure explicitly
 *       (SS$_NOPRIV) rather than dodging it. A temporary mailbox is freed the
 *       instant its last channel goes away (src/kernel-core/vms_mbx.c), so
 *       this process holds its channel open -- staying alive for <holdsecs>,
 *       mirroring vmslock's hold_release -- so a later, DIFFERENT process can
 *       still $ASSIGN it by name while A is up. Prints the assigned MBAn:
 *       device name before the hold.
 *   vmsmbx write <devnam> <text>
 *       $ASSIGN(devnam) then $QIO write <text> into it.
 *   vmsmbx read <devnam> <expected>
 *       $ASSIGN(devnam) then $QIO read; compares the bytes read against
 *       <expected> byte-for-byte.
 *
 * EXIT CODES:
 *   create_hold: 0 = mailbox created, hold completed, channel closed cleanly.
 *   write:  0 = assign+write succeeded.
 *   read:   0 = assign+read succeeded AND the bytes matched <expected>;
 *           1 = read succeeded but the bytes did NOT match.
 *   any op: 2 = usage error; 3 = /dev/vms unreachable (honest SS$_NOSUCHDEV,
 *           the module-absent negative control); 4 = ioctl delivery failed;
 *           6 = executive returned a failure status.
 *
 * The stdout lines are stable, greppable tokens the driver
 * (drive_netbsd_p4a.py) keys on: "MBX CREATE devnam=.. status=..",
 * "MBX WRITE devnam=.. len=.. status=..", "MBX READ devnam=.. len=.. match=1".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kif_transport.h"
#include "vms_mbx_nb.h"

#define VMS_SS_NOSUCHDEV 2680u

enum { OP_CREATE_HOLD, OP_WRITE, OP_READ };

static int
open_or_honest_fail(int *fd_out)
{
	int fd = kif_xport_dev_open();
	if (fd < 0) {
		printf("MBX UNREACHABLE /dev/vms open rc=%d -> honest failure, "
		    "SS$_NOSUCHDEV (%u); NOT faking success\n", fd, VMS_SS_NOSUCHDEV);
		return -1;
	}
	*fd_out = fd;
	return 0;
}

/* Assign a channel to `devnam'; returns the channel number, or 0 on failure
 * (status printed by the caller from *status_out). */
static uint32_t
assign(int fd, const char *devnam, uint32_t *status_out)
{
	struct vms_mbx_assign_args aa;
	int rc;

	memset(&aa, 0, sizeof(aa));
	strncpy(aa.devnam, devnam, sizeof(aa.devnam) - 1);
	rc = kif_xport_ioctl(fd, VMS_IOCTL_MBX_ASSIGN, &aa);
	if (rc < 0) {
		printf("MBX IOCTLFAIL ASSIGN devnam=%s rc=%d (negated errno)\n",
		    devnam, rc);
		*status_out = 0;
		return 0;
	}
	*status_out = aa.status;
	return (aa.status & 1u) ? aa.chan : 0;
}

int
main(int argc, char **argv)
{
	int op, fd, rc;

	if (argc < 2) {
		fprintf(stderr,
		    "usage: %s create_hold <holdsecs> | write <devnam> <text> | "
		    "read <devnam> <expected>\n", argv[0]);
		return 2;
	}

	if (strcmp(argv[1], "create_hold") == 0) op = OP_CREATE_HOLD;
	else if (strcmp(argv[1], "write") == 0)  op = OP_WRITE;
	else if (strcmp(argv[1], "read") == 0)   op = OP_READ;
	else {
		fprintf(stderr, "%s: unknown op '%s'\n", argv[0], argv[1]);
		return 2;
	}

	if (open_or_honest_fail(&fd) < 0)
		return 3;

	if (op == OP_CREATE_HOLD) {
		struct vms_mbx_create_args ca;
		unsigned int holdsecs;

		if (argc != 3) {
			fprintf(stderr, "usage: %s create_hold <holdsecs>\n", argv[0]);
			kif_xport_dev_close(fd);
			return 2;
		}
		holdsecs = (unsigned int)strtoul(argv[2], NULL, 0);

		memset(&ca, 0, sizeof(ca));
		ca.permanent = 0u;   /* TEMPORARY: same oracle as Linux's cross-process
		                      * mbx proofs -- PRMMBX is not held by default. */
		rc = kif_xport_ioctl(fd, VMS_IOCTL_MBX_CREATE, &ca);
		if (rc < 0) {
			printf("MBX IOCTLFAIL CREATE rc=%d (negated errno)\n", rc);
			kif_xport_dev_close(fd);
			return 4;
		}
		printf("MBX CREATE devnam=%s status=%u\n", ca.devnam, ca.status);
		fflush(stdout);
		if ((ca.status & 1u) == 0u) {
			kif_xport_dev_close(fd);
			return 6;
		}

		/* Hold the channel open: a temporary mailbox is freed the instant its
		 * last channel goes away, so this process must stay alive (fd open)
		 * for B and C to reach it by name. Closing fd here (or process exit)
		 * releases the channel via vms_close() -> vms_mbx_release_all(). */
		sleep(holdsecs);
		kif_xport_dev_close(fd);
		return 0;
	}

	if (op == OP_WRITE) {
		struct vms_mbx_write_args *wa;
		uint32_t chan, astatus;
		size_t textlen;

		if (argc != 4) {
			fprintf(stderr, "usage: %s write <devnam> <text>\n", argv[0]);
			kif_xport_dev_close(fd);
			return 2;
		}
		chan = assign(fd, argv[2], &astatus);
		if (chan == 0) {
			printf("MBX WRITE devnam=%s ASSIGN_FAILED status=%u\n",
			    argv[2], astatus);
			kif_xport_dev_close(fd);
			return 6;
		}

		wa = calloc(1, sizeof(*wa));
		if (wa == NULL) {
			kif_xport_dev_close(fd);
			return 4;
		}
		textlen = strlen(argv[3]);
		if (textlen > VMS_MBX_IOCTL_MAXLEN)
			textlen = VMS_MBX_IOCTL_MAXLEN;
		wa->chan = chan;
		wa->len = (uint32_t)textlen;
		memcpy(wa->data, argv[3], textlen);

		rc = kif_xport_ioctl(fd, VMS_IOCTL_MBX_WRITE, wa);
		kif_xport_dev_close(fd);
		if (rc < 0) {
			printf("MBX IOCTLFAIL WRITE devnam=%s rc=%d (negated errno)\n",
			    argv[2], rc);
			free(wa);
			return 4;
		}
		printf("MBX WRITE devnam=%s len=%u status=%u\n",
		    argv[2], wa->len, wa->status);
		rc = (wa->status & 1u) ? 0 : 6;
		free(wa);
		return rc;
	}

	/* read */
	{
		struct vms_mbx_read_args *ra;
		uint32_t chan, astatus;
		int match;

		if (argc != 4) {
			fprintf(stderr, "usage: %s read <devnam> <expected>\n", argv[0]);
			kif_xport_dev_close(fd);
			return 2;
		}
		chan = assign(fd, argv[2], &astatus);
		if (chan == 0) {
			printf("MBX READ devnam=%s ASSIGN_FAILED status=%u\n",
			    argv[2], astatus);
			kif_xport_dev_close(fd);
			return 6;
		}

		ra = calloc(1, sizeof(*ra));
		if (ra == NULL) {
			kif_xport_dev_close(fd);
			return 4;
		}
		ra->chan = chan;
		ra->bufsz = VMS_MBX_IOCTL_MAXLEN;
		ra->flags = 0;   /* blocking read: the writer has already run by the
		                  * time the driver invokes this op. */

		rc = kif_xport_ioctl(fd, VMS_IOCTL_MBX_READ, ra);
		kif_xport_dev_close(fd);
		if (rc < 0) {
			printf("MBX IOCTLFAIL READ devnam=%s rc=%d (negated errno)\n",
			    argv[2], rc);
			free(ra);
			return 4;
		}
		if ((ra->status & 1u) == 0u) {
			printf("MBX READ devnam=%s STATUSFAIL status=%u\n",
			    argv[2], ra->status);
			free(ra);
			return 6;
		}
		match = (ra->len == strlen(argv[3])) &&
		        (memcmp(ra->data, argv[3], ra->len) == 0);
		printf("MBX READ devnam=%s len=%u match=%d\n",
		    argv[2], ra->len, match);
		free(ra);
		return match ? 0 : 1;
	}
}
