/*
 * dcl_messages.c - VMS-Format Message Database
 *
 * Compiled-in message database with 50+ VMS-style message codes.
 * Provides errno-to-VMS mapping and Linux-to-VMS path translation
 * for error messages.
 *
 * VMS message format: %FACILITY-SEVERITY-IDENT, message text
 * Severity: W=warning(0), S=success(1), E=error(2), I=info(3), F=fatal(4)
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "dcl/vms_messages.h"

/* External: VMS filespec formatting */
extern int dcl_format_filespec(const char *linux_path, char *vms_spec,
                               size_t spec_size);

/* ---- Severity character table ------------------------------------------ */

static const char sev_chars[] = "WSEIF";  /* 0=W, 1=S, 2=E, 3=I, 4=F */

char vms_severity_char(int severity)
{
    if (severity >= 0 && severity <= 4)
        return sev_chars[severity];
    return 'E';
}

/* ---- Message database -------------------------------------------------- */

static const struct vms_msg_entry msg_db[] = {
    /* DCL facility (1) */
    { MSG_DCL_IVVERB,     "DCL",    "IVVERB",     "unrecognized command verb" },
    { MSG_DCL_IVQUAL,     "DCL",    "IVQUAL",     "unrecognized qualifier" },
    { MSG_DCL_NOCOMD,     "DCL",    "NOCOMD",     "no command on line" },
    { MSG_DCL_ABKEYW,     "DCL",    "ABKEYW",     "ambiguous keyword" },
    { MSG_DCL_MAXPARM,    "DCL",    "MAXPARM",    "too many parameters" },
    { MSG_DCL_UNPRIC,     "DCL",    "UNPRIC",     "insufficient privilege for attempted operation" },
    { MSG_DCL_UNDSYM,     "DCL",    "UNDSYM",     "undefined symbol" },
    { MSG_DCL_INVRANGE,   "DCL",    "INVRANGE",   "invalid range specified" },
    { MSG_DCL_NOKEYW,     "DCL",    "NOKEYW",     "missing keyword" },
    { MSG_DCL_NODIR,      "DCL",    "NODIR",      "missing directory specification" },
    { MSG_DCL_NOFILE,     "DCL",    "NOFILE",     "missing file specification" },
    { MSG_DCL_NOLOG,      "DCL",    "NOLOG",      "no logical name match" },
    { MSG_DCL_NOLCL,      "DCL",    "NOLCL",      "no local symbols match" },
    { MSG_DCL_BADPROT,    "DCL",    "BADPROT",    "invalid protection string" },
    { MSG_DCL_SYNTAX,     "DCL",    "SYNTAX",     "syntax error in command line" },
    { MSG_DCL_NESTLEV,    "DCL",    "NESTLEV",    "maximum nesting level exceeded" },
    { MSG_DCL_NOIFBLK,    "DCL",    "NOIFBLK",    "no matching IF for ELSE/ENDIF" },
    { MSG_DCL_NOLAB,      "DCL",    "NOLAB",      "no label specified" },
    { MSG_DCL_NOINTERACT, "DCL",    "NOINTERACT", "command requires interactive terminal" },
    { MSG_DCL_USGOTO,     "DCL",    "USGOTO",     "target label not found" },
    { MSG_DCL_NOGOSUB,    "DCL",    "NOGOSUB",    "RETURN without GOSUB" },
    { MSG_DCL_IVKEYW,     "DCL",    "IVKEYW",     "invalid keyword" },
    { MSG_DCL_OPENIN,     "DCL",    "OPENIN",     "error opening file as input" },
    { MSG_DCL_CREATED,    "DCL",    "CREATED",    "directory already exists" },

    /* COPY facility (2) */
    { MSG_COPY_COPIED,    "COPY",   "COPIED",     "file(s) copied" },
    { MSG_COPY_NEWFILES,  "COPY",   "NEWFILES",   "new file(s) created" },
    { MSG_COPY_OPENIN,    "COPY",   "OPENIN",     "error opening input file" },
    { MSG_COPY_OPENOUT,   "COPY",   "OPENOUT",    "error opening output file" },
    { MSG_COPY_FNF,       "COPY",   "FNF",        "file not found" },

    /* DELETE facility (3) */
    { MSG_DELETE_FILDEL,     "DELETE", "FILDEL",     "file deleted" },
    { MSG_DELETE_SEARCHFAIL, "DELETE", "SEARCHFAIL", "search failed to find file" },
    { MSG_DELETE_FILNOTDEL,  "DELETE", "FILNOTDEL",  "error deleting file" },
    { MSG_DELETE_NOFILE,     "DELETE", "NOFILE",     "no file specification given" },

    /* DIRECTORY facility (4) */
    { MSG_DIR_NOFILES,    "DIRECT", "NOFILES",    "no files found" },
    { MSG_DIR_TOTAL,      "DIRECT", "TOTAL",      "total files" },
    { MSG_DIR_GRAND,      "DIRECT", "GRAND",      "grand total" },
    { MSG_DIR_DATACHECK,  "DIRECT", "DATACHECK",  "data check error" },

    /* RMS facility (5) */
    { MSG_RMS_FNF,        "RMS",    "FNF",        "file not found" },
    { MSG_RMS_DNF,        "RMS",    "DNF",        "directory not found" },
    { MSG_RMS_PRV,        "RMS",    "PRV",        "insufficient privilege or file protection violation" },
    { MSG_RMS_FLK,        "RMS",    "FLK",        "file locked by another user" },
    { MSG_RMS_RNF,        "RMS",    "RNF",        "record not found" },
    { MSG_RMS_EOF,        "RMS",    "EOF",        "end of file detected" },
    { MSG_RMS_FAC,        "RMS",    "FAC",        "record operation not permitted by file access" },
    { MSG_RMS_SHR,        "RMS",    "SHR",        "record locked; file sharing conflict" },
    { MSG_RMS_CRE,        "RMS",    "CRE",        "error creating file" },
    { MSG_RMS_WER,        "RMS",    "WER",        "error writing record" },
    { MSG_RMS_RER,        "RMS",    "RER",        "error reading record" },
    { MSG_RMS_DEL,        "RMS",    "DEL",        "error deleting record" },
    { MSG_RMS_FEX,        "RMS",    "FEX",        "file already exists" },
    { MSG_RMS_ACC,        "RMS",    "ACC",        "file access error" },
    { MSG_RMS_RAT,        "RMS",    "RAT",        "invalid record attributes" },

    /* SYSTEM facility (6) */
    { MSG_SYSTEM_ACCVIO,     "SYSTEM", "ACCVIO",     "access violation" },
    { MSG_SYSTEM_NOPRIV,     "SYSTEM", "NOPRIV",     "no privilege for attempted operation" },
    { MSG_SYSTEM_NOSUCHFILE, "SYSTEM", "NOSUCHFILE", "no such file" },
    { MSG_SYSTEM_IVLOGNAM,   "SYSTEM", "IVLOGNAM",   "invalid logical name" },
    { MSG_SYSTEM_IVDEVNAM,   "SYSTEM", "IVDEVNAM",   "invalid device name" },
    { MSG_SYSTEM_BADPARAM,   "SYSTEM", "BADPARAM",   "bad parameter value" },
    { MSG_SYSTEM_ABORT,      "SYSTEM", "ABORT",      "abort" },
    { MSG_SYSTEM_IVTIME,     "SYSTEM", "IVTIME",     "invalid time" },
    { MSG_SYSTEM_DEVNOTMNT,  "SYSTEM", "DEVNOTMNT",  "device not mounted" },
    { MSG_SYSTEM_FILACCERR,  "SYSTEM", "FILACCERR",  "file access error" },

    /* SET facility (7) */
    { MSG_SET_NOPRIV,     "SET",    "NOPRIV",     "insufficient privilege for attempted operation" },
    { MSG_SET_INVWIDTH,   "SET",    "INVWIDTH",   "invalid terminal width" },
    { MSG_SET_INVPAGE,    "SET",    "INVPAGE",    "invalid page size" },
    { MSG_SET_INVPRI,     "SET",    "INVPRI",     "invalid priority value" },
    { MSG_SET_NOFILES,    "SET",    "NOFILES",    "no file specification" },
    { MSG_SET_NOSUCHFILE, "SET",    "NOSUCHFILE", "file does not exist" },
    { MSG_SET_INVVLIM,    "SET",    "INVVLIM",    "invalid version limit" },
    { MSG_SET_IVTIME,     "SET",    "IVTIME",     "invalid time specification" },
    { MSG_SET_PRV,        "SET",    "PRV",        "file protection violation" },
    { MSG_SET_IVUIC,      "SET",    "IVUIC",      "invalid UIC" },
    { MSG_SET_INVQUO,     "SET",    "INVQUO",     "invalid quota value" },

    /* TYPE facility (8) */
    { MSG_TYPE_OPENIN,    "TYPE",   "OPENIN",     "error opening file as input" },
    { MSG_TYPE_FNF,       "TYPE",   "FNF",        "file not found" },

    /* SEARCH facility (9) */
    { MSG_SEARCH_NOMATCHES, "SEARCH", "NOMATCHES", "no strings matched" },
    { MSG_SEARCH_OPENIN,    "SEARCH", "OPENIN",    "error opening file as input" },
    { MSG_SEARCH_FNF,       "SEARCH", "FNF",       "file not found" },

    /* CREATE facility (10) */
    { MSG_CREATE_EXISTS,  "CREATE", "EXISTS",     "file already exists" },
    { MSG_CREATE_CRE,     "CREATE", "CRE",       "error creating file" },

    /* RENAME facility (11) */
    { MSG_RENAME_RNF,     "RENAME", "RNF",        "error renaming file" },

    /* OPEN facility (12) */
    { MSG_OPEN_FNF,       "OPEN",   "FNF",        "file not found" },
    { MSG_OPEN_PRV,       "OPEN",   "PRV",        "insufficient privilege" },

    /* PRINT facility (15) */
    { MSG_PRINT_OPENIN,   "PRINT",  "OPENIN",     "error opening file as input" },

    /* PURGE facility (16) */
    { MSG_PURGE_FNF,      "PURGE",  "FNF",        "file not found" },

    /* SORT facility (17) */
    { MSG_SORT_OPENIN,    "SORT",   "OPENIN",     "error opening input file" },
    { MSG_SORT_OPENOUT,   "SORT",   "OPENOUT",    "error opening output file" },

    /* Sentinel */
    { 0, NULL, NULL, NULL }
};

#define MSG_DB_SIZE  (sizeof(msg_db) / sizeof(msg_db[0]) - 1)  /* exclude sentinel */

/* ---- Message lookup ---------------------------------------------------- */

static const struct vms_msg_entry *find_entry(uint32_t msgcode)
{
    for (size_t i = 0; i < MSG_DB_SIZE; i++) {
        if (msg_db[i].code == msgcode)
            return &msg_db[i];
    }
    return NULL;
}

/* Thread-local static buffer for formatted messages */
static _Thread_local char fmt_buf[2048];

const char *vms_message(uint32_t msgcode, const char *extra_text)
{
    const struct vms_msg_entry *ent = find_entry(msgcode);
    int sev = (int)VMS_MSG_SEV(msgcode);
    char sc = vms_severity_char(sev);

    if (ent) {
        if (extra_text && extra_text[0]) {
            snprintf(fmt_buf, sizeof(fmt_buf), "%%%s-%c-%s, %s - %s",
                     ent->facility, sc, ent->ident, ent->text, extra_text);
        } else {
            snprintf(fmt_buf, sizeof(fmt_buf), "%%%s-%c-%s, %s",
                     ent->facility, sc, ent->ident, ent->text);
        }
    } else {
        /* Unknown code — format generically */
        if (extra_text && extra_text[0]) {
            snprintf(fmt_buf, sizeof(fmt_buf), "%%SYSTEM-E-UNKNOWN, unknown message code 0x%08X - %s",
                     msgcode, extra_text);
        } else {
            snprintf(fmt_buf, sizeof(fmt_buf), "%%SYSTEM-E-UNKNOWN, unknown message code 0x%08X",
                     msgcode);
        }
    }

    return fmt_buf;
}

const char *vms_msg_lookup(uint32_t msgcode)
{
    return vms_message(msgcode, NULL);
}

const char *vms_msg_facility(uint32_t msgcode)
{
    const struct vms_msg_entry *ent = find_entry(msgcode);
    return ent ? ent->facility : "SYSTEM";
}

const char *vms_msg_ident(uint32_t msgcode)
{
    const struct vms_msg_entry *ent = find_entry(msgcode);
    return ent ? ent->ident : "UNKNOWN";
}

/* ---- errno to VMS translation ------------------------------------------ */

struct errno_map {
    int          unix_errno;
    uint32_t     vms_code;
    const char  *extra;      /* Optional extra text (NULL if default msg is fine) */
};

static const struct errno_map errno_table[] = {
    { EACCES,  MSG_RMS_PRV,           NULL },
    { EPERM,   MSG_SYSTEM_NOPRIV,     NULL },
    { ENOENT,  MSG_RMS_FNF,           NULL },
    { EEXIST,  MSG_RMS_FEX,           NULL },
    { EISDIR,  MSG_RMS_FAC,           "is a directory" },
    { ENOTDIR, MSG_RMS_DNF,           "not a directory" },
    { ENOTEMPTY, MSG_RMS_DEL,         "directory not empty" },
    { ENAMETOOLONG, MSG_SYSTEM_BADPARAM, "file specification too long" },
    { ENOSPC,  MSG_RMS_CRE,           "device full" },
    { EROFS,   MSG_RMS_PRV,           "write-locked device" },
    { EMFILE,  MSG_RMS_ACC,           "too many open files" },
    { ENFILE,  MSG_RMS_ACC,           "system file table overflow" },
    { EBADF,   MSG_RMS_FAC,           "bad file descriptor" },
    { EIO,     MSG_RMS_RER,           "device I/O error" },
    { ENOMEM,  MSG_SYSTEM_ABORT,      "insufficient memory" },
    { EINVAL,  MSG_SYSTEM_BADPARAM,   NULL },
    { EBUSY,   MSG_RMS_FLK,           "resource busy" },
    { EXDEV,   MSG_RMS_ACC,           "cross-device rename" },
    { ELOOP,   MSG_RMS_ACC,           "too many symbolic links" },
    { ETXTBSY, MSG_RMS_FLK,           "text file busy" },
    { 0, 0, NULL }  /* sentinel */
};

const char *vms_strerror(int unix_errno)
{
    for (const struct errno_map *m = errno_table; m->unix_errno != 0; m++) {
        if (m->unix_errno == unix_errno)
            return vms_message(m->vms_code, m->extra);
    }

    /* Fallback: generic system error, no Unix text */
    snprintf(fmt_buf, sizeof(fmt_buf),
             "%%SYSTEM-E-FILACCERR, file access error (error code %d)",
             unix_errno);
    return fmt_buf;
}

/* ---- Linux path to VMS path for error messages ------------------------- */

const char *vms_error_path(const char *linux_path)
{
    static _Thread_local char path_buf[1024];

    if (!linux_path || !linux_path[0])
        return "";

    /* Try the proper VMS filespec formatter */
    if (dcl_format_filespec(linux_path, path_buf, sizeof(path_buf)) == 0) {
        return path_buf;
    }

    /* Fallback: just uppercase the basename */
    const char *basename = strrchr(linux_path, '/');
    if (basename)
        basename++;
    else
        basename = linux_path;

    size_t i;
    for (i = 0; basename[i] && i < sizeof(path_buf) - 1; i++) {
        path_buf[i] = (char)toupper((unsigned char)basename[i]);
    }
    path_buf[i] = '\0';
    return path_buf;
}
