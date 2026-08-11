/*
 * dcl_cmd_io.c - DCL device I/O command implementations
 *
 * ASSIGN, DEASSIGN, DEFINE, OPEN, CLOSE, READ, WRITE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "dcl/dcl_cmd.h"
#include "ssdef.h"
#include "vms/logical.h"
#include "vmsfs/filespec.h"

/*
 * ci_word_present - case-insensitive test for keyword `word` inside the
 * /TRANSLATION_ATTRIBUTES value string (a single keyword like "TERMINAL" or a
 * parenthesised list like "(CONCEALED,TERMINAL)"). Bounded on both sides so
 * "TERMINAL" does not match inside a longer token.
 */
static int ci_word_present(const char *hay, const char *word)
{
    size_t wl = strlen(word);
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, word, wl) == 0) {
            char before = (p == hay) ? '\0' : p[-1];
            char after = p[wl];
            int lb = !(isalnum((unsigned char)before) || before == '_');
            int ra = !(isalnum((unsigned char)after) || after == '_');
            if (lb && ra)
                return 1;
        }
    }
    return 0;
}

/*
 * lnm_attrs_from_qualifier - LNM$M_* translation attributes for DEFINE/ASSIGN,
 * from the /TRANSLATION_ATTRIBUTES qualifier.
 *
 * The VMS default is NON-terminal, non-concealed: a plain DEFINE/ASSIGN
 * associates NO translation attributes with the logical, so its equivalence
 * remains subject to further (iterative) logical-name translation (VSI OpenVMS
 * DCL Dictionary, DEFINE /TRANSLATION_ATTRIBUTES). /TRANSLATION_ATTRIBUTES=
 * TERMINAL marks the logical terminal (LNM$M_TERMINAL -- iterative translation
 * stops at it); =CONCEALED marks a concealed-device logical (LNM$M_CONCEALED).
 *
 * vms-240: this REPLACES the previous unconditional LNM_ATTR_TERMINAL, which
 * was the OPPOSITE of the VMS default. It only ever "worked" because both
 * iterative translators (the now-removed one in src/vmslnm and
 * vmsfs_resolve_device_r) ignored the TERMINAL flag; the moment vms-240 makes
 * them honor it, an always-terminal default would break every A -> B -> C
 * DEFINE chain, so the default MUST be non-terminal.
 */
static uint32_t lnm_attrs_from_qualifier(struct dcl_command *cmd)
{
    uint32_t attrs = 0;
    const char *ta = dcl_qualifier_value(cmd, "TRANSLATION_ATTRIBUTES");
    if (ta && ta[0]) {
        if (ci_word_present(ta, "TERMINAL"))
            attrs |= LNM_ATTR_TERMINAL;
        if (ci_word_present(ta, "CONCEALED"))
            attrs |= LNM_ATTR_CONCEALED;
    }
    return attrs;
}

/*
 * dcl_logical_is_terminal - does a resolved SYS$OUTPUT / SYS$INPUT / SYS$ERROR
 * equivalence name the terminal (the default), rather than a file to redirect
 * through? (vms-f89)
 *
 * The seeded default equivalence is "TT:" (lnm_setup_defaults), the terminal
 * device; a plain string like a file spec or a VMS/Linux path is a redirect
 * target. An empty/undefined equivalence is treated as the terminal, which
 * preserves the pre-vms-f89 behavior of writing to / reading from the
 * process's own stdout/stdin when the logical is not defined.
 */
static int dcl_logical_is_terminal(const char *equiv)
{
    if (!equiv || !equiv[0])
        return 1;
    char t[64];
    strncpy(t, equiv, sizeof(t) - 1);
    t[sizeof(t) - 1] = '\0';
    size_t n = strlen(t);
    if (n && t[n - 1] == ':')
        t[n - 1] = '\0';
    if (strcasecmp(t, "TT") == 0 || strcasecmp(t, "SYS$COMMAND") == 0)
        return 1;
    if (strncasecmp(equiv, "/dev/tty", 8) == 0)
        return 1;
    return 0;
}

/* Cached SYS$INPUT stream for a file-redirected SYS$INPUT, so successive
 * READ SYS$INPUT calls advance through the file rather than re-reading line 1
 * (vms-f89). Keyed by resolved path; re-opened when the redirection changes. */
static FILE *g_sysinput_fp;
static char  g_sysinput_key[1024];

/*
 * ASSIGN - Assign a logical name.
 * Format: ASSIGN equivalence-name logical-name
 *                 [/PROCESS | /JOB | /GROUP | /SYSTEM] [/TABLE=table-name]
 * (OpenVMS DCL Dictionary, ASSIGN.) Unlike DEFINE (which takes
 * logical-name THEN equivalence-string[,...] and supports multi-valued
 * search lists), ASSIGN's parameter order is equivalence-name THEN
 * logical-name and is single-valued -- "This command performs a subset
 * of the function of the DEFINE command" (DCL Dictionary, ASSIGN). Both
 * commands share the same default target table, LNM$PROCESS, and the
 * same /SYSTEM /GROUP /JOB /PROCESS scope qualifiers.
 */
int cmd_assign(struct dcl_command *cmd)
{
    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOKEYW",
                  "missing equivalence name and/or logical name");
        return SS$_BADPARAM;
    }

    const char *equiv   = cmd->params[0];
    const char *logname = cmd->params[1];

    /*
     * vms-263 (Phase 2, docs/design-dcl-fidelity.md sec 5): /TABLE=name
     * selects an arbitrary, caller-named logical-name table (DCL
     * Dictionary, ASSIGN /TABLE) -- real syntax, but there is no
     * generic "create/look up a table by name" registry anywhere in
     * src/vmslnm, only the four well-known tables (LNM$PROCESS/JOB/
     * GROUP/SYSTEM) that DEFINE and ASSIGN both route to below. Neither
     * command implements it. Refuse honestly rather than silently
     * ignoring it or guessing a table (INV-DCL).
     */
    if (dcl_has_qualifier(cmd, "TABLE")) {
        dcl_error("DCL", 0, "NOTIMPL",
                  "ASSIGN /TABLE is not yet implemented - no logical name "
                  "was created");
        return SS$_UNSUPPORTED;
    }

    /* Uppercase the logical name (VMS convention) */
    char upper_name[256];
    size_t i;
    for (i = 0; i < sizeof(upper_name) - 1 && logname[i]; i++)
        upper_name[i] = (char)toupper((unsigned char)logname[i]);
    upper_name[i] = '\0';

    /*
     * Target table from the scope qualifiers (/PROCESS is the default) --
     * the same precedence and the same three executive-resident tables
     * cmd_define() below already routes to. LNM$SYSTEM/GROUP/JOB are wired
     * through lnm_create() -> vms_kif -> /dev/vms (src/vmslnm/lnm_client.c's
     * is_system_table()/is_group_table()/is_job_table()): real, shared with
     * every other process on the node/group/job tree, and honestly fail
     * SS$_NOSUCHDEV with no executive present -- no per-process fallback
     * (INV-6). Reachable and already load-bearing for DEFINE, so ASSIGN
     * wires to them too rather than refusing.
     */
    const char *table = LNM_PROCESS_TABLE;
    if (dcl_has_qualifier(cmd, "SYSTEM"))
        table = LNM_SYSTEM_TABLE;
    else if (dcl_has_qualifier(cmd, "GROUP"))
        table = LNM_GROUP_TABLE;
    else if (dcl_has_qualifier(cmd, "JOB"))
        table = LNM_JOB_TABLE;

    /*
     * vms-263: reach the REAL logical-name manager, mirroring cmd_define()'s
     * LNM path below. Previously ASSIGN wrote upper_name into the DCL
     * SYMBOL table (dcl_sym_set()) regardless of qualifier -- the wrong
     * subsystem: F$TRNLNM()/SHOW LOGICAL never saw it (INV-DCL facade).
     */
    lnm_manager_t *mgr = lnm_get_manager();
    if (mgr) {
        uint32_t status = lnm_create(mgr, table, upper_name, equiv,
                                     lnm_attrs_from_qualifier(cmd),
                                     LNM_MODE_USER);
        if (status != SS$_NORMAL && status != SS$_SUPERSEDE) {
            dcl_error("DCL", 2, "LNMFAIL",
                      "failed to create logical name \\%s\\", upper_name);
            return (int)status;
        }
    } else {
        /* Graceful fallback: store as global symbol, same as DEFINE's. */
        dcl_sym_set(upper_name, equiv, DCL_SYM_GLOBAL);
    }

    return SS$_NORMAL;
}

/* ================================================================== */
/*                    Logical Name Operations                          */
/* ================================================================== */

/*
 * DEFINE - Define a logical name.
 *
 * Qualifiers:
 *   /PROCESS   (default) — create in LNM$PROCESS_TABLE
 *   /JOB                 — create in LNM$JOB
 *   /GROUP               — create in LNM$GROUP
 *   /SYSTEM              — create in LNM$SYSTEM
 *
 * Format: DEFINE logical-name equivalence-string[,equivalence-string...]
 * A comma-separated equivalence list creates a search-list (multi-valued)
 * logical name -- e.g. DEFINE SYS$STARTUP SYS$SYSROOT:[SYS$STARTUP],
 * SYS$MANAGER, exactly like real DCL. vms-420: this used to read only
 * params[1] and silently drop every equivalence string after the first
 * (DEFINE FOO BAR,BAZ created "FOO" = "BAR" with BAZ gone) because the
 * parser already splits a comma-separated parameter list into separate
 * cmd->params[] entries (dcl_parser.c's TOK_COMMA case) -- there was
 * simply nothing here consuming params[2..].
 *
 * If the LNM manager is not available, fall back to storing as a
 * global DCL symbol so callers don't lose the value entirely (only the
 * first equivalence string survives that fallback -- DCL symbols are
 * single-valued and have no search-list concept).
 */
int cmd_define(struct dcl_command *cmd)
{
    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOKEYW",
                  "missing logical name and/or equivalence string");
        return SS$_BADPARAM;
    }

    const char *logname = cmd->params[0];
    int num_equiv = cmd->param_count - 1;
    const char *equivs[DCL_MAX_PARAMS];
    for (int e = 0; e < num_equiv; e++)
        equivs[e] = cmd->params[1 + e];

    /* Uppercase the logical name */
    char upper_name[256];
    size_t i;
    for (i = 0; i < sizeof(upper_name) - 1 && logname[i]; i++)
        upper_name[i] = (char)toupper((unsigned char)logname[i]);
    upper_name[i] = '\0';

    /* Determine target table from qualifiers (/PROCESS is the default) */
    const char *table = LNM_PROCESS_TABLE;
    if (dcl_has_qualifier(cmd, "SYSTEM"))
        table = LNM_SYSTEM_TABLE;
    else if (dcl_has_qualifier(cmd, "GROUP"))
        table = LNM_GROUP_TABLE;
    else if (dcl_has_qualifier(cmd, "JOB"))
        table = LNM_JOB_TABLE;

    lnm_manager_t *mgr = lnm_get_manager();
    if (mgr) {
        uint32_t status = lnm_create_multi(mgr, table, upper_name, equivs,
                                           num_equiv,
                                           lnm_attrs_from_qualifier(cmd),
                                           LNM_MODE_USER);
        if (status != SS$_NORMAL && status != SS$_SUPERSEDE) {
            dcl_error("DCL", 2, "LNMFAIL",
                      "failed to create logical name \\%s\\", upper_name);
            return (int)status;
        }
    } else {
        /* Graceful fallback: store as global symbol (first value only) */
        dcl_sym_set(upper_name, equivs[0], DCL_SYM_GLOBAL);
    }

    return SS$_NORMAL;
}

/*
 * DEASSIGN - Remove a logical name.
 *
 * Qualifiers:
 *   /PROCESS   (default) — delete from LNM$PROCESS_TABLE
 *   /JOB                 — delete from LNM$JOB
 *   /GROUP               — delete from LNM$GROUP
 *   /SYSTEM              — delete from LNM$SYSTEM
 *   /ALL                 — delete from all tables (searches in order)
 *
 * If the LNM manager is not available, attempt to remove from the
 * global symbol table as a fallback.
 */

int cmd_deassign(struct dcl_command *cmd)
{
    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOKEYW", "missing logical name");
        return SS$_BADPARAM;
    }

    /* Uppercase the logical name */
    char upper_name[256];
    size_t i;
    for (i = 0; i < sizeof(upper_name) - 1 && cmd->params[0][i]; i++)
        upper_name[i] = (char)toupper((unsigned char)cmd->params[0][i]);
    upper_name[i] = '\0';

    /* Determine target table from qualifiers (/PROCESS is the default) */
    const char *table = LNM_PROCESS_TABLE;
    int all_tables = dcl_has_qualifier(cmd, "ALL");
    if (!all_tables) {
        if (dcl_has_qualifier(cmd, "SYSTEM"))
            table = LNM_SYSTEM_TABLE;
        else if (dcl_has_qualifier(cmd, "GROUP"))
            table = LNM_GROUP_TABLE;
        else if (dcl_has_qualifier(cmd, "JOB"))
            table = LNM_JOB_TABLE;
    }

    lnm_manager_t *mgr = lnm_get_manager();
    if (mgr) {
        if (all_tables) {
            /* Try all tables; ignore "not found" errors */
            lnm_delete(mgr, LNM_PROCESS_TABLE, upper_name, LNM_MODE_USER);
            lnm_delete(mgr, LNM_JOB_TABLE,     upper_name, LNM_MODE_USER);
            lnm_delete(mgr, LNM_GROUP_TABLE,   upper_name, LNM_MODE_USER);
            lnm_delete(mgr, LNM_SYSTEM_TABLE,  upper_name, LNM_MODE_USER);
        } else {
            uint32_t status = lnm_delete(mgr, table, upper_name, LNM_MODE_USER);
            if (status == SS$_NOLOGNAM) {
                /* Not an error on VMS — deassigning a non-existent name is silent */
                return SS$_NORMAL;
            }
            if (status != SS$_NORMAL) {
                dcl_error("DCL", 1, "NOLOGNAM",
                          "no logical name match for \\%s\\", upper_name);
                return (int)status;
            }
        }
    } else {
        /* Graceful fallback: remove from global symbol table */
        dcl_sym_delete(upper_name, DCL_SYM_GLOBAL);
    }

    return SS$_NORMAL;
}

/* ================================================================== */
/*                     I/O Channel Operations                          */
/* ================================================================== */

/*
 * OPEN - Open a file for READ, WRITE, or APPEND.
 */

int cmd_open(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE",
                  "missing channel name and/or file specification");
        return SS$_BADPARAM;
    }

    const char *channel_name = cmd->params[0];
    const char *filespec = cmd->params[1];

    /* Determine mode */
    int mode = 0; /* default: read */
    if (dcl_has_qualifier(cmd, "WRITE")) mode = 1;
    else if (dcl_has_qualifier(cmd, "APPEND")) mode = 2;
    else if (dcl_has_qualifier(cmd, "READ")) mode = 0;

    /* Find a free channel slot */
    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (ctx->channels[i].fp == NULL) {
            slot = i;
            break;
        }
        if (strcasecmp(ctx->channels[i].name, channel_name) == 0) {
            /* Already open - close and reuse */
            fclose(ctx->channels[i].fp);
            ctx->channels[i].fp = NULL;
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        dcl_error("DCL", 2, "MAXCHAN", "maximum channels exceeded");
        return SS$_BADPARAM;
    }

    /* Resolve filespec */
    char linux_path[1024];
    dcl_resolve_path(ctx, filespec, linux_path, sizeof(linux_path));

    const char *fmode;
    switch (mode) {
        case 0: fmode = "r"; break;
        case 1: fmode = "w"; break;
        case 2: fmode = "a"; break;
        default: fmode = "r"; break;
    }

    FILE *fp = fopen(linux_path, fmode);
    if (!fp) {
        dcl_error("RMS", 2, "FNF",
                  "error opening %s", filespec);
        return SS$_NOSUCHFILE;
    }

    ctx->channels[slot].fp = fp;
    ctx->channels[slot].mode = mode;
    strncpy(ctx->channels[slot].name, channel_name,
            sizeof(ctx->channels[0].name) - 1);
    ctx->channels[slot].name[sizeof(ctx->channels[0].name) - 1] = '\0';
    /* Uppercase channel name */
    for (size_t i = 0; ctx->channels[slot].name[i]; i++) {
        ctx->channels[slot].name[i] =
            (char)toupper((unsigned char)ctx->channels[slot].name[i]);
    }

    return SS$_NORMAL;
}

/*
 * CLOSE - Close a file channel.
 */

int cmd_close(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOKEYW", "missing channel name");
        return SS$_BADPARAM;
    }

    for (int i = 0; i < 16; i++) {
        if (ctx->channels[i].fp &&
            strcasecmp(ctx->channels[i].name, cmd->params[0]) == 0) {
            fclose(ctx->channels[i].fp);
            ctx->channels[i].fp = NULL;
            ctx->channels[i].name[0] = '\0';
            return SS$_NORMAL;
        }
    }

    dcl_error("DCL", 2, "IVLOGNAM",
              "channel %s is not open", cmd->params[0]);
    return SS$_BADPARAM;
}

/*
 * READ - Read a line from a file channel into a symbol.
 */

int cmd_read(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOKEYW",
                  "missing channel name and/or symbol name");
        return SS$_BADPARAM;
    }

    const char *channel_name = cmd->params[0];
    const char *symbol_name = cmd->params[1];

    /* Find the channel */
    FILE *fp = NULL;
    for (int i = 0; i < 16; i++) {
        if (ctx->channels[i].fp &&
            strcasecmp(ctx->channels[i].name, channel_name) == 0) {
            fp = ctx->channels[i].fp;
            break;
        }
    }

    /* Check for /PROMPT qualifier (read from SYS$INPUT) */
    const char *prompt = dcl_qualifier_value(cmd, "PROMPT");
    if (prompt) {
        char buf[4096];
        if (prompt[0]) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if (!fgets(buf, sizeof(buf), stdin)) {
            return SS$_ENDOFFILE;
        }
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
        dcl_sym_set(symbol_name, buf, DCL_SYM_LOCAL);
        return SS$_NORMAL;
    }

    /*
     * SYS$INPUT: resolve through real logical translation (vms-f89) so a
     * DEFINE SYS$INPUT <file> redirects READ, mirroring cmd_write's SYS$OUTPUT
     * path. A terminal equivalence (the default) reads the process stdin; any
     * other equivalence is a file whose lines successive READs advance through.
     * SYS$INPUT is the process-permanent logical name for the default input
     * stream (VSI OpenVMS DCL Dictionary).
     */
    if (!fp && strcasecmp(channel_name, "SYS$INPUT") == 0) {
        char equiv[1024];
        int redirected = 0;
        char linux_path[1024];
        if (dcl_translate_logical("SYS$INPUT", equiv, sizeof(equiv)) == 0 &&
                equiv[0] && !dcl_logical_is_terminal(equiv)) {
            dcl_resolve_path(ctx, equiv, linux_path, sizeof(linux_path));
            redirected = 1;
        }

        if (redirected) {
            if (!g_sysinput_fp || strcmp(g_sysinput_key, linux_path) != 0) {
                if (g_sysinput_fp)
                    fclose(g_sysinput_fp);
                g_sysinput_fp = fopen(linux_path, "r");
                strncpy(g_sysinput_key, linux_path, sizeof(g_sysinput_key) - 1);
                g_sysinput_key[sizeof(g_sysinput_key) - 1] = '\0';
            }
            if (!g_sysinput_fp) {
                dcl_sym_set(symbol_name, "", DCL_SYM_LOCAL);
                return SS$_ENDOFFILE;
            }
            char rline[4096];
            if (!fgets(rline, sizeof(rline), g_sysinput_fp)) {
                dcl_sym_set(symbol_name, "", DCL_SYM_LOCAL);
                return SS$_ENDOFFILE;
            }
            size_t rl = strlen(rline);
            if (rl > 0 && rline[rl - 1] == '\n') rline[rl - 1] = '\0';
            dcl_sym_set(symbol_name, rline, DCL_SYM_LOCAL);
            return SS$_NORMAL;
        }

        /* Terminal SYS$INPUT: read one line from the process stdin. */
        char tbuf[4096];
        if (!fgets(tbuf, sizeof(tbuf), stdin)) {
            dcl_sym_set(symbol_name, "", DCL_SYM_LOCAL);
            return SS$_ENDOFFILE;
        }
        size_t tl = strlen(tbuf);
        if (tl > 0 && tbuf[tl - 1] == '\n') tbuf[tl - 1] = '\0';
        dcl_sym_set(symbol_name, tbuf, DCL_SYM_LOCAL);
        return SS$_NORMAL;
    }

    if (!fp) {
        dcl_error("DCL", 2, "IVLOGNAM",
                  "channel %s is not open", channel_name);
        return SS$_BADPARAM;
    }

    char line[4096];
    if (!fgets(line, sizeof(line), fp)) {
        dcl_sym_set(symbol_name, "", DCL_SYM_LOCAL);
        return SS$_ENDOFFILE;
    }

    /* Remove trailing newline */
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

    dcl_sym_set(symbol_name, line, DCL_SYM_LOCAL);
    return SS$_NORMAL;
}

/*
 * WRITE - Write text to a file channel.
 */

int cmd_write(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOKEYW",
                  "missing channel name and/or text");
        return SS$_BADPARAM;
    }

    const char *channel_name = cmd->params[0];

    /*
     * SYS$OUTPUT / SYS$ERROR: resolve through real logical-name translation
     * (vms-f89). Previously these were string-matched and ALWAYS went to the
     * process stdout/stderr, so DEFINE SYS$OUTPUT <file> did NOT redirect --
     * an INV-DCL facade (the string match named the subsystem's shape without
     * consulting the logical). Now the equivalence is translated at point of
     * use: a terminal equivalence (the "TT:" default) writes to the terminal,
     * any other equivalence is a file the output is appended to, so a
     * redefinition takes effect live. SYS$OUTPUT / SYS$ERROR are the
     * process-permanent logical names for the default output/error streams
     * (VSI OpenVMS DCL Dictionary; System Manager's Manual, logical names).
     */
    if (strcasecmp(channel_name, "SYS$OUTPUT") == 0 ||
        strcasecmp(channel_name, "SYS$ERROR") == 0) {
        int is_err = (strcasecmp(channel_name, "SYS$ERROR") == 0);

        char equiv[1024];
        if (dcl_translate_logical(channel_name, equiv, sizeof(equiv)) == 0 &&
                equiv[0] && !dcl_logical_is_terminal(equiv)) {
            char linux_path[1024];
            dcl_resolve_path(ctx, equiv, linux_path, sizeof(linux_path));
            FILE *fp = fopen(linux_path, "a");
            if (!fp) {
                dcl_error("RMS", 2, "OPENOUT",
                          "error opening %s as output", equiv);
                return SS$_NOSUCHFILE;
            }
            for (int i = 1; i < cmd->param_count; i++)
                fprintf(fp, "%s", cmd->params[i]);
            fprintf(fp, "\n");
            fclose(fp);
            return SS$_NORMAL;
        }

        FILE *out = is_err ? stderr : stdout;
        for (int i = 1; i < cmd->param_count; i++)
            fprintf(out, "%s", cmd->params[i]);
        fprintf(out, "\n");
        return SS$_NORMAL;
    }

    /* Find the channel */
    FILE *fp = NULL;
    for (int i = 0; i < 16; i++) {
        if (ctx->channels[i].fp &&
            strcasecmp(ctx->channels[i].name, channel_name) == 0) {
            fp = ctx->channels[i].fp;
            break;
        }
    }

    if (!fp) {
        dcl_error("DCL", 2, "IVLOGNAM",
                  "channel %s is not open", channel_name);
        return SS$_BADPARAM;
    }

    /* Write all remaining params joined by spaces (but actually as-is) */
    for (int i = 1; i < cmd->param_count; i++) {
        if (i > 1) fprintf(fp, " ");
        fprintf(fp, "%s", cmd->params[i]);
    }
    fprintf(fp, "\n");
    fflush(fp);

    return SS$_NORMAL;
}

