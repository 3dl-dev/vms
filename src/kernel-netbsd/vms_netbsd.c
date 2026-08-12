/*
 * vms_netbsd.c - the OVMX/NetBSD `vms' pseudo-device (rd vms-bfe, parent
 * vms-dd8, epic vms-8e8; docs/design-ovmx-netbsd-syskrnl.md).
 *
 * The NetBSD-substrate sibling of the Linux executive (src/kernel/, vms.ko).
 * This is the MINIMAL, vertical P2b slice: a real in-kernel cdevsw character
 * device that creates /dev/vms and answers ONE request -- the version/ping
 * handshake defined by the shared /dev/vms contract in vms_ping.h. A full
 * executive facility (event flags, locks) is P2c and is deliberately NOT built
 * here.
 *
 * It is a LOADABLE module (module(9), modload/modunload). On amd64 that is the
 * simplest way to prove a real in-kernel /dev/vms without rebuilding the
 * kernel; the harness (tests/netbsd/) builds it in-guest against the installed
 * kernel sources, loads it, and drives the probe through the transport seam.
 *
 * INV-6 / CLAUDE.md Rule 9. This module IS the executive on the NetBSD
 * substrate: it provides /dev/vms honestly. When it is not loaded, the device
 * node does not exist, open() returns an error, and the probe reports an honest
 * SS$_NOSUCHDEV-equivalent -- there is no userspace fake anywhere in the path.
 *
 * CLEAN ROOM (CLAUDE.md Rule 8). Written from the public NetBSD cdevsw(9) /
 * module(9) interfaces (sys/conf.h, sys/module.h) and the OVMX /dev/vms ping
 * contract. No NetBSD kernel source is copied beyond the public driver-API
 * idioms every out-of-tree driver uses.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/ioccom.h>
#include <sys/errno.h>

#include "vms_ping.h"

static dev_type_open(vms_open);
static dev_type_close(vms_close);
static dev_type_ioctl(vms_ioctl);

static struct cdevsw vms_cdevsw = {
	.d_open     = vms_open,
	.d_close    = vms_close,
	.d_read     = noread,
	.d_write    = nowrite,
	.d_ioctl    = vms_ioctl,
	.d_stop     = nostop,
	.d_tty      = notty,
	.d_poll     = nopoll,
	.d_mmap     = nommap,
	.d_kqfilter = nokqfilter,
	.d_discard  = nodiscard,
	.d_flag     = D_OTHER,
};

/*
 * THE EXECUTIVE ENTRY POINT IS NOT PRIVILEGE-GATED, exactly as on the Linux
 * substrate (src/kernel/vms_module.c). On OpenVMS the change-mode-to-kernel
 * that a $-service jackets is an unprivileged instruction available to every
 * process; access control lives INSIDE each service. So opening /dev/vms is
 * open to any process; a ping needs no privilege. The device node's mode is
 * chosen by the harness (mknod ... ; chmod 666) -- an OVMX design choice, not a
 * VMS-authentic value, since /dev/vms has no VMS counterpart (Rule 8).
 */
static int
vms_open(dev_t self __unused, int flag __unused, int mode __unused,
    struct lwp *l __unused)
{
	return 0;
}

static int
vms_close(dev_t self __unused, int flag __unused, int mode __unused,
    struct lwp *l __unused)
{
	return 0;
}

/*
 * The version/ping handshake. NetBSD's generic ioctl path has ALREADY copied
 * the caller's struct in from userspace (the command carries IOC_IN) and will
 * copy our answer out after we return (IOC_OUT), because VMS_IOCTL_PING is an
 * _IOWR. So we operate on the kernel-resident `data' buffer directly -- no
 * copyin/copyout here (this differs from Linux, where the module copies itself).
 */
static int
vms_ioctl(dev_t self __unused, u_long cmd, void *data, int flag __unused,
    struct lwp *l __unused)
{
	struct vms_ping_args *pa;

	switch (cmd) {
	case VMS_IOCTL_PING:
		pa = (struct vms_ping_args *)data;
		if (pa->magic != VMS_PING_REQ) {
			/* Delivered, but the request cookie was wrong: report the
			 * VMS error in-band (odd/even status), as a $-service does. */
			pa->magic       = 0;
			pa->abi_version = VMS_PING_ABI_VERSION;
			pa->substrate   = VMS_SUBSTRATE_NETBSD;
			pa->status      = VMS_SS_BADPARAM;
			return 0;
		}
		pa->magic       = VMS_PING_ACK;
		pa->abi_version = VMS_PING_ABI_VERSION;
		pa->substrate   = VMS_SUBSTRATE_NETBSD;
		pa->status      = VMS_SS_NORMAL;
		return 0;
	default:
		return ENOTTY;
	}
}

MODULE(MODULE_CLASS_DRIVER, vms, NULL);

static int
vms_modcmd(modcmd_t cmd, void *arg __unused)
{
	/* -1 (NODEVMAJOR) asks devsw_attach for a dynamically assigned major;
	 * bdev is NULL because this is a character-only pseudo-device. */
	devmajor_t bmajor = -1, cmajor = -1;
	int error;

	switch (cmd) {
	case MODULE_CMD_INIT:
		error = devsw_attach("vms", NULL, &bmajor, &vms_cdevsw, &cmajor);
		if (error != 0) {
			printf("vms: devsw_attach failed: %d\n", error);
			return error;
		}
		/* The harness reads this line back from dmesg to mknod /dev/vms
		 * with the dynamically assigned major. */
		printf("vms: registered, char major %d\n", cmajor);
		return 0;

	case MODULE_CMD_FINI:
		/* Called as a bare statement so this compiles whether the running
		 * NetBSD declares devsw_detach as returning void or int. In the
		 * harness the probe has already closed the device before unload,
		 * so the device is not busy. */
		devsw_detach(NULL, &vms_cdevsw);
		printf("vms: unregistered\n");
		return 0;

	default:
		return ENOTTY;
	}
}
