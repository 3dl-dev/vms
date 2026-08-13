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

/*
 * Every RMS service takes the VMS three-argument form  SYS$xxx cb ,[err] ,[suc]
 * (VSI OpenVMS Record Management Services Reference Manual, Part III):
 *   cb  — control-block address (FAB for file ops, RAB for record ops)
 *   err — AST-level error   completion routine entry mask (or 0/NULL)
 *   suc — AST-level success completion routine entry mask (or 0/NULL)
 * OVMX RMS completes synchronously; a supplied completion routine is invoked
 * with the control-block address before the service returns.
 */

/* File-level operations */
uint32_t sys$open(void *fab, void (*err)(void *), void (*suc)(void *));       /* Open existing file */
uint32_t sys$close(void *fab, void (*err)(void *), void (*suc)(void *));      /* Close file */
uint32_t sys$create(void *fab, void (*err)(void *), void (*suc)(void *));     /* Create new file */
uint32_t sys$erase(void *fab, void (*err)(void *), void (*suc)(void *));      /* Delete file */
uint32_t sys$display(void *fab, void (*err)(void *), void (*suc)(void *));    /* Display file attributes */
uint32_t sys$extend(void *fab, void (*err)(void *), void (*suc)(void *));     /* Extend file allocation */

/* Stream (record context) operations */
uint32_t sys$connect(void *rab, void (*err)(void *), void (*suc)(void *));    /* Connect RAB to FAB (open stream) */
uint32_t sys$disconnect(void *rab, void (*err)(void *), void (*suc)(void *)); /* Disconnect RAB from FAB */

/* Record-level operations */
uint32_t sys$get(void *rab, void (*err)(void *), void (*suc)(void *));        /* Read a record */
uint32_t sys$put(void *rab, void (*err)(void *), void (*suc)(void *));        /* Write a record */
uint32_t sys$update(void *rab, void (*err)(void *), void (*suc)(void *));     /* Update current record */
uint32_t sys$delete(void *rab, void (*err)(void *), void (*suc)(void *));     /* Delete current record */
uint32_t sys$find(void *rab, void (*err)(void *), void (*suc)(void *));       /* Position to record without reading */

/* Filespec operations */
uint32_t sys$parse(void *fab, void (*err)(void *), void (*suc)(void *));      /* Parse filespec into NAM block */
uint32_t sys$search(void *fab, void (*err)(void *), void (*suc)(void *));     /* Search for next wildcard match */

/* Positioning and I/O control */
uint32_t sys$rewind(void *rab, void (*err)(void *), void (*suc)(void *));     /* Rewind to beginning of file */
uint32_t sys$flush(void *rab, void (*err)(void *), void (*suc)(void *));      /* Flush buffers to disk */

#endif /* __RMS_RMS_H */
