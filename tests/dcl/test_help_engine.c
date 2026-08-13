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
#include <stdint.h>
#include <unistd.h>

#include "dcl/help.h"
#include "dcl/hlb.h"
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

    /* 7. Compiled .HLB reader (help_open_hlb): build an LBRO HELP container by
     * hand -- one module per level-1 key, module body = that key's subtree --
     * and assert HELP navigates it identically to the .HLP source. This is the
     * indexed binary form LIBRARY/HELP/CREATE writes (dcl/hlb.h). */
    {
        const char *modA_name = "COPY";
        const char *modA_body =
            "1 COPY\n"
            " Copies a file.\n"
            "2 Qualifiers\n"
            "3 /LOG\n"
            " Displays each file as it is copied.\n";
        const char *modB_name = "DELETE";
        const char *modB_body = "1 DELETE\n Deletes a file.\n";

        char path[] = "/tmp/ovmx_help_XXXXXX";
        int fd = mkstemp(path);
        check(fd >= 0, "mkstemp for .HLB");
        FILE *fp = fdopen(fd, "wb");

        struct lbr_header hdr = {0};
        hdr.magic = LBR_MAGIC;
        hdr.type = LBR_TYPE_HELP;
        hdr.module_count = 2;

        struct lbr_module m[2];
        memset(m, 0, sizeof(m));
        uint32_t base = (uint32_t)(sizeof(hdr) + 2 * sizeof(struct lbr_module));
        strncpy(m[0].name, modA_name, LBR_NAME_LEN - 1);
        m[0].offset = base;
        m[0].length = (uint32_t)strlen(modA_body);
        strncpy(m[1].name, modB_name, LBR_NAME_LEN - 1);
        m[1].offset = base + m[0].length;
        m[1].length = (uint32_t)strlen(modB_body);

        fwrite(&hdr, sizeof(hdr), 1, fp);
        fwrite(m, sizeof(m[0]), 2, fp);
        fwrite(modA_body, 1, m[0].length, fp);
        fwrite(modB_body, 1, m[1].length, fp);
        fclose(fp);

        help_lib_t *hlb = help_open_hlb(path);
        check(hlb != NULL, "help_open_hlb parses the compiled .HLB");

        int st;
        const char *toppath[] = { "COPY" };
        char *out = render_path(hlb, toppath, 1, &st);
        check(out && strstr(out, "Copies a file.") != NULL,
              ".HLB HELP COPY shows body text from the compiled library");
        free(out);

        const char *deep[] = { "COPY", "Qualifiers", "/LOG" };
        out = render_path(hlb, deep, 3, &st);
        check(out && strstr(out, "Displays each file as it is copied.") != NULL,
              ".HLB deep subtopic navigates the reconstructed tree");
        free(out);

        out = render_path(hlb, NULL, 0, &st);
        check(out && strstr(out, "COPY") && strstr(out, "DELETE"),
              ".HLB top level lists both module keys");
        free(out);

        /* help_open_any auto-detects the LBRO magic. */
        help_lib_t *any = help_open_any(path);
        check(any != NULL, "help_open_any detects and opens the .HLB");
        help_close(any);

        help_close(hlb);
        unlink(path);
    }

    /* 8. HLP$LIBRARY search-list merge (help_open_libraries): two libraries,
     * earlier wins for a shared key, top level is the union. */
    {
        char pa[] = "/tmp/ovmx_helpa_XXXXXX";
        char pb[] = "/tmp/ovmx_helpb_XXXXXX";
        int fa = mkstemp(pa), fb = mkstemp(pb);
        check(fa >= 0 && fb >= 0, "mkstemp for search-list .HLP files");
        FILE *A = fdopen(fa, "wb"), *B = fdopen(fb, "wb");
        /* Library A: ALPHA + SHARED (variant A). */
        fputs("1 ALPHA\n Alpha from library A.\n"
              "1 SHARED\n SHARED as defined in library A.\n", A);
        /* Library B: BETA + SHARED (variant B). */
        fputs("1 BETA\n Beta from library B.\n"
              "1 SHARED\n SHARED as defined in library B.\n", B);
        fclose(A);
        fclose(B);

        const char *paths[] = { pa, pb };
        help_lib_t *merged = help_open_libraries(paths, 2);
        check(merged != NULL, "help_open_libraries opens the search list");

        int st;
        const char *pAlpha[] = { "ALPHA" };
        char *out = render_path(merged, pAlpha, 1, &st);
        check(out && strstr(out, "Alpha from library A.") != NULL,
              "search list finds a key present only in the first library");
        free(out);

        const char *pBeta[] = { "BETA" };
        out = render_path(merged, pBeta, 1, &st);
        check(out && strstr(out, "Beta from library B.") != NULL && st == SS$_NORMAL,
              "search continues to the second library (HLP$LIBRARY_1 semantics)");
        free(out);

        const char *pShared[] = { "SHARED" };
        out = render_path(merged, pShared, 1, &st);
        check(out && strstr(out, "SHARED as defined in library A.") != NULL,
              "a key in both libraries resolves to the FIRST (search order wins)");
        check(out && strstr(out, "SHARED as defined in library B.") == NULL,
              "the later library does NOT override an earlier key");
        free(out);

        help_close(merged);
        unlink(pa);
        unlink(pb);
    }

    if (failures == 0) {
        printf("HELP_ENGINE_OK\n");
        return 0;
    }
    printf("HELP_ENGINE_FAILURES=%d\n", failures);
    return 1;
}
