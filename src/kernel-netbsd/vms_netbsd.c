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
 * The executive process table -- ONE table, shared by every facility (rd
 * vms-ca7). The Linux vms.ko defines vms_proc_hash + vms_proc_hash_lock in its
 * module-lifecycle glue (src/kernel/vms_module.c); on NetBSD the module
 * lifecycle IS this file, so the table lives here. Both are extern'd by the
 * struct twin (vms_internal.h) so the shared facility src/kernel-core/
 * vms_proctab.c walks the SAME table this glue populates -- $GETJPI must be able
 * to find the very proc that $ASCEFC'd a cluster or queued an AST, so there is
 * exactly one per-pid struct.
 *
 * These are NON-static (linked to by vms_proctab.c). All walks -- here and in
 * the facility -- run under vms_proc_hash_lock; there are no lockless readers on
 * this substrate, which is what makes the RCU-lite blessed fallback (immediate
 * exec_free_deferred) correct (exec_kbackend_netbsd.h §6).
 * ================================================================ */

EXEC_DEFINE_HASHTABLE(vms_proc_hash, VMS_PROC_HASH_BITS);
exec_lock_t vms_proc_hash_lock;

/*
 * This node's cluster system ID (P4-A locks, rd vms-ff7). DEFINED here in the
 * module glue, exactly as the Linux vms.ko defines it in vms_module.c -- the
 * CSID is a per-substrate module-lifecycle value, not portable executive logic
 * (design record §4). The shared lock manager (src/kernel-core/vms_lock.c) reads
 * it through the extern in vms_internal.h for its DLM directory/mastering
 * resolution. 0 is reserved for "unmastered", so the default is a non-zero OVMX
 * local placeholder; the connection manager assigns the real CSID at cluster
 * join (0.4). This is NOT a VMS-authentic CSID value or layout (CLAUDE.md
 * Rule 8).
 */
uint32_t vms_local_csid = 1;

/*
 * dlm_member_csids[] / dlm_member_count - the DLM directory membership vector
 * (rd vms-1bba, "DB" rung), read by the shared lock manager through the extern
 * in vms_internal.h. On Linux these are a module_param_array (harness-supplied);
 * this NetBSD substrate defines them with a cluster-of-one default (count 0 ->
 * the directory helpers fall back to vms_local_csid). A static controlled input,
 * NOT the live 0.4/DC membership feed.
 */
uint32_t dlm_member_csids[VMS_DLM_MAX_MEMBERS];
int      dlm_member_count;

/*
 * vms_proc_get - find (or create) the vms_proc for `pid'. The shared facilities
 * need a stable per-process struct across a process's ioctls, keyed by pid; it
 * lives until the facility reaper reclaims it (its backing task has exited) or
 * module unload. Allocation happens OUTSIDE the table lock -- exec_zalloc may
 * sleep -- with a re-check after re-acquiring the lock to resolve a race between
 * two lwps of the same process.
 */
static struct vms_proc *
vms_proc_get(pid_t pid)
{
	struct vms_proc *p, *np;
	int bkt;

	exec_lock(&vms_proc_hash_lock);
	exec_hash_for_each(vms_proc_hash, bkt, p, hash_node) {
		if (p->pid == pid) {
			exec_unlock(&vms_proc_hash_lock);
			return p;
		}
	}
	exec_unlock(&vms_proc_hash_lock);

	np = exec_zalloc(sizeof(*np));
	if (np == NULL)
		return NULL;
	np->pid = pid;

	/*
	 * Executive-resident identity (P4-A proctab, rd vms-ca7). prcnam/username/
	 * terminal come "" from the zeroed allocation ($SETPRN / $SETIDENT stamp
	 * them; terminal stays "" -- no device table in SRCS). uic is DERIVED from
	 * the calling task's REAL credentials, never a value a process supplies:
	 * exec_current_gid/uid are the kauth(9) twins of Linux from_kgid/kuid, and
	 * [group,member] packs (gid<<16)|uid exactly as sys$getjpi's JPI$_UIC does
	 * (mirrors vms_proc_register). pid_ref is the facility's opaque liveness
	 * handle: it carries just the pid on this substrate, so the storage is
	 * embedded in the PCB and pid_ref points at it (proc_find(9) takes no ref to
	 * drop later). The p0/p1 extents and wake_pending come zeroed.
	 */
	np->uic = (((uint32_t)exec_current_gid() & 0xFFFFu) << 16) |
	          ((uint32_t)exec_current_uid() & 0xFFFFu);
	np->pid_ref_store.pid = pid;
	np->pid_ref = &np->pid_ref_store;

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
	 *     capable(CAP_SYS_ADMIN) (a real credential read, not an asserted word).
	 *     EVERY process -- privileged or not -- gets VMS_DEFAULT_PRIVS
	 *     (TMPMBX|NETMBX), the privileges every VMS process holds by default;
	 *     this matches src/kernel/vms_module.c's seed EXACTLY (privileged:
	 *     ENFORCED|DEFAULT, unprivileged: DEFAULT) and is what lets a
	 *     default-privilege $CREMBX of a temporary mailbox succeed (rd vms-f8a:
	 *     omitting DEFAULT here made the P4-A mbx proof's $CREMBX return
	 *     SS$_NOPRIV once proctab stopped masking it). A privileged caller ALSO
	 *     gets VMS_PRV_M_ENFORCED -- the access-mode privileges vms_access.c/
	 *     vms_ast.c enforce (CMKRNL/CMEXEC/SETPRV) AND SYSNAM/GRPNAM/WORLD/MOUNT --
	 *     so both the access-mode allow/deny paths and LNM$SYSTEM DEFINE at boot
	 *     work (rd vms-72da). Broader SYSUAF privileges arrive later via $SETIDENT
	 *     (proctab, P4-B), never conjured here. This seed is an OVMX glue choice
	 *     (Rule 8), not a VMS-authentic value.
	 *   - each per-mode AST queue is enabled by default (Linux vms_module.c) with
	 *     an empty (self-linked) pending ring and its own guard.
	 *   - the hibernate cv + its paired lock back async AST delivery (vms-feb).
	 */
	np->current_mode = PSL_C_USER;
	/*
	 * A privileged (root/kauth) caller gets VMS_PRV_M_ENFORCED | VMS_DEFAULT_PRIVS,
	 * BYTE-IDENTICAL to src/kernel/vms_module.c (capable(CAP_SYS_ADMIN) path). This
	 * previously hand-listed a SUBSET (CMKRNL|CMEXEC|SETPRV) that omitted SYSNAM,
	 * so PID 1's lnm_setup_defaults could not DEFINE the system logicals
	 * (SYS$SYSTEM et al.) -- the executive gates LNM$SYSTEM on SYSNAM|SYSPRV -- and
	 * the netbsd-vax boot halted at %OVMX-F-EXECINIT with SYS$SYSTEM unresolved
	 * (rd vms-72da). ENFORCED carries SYSNAM (+ WORLD/GRPNAM/MOUNT), restoring
	 * exact Linux parity. Broader SYSUAF privileges still arrive later via
	 * $SETIDENT (proctab, P4-B), never conjured here.
	 */
	np->perm_privs = exec_current_is_privileged()
	               ? (VMS_PRV_M_ENFORCED | VMS_DEFAULT_PRIVS)
	               : VMS_DEFAULT_PRIVS;
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

	/*
	 * Files-11 (ODS-2) ACP file-class channels (rd vms-6a7f, epic vms-208) --
	 * this process's (initially empty) file-channel ring, same chan_lock/
	 * next_chan space as mbx_channels above. Only the list head is
	 * initialized here; vmsfs_acp.c is now a TU of this module (vms-d5d), so its
	 * release-all teardown IS wired below: vms_proc_free_claimed() calls
	 * vms_acp_release_all(proc) to $DASSGN every file channel at process death,
	 * beside the mailbox release.
	 */
	exec_list_head_init(&np->file_channels);

	/*
	 * DEVICE channels (rd vms-618) -- this process's (initially empty) ring of
	 * channels to executive device-table rows, on the SAME chan_lock/next_chan
	 * space as the mailbox and file channels above. vms_proc_release_channels()
	 * (src/kernel-core/vms_devtab.c) drains it at process death, and gives back
	 * any DEVICE this process had $ALLOCated.
	 */
	exec_list_head_init(&np->channels);

	/*
	 * Lock-manager per-process state (P4-A, rd vms-ff7): this process's (initially
	 * empty, self-linked) list of held locks, its count and their guard. Mirrors
	 * the Linux vms.ko's proc registration (vms_module.c: INIT_LIST_HEAD(&locks)
	 * + lock_count=0 + spin_lock_init(&lock_list_lock)). vms_proc_release_locks()
	 * drains this list at process death.
	 */
	exec_list_head_init(&np->locks);
	np->lock_count = 0;
	exec_lock_init(&np->lock_list_lock);

	exec_lock(&vms_proc_hash_lock);
	exec_hash_for_each(vms_proc_hash, bkt, p, hash_node) {
		if (p->pid == pid) {
			/* Lost the race: use the existing proc, drop ours. Its AST
			 * pending rings, mailbox channel ring and lock list are still
			 * empty (just created), so only the sync objects need tearing
			 * down before the free -- a live kmutex/kcondvar must be
			 * destroyed on NetBSD. */
			int m;
			exec_unlock(&vms_proc_hash_lock);
			exec_lock_destroy(&np->lock_list_lock);
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
	exec_hash_add(vms_proc_hash, &np->hash_node, (uint32_t)pid);
	exec_unlock(&vms_proc_hash_lock);
	return np;
}

/*
 * vms_proc_continue_identity - stamp the calling task's REAL PARENT's executive
 * identity onto `proc' (a child PCB vms_proc_get() has just created and seeded
 * fresh) for VMS_IOCTL_REGISTER_CONTINUE. This is the NetBSD twin of
 * src/kernel/vms_module.c:vms_proc_continue_identity().
 *
 * WHY THIS EXISTS. On OpenVMS, RUN / a foreign command / a DCL utility
 * activates an image IN the current process -- same PID, UIC, privileges. OVMX
 * fork()s + execv()s instead (src/vmsdcl/dcl_cmd_process.c), and the child
 * calls vms_kif_register_continue() BEFORE execv while it is still DCL's child.
 * Without this the child's PCB carried the FRESH kauth-seeded mask
 * vms_proc_get() derives (VMS_DEFAULT_PRIVS, plus VMS_PRV_M_ENFORCED for a
 * root/kauth caller) -- which NEVER includes SYSPRV/BYPASS, since those arrive
 * only via $SETIDENT (VMS_IOCTL_SETIDENT) after login. So SYSTEM's DCL, holding
 * SYSPRV/BYPASS, activated AUTHORIZE/LOGINOUT into a child that did NOT, and the
 * activated image could not open the World-denied SYSUAF.DAT: %UAF-E-NAOFIL,
 * -RMS-E-PRV. The combined REGISTER/_CONTINUE case here fresh-seeded on BOTH
 * paths -- it never once looked at the parent -- which is the fabrication this
 * removes (INV-6): _CONTINUE claimed to continue the parent while handing back
 * a fresh, differently-privileged identity.
 *
 * WHAT IT COPIES. The parent's identity fields (uic/username/prcnam/terminal),
 * CLI invocation context (cli_present/length/command, so the image reads its
 * OWN invoking DCL command line back), and BOTH privilege masks -- the parent's
 * CURRENT (possibly $SETIDENT-reduced) cur_privs, so a non-privileged parent's
 * child stays non-privileged and a setident-down cannot be undone by a fork.
 * The child then SHARES the parent's vms_pid: to the rest of the executive DCL
 * and the image it activated are ONE VMS process ($GETJPI resolves either to
 * the same identity). Mirrors the Linux twin field-for-field; job_id is not
 * copied because the NetBSD substrate has no job glue yet (it stays 0 here, as
 * vms_proc_get leaves it -- honest omission, not a fake value).
 *
 * LOCKING. hash_lock (outer) covers the identity fields and vms_pid; each proc's
 * mode_lock (inner) covers its 64-bit privilege masks -- taken outer-then-inner,
 * the same order as elsewhere. The parent's masks are read into locals under the
 * parent's mode_lock, then written to the child under the child's mode_lock, so
 * the two mode_locks are never held at once and the 64-bit masks are never torn
 * (which matters on 32-bit VAX, where a uint64_t store is two words).
 *
 * The parent is matched by pid NUMBER -- the same key vms_proc_get() and every
 * walk of vms_proc_hash use on this substrate; the facility reaper clears a dead
 * PCB before its pid can recycle, which is what makes the number a safe key.
 *
 * Returns the parent's VMS PID (nonzero) that the child now shares, or 0 when
 * the task has no registered VMS parent to continue (0 is never a valid vms_pid;
 * assign-side keys never hand it out). On the 0 return the child's fresh seed is
 * left untouched and the caller fails the _CONTINUE honestly -- it does NOT
 * silently substitute a fresh identity for a continuation that was asked for.
 */
static bool
vms_proc_continue_identity(struct vms_proc *proc, pid_t parent_pid,
    bool share_pid)
{
	struct vms_proc *p, *parent = NULL;
	uint64_t parent_perm, parent_cur;
	bool inherited = false;
	int bkt;

	if (parent_pid == 0)
		return false;

	exec_lock(&vms_proc_hash_lock);
	exec_hash_for_each(vms_proc_hash, bkt, p, hash_node) {
		if (p->pid == parent_pid) {
			parent = p;
			break;
		}
	}
	if (parent != NULL) {
		/* Identity fields: read parent + write child under hash_lock. */
		proc->uic = parent->uic;
		memcpy(proc->username, parent->username, sizeof(proc->username));
		memcpy(proc->prcnam,   parent->prcnam,   sizeof(proc->prcnam));
		memcpy(proc->terminal, parent->terminal, sizeof(proc->terminal));
		proc->cli_present = parent->cli_present;
		proc->cli_length  = parent->cli_length;
		memcpy(proc->cli_command, parent->cli_command,
		       sizeof(proc->cli_command));

		/* Privilege masks: read parent under its mode_lock into locals... */
		exec_lock(&parent->mode_lock);
		parent_perm = parent->perm_privs;
		parent_cur  = parent->cur_privs;
		exec_unlock(&parent->mode_lock);
		/* ...then write the child under the child's mode_lock. */
		exec_lock(&proc->mode_lock);
		proc->perm_privs = parent_perm;
		proc->cur_privs  = parent_cur;
		exec_unlock(&proc->mode_lock);

		/*
		 * SHARE the parent's VMS PID only for an image-activation CONTINUE
		 * (share_pid). A SUBPROCESS (_SUBPROCESS, vms-19e9) inherits the
		 * identity above but KEEPS the fresh vms_pid vms_proc_get() minted --
		 * a genuinely new VMS process, as $CREPRC creates. The Linux twin
		 * mirror.
		 */
		if (share_pid)
			proc->vms_pid = parent->vms_pid;
		inherited = true;
	}
	exec_unlock(&vms_proc_hash_lock);

	return inherited;
}

/*
 * vms_proc_free_claimed - tear down a PCB the facility's reaper has ALREADY
 * unlinked from vms_proc_hash under vms_proc_hash_lock (rd vms-ca7). That unlink
 * is the ownership claim (exactly one caller reaches here per entry), so this
 * runs unlocked and reclaims everything the process owned:
 *
 *   - its mailbox channels (dropping each mailbox's reference), while the
 *     mailbox list guard is still alive;
 *   - any ASTs still queued at all four modes (min PSL_C_KERNEL == 0);
 *   - its COMMON event-flag associations -- UNLIKE the module-unload teardown
 *     below, this DOES call vms_proc_release_common_ef(), because the reaped
 *     process dies mid-life while the clusters it $ASCEFC'd are still live and
 *     their refcounts must be decremented (a temporary cluster whose last
 *     associate this was is then freed). The unload path must NOT do this
 *     (vms_eflag_cleanup has already freed the clusters), which is the one
 *     asymmetry between the two teardown paths.
 *
 * Then it destroys the per-process sync objects and frees the PCB through
 * exec_free_deferred -- IMMEDIATE on NetBSD (no lockless readers; the blessed
 * grace-period fallback), mirroring the Linux vms_proc_free_claimed's kfree_rcu.
 * This is the glue half of the RCU-lite seam: the facility unlinks with
 * exec_hash_del_rcu and reclaims with exec_free_deferred -- two halves of one
 * idiom (exec_kbackend.h §6 GRACE-PERIOD CONTRACT).
 */
static void
vms_proc_rcu_free(exec_rcu_head_t *h)
{
	struct vms_proc *proc =
	    (struct vms_proc *)((char *)h - offsetof(struct vms_proc, rcu));
	exec_free(proc);
}

void
vms_proc_free_claimed(struct vms_proc *proc)
{
	int m;

	/*
	 * Release every DEVICE channel this process still held -- which ends any
	 * ownership resting on those channels -- and give back any device it had
	 * $ALLOCated (rd vms-618). vms_proc_release_channels() (vms_devtab.c) calls
	 * vms_mbx_release_all() itself as its last step, so the mailbox release
	 * that used to be here is INSIDE it now, not dropped. A device left owned
	 * by a process that no longer exists is not a state VMS has.
	 */
	vms_proc_release_channels(proc);
	/* Release every Files-11 ACP file-class channel this process still held
	 * ($DASSGN-all at process death, vms-d5d) -- the file_channels twin of the
	 * mailbox release above, mirroring the Linux vms.ko's vms_acp_release_all in
	 * vms_module.c's proc free. */
	vms_acp_release_all(proc);
	/* Release every lock this process still held ($DEQ-all at process death, P4-A
	 * rd vms-ff7). Runs while lock_list_lock is still alive, before it is
	 * destroyed below -- mirrors the Linux vms.ko's vms_proc_release_locks call
	 * in vms_module.c's proc free. */
	vms_proc_release_locks(proc);
	vms_proc_rundown_asts(proc, PSL_C_KERNEL);
	vms_proc_release_common_ef(proc);

	exec_lock_destroy(&proc->lock_list_lock);
	exec_lock_destroy(&proc->chan_lock);
	exec_cv_destroy(&proc->hiber_wq);
	exec_lock_destroy(&proc->hiber_lock);
	for (m = 0; m < 4; m++)
		exec_lock_destroy(&proc->ast[m].lock);
	exec_lock_destroy(&proc->mode_lock);
	exec_cv_destroy(&proc->ef.waitq);
	exec_lock_destroy(&proc->ef.lock);

	/* NetBSD: exec_free_deferred runs the callback at once -- no reader can
	 * still reach a node the reaper unlinked under vms_proc_hash_lock. */
	exec_free_deferred(&proc->rcu, vms_proc_rcu_free);
}

/*
 * vms_proctab_teardown - free every proc at module unload, walking the hash. The
 * COMMON clusters are freed separately by vms_eflag_cleanup(); we do NOT call
 * vms_proc_release_common_ef() here (it would touch clusters this teardown's
 * sibling has already freed) -- the one asymmetry with vms_proc_free_claimed
 * above, which runs mid-life while the clusters are still owned. We only tear
 * down each proc's own sync objects and free it. proc->ef.common[] pointers are
 * never dereferenced again. exec_hash_for_each_safe lets the body free the entry
 * it is standing on (the scratch `tmp' cursor captures ->next before the free).
 */
static void
vms_proctab_teardown(void)
{
	struct vms_proc *p;
	exec_hash_node_t *tmp;
	int bkt;

	exec_lock(&vms_proc_hash_lock);
	exec_hash_for_each_safe(vms_proc_hash, bkt, tmp, p, hash_node) {
		int m;
		exec_hash_del_rcu(&p->hash_node);
		/* Give back this process's mailbox channels first (dropping each
		 * mailbox's reference, freeing temporary/deleted mailboxes whose last
		 * channel this was) -- runs while the mailbox list guard is still alive
		 * (vms_mbx_cleanup() runs AFTER this, in vms_modcmd's FINI). Then free
		 * any AST entries still queued at unload (all four modes: min
		 * PSL_C_KERNEL == 0), then tear down each mode's guard and the mailbox/
		 * access/hibernate sync objects. In the harness every process has closed
		 * the device and no wait is in flight, so this is unraced.
		 *
		 * We do NOT call vms_proc_release_locks() here: vms_lock_cleanup() (run
		 * just before this in vms_modcmd's FINI) has already freed every lock
		 * entry by walking the resource database, so the entries this process's
		 * p->locks list pointed at are gone -- the same asymmetry as the COMMON
		 * event-flag teardown above (freed by vms_eflag_cleanup, not here). We
		 * only destroy this process's lock_list_lock guard, which vms_lock_cleanup
		 * did not own. */
		vms_proc_release_channels(p);
		vms_proc_rundown_asts(p, PSL_C_KERNEL);
		exec_lock_destroy(&p->lock_list_lock);
		exec_lock_destroy(&p->chan_lock);
		exec_cv_destroy(&p->hiber_wq);
		exec_lock_destroy(&p->hiber_lock);
		for (m = 0; m < 4; m++)
			exec_lock_destroy(&p->ast[m].lock);
		exec_lock_destroy(&p->mode_lock);
		exec_cv_destroy(&p->ef.waitq);
		exec_lock_destroy(&p->ef.lock);
		exec_free(p);
	}
	exec_unlock(&vms_proc_hash_lock);
}

/* ================================================================
 * Cross-facility image-rundown helpers (P4-A) -- ALL THREE NOW REAL.
 *
 * vms_ioctl_image_rundown() (src/kernel-core/vms_access.c) releases the image's
 * locks, channels and ASTs at rundown by calling three per-facility helpers.
 * vms_proc_rundown_asts is DEFINED (vms_ast.c), vms_proc_rundown_locks is
 * DEFINED (vms_lock.c, rd vms-ff7) and vms_proc_rundown_channels is DEFINED as
 * of the device-table port (vms_devtab.c, rd vms-618) -- it deassigns the USER-
 * mode DEVICE channels an activated image took. So the WEAK no-op stub this file
 * used to carry for the third one is GONE: nothing here shadows a real facility
 * any more. (Deleting it, rather than leaving it weak beside a strong twin, is
 * deliberate -- the static-link weak-seam trap: a weak stub that wins the link
 * makes a facility silently do nothing, which is the failure mode this project
 * has already been bitten by once.)
 * ================================================================ */

/* ================================================================
 * cdevsw
 * ================================================================ */

static dev_type_open(vms_open);
static dev_type_close(vms_close);
static dev_type_ioctl(vms_ioctl);
/*
 * The read-only logical-name arena's d_mmap (LNM$SYSTEM, rd vms-72da). Its
 * DEFINITION lives in the dedicated glue TU src/kernel-netbsd/vms_lnm_arena_netbsd.c
 * -- NOT here -- because it needs the uvm/pmap KPIs (<uvm/uvm_extern.h>) whose
 * transitive <sys/rbtree.h> collides with the executive's exec_rbtree_netbsd.h
 * that this TU includes (via vms_internal.h, for the DLM). So it is an EXTERNAL
 * reference here: dev_type_mmap declares it (external linkage), the cdevsw points
 * at it, and the glue TU defines it. See that file's header for the mapping. */
dev_type_mmap(vms_mmap);

static struct cdevsw vms_cdevsw = {
	.d_open     = vms_open,
	.d_close    = vms_close,
	.d_read     = noread,
	.d_write    = nowrite,
	.d_ioctl    = vms_ioctl,
	.d_stop     = nostop,
	.d_tty      = notty,
	.d_poll     = nopoll,
	.d_mmap     = vms_mmap,   /* read-only logical-name arena (LNM$SYSTEM, vms-72da) */
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

/*
 * vms_acp_rw_bounce - dispatch the Files-11 ACP READVBLK/WRITEVBLK, whose arg
 * (vms_acp_rw_args) carries a SEPARATE user data-buffer POINTER (.buffer), not
 * an inline payload like every other facility. NetBSD's cdevsw framework does
 * exactly ONE user<->kernel boundary crossing -- for the _IOWR arg struct
 * itself -- so the SHARED handler's exec_copyout(args.buffer, ...), an in-kernel
 * memcpy on this backend, cannot reach that user address. We bounce the payload
 * here with the SAME manual-copy technique vms_mbx_bigio() uses for the mailbox
 * transfer ops: kmem a kernel buffer, rewrite args.buffer to it so the shared
 * (BYTE-UNCHANGED) handler memcpys against the bounce, then copyout the result
 * to the caller. DO NOT "simplify" this into the shared path: the shared handler
 * is platform-agnostic by design, and this user-copy glue belongs in the
 * platform dispatch, exactly where mbx's lives (vms-d5d).
 */
static int
vms_acp_rw_bounce(struct lwp *l, void *data, int for_write)
{
	struct vms_acp_rw_args *a = (struct vms_acp_rw_args *)data;
	void *ubuf = (void *)(uintptr_t)a->buffer;   /* caller's separate data buffer */
	uint32_t len = a->length;
	struct vms_proc *proc;
	void *bounce = NULL;
	long r;

	proc = vms_proc_get(l->l_proc->p_pid);
	if (proc == NULL)
		return ENOMEM;

	/*
	 * Bounce only an in-range transfer. len == 0 moves nothing; len > 1 MiB
	 * (ACP_RW_MAX_XFER) the shared handler rejects with SS$_BADPARAM BEFORE it
	 * touches .buffer -- so both fall through with .buffer left as the user
	 * pointer, and we never kmem an attacker-sized bounce.
	 */
	if (len > 0 && len <= (1u << 20)) {
		bounce = exec_alloc(len);
		if (bounce == NULL)
			return ENOMEM;
		if (for_write && ubuf != NULL && copyin(ubuf, bounce, len)) {
			exec_free(bounce);
			return EFAULT;
		}
		a->buffer = (uint64_t)(uintptr_t)bounce;   /* handler memcpys the bounce */
	}

	r = for_write ? vms_ioctl_acp_writevb(proc, (unsigned long)data)
	              : vms_ioctl_acp_readvb(proc, (unsigned long)data);

	if (!for_write && bounce != NULL && r == 0 && a->xferred > 0) {
		uint32_t n = a->xferred > len ? len : a->xferred;
		if (copyout(bounce, ubuf, n))
			r = -EFAULT;
	}
	if (bounce != NULL) {
		/* Restore the caller's pointer so the framework's arg copyout does not
		 * leak the kernel bounce address back to userspace. */
		a->buffer = (uint64_t)(uintptr_t)ubuf;
		exec_free(bounce);
	}
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
	 * System memory statistics ($GETSYI-style; SHOW MEMORY's "Physical Memory
	 * Usage" section, rd vms-a3cd). System-wide, so -- like PING above and
	 * unlike the process-table group below -- it needs no proc lookup: it reads
	 * the global uvm counters directly. _IOWR, so `data' is the kernel-resident
	 * copy NetBSD copies back out on return 0. ovmx_sysmem_bytes() (the
	 * dedicated uvm-only TU vms_sysmem_netbsd.c) sources total/free physical
	 * memory in bytes via the MAINTAINED accessor uvm_availmem(true); when uvm
	 * is not yet up it returns 0 and we leave VMS_SYIMEM_V_PHYS clear so the
	 * renderer honestly omits the section (INV-6), never a fabricated 0. */
	case VMS_IOCTL_GETSYIMEM: {
		struct vms_getsyi_mem_args *ma = (struct vms_getsyi_mem_args *)data;
		uint64_t total_b = 0, free_b = 0;

		ma->info.total_bytes  = 0;
		ma->info.free_bytes   = 0;
		ma->info.fields_valid = 0;
		ma->info.reserved     = 0;
		ma->reserved          = 0;
		if (ovmx_sysmem_bytes(&total_b, &free_b)) {
			ma->info.total_bytes  = total_b;
			ma->info.free_bytes   = free_b;
			ma->info.fields_valid = VMS_SYIMEM_V_PHYS;
		}
		ma->status = VMS_SS_NORMAL;
		return 0;
	}

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

	/*
	 * Files-11 (ODS-2) ACP file operations (vms-d5d, epic vms-208): the VAX
	 * runtime reaches SYS$DISK over the executive ACP, not the vmsfs.ko VFS
	 * mount. These are _IOWR and <= 344 bytes (fileop_args), so they ride the
	 * framework pre-copy path -- hand `data' straight to the shared handler
	 * (src/kernel-core/vmsfs_acp.c), identical to the Linux vms.ko dispatch
	 * (vms_module.c). READVBLK/WRITEVBLK are the EXCEPTION: their arg carries a
	 * separate user data-buffer pointer, so they route through vms_acp_rw_bounce.
	 */
	case VMS_IOCTL_ACP_MOUNT:
	case VMS_IOCTL_ACP_DMOUNT:
	case VMS_IOCTL_ACP_ASSIGN:
	case VMS_IOCTL_ACP_ACCESS:
	case VMS_IOCTL_ACP_DEACCESS:
	case VMS_IOCTL_ACP_ACPCONTROL:
	case VMS_IOCTL_ACP_FILEOP:
	case VMS_IOCTL_GETVOL:
	case VMS_IOCTL_DISK_RESOLVE:
		uarg = data;
		proc = vms_proc_get(l->l_proc->p_pid);
		if (proc == NULL)
			return ENOMEM;

		switch (cmd) {
		case VMS_IOCTL_ACP_MOUNT:
			r = vms_ioctl_acp_mount(proc, (unsigned long)uarg);      break;
		case VMS_IOCTL_ACP_DMOUNT:
			r = vms_ioctl_acp_dmount(proc, (unsigned long)uarg);     break;
		case VMS_IOCTL_ACP_ASSIGN:
			r = vms_ioctl_acp_assign(proc, (unsigned long)uarg);     break;
		case VMS_IOCTL_ACP_ACCESS:
			r = vms_ioctl_acp_access(proc, (unsigned long)uarg);     break;
		case VMS_IOCTL_ACP_DEACCESS:
			r = vms_ioctl_acp_deaccess(proc, (unsigned long)uarg);   break;
		case VMS_IOCTL_ACP_ACPCONTROL:
			r = vms_ioctl_acp_acpcontrol(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_ACP_FILEOP:
			r = vms_ioctl_acp_fileop(proc, (unsigned long)uarg);     break;
		case VMS_IOCTL_GETVOL:
			r = vms_ioctl_acp_getvol(proc, (unsigned long)uarg);     break;
		case VMS_IOCTL_DISK_RESOLVE:
			r = vms_ioctl_disk_resolve(proc, (unsigned long)uarg);   break;
		default:
			return ENOTTY;   /* unreachable */
		}
		return vms_facility_errno(r);

	/*
	 * VMS_IOCTL_REGISTER (vms-329): adopt-or-create this process's executive
	 * PCB, FRESH-SEEDED. Every other ioctl path here does that implicitly
	 * through vms_proc_get(), which is why the op was never wired -- but the
	 * SHARED userspace ACP client (src/imgact/imgact_acp.c, used by IMGACT.EXE,
	 * the RMS ACP arm and PID 1's boot bridge) opens every file with
	 * acp_register() and treats a failure as fatal. Unanswered, it returned
	 * ENOTTY -> SS$_NOSUCHDEV and NO ACP file open could ever succeed on this
	 * substrate. This creates no new policy: it returns the PCB vms_proc_get()
	 * builds anyway, matching the Linux twin's "hand back the process that
	 * already exists" (vms_module.c). _IOWR, 8 bytes: `data' is the kernel
	 * copy, answered in place.
	 */
	case VMS_IOCTL_REGISTER: {
		struct vms_register_args *ra = (struct vms_register_args *)data;

		proc = vms_proc_get(l->l_proc->p_pid);
		if (proc == NULL)
			return ENOMEM;
		ra->vms_pid = proc->vms_pid;
		ra->status  = SS__NORMAL;
		return 0;
	}

	/*
	 * VMS_IOCTL_REGISTER_CONTINUE (vms-381): register this task as a
	 * CONTINUATION of its parent's VMS process, NOT a fresh identity. DCL
	 * fork()s + execv()s to activate an image and the child calls this before
	 * execv while it is still DCL's child; the activated image must run as the
	 * SAME VMS process -- same UIC, user name, CLI context and, decisively, the
	 * parent's CURRENT privilege masks (SYSPRV/BYPASS granted to SYSTEM's DCL
	 * via $SETIDENT), so it can open the World-denied SYSUAF.DAT that AUTHORIZE
	 * and LOGINOUT need. This case used to be folded in with REGISTER above and
	 * fresh-seeded identically, so the child NEVER inherited SYSPRV/BYPASS and
	 * install failed with %UAF-E-NAOFIL / -RMS-E-PRV. It now genuinely continues
	 * the parent, mirroring src/kernel/vms_module.c's vms_proc_continue_identity
	 * (INV-6: this REMOVES the fresh-seed fabrication on the continue path).
	 *
	 * The parent is the calling task's real parent, p_pptr->p_pid. If it has no
	 * registered VMS PCB there is nothing to continue: fail honestly (ESRCH,
	 * leaving vms_proc_get()'s fresh seed as this child's own top-level
	 * identity) rather than pretend a continuation happened. REGISTER (above),
	 * not _CONTINUE, is the fresh-seed path.
	 */
	case VMS_IOCTL_REGISTER_CONTINUE: {
		struct vms_register_args *ra = (struct vms_register_args *)data;
		struct proc *pp;
		pid_t parent_pid;

		proc = vms_proc_get(l->l_proc->p_pid);
		if (proc == NULL)
			return ENOMEM;

		pp = l->l_proc->p_pptr;
		parent_pid = (pp != NULL) ? pp->p_pid : 0;
		/* share_pid = true: DCL and the image it activated are ONE VMS
		 * process, so the child adopts the parent's VMS PID. */
		if (!vms_proc_continue_identity(proc, parent_pid, true))
			return ESRCH;   /* no registered VMS parent to continue */

		ra->vms_pid = proc->vms_pid;   /* == the shared parent VMS PID */
		ra->status  = SS__NORMAL;
		return 0;
	}

	/*
	 * VMS_IOCTL_REGISTER_SUBPROCESS (vms-19e9): SPAWN/$CREPRC. A genuinely
	 * new VMS process that INHERITS the creator's identity by continuation
	 * from its real parent but KEEPS the fresh VMS PID vms_proc_get() minted
	 * -- a distinct VMS process, unlike _CONTINUE. Mirrors the Linux twin
	 * (src/kernel/vms_module.c). Unlike _CONTINUE, a task with no registered
	 * VMS parent does NOT fail: it keeps vms_proc_get()'s fresh, honestly
	 * credential-derived identity (a top-level process), so SPAWN never
	 * fails merely because the creator was not registered.
	 */
	case VMS_IOCTL_REGISTER_SUBPROCESS: {
		struct vms_register_args *ra = (struct vms_register_args *)data;
		struct proc *pp;
		pid_t parent_pid;

		proc = vms_proc_get(l->l_proc->p_pid);
		if (proc == NULL)
			return ENOMEM;

		pp = l->l_proc->p_pptr;
		parent_pid = (pp != NULL) ? pp->p_pid : 0;
		/* share_pid = false: inherit identity, keep the fresh VMS PID. */
		(void)vms_proc_continue_identity(proc, parent_pid, false);

		ra->vms_pid = proc->vms_pid;   /* the FRESH, distinct VMS PID */
		ra->status  = SS__NORMAL;
		return 0;
	}

	/*
	 * DEVICE-TABLE facility (src/kernel-core/vms_devtab.c) -- rd vms-618, the
	 * LAST executive facility to join this module. Same dispatch shape as every
	 * other facility above: find-or-create the caller's proc, hand the
	 * framework's kernel buffer `data' straight to the SHARED facility, map its
	 * Linux-style return to a NetBSD errno. Nothing here interprets the
	 * argument, and nothing here is substrate-local: $ALLOC's answer comes from
	 * the ONE executive-resident device table every process on the node shares,
	 * which is the whole point (INV-6 -- a substrate-local ALLOC that returned
	 * success with no real table would pass every single-process test and still
	 * be a facade; the decisive property is that a SECOND process is refused
	 * SS$_DEVALLOC).
	 *
	 * $ALLOC (0x55) is the DCL MOUNT prerequisite: dcl_cmd_misc.c's cmd_mount()
	 * calls vms_kif_alloc(dev) BEFORE vms_kif_acp_mount(), so while this
	 * answered ENOTTY no MOUNT of any device could succeed on NetBSD/vax.
	 *
	 * $DASSGN (0x51, wired by vms-329) MOVES HERE from the substrate-local
	 * fallback chain this file used to carry: vms_devtab.c's handler is a strict
	 * superset -- device channels FIRST, then file-class (ACP), then mailbox --
	 * so there is now exactly one $DASSGN implementation, and a device channel
	 * (impossible before the table existed) is released correctly. A channel
	 * that is none of the three still gets SS$_IVCHAN, never a fabricated
	 * success.
	 */
	/*
	 * Device-table facility (src/kernel-core/vms_devtab.c). rd vms-6a6: the
	 * $ASSIGN/$GETDVI/device-scan/terminal ioctls were handled by the SHARED
	 * kernel-core code (already compiled + linked into vms.kmod, declared in
	 * vms_internal.h) but were NEVER routed here -- they fell through the outer
	 * default: ENOTTY -> SS$_ILLIOFUNC, so on VAX SHOW DEVICE printed nothing,
	 * SHOW DEVICES enumerated nothing, and SHOW USERS reported 0 users (no login
	 * could bind its console terminal via $ASSIGN + SETTERM, so vms_proctab's
	 * classifier never stamped any process INTERACTIVE). The device table AND the
	 * console terminal OPA0: are already populated on VAX (vms_devtab_init() +
	 * vms_blockdev_netbsd_register_units at attach); this just wires the dispatch,
	 * exactly as x86 does (src/kernel/vms_module.c). Same marshaling as the
	 * $DASSGN/$ALLOC group: find-or-create the caller's proc, hand the framework's
	 * kernel buffer `data' straight to the shared (proc, arg) handler.
	 */
	case VMS_IOCTL_ASSIGN:
	case VMS_IOCTL_DASSGN:
	case VMS_IOCTL_ALLOC:
	case VMS_IOCTL_DALLOC:
	case VMS_IOCTL_GETDVI:
	case VMS_IOCTL_DEVSCAN:
	case VMS_IOCTL_TTSETMODE:
	case VMS_IOCTL_SETTERM:
		uarg = data;
		proc = vms_proc_get(l->l_proc->p_pid);
		if (proc == NULL)
			return ENOMEM;

		switch (cmd) {
		case VMS_IOCTL_ASSIGN:
			r = vms_ioctl_assign(proc, (unsigned long)uarg);    break;
		case VMS_IOCTL_DASSGN:
			r = vms_ioctl_dassgn(proc, (unsigned long)uarg);    break;
		case VMS_IOCTL_ALLOC:
			r = vms_ioctl_alloc(proc, (unsigned long)uarg);     break;
		case VMS_IOCTL_DALLOC:
			r = vms_ioctl_dalloc(proc, (unsigned long)uarg);    break;
		case VMS_IOCTL_GETDVI:
			r = vms_ioctl_getdvi(proc, (unsigned long)uarg);    break;
		case VMS_IOCTL_DEVSCAN:
			r = vms_ioctl_devscan(proc, (unsigned long)uarg);   break;
		case VMS_IOCTL_TTSETMODE:
			r = vms_ioctl_ttsetmode(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_SETTERM:
			r = vms_ioctl_setterm(proc, (unsigned long)uarg);   break;
		default:
			return ENOTTY;   /* unreachable */
		}
		return vms_facility_errno(r);

	case VMS_IOCTL_ACP_READVBLK:
		return vms_acp_rw_bounce(l, data, 0);
	case VMS_IOCTL_ACP_WRITEVBLK:
		return vms_acp_rw_bounce(l, data, 1);

	/*
	 * Process-table facility (src/kernel-core/vms_proctab.c) -- P4-A, rd
	 * vms-ca7. Same dispatch shape as event flags: find-or-create the caller's
	 * proc, hand the framework's kernel buffer `data' straight to the shared
	 * facility, map its Linux-style return to a NetBSD errno. All _IOWR and <=
	 * 288 bytes (getjpi_args), so all ride the framework pre-copy path -- no
	 * IOC_VOID big-io shape (unlike the mailbox WRITE/READ transfer ops).
	 *
	 * $GETJPI/$PROCSCAN read a process's identity + real host accounting out of
	 * the ONE executive process table (vms_proc_hash) every process shares;
	 * $SETPRN records this process's name there where another process's $GETJPI
	 * can resolve it -- the INV-6-decisive property, the whole reason the name
	 * lives in the kernel and not in the asking process. $HIBER/$WAKE block/
	 * release in the executive (interruptible by deliverable ASTs); $SETIDENT/
	 * $ESTABLISH_SYSTEM stamp an authenticated / SYSTEM identity, gated by the
	 * caller's real host credential (exec_current_is_privileged).
	 */
	case VMS_IOCTL_SETPRN:
	case VMS_IOCTL_GETJPI:
	case VMS_IOCTL_PROCSCAN:
	case VMS_IOCTL_SETIDENT:
	case VMS_IOCTL_ESTABLISH_SYSTEM:
	case VMS_IOCTL_HIBER:
	case VMS_IOCTL_WAKE:
	case VMS_IOCTL_SETEXIT:
	case VMS_IOCTL_GETEXIT:
	case VMS_IOCTL_SETCLI:
	case VMS_IOCTL_GETCLI:
	case VMS_IOCTL_SPAWN_NOTIFY:
		uarg = data;
		proc = vms_proc_get(l->l_proc->p_pid);
		if (proc == NULL)
			return ENOMEM;

		switch (cmd) {
		case VMS_IOCTL_SETPRN:
			r = vms_ioctl_setprn(proc, (unsigned long)uarg);            break;
		case VMS_IOCTL_GETJPI:
			r = vms_ioctl_getjpi(proc, (unsigned long)uarg);           break;
		case VMS_IOCTL_PROCSCAN:
			r = vms_ioctl_procscan(proc, (unsigned long)uarg);         break;
		case VMS_IOCTL_SETIDENT:
			r = vms_ioctl_setident(proc, (unsigned long)uarg);         break;
		case VMS_IOCTL_ESTABLISH_SYSTEM:
			r = vms_ioctl_establish_system(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_HIBER:
			r = vms_ioctl_hiber(proc, (unsigned long)uarg);            break;
		case VMS_IOCTL_WAKE:
			r = vms_ioctl_wake(proc, (unsigned long)uarg);             break;
		/* $EXIT/$STATUS + CLI invocation context (vms-f60d) */
		case VMS_IOCTL_SETEXIT:
			r = vms_ioctl_setexit(proc, (unsigned long)uarg);          break;
		case VMS_IOCTL_GETEXIT:
			r = vms_ioctl_getexit(proc, (unsigned long)uarg);          break;
		case VMS_IOCTL_SETCLI:
			r = vms_ioctl_setcli(proc, (unsigned long)uarg);           break;
		case VMS_IOCTL_GETCLI:
			r = vms_ioctl_getcli(proc, (unsigned long)uarg);           break;
		/* /NOWAIT subprocess-exit completion arm (vms-e9a B1) */
		case VMS_IOCTL_SPAWN_NOTIFY:
			r = vms_ioctl_spawn_notify(proc, (unsigned long)uarg);     break;
		default:
			return ENOTTY;   /* unreachable */
		}
		return vms_facility_errno(r);

	/*
	 * Lock-manager facility (DLM, src/kernel-core/vms_lock.c) -- P4-A, rd
	 * vms-ff7, the LAST executive facility. Same dispatch shape as the others:
	 * find-or-create the caller's proc, hand the framework's kernel buffer `data'
	 * straight to the shared facility, map its Linux-style return to a NetBSD
	 * errno. All five are _IOWR and small (<= 104 bytes, vms_enq_args), so all
	 * ride the framework pre-copy path -- no IOC_VOID big-io shape (unlike the
	 * mailbox WRITE/READ transfer ops).
	 *
	 * $ENQ/$CONVERT take/upgrade a lock on a NAMED resource held in the ONE
	 * executive resource database (vms_res_hash) every process shares, so an
	 * incompatible request from a DIFFERENT process genuinely blocks or is
	 * refused -- the INV-6-decisive property. A synchronous ($ENQW) request that
	 * cannot be granted at once BLOCKS in the executive (exec_cv_wait_timeout in
	 * enq_wait_sync) until granted or a wait-for cycle is detected (SS$_DEADLOCK);
	 * that in-kernel wait can return -ERESTARTSYS, which vms_facility_errno maps
	 * to ERESTART so the ioctl restarts, exactly as the event-flag WAITFR path
	 * does. GET_RESMASTER is a read-only DLM directory/mastering view.
	 */
	case VMS_IOCTL_ENQ:
	case VMS_IOCTL_DEQ:
	case VMS_IOCTL_CONVERT:
	case VMS_IOCTL_GETLKI:
	case VMS_IOCTL_GET_RESMASTER:
	case VMS_IOCTL_DLM_MEMBER_DEPART:
	case VMS_IOCTL_DLM_GET_GRANTED:
	case VMS_IOCTL_DLM_ENUM_WAITS:
	case VMS_IOCTL_DLM_ENUM_STANDING:
	case VMS_IOCTL_DLM_DIRECTORY_SET:
	case VMS_IOCTL_CLUSTER_MEMBER_SET:
	case VMS_IOCTL_CLUSTER_MEMBER_CLEAR:
	case VMS_IOCTL_CLUSTER_MEMBER_GET:
		uarg = data;
		proc = vms_proc_get(l->l_proc->p_pid);
		if (proc == NULL)
			return ENOMEM;

		switch (cmd) {
		case VMS_IOCTL_ENQ:
			r = vms_ioctl_enq(proc, (unsigned long)uarg);           break;
		case VMS_IOCTL_DEQ:
			r = vms_ioctl_deq(proc, (unsigned long)uarg);           break;
		case VMS_IOCTL_CONVERT:
			r = vms_ioctl_convert(proc, (unsigned long)uarg);       break;
		case VMS_IOCTL_GETLKI:
			r = vms_ioctl_getlki(proc, (unsigned long)uarg);        break;
		case VMS_IOCTL_GET_RESMASTER:
			r = vms_ioctl_get_resmaster(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_DLM_MEMBER_DEPART:
			r = vms_ioctl_dlm_member_depart(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_DLM_GET_GRANTED:
			r = vms_ioctl_dlm_get_granted(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_DLM_ENUM_WAITS:
			r = vms_ioctl_dlm_enum_waits(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_DLM_ENUM_STANDING:
			r = vms_ioctl_dlm_enum_standing(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_DLM_DIRECTORY_SET:
			r = vms_ioctl_dlm_directory_set(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_CLUSTER_MEMBER_SET:
			r = vms_ioctl_cluster_member_set(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_CLUSTER_MEMBER_CLEAR:
			r = vms_ioctl_cluster_member_clear(proc, (unsigned long)uarg); break;
		case VMS_IOCTL_CLUSTER_MEMBER_GET:
			r = vms_ioctl_cluster_member_get(proc, (unsigned long)uarg); break;
		default:
			return ENOTTY;   /* unreachable */
		}
		return vms_facility_errno(r);

	/*
	 * Logical-name facility (LNM$SYSTEM/GROUP/JOB, src/kernel-core/vms_lnm.c) --
	 * rd vms-72da. Same dispatch shape as the others: find-or-create the caller's
	 * proc (the facility derives its LNM$GROUP/JOB scope from the PCB's uic/job_id
	 * and gates DEFINE/DELETE on the PCB's cur_privs), hand the framework's kernel
	 * buffer `data' straight to the shared facility, map its Linux-style return to
	 * a NetBSD errno. All three are _IOWR; the largest (vms_lnm_def_args, 2352 B)
	 * is <= one page (NBPG == 4096 on NetBSD/vax), so all ride the framework
	 * pre-copy path -- no IOC_VOID big-io shape (unlike the mailbox WRITE/READ
	 * transfer ops). Translation is NOT here: it is a zero-syscall read of the
	 * read-only arena the char device's d_mmap (vms_mmap) publishes.
	 *
	 * These CREATE and RESOLVE SYS$STARTUP / SYS$LOGIN / SYS$UPDATE (and the whole
	 * LNM$SYSTEM table) during PROVISION's STARTUP phase -- the INV-6-decisive
	 * property: a name one process defines in LNM$SYSTEM is visible to every other
	 * process on the node, because the table lives in the executive and is shared,
	 * not faked per-process.
	 */
	case VMS_IOCTL_LNM_DEFINE:
	case VMS_IOCTL_LNM_DELETE:
	case VMS_IOCTL_LNM_GETSCOPE:
		uarg = data;
		proc = vms_proc_get(l->l_proc->p_pid);
		if (proc == NULL)
			return ENOMEM;

		switch (cmd) {
		case VMS_IOCTL_LNM_DEFINE:
			r = vms_ioctl_lnm_define(proc, (unsigned long)uarg);   break;
		case VMS_IOCTL_LNM_DELETE:
			r = vms_ioctl_lnm_delete(proc, (unsigned long)uarg);   break;
		case VMS_IOCTL_LNM_GETSCOPE:
			r = vms_ioctl_lnm_getscope(proc, (unsigned long)uarg); break;
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
		 * global); the process table has its own guard and its buckets are
		 * emptied here. (vms_proctab.c's reap serializer, vms_reap_mutex, is a
		 * file-static exec_mutex that initializes itself on first use -- see
		 * exec_kbackend_netbsd.h -- so it needs no init here.)
		 */
		exec_lock_init(&vms_proc_hash_lock);
		exec_hash_init(vms_proc_hash);
		vms_eflag_init();
		/* Bring up the mailbox table's list guard too (the table starts empty
		 * -- mailboxes are created on demand by $CREMBX). */
		vms_mbx_init();
		/* Bring up the lock manager (P4-A, rd vms-ff7): its two runtime-init'd
		 * guards (resource hash + lock-ID tree) and the empty resource-database
		 * bucket array, before any $ENQ can run. */
		vms_lock_init();
		/* Bring up the logical-name arena (rd vms-72da) BEFORE /dev/vms exists,
		 * so no open/mmap can race a NULL arena. vms_lnm_init allocates the ONE
		 * read-only-publishable arena (uvm_km_alloc(UVM_KMF_WIRED)); a failure
		 * here is out of memory, so unwind the facilities brought up above and
		 * refuse to attach rather than expose a device whose LNM path can only
		 * fail. */
		error = vms_lnm_init();
		if (error != 0) {
			printf("vms: vms_lnm_init failed: %d\n", error);
			vms_eflag_cleanup();
			vms_lock_cleanup();
			vms_proctab_teardown();
			vms_mbx_cleanup();
			exec_lock_destroy(&vms_proc_hash_lock);
			return ENOMEM;
		}
		/* Prove the arena seam on THIS substrate before /dev/vms is openable
		 * (rd vms-72da): one console line reporting the arena kva -> pa ->
		 * magic roundtrip, so a broken mmap publish is visible here, not only as
		 * a downstream SYS$SYSTEM resolution failure. */
		vms_lnm_arena_selftest();

		/*
		 * Bring up the EXECUTIVE DEVICE TABLE (rd vms-618) BEFORE /dev/vms
		 * exists, so no $ALLOC can race an uninitialised list guard. Two
		 * steps, in the order a real VMS system initializes:
		 *   1. vms_devtab_init() creates the console terminal OPA0: -- no
		 *      process registers it; a process that never asked for it still
		 *      sees it, exactly as the terminal driver creates the console
		 *      unit during system initialization.
		 *   2. vms_blockdev_netbsd_register_units() enters this node's DISK
		 *      units from the device-native unit map (DUA0: -> ra1c,
		 *      DUA100: -> ra2c), and ONLY for a device that really resolves
		 *      -- an absent disk gets no row, so $ALLOC of it is an honest
		 *      SS$_NOSUCHDEV rather than an invented unit (INV-6).
		 * A failure here is out of memory: unwind exactly as the lnm arm above.
		 */
		error = vms_devtab_init();
		if (error != 0) {
			printf("vms: vms_devtab_init failed: %d\n", error);
			vms_lnm_cleanup();
			vms_eflag_cleanup();
			vms_lock_cleanup();
			vms_proctab_teardown();
			vms_mbx_cleanup();
			exec_lock_destroy(&vms_proc_hash_lock);
			return ENOMEM;
		}
		vms_blockdev_netbsd_register_units();

		error = devsw_attach("vms", NULL, &bmajor, &vms_cdevsw, &cmajor);
		if (error != 0) {
			printf("vms: devsw_attach failed: %d\n", error);
			vms_devtab_cleanup();
			vms_lnm_cleanup();
			vms_eflag_cleanup();
			vms_lock_cleanup();
			vms_proctab_teardown();
			vms_mbx_cleanup();
			exec_lock_destroy(&vms_proc_hash_lock);
			return error;
		}
		/* The harness reads this line back from dmesg to mknod /dev/vms with
		 * the dynamically assigned major. */
		printf("vms: registered, char major %d\n", cmajor);
		/* Bring up the Files-11 ODS-2 ACP global state (vms-d5d) AFTER the device
		 * is attached: no ioctl can arrive until userspace opens /dev/vms, which
		 * is after INIT returns, and vms_acp_init cannot fail (vmsfs_acp.c), so it
		 * needs no unwind. Mirrors the Linux vms.ko init. */
		vms_acp_init();
		return 0;

	case MODULE_CMD_FINI:
		/*
		 * Called as a bare statement so this compiles whether the running
		 * NetBSD declares devsw_detach as returning void or int. In the
		 * harness every process has closed the device before unload, so it is
		 * not busy and no facility wait is in flight.
		 */
		devsw_detach(NULL, &vms_cdevsw);
		/* Free the shared common-EF clusters and every lock entry + resource
		 * (vms_lock_cleanup, walking the resource database) FIRST, then tear down
		 * the procs (this gives back each proc's mailbox channels, drains its
		 * ASTs and destroys its lock_list_lock while the mailbox list guard is
		 * still alive -- the lock ENTRIES it pointed at are already gone), then
		 * free any remaining permanent mailboxes and destroy the mailbox list
		 * guard, then the proc-table guard. Ordering mirrors the eflag asymmetry:
		 * the facility frees its shared objects, then proctab_teardown frees the
		 * PCBs without re-releasing them. */
		vms_eflag_cleanup();
		vms_lock_cleanup();
		vms_proctab_teardown();
		vms_mbx_cleanup();
		/* Tear down the Files-11 ODS-2 ACP (vms-d5d): free its global volume/
		 * channel state (each proc already gave back its file channels via
		 * vms_proc_free_claimed above), then close the cached backing device
		 * vnode the ACP $MOUNT opened. Mirrors the Linux vms.ko FINI. */
		vms_acp_cleanup();
		/* Free the device table's rows AFTER the procs are gone (rd vms-618):
		 * vms_proctab_teardown above ran each proc's vms_proc_release_channels,
		 * which unlinks its channels FROM these rows, so nothing points at a
		 * device row by the time it is freed. */
		vms_devtab_cleanup();
		vms_blockdev_netbsd_release_all();
		/* Free the logical-name arena LAST (rd vms-72da): no process can reach it
		 * anymore (the device is detached) and no facility above references it. */
		vms_lnm_cleanup();
		exec_lock_destroy(&vms_proc_hash_lock);
		printf("vms: unregistered\n");
		return 0;

	default:
		return ENOTTY;
	}
}
