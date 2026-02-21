/*
 * dcl_script.c - DCL Command Procedure (.COM) Execution
 *
 * Handles execution of DCL command procedures (scripts), including
 * parameter passing (P1-P8), ON ERROR handling, GOTO/GOSUB/RETURN,
 * and nested procedure invocation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "ssdef.h"

/* External functions */
extern int dcl_execute_line(const char *line);
extern int dcl_resolve_path(struct dcl_context *ctx, const char *spec,
                            char *linux_path, size_t path_size);
extern void dcl_error(const char *facility, int severity, const char *ident,
                      const char *fmt, ...);

/*
 * Search a procedure file for a label.
 * Positions the file pointer to the line after the label.
 * Returns 0 on success, -1 if label not found.
 */
int dcl_find_label(FILE *fp, const char *label)
{
    if (!fp || !label) return -1;

    /* Rewind to beginning */
    rewind(fp);

    char line[DCL_MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        /* Remove leading whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        /* Skip $ prefix */
        if (*p == '$') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
        }

        /* Check for LABEL: */
        char *colon = strchr(p, ':');
        if (colon) {
            /* Check that everything before colon is a valid label name */
            char lbl[256];
            size_t llen = (size_t)(colon - p);
            if (llen > 0 && llen < sizeof(lbl)) {
                memcpy(lbl, p, llen);
                lbl[llen] = '\0';

                /* Trim trailing whitespace from label */
                while (llen > 0 && (lbl[llen - 1] == ' ' || lbl[llen - 1] == '\t'))
                    lbl[--llen] = '\0';

                /* Check that label is valid (alphanumeric and _$) */
                int valid = 1;
                for (size_t i = 0; i < llen; i++) {
                    if (!isalnum((unsigned char)lbl[i]) && lbl[i] != '_' && lbl[i] != '$') {
                        valid = 0;
                        break;
                    }
                }

                if (valid && strcasecmp(lbl, label) == 0) {
                    return 0; /* Found - file pointer is now after this line */
                }
            }
        }
    }

    return -1; /* Not found */
}

/*
 * Execute a command procedure (.COM file).
 *
 * Parameters p1..p8 are passed via argc/argv.
 * Returns VMS status code.
 */
int dcl_execute_script(const char *filename, int argc, char **argv)
{
    struct dcl_context *ctx = dcl_get_context();

    if (!filename) return SS$_BADPARAM;

    /* Resolve the filespec */
    char linux_path[1024];
    char spec[1024];

    /* Trim whitespace from filename */
    while (*filename == ' ' || *filename == '\t') filename++;
    strncpy(spec, filename, sizeof(spec) - 1);
    spec[sizeof(spec) - 1] = '\0';
    size_t slen = strlen(spec);
    while (slen > 0 && (spec[slen - 1] == ' ' || spec[slen - 1] == '\t'))
        spec[--slen] = '\0';

    dcl_resolve_path(ctx, spec, linux_path, sizeof(linux_path));

    /* Try to open the file */
    FILE *fp = fopen(linux_path, "r");

    /* If not found, try adding .com extension */
    if (!fp) {
        char with_ext[1024];
        snprintf(with_ext, sizeof(with_ext), "%s.com", linux_path);
        fp = fopen(with_ext, "r");
        if (!fp) {
            /* Also try uppercase .COM */
            snprintf(with_ext, sizeof(with_ext), "%s.COM", linux_path);
            fp = fopen(with_ext, "r");
        }
        if (fp) {
            strncpy(linux_path, with_ext, sizeof(linux_path) - 1);
        }
    }

    if (!fp) {
        dcl_error("DCL", 2, "OPENIN",
                  "error opening %s as input", spec);
        return SS$_NOSUCHFILE;
    }

    /* Push procedure onto stack */
    if (ctx->proc_depth >= DCL_MAX_NEST - 1) {
        dcl_error("DCL", 4, "NESTLEV",
                  "maximum procedure nesting level exceeded");
        fclose(fp);
        return SS$_BADPARAM;
    }

    ctx->proc_depth++;
    memset(&ctx->proc_stack[ctx->proc_depth], 0,
           sizeof(ctx->proc_stack[0]));
    ctx->proc_stack[ctx->proc_depth].fp = fp;
    strncpy(ctx->proc_stack[ctx->proc_depth].filename, linux_path,
            sizeof(ctx->proc_stack[0].filename) - 1);
    ctx->proc_stack[ctx->proc_depth].line_number = 0;

    /* Set P1-P8 parameters as local symbols */
    for (int i = 0; i < 8; i++) {
        char pname[4];
        snprintf(pname, sizeof(pname), "P%d", i + 1);
        if (argv && i < argc && argv[i]) {
            dcl_sym_set(pname, argv[i], DCL_SYM_LOCAL);
            strncpy(ctx->proc_stack[ctx->proc_depth].params[i],
                    argv[i], sizeof(ctx->proc_stack[0].params[0]) - 1);
        } else {
            dcl_sym_set(pname, "", DCL_SYM_LOCAL);
        }
    }

    /* Execute lines */
    char line[DCL_MAX_LINE];
    int status = SS$_NORMAL;

    while (fgets(line, sizeof(line), fp)) {
        ctx->proc_stack[ctx->proc_depth].line_number++;

        /* Remove trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        /* Verify mode - echo the line */
        if (ctx->verify) {
            printf("%s\n", line);
        }

        /* Skip leading whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        /* Strip leading $ */
        if (*p == '$') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
        }

        /* Skip empty and comment lines */
        if (*p == '\0' || *p == '!') continue;
        if (*p == '$' && *(p + 1) == '!') continue;

        /* Handle line continuation */
        char full_line[DCL_MAX_LINE];
        strncpy(full_line, p, sizeof(full_line) - 1);
        full_line[sizeof(full_line) - 1] = '\0';

        len = strlen(full_line);
        while (len > 0 && full_line[len - 1] == '-') {
            full_line[len - 1] = '\0';
            if (!fgets(line, sizeof(line), fp)) break;
            ctx->proc_stack[ctx->proc_depth].line_number++;
            len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

            p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '$') { p++; while (*p == ' ' || *p == '\t') p++; }

            strncat(full_line, p, sizeof(full_line) - strlen(full_line) - 1);
            len = strlen(full_line);
        }

        /* Perform symbol substitution */
        char substituted[DCL_MAX_LINE];
        dcl_sym_substitute(full_line, substituted, sizeof(substituted));

        /* Execute the line */
        status = dcl_execute_line(substituted);

        /* Check for EXIT or error */
        if (ctx->exit_requested) {
            /* EXIT was executed in the procedure */
            ctx->exit_requested = 0; /* Reset for the caller */
            break;
        }

        /* Check ON ERROR handling */
        if (!(status & 1)) {
            /* Error occurred */
            int severity = status & 7;
            int handle_error = 0;

            /* SET NOON suppresses the ON ERROR handler */
            if (!ctx->noon_active) {
                if (severity >= 4 && ctx->proc_stack[ctx->proc_depth].on_severe) {
                    handle_error = ctx->proc_stack[ctx->proc_depth].on_severe;
                } else if (severity >= 2 && ctx->proc_stack[ctx->proc_depth].on_error) {
                    handle_error = ctx->proc_stack[ctx->proc_depth].on_error;
                }
            }

            if (handle_error == 1) {
                /* ON ERROR THEN CONTINUE - just continue */
                continue;
            } else if (handle_error == 2) {
                /* ON ERROR THEN GOTO label */
                const char *label = (severity >= 4) ?
                    ctx->proc_stack[ctx->proc_depth].on_severe_label :
                    ctx->proc_stack[ctx->proc_depth].on_error_label;
                if (label[0]) {
                    if (dcl_find_label(fp, label) == 0) {
                        continue; /* Jump to label */
                    } else {
                        dcl_error("DCL", 2, "USGOTO",
                                  "target of GOTO not found - \\%s\\", label);
                        break;
                    }
                }
            } else if (handle_error == 0) {
                /* No handler - stop procedure on error */
                /* Only stop on error/severe, not warning */
                if (severity >= 2) {
                    break;
                }
            }
        }
    }

    /* Pop procedure from stack */
    fclose(ctx->proc_stack[ctx->proc_depth].fp);
    ctx->proc_stack[ctx->proc_depth].fp = NULL;
    ctx->proc_depth--;

    /* Restore P1-P8 from parent scope (or clear them) */
    for (int i = 0; i < 8; i++) {
        char pname[4];
        snprintf(pname, sizeof(pname), "P%d", i + 1);
        if (ctx->proc_depth >= 0) {
            dcl_sym_set(pname, ctx->proc_stack[ctx->proc_depth].params[i],
                       DCL_SYM_LOCAL);
        } else {
            dcl_sym_set(pname, "", DCL_SYM_LOCAL);
        }
    }

    return status;
}
