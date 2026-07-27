/*
 * ACMEDEF.H - VMS Authentication and Credential Management (ACME$)
 *             Definitions
 *
 * OpenVMX compatibility layer - Defines the ACME$ item codes, function
 * codes, logon-type constants, status codes, and the ACMESB (ACME
 * status block) structure used with sys$acmw (e.g. to implement SET
 * PASSWORD-style password changes).
 *
 * PROVENANCE: the ACME$ item-list mechanism and general shape are
 * documented in the public OpenVMS Guide to System Security /
 * System Services Reference Manual ($ACM). Exact numeric values and
 * the byte-level ACMESB layout could NOT be confirmed against a
 * fetchable public source this session — sequential OVMX assignment
 * (clean-room, CLAUDE.md rule 8), flagged in vms-531 findings for
 * operator sign-off.
 *
 * Reference: OpenVMS Guide to System Security
 *            OpenVMS System Services Reference Manual ($ACM)
 */

#ifndef __ACMEDEF_H
#define __ACMEDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * ACME$_ item codes for SYS$ACMW item lists
 * ================================================================ */

#define ACME$_PRINCIPAL_NAME_IN     0x0001  /* Principal (user) name (T) */
#define ACME$_PASSWORD_1            0x0002  /* Current/primary password (T) */
#define ACME$_NEW_PASSWORD_1        0x0003  /* New primary password (T) */
#define ACME$_NEW_PASSWORD_FLAGS    0x0004  /* Flags for new password (L) */
#define ACME$_LOGON_TYPE            0x0005  /* Logon type (L) — see ACME$K_ below */

/* ================================================================
 * ACME$_FC_ — SYS$ACMW function codes
 * ================================================================ */

#define ACME$_FC_CHANGE_PASSWORD    1   /* Change-password function */

/* ================================================================
 * ACME$K_ — logon type constants
 * ================================================================ */

#define ACME$K_LOCAL    1   /* Local (interactive) logon */

/* ================================================================
 * ACME$_ — SYS$ACMW / password-change status codes
 * (encoded per the standard VMS condition-value layout: facility in
 * bits <27:16>, message number in bits <15:3>, severity in bits <2:0>.
 * Facility number 24 is an OVMX placeholder — see PROVENANCE above.)
 * ================================================================ */

#define ACME$_PWDINHISTORY    1572874   /* facility 24, msgnum 1, severity ERROR(2) */
#define ACME$_AUTHFAILURE     1572882   /* facility 24, msgnum 2, severity ERROR(2) */
#define ACME$_PWDTOOSHORT     1572890   /* facility 24, msgnum 3, severity ERROR(2) */
#define ACME$_INVNEWPWD       1572898   /* facility 24, msgnum 4, severity ERROR(2) */

/* ================================================================
 * ACMESB — ACME status block
 * ================================================================ */

typedef struct _acmesb {
    unsigned short int acmesb$w_length;   /* Length of this block */
    unsigned short int acmesb$w_unused;   /* Reserved/alignment */
    unsigned int        acmesb$l_status;  /* Final ACME completion status */
} ACMESB;

#ifdef __cplusplus
}
#endif

#endif /* __ACMEDEF_H */
