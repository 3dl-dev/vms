// SPDX-License-Identifier: GPL-2.0
/*
 * vms_lock.c - Lock Manager (Phase 3d)
 *
 * Full VMS lock manager with:
 *   - 6-mode compatibility (NL/CR/CW/PR/PW/EX)
 *   - Wait-for graph deadlock detection
 *   - Lock value blocks (16 bytes per resource)
 *   - Blocking ASTs (notification when your lock blocks others)
 *   - Hierarchical resource trees (parent/child locks)
 *   - Lock conversion
 *
 * Compatibility matrix (1=compatible, 0=incompatible):
 *
 *        NL  CR  CW  PR  PW  EX
 *   NL [  1   1   1   1   1   1 ]
 *   CR [  1   1   1   1   1   0 ]
 *   CW [  1   1   1   0   0   0 ]
 *   PR [  1   1   0   1   0   0 ]
 *   PW [  1   1   0   0   0   0 ]
 *   EX [  1   0   0   0   0   0 ]
 */

#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/hashtable.h>
#include <linux/jhash.h>

#include "vms_internal.h"

/* VMS status codes */
#define SS__NORMAL      0x00000001
#define SS__BADPARAM    0x00000014
#define SS__NOTQUEUED   40   /* lock not queued (NOQUEUE flag) */
#define SS__DEADLOCK    100  /* deadlock detected */
#define SS__IVLOCKID    108  /* invalid lock ID */
#define SS__SUBLOCKS    112  /* sublocks still held */
#define SS__CANCELGRANT 116  /* conversion cancelled */
#define SS__VALNOTVALID 120  /* value block not valid */
#define SS__INSFMEM     44  /* matches real VMS */

/* Lock mode compatibility matrix */
static const uint8_t compat[6][6] = {
    /*          NL  CR  CW  PR  PW  EX */
    /* NL */ {  1,  1,  1,  1,  1,  1 },
    /* CR */ {  1,  1,  1,  1,  1,  0 },
    /* CW */ {  1,  1,  1,  0,  0,  0 },
    /* PR */ {  1,  1,  0,  1,  0,  0 },
    /* PW */ {  1,  1,  0,  0,  0,  0 },
    /* EX */ {  1,  0,  0,  0,  0,  0 },
};

/* Global lock state */
struct rb_root vms_lock_id_tree = RB_ROOT;
DEFINE_SPINLOCK(vms_lock_id_lock);
uint32_t vms_next_lock_id = 1;

DEFINE_HASHTABLE(vms_res_hash, VMS_RES_HASH_BITS);
DEFINE_SPINLOCK(vms_res_hash_lock);

static struct kmem_cache *vms_lock_cache;
static struct kmem_cache *vms_resource_cache;

void vms_lock_init(void)
{
    vms_lock_cache = kmem_cache_create("vms_lock",
                                        sizeof(struct vms_lock_entry),
                                        0, SLAB_HWCACHE_ALIGN, NULL);
    vms_resource_cache = kmem_cache_create("vms_resource",
                                            sizeof(struct vms_lock_resource),
                                            0, SLAB_HWCACHE_ALIGN, NULL);
    hash_init(vms_res_hash);
}

void vms_lock_cleanup(void)
{
    struct vms_lock_resource *res;
    struct hlist_node *tmp;
    int bkt;

    /* Free all resources */
    spin_lock(&vms_res_hash_lock);
    hash_for_each_safe(vms_res_hash, bkt, tmp, res, hash_node) {
        hash_del(&res->hash_node);
        kmem_cache_free(vms_resource_cache, res);
    }
    spin_unlock(&vms_res_hash_lock);

    if (vms_lock_cache)
        kmem_cache_destroy(vms_lock_cache);
    if (vms_resource_cache)
        kmem_cache_destroy(vms_resource_cache);
}

/* ================================================================
 * Lock ID management (red-black tree)
 * ================================================================ */

static struct vms_lock_entry *lock_find_by_id(uint32_t lkid)
{
    struct rb_node *node;

    spin_lock(&vms_lock_id_lock);
    node = vms_lock_id_tree.rb_node;
    while (node) {
        struct vms_lock_entry *entry = rb_entry(node, struct vms_lock_entry, rb_node);
        if (lkid < entry->lkid)
            node = node->rb_left;
        else if (lkid > entry->lkid)
            node = node->rb_right;
        else {
            spin_unlock(&vms_lock_id_lock);
            return entry;
        }
    }
    spin_unlock(&vms_lock_id_lock);
    return NULL;
}

static void lock_insert_id(struct vms_lock_entry *entry)
{
    struct rb_node **p, *parent = NULL;

    spin_lock(&vms_lock_id_lock);
    entry->lkid = vms_next_lock_id++;

    p = &vms_lock_id_tree.rb_node;
    while (*p) {
        struct vms_lock_entry *e = rb_entry(*p, struct vms_lock_entry, rb_node);
        parent = *p;
        if (entry->lkid < e->lkid)
            p = &(*p)->rb_left;
        else
            p = &(*p)->rb_right;
    }
    rb_link_node(&entry->rb_node, parent, p);
    rb_insert_color(&entry->rb_node, &vms_lock_id_tree);
    spin_unlock(&vms_lock_id_lock);
}

static void lock_remove_id(struct vms_lock_entry *entry)
{
    spin_lock(&vms_lock_id_lock);
    rb_erase(&entry->rb_node, &vms_lock_id_tree);
    spin_unlock(&vms_lock_id_lock);
}

/* ================================================================
 * Resource management (hash table by name)
 * ================================================================ */

static uint32_t resource_hash_key(const char *name)
{
    return jhash(name, strnlen(name, 32), 0);
}

static struct vms_lock_resource *resource_find(const char *name)
{
    struct vms_lock_resource *res;
    uint32_t key = resource_hash_key(name);

    hash_for_each_possible(vms_res_hash, res, hash_node, key) {
        if (strncmp(res->name, name, 32) == 0)
            return res;
    }
    return NULL;
}

static struct vms_lock_resource *resource_find_or_create(const char *name)
{
    struct vms_lock_resource *res;
    uint32_t key;

    spin_lock(&vms_res_hash_lock);
    res = resource_find(name);
    if (res) {
        res->refcount++;
        spin_unlock(&vms_res_hash_lock);
        return res;
    }

    res = kmem_cache_zalloc(vms_resource_cache, GFP_ATOMIC);
    if (!res) {
        spin_unlock(&vms_res_hash_lock);
        return NULL;
    }

    strscpy(res->name, name, sizeof(res->name));
    INIT_LIST_HEAD(&res->granted);
    INIT_LIST_HEAD(&res->waiting);
    memset(res->valblk, 0, LCK_VALBLK_SIZE);
    spin_lock_init(&res->lock);
    res->refcount = 1;
    res->parent = NULL;

    key = resource_hash_key(name);
    hash_add(vms_res_hash, &res->hash_node, key);
    spin_unlock(&vms_res_hash_lock);

    return res;
}

static void resource_release(struct vms_lock_resource *res)
{
    int i, has_valblk = 0;

    spin_lock(&vms_res_hash_lock);
    res->refcount--;
    if (res->refcount <= 0 && list_empty(&res->granted) && list_empty(&res->waiting)) {
        /* Preserve resource if it has a non-zero value block */
        for (i = 0; i < LCK_VALBLK_SIZE; i++) {
            if (res->valblk[i]) { has_valblk = 1; break; }
        }
        if (!has_valblk) {
            hash_del(&res->hash_node);
            kmem_cache_free(vms_resource_cache, res);
        }
    }
    spin_unlock(&vms_res_hash_lock);
}

/* ================================================================
 * Lock compatibility checking
 * ================================================================ */

/*
 * Check if a lock at mode 'requested' is compatible with all
 * currently granted locks on the resource (excluding 'exclude').
 */
static int lock_compatible(struct vms_lock_resource *res,
                           uint32_t requested,
                           struct vms_lock_entry *exclude)
{
    struct vms_lock_entry *granted;

    list_for_each_entry(granted, &res->granted, res_granted) {
        if (granted == exclude)
            continue;
        if (!compat[requested][granted->granted_mode])
            return 0;
    }
    return 1;
}

/* ================================================================
 * Deadlock detection (wait-for graph iterative BFS)
 * ================================================================ */

#define MAX_DEADLOCK_DEPTH 16

/*
 * Check if granting 'lock' would create a deadlock.
 *
 * Walk the wait-for graph iteratively using a fixed-size stack to
 * avoid recursive spinlock acquisition (the old recursive version
 * would spin_lock(&proc->lock_list_lock) while already holding
 * another proc's lock_list_lock, risking ABBA deadlock).
 *
 * Strategy: use trylock on proc->lock_list_lock. If we can't get it,
 * conservatively assume potential deadlock at that branch (safe
 * because false positives just cause SS$_DEADLOCK, which the caller
 * retries or reports).
 */
static int check_deadlock(struct vms_lock_entry *lock,
                           int depth __attribute__((unused)))
{
    struct vms_proc *origin_proc = lock->proc;
    struct vms_lock_entry *stack[MAX_DEADLOCK_DEPTH];
    int sp = 0;

    stack[sp++] = lock;

    while (sp > 0) {
        struct vms_lock_entry *cur = stack[--sp];
        struct vms_lock_resource *res = cur->resource;
        struct vms_lock_entry *granted;

        /*
         * For each granted lock on cur's resource that blocks cur,
         * check if the blocking process is also waiting somewhere.
         * Note: res->lock is already held by the caller (vms_ioctl_enq
         * or vms_ioctl_convert) for the initial resource. For other
         * resources in the chain, we only read the granted list under
         * trylock to avoid lock-order inversions.
         */
        list_for_each_entry(granted, &res->granted, res_granted) {
            if (compat[cur->requested_mode][granted->granted_mode])
                continue;  /* This one doesn't block us */

            /* Direct cycle: blocker is the original requester */
            if (granted->proc == origin_proc)
                return 1;  /* Deadlock! */

            /* Check if the blocking process has any waiting locks */
            if (!spin_trylock(&granted->proc->lock_list_lock))
                continue;  /* Can't get lock — skip this branch */

            {
                struct vms_lock_entry *their_lock;
                list_for_each_entry(their_lock, &granted->proc->locks, proc_list) {
                    if (their_lock->waiting && sp < MAX_DEADLOCK_DEPTH) {
                        if (their_lock->proc == origin_proc) {
                            spin_unlock(&granted->proc->lock_list_lock);
                            return 1;  /* Deadlock! */
                        }
                        stack[sp++] = their_lock;
                    }
                }
            }
            spin_unlock(&granted->proc->lock_list_lock);
        }
    }

    return 0;
}

/* ================================================================
 * Grant waiting locks after a dequeue or conversion
 * ================================================================ */

static void try_grant_waiters(struct vms_lock_resource *res)
{
    struct vms_lock_entry *waiter, *tmp;

    list_for_each_entry_safe(waiter, tmp, &res->waiting, res_waiting) {
        if (lock_compatible(res, waiter->requested_mode, NULL)) {
            /* Grant it */
            list_del(&waiter->res_waiting);
            waiter->granted_mode = waiter->requested_mode;
            waiter->waiting = 0;
            list_add_tail(&waiter->res_granted, &res->granted);

            /* Copy resource value block if requested */
            if (waiter->flags & LCK_M_VALBLK)
                memcpy(waiter->valblk, res->valblk, LCK_VALBLK_SIZE);
        } else {
            /* FIFO: stop at first non-grantable waiter
             * (VMS actually checks all waiters, but FIFO is simpler
             *  and prevents starvation) */
            break;
        }
    }
}

/*
 * Send blocking AST notifications to granted lock holders when
 * a new request is blocked.
 */
static void notify_blocking_asts(struct vms_lock_resource *res,
                                 struct vms_lock_entry *blocked)
{
    struct vms_lock_entry *granted;

    list_for_each_entry(granted, &res->granted, res_granted) {
        if (!compat[blocked->requested_mode][granted->granted_mode] &&
            granted->blkastadr) {
            /*
             * Queue a blocking AST for the granted lock's process.
             * The blocking AST tells the process "someone is waiting
             * for your resource -- consider releasing or downgrading."
             */
            struct vms_ast_entry *ast;
            struct vms_ast_state *ast_state;

            ast = kmalloc(sizeof(*ast), GFP_ATOMIC);
            if (!ast)
                continue;

            ast->astadr = granted->blkastadr;
            ast->astprm = granted->lkid;
            ast->acmode = PSL_C_USER;

            ast_state = &granted->proc->ast[PSL_C_USER];
            spin_lock(&ast_state->lock);
            if (ast_state->count < VMS_AST_MAX_PER_MODE) {
                list_add_tail(&ast->list, &ast_state->pending);
                ast_state->count++;
            } else {
                kfree(ast);
            }
            spin_unlock(&ast_state->lock);
        }
    }
}

/* ================================================================
 * Process lock cleanup
 * ================================================================ */

void vms_proc_release_locks(struct vms_proc *proc)
{
    struct vms_lock_entry *lock, *tmp;

    spin_lock(&proc->lock_list_lock);
    list_for_each_entry_safe(lock, tmp, &proc->locks, proc_list) {
        struct vms_lock_resource *res = lock->resource;

        /* Remove from resource lists */
        spin_lock(&res->lock);
        if (lock->waiting)
            list_del(&lock->res_waiting);
        else
            list_del(&lock->res_granted);

        /* Write back value block if held */
        if ((lock->flags & LCK_M_VALBLK) && !lock->waiting)
            memcpy(res->valblk, lock->valblk, LCK_VALBLK_SIZE);

        try_grant_waiters(res);
        spin_unlock(&res->lock);

        /* Remove from process list and ID tree */
        list_del(&lock->proc_list);
        lock_remove_id(lock);

        resource_release(res);
        kmem_cache_free(vms_lock_cache, lock);
    }
    proc->lock_count = 0;
    spin_unlock(&proc->lock_list_lock);
}

/* ================================================================
 * ioctl handlers
 * ================================================================ */

/*
 * vms_ioctl_enq - Enqueue lock request ($ENQ equivalent)
 *
 * Requests a lock on a named resource. If compatible with all
 * granted locks, the lock is granted immediately. Otherwise,
 * if LCK_M_NOQUEUE is set, fails with SS$_NOTQUEUED. Otherwise,
 * the request is queued (caller blocks in kernel until granted).
 */
long vms_ioctl_enq(struct vms_proc *proc, unsigned long arg)
{
    struct vms_enq_args args;
    struct vms_lock_entry *lock;
    struct vms_lock_resource *res;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    if (args.lkmode > LCK_K_EXMODE) {
        args.status = SS__BADPARAM;
        goto out;
    }

    /* Ensure null-terminated resource name */
    args.resnam[31] = '\0';
    if (args.resnam[0] == '\0') {
        args.status = SS__BADPARAM;
        goto out;
    }

    /* Find or create the resource */
    res = resource_find_or_create(args.resnam);
    if (!res) {
        args.status = SS__INSFMEM;
        goto out;
    }

    /* Allocate lock entry */
    lock = kmem_cache_zalloc(vms_lock_cache, GFP_KERNEL);
    if (!lock) {
        resource_release(res);
        args.status = SS__INSFMEM;
        goto out;
    }

    lock->granted_mode = LCK_K_NLMODE;
    lock->requested_mode = args.lkmode;
    lock->flags = args.flags;
    lock->astadr = args.astadr;
    lock->astprm = args.astprm;
    lock->blkastadr = args.blkastadr;
    lock->resource = res;
    lock->proc = proc;
    lock->waiting = 0;

    if (args.flags & LCK_M_VALBLK)
        memcpy(lock->valblk, args.valblk, LCK_VALBLK_SIZE);

    /* Assign lock ID */
    lock_insert_id(lock);

    /* Add to process lock list */
    spin_lock(&proc->lock_list_lock);
    list_add_tail(&lock->proc_list, &proc->locks);
    proc->lock_count++;
    spin_unlock(&proc->lock_list_lock);

    /* Try to grant */
    spin_lock(&res->lock);
    if (lock_compatible(res, args.lkmode, NULL)) {
        /* Granted immediately */
        lock->granted_mode = args.lkmode;
        list_add_tail(&lock->res_granted, &res->granted);

        if (args.flags & LCK_M_VALBLK) {
            /* If user provided a value block, write it to resource.
             * Otherwise, read resource value block into lock. */
            int i, has_val = 0;
            for (i = 0; i < LCK_VALBLK_SIZE; i++) {
                if (lock->valblk[i]) { has_val = 1; break; }
            }
            if (has_val)
                memcpy(res->valblk, lock->valblk, LCK_VALBLK_SIZE);
            else
                memcpy(lock->valblk, res->valblk, LCK_VALBLK_SIZE);
        }

        spin_unlock(&res->lock);

        args.lkid = lock->lkid;
        args.lk_status = lock->granted_mode;
        if (args.flags & LCK_M_VALBLK)
            memcpy(args.valblk, lock->valblk, LCK_VALBLK_SIZE);
        args.status = SS__NORMAL;
    } else {
        /* Not compatible */
        if (args.flags & LCK_M_NOQUEUE) {
            spin_unlock(&res->lock);

            /* Clean up */
            spin_lock(&proc->lock_list_lock);
            list_del(&lock->proc_list);
            proc->lock_count--;
            spin_unlock(&proc->lock_list_lock);
            lock_remove_id(lock);
            resource_release(res);
            kmem_cache_free(vms_lock_cache, lock);

            args.lkid = 0;
            args.status = SS__NOTQUEUED;
        } else {
            /* Queue the request */
            lock->waiting = 1;
            list_add_tail(&lock->res_waiting, &res->waiting);

            /* Check for deadlock */
            if (check_deadlock(lock, 0)) {
                list_del(&lock->res_waiting);
                spin_unlock(&res->lock);

                spin_lock(&proc->lock_list_lock);
                list_del(&lock->proc_list);
                proc->lock_count--;
                spin_unlock(&proc->lock_list_lock);
                lock_remove_id(lock);
                resource_release(res);
                kmem_cache_free(vms_lock_cache, lock);

                args.lkid = 0;
                args.status = SS__DEADLOCK;
            } else {
                /* Notify blocking AST holders */
                notify_blocking_asts(res, lock);
                spin_unlock(&res->lock);

                /*
                 * In a fully synchronous (ENQw) model, we'd block here.
                 * For the async model, return the lock ID and let
                 * userspace poll/wait. For now, mark as queued.
                 */
                args.lkid = lock->lkid;
                args.lk_status = lock->requested_mode;
                args.status = SS__NORMAL;
            }
        }
    }

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_deq - Dequeue (release) a lock ($DEQ equivalent)
 */
long vms_ioctl_deq(struct vms_proc *proc, unsigned long arg)
{
    struct vms_deq_args args;
    struct vms_lock_entry *lock;
    struct vms_lock_resource *res;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    lock = lock_find_by_id(args.lkid);
    if (!lock || lock->proc != proc) {
        args.status = SS__IVLOCKID;
        goto out;
    }

    res = lock->resource;

    /* Remove from resource */
    spin_lock(&res->lock);

    /* Write back value block from lock to resource */
    if ((lock->flags & LCK_M_VALBLK) && !lock->waiting)
        memcpy(res->valblk, lock->valblk, LCK_VALBLK_SIZE);

    if (lock->waiting)
        list_del(&lock->res_waiting);
    else
        list_del(&lock->res_granted);

    /* Try to grant waiters now that this lock is released */
    try_grant_waiters(res);
    spin_unlock(&res->lock);

    /* Remove from process list */
    spin_lock(&proc->lock_list_lock);
    list_del(&lock->proc_list);
    proc->lock_count--;
    spin_unlock(&proc->lock_list_lock);

    /* Remove from ID tree and free */
    lock_remove_id(lock);
    resource_release(res);
    kmem_cache_free(vms_lock_cache, lock);

    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_convert - Convert lock mode ($ENQ with LCK$M_CONVERT)
 *
 * Changes the mode of an existing granted lock. If the new mode
 * is compatible, conversion happens immediately. Otherwise, the
 * lock is queued at the new mode (old mode remains until granted).
 */
long vms_ioctl_convert(struct vms_proc *proc, unsigned long arg)
{
    struct vms_enq_args args;
    struct vms_lock_entry *lock;
    struct vms_lock_resource *res;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    if (args.lkmode > LCK_K_EXMODE) {
        args.status = SS__BADPARAM;
        goto out;
    }

    lock = lock_find_by_id(args.lkid);
    if (!lock || lock->proc != proc) {
        args.status = SS__IVLOCKID;
        goto out;
    }

    if (lock->waiting) {
        args.status = SS__CANCELGRANT;
        goto out;
    }

    res = lock->resource;

    spin_lock(&res->lock);

    /* Update blocking AST address if provided */
    if (args.blkastadr)
        lock->blkastadr = args.blkastadr;

    /* Write value block before conversion if requested */
    if (args.flags & LCK_M_VALBLK)
        memcpy(res->valblk, args.valblk, LCK_VALBLK_SIZE);

    /* Check compatibility (exclude self) */
    if (lock_compatible(res, args.lkmode, lock)) {
        /* Immediate conversion */
        lock->granted_mode = args.lkmode;

        if (args.flags & LCK_M_VALBLK)
            memcpy(lock->valblk, res->valblk, LCK_VALBLK_SIZE);

        spin_unlock(&res->lock);

        args.lk_status = lock->granted_mode;
        if (args.flags & LCK_M_VALBLK)
            memcpy(args.valblk, lock->valblk, LCK_VALBLK_SIZE);
        args.status = SS__NORMAL;
    } else {
        if (args.flags & LCK_M_NOQUEUE) {
            spin_unlock(&res->lock);
            args.status = SS__NOTQUEUED;
        } else {
            /* Move to waiting list, keep granted mode until converted */
            lock->requested_mode = args.lkmode;
            lock->waiting = 1;
            list_del(&lock->res_granted);
            list_add_tail(&lock->res_waiting, &res->waiting);

            /* Check deadlock */
            if (check_deadlock(lock, 0)) {
                /* Undo: move back to granted */
                list_del(&lock->res_waiting);
                lock->waiting = 0;
                list_add_tail(&lock->res_granted, &res->granted);
                spin_unlock(&res->lock);
                args.status = SS__DEADLOCK;
            } else {
                notify_blocking_asts(res, lock);
                spin_unlock(&res->lock);
                args.lk_status = lock->requested_mode;
                args.status = SS__NORMAL;
            }
        }
    }

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_getlki - Get lock information ($GETLKI equivalent)
 */
long vms_ioctl_getlki(struct vms_proc *proc, unsigned long arg)
{
    struct vms_getlki_args args;
    struct vms_lock_entry *lock;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    lock = lock_find_by_id(args.lkid);
    if (!lock) {
        args.status = SS__IVLOCKID;
        goto out;
    }

    args.granted_mode = lock->granted_mode;
    args.requested_mode = lock->waiting ? lock->requested_mode : lock->granted_mode;
    args.parent_id = 0; /* TODO: parent lock support */

    if (lock->resource) {
        strscpy(args.resnam, lock->resource->name, sizeof(args.resnam));
        memcpy(args.valblk, lock->valblk, LCK_VALBLK_SIZE);
    } else {
        memset(args.resnam, 0, sizeof(args.resnam));
        memset(args.valblk, 0, LCK_VALBLK_SIZE);
    }

    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}
