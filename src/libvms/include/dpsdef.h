/*
 * DPSDEF.H - VMS Device Path Scan Item Code Definitions
 *
 * OpenVMX compatibility layer - Defines the DPS$_ item codes used with
 * sys$device_path_scan to enumerate the I/O paths available to a
 * multipath disk device.
 *
 * Reference: OpenVMS System Services Reference Manual
 */

#ifndef __DPSDEF_H
#define __DPSDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * DPS$_ — Item codes for sys$device_path_scan item list
 * ================================================================ */

#define DPS$_MP_PATHNAME    1   /* Multipath device path name (string) */

#ifdef __cplusplus
}
#endif

#endif /* __DPSDEF_H */
