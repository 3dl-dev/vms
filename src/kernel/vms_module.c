// SPDX-License-Identifier: GPL-2.0
/*
 * vms_module.c - Core VMS kernel module
 *
 * Provides /dev/vms character device with ioctl interface for:
 *   - Access mode enforcement (kernel/exec/super/user)
 *   - 4-level AST delivery
 *   - Kernel event flags
 *   - Lock manager with 6-mode compatibility
 *
 * Processes register via VMS_IOCTL_REGISTER to get a per-process
 * vms_proc structure allocated in kernel memory.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/rbtree.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>     /* thread_group_empty() */
#include <linux/mm.h>               /* vm_area_struct, vm_flags_clear, PAGE_* (vms_lnm_mmap) */
#include <linux/vmalloc.h>          /* remap_vmalloc_range (vms_lnm_mmap, vms-d61) */

#include "vms_internal.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OVMX Project");
MODULE_DESCRIPTION("VMS subsystem kernel module");
MODULE_VERSION("1.0");

/* ================================================================
 * Global state
 * ================================================================ */

DEFINE_HASHTABLE(vms_proc_hash, VMS_PROC_HASH_BITS);
DEFINE_SPINLOCK(vms_proc_hash_lock);

static struct kmem_cache *vms_proc_cache;

/*
 * vms_local_csid - this node's cluster system ID, consumed by the DLM directory
 * + mastering logic in the shared lock manager (src/kernel-core/vms_lock.c). The
 * variable and its insmod module parameter live HERE, in the Linux module rind,
 * not in the substrate-agnostic core: a module parameter is a host-module-
 * lifecycle facility, and the core facility reaches this value through the extern
 * in vms_internal.h. 0 is reserved for "unmastered", so the default is a non-zero
 * OVMX local placeholder; it is NOT a claim of a VMS-authentic CSID value or
 * layout (CLAUDE.md Rule 8) -- real CSIDs are assigned by the connection manager
 * at cluster join in 0.4.
 */
uint32_t vms_local_csid = 1;
module_param(vms_local_csid, uint, 0444);
MODULE_PARM_DESC(vms_local_csid,
    "OVMX DLM: this node's cluster system ID (local scaffolding; the connection manager assigns the real CSID at cluster join in 0.4)");

/* ================================================================
 * Process management
 * ================================================================ */

struct vms_proc *vms_proc_find(pid_t pid)
{
    struct vms_proc *proc;

    rcu_read_lock();
    hash_for_each_possible_rcu(vms_proc_hash, proc, hash_node, pid) {
        if (proc->linux_pid == pid) {
            rcu_read_unlock();
            return proc;
        }
    }
    rcu_read_unlock();
    return NULL;
}

/*
 * vms_proc_find_or_err - this TASK'S process entry.
 *
 * KEYED ON THE THREAD GROUP, NOT THE THREAD (vms-9fc round 2).
 *
 * On OpenVMS a process has exactly ONE PCB and its kernel threads SHARE
 * it -- that shared residency is the entire meaning of a process-wide
 * event flag cluster, a process logical name table, a process name and a
 * process's lock ids. Keying this table on current->pid (the Linux TID)
 * minted one VMS process per THREAD: two threads of one image disagreed
 * about their own identity, could not see each other's event flags, and
 * could not release each other's locks. That is Rule 11's facade shape
 * inverted -- per-thread state pretending to be per-process -- and it is
 * not something VMS can be in.
 *
 * Linux's process-wide identifier is the thread-group id: current->tgid,
 * which is what getpid(2) returns, while current->pid is what gettid(2)
 * returns. So tgid is the key, and task_tgid() is the pinned identity.
 */
struct vms_proc *vms_proc_find_or_err(void)
{
    struct vms_proc *proc = vms_proc_find(current->tgid);

    /*
     * A table entry now outlives the /dev/vms channel (vms-8019), so an
     * entry keyed by this pid number is not automatically OURS: if the
     * pid has been recycled, the entry belongs to a process that no
     * longer exists, and handing it over would hand over its
     * privileges with it. Match on the pinned struct pid, which is
     * unique per process instance, rather than on the reusable number.
     */
    if (proc && proc->pid_ref != task_tgid(current))
        return NULL;

    return proc;
}

/*
 * vms_pid_counter - the executive's process-ID generator (vms-2b8).
 *
 * OVMX DESIGN CHOICE, labelled in vms_ioctl.h: OpenVMS builds a process
 * ID from a PCB-vector index plus a sequence number, and no public
 * document publishes that layout byte for byte, so OVMX does not
 * imitate it. What OVMX reproduces is the property that matters to
 * every caller -- the ID is assigned by the executive, is unique among
 * live processes, and is not handed straight back to the next process
 * when one exits.
 *
 * The base is deliberately above any value a Linux pid can take under
 * the kernel's own PID_MAX_LIMIT (2^22), so a VMS process ID is never
 * mistakable for the Linux pid it used to be copied from.
 */
#define VMS_PID_BASE    0x10000000u
static atomic_t vms_pid_counter = ATOMIC_INIT(0);

/*
 * assign_vms_pid - hand out an unused VMS process ID.
 *
 * MUST be called with vms_proc_hash_lock held, so that the uniqueness
 * scan and the insertion that follows it cannot be separated: two
 * concurrent registrations that each found "no clash" and then both
 * inserted would recreate the very collision this exists to prevent.
 *
 * A duplicate is possible only after 2^32 registrations have wrapped
 * the counter onto a still-live process, so the retry loop is a
 * correctness backstop rather than a hot path -- but it is not
 * optional: "unlikely" is not "unique", and a single collision lets
 * $GETJPI resolve one process's identity to another's row.
 */
static uint32_t assign_vms_pid(void)
{
    struct vms_proc *cur;
    uint32_t candidate;
    int bkt, attempts;

    for (attempts = 0; attempts < 1024; attempts++) {
        bool taken = false;

        candidate = VMS_PID_BASE +
                    (uint32_t)atomic_inc_return(&vms_pid_counter);
        if (candidate == 0)
            continue;   /* 0 is "no process"; never hand it out */

        hash_for_each(vms_proc_hash, bkt, cur, hash_node) {
            if (cur->vms_pid == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken)
            return candidate;
    }
    return 0;   /* table is pathologically full: refuse to register */
}

/*
 * vms_proc_continue_identity - stamp this task's registered VMS parent's
 * identity onto a not-yet-inserted child PCB (vms-4d7, Option B).
 *
 * This is what makes OVMX's fork-per-image invisible to VMS. On OpenVMS an
 * activated image runs IN the current process; OVMX runs it in a fresh
 * forked+exec'd process, so the executive is told (via
 * VMS_IOCTL_REGISTER_CONTINUE, selected by kif_bind when DCL activated the
 * image) to CONTINUE the parent rather than mint a new PCB.
 *
 * THE IDENTITY IS READ FROM THE PARENT'S PCB, NOT FROM THE CALLER. The
 * caller declares only the RELATIONSHIP ("continue my parent"); which
 * process is the parent comes from current->real_parent, which a process
 * cannot forge, and the UIC/username/privileges come from that parent's
 * executive row. This is the same discipline as UIC derivation: a derived
 * fact, never an asserted one -- so it is not the self-declared-identity
 * facade VMS_IOCTL_REGISTER's args were stripped to remove.
 *
 * SECURITY: the parent's CURRENT masks are copied. A parent that
 * setident'd DOWN to an unprivileged identity has reduced masks, so
 * continuing it cannot bring a privilege back -- the reduction now
 * survives image activation, where under the derive-from-capable() path it
 * did not.
 *
 * LOCKING: hash_lock covers uic/username/prcnam/terminal; mode_lock covers
 * the privilege masks. Taken hash-outer then mode-inner, the same order as
 * vms_ioctl_setident(), so an identity is never torn. proc is not yet in
 * the hash, so its own fields need no lock.
 *
 * Returns the parent's VMS PID (nonzero) to be SHARED by this child, or 0
 * when the task has no registered VMS parent -- in which case the caller
 * derives an identity and mints a fresh VMS PID the ordinary way. 0 is not
 * a valid VMS PID (assign_vms_pid never hands it out), so it is an
 * unambiguous "no parent" sentinel.
 */
static uint32_t vms_proc_continue_identity(struct vms_proc *proc)
{
    struct task_struct *rp;
    struct pid *parent_pid = NULL;
    pid_t parent_tgid = 0;
    struct vms_proc *parent;
    uint32_t shared_vms_pid = 0;

    rcu_read_lock();
    rp = rcu_dereference(current->real_parent);
    if (rp) {
        parent_pid  = task_tgid(rp);
        parent_tgid = task_tgid_nr(rp);
    }
    if (!parent_pid) {
        rcu_read_unlock();
        return 0;
    }

    spin_lock(&vms_proc_hash_lock);
    hash_for_each_possible(vms_proc_hash, parent, hash_node, parent_tgid) {
        if (parent->linux_pid != parent_tgid)
            continue;
        /*
         * Recycle-safe: an entry keyed by this pid NUMBER is ours to
         * continue only if it belongs to this same parent INSTANCE. The
         * pinned struct pid is unique per instance; the number is reused.
         * (Same check as vms_proc_find_or_err.)
         */
        if (parent->pid_ref != parent_pid)
            continue;

        proc->uic = parent->uic;
        memcpy(proc->username, parent->username, sizeof(proc->username));
        memcpy(proc->prcnam,   parent->prcnam,   sizeof(proc->prcnam));
        memcpy(proc->terminal, parent->terminal, sizeof(proc->terminal));

        spin_lock(&parent->mode_lock);
        proc->perm_privs = parent->perm_privs;
        proc->cur_privs  = parent->cur_privs;
        spin_unlock(&parent->mode_lock);

        shared_vms_pid = parent->vms_pid;
        break;
    }
    spin_unlock(&vms_proc_hash_lock);
    rcu_read_unlock();

    return shared_vms_pid;
}

/*
 * vms_proc_parent_job_id - the JOB this registration belongs to (vms-aba,
 * LNM$JOB residency), inherited from the DIRECT parent's PCB if it already
 * has one.
 *
 * A VMS job is a top-level process (an interactive login, a detached
 * process) plus every subprocess it creates -- SPAWN's child stays in the
 * parent's job (documented VMS behaviour: DCL Dictionary, SPAWN; System
 * Services Reference, $CREPRC's JOBCTL parameter). This mirrors that
 * inheritance rule directly against the task hierarchy: a task whose real
 * parent is ALREADY a registered VMS process inherits that parent's
 * job_id, whether this registration is an image-activation CONTINUE of the
 * very same VMS process (vms_proc_continue_identity(), just above) or a
 * genuinely new PCB such as a SPAWNed subprocess -- either way the child
 * is part of the parent's job. Returns 0 when the real parent has no
 * registered PCB, which the caller reads as "this task is a job root."
 *
 * Same read-the-parent's-row-never-the-caller's-word discipline as
 * vms_proc_continue_identity(): a process cannot forge which job it
 * belongs to any more than it can forge its own UIC.
 */
static uint32_t vms_proc_parent_job_id(void)
{
    struct task_struct *rp;
    struct pid *parent_pid = NULL;
    pid_t parent_tgid = 0;
    struct vms_proc *parent;
    uint32_t job_id = 0;

    rcu_read_lock();
    rp = rcu_dereference(current->real_parent);
    if (rp) {
        parent_pid  = task_tgid(rp);
        parent_tgid = task_tgid_nr(rp);
    }
    if (!parent_pid) {
        rcu_read_unlock();
        return 0;
    }

    spin_lock(&vms_proc_hash_lock);
    hash_for_each_possible(vms_proc_hash, parent, hash_node, parent_tgid) {
        if (parent->linux_pid != parent_tgid)
            continue;
        /* Recycle-safe: see vms_proc_continue_identity(). */
        if (parent->pid_ref != parent_pid)
            continue;

        job_id = parent->job_id;
        break;
    }
    spin_unlock(&vms_proc_hash_lock);
    rcu_read_unlock();

    return job_id;
}

struct vms_proc *vms_proc_register(pid_t pid, bool continue_identity)
{
    struct vms_proc *existing, *proc;
    uint32_t shared_vms_pid = 0;
    uint32_t parent_job_id;
    int i;

    /*
     * Read before the PCB exists and before any lock this function takes
     * below -- vms_proc_parent_job_id() takes and releases
     * vms_proc_hash_lock itself, the same pattern vms_proc_continue_identity()
     * uses just below.
     */
    parent_job_id = vms_proc_parent_job_id();

    proc = kmem_cache_zalloc(vms_proc_cache, GFP_KERNEL);
    if (!proc)
        return ERR_PTR(-ENOMEM);

    proc->linux_pid = pid;
    proc->current_mode = PSL_C_USER;    /* start in user mode */

    /*
     * Executive-owned identity (vms-8019).
     *
     * The process starts unnamed; $SETPRN (VMS_IOCTL_SETPRN) names it.
     * The UIC is derived here from the task's own credentials -- it is
     * deliberately NOT taken from the register arguments, because a
     * process that can declare its own UIC can forge the group that
     * scopes process-name uniqueness and lookup. UIC [group,member] is
     * packed as (group << 16) | member, the packing sys$getjpi's
     * JPI$_UIC item returns, with OVMX's [gid,uid] mapping.
     */
    proc->prcnam[0] = '\0';
    proc->uic = (((uint32_t)from_kgid(&init_user_ns, current_gid()) & 0xFFFFu) << 16) |
                ((uint32_t)from_kuid(&init_user_ns, current_uid()) & 0xFFFFu);

    /*
     * No user name yet (vms-2b8). A registered process is not an
     * AUTHENTICATED process: registration proves only that a task
     * exists. The name arrives with VMS_IOCTL_SETIDENT, after something
     * that holds privilege has checked SYSUAF -- which is why the
     * executive can report it as an identity rather than a claim.
     */
    memset(proc->username, 0, sizeof(proc->username));

    /*
     * Pin the backing PROCESS's pid so the entry's liveness can be tested
     * without racing pid reuse. task_tgid(), not task_pid(): the entry
     * belongs to the whole thread group and must outlive any one thread
     * in it (see vms_proc_find_or_err). Released in vms_proc_free().
     */
    proc->pid_ref = get_pid(task_tgid(current));

    /*
     * THE AUTHORIZED MASK IS DERIVED, NOT REQUESTED (vms-2b8).
     *
     * This used to clamp a mask the PROCESS supplied. Clamping an
     * attacker-supplied value is still trusting an attacker-supplied
     * value in the unclamped case, and the unclamped case was "the
     * process is CAP_SYS_ADMIN" -- so every privileged process got
     * exactly the privileges it asked for, which is the honor system
     * this item exists to remove. src/ovmx_init/ovmx_init.c asked for
     * 0xFFFFFFFFFFFFFFFF and got it.
     *
     * Now nothing is supplied. capable(CAP_SYS_ADMIN) is a REAL kernel
     * credential read from the task -- a process cannot grant itself
     * CAP_SYS_ADMIN, exactly as it cannot grant itself the uid and gid
     * the UIC above is derived from. That is the whole difference
     * between a derived fact and an asserted one.
     *
     * The privileged case gets the enforced set rather than all 64 bits
     * (CLAUDE.md Rule 10, and this item's constraint): the executive
     * hands out only privileges it will actually refuse an operation
     * over. Privileges beyond that arrive from SYSUAF through
     * VMS_IOCTL_SETIDENT, where they are stored and reported because
     * VMS reports them -- but they are never conjured at registration.
     */
    /*
     * CONTINUE THE PARENT, OR DERIVE (vms-4d7, Option B).
     *
     * When this registration is an IMAGE ACTIVATION continuing an
     * already-registered VMS process (VMS_IOCTL_REGISTER_CONTINUE), the
     * identity above is REPLACED by the parent's -- UIC, user name,
     * process name, terminal and both privilege masks -- and this child
     * SHARES the parent's VMS PID, so to the rest of the system DCL and
     * the image it activated are one VMS process. See
     * vms_proc_continue_identity(); it reads the parent's row, never the
     * caller's word, and copies the parent's CURRENT (possibly reduced)
     * masks so a setident-down cannot be undone by a fork.
     */
    if (continue_identity)
        shared_vms_pid = vms_proc_continue_identity(proc);

    if (shared_vms_pid == 0) {
        proc->perm_privs = capable(CAP_SYS_ADMIN)
                         ? (VMS_PRV_M_ENFORCED | VMS_DEFAULT_PRIVS)
                         : VMS_DEFAULT_PRIVS;
        proc->cur_privs = proc->perm_privs;
    }
    /* image_active/pre_image_mode (vms-68f.iii): kmem_cache_zalloc() above
     * already zeroed both, so a fresh process starts with no controlled
     * descent open, exactly what VMS_IOCTL_IMAGE_RUNDOWN's guard needs. */
    spin_lock_init(&proc->mode_lock);

    /* Initialize AST queues */
    for (i = 0; i < 4; i++) {
        INIT_LIST_HEAD(&proc->ast[i].pending);
        proc->ast[i].count = 0;
        proc->ast[i].enabled = 1;  /* enabled by default */
        spin_lock_init(&proc->ast[i].lock);
    }

    /* Initialize event flags */
    proc->ef.local[0] = 0;
    proc->ef.local[1] = 0;
    proc->ef.common[0] = NULL;
    proc->ef.common[1] = NULL;
    init_waitqueue_head(&proc->ef.waitq);
    spin_lock_init(&proc->ef.lock);

    /* Initialize lock list */
    INIT_LIST_HEAD(&proc->locks);
    proc->lock_count = 0;
    spin_lock_init(&proc->lock_list_lock);

    /* Initialize the I/O channel list (device table, vms-d0b) */
    INIT_LIST_HEAD(&proc->channels);
    proc->next_chan = 0;
    spin_lock_init(&proc->chan_lock);

    /* Mailbox channels (vms-d44) -- a separate list, same chan_lock and
     * next_chan counter as the device channels above (vms_mbx.h). */
    INIT_LIST_HEAD(&proc->mbx_channels);

    /* P0 program region (vms-68f.i): unmapped until VMS_IOCTL_P0_MAP
     * records an extent. kmem_cache_zalloc() above already zeroed
     * p0_base/p0_limit; only the lock needs initializing. */
    spin_lock_init(&proc->p0_lock);

    /* P1 control region (vms-68f.ii): unregistered until VMS_IOCTL_P1_MAP
     * records an extent. Separate lock from p0_lock -- see the p1_lock
     * comment in vms_internal.h for why that separation is the mechanism
     * behind "P0 deleted on rundown, P1 survives", not decoration.
     * kmem_cache_zalloc() above already zeroed p1_base/p1_limit. */
    spin_lock_init(&proc->p1_lock);

    /* Atomically check-and-insert under spinlock to avoid TOCTOU race */
    spin_lock(&vms_proc_hash_lock);
    hash_for_each_possible_rcu(vms_proc_hash, existing, hash_node, pid) {
        if (existing->linux_pid == pid) {
            spin_unlock(&vms_proc_hash_lock);
            put_pid(proc->pid_ref);
            kmem_cache_free(vms_proc_cache, proc);
            return ERR_PTR(-EEXIST);
        }
    }
    /*
     * THE VMS PROCESS ID IS ASSIGNED HERE, UNDER THE SAME LOCK AS THE
     * INSERTION (vms-2b8 round 3). It used to be copied from the
     * register arguments -- i.e. chosen by the process -- with no
     * uniqueness check at all, so two processes could share one VMS PID
     * and $GETJPI by that PID returned whichever the hash walk reached
     * first. Choosing it here, inside the critical section, is what
     * makes "unique among live processes" true rather than likely.
     */
    /*
     * A CONTINUED image activation SHARES its parent's VMS PID rather than
     * minting a new one (vms-4d7): the two Linux tasks are one VMS process,
     * so $GETJPI must resolve either to the same identity. This is not the
     * duplicate-PID defect vms-2b8 round 3 removed -- that was two DIFFERENT
     * identities under one PID; here both rows carry the SAME identity by
     * construction. assign_vms_pid()'s uniqueness scan still protects new
     * processes: it walks the live table, so it will never hand a fresh
     * process a PID a live continued child is already sharing.
     */
    proc->vms_pid = shared_vms_pid ? shared_vms_pid : assign_vms_pid();
    if (proc->vms_pid == 0) {
        spin_unlock(&vms_proc_hash_lock);
        put_pid(proc->pid_ref);
        kmem_cache_free(vms_proc_cache, proc);
        return ERR_PTR(-ENOSPC);
    }
    /*
     * JOB DERIVATION (vms-aba). Inherit the parent's job if it has one;
     * otherwise this registration is a job root and its job IS its own
     * freshly assigned vms_pid -- exactly as a new VMS job's ID is the PID
     * of the process that started it. Finalised here, inside the same
     * critical section as vms_pid, and before hash_add_rcu() publishes the
     * entry, so no reader can observe a zero/unset job_id.
     */
    proc->job_id = parent_job_id ? parent_job_id : proc->vms_pid;
    hash_add_rcu(vms_proc_hash, &proc->hash_node, pid);
    spin_unlock(&vms_proc_hash_lock);

    /*
     * ROUTINE PER-PROCESS TRACE -- pr_debug, NOT pr_info (vms-2213). Every
     * process registration emitted this at pr_info, so a single interactive
     * login (LOGINOUT -> DCL -> each spawned image) sprayed several
     * "vms: registered process ..." lines onto the operator console during
     * the login flow -- kernel-driver chatter real VMS never shows there.
     * This line is routine per-object diagnostic trace, so it belongs at
     * debug level: it stays available through the kernel log (dmesg with
     * dynamic-debug enabled for this module) but does not reach the console.
     * Genuine warnings/errors elsewhere in this module keep pr_warn/pr_err.
     */
    pr_debug("vms: registered process pid=%d vms_pid=0x%08x uic=[%o,%o] job=0x%08x privs=0x%llx (%s)\n",
             pid, proc->vms_pid, proc->uic >> 16, proc->uic & 0xFFFFu,
             proc->job_id, proc->perm_privs, shared_vms_pid ? "continued" : "derived");

    return proc;
}

/*
 * vms_proc_free_claimed - tear down an entry already removed from the hash.
 *
 * The caller must have unlinked proc under vms_proc_hash_lock; that
 * removal IS the ownership claim, so exactly one caller reaches here
 * per entry. Callers that still need to claim go through
 * vms_proc_free().
 */
void vms_proc_free_claimed(struct vms_proc *proc)
{
    int i;
    struct vms_ast_entry *ast, *tmp;

    /* Free AST queues */
    for (i = 0; i < 4; i++) {
        spin_lock(&proc->ast[i].lock);
        list_for_each_entry_safe(ast, tmp, &proc->ast[i].pending, list) {
            list_del(&ast->list);
            kfree(ast);
        }
        spin_unlock(&proc->ast[i].lock);
    }

    /* Release all locks */
    vms_proc_release_locks(proc);

    /* Release common event flag associations */
    vms_proc_release_common_ef(proc);

    /*
     * Give back every I/O channel. The devices themselves belong to
     * the executive and outlive the process; what dies here is this
     * process's claim on them, which is what releases device
     * ownership when the last channel goes.
     */
    vms_proc_release_channels(proc);

    /* Drop the pinned pid reference taken at registration */
    if (proc->pid_ref) {
        put_pid(proc->pid_ref);
        proc->pid_ref = NULL;
    }

    /* RCU-deferred free — proc may still be accessed by RCU readers */
    kfree_rcu(proc, rcu);
}

void vms_proc_free(struct vms_proc *proc)
{
    /*
     * Claim the entry: an entry can now be freed from three places
     * (channel release of an exiting task, the lazy reaper, and module
     * unload), so removal from the hash is what decides ownership.
     * hash_del_rcu() leaves the node unhashed, so a second claimant
     * bails out here instead of double-freeing.
     */
    spin_lock(&vms_proc_hash_lock);
    if (hlist_unhashed(&proc->hash_node)) {
        spin_unlock(&vms_proc_hash_lock);
        return;
    }
    hash_del_rcu(&proc->hash_node);
    spin_unlock(&vms_proc_hash_lock);

    vms_proc_free_claimed(proc);
}

/* ================================================================
 * ioctl dispatch
 * ================================================================ */

static long vms_ioctl_register(unsigned long arg, bool continue_identity)
{
    struct vms_register_args args;
    struct vms_proc *proc;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    /*
     * Clear out entries whose process is gone before claiming a slot,
     * so a recycled pid never collides with a dead predecessor.
     */
    vms_proc_reap_dead();

    /*
     * NOTHING FROM args IS READ. The struct is output-only: the privilege
     * mask went in the first round of vms-2b8 and the VMS process ID went
     * in the third. A registration that takes no input from the process
     * cannot be steered by one.
     *
     * ADOPT, do not recreate, and do not report an error (vms-9fc).
     *
     * This used to answer 0x1C for a task that already had an entry, which
     * made registration a once-per-image operation. It is not: the executive
     * entry belongs to the PROCESS and survives execve(), so the very next
     * image activated in the same process would be told "already registered"
     * and, having nothing else to do with that, would carry on unregistered.
     *
     * ORACLE PIN (reference lab node VAX1, OpenVMS VAX V7.3, 2026-07-30):
     * activating an image inside an existing process does not recreate the
     * process and is not an error. SHOW PROCESS/ACCOUNTING before and after
     * two further image activations reports
     *     Process ID: 2020021D   Process name: "SYSTEM"   Images activated: 19
     *     Process ID: 2020021D   Process name: "SYSTEM"   Images activated: 21
     * -- same PCB, same identity, same connect time, images activated simply
     * counts up. So the VMS-faithful answer to "register a process that
     * already exists" is to hand back the process that already exists --
     * INCLUDING the VMS process ID it was already assigned. Minting a second
     * ID for the same process would make one process answer to two, which is
     * the collision defect vms-2b8 round 3 removed, arriving from the other
     * direction.
     */
    proc = vms_proc_find_or_err();
    if (proc) {
        args.vms_pid = proc->vms_pid;
        args.status = 0x00000001;  /* SS$_NORMAL */
        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;
        return 0;
    }

    /* current->tgid, not current->pid: one PCB per process, shared by
     * every thread in it (see vms_proc_find_or_err). */
    proc = vms_proc_register(current->tgid, continue_identity);
    if (IS_ERR(proc)) {
        if (PTR_ERR(proc) != -EEXIST)
            return PTR_ERR(proc);   /* -ENOMEM -> SS$_INSFMEM at the boundary */

        /*
         * Lost the insert race, or the hash holds an entry for this pid
         * number that is NOT ours (a recycled pid the reaper missed).
         * Re-resolve through the pid-identity check: adopting on the
         * strength of a matching pid NUMBER would hand this task another
         * process's entry, and its privileges with it.
         */
        proc = vms_proc_find_or_err();
        if (!proc)
            return -ESRCH;
    }

    args.vms_pid = proc->vms_pid;
    args.status = 0x00000001;  /* SS$_NORMAL */
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;

    return 0;
}

static long vms_dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct vms_proc *proc;

    /* REGISTER doesn't require an existing proc. REGISTER_CONTINUE is the
     * image-activation variant (vms-4d7): it continues the caller's already-
     * registered VMS parent instead of deriving a fresh identity. */
    if (cmd == VMS_IOCTL_REGISTER)
        return vms_ioctl_register(arg, false);
    if (cmd == VMS_IOCTL_REGISTER_CONTINUE)
        return vms_ioctl_register(arg, true);

    /* All other ioctls require a registered process */
    proc = vms_proc_find_or_err();
    if (!proc)
        return -ESRCH;

    switch (cmd) {
    /* Access mode (3a) */
    case VMS_IOCTL_SETMODE:
        return vms_ioctl_setmode(proc, arg);
    case VMS_IOCTL_GETMODE:
        return vms_ioctl_getmode(proc, arg);
    case VMS_IOCTL_ENTER_IMAGE:
        return vms_ioctl_enter_image(proc, arg);
    case VMS_IOCTL_IMAGE_RUNDOWN:
        return vms_ioctl_image_rundown(proc, arg);
    case VMS_IOCTL_SETPRV:
        return vms_ioctl_setprv(proc, arg);
    case VMS_IOCTL_CHKPRIV:
        return vms_ioctl_chkpriv(proc, arg);

    /* AST delivery (3b) */
    case VMS_IOCTL_DCLAST:
        return vms_ioctl_dclast(proc, arg);
    case VMS_IOCTL_SETAST:
        return vms_ioctl_setast(proc, arg);
    case VMS_IOCTL_DELIVERAST:
        return vms_ioctl_deliverast(proc, arg);

    /* Event flags (3c) */
    case VMS_IOCTL_SETEF:
        return vms_ioctl_setef(proc, arg);
    case VMS_IOCTL_CLREF:
        return vms_ioctl_clref(proc, arg);
    case VMS_IOCTL_WAITFR:
        return vms_ioctl_waitfr(proc, arg);
    case VMS_IOCTL_WFLOR:
        return vms_ioctl_wflor(proc, arg);
    case VMS_IOCTL_WFLAND:
        return vms_ioctl_wfland(proc, arg);
    case VMS_IOCTL_READEF:
        return vms_ioctl_readef(proc, arg);
    case VMS_IOCTL_ASCEFC:
        return vms_ioctl_ascefc(proc, arg);
    case VMS_IOCTL_DACEFC:
        return vms_ioctl_dacefc(proc, arg);
    case VMS_IOCTL_DLCEFC:
        return vms_ioctl_dlcefc(proc, arg);

    /* Lock manager (3d) */
    case VMS_IOCTL_ENQ:
        return vms_ioctl_enq(proc, arg);
    case VMS_IOCTL_DEQ:
        return vms_ioctl_deq(proc, arg);
    case VMS_IOCTL_CONVERT:
        return vms_ioctl_convert(proc, arg);
    case VMS_IOCTL_GETLKI:
        return vms_ioctl_getlki(proc, arg);
    /* DLM resource-directory + mastering readback (vms-ci.5 DB) */
    case VMS_IOCTL_GET_RESMASTER:
        return vms_ioctl_get_resmaster(proc, arg);

    /* Device table (executive-resident I/O database) */
    case VMS_IOCTL_ASSIGN:
        return vms_ioctl_assign(proc, arg);
    case VMS_IOCTL_DASSGN:
        return vms_ioctl_dassgn(proc, arg);
    case VMS_IOCTL_GETDVI:
        return vms_ioctl_getdvi(proc, arg);
    case VMS_IOCTL_DEVSCAN:
        return vms_ioctl_devscan(proc, arg);
    case VMS_IOCTL_TTSETMODE:
        return vms_ioctl_ttsetmode(proc, arg);
    case VMS_IOCTL_ALLOC:
        return vms_ioctl_alloc(proc, arg);
    case VMS_IOCTL_DALLOC:
        return vms_ioctl_dalloc(proc, arg);
    case VMS_IOCTL_DISK_RESOLVE:
        return vms_ioctl_disk_resolve(proc, arg);
    case VMS_IOCTL_SETTERM:
        return vms_ioctl_setterm(proc, arg);

    /* Process table (executive-resident PCB directory) */
    case VMS_IOCTL_SETPRN:
        return vms_ioctl_setprn(proc, arg);
    case VMS_IOCTL_GETJPI:
        return vms_ioctl_getjpi(proc, arg);
    case VMS_IOCTL_PROCSCAN:
        return vms_ioctl_procscan(proc, arg);
    case VMS_IOCTL_SETIDENT:
        return vms_ioctl_setident(proc, arg);
    case VMS_IOCTL_ESTABLISH_SYSTEM:
        return vms_ioctl_establish_system(proc, arg);

    /* Logical name tables (executive-resident LNM$SYSTEM, vms-d37) */
    case VMS_IOCTL_LNM_DEFINE:
        return vms_ioctl_lnm_define(proc, arg);
    case VMS_IOCTL_LNM_DELETE:
        return vms_ioctl_lnm_delete(proc, arg);
    case VMS_IOCTL_LNM_GETSCOPE:
        return vms_ioctl_lnm_getscope(proc, arg);

    /* Mailboxes (executive-resident MBAn:, vms-d44) */
    case VMS_IOCTL_MBX_CREATE:
        return vms_ioctl_mbx_create(proc, arg);
    case VMS_IOCTL_MBX_ASSIGN:
        return vms_ioctl_mbx_assign(proc, arg);
    case VMS_IOCTL_MBX_WRITE:
        return vms_ioctl_mbx_write(proc, arg);
    case VMS_IOCTL_MBX_READ:
        return vms_ioctl_mbx_read(proc, arg);
    case VMS_IOCTL_MBX_DELMBX:
        return vms_ioctl_mbx_delmbx(proc, arg);
    case VMS_IOCTL_MBX_SET_WRTATTN:
        return vms_ioctl_mbx_set_wrtattn(proc, arg);

    /* P0 program region (vms-68f.i, in-process image activation foundation) */
    case VMS_IOCTL_P0_MAP:
        return vms_ioctl_p0_map(proc, arg);
    case VMS_IOCTL_P0_UNMAP:
        return vms_ioctl_p0_unmap(proc, arg);

    /* P1 control region (vms-68f.ii, same design, increment (ii)) */
    case VMS_IOCTL_P1_MAP:
        return vms_ioctl_p1_map(proc, arg);

    default:
        return -ENOTTY;
    }
}

static int vms_dev_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static int vms_dev_release(struct inode *inode, struct file *filp)
{
    struct vms_proc *proc;

    /*
     * The executive PCB belongs to the PROCESS, not to the channel
     * (vms-8019). Closing /dev/vms -- including the implicit close of
     * an inherited descriptor at execve() time -- must not delete the
     * process from the executive's process table, or the process name
     * would not survive image activation and would once again be
     * something only the current image can see.
     *
     * So the entry is destroyed here only when the task that owns it is
     * actually going away. Entries whose task exits without ever
     * reaching this path (a forked child sharing the parent's struct
     * file, for instance) are reclaimed by vms_proc_reap_dead().
     *
     * AND ONLY WHEN THE WHOLE THREAD GROUP IS GOING AWAY (vms-2b8). The
     * entry is keyed by tgid and shared by every thread, so an exiting
     * worker thread that happens to hold a channel must not delete the
     * PCB out from under the threads still running: on VMS a thread
     * terminating does not delete the process. thread_group_empty() is
     * true only for the last thread standing, which is the point at
     * which the VMS process really is ending.
     */
    if (!(current->flags & PF_EXITING) || !thread_group_empty(current))
        return 0;

    proc = vms_proc_find_or_err();
    if (proc)
        vms_proc_free(proc);

    return 0;
}

/*
 * vms_lnm_mmap - hand userspace a READ-ONLY view of the executive-resident
 * logical-name arena (vms-d37; the CHAR-DEVICE half of the arena seam, vms-d61).
 *
 * The arena itself is owned, allocated and written by the substrate-agnostic
 * facility (src/kernel-core/vms_lnm.c) through the exec_arena seam
 * (exec_kbackend.h §10). The MMAP-TIME MAPPING here -- Linux mm machinery:
 * remap_vmalloc_range and clearing VM_MAYWRITE -- is host char-device glue and
 * stays in the Linux module rind, NOT in the portable facility (design record
 * docs/design-netbsd-executive-core.md §2, the host-mm coupling stays in the
 * rind exactly like registration does). This handler asks the facility for the
 * arena base+size (vms_lnm_arena_base/_size) and does the Linux mapping itself;
 * the facility never names struct vm_area_struct, remap_vmalloc_range, VM_* or
 * PAGE_*.
 *
 * The mapping is read-only and VM_MAYWRITE is cleared so mprotect() cannot
 * turn it writable afterwards -- this is the direct analogue of VMS
 * protecting system space by processor access mode (design §2.4). The MMU,
 * not a convention, is what stops a process corrupting the system logical
 * name table.
 */
static int vms_lnm_mmap(struct file *filp, struct vm_area_struct *vma)
{
    void *base = vms_lnm_arena_base();
    unsigned long size = vma->vm_end - vma->vm_start;
    int ret;

    (void)filp;

    if (!base)
        return -ENODEV;

    /* Only the arena, only from its start. */
    if (vma->vm_pgoff != (VMS_LNM_MMAP_OFFSET >> PAGE_SHIFT))
        return -EINVAL;
    if (size > PAGE_ALIGN(vms_lnm_arena_size()))
        return -EINVAL;

    /* Reject any write intent, now and forever. */
    if (vma->vm_flags & VM_WRITE)
        return -EACCES;
    vm_flags_clear(vma, VM_MAYWRITE);

    ret = remap_vmalloc_range(vma, base, 0);
    if (ret < 0)
        return ret;

    return 0;
}

static const struct file_operations vms_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = vms_dev_ioctl,
    .mmap           = vms_lnm_mmap,      /* read-only logical-name arena (vms-d37) */
    .open           = vms_dev_open,
    .release        = vms_dev_release,
};

/*
 * THE EXECUTIVE ENTRY POINT IS NOT PRIVILEGE-GATED (vms-2b8).
 *
 * miscdevice with no .mode creates the node 0600 root:root, which meant NO
 * UNPRIVILEGED PROCESS COULD REACH THE EXECUTIVE AT ALL. That is not a
 * conservative default here, it is a different system: with a 0600 door,
 * every reachable caller is root, multi-user OVMX cannot exist, and the
 * per-service privilege checks this module enforces are unreachable in the
 * product even though they are demonstrable in a test.
 *
 * VMS SEMANTICS THIS MATCHES (CLAUDE.md Rule 10, answer 1). On OpenVMS the
 * system-service entry sequence -- the change-mode-to-kernel/exec instruction
 * that the $-service jackets execute -- is an UNPRIVILEGED instruction
 * available to every process at every access mode. There is no permission on
 * "may I call the executive". Access control lives INSIDE each service, which
 * validates the caller's privilege mask and returns SS$_NOPRIV. That is
 * exactly the shape this module already has (vms_ioctl_setident,
 * vms_ioctl_setprv, vms_access_check), so the door must be open for the
 * checks behind it to be the thing that decides.
 *
 * THE ANSWER TO THE QUESTION THIS COMMENT WAS ASKED (round 3): YES,
 * unprivileged processes SHOULD be able to open /dev/vms, and therefore the
 * per-service checks are load-bearing security, not defence in depth. That
 * has a cost paid in this same change: when the door was opened in round 2,
 * vms_ioctl_getjpi() and vms_ioctl_procscan() had NO caller check, so an
 * unprivileged process could read the user name, UIC and privilege mask of
 * every process on the system -- the premise that "access control lives
 * inside each service" was false of the two services the open door newly
 * exposed. vms_proc_may_read() is that missing check, and its rule is
 * measured on the oracle rather than assumed (see vms_ioctl.h).
 *
 * OVMX DESIGN CHOICE, labelled as such (CLAUDE.md Rule 8): /dev/vms is an
 * OVMX construct with no VMS counterpart, so no public OpenVMS document
 * publishes a mode for it. 0666 is chosen as the closest Linux expression of
 * "every process may enter the executive"; it is not presented as
 * VMS-authentic.
 *
 * KNOWN CONSEQUENCE, deliberately not handled here and reported to the
 * security review (vms-cb5): an unprivileged process may now consume
 * executive memory -- process entries, locks, event flags, queued ASTs --
 * with no bound. VMS bounds exactly this with per-process quotas (BYTLM,
 * ENQLM, ASTLM) charged from SYSUAF. OVMX has no quota system yet, so this
 * enlarges a local denial-of-service surface. Adding an arbitrary in-module
 * cap would be the illegal third answer (Rule 10): VMS's answer is quotas,
 * and quotas are the item to write, not a limit invented here.
 */
static struct miscdevice vms_misc = {
    .minor  = MISC_DYNAMIC_MINOR,
    .name   = "vms",
    .fops   = &vms_fops,
    .mode   = 0666,
};

/* ================================================================
 * Module init / exit
 * ================================================================ */

static int __init vms_init(void)
{
    int ret;

    pr_info("vms: initializing VMS kernel module\n");

    /* Create slab cache for process structs */
    vms_proc_cache = kmem_cache_create("vms_proc",
                                        sizeof(struct vms_proc),
                                        0, SLAB_HWCACHE_ALIGN, NULL);
    if (!vms_proc_cache) {
        pr_err("vms: failed to create process slab cache\n");
        return -ENOMEM;
    }

    /* Initialize subsystems */
    hash_init(vms_proc_hash);
    ret = vms_lock_init();
    if (ret) {
        pr_err("vms: failed to initialize lock manager: %d\n", ret);
        kmem_cache_destroy(vms_proc_cache);
        return ret;
    }
    vms_eflag_init();

    /*
     * Bring up the device table before /dev/vms exists, so that the
     * console terminal is in the executive's I/O database before any
     * process can possibly ask about it -- a device is never something
     * a process introduces.
     */
    ret = vms_devtab_init();
    if (ret) {
        pr_err("vms: failed to initialize device table: %d\n", ret);
        vms_lock_cleanup();
        vms_eflag_cleanup();
        kmem_cache_destroy(vms_proc_cache);
        return ret;
    }

    /*
     * Announce the SYSTEM identity constant before /dev/vms exists, for the
     * same reason the device table just did (vms-a17e): VMS_SYSTEM_UIC and
     * VMS_PRV_M_SYSTEM_ALL (vms_internal.h) are facts the executive owns
     * from module load, not values any process registers or supplies.
     * vms_ioctl_establish_system() stamps them onto a process later, on
     * request (PROVISION.EXE, at boot) -- this line is the proof that what
     * gets stamped was already true before that process, or SYSUAF, existed
     * to the executive at all.
     */
    pr_info("vms: system identity constant SYSTEM [%o,%o] privileges=ALL established by the executive\n",
            VMS_SYSTEM_UIC >> 16, VMS_SYSTEM_UIC & 0xFFFFu);

    /*
     * Bring up the logical-name arena before /dev/vms exists, for the same
     * reason as the device table: the executive-resident tables are a
     * property of the node, present before any process can ask (vms-d37).
     */
    ret = vms_lnm_init();
    if (ret) {
        pr_err("vms: failed to initialize logical-name arena: %d\n", ret);
        vms_devtab_cleanup();
        vms_lock_cleanup();
        vms_eflag_cleanup();
        kmem_cache_destroy(vms_proc_cache);
        return ret;
    }

    /* Mailbox table (vms-d44) -- starts empty, nothing that can fail here
     * (see vms_mbx.c's vms_mbx_init()). */
    vms_mbx_init();

    /* Register /dev/vms */
    ret = misc_register(&vms_misc);
    if (ret) {
        pr_err("vms: failed to register /dev/vms: %d\n", ret);
        vms_mbx_cleanup();
        vms_lnm_cleanup();
        vms_devtab_cleanup();
        vms_lock_cleanup();
        vms_eflag_cleanup();
        kmem_cache_destroy(vms_proc_cache);
        return ret;
    }

    pr_info("vms: /dev/vms registered successfully\n");
    return 0;
}

static void __exit vms_exit(void)
{
    struct vms_proc *proc;
    struct hlist_node *tmp;
    int bkt;

    pr_info("vms: unloading VMS kernel module\n");

    /* Unregister device */
    misc_deregister(&vms_misc);

    /* Free all process state (vms_proc_free handles sub-objects) */
    hash_for_each_safe(vms_proc_hash, bkt, tmp, proc, hash_node) {
        vms_proc_free(proc);
    }
    /* Wait for RCU callbacks to complete before destroying the cache */
    rcu_barrier();

    /* Cleanup subsystems */
    vms_lock_cleanup();
    vms_eflag_cleanup();
    vms_devtab_cleanup();
    vms_lnm_cleanup();
    vms_mbx_cleanup();

    kmem_cache_destroy(vms_proc_cache);

    pr_info("vms: module unloaded\n");
}

module_init(vms_init);
module_exit(vms_exit);
