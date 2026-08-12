/*
 * vms_netbsd.c - the OVMX/NetBSD `vms' pseudo-device (rd vms-bfe P2b + vms-4b4
 * P2c, parent vms-dd8, epic vms-8e8; docs/design-ovmx-netbsd-syskrnl.md).
 *
 * The NetBSD-substrate sibling of the Linux executive (src/kernel/, vms.ko). A
 * real in-kernel cdevsw character device that creates /dev/vms and answers the
 * requests of the shared /dev/vms contract:
 *
 *   - P2b (vms-bfe): the version/ping handshake (vms_ping.h) -- proves a real
 *     in-kernel /dev/vms round-trips one ioctl through the transport seam.
 *   - P2c (vms-4b4): ONE real VMS EXECUTIVE FACILITY -- the COMMON EVENT FLAG
 *     CLUSTERS (vms_eflag_nb.h). This is the phase that proves the executive is
 *     REAL: the common-cluster flag state lives in this module's KERNEL memory,
 *     guarded by a kmutex, so it is SYSTEM-WIDE SHARED across processes. Process
 *     A's $SETEF is visible to a DIFFERENT process B's $READEF because the flag
 *     lives in the executive, not in either process -- the INV-6-decisive
 *     property (CLAUDE.md Rule 9). A per-process userspace fake could report
 *     ioctl success while sharing nothing; this cannot, because there is exactly
 *     one copy of the state and it is in the kernel.
 *
 * SCOPE OF P2c. Only the COMMON (shared) clusters -- EFN 64..127 -- are built,
 * because they are the ones that MUST live in the executive and whose
 * cross-process visibility is the whole point. The LOCAL per-process clusters
 * (EFN 0..63) are a different facility not built this phase; an EFN outside the
 * common range is rejected with an honest SS$_ILLEFC, never a faked per-process
 * success (INV-6). Locks are a later phase and are NOT built here.
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
 * module(9) / kmutex(9) interfaces (sys/conf.h, sys/module.h, sys/mutex.h) and
 * the OVMX /dev/vms contract. No NetBSD kernel source is copied beyond the
 * public driver-API idioms every out-of-tree driver uses. The event-flag
 * SEMANTICS match the publicly documented OpenVMS $SETEF/$CLREF/$READEF and the
 * in-tree Linux executive (src/kernel/vms_eflag.c) as the semantic reference.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/ioccom.h>
#include <sys/errno.h>
#include <sys/mutex.h>

#include "vms_ping.h"
#include "vms_eflag_nb.h"

/*
 * THE EXECUTIVE'S COMMON EVENT FLAG STATE -- module-global KERNEL memory.
 *
 * Two 32-bit words = the two common clusters: vms_cef[0] is cluster 2 (EFN
 * 64..95), vms_cef[1] is cluster 3 (EFN 96..127). There is exactly ONE copy,
 * in the kernel, shared by every process that opens /dev/vms -- that is what
 * makes a set by process A observable to a read by process B (INV-6). The
 * kmutex serialises the read-modify-write of $SETEF/$CLREF against a concurrent
 * $READEF or another writer; static-initialised to all-clear at load, exactly
 * as a freshly created common cluster is.
 */
static uint32_t  vms_cef[2];
static kmutex_t  vms_cef_lock;

/*
 * cef_resolve - map a VMS common EFN to its 32-bit word and bit, or reject.
 * Returns 0 and fills *word/*bit for an EFN in the common range; -1 otherwise
 * (the caller answers SS$_ILLEFC -- an honest rejection, not a fake).
 */
static int
cef_resolve(uint32_t efn, uint32_t **word, int *bit)
{
	uint32_t rel;

	if (efn < VMS_EF_COMMON_LO || efn > VMS_EF_COMMON_HI)
		return -1;
	rel = efn - VMS_EF_COMMON_LO;   /* 0..63 */
	*word = &vms_cef[rel >> 5];     /* which common cluster (/32) */
	*bit  = (int)(rel & 31u);       /* bit within the cluster    */
	return 0;
}

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
	struct vms_eflag_args *ea;
	uint32_t *word, prev;
	int bit;

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

	/*
	 * $SETEF -- set a common event flag. Reports the flag's PREVIOUS state
	 * (SS$_WASSET / SS$_WASCLR), exactly as OpenVMS $SETEF does. The
	 * read-modify-write is serialised by vms_cef_lock. This mutation is
	 * SYSTEM-WIDE: it lands in module-global kernel memory, so it is visible
	 * to every other process's later $READEF (INV-6 / Rule 9).
	 */
	case VMS_IOCTL_SETEF:
		ea = (struct vms_eflag_args *)data;
		ea->state = 0;
		if (cef_resolve(ea->efn, &word, &bit) < 0) {
			ea->status = VMS_SS_ILLEFC;
			return 0;
		}
		mutex_enter(&vms_cef_lock);
		prev   = *word & (1U << bit);
		*word |= (1U << bit);
		mutex_exit(&vms_cef_lock);
		ea->status = prev ? VMS_SS_WASSET : VMS_SS_WASCLR;
		return 0;

	/*
	 * $CLREF -- clear a common event flag. Also reports the PREVIOUS state,
	 * so a caller can tell whether it was the one that cleared a set flag.
	 */
	case VMS_IOCTL_CLREF:
		ea = (struct vms_eflag_args *)data;
		ea->state = 0;
		if (cef_resolve(ea->efn, &word, &bit) < 0) {
			ea->status = VMS_SS_ILLEFC;
			return 0;
		}
		mutex_enter(&vms_cef_lock);
		prev    = *word & (1U << bit);
		*word  &= ~(1U << bit);
		mutex_exit(&vms_cef_lock);
		ea->status = prev ? VMS_SS_WASSET : VMS_SS_WASCLR;
		return 0;

	/*
	 * $READEF -- read the naming cluster's 32-bit state WITHOUT waiting. The
	 * caller tests its own bit in `state'. This is the read side of the
	 * cross-process proof: a different process reads the same kernel word A
	 * wrote. SS$_NORMAL on any legal common EFN.
	 */
	case VMS_IOCTL_READEF:
		ea = (struct vms_eflag_args *)data;
		if (cef_resolve(ea->efn, &word, &bit) < 0) {
			ea->state  = 0;
			ea->status = VMS_SS_ILLEFC;
			return 0;
		}
		mutex_enter(&vms_cef_lock);
		ea->state = *word;
		mutex_exit(&vms_cef_lock);
		ea->status = VMS_SS_NORMAL;
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
		/* Bring the common-event-flag lock up BEFORE the device can be
		 * opened, so no $SETEF/$READEF can race an uninitialised mutex. The
		 * flag words are static-initialised to all-clear (a fresh cluster).
		 * IPL_NONE: these ioctls run only in process context. */
		mutex_init(&vms_cef_lock, MUTEX_DEFAULT, IPL_NONE);

		error = devsw_attach("vms", NULL, &bmajor, &vms_cdevsw, &cmajor);
		if (error != 0) {
			printf("vms: devsw_attach failed: %d\n", error);
			mutex_destroy(&vms_cef_lock);
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
		mutex_destroy(&vms_cef_lock);
		printf("vms: unregistered\n");
		return 0;

	default:
		return ENOTTY;
	}
}
