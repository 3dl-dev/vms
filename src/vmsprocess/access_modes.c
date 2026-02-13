/*
 * access_modes.c - VMS Access Mode and Privilege emulation
 *
 * VMS defines four hierarchical access modes:
 *   0  Kernel     - Most privileged; runs the kernel itself
 *   1  Executive  - File system, RMS, etc.
 *   2  Supervisor - CLI, command language interpreter
 *   3  User       - Normal user programs
 *
 * Lower numbers are more privileged.  A routine running in one mode
 * can only call into an equal or more privileged mode (never less
 * privileged).
 *
 * State is stored in the Per-Process Control Block (PCB).
 * - current_mode is per-thread (PCB is thread-local)
 * - privileges are process-wide (protected by PCB priv_lock)
 */

#include <stdint.h>
#include <pthread.h>

#include "ssdef.h"
#include "vms/pcb.h"

/* ------------------------------------------------------------------ */
/* Access mode constants                                              */
/* ------------------------------------------------------------------ */
#define PSL$C_KERNEL     0
#define PSL$C_EXEC       1
#define PSL$C_SUPER      2
#define PSL$C_USER       3

/* ------------------------------------------------------------------ */
/* Privilege bit definitions (matches VMS PRV$ symbols)               */
/* ------------------------------------------------------------------ */
#define PRV$M_CMKRNL     ((uint64_t)1 <<  0)
#define PRV$M_CMEXEC     ((uint64_t)1 <<  1)
#define PRV$M_SYSNAM     ((uint64_t)1 <<  2)
#define PRV$M_GRPNAM     ((uint64_t)1 <<  3)
#define PRV$M_ALLSPOOL   ((uint64_t)1 <<  4)
#define PRV$M_DETACH     ((uint64_t)1 <<  5)
#define PRV$M_DIAGNOSE   ((uint64_t)1 <<  6)
#define PRV$M_LOG_IO     ((uint64_t)1 <<  7)
#define PRV$M_GROUP      ((uint64_t)1 <<  8)
#define PRV$M_NOACNT     ((uint64_t)1 <<  9)
#define PRV$M_PRMCEB     ((uint64_t)1 << 10)
#define PRV$M_PRMGBL     ((uint64_t)1 << 11)
#define PRV$M_PRMMBX     ((uint64_t)1 << 12)
#define PRV$M_PSWAPM     ((uint64_t)1 << 13)
#define PRV$M_SETPRI     ((uint64_t)1 << 14)
#define PRV$M_SETPRV     ((uint64_t)1 << 15)
#define PRV$M_TMPMBX     ((uint64_t)1 << 16)
#define PRV$M_WORLD      ((uint64_t)1 << 17)
#define PRV$M_NETMBX     ((uint64_t)1 << 18)
#define PRV$M_VOLPRO     ((uint64_t)1 << 19)
#define PRV$M_PHY_IO     ((uint64_t)1 << 20)
#define PRV$M_BUGCHK     ((uint64_t)1 << 21)
#define PRV$M_PRMJNL     ((uint64_t)1 << 22)
#define PRV$M_OPER       ((uint64_t)1 << 23)
#define PRV$M_EXQUOTA    ((uint64_t)1 << 24)
#define PRV$M_BYPASS     ((uint64_t)1 << 25)
#define PRV$M_SYSGBL     ((uint64_t)1 << 26)
#define PRV$M_SYSLCK     ((uint64_t)1 << 27)
#define PRV$M_SHARE      ((uint64_t)1 << 28)
#define PRV$M_UPGRADE    ((uint64_t)1 << 29)
#define PRV$M_DOWNGRADE  ((uint64_t)1 << 30)
#define PRV$M_SECURITY   ((uint64_t)1 << 31)
#define PRV$M_ACNT       ((uint64_t)1 << 32)
#define PRV$M_ALTPRI     ((uint64_t)1 << 33)
#define PRV$M_READALL    ((uint64_t)1 << 34)
#define PRV$M_IMPORT     ((uint64_t)1 << 35)
#define PRV$M_AUDIT      ((uint64_t)1 << 36)
#define PRV$M_SYSPRV     ((uint64_t)1 << 37)

#define PRV$M_ALL        ((uint64_t)0x3FFFFFFFFF)

/* ------------------------------------------------------------------ */
/* Access mode API                                                    */
/* ------------------------------------------------------------------ */

uint8_t access_mode_get(void)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return PSL$C_USER;
    return pcb->current_mode;
}

uint32_t access_mode_set(uint8_t mode)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return SS$_BADPARAM;

    if (mode > PSL$C_USER)
        return SS$_BADPARAM;

    /* Moving to less privileged mode is always OK */
    if (mode >= pcb->current_mode) {
        pcb->current_mode = mode;
        return SS$_NORMAL;
    }

    /* Moving to more privileged mode needs privilege */
    uint64_t privs;
    pthread_mutex_lock(&pcb->priv_lock);
    privs = pcb->cur_privs;
    pthread_mutex_unlock(&pcb->priv_lock);

    switch (mode) {
    case PSL$C_KERNEL:
        if (!(privs & PRV$M_CMKRNL))
            return SS$_NOPRIV;
        break;
    case PSL$C_EXEC:
        if (!(privs & PRV$M_CMEXEC))
            return SS$_NOPRIV;
        break;
    case PSL$C_SUPER:
        break;
    default:
        break;
    }

    pcb->current_mode = mode;
    return SS$_NORMAL;
}

int access_mode_check(uint8_t required)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return 0;
    return pcb->current_mode <= required;
}

/* ------------------------------------------------------------------ */
/* Privilege API                                                      */
/* ------------------------------------------------------------------ */

int priv_check(uint64_t priv)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return 0;

    int result;
    pthread_mutex_lock(&pcb->priv_lock);
    result = (pcb->cur_privs & priv) ? 1 : 0;
    pthread_mutex_unlock(&pcb->priv_lock);
    return result;
}

uint64_t priv_set(uint64_t mask, int enable)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return 0;

    uint64_t prev;

    pthread_mutex_lock(&pcb->priv_lock);
    prev = pcb->cur_privs;

    if (enable) {
        if ((pcb->cur_privs & PRV$M_SETPRV) || pcb->current_mode == PSL$C_KERNEL) {
            pcb->cur_privs |= mask;
        }
    } else {
        pcb->cur_privs &= ~mask;
    }

    pthread_mutex_unlock(&pcb->priv_lock);
    return prev;
}

uint64_t priv_get_all(void)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return 0;

    uint64_t val;
    pthread_mutex_lock(&pcb->priv_lock);
    val = pcb->cur_privs;
    pthread_mutex_unlock(&pcb->priv_lock);
    return val;
}

void priv_init(uint64_t initial_privs)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return;

    pthread_mutex_lock(&pcb->priv_lock);
    pcb->cur_privs = initial_privs;
    pcb->perm_privs = initial_privs;
    pthread_mutex_unlock(&pcb->priv_lock);
}
