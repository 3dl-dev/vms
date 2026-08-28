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
extern void dcl_set_status(struct dcl_context *ctx, int status);

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
 * Run command lines from the current procedure level's file stream until EOF,
 * an unhandled error stops the procedure, EXIT is requested, or (in a CALLed
 * subroutine) RETURN/ENDSUBROUTINE sets return_requested. Shared verbatim by
 * @-procedure execution and CALL subroutine execution so both honour the same
 * continuation, symbol-substitution, verify and ON-ERROR semantics.
 *
 * The caller has already pushed the proc_stack level (with .fp set), pushed the
 * local symbol frame, and set gosub_base. Returns the status of the last line.
 */
static int dcl_run_proc_lines(struct dcl_context *ctx, FILE *fp)
{
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

        /* RETURN / ENDSUBROUTINE inside a CALLed subroutine: stop this level. */
        if (ctx->return_requested)
            break;

        /* Check for EXIT or error */
        if (ctx->exit_requested) {
            /* EXIT was executed in the procedure */
            ctx->exit_requested = 0; /* Reset for the caller */
            break;
        }

        /* DCL error control, applied after every command at this level using
         * the REAL $STATUS of the line just run (vms-3983). Reference
         * (clean-room, Rule 8): VSI OpenVMS DCL Dictionary, "ON", "SET ON"/"SET
         * NOON"; OpenVMS User's Manual, "Controlling Error Conditions". State is
         * per command level (vms-2af frames): proc_stack[proc_depth]. */
        {
            int lvl = ctx->proc_depth;

            /* SET NOON: DCL does not test $STATUS at all -- no default exit and
             * no ON action -- so execution continues past any error. This is the
             * fix for the flagged bug where SET NOON suppressed only the ON
             * handler but not the default in-procedure error-stop (vms-ada). */
            if (lvl >= 0 && ctx->proc_stack[lvl].noon)
                continue;

            if (!(status & 1)) {
                /* Failure: severity is even -- 0=WARNING, 2=ERROR, 4=SEVERE. */
                int severity = status & 7;

                if (lvl >= 0 && ctx->proc_stack[lvl].on_armed &&
                    severity >= ctx->proc_stack[lvl].on_severity) {
                    /* Armed ON action fires. ON ERROR THEN CONTINUE just resumes
                     * and REMAINS in effect (the "ignore errors in this block"
                     * idiom). Any other action -- GOTO a handler, EXIT, or an
                     * arbitrary command -- is ONE-SHOT: DCL resets the level to
                     * its default (exit on error) before performing it, so a
                     * GOTO handler cannot infinitely re-enter itself; the
                     * handler re-arms by issuing another ON. (VSI OpenVMS DCL
                     * Dictionary, "ON".) */
                    char action[256];
                    strncpy(action, ctx->proc_stack[lvl].on_action,
                            sizeof(action) - 1);
                    action[sizeof(action) - 1] = '\0';

                    if (strcasecmp(action, "CONTINUE") == 0 || action[0] == '\0')
                        continue;   /* persists; keep on_armed set */

                    ctx->proc_stack[lvl].on_armed = 0;   /* one-shot reset */
                    ctx->proc_stack[lvl].on_action[0] = '\0';

                    /* Run the THEN command through the normal command path:
                     * GOTO reseeks fp, EXIT sets exit_requested, any other
                     * command just runs and execution then continues. */
                    dcl_execute_line(action);
                    if (ctx->return_requested)
                        break;
                    if (ctx->exit_requested) {
                        ctx->exit_requested = 0;
                        break;
                    }
                    continue;
                }

                /* Default action (no ON armed): exit the procedure on ERROR (2)
                 * or SEVERE/FATAL (4); a WARNING (0) does not stop. */
                if (severity >= 2)
                    break;
            }
        }
    }

    return status;
}

/*
 * CALL label [p1 ... p8] -- invoke an in-procedure subroutine.
 *
 * VMS semantics (DCL Dictionary, "CALL"/"SUBROUTINE"/"RETURN"): CALL transfers
 * to a label whose block is bracketed by SUBROUTINE...ENDSUBROUTINE, and gives
 * the subroutine a NEW command level -- its own P1-P8 and its own local symbol
 * frame (per the vms-2af per-level scoping) -- inside the SAME procedure file.
 * RETURN [status] (or ENDSUBROUTINE) returns to the line after CALL and sets
 * $STATUS. The subroutine reads the same file stream; the caller's read
 * position is saved and restored so execution resumes right after the CALL.
 *
 * Returns the subroutine's completion status.
 */
int dcl_call_subroutine(const char *label, int argc, char **argv)
{
    struct dcl_context *ctx = dcl_get_context();

    if (ctx->proc_depth < 0) {
        dcl_error("DCL", 2, "NOINTERACT",
                  "CALL not allowed in interactive mode");
        return SS$_BADPARAM;
    }
    if (ctx->proc_depth >= DCL_MAX_NEST - 1) {
        dcl_error("DCL", 4, "NESTLEV",
                  "maximum procedure nesting level exceeded");
        return SS$_BADPARAM;
    }

    FILE *fp = ctx->proc_stack[ctx->proc_depth].fp;
    if (!fp) return SS$_BADPARAM;

    /* Save the caller's read position so we resume after the CALL line. */
    long resume_off = ftell(fp);
    int  resume_line = ctx->proc_stack[ctx->proc_depth].line_number;

    /* Position at the subroutine's label (fp now points just after it). */
    if (dcl_find_label(fp, label) != 0) {
        fseek(fp, resume_off, SEEK_SET);
        dcl_error("DCL", 2, "USGOTO",
                  "target of CALL not found - \\%s\\", label);
        return SS$_BADPARAM;
    }

    /* Push a new command level that shares the file stream. */
    ctx->proc_depth++;
    struct dcl_context *c = ctx;
    memset(&c->proc_stack[c->proc_depth], 0, sizeof(c->proc_stack[0]));
    c->proc_stack[c->proc_depth].fp = fp;
    strncpy(c->proc_stack[c->proc_depth].filename,
            c->proc_stack[c->proc_depth - 1].filename,
            sizeof(c->proc_stack[0].filename) - 1);
    c->proc_stack[c->proc_depth].is_subroutine = 1;
    c->proc_stack[c->proc_depth].gosub_base = c->gosub_depth;
    c->proc_stack[c->proc_depth].line_number = 0;

    /* Fresh local symbol frame + P1-P8 for the subroutine. */
    dcl_sym_push_frame();
    for (int i = 0; i < 8; i++) {
        char pname[4];
        snprintf(pname, sizeof(pname), "P%d", i + 1);
        if (argv && i < argc && argv[i])
            dcl_sym_set(pname, argv[i], DCL_SYM_LOCAL);
        else
            dcl_sym_set(pname, "", DCL_SYM_LOCAL);
    }

    int status = dcl_run_proc_lines(ctx, fp);

    /* If the loop stopped on RETURN [status], adopt that status. */
    if (ctx->return_requested) {
        status = ctx->return_status;
        ctx->return_requested = 0;
    }

    /* Pop the subroutine's local frame and command level. */
    dcl_sym_pop_frame();
    ctx->gosub_depth = c->proc_stack[c->proc_depth].gosub_base;
    ctx->proc_depth--;

    /* Resume the caller right after the CALL line. */
    fseek(fp, resume_off, SEEK_SET);
    ctx->proc_stack[ctx->proc_depth].line_number = resume_line;

    /* CALL sets $STATUS to the subroutine's completion status. */
    dcl_set_status(ctx, status);
    return status;
}

#if defined(OVMX_HAVE_ACP)
#include "dcl/dcl_rms.h"     /* dcl_rms_read_open/read_record/read_close (ACP) */

/*
 * dcl_proc_open_acp (vms-5f0, epic vms-208 atomic flip) - open a command
 * procedure off the GENUINE ODS-2 SYS$DISK through RMS / the Files-11 ACP,
 * staging its text in a transient stdio stream the existing fseek/fgets script
 * engine (dcl_execute_script / dcl_call_subroutine) drives unchanged.
 *
 * KEYED ON OVMX_HAVE_ACP, NOT __linux__ (vms-329). This was Linux-only while
 * the netbsd-vax runtime still VFS-mounted SYS$DISK and so still had a /vms
 * POSIX tree to fopen(). The coupled VAX cutover retired that mount, so on the
 * VAX the fopen chain below reaches nothing at all: the first traced cutover
 * boot got PID 1 all the way to "handing SYS$MANAGER:STARTUP.COM to DCL for ACP
 * resolution" and DCL answered %DCL-E-OPENIN, because this arm was compiled out
 * and the passthrough it fell back to no longer exists. The implementation was
 * already substrate-neutral -- dcl_rms_read_open() lives in dcl_filespec.c,
 * which every configuration compiles -- so the gate, not the code, was the gap.
 * (It also removes a __linux__ source fork, which build-boot-images-vax.sh's
 * INV-DRIFT note forbids in this image set.)
 *
 * WHY: a boot-time procedure (SYS$STARTUP:JOB_CONTROL_STARTUP.COM,
 * SYS$MANAGER:*.COM, ...) lives ONLY on the mounted ODS-2 volume; fopen() on
 * the vmsfs_to_linux_path("/vms/...") passthrough cannot see it, so @-execution
 * of it failed %DCL-E-OPENIN and the END-phase JOB_CONTROL/LOGINOUT never
 * started. DCL's other file verbs already ride RMS-over-ACP (vms-481/vms-5f0);
 * this brings @-procedure execution onto the same path.
 *
 * A STMLF/VAR text file's records ARE its lines, so joining them with '\n'
 * reconstructs the procedure text the engine expects. Tries `spec` then the
 * `.COM` default type. Returns NULL when the ACP cannot resolve the spec (a
 * build with no /dev/vms, or a genuinely absent file), so the caller falls back
 * to the passthrough fopen chain -- keeping the plain host ctest environment
 * (no ACP-mounted SYS$DISK) working exactly as before.
 */
static FILE *dcl_proc_open_acp(struct dcl_context *ctx, const char *spec)
{
    static const char *exts[] = { "", ".COM" };
    for (unsigned e = 0; e < 2; e++) {
        char trial[1056];
        uint32_t rst = 0;
        struct dcl_rms_reader *r;

        snprintf(trial, sizeof(trial), "%s%s", spec, exts[e]);
        r = dcl_rms_read_open(ctx, trial, &rst);
        if (!r)
            continue;                       /* try .COM, then passthrough */

        FILE *fp = tmpfile();
        if (!fp) { dcl_rms_read_close(r); return NULL; }

        char rec[8192];
        int eof = 0;
        for (;;) {
            int n = dcl_rms_read_record(r, rec, sizeof(rec), &eof);
            if (n < 0)
                break;                      /* end-of-file / read error */
            if (n > 0)
                fwrite(rec, 1, (size_t)n, fp);
            fputc('\n', fp);
        }
        dcl_rms_read_close(r);
        rewind(fp);
        return fp;
    }
    return NULL;
}
#endif /* OVMX_HAVE_ACP */

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

    /* Try to open the file. vms-5f0: FIRST reach it the VMS way -- through RMS /
     * the Files-11 ACP on the mounted ODS-2 SYS$DISK -- so a procedure that
     * lives only on the volume (JOB_CONTROL_STARTUP.COM et al) is found. Fall
     * back to the /vms passthrough fopen chain below only when the ACP has no
     * such device/file (host builds without /dev/vms), preserving those tests. */
    FILE *fp = NULL;
#if defined(OVMX_HAVE_ACP)
    fp = dcl_proc_open_acp(ctx, spec);
    if (fp) {
        strncpy(linux_path, spec, sizeof(linux_path) - 1);
        linux_path[sizeof(linux_path) - 1] = '\0';
    }
#endif
    if (!fp)
        fp = fopen(linux_path, "r");

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

    /* Still not found: the ODS-2 namespace is case-folded but the Linux backing
     * store is case-sensitive, so vmsfs_to_linux_path lowercases a leaf that does
     * not exist yet -- e.g. `@SYS$SYSTEM:BUILD` -> ".../build", missing the real
     * BUILD.COM. Case-resolve the base and the two .COM spellings against the
     * actual directory entries. (vms-615) */
    if (!fp) {
        extern int vmsfs_resolve_path_case(const char *, char *, size_t);
        const char *exts[] = { "", ".com", ".COM" };
        char cand[1024], resolved[1024];
        for (unsigned e = 0; e < 3 && !fp; e++) {
            snprintf(cand, sizeof(cand), "%s%s", linux_path, exts[e]);
            if (vmsfs_resolve_path_case(cand, resolved, sizeof(resolved)) == 0) {
                fp = fopen(resolved, "r");
                if (fp) {
                    strncpy(linux_path, resolved, sizeof(linux_path) - 1);
                    linux_path[sizeof(linux_path) - 1] = '\0';
                }
            }
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
    ctx->proc_stack[ctx->proc_depth].is_subroutine = 0;
    /* GOSUBs opened inside this procedure return within it; a RETURN below this
     * base ends the procedure rather than a caller's GOSUB. */
    ctx->proc_stack[ctx->proc_depth].gosub_base = ctx->gosub_depth;

    /* Push a fresh local symbol frame for this command level. VMS gives each
     * @ level its own local symbol table: locals defined here (including P1-P8)
     * are private to this procedure and are discarded on return, so they never
     * leak to the caller and the caller's locals are read-only from here. */
    dcl_sym_push_frame();

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
    int status = dcl_run_proc_lines(ctx, fp);

    /* A stray RETURN at the top level of a procedure (no open GOSUB, not a
     * subroutine) behaves like EXIT: adopt its status and stop. */
    if (ctx->return_requested) {
        status = ctx->return_status;
        ctx->return_requested = 0;
    }

    /* Pop procedure from stack */
    fclose(ctx->proc_stack[ctx->proc_depth].fp);
    ctx->proc_stack[ctx->proc_depth].fp = NULL;
    ctx->proc_depth--;

    /* Discard this level's local symbol frame. This is what restores the
     * caller's P1-P8 and every other caller-level local: they live in the
     * caller's frame, untouched, and this procedure's locals (including its
     * own P1-P8) are freed here rather than persisting into the caller. */
    dcl_sym_pop_frame();

    return status;
}
