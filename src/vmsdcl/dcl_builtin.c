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
/* DIRECTORY/SIZE=option (VSI OpenVMS DCL Dictionary): USED (default),
 * ALLOCATION, or ALL (used/allocated). */
static const char *const dir_size_keywords[] = {
    "USED", "ALLOCATION", "ALL", NULL
};

static const struct dcl_qual_def q_type[] = {
    { "PAGE", CDU_VT_NONE, CDU_Q_NEGATABLE, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_copy[] = {
    { "LOG",     CDU_VT_NONE, CDU_Q_NEGATABLE, NULL, NULL },
    /* vms-7543: /CONFIRM prompts for Y/N before the copy (DCL Dictionary COPY
     * /CONFIRM). Honoured by cmd_copy(); /NOCONFIRM is the default. */
    { "CONFIRM", CDU_VT_NONE, CDU_Q_NEGATABLE, NULL, NULL },
    /* vms-1c6: /NEW_VERSION forces a higher output version when an explicit
     * output version collides (DCL Dictionary COPY /NEW_VERSION). */
    { "NEW_VERSION", CDU_VT_NONE, CDU_Q_NEGATABLE, NULL, NULL },
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
    /* vms-1c6: /NEW_VERSION forces a higher output version on collision
     * (DCL Dictionary RENAME /NEW_VERSION). */
    { "NEW_VERSION", CDU_VT_NONE, CDU_Q_NEGATABLE, NULL, NULL },
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
    { "SIZE",        CDU_VT_KEYWORD, CDU_Q_NEGATABLE, dir_size_keywords,
                                                      "USED" },
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
    /* vms-7543 coverage additions -- each honoured by cmd_directory():
     *   /PROTECTION  display the file protection column (VMS DCL Dictionary:
     *                DIRECTORY /PROTECTION). Already rendered under /FULL; now
     *                selectable on its own.
     *   /VERSIONS=n  limit the number of versions listed per file (real
     *                selection; n<=0 rejected). DCL Dictionary /VERSIONS=n.
     *   /EXCLUDE=(spec[,...])  omit files whose name matches any spec, via the
     *                same VMS wildcard engine used for the positional pattern.
     * All three do real work in the handler -- not declared-and-ignored
     * (INV-DCL sec 3). */
    { "PROTECTION",  CDU_VT_NONE,    CDU_Q_NEGATABLE, NULL, NULL },
    { "VERSIONS",    CDU_VT_VALUE,   CDU_Q_VALREQ,    NULL, NULL },
    { "EXCLUDE",     CDU_VT_VALUE,   CDU_Q_VALREQ,    NULL, NULL },
    QUAL_END
};
/* PRINT/SUBMIT (vms-7543). Both submit into the real queue manager
 * (vmsq_submit), so the qualifiers that map onto a queue-entry field or a
 * queue-manager call are honoured for real:
 *   /QUEUE=name  target queue (already implemented).
 *   /NAME=string overrides the job name written to the entry
 *                (vms_queue_entry.job_name) -- DCL Dictionary /NAME=job-name.
 *   /HOLD        submit then place the entry in HOLDING state via
 *                vmsq_hold_entry() -- DCL Dictionary /HOLD (default /NOHOLD).
 * Qualifiers with no backing field in the queue entry today (/COPIES,
 * /PRIORITY, /AFTER, /PARAMETERS, ...) are deliberately NOT listed: they draw
 * the authentic %DCL-W-IVQUAL (honest over-restriction) rather than being
 * silently accepted and dropped (INV-DCL sec 3). */
static const struct dcl_qual_def q_print[] = {
    { "QUEUE", CDU_VT_VALUE, 0,               NULL, NULL },
    { "NAME",  CDU_VT_VALUE, CDU_Q_VALREQ,    NULL, NULL },
    { "HOLD",  CDU_VT_NONE,  CDU_Q_NEGATABLE, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_submit[] = {
    { "QUEUE", CDU_VT_VALUE, 0,               NULL, NULL },
    { "NAME",  CDU_VT_VALUE, CDU_Q_VALREQ,    NULL, NULL },
    { "HOLD",  CDU_VT_NONE,  CDU_Q_NEGATABLE, NULL, NULL },
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
/*          Engine A rollout, tranche 2 (vms-7543, vms-b9a)            */
/* ================================================================== */
/*
 * Retrofit tables for the in-process, self-parsing verbs that Phase 1 left as
 * legacy accept-all. Each lists EXACTLY the qualifiers its handler reads via
 * dcl_has_qualifier()/dcl_qualifier_value() (verified against the handler
 * bodies) so %DCL-W-IVQUAL is now structurally reachable for them too; every
 * other qualifier real VMS accepts but OVMX does not yet implement is honestly
 * rejected (over-restriction, not a lie -- INV-DCL sec 3). Names/value-types
 * are grounded in the public VSI DCL Dictionary per-command entries.
 *
 * The tail (ANALYZE, LINK, PRODUCT, MOUNT, DISMOUNT, BACKUP, REQUEST, EDIT,
 * ACCOUNTING, MONITOR, SYSGEN, SYSMAN) is retrofit by vms-332 -- see the
 * "Engine A rollout, tail" tables further down; the empty-table verbs reuse
 * q_none[] (they honour no qualifier, so any qualifier is IVQUAL).
 *
 * STILL NOT retrofit here, with the reason (kept accept-all / NULL on purpose):
 *   - True pass-through delegators: MAIL and INSTALL forward EVERY qualifier
 *     verbatim to the child SYS$SYSTEM:*.EXE, which validates them
 *     authentically; a DCL-side table would wrongly reject valid ones. (This
 *     is the ONLY delegator shape that keeps NULL -- ANALYZE/LINK/PRODUCT/
 *     MONITOR read specific qualifiers and drop the rest, so they DO get a
 *     table above.)
 *   - SET/SHOW/TCPIP umbrellas: qualifiers depend on the sub-verb (param[0]);
 *     a single flat table is wrong -- they need per-sub-verb CLD tables
 *     (Tier-2 follow-up; SET already validates several sub-verbs by hand).
 *   - RUN: self-validates through its own run_process_qualifiers[] layer
 *     (oracle-pinned shortest-unique-prefix + two HELP topics); a flat CDU
 *     table would fight that abbreviation logic. Left to that layer.
 *   - LIBRARY: full LIBRARIAN grammar (/CREATE,/REPLACE,/INSERT,/EXTRACT,
 *     /LIST,/DELETE,/COMPRESS,/HELP,/OBJECT,/TEXT,/VAX,/OUTPUT). The
 *     self-hosting MMK corpus (tests/corpus/tier3-mmk) uses /REPLACE,
 *     /COMPRESS et al. that the handler routes by param-count rather than
 *     reading, so a "what it reads" table would break those builds -- needs
 *     the full grammar, a sized follow-up.
 *   - READ/WRITE: control-flow qualifiers -- READ /END_OF_FILE=label and
 *     /ERROR=label, WRITE /SYMBOL and /ERROR=label -- want REAL branch/symbol
 *     semantics, not restriction. WRITE/SYMBOL is used by the MMK corpus and
 *     READ/END=/ERR= throughout the MX corpus; a thin table would IVQUAL them.
 *     Filed as follow-ups.
 */

/* Logical-name placement qualifiers, shared by ASSIGN/DEFINE/DEASSIGN. The
 * handlers read SYSTEM/GROUP/JOB (and treat their absence as /PROCESS, the
 * VMS default), so /PROCESS is listed as the explicit form of that default. */
/* /TRANSLATION_ATTRIBUTES=(keyword,...) selects the LNM$M_* translation
 * attributes (CONCEALED, TERMINAL) of the created logical (VSI OpenVMS DCL
 * Dictionary, DEFINE /TRANSLATION_ATTRIBUTES). Absent it, the logical has NO
 * translation attributes -- the VMS default the handlers now honor (vms-240):
 * non-terminal, so its equivalence stays subject to iterative translation. */
static const struct dcl_qual_def q_assign[] = {
    { "PROCESS",                 CDU_VT_NONE,  0,            NULL, NULL },
    { "SYSTEM",                  CDU_VT_NONE,  0,            NULL, NULL },
    { "GROUP",                   CDU_VT_NONE,  0,            NULL, NULL },
    { "JOB",                     CDU_VT_NONE,  0,            NULL, NULL },
    { "TABLE",                   CDU_VT_VALUE, CDU_Q_VALREQ, NULL, NULL },
    { "TRANSLATION_ATTRIBUTES",  CDU_VT_LIST,  0,            NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_define[] = {
    { "PROCESS",                 CDU_VT_NONE, 0, NULL, NULL },
    { "SYSTEM",                  CDU_VT_NONE, 0, NULL, NULL },
    { "GROUP",                   CDU_VT_NONE, 0, NULL, NULL },
    { "JOB",                     CDU_VT_NONE, 0, NULL, NULL },
    { "TRANSLATION_ATTRIBUTES",  CDU_VT_LIST, 0, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_deassign[] = {
    { "PROCESS", CDU_VT_NONE, 0, NULL, NULL },
    { "SYSTEM",  CDU_VT_NONE, 0, NULL, NULL },
    { "GROUP",   CDU_VT_NONE, 0, NULL, NULL },
    { "JOB",     CDU_VT_NONE, 0, NULL, NULL },
    { "ALL",     CDU_VT_NONE, 0, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_open[] = {
    { "READ",   CDU_VT_NONE, 0, NULL, NULL },
    { "WRITE",  CDU_VT_NONE, 0, NULL, NULL },
    { "APPEND", CDU_VT_NONE, 0, NULL, NULL },
    QUAL_END
};
/* SPAWN reads /NOWAIT and /OUTPUT. Declaring "NOWAIT" (not "WAIT" negatable)
 * matches the handler's literal dcl_has_qualifier(cmd,"NOWAIT") read: the
 * parser splits /NOWAIT into name="WAIT" negated, and the validator's
 * NO-undo path reconstructs it to name="NOWAIT" so the read matches -- which
 * ALSO repairs a latent bug where /NOWAIT never took effect pre-rollout. */
static const struct dcl_qual_def q_spawn[] = {
    { "NOWAIT",  CDU_VT_NONE,  0,            NULL, NULL },
    { "OUTPUT",  CDU_VT_VALUE, CDU_Q_VALREQ, NULL, NULL },
    /* /PROCESS=name names the subprocess (OpenVMS DCL Dictionary, SPAWN
     * /PROCESS): the handler reads dcl_qualifier_value(cmd,"PROCESS") and
     * passes it to vms_kif_setprn, so the name must reach the handler rather
     * than being rejected by the validator with %DCL-W-IVQUAL. VALREQ: a name
     * is required. Only qualifiers the handler actually honors are declared
     * (honest over-restriction, INV-DCL): the remaining VMS SPAWN qualifiers
     * (/INPUT, /LOG, /SYMBOLS, /LOGICAL_NAMES, ...) stay %DCL-W-IVQUAL until
     * the handler acts on them, never a fake accept. */
    { "PROCESS", CDU_VT_VALUE, CDU_Q_VALREQ, NULL, NULL },
    QUAL_END
};
/* INQUIRE reads /NOPUNCTUATION literally (same NO-undo case as SPAWN's
 * /NOWAIT; the table makes the read actually resolve). */
static const struct dcl_qual_def q_inquire[] = {
    { "NOPUNCTUATION", CDU_VT_NONE, 0, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_attach[] = {
    { "IDENTIFICATION", CDU_VT_VALUE, CDU_Q_VALREQ, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_convert[] = {
    { "FDL", CDU_VT_VALUE, 0, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_reply[] = {
    { "ENABLE",  CDU_VT_VALUE, 0, NULL, NULL },
    { "DISABLE", CDU_VT_NONE,  0, NULL, NULL },
    { "TO",      CDU_VT_VALUE, 0, NULL, NULL },
    QUAL_END
};
static const struct dcl_qual_def q_recall[] = {
    { "ALL",   CDU_VT_NONE, 0, NULL, NULL },
    { "ERASE", CDU_VT_NONE, 0, NULL, NULL },
    QUAL_END
};

/* ================================================================== */
/*          Engine A rollout, tail (vms-332, vms-8ad/vms-b9a)          */
/* ================================================================== */
/*
 * Continues the tranche-2 rollout (vms-7543) across the remaining discrete,
 * non-umbrella verbs. Same rule as above: each table lists EXACTLY the
 * qualifiers its handler HONOURS (verified against the handler body), so a
 * real qualifier parses and every other token draws the authentic
 * %DCL-W-IVQUAL -- honest over-restriction, never a fake accept (INV-DCL sec
 * 3). Names/value-types grounded in the public VSI OpenVMS DCL Dictionary
 * per-command entries (project Rule 8), cited per verb below.
 *
 * ANALYZE (DCL Dictionary: ANALYZE) is a mode dispatcher: cmd_analyze() reads
 * exactly the four mode selectors and re-execs ANALYZE.EXE with the chosen
 * mode; a token that is not one of the four never reaches a mode, so it is a
 * genuine %DCL-W-IVQUAL. (Sub-mode qualifiers -- /HEADER under /IMAGE, etc. --
 * are a follow-up; the handler does not forward them today.) */
static const struct dcl_qual_def q_analyze[] = {
    { "DISK_STRUCTURE", CDU_VT_NONE, 0, NULL, NULL },
    { "IMAGE",          CDU_VT_NONE, 0, NULL, NULL },
    { "OBJECT",         CDU_VT_NONE, 0, NULL, NULL },
    { "SYSTEM",         CDU_VT_NONE, 0, NULL, NULL },
    QUAL_END
};
/* LINK (DCL Dictionary: LINK). cmd_link() honours /EXECUTABLE=name (output
 * image name) and /MAP (produce a link map, /NOMAP the default); it forks the
 * system linker for both. The wider LINK grammar (/SHAREABLE, /DEBUG,
 * /SYSLIB, ...) is a follow-up -- not forwarded today, so honestly rejected. */
static const struct dcl_qual_def q_link[] = {
    { "EXECUTABLE", CDU_VT_VALUE, CDU_Q_VALREQ,    NULL, NULL },
    { "MAP",        CDU_VT_NONE,  CDU_Q_NEGATABLE, NULL, NULL },
    QUAL_END
};
/* PRODUCT (DCL Dictionary: PRODUCT). cmd_product() honours /SOURCE=kit and
 * /DESTINATION=device for the INSTALL/SHOW sub-verbs and re-execs PRODUCT.EXE.
 * Both take a required value. Other PCSI qualifiers (/OPTIONS, /REMOTE, ...)
 * are a follow-up. */
static const struct dcl_qual_def q_product[] = {
    { "SOURCE",      CDU_VT_VALUE, CDU_Q_VALREQ, NULL, NULL },
    { "DESTINATION", CDU_VT_VALUE, CDU_Q_VALREQ, NULL, NULL },
    QUAL_END
};
/* MOUNT (DCL Dictionary: MOUNT). cmd_mount() honours /SYSTEM (place the
 * device logical in LNM$SYSTEM instead of the process table); absence is the
 * process-table default. The full MOUNT utility grammar (/OVERRIDE, /FOREIGN,
 * /NOWRITE, /CLUSTER, ...) is a sized follow-up in the SET VOLUME mould
 * (per-qualifier honest refusal); until then those draw %DCL-W-IVQUAL. */
static const struct dcl_qual_def q_mount[] = {
    { "SYSTEM", CDU_VT_NONE, 0, NULL, NULL },
    QUAL_END
};
/* BACKUP (DCL Dictionary: BACKUP). cmd_backup() honours /SAVE_SET (name the
 * output as a saveset) and /LIST (list a saveset's contents); it does real
 * archive/restore/list I/O. The wider BACKUP grammar (/IMAGE, /VERIFY, /LOG,
 * /REWIND, /IGNORE, ...) is a sized follow-up; until then honestly rejected. */
static const struct dcl_qual_def q_backup[] = {
    { "SAVE_SET", CDU_VT_NONE, 0, NULL, NULL },
    { "LIST",     CDU_VT_NONE, 0, NULL, NULL },
    QUAL_END
};

/* ================================================================== */
/*                     Command Table                                   */
/* ================================================================== */

static struct dcl_verb builtin_verbs[] = {
    { "ACCOUNTING",  cmd_accounting,  CDU_F_ABBREV | CDU_F_QUALIFIER, 4,
      "Display login accounting information for the current user", q_none },
    { "ANALYZE",     cmd_analyze,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Analyze system components", q_analyze },
    { "APPEND",      cmd_append,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Append source file to destination file", q_none },
    { "ASSIGN",      cmd_assign,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Assign a logical name (equivalence name to logical name)", q_assign },
    { "ATTACH",      cmd_attach,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Transfer terminal control to another process", q_attach },
    { "BACKUP",      cmd_backup,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Create, restore, or list a saveset file", q_backup },
    { "CLOSE",       cmd_close,       CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Close a file that was opened for I/O", q_none },
    { "CONTINUE",    cmd_continue,    CDU_F_ABBREV, 4,
      "Resume execution of an interrupted image", q_none },
    { "CONVERT",     cmd_convert,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Convert file format", q_convert },
    { "COPY",        cmd_copy,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Copy a file", q_copy },
    { "CREATE",      cmd_create,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Create a new file (or directory with /DIRECTORY)", q_create },
    { "DEASSIGN",    cmd_deassign,    CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Deassign (remove) a logical name", q_deassign },
    { "DEFINE",      cmd_define,      CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Create a logical name definition", q_define },
    { "DELETE",      cmd_delete,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Delete a file (or symbol with /SYMBOL)", q_delete },
    { "DIFFERENCES", cmd_differences, CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Compare two files and display differences", q_none },
    { "DIRECTORY",   cmd_directory,   CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "List files in a directory", q_directory },
    { "DISMOUNT",    cmd_dismount,    CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Dismount a volume from a device", q_none },
    { "DUMP",        cmd_dump,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Display contents of a file in hexadecimal and ASCII", q_dump },
    { "EDIT",        cmd_edit,        CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Invoke the EDT text editor", q_none },
    { "EXIT",        cmd_exit,        CDU_F_ABBREV, 2,
      "Terminate a command procedure or session", q_none },
    { "FTP",         cmd_ftp,         CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "FTP file transfer client (/GET, /PUT over TCP/IP Services BGn:)" },
    /* HELP takes accept-all qualifiers (quals == NULL, not q_none): a slash
     * token after HELP is a command-qualifier TOPIC to look up (e.g.
     * HELP DIRECTORY QUALIFIERS /EXCLUDE), exactly as VMS HELP treats it, so
     * cmd_help folds them into the topic path. A q_none table would instead
     * draw %DCL-W-IVQUAL and make every qualifier topic unreachable (vms-01b). */
    { "HELP",        cmd_help,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER,
      2, "Obtain information about DCL commands", NULL },
    { "INQUIRE",     cmd_inquire,     CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Read input from SYS$INPUT and assign to a symbol", q_inquire },
    { "INSTALL",     cmd_install,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Manage known images" },
    { "INITIALIZE",  cmd_initialize,  CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Format a volume with VMSFS structure", q_none },
    { "LIBRARY",     cmd_library,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Manage text, help, and object libraries" },
    { "LINK",        cmd_link,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Link object modules into executable image", q_link },
    { "LOGOUT",      cmd_logout,      CDU_F_ABBREV, 2,
      "Terminate an interactive session", q_none },
    { "MAIL",      cmd_mail,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Send and receive electronic mail messages" },
    { "MONITOR",   cmd_monitor,   CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Display real-time system activity statistics", q_none },
    { "MOUNT",       cmd_mount,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Mount a volume on a device", q_mount },
    { "OPEN",        cmd_open,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Open a file for reading or writing", q_open },
    { "PHONE",       cmd_phone,       CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Phone utility for interactive conversation", q_none },
    { "PIPE",        cmd_pipe,        CDU_F_ABBREV | CDU_F_PARAM, 3,
      "Execute a DCL pipeline (cmd1 | cmd2 | ...)", q_none },
    { "PRINT",       cmd_print,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Queue a file for printing", q_print },
    { "PRODUCT",     cmd_product,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 4,
      "Software product management", q_product },
    { "PURGE",       cmd_purge,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Delete old versions of a file", q_purge },
    { "RECALL",    cmd_recall,    CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Show or re-execute commands from command history", q_recall },
    { "READ",        cmd_read,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Read a record from a file into a symbol" },
    { "RENAME",      cmd_rename,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Change the name and/or location of a file", q_rename },
    { "REPLY",       cmd_reply,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Send an operator reply or enable/disable operator terminal", q_reply },
    { "REQUEST",     cmd_request,     CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Send a request message to the operator", q_none },
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
      "Create a subprocess", q_spawn },
    { "STOP",        cmd_stop,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Terminate the current image or command, or delete a named process", q_stop },
    { "SUBMIT",      cmd_submit,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "Submit a command procedure to a batch queue", q_submit },
    { "SYSGEN",      cmd_sysgen,      CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Invoke SYSGEN system parameter utility", q_none },
    { "SYSMAN",      cmd_sysman,      CDU_F_ABBREV | CDU_F_PARAM, 4,
      "Invoke SYSMAN system management utility", q_none },
    { "TCPIP",       cmd_tcpip,       CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "TCP/IP Services network management commands" },
    { "TELNET",      cmd_telnet,      CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 3,
      "TELNET remote terminal client (over TCP/IP Services BGn:)" },
    { "TYPE",        cmd_type,        CDU_F_ABBREV | CDU_F_PARAM | CDU_F_QUALIFIER, 2,
      "Display the contents of a file", q_type },
    { "WAIT",        cmd_wait,        CDU_F_ABBREV | CDU_F_PARAM, 2,
      "Wait for a specified time interval", q_none },
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
