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

struct vms_proc *vms_proc_find_or_err(void)
{
    struct vms_proc *proc = vms_proc_find(current->pid);
    return proc;
}

struct vms_proc *vms_proc_register(pid_t pid, uint32_t vms_pid, uint64_t init_privs)
{
    struct vms_proc *proc;
    int i;

    /* Check if already registered */
    if (vms_proc_find(pid))
        return ERR_PTR(-EEXIST);

    proc = kmem_cache_zalloc(vms_proc_cache, GFP_KERNEL);
    if (!proc)
        return ERR_PTR(-ENOMEM);

    proc->linux_pid = pid;
    proc->vms_pid = vms_pid;
    proc->current_mode = PSL_C_USER;    /* start in user mode */

    /*
     * Privilege escalation guard: only CAP_SYS_ADMIN processes may request
     * arbitrary privileges. Non-privileged processes are clamped to the safe
     * default set (TMPMBX | NETMBX) regardless of what they pass in.
     */
    if (!capable(CAP_SYS_ADMIN)) {
        /* Non-root processes get a restricted default privilege set */
        init_privs &= VMS_DEFAULT_PRIVS;
    }
    proc->cur_privs = init_privs;
    proc->perm_privs = init_privs;
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

    /* Insert into hash table */
    spin_lock(&vms_proc_hash_lock);
    hash_add_rcu(vms_proc_hash, &proc->hash_node, pid);
    spin_unlock(&vms_proc_hash_lock);

    pr_info("vms: registered process pid=%d vms_pid=0x%08x privs=0x%llx\n",
            pid, vms_pid, init_privs);

    return proc;
}

void vms_proc_free(struct vms_proc *proc)
{
    int i;
    struct vms_ast_entry *ast, *tmp;

    /* Remove from hash table */
    spin_lock(&vms_proc_hash_lock);
    hash_del_rcu(&proc->hash_node);
    spin_unlock(&vms_proc_hash_lock);

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

    /* RCU-deferred free — proc may still be accessed by RCU readers */
    kfree_rcu(proc, rcu);
}

/* ================================================================
 * ioctl dispatch
 * ================================================================ */

static long vms_ioctl_register(unsigned long arg)
{
    struct vms_register_args args;
    struct vms_proc *proc;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    proc = vms_proc_register(current->pid, args.vms_pid, args.init_privs);
    if (IS_ERR(proc)) {
        args.status = 0x0000001C;  /* SS$_DUPNAM (already registered) */
        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;
        return 0;
    }

    args.status = 0x00000001;  /* SS$_NORMAL */
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;

    return 0;
}

static long vms_dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct vms_proc *proc;

    /* REGISTER doesn't require an existing proc */
    if (cmd == VMS_IOCTL_REGISTER)
        return vms_ioctl_register(arg);

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

    /* Lock manager (3d) */
    case VMS_IOCTL_ENQ:
        return vms_ioctl_enq(proc, arg);
    case VMS_IOCTL_DEQ:
        return vms_ioctl_deq(proc, arg);
    case VMS_IOCTL_CONVERT:
        return vms_ioctl_convert(proc, arg);
    case VMS_IOCTL_GETLKI:
        return vms_ioctl_getlki(proc, arg);

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
    return 0;
}

static const struct file_operations vms_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = vms_dev_ioctl,
    .open           = vms_dev_open,
    .release        = vms_dev_release,
};

static struct miscdevice vms_misc = {
    .minor  = MISC_DYNAMIC_MINOR,
    .name   = "vms",
    .fops   = &vms_fops,
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
    vms_lock_init();
    vms_eflag_init();

    /* Register /dev/vms */
    ret = misc_register(&vms_misc);
    if (ret) {
        pr_err("vms: failed to register /dev/vms: %d\n", ret);
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

    kmem_cache_destroy(vms_proc_cache);

    pr_info("vms: module unloaded\n");
}

module_init(vms_init);
module_exit(vms_exit);
