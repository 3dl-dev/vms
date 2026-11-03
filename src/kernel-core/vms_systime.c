// SPDX-License-Identifier: GPL-2.0
/*
 * vms_systime.c - the executive's system time: $GETTIM / $GETUTC / $SETIME
 * (rd vms-642).
 *
 * VMS keeps ONE system time for the whole system (EXE$GQ_SYSTIME): every
 * process's $GETTIM reads it, and $SETIME -- DCL SET TIME -- changes it for all
 * of them. It is advanced by the interval timer and recalibrated from the
 * hardware time-of-year clock. OVMX's time-of-year clock is the substrate
 * kernel's realtime clock (exec_time_now_vms, exec_kbackend.h SS17); the system
 * time is that clock plus an offset this executive holds, so it advances with
 * the substrate's clock and a $SETIME moves it without touching the substrate's
 * clock (setting the host's clock is not what a VMS process asked for, and is
 * not OVMX's to do).
 *
 *   $SETIME with a time      -> offset = time - the time-of-year clock now
 *   $SETIME with no timadr   -> recalibrate: offset = 0
 * Both need OPER and LOG_IO enabled (OpenVMS System Services Reference Manual,
 * $SETIME, "Required Privileges"); SS$_NOPRIV otherwise. A delta time (a
 * negative quadword) is SS$_IVTIME.
 *
 * Substrate-neutral: the only host primitives used are exec_lock and
 * exec_copyin/out and exec_time_now_vms, so the same source is the Linux vms.ko
 * and the NetBSD vms.kmod facility.
 */

#include "vms_internal.h"
#include "exec_kbackend.h"
#include "vms_prot.h"     /* vms_prot_require_priv */

/* The cell. A 64-bit quantity on a 32-bit substrate (VAX) is not read or
 * written in one access, so the offset is read and written under the lock. */
static exec_lock_t systime_lock;
static int64_t     systime_offset;   /* system time - time-of-year clock, 100 ns */

void vms_systime_init(void)
{
    exec_lock_init(&systime_lock);
    systime_offset = 0;
}

void vms_systime_cleanup(void)
{
    exec_lock_destroy(&systime_lock);
}

/* The system time, VMS absolute (100 ns ticks since 17-NOV-1858). */
uint64_t vms_systime_now(void)
{
    int64_t off;

    exec_lock(&systime_lock);
    off = systime_offset;
    exec_unlock(&systime_lock);
    return exec_time_now_vms() + (uint64_t)off;
}

long vms_ioctl_systime(struct vms_proc *proc, unsigned long arg)
{
    struct vms_systime_args a;
    uint64_t privs;

    memset(&a, 0, sizeof(a));
    if (exec_copyin(&a, (const void *)arg, sizeof(a)))
        return -EFAULT;

    switch (a.op) {
    case VMS_SYSTIME_GET:
        a.time = vms_systime_now();
        a.status = SS__NORMAL;
        break;

    case VMS_SYSTIME_SET:
    case VMS_SYSTIME_RECAL:
        exec_lock(&proc->mode_lock);
        privs = proc->cur_privs;
        exec_unlock(&proc->mode_lock);
        a.status = vms_prot_require_priv(privs, VMS_PRV_M_OPER | VMS_PRV_M_LOG_IO);
        if (a.status != SS__NORMAL)
            break;
        if (a.op == VMS_SYSTIME_SET && (int64_t)a.time < 0) {
            a.status = SS__IVTIME;     /* a delta time is not a system time */
            break;
        }
        exec_lock(&systime_lock);
        if (a.op == VMS_SYSTIME_SET)
            systime_offset = (int64_t)(a.time - exec_time_now_vms());
        else
            systime_offset = 0;           /* recalibrate: the time-of-year clock */
        exec_unlock(&systime_lock);
        a.time = vms_systime_now();
        a.status = SS__NORMAL;
        break;

    default:
        a.status = SS__BADPARAM;
        break;
    }

    if (exec_copyout((void *)arg, &a, sizeof(a)))
        return -EFAULT;
    return 0;
}
