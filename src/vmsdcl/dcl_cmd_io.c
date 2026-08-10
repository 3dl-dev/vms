/*
 * dcl_cmd_io.c - DCL device I/O command implementations
 *
 * ASSIGN, DEASSIGN, DEFINE, OPEN, CLOSE, READ, WRITE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
 * ASSIGN - Assign a logical name.
 * Format: ASSIGN equivalence-name logical-name [/TABLE=table-name]
 * Unlike DEFINE, ASSIGN uses a different default table (LNM$PROCESS) and
 * has slightly different semantics for table placement.
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

    /* Uppercase the logical name (VMS convention) */
    char upper_name[256];
    size_t i;
    for (i = 0; i < sizeof(upper_name) - 1 && logname[i]; i++)
        upper_name[i] = (char)toupper((unsigned char)logname[i]);
    upper_name[i] = '\0';

    /* /TABLE qualifier selects target table — stub: we have one table */
    /* /PROCESS (default), /JOB, /GROUP, /SYSTEM — all map to global for now */
    int scope = DCL_SYM_GLOBAL;
    if (dcl_has_qualifier(cmd, "PROCESS")) scope = DCL_SYM_GLOBAL;

    dcl_sym_set(upper_name, equiv, scope);

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
                                           num_equiv, LNM_ATTR_TERMINAL,
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

    /* Check for SYS$OUTPUT special channel */
    if (strcasecmp(channel_name, "SYS$OUTPUT") == 0) {
        for (int i = 1; i < cmd->param_count; i++) {
            printf("%s", cmd->params[i]);
        }
        printf("\n");
        return SS$_NORMAL;
    }

    if (strcasecmp(channel_name, "SYS$ERROR") == 0) {
        for (int i = 1; i < cmd->param_count; i++) {
            fprintf(stderr, "%s", cmd->params[i]);
        }
        fprintf(stderr, "\n");
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

