/*
 * RMS.H - VMS Record Management Services Master Include
 *
 * OpenVMX compatibility layer - Traditional one-stop include for all
 * RMS programming on VMS.  On real VMS, programs include <rms.h> to
 * get the complete RMS API: FAB, RAB, NAM, XAB block definitions,
 * initialization macros (cc$rms_fab, cc$rms_rab, etc.), and RMS
 * status codes.
 *
 * This header is a convenience wrapper.  The actual block definitions
 * live in src/vmsrms/include/rms/ and are accessible via the compiler
 * include path (-I.../src/vmsrms/include/rms).  This header pulls them
 * all together in one #include, matching the VMS convention.
 *
 * Usage:
 *   #include <rms.h>
 *
 * Provides:
 *   - struct FAB and FAB$* constants  (file access block)
 *   - struct RAB and RAB$* constants  (record access block)
 *   - struct NAM and NAM$* constants  (name block)
 *   - struct XABKEY, XABDAT, XABPRO   (extended attribute blocks)
 *   - cc$rms_fab, cc$rms_rab, cc$rms_nam initializers
 *   - RMS$_ status codes              (via rmsdef.h)
 *   - RMS file/record service prototypes (sys$open, sys$get, etc.)
 *
 * Reference: OpenVMS Record Management Services Reference Manual
 *            Guide to OpenVMS File Applications
 */

#ifndef __RMS_H
#define __RMS_H

#include <stdint.h>

/* Pull in the individual RMS block definitions */
#include "fab.h"
#include "rab.h"
#include "xab.h"
#include "nam.h"

/* RMS status codes */
#include "rmsdef.h"

/* RMS file-level service prototypes */
uint32_t sys$open(void *fab);           /* Open existing file */
uint32_t sys$close(void *fab);          /* Close file */
uint32_t sys$create(void *fab);         /* Create new file */
uint32_t sys$erase(void *fab);          /* Delete file */
uint32_t sys$display(void *fab);        /* Display file attributes */

/* Stream (record context) service prototypes */
uint32_t sys$connect(void *rab);        /* Connect RAB to FAB */
uint32_t sys$disconnect(void *rab);     /* Disconnect RAB from FAB */

/* Record-level service prototypes */
uint32_t sys$get(void *rab);            /* Read a record */
uint32_t sys$put(void *rab);            /* Write a record */
uint32_t sys$update(void *rab);         /* Update current record */
uint32_t sys$delete(void *rab);         /* Delete current record */
uint32_t sys$find(void *rab);           /* Position to record without reading */

/* Filespec service prototypes */
uint32_t sys$parse(void *fab);          /* Parse filespec into NAM block */
uint32_t sys$search(void *fab);         /* Search for next wildcard match */

/* Positioning and I/O control */
uint32_t sys$rewind(void *rab);         /* Rewind to beginning of file */
uint32_t sys$flush(void *rab);          /* Flush buffers to disk */

#endif /* __RMS_H */
