// SPDX-License-Identifier: GPL-2.0
/*
 * vms_proctab.c - Executive-resident process table (vms-8019)
 *
 * On OpenVMS the process name is not a property a process keeps to
 * itself: it lives in the executive's process database. $SETPRN writes
 * it there, $GETJPI resolves a process by it, and SHOW SYSTEM
 * enumerates the database. That shared residency is the entire meaning
 * of a VMS process name -- a name only this process can see is not a
 * process name at all.
 *
 * This file gives OVMX the same property. The name is stored in
 * struct vms_proc, which lives in kernel memory and is keyed by the
 * Linux thread-group id (getpid(2)). Because execve() does not change
 * it, the name survives image activation with no userspace carrier of
 * any kind; because it is the group id and not a thread id, every
 * thread of the image sees the same name (vms-9fc round 2).
 *
 * Scoping follows VMS: a process name is unique within, and resolved
 * within, the caller's UIC group (OpenVMS System Services Reference,
 * $SETPRN and $GETJPI). OVMX maps UIC [group,member] onto the task's
 * [gid,uid], the same packing sys$getjpi's JPI$_UIC item returns.
 */

/*
 * SUBSTRATE-AGNOSTIC EXECUTIVE CORE (rd vms-846b, Phase F). This file lives in
 * src/kernel-core/ and names NO <linux/…> symbol: every host primitive it needs
 * goes through the kernel-backend shim. It is the FIRST core facility to bind
 * the host TASK -- process liveness, credentials and accounting -- so it is
 * where the host-task / RCU-lite / sleepable-mutex seam lands (design record
 * docs/design-netbsd-executive-core.md §2, §8 row F). The Linux vms.ko provides
 * the backend (exec_kbackend_linux.h / exec_hash_linux.h); the NetBSD `vms'
 * module will provide its own when proctab joins its SRCS (rd vms-9dc).
 */
#include "vms_internal.h"     /* struct vms_proc, the SS$/args/status codes,
                               * the C-string + fixed-width vocabulary */
#include "exec_kbackend.h"    /* exec_lock/copy/current/task/rcu/mutex */
#include "exec_hash.h"        /* exec_hash_for_each[_safe] / exec_hash_del_rcu */

/* Serializes reaping so two callers cannot claim the same victim. Held across
 * the per-victim teardown, which may sleep, so it is an exec_mutex (sleepable),
 * not an exec_lock (non-sleeping). */
static EXEC_DEFINE_MUTEX(vms_reap_mutex);

/*
 * uic_group - the [group] half of a packed UIC.
 *
 * VMS scopes process-name uniqueness and lookup to the UIC group;
 * everything in this file that compares names compares groups first.
 */
static inline uint32_t uic_group(uint32_t uic)
{
    return uic >> 16;
}

/*
 * vms_proc_task_alive - does the PROCESS backing this entry still exist?
 *
 * proc->pid_ref is the thread group's pid (task_tgid), so the task it
 * resolves to is the group leader. The kernel does not release the
 * leader while any other thread of the group is still running, so this
 * is a whole-process liveness test, not a per-thread one -- a thread
 * exiting out of a live multithreaded image must not make its process
 * reapable. pid_task() returns NULL once the leader has been released,
 * which is the point at which the VMS process has ceased to exist and
 * its slot may be reused.
 */
static bool vms_proc_task_alive(struct vms_proc *proc)
{
    if (!proc->pid_ref)
        return false;

    /* exec_task_alive() does the whole-process liveness test described above
     * (pid_task under an RCU read section) behind the shim; the RCU is the
     * backend's concern, not this facility's. */
    return exec_task_alive(proc->pid_ref);
}

/*
 * vms_proc_reap_dead - remove entries whose process has exited.
 *
 * The PCB is owned by the process, not by an open /dev/vms channel, so
 * closing the channel does not delete it (see vms_dev_release). Entries
 * are therefore reclaimed here, lazily, on every operation that reads
 * or mutates the table.
 *
 * One victim per pass: the teardown below the hash removal can sleep on
 * nothing but must not run under the hash spinlock, so the scan unlinks
 * one entry and then leaves the lock to tear it down. The table is small
 * (VMS SHOW SYSTEM walks the PCB vector linearly too) and reaping only
 * runs on process table operations.
 *
 * The victim is UNLINKED WHILE THE LOCK IS STILL HELD, and that removal
 * is the ownership claim (see vms_proc_free()). Selecting a victim under
 * the lock and only claiming it afterwards would be a use-after-free: a
 * concurrent vms_dev_release() could claim and kfree_rcu() the same
 * entry in the gap, and the claim itself reads proc->hash_node.
 */
void vms_proc_reap_dead(void)
{
    struct vms_proc *proc, *victim;
    exec_hash_node_t *tmp;
    int bkt;

    exec_mutex_lock(&vms_reap_mutex);

    for (;;) {
        victim = NULL;

        exec_lock(&vms_proc_hash_lock);
        exec_hash_for_each_safe(vms_proc_hash, bkt, tmp, proc, hash_node) {
            if (!vms_proc_task_alive(proc)) {
                exec_hash_del_rcu(&proc->hash_node);
                victim = proc;
                break;
            }
        }
        exec_unlock(&vms_proc_hash_lock);

        if (!victim)
            break;

        vms_proc_free_claimed(victim);
    }

    exec_mutex_unlock(&vms_reap_mutex);
}

/*
 * vms_proc_may_read - may `caller` read `target`'s identity?
 *
 * ORACLE-PINNED (vms-2b8 round 3). The full transcript is in
 * docs/oracle/vax73-privileges.md §5; measured on VAX1, OpenVMS
 * VAX V7.3, by driving the caller's own privilege mask with
 * SET PROCESS/PRIVILEGE and reading other processes with F$GETJPI:
 *
 *   NOALL, same UIC group  ($GETJPI AUDIT_SERVER [SYSTEM] USERNAME)
 *                                              -> AUDIT$SERVER
 *   NOALL, other UIC group ($GETJPI TCPIP$FTP_1 [TCPIP$AUX,..])
 *                                              -> %SYSTEM-F-NOPRIV
 *   GROUP only, other UIC group                -> %SYSTEM-F-NOPRIV
 *   WORLD only, other UIC group                -> TCPIP$FTP
 *
 * THE OBVIOUS GUESS IS WRONG AND THE MEASUREMENT IS WHY THIS COMMENT
 * EXISTS. "GROUP to read your own group, WORLD to read anyone" is the
 * rule everybody expects, and this item was dispatched asserting it.
 * The oracle says a same-group read needs NO privilege at all, and that
 * GROUP does not help with a cross-group read -- only WORLD does.
 * Enforcing the guess would have refused reads VMS allows and named the
 * wrong privilege in the refusal message for the rest. CLAUDE.md
 * Rule 10: pin it or do not write it.
 *
 * The privilege consulted is cur_privs (the ENABLED mask), which is what
 * SET PROCESS/PRIVILEGE moved on the oracle -- the authorized mask was
 * untouched throughout and the answers still changed.
 */
bool vms_proc_may_read(const struct vms_proc *caller,
                       const struct vms_proc *target)
{
    if (caller == target)
        return true;
    if (uic_group(caller->uic) == uic_group(target->uic))
        return true;
    return (caller->cur_privs & VMS_PRV_M_WORLD) != 0;
}

/*
 * proc_fill_info - snapshot one table row.
 *
 * Called with vms_proc_hash_lock held: the caller copies to userspace
 * after dropping the lock, since copy_to_user() may sleep.
 *
 * `full` is the outcome of vms_proc_may_read(). When it is false the row
 * is REDACTED down to what the oracle's SHOW SYSTEM displays of a
 * process the caller cannot $GETJPI -- its process ID and its process
 * name -- and nothing else. See the vms_procscan_args comment in
 * vms_ioctl.h for why enumeration and identity have different rules on
 * VMS, and why that is a measurement rather than a compromise.
 */
static void proc_fill_info(const struct vms_proc *proc,
                           struct vms_procinfo *info, bool full)
{
    memset(info, 0, sizeof(*info));
    info->vms_pid      = proc->vms_pid;
    memcpy(info->prcnam, proc->prcnam, VMS_PRCNAM_SIZE);
    info->prcnam[VMS_PRCNAM_SIZE - 1] = '\0';

    if (!full) {
        /*
         * Say so in the row. A reader must not have to deduce
         * "withheld" from a zero it cannot distinguish from a real
         * value -- see the `redacted` comment in vms_ioctl.h for the
         * fabricated CPU time the first reader printed when it tried.
         */
        info->redacted = 1;
        return;
    }

    info->linux_pid    = (uint32_t)proc->linux_pid;
    info->uic          = proc->uic;
    info->current_mode = proc->current_mode;
    info->cur_privs    = proc->cur_privs;
    info->perm_privs   = proc->perm_privs;
    memcpy(info->username, proc->username, VMS_USERNAME_SIZE);
    info->username[VMS_USERNAME_SIZE - 1] = '\0';
    /*
     * The job's terminal is copied out with the rest of the identity --
     * below the early return above, so a row that hit it never reaches
     * this line at all. The oracle refused every $GETJPI item on a
     * cross-group process without WORLD rather than a subset (docs/
     * oracle/vax73-privileges.md §5), so a caller that may not read the
     * row's user name may not read which terminal it is on either.
     * Enforced by tests/qemu/test_kmod_ident.c's scan_d_terminal check
     * (vms-d0b): it fails if this line is ever hoisted above the return.
     */
    memcpy(info->terminal, proc->terminal, VMS_DEVNAM_SIZE);
    info->terminal[VMS_DEVNAM_SIZE - 1] = '\0';

    /*
     * P0 program-region extent (vms-68f.i). Same placement as terminal
     * just above -- below the redaction early return, not above it: a
     * process's memory layout is part of what the oracle's blanket
     * cross-group refusal withholds (docs/oracle/vax73-privileges.md
     * §5), not a fact enumeration alone exposes.
     */
    info->p0_base = proc->p0_base;
    info->p0_limit = proc->p0_limit;

    /*
     * P1 control-region extent (vms-68f.ii). Same placement as p0_base/
     * p0_limit just above -- below the redaction early return, for the
     * same reason: a process's memory layout is part of what the oracle's
     * blanket cross-group refusal withholds.
     */
    info->p1_base = proc->p1_base;
    info->p1_limit = proc->p1_limit;
}

/*
 * Seconds between the VMS system-time base (17-NOV-1858 00:00:00, the
 * Smithsonian Modified Julian Date epoch) and the Unix epoch
 * (01-JAN-1970 00:00:00): 40587 days * 86400. JPI$_LOGINTIM is a VMS
 * quadword of 100-nanosecond units counted from that base. Public
 * arithmetic (the epoch is documented, not reverse-engineered) --
 * CLAUDE.md Rule 8.
 */
#define VMS_EPOCH_OFFSET_SEC   3506716800ULL
#define VMS_TIME_UNITS_PER_SEC 10000000ULL   /* 100ns units */

/*
 * vms_proc_pin_task - the host task backing this PCB, pinned so the caller
 * can read its accounting after dropping vms_proc_hash_lock.
 *
 * Caller MUST hold vms_proc_hash_lock. exec_task_pin() does not sleep, so it
 * is safe under the lock; the heavy, possibly-sleeping accounting read
 * (fill_proc_acct's exec_task_read_acct) is then done by the caller AFTER the
 * lock is dropped, on the pinned handle. Returns NULL if the backing process
 * has already exited (nothing to unpin). The pinned handle is opaque -- the
 * facility never inspects the host task, it only hands the handle back to the
 * shim (exec_task_read_acct / exec_task_unpin).
 */
static exec_task_pin_t *vms_proc_pin_task(const struct vms_proc *proc)
{
    if (!proc->pid_ref)
        return NULL;

    return exec_task_pin(proc->pid_ref);
}

/*
 * fill_proc_acct - source the accounting fields of one row from the real
 * host task the executive owns (vms-a7e).
 *
 * CALLED AFTER vms_proc_hash_lock IS DROPPED, on a handle pinned by
 * vms_proc_pin_task(): exec_task_read_acct() below may sleep (its resident-set
 * read acquires the task's mm) and must not run under the lock.
 *
 * WHERE THE PORTABILITY LINE FALLS (rd vms-846b). The HOST reads -- CPU time,
 * page faults, creation instant, resident pages -- live behind the shim in
 * exec_task_read_acct(), which hands back a substrate-neutral
 * struct exec_proc_acct. This function keeps only the VMS SEMANTICS: the unit
 * conversions (ns -> 10ms, wall-ns -> the 100ns-since-1858 quadword), the
 * JPI$ field mapping and the fields_valid bookkeeping -- the authenticity-
 * critical logic that must never drift between substrates. This is a host-task-
 * PROPERTY read; it is NOT the userspace-arena mapping seam vms_lnm.c waits on.
 *
 * WHY THIS IS NOT A FABRICATION, and where the line is (CLAUDE.md Rule 11,
 * INV-6). CPU time, page faults and resident pages are REAL properties of
 * a real process, measured by the kernel that ran it -- the identical
 * argument the /proc-derived JPI$_CPUTIM already stood on (see jpi_cputim's
 * header in src/libvms/syssvc/sys_process.c). What changes vs. that reader is
 * WHO holds them: the EXECUTIVE now does, sourced from the task its own process
 * table pins by pid_ref, so a VMS command (SHOW SYSTEM) READS an executive
 * facility instead of opening /proc itself as a second source. That is the
 * specific defect docs/oracle/vax73-show-system-process.md §5.1 left standing
 * for Page flts/Pages ("/proc has a figure, but a VMS command is a reader of an
 * executive facility, not a second source").
 *
 * CARRIED ON REDACTED ROWS TOO. This is called for every row the scan
 * returns, redacted or not, because nothing in a SHOW SYSTEM row is
 * privileged on VMS -- the oracle printed the whole accounting row for a
 * cross-group process the caller could not $GETJPI (ibid. §1.2) -- and it
 * sources from the pinned handle, never from info->linux_pid (which
 * redaction zeroes). That is what closes the divergence §1.2 recorded.
 */
static void fill_proc_acct(exec_task_pin_t *pin, struct vms_procinfo *info)
{
    struct exec_proc_acct acct;

    exec_task_read_acct(pin, &acct);

    /* JPI$_CPUTIM -- CPU time in 10ms units, the quantity JPI$_CPUTIM counts.
     * acct.cpu_ns is the task's user+system time in nanoseconds. For a
     * single-threaded process (the DCL case) this is the whole-process figure;
     * a multithreaded image under-counts its sibling threads' time, an honest
     * approximation stated rather than hidden. */
    info->cputim = (uint32_t)(acct.cpu_ns / 10000000ULL);   /* ns -> 10ms */
    info->fields_valid |= VMS_PI_V_CPUTIM;

    /* JPI$_PAGEFLTS -- soft + hard faults taken by this process. */
    info->pageflts = (uint32_t)acct.page_faults;
    info->fields_valid |= VMS_PI_V_PAGEFLTS;

    /* JPI$_LOGINTIM -- process creation time as a VMS quadword. acct.create_wall_ns
     * is nanoseconds since the Unix epoch (the backend did the host-clock
     * conversion); rebase to the VMS 100ns-since-1858 quadword here. */
    info->logintim = acct.create_wall_ns / 100ULL
                   + VMS_EPOCH_OFFSET_SEC * VMS_TIME_UNITS_PER_SEC;
    info->fields_valid |= VMS_PI_V_LOGINTIM;

    /* JPI$_PPGCNT -- resident set size in pages. A process with no user address
     * space (has_rss == 0, e.g. a kernel thread) reports no page count rather
     * than a zero the reader could not tell from a measured one. */
    if (acct.has_rss) {
        info->pages = (uint32_t)acct.rss_pages;
        info->fields_valid |= VMS_PI_V_PAGES;
    }
}

/*
 * name_is_valid - reject malformed name strings from userspace.
 *
 * This is a trust-boundary check on an untrusted buffer AND the length
 * rule VMS enforces: the executive must never index a string that is
 * not NUL-terminated inside the buffer it was given, and a name that
 * does not fit in VMS_PRCNAM_SIZE is not a legal VMS process name. A
 * zero-length name is also rejected -- an unnamed process is expressed
 * by never calling SETPRN, not by setting the empty name.
 *
 * name is an inbound VMS_PRCNAM_XFER buffer, so an oversized name is
 * VISIBLE here (no NUL within the first VMS_PRCNAM_SIZE bytes) and is
 * rejected rather than truncated -- which is what the oracle does; see
 * the VMS_PRCNAM_XFER comment in vms_ioctl.h for the transcript.
 */
static bool name_is_valid(const char *name)
{
    size_t i;

    for (i = 0; i < VMS_PRCNAM_SIZE; i++) {
        if (name[i] == '\0')
            return i > 0;
    }
    return false;
}

/*
 * find_by_name - locate a live process by name within a UIC group.
 *
 * Caller must hold vms_proc_hash_lock. Name comparison is exact; VMS
 * process names are stored as given (DCL upcases them before they get
 * here, as it does for every other unquoted DCL token).
 */
static struct vms_proc *find_by_name(uint32_t group, const char *name)
{
    struct vms_proc *proc;
    int bkt;

    exec_hash_for_each(vms_proc_hash, bkt, proc, hash_node) {
        if (uic_group(proc->uic) != group)
            continue;
        if (proc->prcnam[0] == '\0')
            continue;
        if (strncmp(proc->prcnam, name, VMS_PRCNAM_SIZE) == 0)
            return proc;
    }
    return NULL;
}

/*
 * find_by_vms_pid - locate a process by its VMS process ID.
 *
 * Caller must hold vms_proc_hash_lock.
 */
static struct vms_proc *find_by_vms_pid(uint32_t vms_pid)
{
    struct vms_proc *proc;
    int bkt;

    exec_hash_for_each(vms_proc_hash, bkt, proc, hash_node) {
        if (proc->vms_pid == vms_pid)
            return proc;
    }
    return NULL;
}

/*
 * vms_ioctl_setprn - set the calling process's name ($SETPRN).
 *
 * VMS returns SS$_DUPLNAM when the name is already in use within the
 * UIC group; the name is otherwise recorded in the executive, where
 * every other process can see it.
 */
long vms_ioctl_setprn(struct vms_proc *proc, unsigned long arg)
{
    struct vms_setprn_args args;
    struct vms_proc *clash;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    if (!name_is_valid(args.prcnam)) {
        args.status = SS__IVLOGNAM;
        goto out;
    }

    /* A name freed by an exited process must be available again. */
    vms_proc_reap_dead();

    exec_lock(&vms_proc_hash_lock);
    clash = find_by_name(uic_group(proc->uic), args.prcnam);
    if (clash && clash != proc) {
        exec_unlock(&vms_proc_hash_lock);
        args.status = SS__DUPLNAM;
        goto out;
    }
    memcpy(proc->prcnam, args.prcnam, VMS_PRCNAM_SIZE);
    proc->prcnam[VMS_PRCNAM_SIZE - 1] = '\0';
    exec_unlock(&vms_proc_hash_lock);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_getjpi - resolve a process and return its table row.
 *
 * Selects the caller, a process by VMS PID, or -- the case that makes
 * a process name mean anything -- a process by name within the
 * caller's UIC group. VMS returns SS$_NONEXPR when no such process
 * exists.
 *
 * READING ANOTHER PROCESS IS AUTHORIZED, NOT FREE (vms-2b8 round 3).
 * Until this change the PID selector returned any row in the table to
 * any caller, so an unprivileged process could read the user name, UIC
 * and privilege mask of every process on the system -- and it was newly
 * reachable, because the same round opened /dev/vms to unprivileged
 * callers. vms_proc_may_read() decides, and its rule is oracle-measured
 * (see the comment on that function). A refused read returns
 * SS$_NOPRIV (36) and NO row at all, which is what the oracle does: on
 * VAX 7.3 a privilege-less caller was refused every single JPI item on
 * a cross-group process, so the refusal is on the process rather than
 * on the item, and there is no partial answer to hand back.
 *
 * The NAME selector needs no separate check: find_by_name() searches
 * only the caller's own UIC group, and a same-group read requires no
 * privilege on the oracle. Whether WORLD should WIDEN that search to
 * the whole system is NOT pinned -- OVMX therefore does not do it
 * rather than guess (Rule 10).
 */
long vms_ioctl_getjpi(struct vms_proc *proc, unsigned long arg)
{
    struct vms_getjpi_args args;
    struct vms_proc *target;
    exec_task_pin_t *acct_task;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    if (args.select == VMS_JPI_SEL_PRCNAM && !name_is_valid(args.sel_prcnam)) {
        memset(&args.info, 0, sizeof(args.info));
        args.status = SS__IVLOGNAM;
        goto out;
    }

    vms_proc_reap_dead();

    exec_lock(&vms_proc_hash_lock);
    switch (args.select) {
    case VMS_JPI_SEL_SELF:
        target = proc;
        break;
    case VMS_JPI_SEL_PID:
        target = find_by_vms_pid(args.info.vms_pid);
        break;
    case VMS_JPI_SEL_PRCNAM:
        target = find_by_name(uic_group(proc->uic), args.sel_prcnam);
        break;
    default:
        target = NULL;
        exec_unlock(&vms_proc_hash_lock);
        memset(&args.info, 0, sizeof(args.info));
        args.status = SS__BADPARAM;
        goto out;
    }

    if (!target) {
        exec_unlock(&vms_proc_hash_lock);
        memset(&args.info, 0, sizeof(args.info));
        args.status = SS__NONEXPR;
        goto out;
    }

    if (!vms_proc_may_read(proc, target)) {
        exec_unlock(&vms_proc_hash_lock);
        memset(&args.info, 0, sizeof(args.info));
        args.status = SS__NOPRIV;
        goto out;
    }

    proc_fill_info(target, &args.info, true);
    acct_task = vms_proc_pin_task(target);
    exec_unlock(&vms_proc_hash_lock);

    /* Accounting is sourced from the pinned task AFTER the lock is dropped
     * (fill_proc_acct may sleep). See its header. */
    if (acct_task) {
        fill_proc_acct(acct_task, &args.info);
        exec_task_unpin(acct_task);
    }

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * username_is_valid - trust-boundary check on an inbound name buffer.
 *
 * The executive must never index a string that is not NUL-terminated
 * inside the buffer userspace handed it. A zero-length user name is
 * also rejected: "no identity" is expressed by never calling SETIDENT,
 * not by stamping the empty name -- otherwise a process could erase its
 * user name and every reader would have to guess whether "" meant
 * "never authenticated" or "authenticated as nobody".
 *
 * Nothing else is checked. OVMX does not invent a character-set or
 * length rule for user names (CLAUDE.md Rule 10): SYSUAF is the
 * authority on which names exist, and a name that is not in SYSUAF
 * never reaches here because the caller could not authenticate it.
 */
static bool username_is_valid(const char *name)
{
    size_t i;

    for (i = 0; i < VMS_USERNAME_SIZE; i++) {
        if (name[i] == '\0')
            return i > 0;
    }
    return false;
}

/*
 * vms_ioctl_setident - stamp an authenticated identity on the caller.
 *
 * THE RULE THIS ENFORCES, and the reason the item exists: a process
 * cannot grant itself a privilege it was not given.
 *
 * A caller holding SETPRV may establish any identity -- that is what
 * SETPRV means on VMS, and it is what LOGINOUT holds while it turns
 * itself into the user's process. A caller WITHOUT SETPRV may only
 * establish an identity that is weaker than or equal to its own: the
 * new authorized mask must be a subset of its current authorized mask,
 * and neither its UIC nor its user name may change.
 *
 * THE EXACT SCOPE OF THAT GUARANTEE, corrected in round 6 because the
 * earlier wording here ("no sequence of calls walks a process back up")
 * was FALSE and was disproved by execution. It is one-way FOR THIS
 * THREAD GROUP. It says nothing about a NEW task: vms_proc_register()
 * in vms_module.c derives a fresh authorized mask from
 * capable(CAP_SYS_ADMIN) and a fresh UIC from the task's real
 * credentials, and inherits nothing from the parent's row. So a process
 * that drops to FIELD/[200,10] and then forks a child which is STILL
 * LINUX ROOT gives that child CMKRNL|CMEXEC|SYSNAM|GRPNAM|SETPRV|WORLD at
 * registration, and SETPRV is precisely what lets it stamp itself
 * SYSTEM. The reduction survived exactly until the next fork.
 *
 * What closes that is not a rule in here -- it is tools/vms_login.c
 * dropping the session's Linux credentials to the authenticated user's
 * UIC before exec, so the child's derivation produces the unprivileged
 * answer instead of the privileged one. The two halves are load-bearing
 * together: this function refuses a claim, and the credential drop is
 * what makes the claimant unprivileged in the first place.
 *
 * STILL OUTSIDE THE MODEL, and stated rather than implied: a process
 * that legitimately holds CAP_SYS_ADMIN (PID 1, and any not-yet-dropped
 * pre-authentication session -- LOGINOUT, VMSSSHD -- before it drops)
 * registers with the enforced mask no matter what its parent held. That
 * is not a hole this check can close -- a task with CAP_SYS_ADMIN can
 * rmmod the executive -- but it does mean the SET of root-privileged VMS
 * processes at any moment is itself a security property: every entry
 * point that establishes identity before dropping Linux credentials
 * belongs in it, and NOT WRITING A COUNT here is deliberate -- a number
 * in a comment is a claim nothing forces to stay true when a new
 * pre-authentication entry point (this round adds VMSSSHD) is added
 * beside it. Grep for CAP_SYS_ADMIN-holding callers of
 * vms_kif_setident() to get the current membership, not this comment.
 *
 * SEMANTICS PIN (docs/oracle/vax73-privileges.md §3): SETPRV is what
 * authorizes EXCEEDING the authorized mask, not what authorizes USING
 * it. Measured on the oracle -- with SETPRV removed from the current
 * mask, SET PROCESS/PRIVILEGE=SYSPRV still returned %X10000001 because
 * SYSPRV was in the authorized mask. The subset test below is the same
 * rule applied to establishing an identity rather than enabling a
 * privilege.
 *
 * SS$_NOPRIV (36, %SYSTEM-F-NOPRIV, "insufficient privilege or object
 * protection violation") is oracle-pinned in the same session.
 */
long vms_ioctl_setident(struct vms_proc *proc, unsigned long arg)
{
    struct vms_ident_args args;
    bool has_setprv;
    uint64_t authorized;
    uint32_t cur_uic;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    if (!username_is_valid(args.username)) {
        args.status = SS__IVLOGNAM;
        goto out;
    }

    /*
     * Read the caller's CURRENT authorization before deciding, and hold
     * the decision and the write under the same locks -- otherwise two
     * concurrent SETIDENTs on the same process could each check against
     * the mask the other is about to replace and both succeed.
     *
     * hash_lock OUTER, mode_lock INNER (see struct vms_proc). The pair
     * is held together only here, so there is no ordering to violate.
     */
    exec_lock(&vms_proc_hash_lock);
    exec_lock(&proc->mode_lock);

    has_setprv = (proc->cur_privs & VMS_PRV_M_SETPRV) != 0;
    authorized = proc->perm_privs;
    cur_uic    = proc->uic;

    if (!has_setprv) {
        /*
         * Subset test on the WHOLE mask, including bits the executive
         * does not enforce. A stored-but-unenforced privilege is still
         * reported to the user by SHOW PROCESS/PRIVILEGES, so letting a
         * process add one would let it lie about itself to every reader
         * -- which is the same defect in a quieter costume.
         */
        if ((args.authorized_privs & ~authorized) != 0 ||
            args.uic != cur_uic) {
            exec_unlock(&proc->mode_lock);
            exec_unlock(&vms_proc_hash_lock);
            args.status = SS__NOPRIV;
            goto out;
        }
        /*
         * THE USER NAME IS UNDER THE SAME GUARD AS THE UIC (vms-2b8
         * round 3). Without it this ioctl guarded the two fields nobody
         * displays and left the one every reader shows completely
         * unprotected: a process that had genuinely setuid()'d away from
         * root could pass its OWN uic and its OWN mask -- satisfying
         * both clauses above by construction -- with the name "SYSTEM",
         * and be reported as SYSTEM by $GETJPI to every process from
         * then on. A user name is not something a process may choose
         * (CLAUDE.md Rule 10); it comes from SYSUAF through something
         * holding SETPRV, or it does not come.
         *
         * strncmp rather than memcmp: both buffers are NUL-terminated
         * inside VMS_USERNAME_SIZE (proc->username by construction,
         * args.username by username_is_valid above), and bytes past the
         * NUL are not part of the name -- comparing them would refuse a
         * caller re-stamping its own name from a differently-padded
         * buffer.
         */
        if (strncmp(proc->username, args.username, VMS_USERNAME_SIZE) != 0) {
            exec_unlock(&proc->mode_lock);
            exec_unlock(&vms_proc_hash_lock);
            args.status = SS__NOPRIV;
            goto out;
        }
    }

    /*
     * Store the name normalized: copy up to the NUL and leave the rest
     * zero, so the field the comparison above reads is never a mixture
     * of a name and whatever padding a caller happened to send.
     */
    memset(proc->username, 0, VMS_USERNAME_SIZE);
    strncpy(proc->username, args.username, VMS_USERNAME_SIZE - 1);
    proc->uic        = args.uic;
    proc->perm_privs = args.authorized_privs;
    /*
     * OVMX DESIGN CHOICE, labelled in vms_ioctl.h: current privileges
     * are set equal to authorized. OpenVMS distinguishes AUTHORIZE's
     * /PRIVILEGES from /DEFPRIVILEGES; the OVMX SYSUAF record carries a
     * single uaf$q_priv quadword, so OVMX has one mask.
     */
    proc->cur_privs  = args.authorized_privs;

    exec_unlock(&proc->mode_lock);
    exec_unlock(&vms_proc_hash_lock);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_establish_system - construct the SYSTEM identity onto the
 * caller (vms-a17e).
 *
 * See struct vms_establish_system_args in vms_ioctl.h for the full design
 * rationale (the OPA0: precedent applied to identity) and
 * VMS_SYSTEM_UIC/VMS_PRV_M_SYSTEM_ALL in vms_internal.h for where the
 * constants are grounded. In one line: this is vms_ioctl_setident() with
 * the identity fixed to a constant the executive owns instead of a value
 * the caller read out of SYSUAF, so PROVISION.EXE (the only caller) never
 * has to open SYS$SYSTEM:SYSUAF.DAT to become SYSTEM.
 *
 * capable(CAP_SYS_ADMIN) is the same real kernel credential
 * vms_proc_register() already reads to decide the enforced privilege set
 * at registration -- not a new gate, and not one a process can grant
 * itself. A caller without it gets SS$_NOPRIV, the same status
 * vms_ioctl_setident() returns for the analogous refusal.
 *
 * Locking mirrors vms_ioctl_setident(): hash_lock outer, mode_lock inner,
 * both held across the whole read-decide-write so no concurrent caller on
 * this same process can interleave with the write.
 */
long vms_ioctl_establish_system(struct vms_proc *proc, unsigned long arg)
{
    struct vms_establish_system_args args;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    if (!exec_current_is_privileged()) {
        args.status = SS__NOPRIV;
        goto out;
    }

    exec_lock(&vms_proc_hash_lock);
    exec_lock(&proc->mode_lock);

    memset(proc->username, 0, VMS_USERNAME_SIZE);
    strncpy(proc->username, "SYSTEM", VMS_USERNAME_SIZE - 1);
    proc->uic        = VMS_SYSTEM_UIC;
    proc->perm_privs = VMS_PRV_M_SYSTEM_ALL;
    proc->cur_privs  = VMS_PRV_M_SYSTEM_ALL;

    exec_unlock(&proc->mode_lock);
    exec_unlock(&vms_proc_hash_lock);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_procscan - enumerate the process table (SHOW SYSTEM's reader).
 *
 * Returns the row at the incoming cursor and advances it. The scan ends
 * with SS$_NONEXPR, matching what $PROCESS_SCAN returns once a wildcard
 * search is exhausted.
 *
 * The cursor is an ordinal over the hash walk, so a row can be missed or
 * repeated if the table changes mid-scan. VMS has the same property --
 * SHOW SYSTEM is a sample of a live system, not a transaction.
 *
 * ROWS THE CALLER MAY NOT READ ARE REDACTED, NOT OMITTED (vms-2b8
 * round 3). Before this change the scan handed every field of every row
 * to every caller, which -- once /dev/vms was opened to unprivileged
 * processes in the same round -- let any process enumerate the user
 * name, UIC and privilege mask of the entire system. It was proven by
 * execution against a real /dev/vms, not argued.
 *
 * The fix is shaped by two measurements taken on the oracle in the same
 * privilege-less state (docs/oracle/vax73-privileges.md §5):
 * SHOW SYSTEM listed EVERY process including a cross-group one, with
 * its name; $GETJPI on that same process was refused every item. So
 * enumeration is not privileged on VMS and identity is, and a row the
 * caller cannot $GETJPI comes back carrying only what SHOW SYSTEM shows
 * of it. Skipping it instead would hide a process VMS displays.
 */
long vms_ioctl_procscan(struct vms_proc *proc, unsigned long arg)
{
    struct vms_procscan_args args;
    struct vms_proc *cur, *target = NULL;
    exec_task_pin_t *acct_task;
    uint32_t ordinal = 0;
    int bkt;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    vms_proc_reap_dead();

    exec_lock(&vms_proc_hash_lock);
    exec_hash_for_each(vms_proc_hash, bkt, cur, hash_node) {
        if (ordinal == args.index) {
            target = cur;
            break;
        }
        ordinal++;
    }

    if (!target) {
        exec_unlock(&vms_proc_hash_lock);
        memset(&args.info, 0, sizeof(args.info));
        args.status = SS__NONEXPR;
        goto out;
    }

    proc_fill_info(target, &args.info, vms_proc_may_read(proc, target));
    acct_task = vms_proc_pin_task(target);
    exec_unlock(&vms_proc_hash_lock);

    /* Accounting is carried on EVERY row -- redacted or not -- and sourced
     * from the pinned task after the lock is dropped (fill_proc_acct may
     * sleep). A SHOW SYSTEM row is not privileged on VMS; see fill_proc_acct
     * and docs/oracle/vax73-show-system-process.md §1.2. */
    if (acct_task) {
        fill_proc_acct(acct_task, &args.info);
        exec_task_unpin(acct_task);
    }

    args.index++;
    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}
