/*
 * AFRDEF.H - VMS Alignment Fault Report Definitions
 *
 * OpenVMX compatibility layer - Defines the AFRDEF structure and AFR$_
 * constants used by sys$start_align_fault_report, sys$stop_align_fault_report,
 * and sys$get_align_fault_data.
 *
 * Reference: OpenVMS System Services Reference Manual
 */

#ifndef __AFRDEF_H
#define __AFRDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * AFR$C_ — Mode constants for sys$start_align_fault_report
 * ================================================================ */

#define AFR$C_EXCEPTION     1   /* Report as informational exception */
#define AFR$C_BUFFERED      2   /* Report into caller-supplied buffer */

/* ================================================================
 * AFR$K_USER_LENGTH / AFR$C_USER_LENGTH — size of one AFR record
 * ================================================================ */

#define AFR$K_USER_LENGTH   16  /* Bytes per alignment fault record */
#define AFR$C_USER_LENGTH   AFR$K_USER_LENGTH

/* ================================================================
 * AFRDEF — Alignment Fault Record structure
 *
 * Each record returned by sys$get_align_fault_data describes one
 * alignment fault event.
 * ================================================================ */

struct _afrdef {
    uint32_t afr$l_fault_pc_l;  /* Low 32 bits of faulting PC */
    uint32_t afr$l_fault_pc_h;  /* High 32 bits of faulting PC (Alpha) */
    uint32_t afr$l_fault_va_l;  /* Low 32 bits of faulting virtual address */
    uint32_t afr$l_fault_va_h;  /* High 32 bits of faulting VA (Alpha) */
};

typedef struct _afrdef AFRDEF;

#ifdef __cplusplus
}
#endif

#endif /* __AFRDEF_H */
