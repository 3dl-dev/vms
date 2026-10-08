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
 * SCOPE (vms-d37 core + vms-aba increment): LNM$SYSTEM, LNM$GROUP and
 * LNM$JOB are all fully executive-resident and cross-process. vms-d37 made
 * SYSTEM executive-resident (unblocking DEFINE/SYSTEM propagation and the
 * 0.2 demo) and carried `table` and `scope_key` in the arena format from
 * day one so GROUP/JOB residency would be a data change, not a flag-day.
 * vms-aba is that data change: derive_scope_key() now derives the UIC group
 * for GROUP and the job tree for JOB from the caller's PCB (proc->uic,
 * proc->job_id -- see vms_module.c's vms_proc_parent_job_id()), and
 * VMS_IOCTL_LNM_GETSCOPE hands a caller its own two keys so the userspace
 * read path (the mmap'd arena, no syscall) can filter GROUP/JOB entries
 * exactly as it already does for SYSTEM's scope_key == 0.
 *
 * NO PER-PROCESS FALLBACK (CLAUDE.md Rule 9 / INV-6). There is no
 * non-executive path: a caller reaches these tables only through /dev/vms.
 * When /dev/vms is absent the userspace client fails honestly with
 * SS$_NOSUCHDEV; it does not construct a private SYSTEM table.
 *
 * SUBSTRATE-AGNOSTIC EXECUTIVE CORE (rd vms-d61, epic vms-8e8 -- the LAST
 * facility to leave src/kernel, deferred through Phases D-G because it needed
 * the hardest seam: a userspace-mapped arena plus store barriers, neither
 * covered by the earlier shim). This file lives in src/kernel-core/ and names
 * NO <linux/...> symbol: every host primitive it needs goes through the
 * kernel-backend shim. vms-d61 adds the two seams it waited on --
 *   - exec_membar_producer (exec_kbackend.h §9): the seqlock STORE barrier the
 *     writer used (smp_wmb) between the generation bumps and the entry stores;
 *   - exec_arena_alloc / exec_arena_free (§10): the ALLOCATION half of the
 *     userspace-publishable arena (Linux vmalloc_user / vfree).
 * The MMAP-TIME MAPPING itself (remap_vmalloc_range + clearing VM_MAYWRITE)
 * is host char-device glue, NOT facility logic, and stays in the Linux rind:
 * src/kernel/vms_module.c's vms_lnm_mmap asks this facility for the arena
 * base+size (vms_lnm_arena_base/_size below) and does the substrate-specific
 * mapping there. The Linux vms.ko provides the backend (exec_kbackend_linux.h);
 * the NetBSD `vms' module will provide its own -- including the uvm-object arena
 * and its char-device publish -- when lnm joins its SRCS (a later item; the
 * NetBSD arena seam is contract-only today, following the exec_blockdev
 * precedent).
 */

#include "vms_internal.h"     /* struct vms_proc/vms_lnm_arena, the SS$/args/
                               * status codes, the C-string + ctype vocabulary */
#include "exec_kbackend.h"    /* exec_lock/copy/alloc/membar/arena */

/*
 * The one arena, allocated at module init via exec_arena_alloc() so its pages
 * can be published read-only into userspace by the host char device's mmap
 * handler (src/kernel/vms_module.c's vms_lnm_mmap). The executive is the ONLY
 * writer.
 */
static struct vms_lnm_arena *lnm_arena;

/*
 * Serialises writers and orders them against the seqlock generation
 * counter. Readers (in userspace, over the mmap) take no lock. Initialised at
 * module load in vms_lnm_init() (NOT statically): the NetBSD backend's kmutex
 * cannot be statically initialised, so runtime exec_lock_init() is the
 * substrate-agnostic form -- on Linux it forwards to spin_lock_init(), so
 * behaviour is unchanged.
 */
static exec_lock_t lnm_write_lock;

int vms_lnm_init(void)
{
    lnm_arena = exec_arena_alloc(sizeof(*lnm_arena));
    if (!lnm_arena)
        return -ENOMEM;

    exec_lock_init(&lnm_write_lock);

    /* exec_arena_alloc() zeroes the allocation. Fill the header. */
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
    exec_arena_free(lnm_arena);
    lnm_arena = NULL;
    exec_lock_destroy(&lnm_write_lock);
}

/*
 * vms_lnm_arena_base / vms_lnm_arena_size - hand the host char device's mmap
 * handler what it needs to publish the arena read-only into a process, WITHOUT
 * this facility naming the host mm layer (design record §2; the mm coupling
 * stays in the rind). vms_module.c's vms_lnm_mmap reads the base back and does
 * the substrate-specific mapping (remap_vmalloc_range + clearing VM_MAYWRITE on
 * Linux) itself. The base is NULL until vms_lnm_init() has run; the size is the
 * fixed arena size the mmap handler bounds the request against.
 */
void *vms_lnm_arena_base(void)
{
    return lnm_arena;
}

size_t vms_lnm_arena_size(void)
{
    return sizeof(struct vms_lnm_arena);
}

/* ---- write helpers ------------------------------------------------- */

/*
 * Seqlock: make the generation odd before touching an entry, even after.
 * A userspace reader that samples an odd or changed value retries, so it
 * never observes a torn write. The store barrier (exec_membar_producer, a
 * smp_wmb on Linux) keeps the entry stores from being reordered around the
 * counter, so a reader cannot see the counter move before the payload it
 * guards.
 */
static inline void lnm_write_begin(void)
{
    lnm_arena->generation++;    /* -> odd */
    exec_membar_producer();
}

static inline void lnm_write_end(void)
{
    exec_membar_producer();
    lnm_arena->generation++;    /* -> even */
}

/*
 * derive_scope_key - the scope of a mutation (or a GETSCOPE query), taken
 * from the PCB, never from the caller (design §3.3).
 *
 * SYSTEM is singular (0). GROUP is the caller's UIC group -- documented VMS
 * behaviour (OpenVMS System Services Reference, $CRELNM/$TRNLNM: LNM$GROUP
 * is shared by every process in the same UIC group). JOB is the caller's
 * job tree, proc->job_id, set once at registration by
 * vms_proc_parent_job_id() in vms_module.c: a top-level process (an
 * interactive login, a detached process) is its own job root, and every
 * subprocess it creates (SPAWN) inherits that root's job_id -- also
 * documented VMS behaviour (DCL Dictionary, SPAWN). Both are DERIVED from
 * the PCB the dispatcher already resolved from the caller's task, never
 * supplied by the ioctl argument -- the same discipline as UIC derivation
 * at registration (vms-2b8): a process that could name its own scope could
 * read or clobber another group's or job's logical names by asking for
 * their key.
 */
static uint32_t derive_scope_key(uint32_t table, const struct vms_proc *proc)
{
    switch (table) {
    case VMS_LNM_TBL_SYSTEM:
        return 0;
    case VMS_LNM_TBL_GROUP:
        return proc->uic >> 16;   /* UIC group */
    case VMS_LNM_TBL_JOB:
        return proc->job_id;
    case VMS_LNM_TBL_PROCESS:
        /* The VMS PID: DCL and the image it activated share it (rd vms-ef21). */
        return proc->vms_pid;
    default:
        return 0;
    }
}

/* Is `table` one of the executive-resident LNM tables? */
static bool lnm_table_is_valid(uint32_t table)
{
    return table == VMS_LNM_TBL_SYSTEM ||
           table == VMS_LNM_TBL_GROUP ||
           table == VMS_LNM_TBL_JOB ||
           table == VMS_LNM_TBL_PROCESS;
}

/*
 * Find the in-use entry for (table, scope_key, name, acmode). A logical name is
 * case-SENSITIVE ($CRELNM stores the name exactly as given; only
 * LNM$M_CASE_BLIND on a translation folds case), and one name may exist once
 * per access mode in the same table -- $CRELNM supersedes only the name at the
 * same mode (rd vms-ef21; observed on VAX V7.3 and Alpha V8.4,
 * docs/oracle/semantics/lnm/).
 */
static struct vms_lnm_entry *lnm_find(uint32_t table, uint32_t scope_key,
                                      const char *name, uint8_t acmode)
{
    uint32_t i;

    for (i = 0; i < lnm_arena->max_entries; i++) {
        struct vms_lnm_entry *e = &lnm_arena->entries[i];

        if (!e->in_use)
            continue;
        if (e->table != table || e->scope_key != scope_key ||
            e->acmode != acmode)
            continue;
        if (strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

/*
 * The access mode a request may act at. A name at an inner mode (kernel or
 * executive) needs SYSNAM; without it the request is maximized to supervisor,
 * the mode DCL's own names live at. (OVMX's CLI does not run in a separately
 * tracked supervisor mode -- every task is at user mode as far as
 * proc->current_mode goes -- so supervisor is the outermost mode a request can
 * be pinned to without breaking DCL; an image's own names arrive at the user
 * mode $CRELNM defaults to.)
 */
static uint8_t lnm_effective_mode(uint8_t acmode, const struct vms_proc *proc)
{
    if (acmode > PSL_C_USER)
        acmode = PSL_C_USER;
    if (acmode < PSL_C_SUPER && !(proc->cur_privs & VMS_PRV_M_SYSNAM))
        acmode = PSL_C_SUPER;
    return acmode;
}

/* Free one entry (caller holds lnm_write_lock inside a write section). */
static void lnm_free_entry(struct vms_lnm_entry *e)
{
    e->in_use = 0;
    if (lnm_arena->entry_count)
        lnm_arena->entry_count--;
}

/*
 * vms_lnm_rundown - delete the LNM$PROCESS names of VMS process `vms_pid` at
 * `min_acmode` and every outer mode. Image rundown passes PSL_C_USER (VMS
 * deletes a process's user-mode names when its image exits); process deletion
 * passes PSL_C_KERNEL (the whole table goes with the process).
 */
void vms_lnm_rundown(uint32_t vms_pid, uint8_t min_acmode)
{
    uint32_t i;

    if (!lnm_arena || vms_pid == 0)
        return;
    exec_lock(&lnm_write_lock);
    /*
     * Open a write section (bump the seqlock generation readers retry on)
     * only when there is a name to delete (rd vms-ec7e). This runs at EVERY
     * process teardown, most of which own no such name; bumping the generation
     * for nothing made every concurrent translation retry. The pre-scan is
     * stable: lnm_write_lock excludes every other writer.
     */
    for (i = 0; i < lnm_arena->max_entries; i++) {
        const struct vms_lnm_entry *e = &lnm_arena->entries[i];

        if (e->in_use && e->table == VMS_LNM_TBL_PROCESS &&
            e->scope_key == vms_pid && e->acmode >= min_acmode)
            break;
    }
    if (i == lnm_arena->max_entries) {
        exec_unlock(&lnm_write_lock);
        return;
    }
    lnm_write_begin();
    for (; i < lnm_arena->max_entries; i++) {
        struct vms_lnm_entry *e = &lnm_arena->entries[i];

        if (e->in_use && e->table == VMS_LNM_TBL_PROCESS &&
            e->scope_key == vms_pid && e->acmode >= min_acmode)
            lnm_free_entry(e);
    }
    lnm_write_end();
    exec_unlock(&lnm_write_lock);
}

/*
 * vms_lnm_forget_device - delete the LNM$SYSTEM names whose single equivalence
 * is the device `devnam` ("MBA12:"). A named mailbox's logical name is the
 * mailbox's: when the executive deletes the mailbox the name goes with it, so
 * a later $ASSIGN of the name finds neither (OpenVMS: SS$_IVDEVNAM, observed
 * IO.ASSIGN.MBX.GONE on VAX V7.3 and Alpha V8.4; vms-4a69). Called by the
 * mailbox driver after it has freed the unit, holding none of its own locks.
 */
void vms_lnm_forget_device(const char *devnam)
{
    uint32_t i;

    if (!lnm_arena || !devnam || devnam[0] == '\0')
        return;
    exec_lock(&lnm_write_lock);
    lnm_write_begin();
    for (i = 0; i < lnm_arena->max_entries; i++) {
        struct vms_lnm_entry *e = &lnm_arena->entries[i];

        if (e->in_use && e->table == VMS_LNM_TBL_SYSTEM && e->num_equiv == 1 &&
            strcmp(e->equiv[0].value, devnam) == 0)
            lnm_free_entry(e);
    }
    lnm_write_end();
    exec_unlock(&lnm_write_lock);
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

/*
 * vms_lnm_copy_process - give VMS process `to_pid` a copy of `from_pid`'s
 * LNM$PROCESS names (rd vms-ef21), except the CONFINE ones (LNM$M_CONFINE:
 * "not copied into a subprocess"). The executive analogue of fork() copying a
 * process's memory: before the process table moved into the executive, a
 * forked child inherited it with the rest of its address space, and that is
 * still what a forked (not continued, not $CREPRC'd) child gets -- the same
 * fork-inheritance the executive gives BG channels (vms-3bf). A full arena
 * stops the copy where it is (EXLNMQUOTA has nobody to be reported to here).
 */
void vms_lnm_copy_process(uint32_t from_pid, uint32_t to_pid)
{
    uint32_t i;

    if (!lnm_arena || !from_pid || !to_pid || from_pid == to_pid)
        return;
    exec_lock(&lnm_write_lock);
    /* No write section when the parent has nothing to copy (rd vms-ec7e, as
     * vms_lnm_rundown): this runs at every forked registration. */
    for (i = 0; i < lnm_arena->max_entries; i++) {
        const struct vms_lnm_entry *src = &lnm_arena->entries[i];

        if (src->in_use && src->table == VMS_LNM_TBL_PROCESS &&
            src->scope_key == from_pid && !(src->attributes & 0x02u))
            break;
    }
    if (i == lnm_arena->max_entries) {
        exec_unlock(&lnm_write_lock);
        return;
    }
    lnm_write_begin();
    for (i = 0; i < lnm_arena->max_entries; i++) {
        struct vms_lnm_entry *src = &lnm_arena->entries[i], *dst;

        if (!src->in_use || src->table != VMS_LNM_TBL_PROCESS ||
            src->scope_key != from_pid || (src->attributes & 0x02u))
            continue;
        dst = lnm_alloc_slot();
        if (!dst)
            break;
        memcpy(dst, src, sizeof(*dst));
        dst->scope_key = to_pid;
        lnm_arena->entry_count++;
    }
    lnm_write_end();
    exec_unlock(&lnm_write_lock);
}

/*
 * lnm_priv_check - the ONE gate DEFINE and DELETE both consult (vms-5b7).
 *
 * Real, documented VMS behaviour (OpenVMS DCL Dictionary, DEFINE): creating
 * OR deleting a name in LNM$SYSTEM requires SYSNAM or SYSPRV; LNM$GROUP
 * requires GRPNAM, GRPPRV or SYSPRV. LNM$JOB (and LNM$PROCESS, which never
 * reaches this ioctl) need no privilege at all. VMS gates DEASSIGN
 * identically to DEFINE, which is why one function serves both callers
 * instead of two copies of the same switch. The bit values are oracle-
 * pinned -- see the comment beside VMS_PRV_V_SYSNAM in vms_ioctl.h.
 *
 * Checked against cur_privs (the ENABLED mask), the same privilege the
 * executive consults everywhere else it gates on privilege (vms_access.c,
 * vms_proctab.c) -- not perm_privs, which is the AUTHORIZED ceiling a
 * process may not currently have enabled (SET PROCESS/PRIVILEGE can
 * disable an authorized privilege without touching perm_privs).
 *
 * On refusal, *status is set to SS$_NOPRIV (36, oracle-pinned, docs/oracle/
 * vax73-privileges.md §1) and the caller must not touch the table: both
 * vms_ioctl_lnm_define() and vms_ioctl_lnm_delete() call this BEFORE
 * deriving scope_key, let alone before lnm_find()/lnm_alloc_slot() touch
 * an entry.
 */
static bool lnm_priv_check(uint32_t table, uint64_t cur_privs, uint32_t *status)
{
    bool ok;

    switch (table) {
    case VMS_LNM_TBL_SYSTEM:
        ok = (cur_privs & (VMS_PRV_M_SYSNAM | VMS_PRV_M_SYSPRV)) != 0;
        break;
    case VMS_LNM_TBL_GROUP:
        ok = (cur_privs & (VMS_PRV_M_GRPNAM | VMS_PRV_M_GRPPRV |
                            VMS_PRV_M_SYSPRV)) != 0;
        break;
    default:
        /* LNM$JOB: no privilege required (real VMS behaviour). */
        ok = true;
        break;
    }

    if (!ok)
        *status = SS__NOPRIV;
    return ok;
}

/* ---- ioctl handlers ------------------------------------------------ */

long vms_ioctl_lnm_define(struct vms_proc *proc, unsigned long arg)
{
    struct vms_lnm_def_args *a;
    struct vms_lnm_entry *e;
    uint32_t scope_key;
    long ret = 0;
    int i;

    a = exec_zalloc(sizeof(*a));
    if (!a)
        return -ENOMEM;

    if (exec_copyin(a, (const void *)arg, sizeof(*a))) {
        ret = -EFAULT;
        goto out_free;
    }

    /* Validate the table id and the name (trust-boundary checks). */
    if (!lnm_table_is_valid(a->table)) {
        a->status = SS__BADPARAM;
        goto out_copy;
    }

    /* Name must be NUL-terminated inside the buffer and 1..MAX in length. */
    a->name[VMS_LNM_MAX_NAME] = '\0';
    if (a->name[0] == '\0' || strnlen(a->name, sizeof(a->name)) > VMS_LNM_MAX_NAME) {
        a->status = SS__IVLOGNAM;
        goto out_copy;
    }
    /* A name with NO equivalence string is legal: $CRELNM with an empty item
     * list creates one (LNM.CRE.NOITEMS, rd vms-ef21). */
    if (a->num_equiv > VMS_LNM_MAX_EQUIV) {
        a->status = SS__BADPARAM;
        goto out_copy;
    }
    /* An equivalence is LENGTH-delimited, not NUL-delimited: it may hold any
     * byte (a process-permanent file's begins ESC NUL, rd vms-b14e). Only the
     * length is bounded here. */
    for (i = 0; i < a->num_equiv; i++) {
        a->equiv[i].value[VMS_LNM_MAX_VALUE] = '\0';
        if (a->equiv[i].length > VMS_LNM_MAX_VALUE)
            a->equiv[i].length = VMS_LNM_MAX_VALUE;
    }

    /* PRIVILEGE ENFORCEMENT (vms-5b7) -- see lnm_priv_check()'s header for
     * the full rationale and oracle citation. On refusal the arena is
     * untouched: this returns before scope_key is even derived, let alone
     * before lnm_find()/lnm_alloc_slot() touch the table. */
    if (!lnm_priv_check(a->table, proc->cur_privs, &a->status))
        goto out_copy;

    scope_key = derive_scope_key(a->table, proc);
    a->acmode = lnm_effective_mode(a->acmode, proc);

    exec_lock(&lnm_write_lock);

    e = lnm_find(a->table, scope_key, a->name, a->acmode);
    if (e) {
        /* Supersede in place. */
        lnm_write_begin();
        e->attributes = a->attributes;
        e->acmode = a->acmode;
        e->num_equiv = a->num_equiv;
        memcpy(e->equiv, a->equiv, sizeof(e->equiv));
        lnm_write_end();
        a->status = SS__SUPERSEDE;
        exec_unlock(&lnm_write_lock);
        goto out_copy;
    }

    e = lnm_alloc_slot();
    if (!e) {
        exec_unlock(&lnm_write_lock);
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
    exec_unlock(&lnm_write_lock);

out_copy:
    if (exec_copyout((void *)arg, a, sizeof(*a)))
        ret = -EFAULT;
out_free:
    exec_free(a);
    return ret;
}

/*
 * $DELLNM (rd vms-ef21). A named delete removes the name at the request's
 * access mode AND every outer mode, and is SS$_NOLOGNAM only when there was
 * nothing at those modes (a user-mode delete of a supervisor-mode name is
 * NOLOGNAM; a supervisor delete leaves the kernel-mode name standing). With
 * VMS_LNM_DEL_ALL (no name given) every name in the table at those modes goes.
 * Observed on VAX V7.3 and Alpha V8.4, docs/oracle/semantics/lnm/.
 */
long vms_ioctl_lnm_delete(struct vms_proc *proc, unsigned long arg)
{
    struct vms_lnm_del_args *a;
    uint32_t scope_key, i, n = 0;
    uint8_t mode;
    bool all;
    long ret = 0;

    a = exec_zalloc(sizeof(*a));
    if (!a)
        return -ENOMEM;

    if (exec_copyin(a, (const void *)arg, sizeof(*a))) {
        ret = -EFAULT;
        goto out_free;
    }

    if (!lnm_table_is_valid(a->table)) {
        a->status = SS__BADPARAM;
        goto out_copy;
    }

    all = (a->flags & VMS_LNM_DEL_ALL) != 0;
    a->name[VMS_LNM_MAX_NAME] = '\0';
    if (!all && a->name[0] == '\0') {
        a->status = SS__IVLOGNAM;
        goto out_copy;
    }

    /* Same gate as create (vms-5b7) -- see lnm_priv_check()'s header. */
    if (!lnm_priv_check(a->table, proc->cur_privs, &a->status))
        goto out_copy;

    scope_key = derive_scope_key(a->table, proc);
    mode = lnm_effective_mode(a->acmode, proc);

    exec_lock(&lnm_write_lock);
    lnm_write_begin();
    for (i = 0; i < lnm_arena->max_entries; i++) {
        struct vms_lnm_entry *e = &lnm_arena->entries[i];

        if (!e->in_use || e->table != a->table || e->scope_key != scope_key ||
            e->acmode < mode)
            continue;
        if (!all && strcmp(e->name, a->name) != 0)
            continue;
        lnm_free_entry(e);
        n++;
    }
    lnm_write_end();
    exec_unlock(&lnm_write_lock);

    a->status = (n || all) ? SS__NORMAL : SS__NOLOGNAM;

out_copy:
    if (exec_copyout((void *)arg, a, sizeof(*a)))
        ret = -EFAULT;
out_free:
    exec_free(a);
    return ret;
}

/*
 * vms_ioctl_lnm_getscope - hand the caller its own GROUP and JOB scope
 * keys (vms-aba). See the VMS_IOCTL_LNM_GETSCOPE comment in vms_lnm.h for
 * why the read path needs this even though translation itself is a
 * zero-syscall mmap read: the scope_key to filter on is a derived fact,
 * not something the reader may compute from anything it holds locally
 * (SETIDENT can change a process's UIC to something the task's raw Linux
 * credentials do not reflect -- see vms_ioctl_setident() -- so there is no
 * safe local recomputation of the GROUP key, and JOB has no userspace
 * representation at all). No write lock is needed: this only reads the
 * caller's own PCB fields, already stable under the RCU/hash discipline
 * vms_proc_find_or_err() gives every ioctl handler.
 */
long vms_ioctl_lnm_getscope(struct vms_proc *proc, unsigned long arg)
{
    struct vms_lnm_scope_args args;

    memset(&args, 0, sizeof(args));
    args.group_key = derive_scope_key(VMS_LNM_TBL_GROUP, proc);
    args.job_key   = derive_scope_key(VMS_LNM_TBL_JOB, proc);
    args.process_key = derive_scope_key(VMS_LNM_TBL_PROCESS, proc);
    args.status    = SS__NORMAL;

    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}
