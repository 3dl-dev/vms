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
/*                     VMS Device Table                                */
/* ================================================================== */

struct vms_device vms_device_table[VMS_MAX_DEVICES];
int vms_device_count = 0;

/*
 * Find a device in the table by VMS name (case-insensitive).
 * The name may or may not include trailing colon.
 */
struct vms_device *vms_find_device(const char *name)
{
    char upper[16];
    size_t len = strlen(name);
    if (len >= sizeof(upper)) len = sizeof(upper) - 1;
    for (size_t i = 0; i < len; i++)
        upper[i] = (char)toupper((unsigned char)name[i]);
    upper[len] = '\0';
    /* Strip trailing colon for comparison */
    if (len > 0 && upper[len - 1] == ':')
        upper[--len] = '\0';

    for (int i = 0; i < vms_device_count; i++) {
        char dev[16];
        strncpy(dev, vms_device_table[i].vms_name, sizeof(dev) - 1);
        dev[sizeof(dev) - 1] = '\0';
        size_t dlen = strlen(dev);
        if (dlen > 0 && dev[dlen - 1] == ':')
            dev[--dlen] = '\0';
        if (strcasecmp(upper, dev) == 0)
            return &vms_device_table[i];
    }
    return NULL;
}

/*
 * Recognized VMS disk/tape device-class mnemonics (OpenVMS I/O device naming
 * convention — public documentation, e.g. the VSI OpenVMS I/O User's
 * Reference Manual "Device Naming" chapter). This is OVMX's own allowlist
 * standing in for "the device was autoconfigured" (OVMX has no physical
 * controllers) — flagged for operator purity sign-off per CLAUDE.md Rule 5
 * (never self-certify a VMS-authentic constant/format).
 */
static const char *known_device_classes[] = {
    "DK", "DU", "DG", "DJ", "DL", "DM", "DR", "DB", /* disks */
    "MU", "MK", "MS",                               /* tapes */
    NULL
};

int dcl_is_known_device_class(const char *name)
{
    if (!name || !name[0])
        return 0;

    const char *p = name;

    /* Optional allocation-class prefix: $<digits>$ */
    if (*p == '$') {
        p++;
        if (!isdigit((unsigned char)*p))
            return 0;
        while (isdigit((unsigned char)*p))
            p++;
        if (*p != '$')
            return 0;
        p++;
    }

    /* Exactly 3 letters: 2-letter device code + 1 controller letter */
    char code2[3];
    for (int i = 0; i < 3; i++) {
        if (!isalpha((unsigned char)p[i]))
            return 0;
        if (i < 2)
            code2[i] = (char)toupper((unsigned char)p[i]);
    }
    code2[2] = '\0';
    p += 3;

    /* At least one digit (unit number) */
    if (!isdigit((unsigned char)*p))
        return 0;
    const char *unit_start = p;
    while (isdigit((unsigned char)*p))
        p++;

    /*
     * Reject any unit number other than 0. Real VMS rejects a syntactically
     * valid but never-configured unit with the SAME error as an unrecognized
     * class -- pinned live against the oracle (OpenVMS VAX 7.3,
     * ~/vax/cluster/vax1, 2026-07-29): MOUNT DUA99: (valid class "DU", unit
     * never configured on that system) returned the IDENTICAL
     * "%MOUNT-F-NOSUCHDEV, no such device available" as the bogus class
     * MOUNT ZZQ0:. OVMX has no physical controllers to autoconfigure real
     * units against, so it stands in exactly one unit -- unit 0 -- per
     * recognized class (vms-b9f C3: DKA100:/DKA999:/MKB300:/$77$DGA4242:
     * must all be rejected, not just unrecognized classes). This unit-0-only
     * rule is an OVMX product-behavior stand-in, not itself a VMS-authentic
     * constant -- flagged for operator review alongside known_device_classes.
     */
    for (const char *d = unit_start; d < p; d++) {
        if (*d != '0')
            return 0;
    }

    /* Optional trailing colon, then must be end of string */
    if (*p == ':')
        p++;
    if (*p != '\0')
        return 0;

    for (int j = 0; known_device_classes[j]; j++) {
        if (strcmp(code2, known_device_classes[j]) == 0)
            return 1;
    }
    return 0;
}

/* ================================================================== */
/*                     Command Table                                   */
/* ================================================================== */

static struct dcl_verb builtin_verbs[] = {
    { "ACCOUNTING",  cmd_accounting,  CDU_F_ABBREV | CDU_F_QUALIFIER, 4,
      "Display login accounting information for the current user" },
    { "ANALYZE",     cmd_analyze,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Analyze system components" },
    { "APPEND",      cmd_append,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Append source file to destination file" },
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
    { "COPY",        cmd_copy,        CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Copy a file" },
    { "CREATE",      cmd_create,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Create a new file (or directory with /DIRECTORY)" },
    { "DEASSIGN",    cmd_deassign,    CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Deassign (remove) a logical name" },
    { "DEFINE",      cmd_define,      CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Create a logical name definition" },
    { "DELETE",      cmd_delete,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Delete a file (or symbol with /SYMBOL)" },
    { "DIFFERENCES", cmd_differences, CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Compare two files and display differences" },
    { "DIRECTORY",   cmd_directory,   CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "List files in a directory" },
    { "DISMOUNT",    cmd_dismount,    CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Dismount a volume from a device" },
    { "DUMP",        cmd_dump,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Display contents of a file in hexadecimal and ASCII" },
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
      "Queue a file for printing" },
    { "PRODUCT",     cmd_product,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Software product management" },
    { "PURGE",       cmd_purge,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Delete old versions of a file" },
    { "RECALL",    cmd_recall,    CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Show or re-execute commands from command history" },
    { "READ",        cmd_read,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Read a record from a file into a symbol" },
    { "RENAME",      cmd_rename,      CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Change the name and/or location of a file" },
    { "REPLY",       cmd_reply,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Send an operator reply or enable/disable operator terminal" },
    { "REQUEST",     cmd_request,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Send a request message to the operator" },
    { "RUN",         cmd_run,         CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Execute a program image" },
    { "SEARCH",      cmd_search,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Search a file for a text string" },
    { "SET",         cmd_set,         CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Set or modify system, process, or file characteristics" },
    { "SHOW",        cmd_show,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Display information about the system, process, or files" },
    { "SORT",        cmd_sort,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Sort records in a file" },
    { "SPAWN",       cmd_spawn,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Create a subprocess" },
    { "STOP",        cmd_stop,        CDU_F_ABBREV, 2,
      "Stop the current process" },
    { "SUBMIT",      cmd_submit,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Submit a command procedure to a batch queue" },
    { "SYSGEN",      cmd_sysgen,      CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Invoke SYSGEN system parameter utility" },
    { "SYSMAN",      cmd_sysman,      CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Invoke SYSMAN system management utility" },
    { "TCPIP",       cmd_tcpip,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "TCP/IP Services network management commands" },
    { "TYPE",        cmd_type,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Display the contents of a file" },
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
