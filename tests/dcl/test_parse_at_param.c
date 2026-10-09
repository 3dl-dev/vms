/*
 * test_parse_at_param.c - '@' survives in a DCL PARAMETER position (rd vms-a18).
 *
 * `$ SPAWN/NOWAIT @SYS$LOGIN:X.COM` on a booted OVMX node ran the subprocess
 * with the verb SYS$LOGIN:X.COM and answered %DCL-E-IVVERB: the parser honours
 * '@' as a VERB only at the start of a line, and its generic parameter loop
 * silently discarded the TOK_AT token everywhere else. cmd_spawn builds the
 * subprocess's command text by joining the surviving parameters, so the '@'
 * was simply gone by the time anything could use it. Found while trying to run
 * the ci.6 evacuation workload in the background (rd vms-06c).
 *
 * This is a HERMETIC unit test of the parser itself (the same shape as
 * test_help_engine.c): it compiles dcl_parser.c + dcl_lexer.c and calls
 * dcl_parse_line() directly, so the property is proven where the defect lives,
 * with no DCL image, no subprocess and no executive. The end-to-end SPAWN
 * behaviour needs a real /dev/vms ($CREPRC) and is asserted where that exists.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dcl/parser.h"

/*
 * dcl_error is DCL's %FACILITY-S-IDENT message PRINTER (dcl_io.c, which also
 * pulls in readline). The parser calls it only to report a bad qualifier or
 * keyword; nothing about parsing depends on what it does. Printing the same
 * text here keeps this test hermetic without stubbing out any parsing
 * behaviour -- and the cases below parse clean, so it is not even reached.
 */
void dcl_error(const char *facility, int severity, const char *ident,
               const char *fmt, ...);
void dcl_error(const char *facility, int severity, const char *ident,
               const char *fmt, ...)
{
    va_list ap;
    (void)severity;
    fprintf(stderr, "  INFO: %%%s-?-%s, ", facility ? facility : "DCL",
            ident ? ident : "ERROR");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int failures;

static void check(int ok, const char *what)
{
    printf("  %s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

/* Parse `line` and report whether parameter 0 is exactly `expect`. */
static void check_param0(const char *line, const char *expect, const char *what)
{
    struct dcl_command cmd;
    int rc = dcl_parse_line(line, &cmd);
    int ok = (rc == 0 && cmd.param_count >= 1 &&
              strcmp(cmd.params[0], expect) == 0);
    if (!ok)
        printf("  INFO: \"%s\" -> rc=%d param_count=%d params[0]=\"%s\"\n",
               line, rc, rc == 0 ? cmd.param_count : -1,
               rc == 0 && cmd.param_count >= 1 ? cmd.params[0] : "");
    check(ok, what);
}

int main(void)
{
    printf("=== test_parse_at_param ('@' in a parameter position, vms-a18) ===\n");

    /* The reported case, and its /qualifier form. */
    check_param0("SPAWN @SYS$LOGIN:X.COM", "@SYS$LOGIN:X.COM",
                 "SPAWN @SYS$LOGIN:X.COM keeps the '@' on parameter 1");
    check_param0("SPAWN/NOWAIT @SYS$LOGIN:X.COM", "@SYS$LOGIN:X.COM",
                 "SPAWN/NOWAIT @SYS$LOGIN:X.COM keeps the '@' on parameter 1");
    check_param0("SPAWN/NOWAIT/OUTPUT=F.LOG @SYS$LOGIN:X.COM",
                 "@SYS$LOGIN:X.COM",
                 "SPAWN/NOWAIT/OUTPUT= @SYS$LOGIN:X.COM keeps the '@' on parameter 1");

    /* OpenVMS accepts a blank between '@' and the procedure name. */
    check_param0("SPAWN @ SYS$LOGIN:X.COM", "@SYS$LOGIN:X.COM",
                 "'@ FILE' (blank after the '@') is ONE parameter, '@FILE'");

    /* A trailing '@' is kept as its own parameter rather than vanishing, so a
     * consumer can report its own error about a command it really received. */
    {
        struct dcl_command cmd;
        int rc = dcl_parse_line("SPAWN @", &cmd);
        check(rc == 0 && cmd.param_count == 1 &&
              strcmp(cmd.params[0], "@") == 0,
              "a trailing '@' survives as its own parameter");
    }

    /* '@' at the START of a line is still the procedure-execution VERB, with
     * the filespec as parameter 1 and NO '@' glued to it -- the path this
     * change must not disturb. */
    {
        struct dcl_command cmd;
        int rc = dcl_parse_line("@SYS$LOGIN:X.COM", &cmd);
        check(rc == 0 && strcmp(cmd.verb, "@") == 0 &&
              cmd.param_count == 1 &&
              strcmp(cmd.params[0], "SYS$LOGIN:X.COM") == 0,
              "'@FILE' at line start is still the '@' verb with FILE as parameter 1");
    }

    /* A command with no '@' is unchanged. */
    check_param0("SPAWN SHOW TIME", "SHOW",
                 "a SPAWN with no '@' parses its parameters unchanged");

    printf("=== test_parse_at_param: %s (%d failure(s)) ===\n",
           failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
