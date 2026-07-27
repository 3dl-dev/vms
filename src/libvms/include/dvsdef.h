/*
 * DVSDEF.H - VMS Device Scan Item Codes
 *
 * OpenVMX compatibility layer - Defines the DVS$_ item codes used
 * with sys$device_scan to filter the devices returned by wildcard
 * device-name matching.
 *
 * PROVENANCE: item-code mechanism documented in the public OpenVMS
 * System Services Reference Manual ($DEVICE_SCAN). Exact numeric
 * value not confirmed against a fetchable public source this
 * session — sequential OVMX assignment, flagged in vms-531 findings.
 *
 * Reference: OpenVMS System Services Reference Manual ($DEVICE_SCAN)
 */

#ifndef __DVSDEF_H
#define __DVSDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * DVS$_ item codes for SYS$DEVICE_SCAN item lists
 * ================================================================ */

#define DVS$_DEVCLASS    0x0001  /* Filter by device class code (L) — see dcdef.h */

#ifdef __cplusplus
}
#endif

#endif /* __DVSDEF_H */
