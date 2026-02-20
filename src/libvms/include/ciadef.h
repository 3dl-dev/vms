/*
 * CIADEF.H - VMS Breakin (CIA) Record Definitions
 *
 * OpenVMX compatibility layer - Defines the CIA$M_ bit-mask constants
 * used with the intrusion/breakin database accessed via the security
 * server (sys$show_intrusion, SECSRV$_SHOW_INTRUDER).
 *
 * CIA stands for "CIA breakin database" — the OpenVMS intrusion detection
 * facility that tracks failed login attempts.
 *
 * Reference: OpenVMS System Manager's Manual
 *            OpenVMS Security Reference Manual
 */

#ifndef __CIADEF_H
#define __CIADEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * CIA$M_ — Breakin record type bit masks
 *
 * These bits appear in the "type" field of the breakin record
 * returned by the security server.
 * ================================================================ */

#define CIA$M_NETWORK       0x0001  /* Network-based intrusion attempt */
#define CIA$M_TERMINAL      0x0002  /* Terminal-based intrusion attempt */
#define CIA$M_USERNAME      0x0004  /* Username-based intrusion attempt */
#define CIA$M_TERM_USER     0x0008  /* Terminal+username combined entry */

#define CIA$M_INTRUDER      0x0100  /* Entry is classified as intruder */
#define CIA$M_SUSPECT       0x0200  /* Entry is classified as suspect */

/* ================================================================
 * CIA$C_ — Breakin record context codes
 * ================================================================ */

#define CIA$C_NETWORK       1   /* Network breakin context */
#define CIA$C_TERMINAL      2   /* Terminal breakin context */
#define CIA$C_USERNAME      3   /* Username breakin context */

#ifdef __cplusplus
}
#endif

#endif /* __CIADEF_H */
