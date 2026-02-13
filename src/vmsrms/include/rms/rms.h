#ifndef __RMS_RMS_H
#define __RMS_RMS_H

/*
 * RMS Public Interface - Master include for Record Management Services
 *
 * This header provides the complete RMS API for VMS-compatible
 * file and record operations on the OVMX system.
 */

#include <stdint.h>

#include "fab.h"
#include "rab.h"
#include "xab.h"
#include "nam.h"

#include "rmsdef.h"
#include "ssdef.h"

/* File-level operations */
uint32_t sys$open(void *fab);          /* Open existing file */
uint32_t sys$close(void *fab);         /* Close file */
uint32_t sys$create(void *fab);        /* Create new file */
uint32_t sys$erase(void *fab);         /* Delete file */
uint32_t sys$display(void *fab);       /* Display file attributes */

/* Stream (record context) operations */
uint32_t sys$connect(void *rab);       /* Connect RAB to FAB (open stream) */
uint32_t sys$disconnect(void *rab);    /* Disconnect RAB from FAB */

/* Record-level operations */
uint32_t sys$get(void *rab);           /* Read a record */
uint32_t sys$put(void *rab);           /* Write a record */
uint32_t sys$update(void *rab);        /* Update current record */
uint32_t sys$delete(void *rab);        /* Delete current record */
uint32_t sys$find(void *rab);          /* Position to record without reading */

/* Filespec operations */
uint32_t sys$parse(void *fab);         /* Parse filespec into NAM block */
uint32_t sys$search(void *fab);        /* Search for next wildcard match */

/* Positioning and I/O control */
uint32_t sys$rewind(void *rab);        /* Rewind to beginning of file */
uint32_t sys$flush(void *rab);         /* Flush buffers to disk */

#endif /* __RMS_RMS_H */
