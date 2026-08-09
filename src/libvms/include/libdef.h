/*
 * LIBDEF.H - VMS Library (LIB$) Facility Status Codes and Constants
 *
 * OpenVMX compatibility layer - Defines the LIB$_ condition values,
 * LIB$M_ bit-mask constants, and LIB$K_ enumeration constants for
 * the LIB$ run-time library facility.
 *
 * Status values follow the VMS condition value layout defined in
 * STSDEF.H.  LIB$ uses facility number 21 (0x15).
 *
 * The condition value encoding for LIB$ codes:
 *   Bits 16-27: facility = 21 (0x00150000 base)
 *   Bit  15:    facility-specific flag
 *   Bits  3-14: message number
 *   Bits  0-2:  severity
 *
 * Programs that include LIBDEF.H should also include SSDEF.H and
 * STSDEF.H for status testing macros.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual
 *            OpenVMS System Messages and Recovery Procedures Reference
 */

#ifndef __LIBDEF_H
#define __LIBDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * LIB$ facility condition values
 *
 * Facility number = 21 (decimal), 0x15 (hex).
 * Base = facility << 16 = 0x00150000.
 *
 * Severity encoding (bits 0-2):
 *   1 = success  (odd)
 *   0 = warning
 *   2 = error
 *   4 = severe/fatal
 *
 * The message numbers below match the real OpenVMS values.
 * ================================================================ */

#define LIB$_FACILITY       21          /* LIB$ facility number */

/* TRUE / FALSE — many ported VMS C sources (e.g. the eight-cubed LIB$
 * examples) use these bare boolean constants and expect them from a
 * commonly-included VMS header.  DECC makes them available through its
 * predefines; provide them here (guarded) as an OVMX source-compat
 * convenience so those sources build.  Not a VMS-authentic libdef symbol. */
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

/* Success (severity = 1) */
#define LIB$_NORMAL         0x00158001  /* Normal successful completion */
#define LIB$_STRTRU         0x00158004  /* String truncated (warning, sev=0) */
#define LIB$_KEYALRINS      0x00158009  /* Key already inserted in tree */
#define LIB$_ONEENTQUE      0x00158011  /* One entry on queue */

/* Warning (severity = 0) */
#define LIB$_QUEWASEMP      0x00158000  /* Queue was empty */
#define LIB$_DEFFORUSE      0x00158020  /* Default format used */
#define LIB$_ENGLUSED       0x00158028  /* English language used */
#define LIB$_NOTFOU         0x00158030  /* Not found */
#define LIB$_NEGTIM         0x00158038  /* Negative time result */

/* Error (severity = 2) */
#define LIB$_INVARG         0x00158012  /* Invalid argument */
#define LIB$_INVSTRDES      0x0015801A  /* Invalid string descriptor */
#define LIB$_INSVIRMEM      0x00158022  /* Insufficient virtual memory */
#define LIB$_FATERRLIB      0x0015802A  /* Fatal error in library */
#define LIB$_INTLOGERR      0x00158032  /* Internal logic error */
#define LIB$_NOCLI          0x0015803A  /* No CLI present */
#define LIB$_UNECLIERR      0x00158042  /* Unexpected CLI error */
#define LIB$_AMBKEY         0x0015804A  /* Ambiguous keyword */
#define LIB$_AMBVAL         0x00158052  /* Ambiguous value */
#define LIB$_NOSUCHSYM      0x0015805A  /* No such symbol */
#define LIB$_INSCLIMEM      0x00158062  /* Insufficient CLI memory */
#define LIB$_BADZONE        0x0015806A  /* Bad zone identifier */
#define LIB$_KEYNOTFOU      0x00158072  /* Key not found in tree */
#define LIB$_WRONUMARG      0x0015807A  /* Wrong number of arguments */
#define LIB$_BADSUBSCR      0x00158082  /* Bad subscript */
#define LIB$_SYNTAXERR      0x0015808A  /* Syntax error */
#define LIB$_UNRKEY         0x00158092  /* Unrecognized keyword */
#define LIB$_SECINTFAI      0x0015809A  /* Secondary interlock failure */

/* Severe/fatal (severity = 4) */
#define LIB$_BUGCHECK       0x00158004  /* Internal consistency failure */

/* ================================================================
 * LIB$M_ bit-mask constants
 *
 * These are bit masks used as flag arguments to various LIB$ routines.
 * ================================================================ */

/* Virtual memory zone flags (lib$create_vm_zone) */
#define LIB$M_VM_BOUNDARY_TAGS  0x00000001  /* Add boundary tags to blocks */
#define LIB$M_VM_GET_FILL0      0x00000002  /* Zero-fill blocks on get */
#define LIB$M_VM_FREE_FILL0     0x00000004  /* Zero-fill blocks on free */
#define LIB$M_VM_FREE_FILLF     0x00000008  /* Fill blocks with 0xFF on free */
#define LIB$M_VM_NO_CACHE       0x00000010  /* Disable lookaside cache */
#define LIB$M_VM_EXTEND_AREA    0x00000020  /* Extend area on demand */
#define LIB$M_VM_TAIL_LARGE     0x00000040  /* Keep large blocks at tail */

/* lib$format_date_time / lib$get_maximum_date_length flags */
#define LIB$M_DATE_FIELDS       0x00000001  /* Include date fields */
#define LIB$M_TIME_FIELDS       0x00000002  /* Include time fields */
#define LIB$M_WEEKDAY           0x00000004  /* Include day of week */
#define LIB$M_RELATIVE          0x00000008  /* Use relative (delta) format */

/* ================================================================
 * LIB$K_ enumeration constants
 *
 * Named constants for algorithm selectors, table types, and other
 * enumerated values used in LIB$ routine arguments.
 * ================================================================ */

/* Virtual memory zone allocation algorithms (lib$create_vm_zone) */
#define LIB$K_VM_FIRST_FIT      1   /* First-fit allocation algorithm */
#define LIB$K_VM_BEST_FIT       2   /* Best-fit allocation algorithm */
#define LIB$K_VM_QUICK_FIT      3   /* Quick-fit with lookaside lists */
#define LIB$K_VM_FREQ_SIZES     4   /* Frequent sizes allocation */
#define LIB$K_VM_FIXED          5   /* Fixed-size block allocator */

/* Symbol table type codes (lib$get_symbol / lib$set_symbol) */
#define LIB$K_CLI_LOCAL_SYM     1   /* Local symbol table */
#define LIB$K_CLI_GLOBAL_SYM    2   /* Global symbol table */

/* lib$stat_vm statistics codes */
#define LIB$K_VM_STAT_GET       1   /* Count of lib$get_vm calls */
#define LIB$K_VM_STAT_FREE      2   /* Count of lib$free_vm calls */
#define LIB$K_VM_STAT_SIZE      3   /* Total bytes currently allocated */

/* lib$cvtf_from_internal_time / lib$cvtf_to_internal_time operation codes */
/* (These are defined in libdtdef.h — included here for completeness) */

/* ================================================================
 * LIB$ data tables (external symbols)
 *
 * These are character translation tables provided by the LIB$ library.
 * Programs reference them by address using a descriptor.
 * ================================================================ */

/*
 * LIB$AB_UPCASE - 256-byte uppercase translation table
 *
 * Each byte i contains the uppercase equivalent of character i.
 * Used with lib$movtc to convert strings to uppercase.
 */
extern char LIB$AB_UPCASE;

/*
 * LIB$AB_LOWERCASE - 256-byte lowercase translation table
 */
extern char LIB$AB_LOWERCASE;

/*
 * LIB$AB_ASC_EBC - 256-byte ASCII to EBCDIC translation table
 */
extern char LIB$AB_ASC_EBC;

/*
 * LIB$AB_EBC_ASC - 256-byte EBCDIC to ASCII translation table
 */
extern char LIB$AB_EBC_ASC;

#ifdef __cplusplus
}
#endif

#endif /* __LIBDEF_H */
