/*
 * vmsexitcompl.c - OVMX/NetBSD: a creator's armed /NOWAIT completion fires when
 * its subprocess ENDS, whichever thread ends it (rd vms-003b).
 *
 * The creator arms VMS_IOCTL_SPAWN_NOTIFY (efn 11) on a child process that has a
 * PCB of its own. The child never records an exit ($EXIT): its main thread
 * pthread_exit()s at once and a second thread ends the process 200 ms later --
 * the shape of DCL with its SYS$INPUT reader thread. The executive must notice
 * the process ended and set the creator's flag (VMS notifies the creator when a
 * subprocess is deleted, however it ends). The creator only polls $READEF, which
 * is not a process-table operation, so nothing else can have reclaimed the child
 * for it. The child is reaped (waitpid) only AFTER the verdict.
 *
 * Reaches the in-kernel /dev/vms through kif_transport_netbsd.c, the same
 * ioctls the shared facility implements. Prints one greppable token:
 *   EXITCOMPL SET waited_ms=..     the flag was set (exit 0)
 *   EXITCOMPL LOST waited_ms=..    not set within 10 s (exit 1)
 *   EXITCOMPL SETUPFAIL ...        could not arm (exit 5)
 *   ... NOT faking success          /dev/vms unreachable (exit 3, honest)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <sys/wait.h>

#include "kif_transport.h"
#include "vms_eflag_nb.h"
#include "vms_proctab_nb.h"

#define EFN 11u

static void *
tail(void *v)
{
	(void)v;
	poll(NULL, 0, 200);
	exit(0);
}

static int
readef(int fd, unsigned efn, int *set)
{
	struct vms_ef_read_args ra;
	memset(&ra, 0, sizeof(ra));
	ra.efn = efn;
	if (kif_xport_ioctl(fd, VMS_IOCTL_READEF, &ra) < 0 || !(ra.status & 1u))
		return -1;
	*set = (ra.state & (1u << (efn & 31u))) != 0;
	return 0;
}

int
main(void)
{
	int fd = kif_xport_dev_open();
	if (fd < 0) {
		printf("EXITCOMPL UNREACHABLE /dev/vms open rc=%d -> honest failure, "
		    "SS$_NOSUCHDEV; NOT faking success\n", fd);
		return 3;
	}
	struct vms_ef_args ea;
	memset(&ea, 0, sizeof(ea));
	ea.efn = EFN;
	(void)kif_xport_ioctl(fd, VMS_IOCTL_CLREF, &ea);     /* this process's PCB */

	int hold[2], up[2], go[2];
	if (pipe(hold) != 0 || pipe(up) != 0 || pipe(go) != 0) {
		printf("EXITCOMPL SETUPFAIL pipe\n");
		return 5;
	}
	pid_t child = fork();
	if (child < 0) {
		printf("EXITCOMPL SETUPFAIL fork\n");
		return 5;
	}
	if (child == 0) {
		char h = 0;
		close(hold[1]); close(up[0]); close(go[1]);
		/* Held until this creator has made it a VMS process (rd vms-6ed): a
		 * task the executive did not create has no PCB. */
		if (read(hold[0], &h, 1) != 1 || h != 'g')
			_exit(125);
		struct vms_ef_args c;
		memset(&c, 0, sizeof(c));
		c.efn = 1;
		int ok = kif_xport_ioctl(fd, VMS_IOCTL_CLREF, &c) >= 0;   /* its own PCB */
		char r = ok ? 'u' : 'x', g;
		(void)write(up[1], &r, 1);
		(void)read(go[0], &g, 1);
		pthread_t t;
		if (pthread_create(&t, NULL, tail, NULL) != 0)
			_exit(2);
		pthread_exit(NULL);        /* the main thread ends first */
	}
	close(hold[0]); close(up[1]); close(go[0]);
	/* $CREPRC: the executive makes the child's PCB, this process its creator. */
	struct vms_creprc_pcb_args cp;
	memset(&cp, 0, sizeof(cp));
	cp.child_pid = (uint32_t)child;
	cp.flags = VMS_CREPRC_PCB_SUBPROCESS;
	int crc = kif_xport_ioctl(fd, VMS_IOCTL_CREPRC_PCB, &cp);
	(void)write(hold[1], (crc >= 0 && (cp.status & 1u)) ? "g" : "x", 1);
	if (crc < 0 || !(cp.status & 1u)) {
		printf("EXITCOMPL SETUPFAIL creprc_pcb rc=%d status=%u\n", crc, cp.status);
		(void)waitpid(child, NULL, 0);
		return 5;
	}
	char r = 0;
	if (read(up[0], &r, 1) != 1 || r != 'u') {
		printf("EXITCOMPL SETUPFAIL child registration\n");
		return 5;
	}
	struct vms_spawn_notify_args sn;
	memset(&sn, 0, sizeof(sn));
	sn.child_vms_pid = cp.vms_pid;        /* the PCB the executive made */
	sn.efn = EFN;
	int arc = kif_xport_ioctl(fd, VMS_IOCTL_SPAWN_NOTIFY, &sn);
	(void)write(go[1], "g", 1);
	if (arc < 0 || !(sn.status & 1u)) {
		printf("EXITCOMPL SETUPFAIL arm rc=%d status=%u\n", arc, sn.status);
		(void)waitpid(child, NULL, 0);
		return 5;
	}
	int set = 0, waited = 0;
	for (; waited < 10000; waited += 50) {
		if (readef(fd, EFN, &set) == 0 && set)
			break;
		poll(NULL, 0, 50);
	}
	printf("EXITCOMPL %s waited_ms=%d child=%d\n", set ? "SET" : "LOST", waited,
	    (int)child);
	fflush(stdout);
	(void)waitpid(child, NULL, 0);
	kif_xport_dev_close(fd);
	return set ? 0 : 1;
}
