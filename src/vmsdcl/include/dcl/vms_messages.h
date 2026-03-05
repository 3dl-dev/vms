/*
 * vms_messages.h - VMS-Format Error and Status Messages
 *
 * Provides VMS-style %FACILITY-SEVERITY-IDENT message formatting,
 * a compiled-in message database, and errno-to-VMS translation.
 *
 * VMS severity codes:
 *   0 = Warning  (W)
 *   1 = Success  (S)
 *   2 = Error    (E)
 *   3 = Info     (I)
 *   4 = Fatal    (F)
 */

#ifndef DCL_VMS_MESSAGES_H
#define DCL_VMS_MESSAGES_H

#include <stdint.h>

/* ---- Severity codes ---------------------------------------------------- */

#define VMS_SEV_WARNING  0
#define VMS_SEV_SUCCESS  1
#define VMS_SEV_ERROR    2
#define VMS_SEV_INFO     3
#define VMS_SEV_FATAL    4

/* ---- Facility codes ---------------------------------------------------- */

#define VMS_FAC_DCL       1
#define VMS_FAC_COPY      2
#define VMS_FAC_DELETE     3
#define VMS_FAC_DIRECTORY  4
#define VMS_FAC_RMS        5
#define VMS_FAC_SYSTEM     6
#define VMS_FAC_SET        7
#define VMS_FAC_TYPE       8
#define VMS_FAC_SEARCH     9
#define VMS_FAC_CREATE    10
#define VMS_FAC_RENAME    11
#define VMS_FAC_OPEN      12
#define VMS_FAC_CLOSE     13
#define VMS_FAC_SHOW      14
#define VMS_FAC_PRINT     15
#define VMS_FAC_PURGE     16
#define VMS_FAC_SORT      17

/* ---- Message code construction ----------------------------------------- */

/*
 * Message code layout (32 bits):
 *   bits 31..16: facility
 *   bits 15..3:  message number
 *   bits 2..0:   severity
 */
#define VMS_MSG(fac, num, sev)  (((uint32_t)(fac) << 16) | ((uint32_t)(num) << 3) | (uint32_t)(sev))
#define VMS_MSG_FAC(code)       (((code) >> 16) & 0xFFFF)
#define VMS_MSG_NUM(code)       (((code) >> 3) & 0x1FFF)
#define VMS_MSG_SEV(code)       ((code) & 0x7)

/* ---- DCL facility messages --------------------------------------------- */

#define MSG_DCL_IVVERB     VMS_MSG(VMS_FAC_DCL, 1,  VMS_SEV_ERROR)
#define MSG_DCL_IVQUAL     VMS_MSG(VMS_FAC_DCL, 2,  VMS_SEV_ERROR)
#define MSG_DCL_NOCOMD     VMS_MSG(VMS_FAC_DCL, 3,  VMS_SEV_ERROR)
#define MSG_DCL_ABKEYW     VMS_MSG(VMS_FAC_DCL, 4,  VMS_SEV_ERROR)
#define MSG_DCL_MAXPARM    VMS_MSG(VMS_FAC_DCL, 5,  VMS_SEV_ERROR)
#define MSG_DCL_UNPRIC     VMS_MSG(VMS_FAC_DCL, 6,  VMS_SEV_ERROR)
#define MSG_DCL_UNDSYM     VMS_MSG(VMS_FAC_DCL, 7,  VMS_SEV_WARNING)
#define MSG_DCL_INVRANGE   VMS_MSG(VMS_FAC_DCL, 8,  VMS_SEV_ERROR)
#define MSG_DCL_NOKEYW     VMS_MSG(VMS_FAC_DCL, 9,  VMS_SEV_ERROR)
#define MSG_DCL_NODIR      VMS_MSG(VMS_FAC_DCL, 10, VMS_SEV_ERROR)
#define MSG_DCL_NOFILE     VMS_MSG(VMS_FAC_DCL, 11, VMS_SEV_ERROR)
#define MSG_DCL_NOLOG      VMS_MSG(VMS_FAC_DCL, 12, VMS_SEV_WARNING)
#define MSG_DCL_NOLCL      VMS_MSG(VMS_FAC_DCL, 13, VMS_SEV_WARNING)
#define MSG_DCL_BADPROT    VMS_MSG(VMS_FAC_DCL, 14, VMS_SEV_ERROR)
#define MSG_DCL_SYNTAX     VMS_MSG(VMS_FAC_DCL, 15, VMS_SEV_ERROR)
#define MSG_DCL_NESTLEV    VMS_MSG(VMS_FAC_DCL, 16, VMS_SEV_FATAL)
#define MSG_DCL_NOIFBLK    VMS_MSG(VMS_FAC_DCL, 17, VMS_SEV_ERROR)
#define MSG_DCL_NOLAB      VMS_MSG(VMS_FAC_DCL, 18, VMS_SEV_ERROR)
#define MSG_DCL_NOINTERACT VMS_MSG(VMS_FAC_DCL, 19, VMS_SEV_ERROR)
#define MSG_DCL_USGOTO     VMS_MSG(VMS_FAC_DCL, 20, VMS_SEV_ERROR)
#define MSG_DCL_NOGOSUB    VMS_MSG(VMS_FAC_DCL, 21, VMS_SEV_ERROR)
#define MSG_DCL_IVKEYW     VMS_MSG(VMS_FAC_DCL, 22, VMS_SEV_ERROR)
#define MSG_DCL_OPENIN     VMS_MSG(VMS_FAC_DCL, 23, VMS_SEV_ERROR)
#define MSG_DCL_CREATED    VMS_MSG(VMS_FAC_DCL, 24, VMS_SEV_WARNING)

/* ---- COPY facility messages -------------------------------------------- */

#define MSG_COPY_COPIED    VMS_MSG(VMS_FAC_COPY, 1, VMS_SEV_SUCCESS)
#define MSG_COPY_NEWFILES  VMS_MSG(VMS_FAC_COPY, 2, VMS_SEV_INFO)
#define MSG_COPY_OPENIN    VMS_MSG(VMS_FAC_COPY, 3, VMS_SEV_ERROR)
#define MSG_COPY_OPENOUT   VMS_MSG(VMS_FAC_COPY, 4, VMS_SEV_ERROR)
#define MSG_COPY_FNF       VMS_MSG(VMS_FAC_COPY, 5, VMS_SEV_ERROR)

/* ---- DELETE facility messages ------------------------------------------ */

#define MSG_DELETE_FILDEL     VMS_MSG(VMS_FAC_DELETE, 1, VMS_SEV_INFO)
#define MSG_DELETE_SEARCHFAIL VMS_MSG(VMS_FAC_DELETE, 2, VMS_SEV_ERROR)
#define MSG_DELETE_FILNOTDEL  VMS_MSG(VMS_FAC_DELETE, 3, VMS_SEV_ERROR)
#define MSG_DELETE_NOFILE     VMS_MSG(VMS_FAC_DELETE, 4, VMS_SEV_ERROR)

/* ---- DIRECTORY facility messages --------------------------------------- */

#define MSG_DIR_NOFILES    VMS_MSG(VMS_FAC_DIRECTORY, 1, VMS_SEV_WARNING)
#define MSG_DIR_TOTAL      VMS_MSG(VMS_FAC_DIRECTORY, 2, VMS_SEV_INFO)
#define MSG_DIR_GRAND      VMS_MSG(VMS_FAC_DIRECTORY, 3, VMS_SEV_INFO)
#define MSG_DIR_DATACHECK  VMS_MSG(VMS_FAC_DIRECTORY, 4, VMS_SEV_WARNING)

/* ---- RMS facility messages --------------------------------------------- */

#define MSG_RMS_FNF        VMS_MSG(VMS_FAC_RMS, 1,  VMS_SEV_ERROR)
#define MSG_RMS_DNF        VMS_MSG(VMS_FAC_RMS, 2,  VMS_SEV_ERROR)
#define MSG_RMS_PRV        VMS_MSG(VMS_FAC_RMS, 3,  VMS_SEV_ERROR)
#define MSG_RMS_FLK        VMS_MSG(VMS_FAC_RMS, 4,  VMS_SEV_ERROR)
#define MSG_RMS_RNF        VMS_MSG(VMS_FAC_RMS, 5,  VMS_SEV_ERROR)
#define MSG_RMS_EOF        VMS_MSG(VMS_FAC_RMS, 6,  VMS_SEV_WARNING)
#define MSG_RMS_FAC        VMS_MSG(VMS_FAC_RMS, 7,  VMS_SEV_ERROR)
#define MSG_RMS_SHR        VMS_MSG(VMS_FAC_RMS, 8,  VMS_SEV_ERROR)
#define MSG_RMS_CRE        VMS_MSG(VMS_FAC_RMS, 9,  VMS_SEV_ERROR)
#define MSG_RMS_WER        VMS_MSG(VMS_FAC_RMS, 10, VMS_SEV_ERROR)
#define MSG_RMS_RER        VMS_MSG(VMS_FAC_RMS, 11, VMS_SEV_ERROR)
#define MSG_RMS_DEL        VMS_MSG(VMS_FAC_RMS, 12, VMS_SEV_ERROR)
#define MSG_RMS_FEX        VMS_MSG(VMS_FAC_RMS, 13, VMS_SEV_ERROR)
#define MSG_RMS_ACC        VMS_MSG(VMS_FAC_RMS, 14, VMS_SEV_ERROR)
#define MSG_RMS_RAT        VMS_MSG(VMS_FAC_RMS, 15, VMS_SEV_ERROR)

/* ---- SYSTEM facility messages ------------------------------------------ */

#define MSG_SYSTEM_ACCVIO     VMS_MSG(VMS_FAC_SYSTEM, 1,  VMS_SEV_FATAL)
#define MSG_SYSTEM_NOPRIV     VMS_MSG(VMS_FAC_SYSTEM, 2,  VMS_SEV_ERROR)
#define MSG_SYSTEM_NOSUCHFILE VMS_MSG(VMS_FAC_SYSTEM, 3,  VMS_SEV_ERROR)
#define MSG_SYSTEM_IVLOGNAM   VMS_MSG(VMS_FAC_SYSTEM, 4,  VMS_SEV_ERROR)
#define MSG_SYSTEM_IVDEVNAM   VMS_MSG(VMS_FAC_SYSTEM, 5,  VMS_SEV_ERROR)
#define MSG_SYSTEM_BADPARAM   VMS_MSG(VMS_FAC_SYSTEM, 6,  VMS_SEV_ERROR)
#define MSG_SYSTEM_ABORT      VMS_MSG(VMS_FAC_SYSTEM, 7,  VMS_SEV_FATAL)
#define MSG_SYSTEM_IVTIME     VMS_MSG(VMS_FAC_SYSTEM, 8,  VMS_SEV_ERROR)
#define MSG_SYSTEM_DEVNOTMNT  VMS_MSG(VMS_FAC_SYSTEM, 9,  VMS_SEV_ERROR)
#define MSG_SYSTEM_FILACCERR  VMS_MSG(VMS_FAC_SYSTEM, 10, VMS_SEV_ERROR)

/* ---- SET facility messages --------------------------------------------- */

#define MSG_SET_NOPRIV     VMS_MSG(VMS_FAC_SET, 1,  VMS_SEV_ERROR)
#define MSG_SET_INVWIDTH   VMS_MSG(VMS_FAC_SET, 2,  VMS_SEV_ERROR)
#define MSG_SET_INVPAGE    VMS_MSG(VMS_FAC_SET, 3,  VMS_SEV_ERROR)
#define MSG_SET_INVPRI     VMS_MSG(VMS_FAC_SET, 4,  VMS_SEV_ERROR)
#define MSG_SET_NOFILES    VMS_MSG(VMS_FAC_SET, 5,  VMS_SEV_ERROR)
#define MSG_SET_NOSUCHFILE VMS_MSG(VMS_FAC_SET, 6,  VMS_SEV_ERROR)
#define MSG_SET_INVVLIM    VMS_MSG(VMS_FAC_SET, 7,  VMS_SEV_ERROR)
#define MSG_SET_IVTIME     VMS_MSG(VMS_FAC_SET, 8,  VMS_SEV_ERROR)
#define MSG_SET_PRV        VMS_MSG(VMS_FAC_SET, 9,  VMS_SEV_ERROR)
#define MSG_SET_IVUIC      VMS_MSG(VMS_FAC_SET, 10, VMS_SEV_ERROR)
#define MSG_SET_INVQUO     VMS_MSG(VMS_FAC_SET, 11, VMS_SEV_ERROR)

/* ---- TYPE facility messages -------------------------------------------- */

#define MSG_TYPE_OPENIN    VMS_MSG(VMS_FAC_TYPE, 1,  VMS_SEV_ERROR)
#define MSG_TYPE_FNF       VMS_MSG(VMS_FAC_TYPE, 2,  VMS_SEV_ERROR)

/* ---- SEARCH facility messages ------------------------------------------ */

#define MSG_SEARCH_NOMATCHES  VMS_MSG(VMS_FAC_SEARCH, 1, VMS_SEV_WARNING)
#define MSG_SEARCH_OPENIN     VMS_MSG(VMS_FAC_SEARCH, 2, VMS_SEV_ERROR)
#define MSG_SEARCH_FNF        VMS_MSG(VMS_FAC_SEARCH, 3, VMS_SEV_ERROR)

/* ---- CREATE facility messages ------------------------------------------ */

#define MSG_CREATE_EXISTS  VMS_MSG(VMS_FAC_CREATE, 1, VMS_SEV_INFO)
#define MSG_CREATE_CRE     VMS_MSG(VMS_FAC_CREATE, 2, VMS_SEV_ERROR)

/* ---- RENAME facility messages ------------------------------------------ */

#define MSG_RENAME_RNF     VMS_MSG(VMS_FAC_RENAME, 1, VMS_SEV_ERROR)

/* ---- OPEN/CLOSE facility messages -------------------------------------- */

#define MSG_OPEN_FNF       VMS_MSG(VMS_FAC_OPEN, 1, VMS_SEV_ERROR)
#define MSG_OPEN_PRV       VMS_MSG(VMS_FAC_OPEN, 2, VMS_SEV_ERROR)

/* ---- PRINT facility messages ------------------------------------------- */

#define MSG_PRINT_OPENIN   VMS_MSG(VMS_FAC_PRINT, 1, VMS_SEV_ERROR)

/* ---- PURGE facility messages ------------------------------------------- */

#define MSG_PURGE_FNF      VMS_MSG(VMS_FAC_PURGE, 1, VMS_SEV_ERROR)

/* ---- SORT facility messages -------------------------------------------- */

#define MSG_SORT_OPENIN    VMS_MSG(VMS_FAC_SORT, 1, VMS_SEV_ERROR)
#define MSG_SORT_OPENOUT   VMS_MSG(VMS_FAC_SORT, 2, VMS_SEV_ERROR)

/* ---- Message database entry -------------------------------------------- */

struct vms_msg_entry {
    uint32_t    code;       /* Message code (facility|number|severity) */
    const char *facility;   /* Facility name string */
    const char *ident;      /* Identifier string */
    const char *text;       /* Default message text */
};

/* ---- Public API -------------------------------------------------------- */

/*
 * Look up a message by code and return its formatted text.
 * Returns pointer to static buffer: "%DCL-E-IVVERB, unrecognized command verb"
 * If extra_text is non-NULL, it's appended: "%DCL-E-IVVERB, unrecognized command verb - XYZZY"
 */
const char *vms_message(uint32_t msgcode, const char *extra_text);

/*
 * Look up a message by code only (no extra text).
 * Returns pointer to static buffer.
 */
const char *vms_msg_lookup(uint32_t msgcode);

/*
 * Map a Unix errno to a VMS-format error string.
 * Returns formatted message like "%RMS-E-FNF, file not found"
 */
const char *vms_strerror(int unix_errno);

/*
 * Translate a Linux path to VMS-format for error messages.
 * E.g., "/vms/home/system/test.txt" → "SYS$SYSDEVICE:[HOME.SYSTEM]TEST.TXT"
 * Falls back to uppercase if translation fails.
 * Returns pointer to static buffer.
 */
const char *vms_error_path(const char *linux_path);

/*
 * Get facility name for a message code.
 */
const char *vms_msg_facility(uint32_t msgcode);

/*
 * Get ident string for a message code.
 */
const char *vms_msg_ident(uint32_t msgcode);

/*
 * Get severity character for a severity code.
 */
char vms_severity_char(int severity);

#endif /* DCL_VMS_MESSAGES_H */
