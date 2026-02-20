/*
 * UTCBLKDEF.H - VMS UTC Time Block Definitions
 *
 * OpenVMX compatibility layer - Defines the UTCBLK structure and UTC$_
 * constants for the 128-bit UTC (Coordinated Universal Time) block used
 * in VMS time services on Alpha and later systems.
 *
 * The UTCBLK is a 128-bit (16-byte) structure holding a UTC timestamp
 * as a 64-bit time value plus a 64-bit inaccuracy field.  This format
 * is used with SYS$GETTIM_PREC, SYS$NUMTIM_PREC, and certain ACM
 * (Access Control Management) service parameters.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Programming Concepts Manual — Time Services
 */

#ifndef __UTCBLKDEF_H
#define __UTCBLKDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * UTCBLK — 128-bit UTC Time Block structure
 *
 * Two 64-bit fields:
 *   utc$q_time        — 100-nanosecond ticks since the VMS epoch
 *                       (17-NOV-1858 00:00:00.000000000)
 *   utc$q_inaccuracy  — Inaccuracy/uncertainty of the time value
 *                       (same units; 0 = perfectly accurate)
 * ================================================================ */

struct _utcblk {
    int64_t utc$q_time;         /* 64-bit UTC time value (100ns ticks) */
    int64_t utc$q_inaccuracy;   /* 64-bit time inaccuracy */
};

typedef struct _utcblk UTCBLK;

/* ================================================================
 * UTC$K_ — UTCBLK size constant
 * ================================================================ */

#define UTC$K_LENGTH    16  /* Size of UTCBLK structure (bytes) */
#define UTC$C_LENGTH    UTC$K_LENGTH

#ifdef __cplusplus
}
#endif

#endif /* __UTCBLKDEF_H */
