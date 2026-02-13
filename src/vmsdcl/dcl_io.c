/*
 * dcl_io.c - DCL I/O Operations
 *
 * Handles SYS$INPUT/SYS$OUTPUT abstraction, VMS-style error
 * message formatting, and interactive input with readline support.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#include "dcl/context.h"
#include "dcl/parser.h"

#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

/*
 * Read a line of input from the current source.
 * If interactive, uses readline. If in a procedure, reads from the file.
 * Returns 0 on success, -1 on EOF/error.
 */
int dcl_read_input(struct dcl_context *ctx, const char *prompt,
                   char *buffer, size_t bufsize)
{
    if (!ctx || !buffer || bufsize == 0) return -1;

    /* If in a command procedure, read from procedure file */
    if (ctx->proc_depth >= 0 && ctx->proc_stack[ctx->proc_depth].fp) {
        if (!fgets(buffer, (int)bufsize,
                   ctx->proc_stack[ctx->proc_depth].fp)) {
            return -1; /* EOF */
        }
        /* Remove trailing newline */
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
        }
        ctx->proc_stack[ctx->proc_depth].line_number++;

        /* If verify mode is on, echo the command */
        if (ctx->verify) {
            printf("%s\n", buffer);
        }
        return 0;
    }

    /* Interactive input */
    if (ctx->interactive) {
#ifdef HAVE_READLINE
        char *line = readline(prompt ? prompt : "$ ");
        if (!line) return -1; /* EOF */
        strncpy(buffer, line, bufsize - 1);
        buffer[bufsize - 1] = '\0';
        if (line[0] != '\0') {
            add_history(line);
        }
        free(line);
        return 0;
#else
        if (prompt) {
            fputs(prompt, stdout);
            fflush(stdout);
        }
        if (!fgets(buffer, (int)bufsize, stdin)) return -1;
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
        }
        return 0;
#endif
    }

    /* Non-interactive stdin */
    if (!fgets(buffer, (int)bufsize, stdin)) return -1;
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
        buffer[len - 1] = '\0';
    }
    return 0;
}

/*
 * Write a line to SYS$OUTPUT (stdout).
 */
void dcl_write_output(struct dcl_context *ctx, const char *text)
{
    (void)ctx;
    if (text) {
        fputs(text, stdout);
    }
}

/*
 * Write a line to SYS$ERROR (stderr).
 */
void dcl_write_error(struct dcl_context *ctx, const char *text)
{
    (void)ctx;
    if (text) {
        fputs(text, stderr);
    }
}

/*
 * Format a VMS-style error/status message.
 *
 * VMS format: %FACILITY-S-IDENT, text
 * Where S is severity letter: S(uccess), I(nfo), W(arning), E(rror), F(atal)
 */
void dcl_format_error(const char *facility, int severity, const char *ident,
                      const char *text, char *buf, size_t bufsize)
{
    static const char sev_chars[] = "WSEIF";  /* 0=W, 1=S, 2=E, 3=I, 4=F */
    char sev_char = 'E';

    if (severity >= 0 && severity <= 4) {
        sev_char = sev_chars[severity];
    }

    snprintf(buf, bufsize, "%%%s-%c-%s, %s",
             facility ? facility : "DCL",
             sev_char,
             ident ? ident : "ERROR",
             text ? text : "");
}

/*
 * Print a VMS-style error message to stderr.
 */
void dcl_error(const char *facility, int severity, const char *ident,
               const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    char msg[1280];
    dcl_format_error(facility, severity, ident, text, msg, sizeof(msg));
    fprintf(stderr, "%s\n", msg);
}
