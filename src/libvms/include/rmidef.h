/*
 * RMIDEF.H - VMS Remote Management Interface (RMI$) Item Codes
 *
 * OpenVMX compatibility layer - Defines the RMI$_ item codes used
 * with sys$getrmi item lists to retrieve system performance counters
 * (CPU idle ticks, process-state counts, ...).
 *
 * PROVENANCE: the $GETRMI item-list mechanism is documented in the
 * public OpenVMS System Services Reference Manual ($GETRMI). Exact
 * numeric values not confirmed against a fetchable public source
 * this session — sequential OVMX assignment, flagged in vms-531
 * findings.
 *
 * Reference: OpenVMS System Services Reference Manual ($GETRMI)
 */

#ifndef __RMIDEF_H
#define __RMIDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * RMI$_ item codes for SYS$GETRMI item lists
 * ================================================================ */

#define RMI$_CPUIDLE    0x0001  /* Cumulative CPU idle ticks (Q) */
#define RMI$_HIB        0x0002  /* Number of processes in HIB state (L) */

#ifdef __cplusplus
}
#endif

#endif /* __RMIDEF_H */
