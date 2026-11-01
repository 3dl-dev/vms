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
 * The special value EFN$C_ENF (128, the V7.3 $EFNDEF value) tells system services not to
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
#define EFN$C_ENF           128     /* No event flag (do not set any EF); V7.3 $EFNDEF */

#ifdef __cplusplus
}
#endif

#endif /* __EFNDEF_H */
