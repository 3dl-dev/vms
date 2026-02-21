/*
 * SSDEF.H - VMS System Service Status Code Definitions
 *
 * OpenVMX compatibility layer - Defines the SS$_ condition values
 * returned by VMS system services.
 *
 * Status values are 32-bit longwords with the structure defined
 * in STSDEF.H:
 *   Bits  0-2:  Severity (0=warning, 1=success, 2=error, 3=info, 4=severe)
 *   Bits  3-15: Message number
 *   Bits 16-27: Facility number (0 = SYSTEM)
 *   Bit   28:   Customer bit
 *   Bits 29-31: Reserved
 *
 * System service status codes use facility number 0 (SYSTEM).
 * The actual numeric values below match the real OpenVMS definitions.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS System Messages and Recovery Procedures Reference
 */

#ifndef __SSDEF_H
#define __SSDEF_H

#include <stdint.h>
#include "stsdef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Success status codes (severity = 1, bit 0 set)
 * ================================================================ */

#define SS$_NORMAL          1       /* Normal successful completion */
#define SS$_WASCLR          1       /* Previous state was clear (alternate) */
#define SS$_WASSET          9       /* Previous state was set */
#define SS$_BUFFEROVF       4       /* Buffer overflow (warning, sev=0) */

/* ================================================================
 * Error/failure status codes
 *
 * Values match the real VMS system message file.  The severity
 * is encoded in bits 0-2 of each value.
 * ================================================================ */

#define SS$_ACCVIO          12      /* Access violation */
#define SS$_BADPARAM        20      /* Bad parameter value */
#define SS$_EXQUOTA         28      /* Exceeded quota */
#define SS$_NOPRIV          36      /* No privilege for attempted operation */
#define SS$_ABORT           44      /* Abort */

/* ================================================================
 * Warning status codes (severity = 0)
 * ================================================================ */

#define SS$_IVTIME          388     /* Invalid time */
#define SS$_DUPLNAM         434     /* Duplicate name */
#define SS$_NOLOGNAM        444     /* No logical name match */
#define SS$_NOTALLPRIV      532     /* Not all privileges available */
#define SS$_IVIDENT         548     /* Invalid identifier */
#define SS$_IVSECFLG        564     /* Invalid section flags */

/* ================================================================
 * Error status codes (severity = 2)
 * ================================================================ */

#define SS$_INSFMEM         292     /* Insufficient dynamic memory */
#define SS$_TIMEOUT         556     /* Device timeout */
#define SS$_ILLIOFUNC       580     /* Illegal I/O function */
#define SS$_NOMORENODE      588     /* No more cluster nodes (VMS: 0x24C) */
#define SS$_IVLOGNAM        596     /* Invalid logical name */
#define SS$_POWERFAIL       598     /* Power failure detected (VMS: 0x254; 596 taken by SS$_IVLOGNAM) */
#define SS$_RESULTOVF       1364    /* Result overflow */
#define SS$_CANCEL          2096    /* I/O operation canceled */
#define SS$_ENDOFFILE       2160    /* End of file */
#define SS$_NOSUCHDEV       2680    /* No such device */
#define SS$_DEVNOTMOUNT     2688    /* Device not mounted */
#define SS$_NOSUCHFILE      2696    /* No such file */

/* ================================================================
 * Additional commonly-used status codes
 * ================================================================ */

#define SS$_ITEMNOTFOUND    35820   /* Item not found */
#define SS$_BUGCHECK        676     /* Internal consistency failure */
#define SS$_FILALRACC       2736    /* File already accessed */
#define SS$_DEVOFFLINE      2692    /* Device offline */
#define SS$_DEVINACT        2704    /* Device inactive */
#define SS$_IVCHAN          602     /* Invalid channel */
#define SS$_IVDEVNAM        608     /* Invalid device name */
#define SS$_IVSSRQ          620     /* Invalid system service request */
#define SS$_SSFAIL          636     /* System service failure */
#define SS$_NOTRAN          2700    /* No translation for logical name */

/* ================================================================
 * Process-related status codes
 * ================================================================ */

#define SS$_NONEXPR         2540    /* Nonexistent process */
#define SS$_SUSPENDED       2584    /* Process suspended */
#define SS$_INCOMPAT        2632    /* Incompatible attributes */
#define SS$_NOSLOT          2732    /* No PCB slot available */

/* ================================================================
 * Condition handling status codes
 * ================================================================ */

#define SS$_RESIGNAL        2328    /* Resignal condition */
#define SS$_UNWIND          2204    /* Unwind in progress */
#define SS$_CONTINUE        2340    /* Continue execution */

/* ================================================================
 * Success/informational status codes
 * ================================================================ */

#define SS$_CREATED         836     /* Object created */
#define SS$_SUPERSEDE       844     /* Object superseded */
#define SS$_CONCEALED       852     /* Concealed device */
#define SS$_REMOTE          860     /* Remote node */
#define SS$_SYNCH           868     /* Synchronous completion */
#define SS$_OPINCOMPL       2552    /* Operation incomplete */

/* ================================================================
 * Lock manager status codes
 * ================================================================ */

#define SS$_NOTQUEUED       2588    /* Not queued */
#define SS$_DEADLOCK        708     /* Deadlock detected */
#define SS$_VALNOTVALID     712     /* Value block not valid */
#define SS$_PARNOTGRANT     716     /* Parent lock not granted */
#define SS$_CVTUNGRANT      2720    /* Convert ungrantable */

/* ================================================================
 * Quota and resource status codes
 * ================================================================ */

#define SS$_EXENQLM         2748    /* Exceeded enqueue limit */
#define SS$_EXASTLM         2756    /* Exceeded AST limit */
#define SS$_EXBYTLM         2764    /* Exceeded byte count limit */

/* ================================================================
 * Privilege and security status codes
 * ================================================================ */

#define SS$_NOCMKRNL        2212    /* No CMKRNL privilege */
#define SS$_NOCMEXEC        2216    /* No CMEXEC privilege */
#define SS$_NOSYSNAM        2220    /* No SYSNAM privilege */
#define SS$_NOGRACELOGIN    2224    /* No grace login */
#define SS$_INVLOGIN        2228    /* Invalid login */
#define SS$_NOSUCHID        2580    /* No such user identifier */

/* ================================================================
 * Timer and AST status codes
 * ================================================================ */

#define SS$_ASTFLT          2244    /* AST fault */
#define SS$_ILLEFC          2260    /* Illegal event flag cluster */
#define SS$_UNASEFC         2280    /* Unassociated event flag cluster */

/* ================================================================
 * I/O-related status codes
 * ================================================================ */

#define SS$_ENDOFTAPE       2164    /* End of tape */
#define SS$_DATACHECK       2168    /* Data check error */
#define SS$_PARITY          2172    /* Parity error */
#define SS$_NOREADER        2176    /* No reader on mailbox */
#define SS$_NOWRITER        2180    /* No writer on mailbox */
#define SS$_NOMSG           2184    /* No message */

/* ================================================================
 * CLI-related status codes
 * ================================================================ */

#define SS$_IVVERB          2284    /* Invalid verb */
#define SS$_IVQUAL          2288    /* Invalid qualifier */
#define SS$_IVKEYW          2292    /* Invalid keyword */

/* ================================================================
 * Miscellaneous status codes
 * ================================================================ */

#define SS$_UNSUPPORTED     2296    /* Unsupported operation */
#define SS$_ACCVIO_RO       2340    /* Read-only access violation */
#define SS$_PAGOWNVIO       2344    /* Page owner violation */
#define SS$_NOSOLICIT       4268    /* No solicitation */
#define SS$_FILNOTACC       2744    /* File not accessed */
#define SS$_IVMODE          2300    /* Invalid access mode */
#define SS$_CHANINTLK       2304    /* Channel interlock */
#define SS$_MSGNOTFND       2308    /* Message not found */

/* Additional status codes */
#define SS$_FILACCERR       2312    /* File access error */
#define SS$_DEVALLOC        2316    /* Device already allocated */
#define SS$_IVLOGTAB        2320    /* Invalid logical name table */
#define SS$_NOLOGTAB        2324    /* No such logical name table */

/* ================================================================
 * Status testing macros
 *
 * These are provided here for convenience since many VMS programs
 * include only SSDEF.H without STSDEF.H.
 * ================================================================ */

#ifndef $VMS_STATUS_SUCCESS
#define $VMS_STATUS_SUCCESS(code)   ((code) & 1)
#endif

#ifndef $VMS_STATUS_SEVERITY
#define $VMS_STATUS_SEVERITY(code)  ((code) & 7)
#endif

#ifndef $VMS_STATUS_FAC_NO
#define $VMS_STATUS_FAC_NO(code)    (((code) >> 16) & 0xFFF)
#endif

#ifndef $VMS_STATUS_CODE
#define $VMS_STATUS_CODE(code)      (((code) >> 3) & 0x1FFF)
#endif

#ifndef $VMS_STATUS_FAC_SP
#define $VMS_STATUS_FAC_SP(code)    (((code) >> 15) & 1)
#endif

#ifdef __cplusplus
}
#endif

#endif /* __SSDEF_H */
