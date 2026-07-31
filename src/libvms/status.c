/*
 * status.c - VMS Condition Value / Status Code Handling
 *
 * VMS system services return 32-bit condition values. This module
 * provides routines to decode, format, and compare these values.
 *
 * Condition value layout (32 bits):
 *   Bits 31-28: Control
 *   Bits 27-16: Facility number
 *   Bit     15: Customer facility flag
 *   Bits 14-3:  Message number
 *   Bits  2-0:  Severity (0=W, 1=S, 2=E, 3=I, 4=F)
 *
 * Bit 0 is the success indicator: odd = success, even = failure.
 */

#include <stdio.h>
#include <string.h>
#include "ssdef.h"
#include "stsdef.h"
#include "rmsdef.h"
#include "descrip.h"
#include "ovmx_status.h"

/* Short severity letter for message formatting */
static const char *severity_letter[] = {
    "W", "S", "E", "I", "F", "?", "?", "?"
};

/*
 * Known status code table - maps numeric codes to ident strings and text.
 */
struct status_entry {
    uint32_t    code;
    const char *facility;
    const char *ident;
    const char *text;
};

static const struct status_entry known_codes[] = {
    /* SYSTEM facility - success / informational */
    { SS$_NORMAL,       "SYSTEM", "NORMAL",       "normal successful completion" },
    { SS$_WASCLR,       "SYSTEM", "WASCLR",       "flag was previously clear" },
    { SS$_WASSET,       "SYSTEM", "WASSET",       "flag was previously set" },
    { SS$_SUPERSEDE,    "SYSTEM", "SUPERSEDE",    "logical name superseded" },
    { SS$_CREATED,      "SYSTEM", "CREATED",      "object created" },
    { SS$_BUFFEROVF,    "SYSTEM", "BUFFEROVF",    "buffer overflow" },

    /* SYSTEM facility - errors and warnings */
    { SS$_ACCVIO,       "SYSTEM", "ACCVIO",       "access violation" },
    { SS$_BADPARAM,     "SYSTEM", "BADPARAM",     "bad parameter value" },
    { SS$_EXQUOTA,      "SYSTEM", "EXQUOTA",      "exceeded quota" },
    { SS$_INSFMEM,      "SYSTEM", "INSFMEM",      "insufficient dynamic memory" },
    { SS$_NOPRIV,       "SYSTEM", "NOPRIV",       "no privilege for attempted operation" },
    { SS$_NOSUCHDEV,    "SYSTEM", "NOSUCHDEV",    "no such device" },
    { SS$_NOSUCHFILE,   "SYSTEM", "NOSUCHFILE",   "no such file" },
    { SS$_BUGCHECK,     "SYSTEM", "BUGCHECK",     "internal consistency failure" },
    { SS$_FILACCERR,    "SYSTEM", "FILACCERR",    "file access error" },
    { SS$_DEVMOUNT,     "SYSTEM", "DEVMOUNT",     "device already mounted" },
    { SS$_DEVNOTMOUNT,  "SYSTEM", "DEVNOTMOUNT",  "device not mounted" },
    /* Message text measured on the ~/vax OpenVMS VAX V7.3 lab via
     * F$MESSAGE -- see the provenance note in ssdef.h. */
    { SS$_DEVALLOC,     "SYSTEM", "DEVALLOC",     "device already allocated to another user" },
    { SS$_DEVNOTALLOC,  "SYSTEM", "DEVNOTALLOC",  "device not allocated" },
    { SS$_IVDEVNAM,     "SYSTEM", "IVDEVNAM",     "invalid device name" },
    { SS$_IVLOGNAM,     "SYSTEM", "IVLOGNAM",     "invalid logical name" },
    /* ORACLE-PINNED: F$MESSAGE(148) on the reference lab VAX V7.3
     * renders "%SYSTEM-F-DUPLNAM, duplicate name" -- the same text the
     * lab's RUN/DETACHED transcript chains under %RUN-F-CREPRC when a
     * process name is already held in the caller's UIC group (the
     * transcript is quoted in tests/qemu/test_kmod_procnam.c). Until
     * this entry existed, the condition the executive returns for a
     * duplicate process name had no text at all. */
    { SS$_DUPLNAM,      "SYSTEM", "DUPLNAM",      "duplicate name" },
    { SS$_IVLOGTAB,     "SYSTEM", "IVLOGTAB",     "invalid logical name table" },
    { SS$_NOLOGNAM,     "SYSTEM", "NOLOGNAM",     "no such logical name" },
    { SS$_NOLOGTAB,     "SYSTEM", "NOLOGTAB",     "no such logical name table" },
    { SS$_RESULTOVF,    "SYSTEM", "RESULTOVF",    "result string overflow" },
    { SS$_CANCEL,       "SYSTEM", "CANCEL",       "I/O operation cancelled" },
    { SS$_IVCHAN,       "SYSTEM", "IVCHAN",       "invalid I/O channel" },
    { SS$_IVMODE,       "SYSTEM", "IVMODE",       "invalid access mode" },
    /* ORACLE-PINNED (vms-9fc): F$MESSAGE(244) on the reference lab VAX
     * V7.3 renders "%SYSTEM-F-ILLIOFUNC, illegal I/O function code" --
     * "code" included. */
    { SS$_ILLIOFUNC,    "SYSTEM", "ILLIOFUNC",    "illegal I/O function code" },
    { SS$_TIMEOUT,      "SYSTEM", "TIMEOUT",      "device timeout" },
    { SS$_ILLEFC,       "SYSTEM", "ILLEFC",       "illegal event flag cluster" },
    { SS$_NONEXPR,      "SYSTEM", "NONEXPR",      "nonexistent process" },
    { SS$_SUSPENDED,    "SYSTEM", "SUSPENDED",    "process suspended" },
    { SS$_NOTQUEUED,    "SYSTEM", "NOTQUEUED",    "timer request not queued" },
    { SS$_DEADLOCK,     "SYSTEM", "DEADLOCK",     "deadlock detected" },
    { SS$_EXENQLM,      "SYSTEM", "EXENQLM",     "exceeded enqueue limit" },
    { SS$_EXASTLM,      "SYSTEM", "EXASTLM",     "exceeded AST limit" },
    { SS$_UNSUPPORTED,  "SYSTEM", "UNSUPPORTED",  "unsupported operation" },
    { SS$_ENDOFFILE,    "SYSTEM", "ENDOFFILE",    "end of file" },
    { SS$_IVVERB,       "SYSTEM", "IVVERB",       "invalid verb" },
    { SS$_IVQUAL,       "SYSTEM", "IVQUAL",       "invalid qualifier" },
    { SS$_IVKEYW,       "SYSTEM", "IVKEYW",       "invalid keyword" },

    /* RMS facility */
    { RMS$_NORMAL,      "RMS",    "NORMAL",       "normal successful completion" },
    { RMS$_FNF,         "RMS",    "FNF",          "file not found" },
    { RMS$_DNF,         "RMS",    "DNF",          "directory not found" },
    { RMS$_PRV,         "RMS",    "PRV",          "insufficient privilege" },
    { RMS$_FEX,         "RMS",    "FEX",          "file already exists" },
    { RMS$_EOF,         "RMS",    "EOF",          "end of file" },
    { RMS$_RNF,         "RMS",    "RNF",          "record not found" },
    { RMS$_NMF,         "RMS",    "NMF",          "no more files" },

    /*
     * OVMX facility -- NOT VMS. These carry the customer-defined bit
     * (see src/libvms/include/ovmx_status.h) and are rendered under the
     * facility name OVMX so no reader can mistake one for a SYSTEM
     * condition value. The text is OVMX's to define precisely because
     * OpenVMS has no equivalent condition.
     *
     * Present because RUN/DETACHED chains whatever $CREPRC returns
     * (src/vmsdcl/dcl_cmd_process.c): without an entry, the one OVMX
     * condition $CREPRC can produce printed as "-NONAME-F-UNKNOWN",
     * which reads like a VMS message and is not one.
     */
    { OVMX$_PRCLOST,    "OVMX",   "PRCLOST",
      "process lost before it entered the executive's process table" },
    { OVMX$_NOSUBPRC,   "OVMX",   "NOSUBPRC",
      "subprocess creation is not implemented" },
    { OVMX$_NOPRCUIC,   "OVMX",   "NOPRCUIC",
      "a created process cannot be given a UIC of the caller's choosing" },
    { OVMX$_NODEBUGGER, "OVMX",   "NODEBUGGER",
      "no debugger is present to run the image under" },

    /* Sentinel */
    { 0, NULL, NULL, NULL }
};

/*
 * vms_status_string - Convert a VMS status code to a human-readable string.
 *
 * Produces output in the standard VMS format:
 *   %FACILITY-S-IDENT, message text
 *
 * Returns the number of characters written (excluding null terminator).
 */
int vms_status_string(uint32_t status, char *buf, size_t bufsize) {
    if (!buf || bufsize == 0) return 0;

    uint32_t sev = status & STS$M_SEVERITY;
    const char *sev_str = severity_letter[sev & 7];

    /* Search known codes */
    for (int i = 0; known_codes[i].facility != NULL; i++) {
        if (known_codes[i].code == status) {
            return snprintf(buf, bufsize, "%%%s-%s-%s, %s",
                            known_codes[i].facility, sev_str,
                            known_codes[i].ident, known_codes[i].text);
        }
    }

    /* Unknown code - format generically */
    const char *fac_name;
    uint32_t fac = $VMS_STATUS_FAC_NO(status);
    switch (fac) {
        case FACILITY_SYSTEM: fac_name = "SYSTEM"; break;
        case FACILITY_RMS:    fac_name = "RMS";    break;
        case FACILITY_CLI:    fac_name = "CLI";    break;
        case FACILITY_LIB:    fac_name = "LIB";    break;
        case FACILITY_STR:    fac_name = "STR";    break;
        default:              fac_name = "NONAME"; break;
    }

    return snprintf(buf, bufsize, "%%%s-%s-%04X, message number %u",
                    fac_name, sev_str, $VMS_STATUS_MSG_NO(status), status);
}

/*
 * vms_status_severity - Return the severity level (0-4) of a status code.
 */
uint32_t vms_status_severity(uint32_t status) {
    return status & STS$M_SEVERITY;
}

/*
 * vms_status_match - Match two condition values ignoring severity.
 *
 * Returns 1 if the facility and message number match, 0 otherwise.
 * This is the equivalent of the $MATCH_COND macro on VMS.
 */
int vms_status_match(uint32_t sts1, uint32_t sts2) {
    return (sts1 & STS$M_COND_ID) == (sts2 & STS$M_COND_ID);
}

/* --- Legacy API aliases used internally by other modules --- */

uint32_t vms$format_status(uint32_t status, char *buf, size_t buflen) {
    if (!buf || buflen == 0) return SS$_BADPARAM;
    vms_status_string(status, buf, buflen);
    return SS$_NORMAL;
}

const char *vms$severity_string(uint32_t status) {
    static const char *severity_full[] = {
        "WARNING", "SUCCESS", "ERROR", "INFO", "FATAL", "?", "?", "?"
    };
    return severity_full[vms_status_severity(status) & 7];
}

int vms$is_success(uint32_t status) {
    return (status & STS$M_SUCCESS) != 0;
}

const char *vms$status_message(uint32_t code) {
    for (int i = 0; known_codes[i].facility != NULL; i++) {
        if (known_codes[i].code == code) {
            return known_codes[i].text;
        }
    }
    return "unknown status code";
}

const char *vms$status_ident(uint32_t code) {
    for (int i = 0; known_codes[i].facility != NULL; i++) {
        if (known_codes[i].code == code) {
            return known_codes[i].ident;
        }
    }
    return "UNKNOWN";
}
