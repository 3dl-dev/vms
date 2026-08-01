#ifndef __VMS_EFLAG_H
#define __VMS_EFLAG_H

#include <stdint.h>

/* Event flag clusters */
#define EFN_LOCAL_0  0    /* Local cluster 0: flags 0-31 */
#define EFN_LOCAL_1  32   /* Local cluster 1: flags 32-63 */
/* Flags 64-127 are common (shared) event flags */
#define EFN_COMMON_BASE 64

#define EFN_MAX_LOCAL   64
#define EFN_MAX_COMMON  64
#define EFN_MAX_TOTAL   128

/* Initialize event flag system */
void eflag_init(void);

/* Set an event flag, return previous state */
int eflag_set(uint32_t efn);

/* Clear an event flag, return previous state */
int eflag_clear(uint32_t efn);

/* Read an event flag */
int eflag_read(uint32_t efn);

/* Read entire cluster state */
uint32_t eflag_read_cluster(uint32_t efn);

/* Wait for a single event flag */
int eflag_wait(uint32_t efn);

/* Wait for any flags in mask (OR wait) */
int eflag_wait_or(uint32_t base_efn, uint32_t mask);

/* Wait for all flags in mask (AND wait) */
int eflag_wait_and(uint32_t base_efn, uint32_t mask);

#endif
