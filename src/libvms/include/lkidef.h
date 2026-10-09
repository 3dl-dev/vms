/*
 * LKIDEF.H - VMS Lock Information (LKI$) Item Codes and Constants
 *
 * OpenVMX compatibility layer - Defines the LKI$_ item codes used
 * with sys$getlki/sys$getlkiw (Get Lock Information) item lists, and
 * the LKI$C_ lock-state constants returned via LKI$_STATE.
 *
 * PROVENANCE (vms-b71): every numeric value below is the real STARLET.MLB
 * value `LIBRARY/MACRO/EXTRACT=$LKIDEF SYS$LIBRARY:STARLET.MLB` printed on a
 * live OpenVMS VAX V7.3 node and a live Alpha V8.4 node (both agree) --
 * docs/oracle/vax73-starlet-defs/LKIDEF.txt,
 * docs/oracle/alpha84-starlet-defs/LKIDEF.txt -- checked by
 * tools/compat/check_oracle_constants.py. Rule 8: these are the PUBLISHED
 * item-code numbers, never an OVMX sequential guess. The vms-531-era
 * disclaimer this comment used to carry ("not confirmed ... sequential OVMX
 * assignment") was itself WRONG for PID/STATE/RESNAM/GRANTCOUNT (they
 * already matched the oracle by coincidence) and was caught by the gate for
 * LOCKID when vms-b71 widened this file with an invented 0x102 that
 * collided with nothing real -- the oracle's actual LOCKID is 260, and 258
 * is LKI$_PARENT, a different item entirely.
 *
 * LKI$_GRMODE and LKI$_RQMODE DO NOT EXIST as top-level $GETLKI item codes
 * on real VMS -- they were an OVMX invention this round caught and removed.
 * Real VMS reports the granted mode, requested mode and queue state packed
 * into the 3-byte STATEF structure LKI$_STATE returns (LKI$B_STATE_RQMODE/
 * GRMODE/QUEUE below), not as separate items.
 *
 * Reference: OpenVMS System Services Reference Manual ($GETLKI)
 *            OpenVMS Programming Concepts Manual, Lock Management
 */

#ifndef __LKIDEF_H
#define __LKIDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * LKI$_ item codes for SYS$GETLKI/SYS$GETLKIW item lists
 * ================================================================ */

#define LKI$_PID           256  /* PID of lock holder (L) */
#define LKI$_STATE         257  /* Lock state -- a packed 3-byte STATEF
                                 * structure (LKI$B_STATE_RQMODE/GRMODE/QUEUE
                                 * below), not a scalar */
#define LKI$_PARENT        258  /* The lock's PARENT lock ID, 0 if root (L)
                                 * -- vms-b71 */
#define LKI$_LOCKID        260  /* The lock's own lock ID (L) -- vms-b71 */
#define LKI$_RESNAM        513  /* Resource name (T) */
#define LKI$_GRANTCOUNT    523  /* Number of granted locks on resource (L) */

/* ================================================================
 * LKI$_STATE's STATEF substructure (LKI$S_STATEF == 3 bytes): byte offsets
 * of the requested mode, the granted mode and the queue-state code within
 * the 3-byte buffer LKI$_STATE fills.
 * ================================================================ */

#define LKI$B_STATE_RQMODE  0
#define LKI$B_STATE_GRMODE  1
#define LKI$B_STATE_QUEUE   2

/* ================================================================
 * LKI$C_ — queue-state values the STATEF QUEUE byte (above) carries
 * ================================================================ */

#define LKI$C_GRANTED    1   /* Lock is granted */
#define LKI$C_CONVERT    0   /* Lock is converting (waiting for a new mode) */

#ifdef __cplusplus
}
#endif

#endif /* __LKIDEF_H */
