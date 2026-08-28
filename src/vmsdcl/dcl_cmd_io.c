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
#include "dcl/dcl_rms.h"   /* vms-5f0: OPEN/READ/WRITE/CLOSE ride RMS (ACP), not fopen */
#include "ssdef.h"
#include "rmsdef.h"
#include "vms/logical.h"
#include "vmsfs/filespec.h"

/* vms-5f0: a channel slot is free when it holds no stream and no RMS handle. */
static int dcl_chan_free(const struct dcl_context *ctx, int i)
{
    return ctx->channels[i].fp == NULL &&
           ctx->channels[i].reader == NULL &&
           ctx->channels[i].writer == NULL;
}

/* vms-5f0: tear down whatever a channel slot holds (stdio stream OR RMS
 * reader/writer) and clear it. */
static void dcl_chan_release(struct dcl_context *ctx, int i)
{
    if (ctx->channels[i].fp)     { fclose(ctx->channels[i].fp); ctx->channels[i].fp = NULL; }
    if (ctx->channels[i].reader) { dcl_rms_read_close(ctx->channels[i].reader); ctx->channels[i].reader = NULL; }
    if (ctx->channels[i].writer) { (void)dcl_rms_write_close(ctx->channels[i].writer); ctx->channels[i].writer = NULL; }
    ctx->channels[i].name[0] = '\0';
}

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

    /* Find a free channel slot (or reuse one already bound to this name). */
    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (!dcl_chan_free(ctx, i) &&
            strcasecmp(ctx->channels[i].name, channel_name) == 0) {
            /* Already open - close and reuse */
            dcl_chan_release(ctx, i);
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < 16; i++) {
            if (dcl_chan_free(ctx, i)) { slot = i; break; }
        }
    }

    if (slot < 0) {
        dcl_error("DCL", 2, "MAXCHAN", "maximum channels exceeded");
        return SS$_BADPARAM;
    }

    const char *fmode;
    switch (mode) {
        case 0: fmode = "r"; break;
        case 1: fmode = "w"; break;
        case 2: fmode = "a"; break;
        default: fmode = "r"; break;
    }

    /* SYS$OUTPUT:/SYS$ERROR:/SYS$INPUT: name the process's standard streams,
     * not RMS files -- OPEN connects a channel to the current output/error/input
     * (VSI OpenVMS DCL Dictionary, OPEN). MMK's subprocess protocol OPENs
     * SYS$OUTPUT: to write its end-of-command $STATUS marker there. dup() the
     * underlying fd so a later CLOSE of the channel frees only this channel's
     * handle, never the process's real standard stream. */
    {
        char norm[64];
        size_t nk = 0;
        for (const char *s = filespec; *s && nk < sizeof(norm) - 1; s++)
            norm[nk++] = (char)toupper((unsigned char)*s);
        norm[nk] = '\0';
        if (nk > 0 && norm[nk - 1] == ':') norm[--nk] = '\0';

        int stdfd = -1;
        if      (strcmp(norm, "SYS$OUTPUT") == 0) stdfd = STDOUT_FILENO;
        else if (strcmp(norm, "SYS$ERROR")  == 0) stdfd = STDERR_FILENO;
        else if (strcmp(norm, "SYS$INPUT")  == 0) stdfd = STDIN_FILENO;

        if (stdfd >= 0) {
            int dfd = dup(stdfd);
            FILE *sfp = (dfd >= 0) ? fdopen(dfd, fmode) : NULL;
            if (!sfp) {
                if (dfd >= 0) close(dfd);
                dcl_error("RMS", 2, "FNF", "error opening %s", filespec);
                return SS$_NOSUCHFILE;
            }
            ctx->channels[slot].fp = sfp;
            ctx->channels[slot].mode = mode;
            strncpy(ctx->channels[slot].name, channel_name,
                    sizeof(ctx->channels[0].name) - 1);
            ctx->channels[slot].name[sizeof(ctx->channels[0].name) - 1] = '\0';
            for (size_t i = 0; ctx->channels[slot].name[i]; i++)
                ctx->channels[slot].name[i] =
                    (char)toupper((unsigned char)ctx->channels[slot].name[i]);
            return SS$_NORMAL;
        }
    }

    /*
     * A REAL file: reach it through RMS so it rides the Files-11 ODS-2 ACP
     * (vms-5f0, epic vms-208). fopen() on a vmsfs_to_linux_path passthrough is
     * gone -- it cannot see files that live only on the genuine ODS-2 SYS$DISK
     * (SYS$STARTUP:VMS$PHASES.DAT et al). The dcl_rms_* helpers translate the
     * device/directory logicals (SYS$STARTUP:, SYS$SYSTEM:) the VMS way and
     * fail-honest with the real RMS status -- no silent local fallback (INV-6).
     */
    uint32_t rms_st = RMS$_NORMAL;
    if (mode == 0) {
        struct dcl_rms_reader *r = dcl_rms_read_open(ctx, filespec, &rms_st);
        if (!r) {
            dcl_error("RMS", 2, "FNF", "error opening %s", filespec);
            return SS$_NOSUCHFILE;
        }
        ctx->channels[slot].reader = r;
    } else {
        /* WRITE / APPEND create the file through RMS ($CREATE writes a new
         * version onto the ODS-2 volume): variable-length carriage-return
         * records, the text-file shape DCL WRITE appends lines to. */
        struct dcl_rms_writer *w =
            dcl_rms_write_create(ctx, filespec, FAB$C_VAR, FAB$M_CR, 0, &rms_st);
        if (!w) {
            dcl_error("RMS", 2, "FNF", "error opening %s", filespec);
            return SS$_NOSUCHFILE;
        }
        ctx->channels[slot].writer = w;
    }
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
        if (!dcl_chan_free(ctx, i) &&
            strcasecmp(ctx->channels[i].name, cmd->params[0]) == 0) {
            dcl_chan_release(ctx, i);
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

    /* Find the channel (stdio stream OR RMS reader). */
    int ch = -1;
    for (int i = 0; i < 16; i++) {
        if (!dcl_chan_free(ctx, i) &&
            strcasecmp(ctx->channels[i].name, channel_name) == 0) {
            ch = i;
            break;
        }
    }
    FILE *fp = (ch >= 0) ? ctx->channels[ch].fp : NULL;

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

    /* An RMS read channel (vms-5f0): one $GET returns one record off the ODS-2
     * volume through the ACP. EOF sets the symbol empty and returns ENDOFFILE,
     * matching the stdio path below (and DCL's own no-END_OF_FILE= behaviour). */
    if (ch >= 0 && ctx->channels[ch].reader) {
        char rec[4096];
        int eof = 0;
        int n = dcl_rms_read_record(ctx->channels[ch].reader, rec, sizeof(rec), &eof);
        if (n < 0) {
            dcl_sym_set(symbol_name, "", DCL_SYM_LOCAL);
            return SS$_ENDOFFILE;
        }
        dcl_sym_set(symbol_name, rec, DCL_SYM_LOCAL);
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
 * Evaluate a WRITE argument list into a single output string.
 *
 * VMS DCL WRITE takes an expression (or a comma-separated list of expressions)
 * whose VALUE is written, e.g.
 *   WRITE SYS$OUTPUT F$GETSYI("VERSION")
 *   WRITE SYS$OUTPUT "ver is ''VER'"
 *   WRITE SYS$OUTPUT "a", "b", F$TIME()
 * Each comma-separated expression is evaluated through the SAME evaluator the
 * `=` assignment right-hand side uses (dcl_eval_expr_string), and the resulting
 * values are concatenated (VMS treats both `,` and `+` as concatenation in the
 * WRITE argument list). String literals stay literal, F$xxx(...) lexical
 * functions evaluate, and symbol / apostrophe substitution resolves. (vms-65f)
 *
 * `arglist` is the raw argument text AFTER the channel name (from raw_tail).
 * Top-level commas (outside quotes and parentheses) separate the expressions.
 */
static void write_eval_arglist(struct dcl_context *ctx, const char *arglist,
                               char *out, size_t outlen)
{
    out[0] = '\0';
    size_t used = 0;

    const char *p = arglist;
    while (*p == ' ' || *p == '\t') p++;

    while (*p) {
        /* Find the end of this comma-separated segment, honoring quotes and
         * parenthesis nesting so a comma inside F$xxx(a,b) or a quoted string
         * does not split the list. */
        const char *seg_start = p;
        int depth = 0;
        int in_quote = 0;
        while (*p) {
            char ch = *p;
            if (in_quote) {
                if (ch == '"') {
                    if (*(p + 1) == '"') { p++; }  /* doubled quote */
                    else in_quote = 0;
                }
            } else if (ch == '"') {
                in_quote = 1;
            } else if (ch == '(') {
                depth++;
            } else if (ch == ')') {
                if (depth > 0) depth--;
            } else if (ch == ',' && depth == 0) {
                break;
            }
            p++;
        }

        /* Copy the raw segment out and trim surrounding whitespace. */
        size_t seg_len = (size_t)(p - seg_start);
        char seg[DCL_MAX_VALUE];
        if (seg_len >= sizeof(seg)) seg_len = sizeof(seg) - 1;
        memcpy(seg, seg_start, seg_len);
        seg[seg_len] = '\0';
        char *s = seg;
        while (*s == ' ' || *s == '\t') s++;
        size_t sl = strlen(s);
        while (sl > 0 && (s[sl - 1] == ' ' || s[sl - 1] == '\t'))
            s[--sl] = '\0';

        /* Evaluate this expression and append its value. */
        char val[DCL_MAX_VALUE];
        val[0] = '\0';
        if (*s)
            dcl_eval_expr_string(ctx, s, val, sizeof(val));

        size_t vl = strlen(val);
        if (used + vl >= outlen) vl = (outlen > used) ? (outlen - used - 1) : 0;
        memcpy(out + used, val, vl);
        used += vl;
        out[used] = '\0';

        if (*p == ',') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
        }
    }
}

/*
 * Extract the WRITE argument text (everything after the channel name) from the
 * raw command tail. raw_tail is e.g. `SYS$OUTPUT F$GETSYI("VERSION")`; this
 * returns a pointer to the text after the first whitespace-delimited token.
 * Returns NULL if there is no argument text after the channel.
 */
static const char *write_arg_tail(const char *raw_tail)
{
    if (!raw_tail) return NULL;
    const char *p = raw_tail;
    while (*p == ' ' || *p == '\t') p++;
    /* skip the channel token */
    while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return (*p) ? p : NULL;
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

    /* WRITE's argument list is an expression (or comma-separated list of
     * expressions) whose VALUE is written. Evaluate it ONCE, up front, through
     * the SAME evaluator the `=` assignment RHS uses (write_eval_arglist ->
     * dcl_eval_expr_string), so lexical functions F$xxx(...), symbol/apostrophe
     * substitution, quoted string literals and +/- string ops all resolve
     * exactly as they do elsewhere in DCL. Comma-separated items are
     * concatenated with no separator (VSI OpenVMS DCL Dictionary, WRITE). This
     * fixes WRITE emitting the LITERAL "F$GETSYIVERSION" instead of evaluating
     * F$GETSYI("VERSION") (vms-65f): the old path pushed the tokenized params[]
     * out verbatim and never routed them through the evaluator. */
    char text[DCL_MAX_VALUE];
    const char *arglist = write_arg_tail(cmd->raw_tail);
    if (arglist) {
        write_eval_arglist(ctx, arglist, text, sizeof(text));
    } else {
        /* No raw tail available (e.g. a synthesized command that only set
         * params[]): fall back to the tokenized params, resolving a bare word
         * that names a defined symbol to its value (MMK marker idiom). */
        text[0] = '\0';
        size_t used = 0;
        for (int i = 1; i < cmd->param_count; i++) {
            const char *sv = dcl_sym_get(cmd->params[i]);
            const char *s = sv ? sv : cmd->params[i];
            size_t l = strlen(s);
            if (used + l >= sizeof(text)) l = sizeof(text) - used - 1;
            memcpy(text + used, s, l);
            used += l;
            text[used] = '\0';
        }
    }

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
            fprintf(fp, "%s\n", text);
            fclose(fp);
            return SS$_NORMAL;
        }

        FILE *out = is_err ? stderr : stdout;
        fprintf(out, "%s\n", text);
        return SS$_NORMAL;
    }

    /* Find the channel (stdio stream OR RMS writer). */
    int ch = -1;
    for (int i = 0; i < 16; i++) {
        if (!dcl_chan_free(ctx, i) &&
            strcasecmp(ctx->channels[i].name, channel_name) == 0) {
            ch = i;
            break;
        }
    }

    if (ch < 0) {
        dcl_error("DCL", 2, "IVLOGNAM",
                  "channel %s is not open", channel_name);
        return SS$_BADPARAM;
    }

    /* An RMS write channel (vms-5f0): assemble the record and $PUT it onto the
     * ODS-2 volume through the ACP. RMS records carry no line terminator (the CR
     * record attribute supplies it), so no trailing '\n' here. */
    if (ctx->channels[ch].writer) {
        if (dcl_rms_write_record(ctx->channels[ch].writer, text,
                                 strlen(text)) != 0) {
            dcl_error("RMS", 2, "WER", "error writing to channel %s", channel_name);
            return SS$_ABORT;
        }
        return SS$_NORMAL;
    }

    FILE *fp = ctx->channels[ch].fp;
    if (!fp) {
        dcl_error("DCL", 2, "IVLOGNAM",
                  "channel %s is not open", channel_name);
        return SS$_BADPARAM;
    }
    fprintf(fp, "%s\n", text);
    fflush(fp);

    return SS$_NORMAL;
}

