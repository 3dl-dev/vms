/*
 * sys_event.c - Event Flag System Services
 *
 * VMS event flags are single-bit semaphores used for synchronisation.
 * Each process has 128 event flags in 4 clusters of 32:
 *   Cluster 0 (flags 0-31):   local
 *   Cluster 1 (flags 32-63):  local
 *   Cluster 2 (flags 64-95):  common (shared between processes)
 *   Cluster 3 (flags 96-127): common
 *
 * Event flag state is stored in the Per-Process Control Block (PCB).
 * Thread-safe waiting uses PCB's pthread mutex/condition variable.
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "starlet.h"
#include "vms/pcb.h"

/* Total event flags supported (4 clusters x 32) */
#define EF_TOTAL 128

/*
 * sys$setef - Set an event flag.
 *
 * Sets the specified event flag bit. If any threads are waiting
 * on this flag (via $WAITFR, $WFLOR, $WFLAND), they are woken.
 *
 * Returns:
 *   SS$_WASSET - Flag was already set before this call
 *   SS$_WASCLR - Flag was clear before this call (now set)
 *   SS$_ILLEFC - Event flag number out of range
 */
uint32_t sys$setef(uint32_t efn) {
    if (efn >= EF_TOTAL) return SS$_ILLEFC;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_ILLEFC;

    int cidx = (int)(efn / 32);
    int bit  = (int)(efn % 32);
    uint32_t mask = (uint32_t)1 << bit;

    pthread_mutex_lock(&pcb->ef_lock);
    uint32_t was_set = (pcb->ef_clusters[cidx] & mask) ? 1 : 0;
    pcb->ef_clusters[cidx] |= mask;
    pthread_cond_broadcast(&pcb->ef_cond);
    pthread_mutex_unlock(&pcb->ef_lock);

    return was_set ? SS$_WASSET : SS$_WASCLR;
}

/*
 * sys$clref - Clear an event flag.
 *
 * Returns:
 *   SS$_WASSET - Flag was set before clearing
 *   SS$_WASCLR - Flag was already clear
 *   SS$_ILLEFC - Event flag number out of range
 */
uint32_t sys$clref(uint32_t efn) {
    if (efn >= EF_TOTAL) return SS$_ILLEFC;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_ILLEFC;

    int cidx = (int)(efn / 32);
    int bit  = (int)(efn % 32);
    uint32_t mask = (uint32_t)1 << bit;

    pthread_mutex_lock(&pcb->ef_lock);
    uint32_t was_set = (pcb->ef_clusters[cidx] & mask) ? 1 : 0;
    pcb->ef_clusters[cidx] &= ~mask;
    pthread_mutex_unlock(&pcb->ef_lock);

    return was_set ? SS$_WASSET : SS$_WASCLR;
}

/*
 * sys$waitfr - Wait for a single event flag to be set.
 *
 * If the flag is already set, returns immediately.
 * Otherwise blocks until another thread/AST sets the flag.
 *
 * Returns:
 *   SS$_NORMAL - Flag is set
 *   SS$_ILLEFC - Event flag number out of range
 */
uint32_t sys$waitfr(uint32_t efn) {
    if (efn >= EF_TOTAL) return SS$_ILLEFC;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_ILLEFC;

    int cidx = (int)(efn / 32);
    int bit  = (int)(efn % 32);
    uint32_t mask = (uint32_t)1 << bit;

    pthread_mutex_lock(&pcb->ef_lock);
    while (!(pcb->ef_clusters[cidx] & mask))
        pthread_cond_wait(&pcb->ef_cond, &pcb->ef_lock);
    pthread_mutex_unlock(&pcb->ef_lock);

    return SS$_NORMAL;
}

/*
 * sys$wflor - Wait for any flags in a mask to be set (logical OR).
 *
 * efn selects the cluster (efn / 32 gives cluster index).
 * mask is the 32-bit pattern of flags within that cluster to wait on.
 * Returns as soon as ANY of the masked flags become set.
 */
uint32_t sys$wflor(uint32_t efn, uint32_t mask) {
    if (efn >= EF_TOTAL) return SS$_ILLEFC;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_ILLEFC;

    int cidx = (int)(efn / 32);

    pthread_mutex_lock(&pcb->ef_lock);
    while ((pcb->ef_clusters[cidx] & mask) == 0)
        pthread_cond_wait(&pcb->ef_cond, &pcb->ef_lock);
    pthread_mutex_unlock(&pcb->ef_lock);

    return SS$_NORMAL;
}

/*
 * sys$wfland - Wait for all flags in a mask to be set (logical AND).
 *
 * Same cluster selection as $WFLOR, but waits until ALL masked
 * flags are set simultaneously.
 */
uint32_t sys$wfland(uint32_t efn, uint32_t mask) {
    if (efn >= EF_TOTAL) return SS$_ILLEFC;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_ILLEFC;

    int cidx = (int)(efn / 32);

    pthread_mutex_lock(&pcb->ef_lock);
    while ((pcb->ef_clusters[cidx] & mask) != mask)
        pthread_cond_wait(&pcb->ef_cond, &pcb->ef_lock);
    pthread_mutex_unlock(&pcb->ef_lock);

    return SS$_NORMAL;
}

/*
 * sys$readef - Read event flag state.
 *
 * Reads the current state of a 32-flag cluster and optionally
 * returns it in *state. Also indicates whether the specific
 * flag efn is set or clear.
 *
 * Returns:
 *   SS$_WASSET - The specific flag is set
 *   SS$_WASCLR - The specific flag is clear
 *   SS$_ILLEFC - Event flag number out of range
 */
uint32_t sys$readef(uint32_t efn, uint32_t *state) {
    if (efn >= EF_TOTAL) return SS$_ILLEFC;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_ILLEFC;

    int cidx = (int)(efn / 32);
    int bit  = (int)(efn % 32);
    uint32_t mask = (uint32_t)1 << bit;

    pthread_mutex_lock(&pcb->ef_lock);
    if (state) {
        *state = pcb->ef_clusters[cidx];
    }
    uint32_t was_set = (pcb->ef_clusters[cidx] & mask) ? 1 : 0;
    pthread_mutex_unlock(&pcb->ef_lock);

    return was_set ? SS$_WASSET : SS$_WASCLR;
}

/* --- Common event flag cluster stubs --- */

/*
 * sys$ascefc - Associate Common Event Flag Cluster (stub).
 */
uint32_t sys$ascefc(uint32_t efn, const struct dsc$descriptor_s *name,
                    uint32_t prot, uint32_t perm) {
    (void)efn; (void)name; (void)prot; (void)perm;
    return SS$_NORMAL;  /* TODO: shared memory based common EF clusters */
}

/*
 * sys$dacefc - Disassociate Common Event Flag Cluster (stub).
 */
uint32_t sys$dacefc(uint32_t efn) {
    (void)efn;
    return SS$_NORMAL;
}

/*
 * sys$dlcefc - Delete Common Event Flag Cluster (stub).
 */
uint32_t sys$dlcefc(const struct dsc$descriptor_s *name) {
    (void)name;
    return SS$_NORMAL;
}
