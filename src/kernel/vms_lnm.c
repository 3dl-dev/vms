// SPDX-License-Identifier: GPL-2.0
/*
 * vms_lnm.c - Executive-resident logical name tables (vms-d37)
 *
 * Implements the ruled "C-corrected" placement (design record
 * docs/design-logical-name-placement.md): vms.ko OWNS the LNM$SYSTEM (and,
 * as a data-only extension later, LNM$GROUP / LNM$JOB) storage. Userspace
 * TRANSLATES by reading a read-only mmap of the arena -- no syscall on the
 * hot path -- and MUTATES through the define/delete ioctls here.
 *
 * SCOPE OF THIS CHANGE (vms-d37 core): LNM$SYSTEM is fully executive-
 * resident and cross-process, which is what unblocks DEFINE/SYSTEM
 * propagation and the 0.2 demo. The arena format carries `table` and
 * `scope_key` so LNM$GROUP / LNM$JOB residency is a later data change and
 * not a flag-day; their scoping derivation is deferred (see the note in
 * derive_scope_key()).
 *
 * NO PER-PROCESS FALLBACK (CLAUDE.md Rule 9 / INV-6). There is no
 * non-executive path: a caller reaches these tables only through /dev/vms.
 * When /dev/vms is absent the userspace client fails honestly with
 * SS$_NOSUCHDEV; it does not construct a private SYSTEM table.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/ctype.h>
#include <linux/string.h>

#include "vms_internal.h"

/*
 * The one arena, allocated at module init with vmalloc_user() so it can be
 * remapped into userspace read-only. The executive is the ONLY writer.
 */
static struct vms_lnm_arena *lnm_arena;

/*
 * Serialises writers and orders them against the seqlock generation
 * counter. Readers (in userspace, over the mmap) take no lock.
 */
static DEFINE_SPINLOCK(lnm_write_lock);

int vms_lnm_init(void)
{
    lnm_arena = vmalloc_user(sizeof(*lnm_arena));
    if (!lnm_arena)
        return -ENOMEM;

    /* vmalloc_user() zeroes the allocation. Fill the header. */
    lnm_arena->magic       = VMS_LNM_ARENA_MAGIC;
    lnm_arena->version     = VMS_LNM_ARENA_VERSION;
    lnm_arena->arena_size  = (uint32_t)sizeof(*lnm_arena);
    lnm_arena->max_entries = VMS_LNM_MAX_ENTRIES;
    lnm_arena->entry_count = 0;
    lnm_arena->generation  = 0;   /* even == no write in flight */

    pr_info("vms: logical-name arena ready (%u entries, %u bytes)\n",
            lnm_arena->max_entries, lnm_arena->arena_size);
    return 0;
}

void vms_lnm_cleanup(void)
{
    vfree(lnm_arena);
    lnm_arena = NULL;
}

/*
 * vms_lnm_mmap - hand userspace a READ-ONLY view of the arena.
 *
 * The mapping is read-only and VM_MAYWRITE is cleared so mprotect() cannot
 * turn it writable afterwards -- this is the direct analogue of VMS
 * protecting system space by processor access mode (design §2.4). The MMU,
 * not a convention, is what stops a process corrupting the system logical
 * name table.
 */
int vms_lnm_mmap(struct file *filp, struct vm_area_struct *vma)
{
    unsigned long size = vma->vm_end - vma->vm_start;
    int ret;

    (void)filp;

    if (!lnm_arena)
        return -ENODEV;

    /* Only the arena, only from its start. */
    if (vma->vm_pgoff != (VMS_LNM_MMAP_OFFSET >> PAGE_SHIFT))
        return -EINVAL;
    if (size > PAGE_ALIGN(sizeof(*lnm_arena)))
        return -EINVAL;

    /* Reject any write intent, now and forever. */
    if (vma->vm_flags & VM_WRITE)
        return -EACCES;
    vm_flags_clear(vma, VM_MAYWRITE);

    ret = remap_vmalloc_range(vma, lnm_arena, 0);
    if (ret < 0)
        return ret;

    return 0;
}

/* ---- write helpers ------------------------------------------------- */

/*
 * Seqlock: make the generation odd before touching an entry, even after.
 * A userspace reader that samples an odd or changed value retries, so it
 * never observes a torn write. Barriers keep the entry stores from being
 * reordered around the counter.
 */
static inline void lnm_write_begin(void)
{
    lnm_arena->generation++;    /* -> odd */
    smp_wmb();
}

static inline void lnm_write_end(void)
{
    smp_wmb();
    lnm_arena->generation++;    /* -> even */
}

/*
 * derive_scope_key - the scope of a mutation, taken from the PCB, never
 * from the caller (design §3.3).
 *
 * SYSTEM is singular (0). GROUP is per-UIC-group. JOB has no OVMX model
 * yet -- OVMX has no job trees -- so rather than invent a key semantic VMS
 * never showed us (Rule 10) the define/delete ioctls reject JOB for now;
 * this returns the UIC group as a placeholder that is never reached while
 * that rejection stands. LNM$GROUP / LNM$JOB executive residency is the
 * deferred follow-up flagged in the PR.
 */
static uint32_t derive_scope_key(uint32_t table, const struct vms_proc *proc)
{
    switch (table) {
    case VMS_LNM_TBL_SYSTEM:
        return 0;
    case VMS_LNM_TBL_GROUP:
    case VMS_LNM_TBL_JOB:
    default:
        return proc->uic >> 16;   /* UIC group */
    }
}

/* Find an in-use entry matching (table, scope_key, upcased name). */
static struct vms_lnm_entry *lnm_find(uint32_t table, uint32_t scope_key,
                                      const char *name)
{
    uint32_t i;

    for (i = 0; i < lnm_arena->max_entries; i++) {
        struct vms_lnm_entry *e = &lnm_arena->entries[i];

        if (!e->in_use)
            continue;
        if (e->table != table || e->scope_key != scope_key)
            continue;
        if (strcasecmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

/* First free slot, or NULL when the arena is full. */
static struct vms_lnm_entry *lnm_alloc_slot(void)
{
    uint32_t i;

    for (i = 0; i < lnm_arena->max_entries; i++) {
        if (!lnm_arena->entries[i].in_use)
            return &lnm_arena->entries[i];
    }
    return NULL;
}

/* Upcase a NUL-terminated name in place (VMS logical names are upcased). */
static void upcase(char *s)
{
    for (; *s; s++)
        *s = (char)toupper((unsigned char)*s);
}

/* ---- ioctl handlers ------------------------------------------------ */

long vms_ioctl_lnm_define(struct vms_proc *proc, unsigned long arg)
{
    struct vms_lnm_def_args *a;
    struct vms_lnm_entry *e;
    uint32_t scope_key;
    long ret = 0;
    int i;

    a = kzalloc(sizeof(*a), GFP_KERNEL);
    if (!a)
        return -ENOMEM;

    if (copy_from_user(a, (void __user *)arg, sizeof(*a))) {
        ret = -EFAULT;
        goto out_free;
    }

    /* Validate the table id and the name (trust-boundary checks). */
    if (a->table != VMS_LNM_TBL_SYSTEM) {
        /*
         * GROUP/JOB executive residency is deferred (see derive_scope_key).
         * The userspace client does not route them here; reject defensively
         * rather than store an entry no reader consults.
         */
        a->status = SS__BADPARAM;
        goto out_copy;
    }

    /* Name must be NUL-terminated inside the buffer and 1..MAX in length. */
    a->name[VMS_LNM_MAX_NAME] = '\0';
    if (a->name[0] == '\0' || strnlen(a->name, sizeof(a->name)) > VMS_LNM_MAX_NAME) {
        a->status = SS__IVLOGNAM;
        goto out_copy;
    }
    if (a->num_equiv == 0 || a->num_equiv > VMS_LNM_MAX_EQUIV) {
        a->status = SS__BADPARAM;
        goto out_copy;
    }
    for (i = 0; i < a->num_equiv; i++)
        a->equiv[i].value[VMS_LNM_MAX_VALUE] = '\0';

    upcase(a->name);

    /*
     * PRIVILEGE ENFORCEMENT IS DEFERRED, stated loudly rather than faked.
     * Real VMS requires SYSNAM to write LNM$SYSTEM (and GRPNAM for
     * LNM$GROUP) -- documented behaviour -- but the SYSNAM/GRPNAM privilege
     * BIT VALUES are not yet pinned in this tree (src/kernel/vms_ioctl.h's
     * VMS_PRV_V_* set does not carry them, and PRV$V_SYSNAM/PRV$V_GRPNAM
     * must be read from SYSDEF.STB on the oracle exactly as the existing
     * bits were, vms-2b8). Gating on a GUESSED bit would be self-
     * certification and could refuse the SYSTEM identity that seeds the
     * defaults at boot. So this ioctl currently enforces only that the
     * caller is a registered process (the dispatcher already did that). The
     * SYSNAM/GRPNAM check is deferred to a follow-up that pins the bits
     * against the oracle first -- filed as an rd item (see PR body), NOT
     * routed to the operator for sign-off.
     */

    scope_key = derive_scope_key(a->table, proc);

    spin_lock(&lnm_write_lock);

    e = lnm_find(a->table, scope_key, a->name);
    if (e) {
        /* Supersede in place. */
        lnm_write_begin();
        e->attributes = a->attributes;
        e->acmode = a->acmode;
        e->num_equiv = a->num_equiv;
        memcpy(e->equiv, a->equiv, sizeof(e->equiv));
        lnm_write_end();
        a->status = SS__SUPERSEDE;
        spin_unlock(&lnm_write_lock);
        goto out_copy;
    }

    e = lnm_alloc_slot();
    if (!e) {
        spin_unlock(&lnm_write_lock);
        a->status = SS__EXLNMQUOTA;   /* arena full (PENDING PURITY, design §4.2) */
        goto out_copy;
    }

    lnm_write_begin();
    memset(e, 0, sizeof(*e));
    e->table = a->table;
    e->scope_key = scope_key;
    e->attributes = a->attributes;
    e->acmode = a->acmode;
    e->num_equiv = a->num_equiv;
    e->name_length = (uint16_t)strnlen(a->name, VMS_LNM_MAX_NAME);
    memcpy(e->name, a->name, sizeof(e->name));
    memcpy(e->equiv, a->equiv, sizeof(e->equiv));
    e->in_use = 1;
    lnm_arena->entry_count++;
    lnm_write_end();

    a->status = SS__NORMAL;
    spin_unlock(&lnm_write_lock);

out_copy:
    if (copy_to_user((void __user *)arg, a, sizeof(*a)))
        ret = -EFAULT;
out_free:
    kfree(a);
    return ret;
}

long vms_ioctl_lnm_delete(struct vms_proc *proc, unsigned long arg)
{
    struct vms_lnm_del_args *a;
    struct vms_lnm_entry *e;
    uint32_t scope_key;
    long ret = 0;

    a = kzalloc(sizeof(*a), GFP_KERNEL);
    if (!a)
        return -ENOMEM;

    if (copy_from_user(a, (void __user *)arg, sizeof(*a))) {
        ret = -EFAULT;
        goto out_free;
    }

    if (a->table != VMS_LNM_TBL_SYSTEM) {
        a->status = SS__BADPARAM;
        goto out_copy;
    }

    a->name[VMS_LNM_MAX_NAME] = '\0';
    if (a->name[0] == '\0') {
        a->status = SS__IVLOGNAM;
        goto out_copy;
    }
    upcase(a->name);

    /* Privilege enforcement deferred -- see vms_ioctl_lnm_define(). */

    scope_key = derive_scope_key(a->table, proc);

    spin_lock(&lnm_write_lock);
    e = lnm_find(a->table, scope_key, a->name);
    if (!e) {
        spin_unlock(&lnm_write_lock);
        a->status = SS__NOLOGNAM;
        goto out_copy;
    }

    lnm_write_begin();
    e->in_use = 0;
    if (lnm_arena->entry_count)
        lnm_arena->entry_count--;
    lnm_write_end();

    a->status = SS__NORMAL;
    spin_unlock(&lnm_write_lock);

out_copy:
    if (copy_to_user((void __user *)arg, a, sizeof(*a)))
        ret = -EFAULT;
out_free:
    kfree(a);
    return ret;
}
