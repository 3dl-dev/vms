/*
 * dcl_builtin.c - DCL Built-in Command Dispatch Table
 *
 * Contains the verb dispatch table (builtin_verbs[]), verb lookup,
 * and shared data structures used by the split command files.
 *
 * Command implementations are in:
 *   dcl_cmd_show.c    - SHOW commands
 *   dcl_cmd_set.c     - SET commands
 *   dcl_cmd_file.c    - File operation commands
 *   dcl_cmd_process.c - Process/job commands
 *   dcl_cmd_io.c      - Device I/O commands
 *   dcl_cmd_misc.c    - Remaining commands
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/cdu.h"
#include "dcl/dcl_cmd.h"
#include "ssdef.h"

/* BACKUP command (dcl_backup.c) */
extern int cmd_backup(struct dcl_command *cmd);

/* LIBRARY command (dcl_library.c) */
extern int cmd_library(struct dcl_command *cmd);

/* ================================================================== */
/*                     Shared Data                                     */
/* ================================================================== */

/* VMS month abbreviations */
const char *vms_months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

/* ================================================================== */
/*             Per-verb qualifier tables (Phase 1, vms-097)            */
/* ================================================================== */
/*
 * These declare the qualifiers each verb ACTUALLY implements (the ones its
 * handler reads via dcl_has_qualifier()/dcl_qualifier_value()). The parser
 * (dcl_validate_qualifiers(), dcl_parser.c) rejects anything else with the
 * authentic %DCL-W-IVQUAL before the handler runs -- closing the universal
 * silent-accept hole the fidelity audit named (docs/design-dcl-fidelity.md
 * sec 2). Per INV-DCL (sec 3) a table lists ONLY implemented qualifiers:
 * declaring a qualifier the handler ignores would recreate the fake-success
 * facade. Qualifiers real VMS accepts but OVMX does not yet implement are
 * therefore honestly rejected (an over-restriction, not a lie).
 *
 * Qualifier NAMES, value-types, and keyword sets are grounded in the public
 * VSI OpenVMS DCL Dictionary per-command entries and the Command Definition
 * Utility manual (project Rule 8) -- never invented. A verb with NO table
 * (quals == NULL in builtin_verbs[]) is not yet retrofit and keeps the legacy
 * accept-all behaviour; those are the Phase 1 follow-up (see vms-097 notes).
 */

/* {name, value-type, flags, keywords, default} ; terminate with {NULL,...} */
#define QUAL_END { NULL, CDU_VT_NONE, 0, NULL, NULL }

/* DIRECTORY /DATE keyword set. OVMX surfaces exactly one timestamp from
 * stat(2) -- the modification time -- so MODIFIED is the only keyword it can
 * honour; the other DCL Dictionary keywords (CREATED, EXPIRED, BACKUP, ALL)
 * are honestly rejected with %DCL-W-IVKEYW rather than silently mapped to the
 * mtime (which would be the fake-success facade INV-DCL bans). */
static const char *const dir_date_keywords[] = { "MODIFIED", NULL };

static const struct dcl_qual_def q_type[] = {
    { "PAGE", CDU_VT_NONE, CDU_Q_NEGATABLE, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_copy[] = {
    { "LOG", CDU_VT_NONE, CDU_Q_NEGATABLE, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_delete[] = {
    { "ENTRY",   CDU_VT_VALUE, 0,               NULL, NULL },
    { "SYMBOL",  CDU_VT_NONE,  0,               NULL, NULL },
    { "GLOBAL",  CDU_VT_NONE,  0,               NULL, NULL },
    { "CONFIRM", CDU_VT_NONE,  CDU_Q_NEGATABLE, NULL, NULL },
    { "LOG",     CDU_VT_NONE,  CDU_Q_NEGATABLE, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_rename[] = {
    { "LOG", CDU_VT_NONE, CDU_Q_NEGATABLE, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_create[] = {
    { "DIRECTORY", CDU_VT_NONE, 0, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_search[] = {
    { "EXACT",      CDU_VT_NONE, CDU_Q_NEGATABLE, NULL, NULL },
    { "NUMBERS",    CDU_VT_NONE, CDU_Q_NEGATABLE, NULL, NULL },
    { "STATISTICS", CDU_VT_NONE, 0,               NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_purge[] = {
    { "LOG",     CDU_VT_NONE,  CDU_Q_NEGATABLE, NULL, NULL },
    { "CONFIRM", CDU_VT_NONE,  CDU_Q_NEGATABLE, NULL, NULL },
    { "KEEP",    CDU_VT_VALUE, 0,               NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_directory[] = {
    { "SIZE",        CDU_VT_NONE,    CDU_Q_NEGATABLE, NULL, NULL },
    { "DATE",        CDU_VT_KEYWORD, CDU_Q_NEGATABLE, dir_date_keywords,
                                                      "MODIFIED" },
    { "FULL",        CDU_VT_NONE,    0,               NULL, NULL },
    { "BRIEF",       CDU_VT_NONE,    0,               NULL, NULL },
    { "OWNER",       CDU_VT_NONE,    CDU_Q_NEGATABLE, NULL, NULL },
    { "TOTAL",       CDU_VT_NONE,    0,               NULL, NULL },
    { "GRAND_TOTAL", CDU_VT_NONE,    0,               NULL, NULL },
    { "HEADING",     CDU_VT_NONE,    CDU_Q_NEGATABLE | CDU_Q_DEFAULT,
                                                      NULL, NULL },
    { "TRAILING",    CDU_VT_NONE,    CDU_Q_NEGATABLE, NULL, NULL },
    { "COLUMNS",     CDU_VT_VALUE,   0,               NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_print[] = {
    { "QUEUE", CDU_VT_VALUE, 0, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_submit[] = {
    { "QUEUE", CDU_VT_VALUE, 0, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_sort[] = {
    { "REVERSE", CDU_VT_NONE, 0, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_dump[] = {
    { "BLOCKS", CDU_VT_VALUE, 0, NULL, NULL },
    QUAL_END
};
/* STOP (vms-1a8, docs/design-dcl-fidelity.md sec 5 Phase 2). The Dictionary
 * lists /IDENTIFICATION as the only qualifier on this (process-control) STOP
 * entry -- /QUEUE, /CPU, /NETWORK belong to the SEPARATE STOP/QUEUE etc.
 * dictionary entries, which OVMX has never implemented, so they draw the
 * authentic %DCL-W-IVQUAL here rather than being silently accepted. */
static const struct dcl_qual_def q_stop[] = {
    { "IDENTIFICATION", CDU_VT_VALUE, CDU_Q_VALREQ, NULL, NULL },
    QUAL_END
};
/* APPEND and DIFFERENCES read NO qualifiers today: an explicit empty table
 * makes any qualifier on them fail honestly with %DCL-W-IVQUAL rather than be
 * silently swallowed. */
static const struct dcl_qual_def q_none[] = {
    QUAL_END
};

/* ================================================================== */
/*                     Command Table                                   */
/* ================================================================== */

static struct dcl_verb builtin_verbs[] = {
    { "ACCOUNTING",  cmd_accounting,  CDU_F_ABBREV | CDU_F_QUALIFIER, 4,
      "Display login accounting information for the current user" },
    { "ANALYZE",     cmd_analyze,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Analyze system components" },
    { "APPEND",      cmd_append,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Append source file to destination file", q_none },
    { "ASSIGN",      cmd_assign,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Assign a logical name (equivalence name to logical name)" },
    { "ATTACH",      cmd_attach,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Transfer terminal control to another process" },
    { "BACKUP",      cmd_backup,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Create, restore, or list a saveset file" },
    { "CLOSE",       cmd_close,       CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Close a file that was opened for I/O" },
    { "CONTINUE",    cmd_continue,    CDU_F_ABBREV, 4,
      "Resume execution of an interrupted image" },
    { "CONVERT",     cmd_convert,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Convert file format" },
    { "COPY",        cmd_copy,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Copy a file", q_copy },
    { "CREATE",      cmd_create,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Create a new file (or directory with /DIRECTORY)", q_create },
    { "DEASSIGN",    cmd_deassign,    CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Deassign (remove) a logical name" },
    { "DEFINE",      cmd_define,      CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Create a logical name definition" },
    { "DELETE",      cmd_delete,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Delete a file (or symbol with /SYMBOL)", q_delete },
    { "DIFFERENCES", cmd_differences, CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Compare two files and display differences", q_none },
    { "DIRECTORY",   cmd_directory,   CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "List files in a directory", q_directory },
    { "DISMOUNT",    cmd_dismount,    CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Dismount a volume from a device" },
    { "DUMP",        cmd_dump,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Display contents of a file in hexadecimal and ASCII", q_dump },
    { "EDIT",        cmd_edit,        CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Invoke the EDT text editor" },
    { "EXIT",        cmd_exit,        CDU_F_ABBREV, 2,
      "Terminate a command procedure or session" },
    { "HELP",        cmd_help,        CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Obtain information about DCL commands" },
    { "INQUIRE",     cmd_inquire,     CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Read input from SYS$INPUT and assign to a symbol" },
    { "INSTALL",     cmd_install,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Manage known images" },
    { "LIBRARY",     cmd_library,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Manage text, help, and object libraries" },
    { "LINK",        cmd_link,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Link object modules into executable image" },
    { "LOGOUT",      cmd_logout,      CDU_F_ABBREV, 2,
      "Terminate an interactive session" },
    { "MAIL",      cmd_mail,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Send and receive electronic mail messages" },
    { "MONITOR",   cmd_monitor,   CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Display real-time system activity statistics" },
    { "MOUNT",       cmd_mount,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Mount a volume on a device" },
    { "OPEN",        cmd_open,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Open a file for reading or writing" },
    { "PHONE",       cmd_phone,       CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Phone utility for interactive conversation" },
    { "PIPE",        cmd_pipe,        CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Execute a DCL pipeline (cmd1 | cmd2 | ...)" },
    { "PRINT",       cmd_print,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Queue a file for printing", q_print },
    { "PRODUCT",     cmd_product,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Software product management" },
    { "PURGE",       cmd_purge,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Delete old versions of a file", q_purge },
    { "RECALL",    cmd_recall,    CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Show or re-execute commands from command history" },
    { "READ",        cmd_read,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Read a record from a file into a symbol" },
    { "RENAME",      cmd_rename,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Change the name and/or location of a file", q_rename },
    { "REPLY",       cmd_reply,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Send an operator reply or enable/disable operator terminal" },
    { "REQUEST",     cmd_request,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Send a request message to the operator" },
    { "RUN",         cmd_run,         CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Execute a program image" },
    { "SEARCH",      cmd_search,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Search a file for a text string", q_search },
    { "SET",         cmd_set,         CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Set or modify system, process, or file characteristics" },
    { "SHOW",        cmd_show,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Display information about the system, process, or files" },
    { "SORT",        cmd_sort,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Sort records in a file", q_sort },
    { "SPAWN",       cmd_spawn,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Create a subprocess" },
    { "STOP",        cmd_stop,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Terminate the current image or command, or delete a named process", q_stop },
    { "SUBMIT",      cmd_submit,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Submit a command procedure to a batch queue", q_submit },
    { "SYSGEN",      cmd_sysgen,      CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Invoke SYSGEN system parameter utility" },
    { "SYSMAN",      cmd_sysman,      CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Invoke SYSMAN system management utility" },
    { "TCPIP",       cmd_tcpip,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "TCP/IP Services network management commands" },
    { "TYPE",        cmd_type,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Display the contents of a file", q_type },
    { "WAIT",        cmd_wait,        CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Wait for a specified time interval" },
    { "WRITE",       cmd_write,       CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Write a record to a file" },
};

static int builtin_count = (int)(sizeof(builtin_verbs) / sizeof(builtin_verbs[0]));

/*
 * Find a verb by name (with minimum-uniqueness abbreviation matching).
 */
const struct dcl_verb *dcl_find_verb(const char *name)
{
    if (!name || !name[0]) return NULL;

    for (int i = 0; i < builtin_count; i++) {
        if (dcl_match_command(name, builtin_verbs[i].name,
                              builtin_verbs[i].min_abbrev)) {
            return &builtin_verbs[i];
        }
    }
    return NULL;
}

/*
 * Get the full verb table (for HELP listing).
 */
const struct dcl_verb *dcl_get_verb_table(int *count)
{
    if (count) *count = builtin_count;
    return builtin_verbs;
}

/*
 * Register built-in commands (called during initialization).
 * Currently a no-op since we use a static table, but reserved
 * for future dynamic command registration.
 */
void dcl_register_builtins(void)
{
    /* Static table - nothing to do */
}
