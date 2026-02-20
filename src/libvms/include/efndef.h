/*
 * EFNDEF.H - VMS Event Flag Number Definitions
 *
 * OpenVMX compatibility layer - Defines the EFN$_ constants used
 * to specify event flags in system services such as SYS$QIO,
 * SYS$QIOW, SYS$SETIMR, SYS$GETJPIW, etc.
 *
 * VMS event flags are numbered 0-127:
 *   Flags  0-63:  Local event flags (private to the process)
 *   Flags 64-127: Common event flags (shared across processes)
 *
 * The special value EFN$C_ENF (0) tells system services not to
 * set any event flag on completion.  This is the most common
 * value used in synchronous "W" variants (SYS$QIOW, etc.) where
 * the caller blocks on the IOSB status directly.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Programming Concepts Manual, Chapter 9
 */

#ifndef __EFNDEF_H
#define __EFNDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Special event flag values
 * ================================================================ */

/*
 * EFN$C_ENF - No event flag
 *
 * Passing this value as the efn argument to a system service
 * requests that no event flag be set upon completion.  The
 * service still completes normally; the caller uses the IOSB
 * or return status to determine outcome.
 */
#define EFN$C_ENF           0       /* No event flag (do not set any EF) */

/* ================================================================
 * Local event flag cluster boundaries
 *
 * Each cluster holds 32 event flags.  Local clusters are private
 * to the process; common clusters are shared.
 * ================================================================ */

#define EFN$C_LOCAL_MIN     0       /* First local event flag number */
#define EFN$C_LOCAL_MAX     63      /* Last local event flag number */

#define EFN$C_CLUSTER_0_MIN 0       /* Local cluster 0: flags  0-31 */
#define EFN$C_CLUSTER_0_MAX 31
#define EFN$C_CLUSTER_1_MIN 32      /* Local cluster 1: flags 32-63 */
#define EFN$C_CLUSTER_1_MAX 63

/* ================================================================
 * Common event flag cluster boundaries
 *
 * Common event flags are associated with a named common event
 * flag cluster (CEFC) shared between cooperating processes.
 * ================================================================ */

#define EFN$C_COMMON_MIN    64      /* First common event flag number */
#define EFN$C_COMMON_MAX    127     /* Last common event flag number */

#define EFN$C_CLUSTER_2_MIN 64      /* Common cluster 2: flags  64-95 */
#define EFN$C_CLUSTER_2_MAX 95
#define EFN$C_CLUSTER_3_MIN 96      /* Common cluster 3: flags 96-127 */
#define EFN$C_CLUSTER_3_MAX 127

/* ================================================================
 * Event flag cluster size
 * ================================================================ */

#define EFN$C_CLUSTER_SIZE  32      /* Flags per cluster */

#ifdef __cplusplus
}
#endif

#endif /* __EFNDEF_H */
