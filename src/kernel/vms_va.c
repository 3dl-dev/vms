// SPDX-License-Identifier: GPL-2.0
/*
 * vms_va.c - process virtual address space: $EXPREG / $CRETVA / $DELTVA
 * (rd vms-bc53)
 *
 * WHAT THIS IS. On OpenVMS the executive owns a process's virtual address
 * space: $EXPREG appends demand-zero pages to the end of the P0 region,
 * $CRETVA creates demand-zero pages over a named range, $DELTVA deletes them,
 * and every page created is charged against the process's paging-file quota
 * (PGFLQUOTA). Running out is a VMS status the caller sees -- SS$_EXQUOTA for
 * the quota, SS$_VASFULL for the region -- not a host errno. (OpenVMS System
 * Services Reference Manual, $EXPREG/$CRETVA/$DELTVA; public documentation,
 * CLAUDE.md Rule 8.)
 *
 * Before this file those three services were userspace mmap()/munmap() in the
 * calling process (src/libvms/syssvc/sys_memory.c, OVMX-USERSPACE), and the C
 * RTL heap went straight to Linux brk/mmap. Here the EXECUTIVE does the work:
 * it decides where the pages go, maps them into the caller's address space
 * itself (vm_mmap/vm_munmap on current->mm -- the executive's substrate), keeps
 * the per-process record of which ranges it created, and charges and enforces
 * the quota. SYS$EXPREG/CRETVA/DELTVA (LIBVMS) and the DEC C RTL heap
 * (src/vmslink/ovmx_decc_vm.c) reach it through VMS_IOCTL_VA (KIF_SVC_VA).
 *
 * THE QUOTA. A process whose identity was established from SYSUAF
 * (VMS_IOCTL_SETIDENT with quota_valid, i.e. LOGINOUT) has a SOURCED
 * PGFLQUOTA in pagelets; every page this file creates for it is charged, and a
 * request that would take the charge past the quota is refused SS$_EXQUOTA
 * before anything is mapped. A process with NO sourced quota block (the boot
 * process and what it creates without a quota list) is still ACCOUNTED --
 * charged, reported, deleted -- but has no limit to enforce, and every reply
 * says so (quota_sourced == 0). OVMX does not invent a limit it has no source
 * for; the SYSGEN PQL_D* defaults and JIB pooling across a job are a tracked
 * follow-up (see the PR / rd vms-bc53 trail), not fabricated here.
 *
 * THE P0 REGION is [VMS_VA_P0_FLOOR, VMS_VA_P0_CEILING): VMS's P0 ends at
 * 0x3FFFFFFF on every architecture, which is also what keeps a DEC C 32-bit
 * pointer valid. $EXPREG grows the region upward from its current end
 * (frep0va), skipping any range the substrate or the image activator already
 * occupies; when nothing above fits, SS$_VASFULL. P1 expansion ($EXPREG with
 * region 1) is not provided: SS$_UNSUPPORTED, honestly.
 *
 * WHAT $CRETVA MAY REPLACE. VMS's $CRETVA replaces existing pages in the range
 * with new demand-zero pages. Here it does that for pages THIS service created
 * for this process; a range that touches pages the executive did not create
 * (the image, shareables, the substrate's own mappings) is refused
 * SS$_PAGOWNVIO -- those pages belong to a more privileged owner (the image
 * activator / executive), and replacing them from user mode is exactly what
 * VMS refuses with that status. $DELTVA deletes only executive-created pages
 * and skips the rest, as VMS skips pages that do not exist.
 *
 * AN ADDRESS SPACE THAT WENT AWAY. The record is tied to the mm it describes
 * (mmgrab). If the process has since exec'd into a new address space, the old
 * record describes nothing and is dropped on the next call -- its pages died
 * with the old mm, so their charge is returned.
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/string.h>

#include "vms_internal.h"

#define VMS_PAGELET 512ULL

struct vms_va_ext {
    struct list_head link;
    u64 start;              /* first byte */
    u64 end;                /* one past the last byte */
};

struct vms_va_space {
    struct mm_struct *mm;   /* the address space this record describes */
    struct list_head exts;  /* sorted, disjoint, adjacent ones merged */
    u64 charged;            /* bytes created and not deleted */
    u64 frep0va;            /* end of the P0 region $EXPREG grows */
    u32 nexts;
};

static void va_free_exts(struct vms_va_space *va)
{
    struct vms_va_ext *e, *n;

    list_for_each_entry_safe(e, n, &va->exts, link) {
        list_del(&e->link);
        kfree(e);
    }
    va->nexts = 0;
    va->charged = 0;
    va->frep0va = 0;
}

/* The caller's record, created on first use; reset if the mm changed. */
static struct vms_va_space *va_get(struct vms_proc *proc)
{
    struct vms_va_space *va = proc->va;
    struct mm_struct *mm = current->mm;

    if (!mm)
        return NULL;
    if (!va) {
        va = kzalloc(sizeof(*va), GFP_KERNEL);
        if (!va)
            return NULL;
        INIT_LIST_HEAD(&va->exts);
        proc->va = va;
    }
    if (va->mm != mm) {
        va_free_exts(va);
        if (va->mm)
            mmdrop(va->mm);
        mmgrab(mm);
        va->mm = mm;
    }
    return va;
}

/* Record [s, e) as executive-created; merges with neighbours. */
static int va_add(struct vms_va_space *va, u64 s, u64 e)
{
    struct vms_va_ext *x, *prev = NULL, *ne;

    list_for_each_entry(x, &va->exts, link) {
        if (x->start > s)
            break;
        prev = x;
    }
    /* merge with the predecessor */
    if (prev && prev->end == s) {
        prev->end = e;
        ne = list_is_last(&prev->link, &va->exts) ? NULL
             : list_next_entry(prev, link);
        if (ne && ne->start == e) {
            prev->end = ne->end;
            list_del(&ne->link);
            kfree(ne);
            va->nexts--;
        }
        return 0;
    }
    /* merge with the successor */
    ne = prev ? (list_is_last(&prev->link, &va->exts) ? NULL
                 : list_next_entry(prev, link))
              : list_first_entry_or_null(&va->exts, struct vms_va_ext, link);
    if (ne && ne->start == e) {
        ne->start = s;
        return 0;
    }
    x = kmalloc(sizeof(*x), GFP_KERNEL);
    if (!x)
        return -ENOMEM;
    x->start = s;
    x->end = e;
    if (prev)
        list_add(&x->link, &prev->link);
    else
        list_add(&x->link, &va->exts);
    va->nexts++;
    return 0;
}

/* Bytes of [s, e) that are executive-created. */
static u64 va_overlap(struct vms_va_space *va, u64 s, u64 e)
{
    struct vms_va_ext *x;
    u64 n = 0;

    list_for_each_entry(x, &va->exts, link) {
        u64 a = max(x->start, s), b = min(x->end, e);

        if (a < b)
            n += b - a;
    }
    return n;
}

/*
 * Forget [s, e) from the record. When `unmap`, also delete the pages from the
 * address space -- only the executive-created pieces, never anything else.
 * Returns the bytes forgotten; lo and hi get the span actually touched.
 */
static u64 va_remove(struct vms_va_space *va, u64 s, u64 e, bool unmap,
                     u64 *lo, u64 *hi)
{
    struct vms_va_ext *x, *n;
    u64 gone = 0;

    *lo = 0;
    *hi = 0;
    list_for_each_entry_safe(x, n, &va->exts, link) {
        u64 a = max(x->start, s), b = min(x->end, e);

        if (a >= b)
            continue;
        if (unmap)
            vm_munmap(a, b - a);
        gone += b - a;
        if (!*lo || a < *lo)
            *lo = a;
        if (b > *hi)
            *hi = b;
        if (a == x->start && b == x->end) {
            list_del(&x->link);
            kfree(x);
            va->nexts--;
        } else if (a == x->start) {
            x->start = b;
        } else if (b == x->end) {
            x->end = a;
        } else {
            struct vms_va_ext *t = kmalloc(sizeof(*t), GFP_KERNEL);

            if (!t) {
                /* Cannot split the record: keep the tail charged (the
                 * pages are gone, so this over-charges, never under). */
                x->end = a;
                continue;
            }
            t->start = b;
            t->end = x->end;
            x->end = a;
            list_add(&t->link, &x->link);
            va->nexts++;
        }
    }
    va->charged -= min(gone, va->charged);
    return gone;
}

/*
 * Is every mapped byte of [s, e) executive-created? (Unmapped bytes are fine:
 * $CRETVA creates them.) Walks the substrate's VMAs under the mmap read lock.
 */
static bool va_range_is_ours(struct vms_va_space *va, u64 s, u64 e)
{
    struct mm_struct *mm = current->mm;
    struct vm_area_struct *vma;
    u64 a = s;
    bool ok = true;

    mmap_read_lock(mm);
    while (a < e) {
        vma = find_vma(mm, a);
        if (!vma || vma->vm_start >= e)
            break;
        {
            u64 vs = max((u64)vma->vm_start, a), ve = min((u64)vma->vm_end, e);

            if (va_overlap(va, vs, ve) != ve - vs) {
                ok = false;
                break;
            }
            a = ve;
        }
    }
    mmap_read_unlock(mm);
    return ok;
}

/* Quota check for `add` more bytes. */
static u32 va_quota_check(struct vms_proc *proc, struct vms_va_space *va, u64 add)
{
    u64 limit;

    if (!proc->quota_valid)
        return SS__NORMAL;          /* unsourced: accounted, not limited */
    limit = (u64)proc->quota.pgflquota * VMS_PAGELET;
    if (va->charged + add > limit || va->charged + add < va->charged)
        return SS__EXQUOTA;
    return SS__NORMAL;
}

static void va_report(struct vms_proc *proc, struct vms_va_space *va,
                      struct vms_va_args *args)
{
    args->quota_sourced = proc->quota_valid ? 1 : 0;
    args->charged = va ? va->charged / VMS_PAGELET : 0;
    args->frep0va = (va && va->frep0va) ? va->frep0va : VMS_VA_P0_FLOOR;
    args->extents = va ? va->nexts : 0;
    if (proc->quota_valid) {
        args->pgflquota = proc->quota.pgflquota;
        args->pagfilcnt = args->pgflquota > args->charged
                          ? args->pgflquota - args->charged : 0;
    } else {
        args->pgflquota = 0;
        args->pagfilcnt = 0;
    }
}

/* $EXPREG, P0: append `len` bytes at the end of the region. */
static u32 va_expreg(struct vms_proc *proc, struct vms_va_space *va,
                     struct vms_va_args *args)
{
    struct mm_struct *mm = current->mm;
    u64 len = PAGE_ALIGN(args->length);
    u64 addr;
    u32 st;
    int tries;

    if (args->region != VMS_VA_REGION_P0)
        return SS__UNSUPPORTED;
    if (len == 0 || len < args->length)
        return SS__BADPARAM;
    st = va_quota_check(proc, va, len);
    if (!(st & 1))
        return st;

    addr = va->frep0va ? va->frep0va : VMS_VA_P0_FLOOR;
    for (tries = 0; tries < 4096; tries++) {
        struct vm_area_struct *vma;
        unsigned long r;

        if (addr + len > VMS_VA_P0_CEILING || addr + len < addr)
            return SS__VASFULL;
        mmap_read_lock(mm);
        vma = find_vma(mm, addr);
        if (vma && vma->vm_start < addr + len) {
            addr = PAGE_ALIGN((u64)vma->vm_end);
            mmap_read_unlock(mm);
            continue;
        }
        mmap_read_unlock(mm);
        r = vm_mmap(NULL, addr, len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0);
        if (r == (unsigned long)-EEXIST)
            continue;               /* raced a substrate mapping: look again */
        if (IS_ERR_VALUE(r))
            return SS__INSFMEM;
        if (r != addr) {            /* never: FIXED_NOREPLACE maps at addr or fails */
            vm_munmap(r, len);
            return SS__INSFMEM;
        }
        if (va_add(va, addr, addr + len)) {
            vm_munmap(addr, len);
            return SS__INSFMEM;
        }
        va->charged += len;
        va->frep0va = addr + len;
        args->base = addr;
        args->length = len;
        return SS__NORMAL;
    }
    return SS__VASFULL;
}

/* $CRETVA: demand-zero pages over [base, base+length). */
static u32 va_cretva(struct vms_proc *proc, struct vms_va_space *va,
                     struct vms_va_args *args)
{
    u64 s = args->base & PAGE_MASK;
    u64 e = PAGE_ALIGN(args->base + args->length);
    u64 have, lo, hi;
    unsigned long r;
    u32 st;

    if (args->length == 0 || e <= s || s == 0 ||
        !access_ok((void __user *)(unsigned long)s, e - s))
        return SS__BADPARAM;
    if (!va_range_is_ours(va, s, e))
        return SS__PAGOWNVIO;
    have = va_overlap(va, s, e);
    st = va_quota_check(proc, va, (e - s) - have);
    if (!(st & 1))
        return st;
    /* Nothing of ours to replace: never clobber, even a mapping that
     * appeared since the check. Something of ours: replace it (VMS). */
    r = vm_mmap(NULL, s, e - s, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS |
                (have ? MAP_FIXED : MAP_FIXED_NOREPLACE), 0);
    if (r == (unsigned long)-EEXIST)
        return SS__PAGOWNVIO;
    if (IS_ERR_VALUE(r) || r != s)
        return SS__INSFMEM;
    /* the replaced pages are re-created: forget, then record the range */
    va_remove(va, s, e, false, &lo, &hi);
    if (va_add(va, s, e)) {
        vm_munmap(s, e - s);
        return SS__INSFMEM;
    }
    va->charged += e - s;
    if (s < VMS_VA_P0_CEILING && e > va->frep0va && e <= VMS_VA_P0_CEILING)
        va->frep0va = e;
    args->base = s;
    args->length = e - s;
    return SS__NORMAL;
}

/* $DELTVA: delete the executive-created pages in [base, base+length). */
static u32 va_deltva(struct vms_proc *proc, struct vms_va_space *va,
                     struct vms_va_args *args)
{
    u64 s = args->base & PAGE_MASK;
    u64 e = PAGE_ALIGN(args->base + args->length);
    u64 lo, hi;

    (void)proc;
    if (args->length == 0 || e <= s)
        return SS__BADPARAM;
    va_remove(va, s, e, true, &lo, &hi);
    /* deleting the top of P0 shrinks the region, as on VMS */
    if (hi && hi == va->frep0va) {
        struct vms_va_ext *x;
        u64 top = 0;

        list_for_each_entry(x, &va->exts, link)
            if (x->end <= VMS_VA_P0_CEILING && x->end > top)
                top = x->end;
        va->frep0va = top;
    }
    args->base = lo;
    args->length = hi - lo;
    return SS__NORMAL;
}

long vms_ioctl_va(struct vms_proc *proc, unsigned long arg)
{
    struct vms_va_args args;
    struct vms_va_space *va;
    u32 st;

    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;
    args.status = 0;

    mutex_lock(&proc->va_lock);
    va = va_get(proc);
    if (!va) {
        st = SS__INSFMEM;
    } else {
        switch (args.op) {
        case VMS_VA_EXPREG: st = va_expreg(proc, va, &args); break;
        case VMS_VA_CRETVA: st = va_cretva(proc, va, &args); break;
        case VMS_VA_DELTVA: st = va_deltva(proc, va, &args); break;
        case VMS_VA_QUERY:  st = SS__NORMAL; break;
        default:            st = SS__BADPARAM; break;
        }
    }
    args.status = st;
    va_report(proc, va, &args);
    mutex_unlock(&proc->va_lock);

    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/* The PCB is going away: drop the record (the pages die with the mm). */
void vms_va_release(struct vms_proc *proc)
{
    struct vms_va_space *va = proc->va;

    if (!va)
        return;
    va_free_exts(va);
    if (va->mm)
        mmdrop(va->mm);
    kfree(va);
    proc->va = NULL;
}
