/*
 * vms_netbsd.c - the OVMX/NetBSD `vms' pseudo-device (rd vms-bfe P2b + vms-4b4
 * P2c, parent vms-dd8, epic vms-8e8; docs/design-ovmx-netbsd-syskrnl.md,
 * docs/design-netbsd-executive-core.md).
 *
 * The NetBSD-substrate sibling of the Linux executive (src/kernel/, vms.ko): a
 * real in-kernel cdevsw character device that creates /dev/vms and answers the
 * requests of the shared /dev/vms contract:
 *
 *   - P2b (vms-bfe): the version/ping handshake (vms_ping.h) -- proves a real
 *     in-kernel /dev/vms round-trips one ioctl through the transport seam.
 *   - P2c (vms-4b4): ONE real VMS EXECUTIVE FACILITY -- EVENT FLAGS -- run from
 *     the SAME source that the Linux vms.ko runs: src/kernel-core/vms_eflag.c.
 *     Nothing here re-implements event-flag semantics. This file is PURELY the
 *     NetBSD backend glue: it provides the exec_*/exec_list_* primitives (via
 *     exec_kbackend_netbsd.h / exec_list_netbsd.{h,c}), a per-pid process table,
 *     and the cdevsw d_ioctl dispatch that hands each request to the shared
 *     facility. The facility holds COMMON event flag clusters in module-global
 *     KERNEL memory, so a flag one process sets in a cluster it has $ASCEFC'd is
 *     visible to a DIFFERENT process that $ASCEFC's the same named cluster --
 *     the INV-6-decisive property (CLAUDE.md Rule 9). A per-process userspace
 *     fake could report ioctl success while sharing nothing; this cannot,
 *     because there is exactly one copy of the cluster and it lives in the
 *     kernel.
 *
 * THE COPY SEAM. The event-flag ioctls are encoded IOC_VOID (the _IO() form,
 * see vms_eflag_nb.h), so NetBSD's generic ioctl path does NOT pre-copy the
 * argument: it hands us the RAW USER POINTER. We pass that straight to the
 * shared facility, whose exec_copyin/exec_copyout (NetBSD copyin/copyout) do the
 * single real user boundary crossing -- exactly as the same facility does on
 * Linux. PING stays _IOWR (the P2b contract), so for PING the framework
 * pre-copies and `data' is a kernel buffer; the two encodings coexist in one
 * dispatch.
 *
 * PROCESS MODEL. The Linux executive keeps a global, module-lifetime process
 * table keyed by pid; so does this one (vms_proctab), because the shared
 * facility needs a `struct vms_proc' whose per-process ASCEFC associations
 * (proc->ef.common[]) point at the shared clusters. Procs are created on demand
 * from the calling lwp's pid and torn down at module unload; the SHARED cluster
 * state a PERMANENT cluster holds outlives any one proc, which is exactly what
 * makes a set by an already-exited process observable to a later one.
 *
 * It is a LOADABLE module (module(9)); the harness (tests/netbsd/) builds it
 * in-guest against the installed kernel sources, loads it, and drives the proof
 * through the transport seam. TOOLING, NOT A RUNTIME (Rule 9): booting a real
 * NetBSD to load and test a real kernel module is exactly what tests/qemu/ does
 * for the Linux vms.ko.
 *
 * CLEAN ROOM (CLAUDE.md Rule 8). Written from the public NetBSD cdevsw(9) /
 * module(9) / mutex(9) / condvar(9) / kmem(9) interfaces and the OVMX /dev/vms
 * contract. No NetBSD or VSI source is copied. The event-flag semantics live in
 * the shared, oracle-pinned facility (src/kernel-core/vms_eflag.c).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/ioccom.h>
#include <sys/errno.h>
#include <sys/proc.h>       /* struct proc, p_pid */
#include <sys/lwp.h>        /* struct lwp, l_proc */

#include "vms_ping.h"
/*
 * vms_internal.h is the NetBSD struct twin: it pulls exec_kbackend.h /
 * exec_list.h (selected to their NetBSD backends by OVMX_KBACKEND_NETBSD) and
 * vms_eflag_nb.h (the arg structs + the IOC_VOID request numbers), and declares
 * the shared facility's entry points. Including it here gives this glue the
 * exec_* primitives, `struct vms_proc', and the vms_ioctl_* prototypes.
 */
#include "vms_internal.h"

/* ================================================================
 * Per-pid process table (NetBSD glue; the Linux side is src/kernel/vms_proctab.c)
 * ================================================================ */

static exec_lock_t      vms_proctab_lock;
static struct vms_proc *vms_proctab;   /* singly-linked; module lifetime */

/*
 * vms_proc_get - find (or create) the vms_proc for `pid'. The shared facility
 * needs a stable per-process struct across a process's ioctls, so it is keyed by
 * pid and lives until module unload (the Linux executive reaps lazily; here the
 * test's procs are few and are freed en masse at FINI). Allocation happens
 * OUTSIDE the table lock -- exec_zalloc may sleep -- with a re-check after
 * re-acquiring the lock to resolve a race between two lwps of the same process.
 */
static struct vms_proc *
vms_proc_get(pid_t pid)
{
	struct vms_proc *p, *np;

	exec_lock(&vms_proctab_lock);
	for (p = vms_proctab; p != NULL; p = p->next) {
		if (p->pid == pid) {
			exec_unlock(&vms_proctab_lock);
			return p;
		}
	}
	exec_unlock(&vms_proctab_lock);

	np = exec_zalloc(sizeof(*np));
	if (np == NULL)
		return NULL;
	np->pid = pid;
	/* local[]=0 and common[]=NULL come from the zeroed allocation. Bring the
	 * per-process ef sync objects up before the proc is visible. */
	exec_lock_init(&np->ef.lock);
	exec_cv_init(&np->ef.waitq);

	exec_lock(&vms_proctab_lock);
	for (p = vms_proctab; p != NULL; p = p->next) {
		if (p->pid == pid) {
			/* Lost the race: use the existing proc, drop ours. */
			exec_unlock(&vms_proctab_lock);
			exec_cv_destroy(&np->ef.waitq);
			exec_lock_destroy(&np->ef.lock);
			exec_free(np);
			return p;
		}
	}
	np->next = vms_proctab;
	vms_proctab = np;
	exec_unlock(&vms_proctab_lock);
	return np;
}

/*
 * vms_proctab_teardown - free every proc at module unload. The COMMON clusters
 * are freed separately by vms_eflag_cleanup(); we do NOT call
 * vms_proc_release_common_ef() here (it would touch clusters this teardown's
 * sibling has already freed), we only tear down each proc's own ef sync objects
 * and free it. proc->ef.common[] pointers are never dereferenced again.
 */
static void
vms_proctab_teardown(void)
{
	struct vms_proc *p, *np;

	exec_lock(&vms_proctab_lock);
	p = vms_proctab;
	vms_proctab = NULL;
	exec_unlock(&vms_proctab_lock);

	while (p != NULL) {
		np = p->next;
		exec_cv_destroy(&p->ef.waitq);
		exec_lock_destroy(&p->ef.lock);
		exec_free(p);
		p = np;
	}
}

/* ================================================================
 * cdevsw
 * ================================================================ */

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
 * substrate (src/kernel/vms_module.c). On OpenVMS the change-mode-to-kernel that
 * a $-service jackets is an unprivileged instruction available to every process;
 * access control lives INSIDE each service. So opening /dev/vms is open to any
 * process. The device node's mode is chosen by the harness (mknod; chmod 666) --
 * an OVMX design choice, not a VMS-authentic value, since /dev/vms has no VMS
 * counterpart (Rule 8).
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
 * Map the shared facility's Linux-style return (0, -EFAULT, -ERESTARTSYS) to a
 * NetBSD errno. The VMS status the caller actually reads is written INTO the
 * user arg struct by the facility (copied out via exec_copyout); this return
 * only distinguishes clean delivery (0) from a copy fault or the
 * unreachable-by-design interrupted wait. -ERESTARTSYS -> ERESTART restarts the
 * ioctl syscall and re-enters the wait (the "re-enter" effect libvmssys gives
 * on Linux).
 */
static int
vms_facility_errno(long r)
{
	if (r == 0)
		return 0;
	if (r == -EFAULT)
		return EFAULT;
	if (r == -ERESTARTSYS)
		return ERESTART;
	return EINVAL;
}

static int
vms_ioctl(dev_t self __unused, u_long cmd, void *data, int flag __unused,
    struct lwp *l)
{
	struct vms_ping_args *pa;
	struct vms_proc *proc;
	void *uarg;
	long r;

	switch (cmd) {
	case VMS_IOCTL_PING:
		/*
		 * PING is _IOWR: NetBSD has ALREADY copied the caller's struct into
		 * the kernel-resident `data' and will copy our answer out after we
		 * return. So we operate on `data' directly -- no copyin/copyout here.
		 */
		pa = (struct vms_ping_args *)data;
		if (pa->magic != VMS_PING_REQ) {
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
	 * Event-flag facility. These are IOC_VOID: NetBSD did NOT pre-copy, and
	 * `data' points at a cell holding the RAW USER argument pointer. We hand
	 * that user pointer straight to the shared facility, which owns the
	 * copyin/copyout. Nothing here interprets the argument -- the facility
	 * (src/kernel-core/vms_eflag.c), identical to the Linux one, does.
	 */
	case VMS_IOCTL_SETEF:
	case VMS_IOCTL_CLREF:
	case VMS_IOCTL_WAITFR:
	case VMS_IOCTL_WFLOR:
	case VMS_IOCTL_WFLAND:
	case VMS_IOCTL_READEF:
	case VMS_IOCTL_ASCEFC:
	case VMS_IOCTL_DACEFC:
	case VMS_IOCTL_DLCEFC:
		uarg = *(void **)data;
		proc = vms_proc_get(l->l_proc->p_pid);
		if (proc == NULL)
			return ENOMEM;

		switch (cmd) {
		case VMS_IOCTL_SETEF:
			r = vms_ioctl_setef(proc, (unsigned long)uarg);  break;
		case VMS_IOCTL_CLREF:
			r = vms_ioctl_clref(proc, (unsigned long)uarg);  break;
		case VMS_IOCTL_WAITFR:
			r = vms_ioctl_waitfr(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_WFLOR:
			r = vms_ioctl_wflor(proc, (unsigned long)uarg);  break;
		case VMS_IOCTL_WFLAND:
			r = vms_ioctl_wfland(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_READEF:
			r = vms_ioctl_readef(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_ASCEFC:
			r = vms_ioctl_ascefc(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_DACEFC:
			r = vms_ioctl_dacefc(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_DLCEFC:
			r = vms_ioctl_dlcefc(proc, (unsigned long)uarg); break;
		default:
			return ENOTTY;   /* unreachable */
		}
		return vms_facility_errno(r);

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
		/*
		 * Bring the executive's locks up BEFORE the device can be opened, so
		 * no ioctl can race an uninitialised lock. vms_eflag_init() inits the
		 * facility's common-cluster list guard (the shared source's own
		 * global); the proc table has its own guard.
		 */
		exec_lock_init(&vms_proctab_lock);
		vms_proctab = NULL;
		vms_eflag_init();

		error = devsw_attach("vms", NULL, &bmajor, &vms_cdevsw, &cmajor);
		if (error != 0) {
			printf("vms: devsw_attach failed: %d\n", error);
			vms_eflag_cleanup();
			vms_proctab_teardown();
			exec_lock_destroy(&vms_proctab_lock);
			return error;
		}
		/* The harness reads this line back from dmesg to mknod /dev/vms with
		 * the dynamically assigned major. */
		printf("vms: registered, char major %d\n", cmajor);
		return 0;

	case MODULE_CMD_FINI:
		/*
		 * Called as a bare statement so this compiles whether the running
		 * NetBSD declares devsw_detach as returning void or int. In the
		 * harness every process has closed the device before unload, so it is
		 * not busy and no facility wait is in flight.
		 */
		devsw_detach(NULL, &vms_cdevsw);
		/* Free the shared clusters first (they destroy their own sync
		 * objects), then the procs, then the proc-table guard. */
		vms_eflag_cleanup();
		vms_proctab_teardown();
		exec_lock_destroy(&vms_proctab_lock);
		printf("vms: unregistered\n");
		return 0;

	default:
		return ENOTTY;
	}
}
