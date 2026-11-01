/*
 * test_parse_ext_names.c - DCL's parameter lexing under the two file-name
 * parse styles (rd vms-0b0, epic vms-810).
 *
 * The oracle is lab-Alpha V8.4 on an ODS-5 volume,
 * tests/lab/captures/ods5-alpha-20261022/CONSOLE.txt (session 1):
 *   - under SET PROCESS/PARSE_STYLE=EXTENDED, `OPEN/WRITE F c^+^+config.h`,
 *     `COPY NL: libstdc^+^+.a`, `COPY NL: a^_b.txt`, `COPY NL: ^U4E2D.txt` and
 *     `COPY NL: MixedCase.Txt` each name one file, its escapes and case intact;
 *   - under PARSE_STYLE=TRADITIONAL, `TYPE c^+^+config.h` is refused
 *     %DCL-W-PARMDEL, invalid parameter delimiter ... \^\.
 *
 * HERMETIC, like test_parse_at_param.c: dcl_parser.c + dcl_lexer.c compiled in,
 * dcl_parse_line() called directly. The parse style the product reads from the
 * executive (dcl_cmd_set.c) is supplied here by the strong definition below,
 * which overrides the parser's weak TRADITIONAL default.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dcl/parser.h"

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

static int g_extended;
int dcl_parse_style_extended(void)
{
    return g_extended;
}

static int failures;

static void check(int ok, const char *what)
{
    printf("  %s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

#define CLI_PARMDEL 0x00038110u

/* Parse `line`; parameter `idx` must be exactly `expect` and the parse clean. */
static void check_param(const char *line, int idx, const char *expect, const char *what)
{
    struct dcl_command cmd;
    int rc = dcl_parse_line(line, &cmd);
    int ok = rc == 0 && cmd.parse_status == 0 && cmd.param_count > idx &&
             strcmp(cmd.params[idx], expect) == 0;
    if (!ok)
        printf("    got rc=%d status=%08X params=%d p[%d]='%s'\n", rc, (unsigned)cmd.parse_status,
               cmd.param_count, idx, cmd.param_count > idx ? cmd.params[idx] : "");
    check(ok, what);
}

static void check_parmdel(const char *line, int want, const char *what)
{
    struct dcl_command cmd;
    int rc = dcl_parse_line(line, &cmd);
    int ok = rc == 0 && (want ? cmd.parse_status == CLI_PARMDEL : cmd.parse_status == 0);
    if (!ok)
        printf("    got rc=%d status=%08X\n", rc, (unsigned)cmd.parse_status);
    check(ok, what);
}

int main(void)
{
    printf("=== test_parse_ext_names: DCL parameters under PARSE_STYLE (rd vms-0b0) ===\n");

    g_extended = 1;
    check_param("OPEN/WRITE F c^+^+config.h", 1, "c^+^+config.h",
                "EXTENDED: c^+^+config.h is one parameter, escapes intact");
    check_param("COPY NL: libstdc^+^+.a", 1, "libstdc^+^+.a",
                "EXTENDED: libstdc^+^+.a is one parameter");
    check_param("COPY NL: a^_b.txt", 1, "a^_b.txt", "EXTENDED: a^_b.txt");
    check_param("COPY NL: a^ b.txt", 1, "a^ b.txt",
                "EXTENDED: an escaped blank does not end the parameter");
    check_param("COPY NL: ^U4E2D.txt", 1, "^U4E2D.txt",
                "EXTENDED: a parameter may start with an escape");
    check_param("COPY NL: a^.b.c.d", 1, "a^.b.c.d", "EXTENDED: an escaped dot");
    check_param("COPY NL: MixedCase.Txt", 1, "MixedCase.Txt",
                "EXTENDED: the case of a parameter is kept");
    check_param("TYPE PLAIN.TXT", 0, "PLAIN.TXT", "EXTENDED: a plain name is unchanged");

    g_extended = 0;
    /* negative control: the same command the EXTENDED cases accept */
    check_parmdel("TYPE c^+^+config.h", 1,
                  "TRADITIONAL: TYPE c^+^+config.h is refused PARMDEL, as lab-Alpha V8.4 refuses it");
    check_parmdel("COPY NL: a^_b.txt", 1, "TRADITIONAL: a^_b.txt is refused PARMDEL");
    check_parmdel("TYPE PLAIN.TXT", 0, "TRADITIONAL: a plain name parses clean");
    check_parmdel("WRITE SYS$OUTPUT \"c^+^+config.h\"", 0,
                  "TRADITIONAL: a '^' inside quotes is text, not a delimiter");

    printf("=== test_parse_ext_names: %s ===\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
