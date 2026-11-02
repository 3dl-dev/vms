/*
 * test_parse_uic_bracket.c - a comma inside '[...]' is part of the word
 * (rd vms-1fb7).
 *
 * `$ SET FILE/OWNER_UIC=[144,144] F.TXT` answered %SYSTEM-F-IVIDENT on a booted
 * OVMX node: the lexer ended the word at the comma, so the qualifier's value
 * was "[144" and "144]" became a stray parameter. To DCL a UIC is ONE value
 * (OpenVMS DCL Dictionary: SET FILE /OWNER_UIC=uic, SET UIC [g,m], RUN
 * /UIC=uic), and so is a UIC-form directory such as [1,4]X.TXT. A comma
 * OUTSIDE brackets still separates list elements (COPY A,B C).
 *
 * HERMETIC: compiles dcl_parser.c + dcl_lexer.c and calls dcl_parse_line(),
 * the same shape as test_parse_at_param.c.
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

static int failures;

static void check(int ok, const char *what)
{
    printf("  %s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

static const char *qual_value(const struct dcl_command *c, const char *name)
{
    for (int i = 0; i < c->qualifier_count; i++)
        if (strcmp(c->qualifiers[i].name, name) == 0)
            return c->qualifiers[i].value;
    return NULL;
}

int main(void)
{
    struct dcl_command c;
    const char *v;

    printf("=== test_parse_uic_bracket (a UIC is one DCL value, vms-1fb7) ===\n");

    check(dcl_parse_line("SET FILE/OWNER_UIC=[144,144] F.TXT", &c) == 0,
          "SET FILE/OWNER_UIC=[144,144] F.TXT parses");
    v = qual_value(&c, "OWNER_UIC");
    check(v && strcmp(v, "[144,144]") == 0,
          "/OWNER_UIC's value is the whole UIC [144,144]");
    check(c.param_count == 2 && strcmp(c.params[0], "FILE") == 0 &&
          strcmp(c.params[1], "F.TXT") == 0,
          "and the only parameters are FILE and F.TXT (no stray \"144]\")");

    check(dcl_parse_line("SET UIC [1,4]", &c) == 0 && c.param_count == 2 &&
          strcmp(c.params[1], "[1,4]") == 0,
          "SET UIC [1,4] carries [1,4] as one parameter");

    check(dcl_parse_line("RUN/UIC=[300,1] X.EXE", &c) == 0 &&
          (v = qual_value(&c, "UIC")) && strcmp(v, "[300,1]") == 0 &&
          c.param_count == 1 && strcmp(c.params[0], "X.EXE") == 0,
          "RUN/UIC=[300,1] X.EXE: the UIC whole, the image the only parameter");

    check(dcl_parse_line("TYPE [1,4]A.TXT", &c) == 0 && c.param_count == 1 &&
          strcmp(c.params[0], "[1,4]A.TXT") == 0,
          "a UIC-form directory [1,4]A.TXT is one filespec");

    check(dcl_parse_line("COPY A.TXT,[X]B.TXT C.TXT", &c) == 0 &&
          c.param_count == 3 && strcmp(c.params[0], "A.TXT") == 0 &&
          strcmp(c.params[1], "[X]B.TXT") == 0,
          "a comma OUTSIDE brackets still separates list elements");

    printf("=== test_parse_uic_bracket: %s (%d failure%s) ===\n",
           failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
