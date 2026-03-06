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
#include "prvdef.h"

/* ------------------------------------------------------------------ */
/* Access mode constants                                              */
/* ------------------------------------------------------------------ */
#define PSL$C_KERNEL     0
#define PSL$C_EXEC       1
#define PSL$C_SUPER      2
#define PSL$C_USER       3

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
        /* current_mode is safe to read here without a separate lock:
         * the PCB is thread-local, so only this thread writes current_mode.
         * priv_lock protects cur_privs against concurrent priv_check(). */
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
