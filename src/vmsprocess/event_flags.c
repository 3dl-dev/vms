/*
 * event_flags.c - VMS Event Flag emulation
 *
 * VMS event flags are single-bit synchronization primitives organized
 * into clusters of 32.  Local clusters 0 (flags 0-31) and 1 (flags
 * 32-63) are per-process.  Common event flags (64-127) are shared
 * between cooperating processes (currently emulated as process-local).
 *
 * State is stored in the Per-Process Control Block (PCB).
 * Waiting uses PCB's pthread condition variable.
 */

#include <string.h>
#include <pthread.h>

#include "vms/eflag.h"
#include "vms/pcb.h"
#include "ssdef.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static int efn_to_cluster(uint32_t efn, int *cluster_idx, int *bit)
{
    if (efn >= EFN_MAX_TOTAL)
        return -1;

    *cluster_idx = (int)(efn / 32);
    *bit         = (int)(efn % 32);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void eflag_init(void)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return;

    pthread_mutex_lock(&pcb->ef_lock);
    memset(pcb->ef_clusters, 0, sizeof(pcb->ef_clusters));
    pthread_mutex_unlock(&pcb->ef_lock);
}

int eflag_set(uint32_t efn)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return SS$_ILLEFC;

    int cidx, bit;
    if (efn_to_cluster(efn, &cidx, &bit) < 0)
        return SS$_ILLEFC;

    uint32_t mask = (uint32_t)1 << bit;
    int was_set;

    pthread_mutex_lock(&pcb->ef_lock);
    was_set = (pcb->ef_clusters[cidx] & mask) ? 1 : 0;
    pcb->ef_clusters[cidx] |= mask;
    pthread_cond_broadcast(&pcb->ef_cond);
    pthread_mutex_unlock(&pcb->ef_lock);

    return was_set ? SS$_WASSET : SS$_WASCLR;
}

int eflag_clear(uint32_t efn)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return SS$_ILLEFC;

    int cidx, bit;
    if (efn_to_cluster(efn, &cidx, &bit) < 0)
        return SS$_ILLEFC;

    uint32_t mask = (uint32_t)1 << bit;
    int was_set;

    pthread_mutex_lock(&pcb->ef_lock);
    was_set = (pcb->ef_clusters[cidx] & mask) ? 1 : 0;
    pcb->ef_clusters[cidx] &= ~mask;
    pthread_mutex_unlock(&pcb->ef_lock);

    return was_set ? SS$_WASSET : SS$_WASCLR;
}

int eflag_read(uint32_t efn)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return SS$_ILLEFC;

    int cidx, bit;
    if (efn_to_cluster(efn, &cidx, &bit) < 0)
        return SS$_ILLEFC;

    uint32_t mask = (uint32_t)1 << bit;
    int val;

    pthread_mutex_lock(&pcb->ef_lock);
    val = (pcb->ef_clusters[cidx] & mask) ? 1 : 0;
    pthread_mutex_unlock(&pcb->ef_lock);

    return val;
}

uint32_t eflag_read_cluster(uint32_t efn)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return 0;

    int cidx, bit;
    if (efn_to_cluster(efn, &cidx, &bit) < 0)
        return 0;

    uint32_t val;
    pthread_mutex_lock(&pcb->ef_lock);
    val = pcb->ef_clusters[cidx];
    pthread_mutex_unlock(&pcb->ef_lock);

    return val;
}

int eflag_wait(uint32_t efn)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return SS$_ILLEFC;

    int cidx, bit;
    if (efn_to_cluster(efn, &cidx, &bit) < 0)
        return SS$_ILLEFC;

    uint32_t mask = (uint32_t)1 << bit;

    pthread_mutex_lock(&pcb->ef_lock);
    while (!(pcb->ef_clusters[cidx] & mask))
        pthread_cond_wait(&pcb->ef_cond, &pcb->ef_lock);
    pthread_mutex_unlock(&pcb->ef_lock);

    return SS$_NORMAL;
}

int eflag_wait_or(uint32_t base_efn, uint32_t mask)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return SS$_ILLEFC;

    int cidx, bit;
    if (efn_to_cluster(base_efn, &cidx, &bit) < 0)
        return SS$_ILLEFC;
    if (bit != 0)
        return SS$_ILLEFC;

    pthread_mutex_lock(&pcb->ef_lock);
    while ((pcb->ef_clusters[cidx] & mask) == 0)
        pthread_cond_wait(&pcb->ef_cond, &pcb->ef_lock);
    pthread_mutex_unlock(&pcb->ef_lock);

    return SS$_NORMAL;
}

int eflag_wait_and(uint32_t base_efn, uint32_t mask)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return SS$_ILLEFC;

    int cidx, bit;
    if (efn_to_cluster(base_efn, &cidx, &bit) < 0)
        return SS$_ILLEFC;
    if (bit != 0)
        return SS$_ILLEFC;

    pthread_mutex_lock(&pcb->ef_lock);
    while ((pcb->ef_clusters[cidx] & mask) != mask)
        pthread_cond_wait(&pcb->ef_cond, &pcb->ef_lock);
    pthread_mutex_unlock(&pcb->ef_lock);

    return SS$_NORMAL;
}
