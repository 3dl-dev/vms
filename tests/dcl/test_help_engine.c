/*
 * test_help_engine.c - Hermetic unit test for the hierarchical HELP engine
 * (vms-01b, src/vmsdcl/dcl_help.c).
 *
 * Asserts the VMS HELP navigation directly against library DATA (no DCL binary,
 * no /vms, no library file on disk): the top-level "Information available:"
 * listing, "HELP topic" text + "Additional information available:" subtopic
 * list, "HELP topic subtopic", abbreviated keys, the "Sorry, no documentation
 * on <x>" not-found message + SS$_ITEMNOTFOUND status, and the interactive
 * "Topic?" / "<path> Subtopic?" prompt loop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcl/help.h"
#include "ssdef.h"

static int failures = 0;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("  ok: %s\n", what);
    } else {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

/* A tiny but real hierarchical library (numbered-level .HLP source). */
static const char *SRC =
    "1 COPY\n"
    " Copies a file.\n"
    " Format: COPY source destination\n"
    "2 Qualifiers\n"
    "3 /LOG\n"
    " Displays each file as it is copied.\n"
    "3 /CONFIRM\n"
    " Prompts before each copy operation.\n"
    "1 DELETE\n"
    " Deletes a file.\n"
    "1 DIRECTORY\n"
    " Lists files in a directory.\n"
    "2 /BRIEF\n"
    " Displays only file names.\n";

/* Render helpers capturing into a heap buffer via open_memstream. */
static char *render_path(help_lib_t *lib, const char *const path[], int n,
                         int *status_out)
{
    char *buf = NULL;
    size_t sz = 0;
    FILE *out = open_memstream(&buf, &sz);
    int st = help_render(lib, path, n, out);
    fclose(out);
    if (status_out) *status_out = st;
    return buf; /* caller frees */
}

int main(void)
{
    help_lib_t *lib = help_open_text(SRC);
    check(lib != NULL, "library parses from text");
    if (!lib) return 1;

    /* 1. Top-level listing */
    {
        int st;
        char *out = render_path(lib, NULL, 0, &st);
        check(strstr(out, "Information available:") != NULL,
              "top-level shows 'Information available:'");
        check(strstr(out, "COPY") && strstr(out, "DELETE") &&
              strstr(out, "DIRECTORY"),
              "top-level lists all level-1 topics");
        check(st == SS$_NORMAL, "top-level returns SS$_NORMAL");
        free(out);
    }

    /* 2. HELP topic: body text + subtopic listing */
    {
        const char *path[] = { "COPY" };
        int st;
        char *out = render_path(lib, path, 1, &st);
        check(strstr(out, "Copies a file.") != NULL,
              "HELP COPY shows the topic body text");
        check(strstr(out, "Additional information available:") != NULL,
              "HELP COPY shows 'Additional information available:'");
        check(strstr(out, "Qualifiers") != NULL,
              "HELP COPY lists its subtopic (Qualifiers)");
        check(st == SS$_NORMAL, "HELP COPY returns SS$_NORMAL");
        free(out);
    }

    /* 3. HELP topic subtopic subtopic (three levels deep) */
    {
        const char *path[] = { "COPY", "Qualifiers", "/LOG" };
        int st;
        char *out = render_path(lib, path, 3, &st);
        check(strstr(out, "Displays each file as it is copied.") != NULL,
              "HELP COPY QUALIFIERS /LOG shows the deep subtopic text");
        check(st == SS$_NORMAL, "deep lookup returns SS$_NORMAL");
        free(out);
    }

    /* 4. Abbreviated keys (VMS accepts prefixes) */
    {
        const char *path[] = { "COP" };
        int st;
        char *out = render_path(lib, path, 1, &st);
        check(strstr(out, "Copies a file.") != NULL && st == SS$_NORMAL,
              "abbreviated 'COP' resolves to COPY");
        free(out);
    }

    /* 5. Unknown topic -> authentic message + SS$_ITEMNOTFOUND */
    {
        const char *path[] = { "XYZZY" };
        int st;
        char *out = render_path(lib, path, 1, &st);
        check(strstr(out, "Sorry, no documentation on XYZZY") != NULL,
              "unknown topic prints 'Sorry, no documentation on XYZZY'");
        check(st == SS$_ITEMNOTFOUND,
              "unknown topic returns SS$_ITEMNOTFOUND");
        check(strstr(out, "%DCL-W-NOHELP") == NULL,
              "unknown topic does NOT emit inauthentic %DCL-W-NOHELP");
        free(out);
    }

    /* 6. Interactive prompt loop: Topic? then COPY Subtopic? */
    {
        /* Input: select COPY, then blank pops to top, then blank exits. */
        const char *script = "COPY\n\n\n";
        FILE *in = fmemopen((void *)script, strlen(script), "r");
        char *buf = NULL;
        size_t sz = 0;
        FILE *out = open_memstream(&buf, &sz);
        help_interactive(lib, NULL, 0, in, out);
        fclose(out);
        fclose(in);
        check(strstr(buf, "Topic? ") != NULL,
              "interactive prints the 'Topic? ' prompt");
        check(strstr(buf, "COPY Subtopic? ") != NULL,
              "interactive prompt becomes 'COPY Subtopic? ' inside a topic");
        check(strstr(buf, "Copies a file.") != NULL,
              "interactive displays selected topic text");
        free(buf);
    }

    help_close(lib);

    if (failures == 0) {
        printf("HELP_ENGINE_OK\n");
        return 0;
    }
    printf("HELP_ENGINE_FAILURES=%d\n", failures);
    return 1;
}
