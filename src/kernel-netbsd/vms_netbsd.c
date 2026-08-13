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
 *     NetBSD backend glue: it provides the exec_* and exec_list_* primitives (via
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
 * THE COPY SEAM. The event-flag ioctls are _IOWR (like PING), so NetBSD's
 * generic cdevsw path copies the caller's argument into a kernel buffer BEFORE
 * calling us and copies our answer back out AFTER we return. We hand that kernel
 * buffer straight to the shared facility; its exec_copyin/exec_copyout are, on
 * the NetBSD backend, in-kernel copies between that buffer and the facility's
 * locals (the one real user boundary crossing is the framework's). This is the
 * honest, idiomatic NetBSD integration and leaves the shared facility source
 * unchanged -- see exec_kbackend_netbsd.h's COPY MODEL note.
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
 * vms_eflag_nb.h (the arg structs + the _IOWR request numbers), and declares
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

	/*
	 * Access-mode + AST state (P4-A). The zeroed allocation gives image_active=0
	 * and count=0; the rest must be brought up explicitly before the proc is
	 * visible. This mirrors src/kernel/vms_module.c's proc registration:
	 *   - current_mode starts at PSL_C_USER (a fresh process runs in user mode;
	 *     0 == PSL_C_KERNEL would be wrong, and zalloc gives 0).
	 *   - the authorized/current privilege masks are DERIVED, not requested:
	 *     exec_current_is_privileged() is the NetBSD kauth(9) twin of Linux
	 *     capable(CAP_SYS_ADMIN) (a real credential read, not an asserted word),
	 *     and a privileged caller is seeded exactly the privileges vms_access.c/
	 *     vms_ast.c ENFORCE (CMKRNL/CMEXEC/SETPRV) so the access-mode proofs can
	 *     exercise both the allow and deny paths; an unprivileged caller gets
	 *     none. Broader SYSUAF privileges arrive later via $SETIDENT (proctab,
	 *     P4-B), never conjured here. This seed is an OVMX glue choice (Rule 8),
	 *     not a VMS-authentic value -- /dev/vms has no VMS counterpart.
	 *   - each per-mode AST queue is enabled by default (Linux vms_module.c) with
	 *     an empty (self-linked) pending ring and its own guard.
	 *   - the hibernate cv + its paired lock back async AST delivery (vms-feb).
	 */
	np->current_mode = PSL_C_USER;
	np->perm_privs = exec_current_is_privileged()
	               ? (VMS_PRV_M_CMKRNL | VMS_PRV_M_CMEXEC | VMS_PRV_M_SETPRV)
	               : 0;
	np->cur_privs = np->perm_privs;
	exec_lock_init(&np->mode_lock);
	{
		int m;
		for (m = 0; m < 4; m++) {
			exec_list_head_init(&np->ast[m].pending);
			np->ast[m].count = 0;
			np->ast[m].enabled = 1;
			exec_lock_init(&np->ast[m].lock);
		}
	}
	exec_cv_init(&np->hiber_wq);
	exec_lock_init(&np->hiber_lock);

	/*
	 * Mailbox per-process state (P4-A, rd vms-d7a): the channel-number allocator
	 * and this process's (initially empty, self-linked) mailbox channel ring plus
	 * its guard. The identity fields stamp a created mailbox's owner (informational
	 * only). No separate VMS PID is assigned on this substrate, so vms_pid tracks
	 * the pid, exactly as the glue key does.
	 */
	np->linux_pid = pid;
	np->vms_pid   = (uint32_t)pid;
	np->next_chan = 0;
	exec_lock_init(&np->chan_lock);
	exec_list_head_init(&np->mbx_channels);

	exec_lock(&vms_proctab_lock);
	for (p = vms_proctab; p != NULL; p = p->next) {
		if (p->pid == pid) {
			/* Lost the race: use the existing proc, drop ours. Its AST
			 * pending rings and mailbox channel ring are still empty (just
			 * created), so only the sync objects need tearing down before the
			 * free -- a live kmutex/kcondvar must be destroyed on NetBSD. */
			int m;
			exec_unlock(&vms_proctab_lock);
			exec_lock_destroy(&np->chan_lock);
			exec_cv_destroy(&np->hiber_wq);
			exec_lock_destroy(&np->hiber_lock);
			for (m = 0; m < 4; m++)
				exec_lock_destroy(&np->ast[m].lock);
			exec_lock_destroy(&np->mode_lock);
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
		int m;
		np = p->next;
		/* Give back this process's mailbox channels first (dropping each
		 * mailbox's reference, freeing temporary/deleted mailboxes whose last
		 * channel this was) -- runs while the mailbox list guard is still alive
		 * (vms_mbx_cleanup() runs AFTER this, in vms_modcmd's FINI). Then free
		 * any AST entries still queued at unload (all four modes: min
		 * PSL_C_KERNEL == 0), then tear down each mode's guard and the mailbox/
		 * access/hibernate sync objects. In the harness every process has closed
		 * the device and no wait is in flight, so this is unraced. */
		vms_mbx_release_all(p);
		vms_proc_rundown_asts(p, PSL_C_KERNEL);
		exec_lock_destroy(&p->chan_lock);
		exec_cv_destroy(&p->hiber_wq);
		exec_lock_destroy(&p->hiber_lock);
		for (m = 0; m < 4; m++)
			exec_lock_destroy(&p->ast[m].lock);
		exec_lock_destroy(&p->mode_lock);
		exec_cv_destroy(&p->ef.waitq);
		exec_lock_destroy(&p->ef.lock);
		exec_free(p);
		p = np;
	}
}

/* ================================================================
 * Cross-facility image-rundown stubs (P4-A).
 *
 * vms_ioctl_image_rundown() (src/kernel-core/vms_access.c) releases the image's
 * locks, channels and ASTs at rundown by calling three per-facility helpers.
 * On this NetBSD build only the AST facility is present, so vms_proc_rundown_asts
 * is DEFINED (vms_ast.c) and linked; vms_proc_rundown_locks (vms_lock.c, rd
 * vms-ff7) and vms_proc_rundown_channels (vms_mbx.c, rd vms-d7a) are not yet in
 * this module's SRCS. These WEAK no-op definitions keep the module link-coherent
 * and loadable until then, and are OVERRIDDEN by the real (strong) facility
 * definitions the moment vms_lock.c / vms_mbx.c join the build -- no edit here
 * needed when they land. This fabricates nothing (INV-6 / Rule 11): a substrate
 * with no lock or channel facility genuinely has no such resources to run down,
 * so running down nothing is the honest result, not a faked success.
 * ================================================================ */
__attribute__((weak)) void
vms_proc_rundown_locks(struct vms_proc *proc __unused, uint8_t min_acmode __unused)
{
}

__attribute__((weak)) void
vms_proc_rundown_channels(struct vms_proc *proc __unused, uint8_t min_acmode __unused)
{
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
	/* DELIVERAST returns -EAGAIN when no AST is deliverable at the caller's
	 * mode; userspace's deliver loop keys on EAGAIN to stop draining, so it
	 * must survive the mapping rather than collapse to EINVAL. */
	if (r == -EAGAIN)
		return EAGAIN;
	return EINVAL;
}

/*
 * vms_mbx_bigio - dispatch a MESSAGE-TRANSFER mailbox ioctl (WRITE/READ) whose
 * argument exceeds NetBSD's one-page IOCPARM_MAX (P4-A, rd vms-d7a; Option B).
 *
 * These two ops are encoded IOC_VOID (vms_mbx_nb.h), so the cdevsw framework did
 * NOT pre-copy the argument -- it stored the raw USER pointer in `data'
 * (sys_generic.c: `*(void **)data = SCARG(uap, data)'). We do the boundary
 * crossing ourselves: copyin the caller's argument into a kernel buffer, hand
 * that buffer to the SHARED facility (whose exec_copyin/exec_copyout are then
 * in-kernel copies against it, exactly as on the _IOWR control path), and copyout
 * the answer only on success. On -ERESTARTSYS (an interrupted mailbox read, the
 * WAITFR precedent) the facility wrote no status, so we skip the copyout and let
 * ERESTART re-enter. A bad user address is rejected here by copyin/copyout,
 * never fabricated (INV-6). `fn' is the facility handler; `argsz' its struct size.
 */
static int
vms_mbx_bigio(struct lwp *l, void *data, size_t argsz,
    long (*fn)(struct vms_proc *, unsigned long))
{
	void *uaddr = *(void **)data;   /* IOC_VOID: the caller's struct address */
	struct vms_proc *proc;
	void *kbuf;
	long r;

	kbuf = exec_alloc(argsz);
	if (kbuf == NULL)
		return ENOMEM;
	if (copyin(uaddr, kbuf, argsz)) {
		exec_free(kbuf);
		return EFAULT;
	}

	proc = vms_proc_get(l->l_proc->p_pid);
	if (proc == NULL) {
		exec_free(kbuf);
		return ENOMEM;
	}

	r = fn(proc, (unsigned long)kbuf);
	if (r == 0 && copyout(kbuf, uaddr, argsz))
		r = -EFAULT;

	exec_free(kbuf);
	return vms_facility_errno(r);
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
	 * Event-flag facility. These are _IOWR: NetBSD has already copied the
	 * caller's argument into the kernel buffer `data' and will copy our answer
	 * back out after we return. We hand `data' straight to the shared facility,
	 * whose exec_copyin/exec_copyout are in-kernel copies on this backend.
	 * Nothing here interprets the argument -- the facility
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
		uarg = data;
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

	/*
	 * AST facility (src/kernel-core/vms_ast.c) and access-mode facility
	 * (src/kernel-core/vms_access.c) -- P4-A. Same dispatch shape as event
	 * flags: find-or-create the caller's proc, hand the framework's kernel
	 * buffer `data' straight to the shared facility, map its Linux-style
	 * return to a NetBSD errno. Nothing here interprets the argument.
	 *
	 * DIRECTION / COPY (see vms_ast_nb.h): the request numbers carry the SAME
	 * direction class as vms_ioctl.h. For _IOWR ($SETAST/$SETPRV/$CHKPRIV/
	 * ENTER_IMAGE/IMAGE_RUNDOWN) the framework copies `data' IN before and OUT
	 * after -- full round-trip. For _IOR ($GETMODE/DELIVERAST) it zeroes `data',
	 * the facility fills it (neither reads input), and the framework copies OUT.
	 * For _IOW ($SETMODE/$DCLAST) the framework copies the request IN but does
	 * NOT copy OUT, so the facility's args.status is applied but not returned to
	 * userspace on NetBSD (the driver never sees the user address). That is a
	 * property of the shared _IOW request number, not a fabricated result: the
	 * command's EFFECT is observable through the paired _IOR/_IOWR command
	 * ($GETMODE after $SETMODE, DELIVERAST after $DCLAST). No silent success is
	 * faked (INV-6). DELIVERAST's "no AST pending" is -EAGAIN -> EAGAIN.
	 */
	case VMS_IOCTL_DCLAST:
	case VMS_IOCTL_SETAST:
	case VMS_IOCTL_DELIVERAST:
	case VMS_IOCTL_SETMODE:
	case VMS_IOCTL_GETMODE:
	case VMS_IOCTL_SETPRV:
	case VMS_IOCTL_CHKPRIV:
	case VMS_IOCTL_ENTER_IMAGE:
	case VMS_IOCTL_IMAGE_RUNDOWN:
		uarg = data;
		proc = vms_proc_get(l->l_proc->p_pid);
		if (proc == NULL)
			return ENOMEM;

		switch (cmd) {
		case VMS_IOCTL_DCLAST:
			r = vms_ioctl_dclast(proc, (unsigned long)uarg);        break;
		case VMS_IOCTL_SETAST:
			r = vms_ioctl_setast(proc, (unsigned long)uarg);        break;
		case VMS_IOCTL_DELIVERAST:
			r = vms_ioctl_deliverast(proc, (unsigned long)uarg);    break;
		case VMS_IOCTL_SETMODE:
			r = vms_ioctl_setmode(proc, (unsigned long)uarg);       break;
		case VMS_IOCTL_GETMODE:
			r = vms_ioctl_getmode(proc, (unsigned long)uarg);       break;
		case VMS_IOCTL_SETPRV:
			r = vms_ioctl_setprv(proc, (unsigned long)uarg);        break;
		case VMS_IOCTL_CHKPRIV:
			r = vms_ioctl_chkpriv(proc, (unsigned long)uarg);       break;
		case VMS_IOCTL_ENTER_IMAGE:
			r = vms_ioctl_enter_image(proc, (unsigned long)uarg);   break;
		case VMS_IOCTL_IMAGE_RUNDOWN:
			r = vms_ioctl_image_rundown(proc, (unsigned long)uarg); break;
		default:
			return ENOTTY;   /* unreachable */
		}
		return vms_facility_errno(r);

	/*
	 * Mailbox CONTROL ioctls (MBAn:, P4-A). All _IOWR and <= 40 bytes, so the
	 * same framework pre-copy model as event flags: NetBSD copied the argument
	 * into `data' and copies our answer back out. We hand `data' to the shared
	 * facility (src/kernel-core/vms_mbx.c), identical to the Linux one.
	 */
	case VMS_IOCTL_MBX_CREATE:
	case VMS_IOCTL_MBX_ASSIGN:
	case VMS_IOCTL_MBX_DELMBX:
	case VMS_IOCTL_MBX_SET_WRTATTN:
		uarg = data;
		proc = vms_proc_get(l->l_proc->p_pid);
		if (proc == NULL)
			return ENOMEM;

		switch (cmd) {
		case VMS_IOCTL_MBX_CREATE:
			r = vms_ioctl_mbx_create(proc, (unsigned long)uarg);      break;
		case VMS_IOCTL_MBX_ASSIGN:
			r = vms_ioctl_mbx_assign(proc, (unsigned long)uarg);      break;
		case VMS_IOCTL_MBX_DELMBX:
			r = vms_ioctl_mbx_delmbx(proc, (unsigned long)uarg);      break;
		case VMS_IOCTL_MBX_SET_WRTATTN:
			r = vms_ioctl_mbx_set_wrtattn(proc, (unsigned long)uarg); break;
		default:
			return ENOTTY;   /* unreachable */
		}
		return vms_facility_errno(r);

	/*
	 * Mailbox MESSAGE-TRANSFER ioctls. IOC_VOID (their >page argument can't ride
	 * the framework pre-copy path, Option B) -- the driver does the copyin/copyout
	 * itself. See vms_mbx_bigio() and vms_mbx_nb.h's COPY MODEL note.
	 */
	case VMS_IOCTL_MBX_WRITE:
		return vms_mbx_bigio(l, data, sizeof(struct vms_mbx_write_args),
		    vms_ioctl_mbx_write);
	case VMS_IOCTL_MBX_READ:
		return vms_mbx_bigio(l, data, sizeof(struct vms_mbx_read_args),
		    vms_ioctl_mbx_read);

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
		/* Bring up the mailbox table's list guard too (the table starts empty
		 * -- mailboxes are created on demand by $CREMBX). */
		vms_mbx_init();

		error = devsw_attach("vms", NULL, &bmajor, &vms_cdevsw, &cmajor);
		if (error != 0) {
			printf("vms: devsw_attach failed: %d\n", error);
			vms_eflag_cleanup();
			vms_proctab_teardown();
			vms_mbx_cleanup();
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
		/* Free the shared common-EF clusters, then tear down the procs (this
		 * gives back each proc's mailbox channels and drains its ASTs while the
		 * mailbox list guard is still alive), then free any remaining permanent
		 * mailboxes and destroy the mailbox list guard, then the proc-table
		 * guard. */
		vms_eflag_cleanup();
		vms_proctab_teardown();
		vms_mbx_cleanup();
		exec_lock_destroy(&vms_proctab_lock);
		printf("vms: unregistered\n");
		return 0;

	default:
		return ENOTTY;
	}
}
