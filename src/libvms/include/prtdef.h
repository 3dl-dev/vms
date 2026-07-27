/*
 * PRTDEF.H - VMS Virtual Memory Protection (PRT$) Codes
 *
 * OpenVMX compatibility layer - Defines the PRT$C_ page-protection
 * codes used with sys$setprt_64 and related virtual-memory services
 * to specify per-page access protection.
 *
 * PROVENANCE: the existence of an ordered "no access" -> "all access"
 * PRT$C_ protection-code scale is documented in the public OpenVMS
 * Programming Concepts Manual (virtual memory protection). The exact
 * assignment of each named code to its ordinal position could NOT be
 * confirmed against a fetchable public source this session —
 * sequential OVMX assignment, flagged in vms-531 findings for
 * operator sign-off. Only the code exercised by the current corpus
 * (PRT$C_EW) plus its two immediate neighbors are defined; not
 * gold-plated with the full table.
 *
 * Reference: OpenVMS Programming Concepts Manual, Virtual Memory
 *            Management, Table of Protection Codes
 */

#ifndef __PRTDEF_H
#define __PRTDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * PRT$C_ — page protection codes
 * ================================================================ */

#define PRT$C_NA    0   /* No access */
#define PRT$C_ER    1   /* Executive-and-more read access */
#define PRT$C_EW    2   /* Executive-and-more read/write access */

#ifdef __cplusplus
}
#endif

#endif /* __PRTDEF_H */
