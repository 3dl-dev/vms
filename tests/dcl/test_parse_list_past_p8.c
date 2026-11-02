/*
 * test_parse_list_past_p8.c - a comma list longer than P1-P8 is never shortened
 * silently (rd vms-465).
 *
 * GCC's ar for VMS (gcc/config/vms/vms-ar.c) writes one LIBRARY command whose
 * input list holds every object of a batch:
 *     $ library /replace LIBBACKEND.A -
 *     A.O,-  B.O,- ... N.O
 * The parser kept only the first eight parameter slots (the library and seven
 * files) and dropped the rest without a word, so building GCC's cc1 on
 * OVMX/Alpha made libraries missing 220 of their members and the link failed
 * %LINK-F-UNDEF. The elements past P8 are now kept in order (dcl_param_total /
 * dcl_param_at); dcl_exec answers %DCL-W-MAXPARM for any built-in except
 * LIBRARY, which takes the whole list.
 *
 * Hermetic: compiles dcl_parser.c + dcl_lexer.c and calls dcl_parse_line().
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

static struct dcl_command cmd;   /* ~45 KB: off the stack */

int main(void)
{
    char line[DCL_MAX_LINE], want[64];
    printf("=== test_parse_list_past_p8 (rd vms-465) ===\n");

    /* the library and 20 object files, as vms-ar.c's command file joins them */
    strcpy(line, "LIBRARY /REPLACE GCCOBJ:[BACKEND]LIBBACKEND.A ");
    for (int i = 1; i <= 20; i++) {
        char f[64];
        snprintf(f, sizeof f, "%sGCCOBJ:[BACKEND]M%02d.O", i > 1 ? "," : "", i);
        strcat(line, f);
    }
    check(dcl_parse_line(line, &cmd) == 0, "a 21-element LIBRARY command parses");
    check(cmd.param_count == 8, "P1-P8 hold the library and the first seven files");
    check(dcl_param_total(&cmd) == 21, "all 21 elements are kept (none dropped)");
    check(!cmd.overflow_lost, "nothing was lost");
    int order = 1;
    for (int i = 1; i <= 20; i++) {
        snprintf(want, sizeof want, "GCCOBJ:[BACKEND]M%02d.O", i);
        const char *got = dcl_param_at(&cmd, i);
        if (!got || strcmp(got, want) != 0) {
            printf("  INFO: element %d = \"%s\", want \"%s\"\n", i, got ? got : "(null)", want);
            order = 0;
        }
    }
    check(order, "elements 1..20 are the files, in order");
    check(dcl_param_at(&cmd, 21) == NULL, "past the end is NULL");
    check(dcl_has_qualifier(&cmd, "REPLACE"), "the /REPLACE qualifier is still parsed");

    /* eight or fewer: no overflow, as before */
    check(dcl_parse_line("DELETE A.O;*,B.O;*,C.O;*", &cmd) == 0 &&
          cmd.param_count == 3 && cmd.overflow_count == 0 && dcl_param_total(&cmd) == 3,
          "a short list fills params[] only");

    printf("test_parse_list_past_p8: %s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
