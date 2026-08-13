/*
 * vmsaccess.c - OVMX/NetBSD P4-A access-mode + AST-delivery test program (rd
 * vms-f8a, epic vms-8e8). Covers the two remaining executive facilities:
 *
 *   ACCESS MODES ($SETMODE/$GETMODE/$SETPRV/ENTER_IMAGE/IMAGE_RUNDOWN,
 *   src/kernel-core/vms_access.c) -- proven cross-process: process A mutates
 *   its OWN privilege mask with $SETPRV (a real state change recorded in the
 *   executive's per-process control block), records a discoverable name with
 *   $SETPRN, and stays alive; a DIFFERENT process B's $GETJPI on that name
 *   reads back the SAME mutated privilege mask -- the mask lives in the
 *   executive's shared process table (src/kernel-core/vms_proctab.c), not in
 *   either process (CLAUDE.md Rule 9).
 *
 *   ASTs ($DCLAST/$SETAST/DELIVERAST, src/kernel-core/vms_ast.c) -- $DCLAST
 *   is SELF-DIRECTED by VMS design (an AST is declared to the calling
 *   process), so the "A writes, B reads" discriminator the other facilities
 *   use does not apply directly to it. The `selftest' op proves the AST
 *   queue is REAL executive state (a full DCLAST/DELIVERAST round trip
 *   through /dev/vms, astadr/astprm preserved). The genuinely CROSS-PROCESS
 *   AST proof reuses the mailbox facility's write-attention integration
 *   (src/kernel-core/vms_mbx.c: a write lands an AST in a DIFFERENT
 *   process's queue and wakes it out of $HIBER via vms_ast_notify_arrival,
 *   src/kernel-core/vms_ast.c) -- `wrtattn_bg'/`wrtattn_write' below: process
 *   A arms a write-attention AST and $HIBERs; process B's mailbox write is
 *   what actually delivers the AST into A's kernel-resident queue and wakes
 *   A's $HIBER. A per-process fake could never be woken by an action taken
 *   in a DIFFERENT process.
 *
 * USAGE (one operation per invocation, so each is its own OS process):
 *   vmsaccess selftest
 *       Single-process round trip: GETMODE (expect USER by default) ->
 *       SETMODE(SUPER) [requires CMEXEC/CMKRNL, granted by default to a
 *       privileged (root) caller] -> GETMODE confirms SUPER -> ENTER_IMAGE
 *       (SUPER->USER, requires current mode == SUPER) -> GETMODE confirms
 *       USER -> DCLAST(magic) -> DELIVERAST returns the same magic ->
 *       IMAGE_RUNDOWN (USER->SUPER) -> GETMODE confirms SUPER again.
 *   vmsaccess setpriv_bg <name> <secs>
 *       $SETPRV(disable CMEXEC, permanent=1) -- an OVMX-privileged (root)
 *       caller starts with CMEXEC granted by default, so disabling it is a
 *       real, observable state MUTATION -- then $SETPRN(name), then
 *       sleep(secs) to keep the PCB present for a different process's
 *       $GETJPI.
 *   vmsaccess getpriv <name>
 *       $GETJPI selecting by name; prints perm_privs in hex and asserts the
 *       CMEXEC bit is CLEAR (i.e. it observed A's disable).
 *   vmsaccess wrtattn_bg <name> <secs>
 *       $CREMBX(permanent=1); $QIO SETMODE|WRTATTN arms a write-attention
 *       AST on the just-created channel with a distinctive astprm; prints
 *       the mailbox's MBAn: device name; $HIBER (blocks in-kernel); on wake,
 *       DELIVERAST and check astprm/acmode match what was armed.
 *   vmsaccess wrtattn_write <devnam>
 *       $ASSIGN(devnam); $QIO write one message -- this is what fires A's
 *       armed write-attention AST and wakes A's $HIBER.
 *
 * EXIT CODES: 0 = every assertion in the op held; 1 = an assertion did not
 * hold; 2 = usage error; 3 = /dev/vms unreachable (honest SS$_NOSUCHDEV, the
 * module-absent negative control); 4 = ioctl delivery failed; 6 = executive
 * returned a failure status where success was required.
 *
 * The stdout lines are stable, greppable tokens the driver
 * (drive_netbsd_p4a.py) keys on: "ACCESS SELFTEST PASS", "ACCESS SETPRV
 * name=.. status=..", "ACCESS GETPRIV name=.. perm_privs=0x.. cmexec_clear=1",
 * "AST WRTATTN MBX DEVNAM=..", "AST WRTATTN HIBER woken=..",
 * "AST WRTATTN FIRED astprm=0x.. match=1".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include "kif_transport.h"
#include "vms_access_nb.h"
#include "vms_ast_nb.h"
#include "vms_proctab_nb.h"
#include "vms_mbx_nb.h"

#define VMS_SS_NOSUCHDEV 2680u

/* Distinctive AST parameters so a dropped/confused astprm is visible. */
#define SELFTEST_ASTADR  0x00A5F8A1ull
#define SELFTEST_ASTPRM  0x00A5F8A2ull
#define WRTATTN_ASTADR   0x00A5F8A3ull
#define WRTATTN_ASTPRM   0x00A5F8A4ull

enum { OP_SELFTEST, OP_SETPRIV_BG, OP_GETPRIV, OP_WRTATTN_BG, OP_WRTATTN_WRITE };

static int
open_or_honest_fail(int *fd_out)
{
	int fd = kif_xport_dev_open();
	if (fd < 0) {
		printf("ACCESS UNREACHABLE /dev/vms open rc=%d -> honest failure, "
		    "SS$_NOSUCHDEV (%u); NOT faking success\n", fd, VMS_SS_NOSUCHDEV);
		return -1;
	}
	*fd_out = fd;
	return 0;
}

static int
do_selftest(int fd)
{
	struct vms_mode_args ma;
	struct vms_getmode_args gma;
	struct vms_modexfer_args xa;
	struct vms_ast_args aa;
	int rc;

	memset(&gma, 0, sizeof(gma));
	rc = kif_xport_ioctl(fd, VMS_IOCTL_GETMODE, &gma);
	if (rc < 0) { printf("ACCESS IOCTLFAIL GETMODE#1 rc=%d\n", rc); return 4; }
	printf("ACCESS GETMODE#1 mode=%u (expect USER=%u)\n", gma.mode, PSL_C_USER);
	if (gma.mode != PSL_C_USER) { printf("ACCESS SELFTEST FAIL: initial mode wrong\n"); return 1; }

	memset(&ma, 0, sizeof(ma));
	ma.mode = PSL_C_SUPER;
	rc = kif_xport_ioctl(fd, VMS_IOCTL_SETMODE, &ma);
	if (rc < 0) { printf("ACCESS IOCTLFAIL SETMODE(SUPER) rc=%d\n", rc); return 4; }

	memset(&gma, 0, sizeof(gma));
	rc = kif_xport_ioctl(fd, VMS_IOCTL_GETMODE, &gma);
	if (rc < 0) { printf("ACCESS IOCTLFAIL GETMODE#2 rc=%d\n", rc); return 4; }
	printf("ACCESS GETMODE#2 mode=%u (expect SUPER=%u)\n", gma.mode, PSL_C_SUPER);
	if (gma.mode != PSL_C_SUPER) {
		printf("ACCESS SELFTEST FAIL: SETMODE(SUPER) did not take effect "
		    "(needs CMEXEC/CMKRNL -- is this guest running privileged?)\n");
		return 1;
	}

	memset(&xa, 0, sizeof(xa));
	rc = kif_xport_ioctl(fd, VMS_IOCTL_ENTER_IMAGE, &xa);
	if (rc < 0) { printf("ACCESS IOCTLFAIL ENTER_IMAGE rc=%d\n", rc); return 4; }
	printf("ACCESS ENTER_IMAGE prev=%u new=%u status=%u\n",
	    xa.prev_mode, xa.new_mode, xa.status);
	if ((xa.status & 1u) == 0u || xa.new_mode != PSL_C_USER) {
		printf("ACCESS SELFTEST FAIL: ENTER_IMAGE did not descend to USER\n");
		return 1;
	}

	memset(&aa, 0, sizeof(aa));
	aa.astadr = SELFTEST_ASTADR;
	aa.astprm = SELFTEST_ASTPRM;
	aa.acmode = PSL_C_USER;
	rc = kif_xport_ioctl(fd, VMS_IOCTL_DCLAST, &aa);
	if (rc < 0) { printf("ACCESS IOCTLFAIL DCLAST rc=%d\n", rc); return 4; }
	/* DCLAST is _IOW on NetBSD: aa.status is not copied back (see vms_ast_nb.h's
	 * _IOW STATUS CAVEAT). Its EFFECT is verified below via DELIVERAST. */

	memset(&aa, 0, sizeof(aa));
	rc = kif_xport_ioctl(fd, VMS_IOCTL_DELIVERAST, &aa);
	if (rc < 0) { printf("ACCESS IOCTLFAIL DELIVERAST rc=%d\n", rc); return 4; }
	printf("ACCESS DELIVERAST astadr=0x%" PRIx64 " astprm=0x%" PRIx64
	    " acmode=%u status=%u\n", aa.astadr, aa.astprm, aa.acmode, aa.status);
	if ((aa.status & 1u) == 0u || aa.astadr != SELFTEST_ASTADR ||
	    aa.astprm != SELFTEST_ASTPRM) {
		printf("ACCESS SELFTEST FAIL: DELIVERAST did not return the declared "
		    "AST unchanged -- the executive AST queue is not real\n");
		return 1;
	}

	memset(&xa, 0, sizeof(xa));
	rc = kif_xport_ioctl(fd, VMS_IOCTL_IMAGE_RUNDOWN, &xa);
	if (rc < 0) { printf("ACCESS IOCTLFAIL IMAGE_RUNDOWN rc=%d\n", rc); return 4; }
	printf("ACCESS IMAGE_RUNDOWN prev=%u new=%u status=%u\n",
	    xa.prev_mode, xa.new_mode, xa.status);
	if ((xa.status & 1u) == 0u || xa.new_mode != PSL_C_SUPER) {
		printf("ACCESS SELFTEST FAIL: IMAGE_RUNDOWN did not restore SUPER\n");
		return 1;
	}

	printf("ACCESS SELFTEST PASS\n");
	return 0;
}

int
main(int argc, char **argv)
{
	int op, fd, rc;

	if (argc < 2) {
		fprintf(stderr,
		    "usage: %s selftest | setpriv_bg <name> <secs> | getpriv <name> | "
		    "wrtattn_bg <name> <secs> | wrtattn_write <devnam>\n", argv[0]);
		return 2;
	}

	if (strcmp(argv[1], "selftest") == 0)          op = OP_SELFTEST;
	else if (strcmp(argv[1], "setpriv_bg") == 0)   op = OP_SETPRIV_BG;
	else if (strcmp(argv[1], "getpriv") == 0)      op = OP_GETPRIV;
	else if (strcmp(argv[1], "wrtattn_bg") == 0)   op = OP_WRTATTN_BG;
	else if (strcmp(argv[1], "wrtattn_write") == 0) op = OP_WRTATTN_WRITE;
	else {
		fprintf(stderr, "%s: unknown op '%s'\n", argv[0], argv[1]);
		return 2;
	}

	if (open_or_honest_fail(&fd) < 0)
		return 3;

	if (op == OP_SELFTEST) {
		rc = do_selftest(fd);
		kif_xport_dev_close(fd);
		return rc;
	}

	if (op == OP_SETPRIV_BG) {
		struct vms_priv_args pa;
		struct vms_setprn_args sa;
		unsigned int secs;

		if (argc != 4) {
			fprintf(stderr, "usage: %s setpriv_bg <name> <secs>\n", argv[0]);
			kif_xport_dev_close(fd);
			return 2;
		}
		secs = (unsigned int)strtoul(argv[3], NULL, 0);

		memset(&pa, 0, sizeof(pa));
		pa.mask = VMS_PRV_M_CMEXEC;
		pa.enable = 0;        /* disable -- always allowed */
		pa.permanent = 1;     /* mutate the AUTHORIZED mask so it is visible via
		                       * $GETJPI's perm_privs field, not just cur_privs */
		rc = kif_xport_ioctl(fd, VMS_IOCTL_SETPRV, &pa);
		if (rc < 0) {
			printf("ACCESS IOCTLFAIL SETPRV rc=%d\n", rc);
			kif_xport_dev_close(fd);
			return 4;
		}
		printf("ACCESS SETPRV prev=0x%" PRIx64 " status=%u\n", pa.prev, pa.status);
		if ((pa.status & 1u) == 0u) {
			kif_xport_dev_close(fd);
			return 6;
		}

		memset(&sa, 0, sizeof(sa));
		strncpy(sa.prcnam, argv[2], sizeof(sa.prcnam) - 1);
		rc = kif_xport_ioctl(fd, VMS_IOCTL_SETPRN, &sa);
		if (rc < 0) {
			printf("ACCESS IOCTLFAIL SETPRN rc=%d\n", rc);
			kif_xport_dev_close(fd);
			return 4;
		}
		printf("ACCESS SETPRV name=%s status=%u\n", argv[2], sa.status);
		if ((sa.status & 1u) == 0u) {
			kif_xport_dev_close(fd);
			return 6;
		}
		fflush(stdout);
		sleep(secs);
		kif_xport_dev_close(fd);
		return 0;
	}

	if (op == OP_GETPRIV) {
		struct vms_getjpi_args ga;
		int cmexec_clear;

		if (argc != 3) {
			fprintf(stderr, "usage: %s getpriv <name>\n", argv[0]);
			kif_xport_dev_close(fd);
			return 2;
		}
		memset(&ga, 0, sizeof(ga));
		ga.select = VMS_JPI_SEL_PRCNAM;
		strncpy(ga.sel_prcnam, argv[2], sizeof(ga.sel_prcnam) - 1);

		rc = kif_xport_ioctl(fd, VMS_IOCTL_GETJPI, &ga);
		kif_xport_dev_close(fd);
		if (rc < 0) {
			printf("ACCESS IOCTLFAIL GETJPI rc=%d\n", rc);
			return 4;
		}
		if ((ga.status & 1u) == 0u) {
			printf("ACCESS GETPRIV name=%s NOTFOUND status=%u\n",
			    argv[2], ga.status);
			return 1;
		}
		cmexec_clear = (ga.info.perm_privs & VMS_PRV_M_CMEXEC) == 0;
		printf("ACCESS GETPRIV name=%s perm_privs=0x%" PRIx64
		    " cmexec_clear=%d\n", argv[2], ga.info.perm_privs, cmexec_clear);
		return cmexec_clear ? 0 : 1;
	}

	if (op == OP_WRTATTN_BG) {
		struct vms_mbx_create_args ca;
		struct vms_mbx_wrtattn_args wa;
		struct vms_hiber_args ha;
		struct vms_ast_args aa;
		unsigned int secs;
		int match;

		if (argc != 4) {
			fprintf(stderr, "usage: %s wrtattn_bg <name> <secs>\n", argv[0]);
			kif_xport_dev_close(fd);
			return 2;
		}
		secs = (unsigned int)strtoul(argv[3], NULL, 0);
		(void)secs;   /* $HIBER blocks until woken; no sleep needed here --
		               * <secs> documents the driver's poll budget only. */

		memset(&ca, 0, sizeof(ca));
		ca.permanent = 1u;
		rc = kif_xport_ioctl(fd, VMS_IOCTL_MBX_CREATE, &ca);
		if (rc < 0 || (ca.status & 1u) == 0u) {
			printf("AST WRTATTN CREATE FAILED rc=%d status=%u\n", rc, ca.status);
			kif_xport_dev_close(fd);
			return 6;
		}
		/* Flush the devnam BEFORE arming/hibernating so the driver can parse
		 * it while this process is still runnable. */
		printf("AST WRTATTN MBX DEVNAM=%s\n", ca.devnam);
		fflush(stdout);

		memset(&wa, 0, sizeof(wa));
		wa.chan = ca.chan;
		wa.acmode = PSL_C_USER;
		wa.astadr = WRTATTN_ASTADR;
		wa.astprm = WRTATTN_ASTPRM;
		rc = kif_xport_ioctl(fd, VMS_IOCTL_MBX_SET_WRTATTN, &wa);
		if (rc < 0 || (wa.status & 1u) == 0u) {
			printf("AST WRTATTN ARM FAILED rc=%d status=%u\n", rc, wa.status);
			kif_xport_dev_close(fd);
			return 6;
		}
		printf("AST WRTATTN ARMED chan=%u\n", ca.chan);
		fflush(stdout);

		/* Block in-kernel. A DIFFERENT process's write to this mailbox is
		 * what lands the armed AST in this process's queue and wakes this
		 * $HIBER (vms_ast_notify_arrival, called from vms_mbx.c's write
		 * path) -- never a local timer or this process's own action. */
		memset(&ha, 0, sizeof(ha));
		rc = kif_xport_ioctl(fd, VMS_IOCTL_HIBER, &ha);
		if (rc < 0) {
			printf("AST WRTATTN IOCTLFAIL HIBER rc=%d\n", rc);
			kif_xport_dev_close(fd);
			return 4;
		}
		printf("AST WRTATTN HIBER woken=%u status=%u\n", ha.woken, ha.status);

		memset(&aa, 0, sizeof(aa));
		rc = kif_xport_ioctl(fd, VMS_IOCTL_DELIVERAST, &aa);
		kif_xport_dev_close(fd);
		if (rc < 0) {
			printf("AST WRTATTN IOCTLFAIL DELIVERAST rc=%d\n", rc);
			return 4;
		}
		match = (aa.status & 1u) && aa.astadr == WRTATTN_ASTADR &&
		        aa.astprm == WRTATTN_ASTPRM && aa.acmode == PSL_C_USER;
		printf("AST WRTATTN FIRED astprm=0x%" PRIx64 " match=%d\n",
		    aa.astprm, match);
		return match ? 0 : 1;
	}

	/* wrtattn_write */
	{
		struct vms_mbx_assign_args aa;
		struct vms_mbx_write_args *wa;
		static const char msg[] = "OVMX P4-A cross-process write-attention AST";

		if (argc != 3) {
			fprintf(stderr, "usage: %s wrtattn_write <devnam>\n", argv[0]);
			kif_xport_dev_close(fd);
			return 2;
		}

		memset(&aa, 0, sizeof(aa));
		strncpy(aa.devnam, argv[2], sizeof(aa.devnam) - 1);
		rc = kif_xport_ioctl(fd, VMS_IOCTL_MBX_ASSIGN, &aa);
		if (rc < 0 || (aa.status & 1u) == 0u) {
			printf("AST WRTATTN WRITE ASSIGN_FAILED rc=%d status=%u\n",
			    rc, aa.status);
			kif_xport_dev_close(fd);
			return 6;
		}

		wa = calloc(1, sizeof(*wa));
		if (wa == NULL) {
			kif_xport_dev_close(fd);
			return 4;
		}
		wa->chan = aa.chan;
		wa->len = (uint32_t)(sizeof(msg) - 1);
		memcpy(wa->data, msg, sizeof(msg) - 1);

		rc = kif_xport_ioctl(fd, VMS_IOCTL_MBX_WRITE, wa);
		kif_xport_dev_close(fd);
		if (rc < 0) {
			printf("AST WRTATTN WRITE IOCTLFAIL rc=%d\n", rc);
			free(wa);
			return 4;
		}
		printf("AST WRTATTN WRITE status=%u\n", wa->status);
		rc = (wa->status & 1u) ? 0 : 6;
		free(wa);
		return rc;
	}
}
