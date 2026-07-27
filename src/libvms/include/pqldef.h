/*
 * PQLDEF.H - VMS Process Quota List (PQL$) Codes
 *
 * OpenVMX compatibility layer - Defines the PQL$_ quota-list tag
 * bytes used to build the "quota" argument to sys$creprc (an array of
 * {tag-byte, longword-value} pairs terminated by PQL$_LISTEND).
 *
 * PROVENANCE: the quota-list mechanism and the general set of quota
 * names are documented in the public OpenVMS System Services
 * Reference Manual ($CREPRC, "quota" argument). Exact numeric tag
 * values not confirmed against a fetchable public source this
 * session — sequential OVMX assignment, flagged in vms-531 findings.
 *
 * Reference: OpenVMS System Services Reference Manual ($CREPRC)
 */

#ifndef __PQLDEF_H
#define __PQLDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * PQL$_ — SYS$CREPRC quota-list tag bytes
 * ================================================================ */

#define PQL$_LISTEND       0   /* Terminates the quota list */
#define PQL$_ASTLM         1   /* AST queue limit */
#define PQL$_BIOLM         2   /* Buffered I/O limit */
#define PQL$_BYTLM         3   /* Buffered I/O byte count limit */
#define PQL$_CPULM         4   /* CPU time limit */
#define PQL$_DIOLM         5   /* Direct I/O limit */
#define PQL$_ENQLM         6   /* Lock enqueue limit */
#define PQL$_FILLM         7   /* Open file limit */
#define PQL$_JTQUOTA       8   /* Job table quota */
#define PQL$_PGFLQUOTA     9   /* Paging file quota */
#define PQL$_PRCLM        10   /* Subprocess creation limit */
#define PQL$_TQELM        11   /* Timer queue entry limit */
#define PQL$_WSDEFAULT    12   /* Default working set size */
#define PQL$_WSEXTENT     13   /* Working set extent */
#define PQL$_WSQUOTA      14   /* Working set quota */

#ifdef __cplusplus
}
#endif

#endif /* __PQLDEF_H */
