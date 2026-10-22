/*
 * test_crtl_efs_oracle.c - the C RTL's UNIX<->OpenVMS name translation against
 * the real OpenVMS Alpha V8.4 DEC C RTL, replayed line by line (rd vms-da9b,
 * epic vms-810).
 *
 * The oracle is tests/lab/captures/ods5-alpha-20261022/CONSOLE-5.txt: our own
 * MACRO-32 probe (TOVMS.MAR, in that console) called decc$to_vms on eleven UNIX
 * names and decc$translate_vms on seven OpenVMS names, once per feature setting
 * (RUN-DEFAULT, RUN-EFS_CHARSET, RUN-EFS_CHARSET+CASE_PRESERVE, ...). Each case
 * is printed as
 *     --
 *     <input>
 *     <output>       (absent when the routine produced nothing)
 * This test sets the same DECC$ switches through decc$feature_set_value, runs
 * the same inputs through OVMX's translator, and compares byte for byte.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "rms/crtl_filespec.h"
#include "rms/crtl_features.h"

static int g_pass, g_fail;

/* decc$to_vms in AUTO mode, no wildcards: the one string it would hand the
 * action routine, or "" when it refuses */
static const char *to_vms(const char *u)
{
    static char out[512];
    if (ovmx_crtl_unix_to_vms(u, out, sizeof out, OVMX_FS_AUTO) < 0)
        out[0] = '\0';
    return out;
}

static const char *to_unix(const char *v)
{
    static char out[512];
    if (ovmx_crtl_vms_to_unix(v, out, sizeof out) < 0)
        out[0] = '\0';
    return out;
}

static void set_switches(const char *run)
{
    int efs = strstr(run, "EFS_CHARSET") != NULL;
    int cp = strstr(run, "CASE_PRESERVE") != NULL;
    ovmx_crtl_feature_set_value(ovmx_crtl_feature_get_index("DECC$EFS_CHARSET"), 1, efs);
    ovmx_crtl_feature_set_value(ovmx_crtl_feature_get_index("DECC$EFS_CASE_PRESERVE"), 1, cp);
}

int main(void)
{
    const char *dir = getenv("ODS5_FIXTURE_DIR");
    char path[1024], line[512], run[512] = "", in[512] = "";
    int state = 0, cases = 0, runs = 0, index_in_run = 0;
    FILE *fp;

    if (!dir) { printf("FAIL: ODS5_FIXTURE_DIR not set\n"); return 1; }
    snprintf(path, sizeof path, "%s/CONSOLE-5.txt", dir);
    fp = fopen(path, "r");
    if (!fp) { printf("FAIL: open %s\n", path); return 1; }

    if (ovmx_crtl_feature_get_index("DECC$EFS_CHARSET") < 1 ||
        ovmx_crtl_feature_get_index("DECC$EFS_CASE_PRESERVE") < 1) {
        printf("FAIL: the EFS switches are not in the feature table\n");
        return 1;
    }

    /* The probe's first 11 inputs went to decc$to_vms, the next 7 to
     * decc$translate_vms (TOVMS.MAR: UNAMES then VNAMES). */
#define CHECK_CASE(got_out, want) do { \
        const char *g_ = (got_out); \
        if (strcmp(g_, (want)) == 0) g_pass++; \
        else { g_fail++; printf("  FAIL [%s] %s '%s': OVMX '%s', VMS '%s'\n", run, \
               index_in_run <= 11 ? "to_vms" : "translate_vms", in, g_, (want)); } \
        cases++; } while (0)

    while (fgets(line, sizeof line, fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "RUN-", 4) == 0) {
            snprintf(run, sizeof run, "%s", line);
            set_switches(run);
            runs++;
            index_in_run = 0;
            state = 0;
            continue;
        }
        if (!run[0])
            continue;
        if (strcmp(line, "--") == 0 || line[0] == '$') {
            if (state == 2)                      /* the previous case had no output */
                CHECK_CASE(index_in_run <= 11 ? to_vms(in) : to_unix(in), "");
            state = strcmp(line, "--") == 0 ? 1 : 0;
            if (line[0] == '$') {
                if (strncmp(line, "$ DEFINE", 8) == 0 || strncmp(line, "$ DEASSIGN", 10) == 0 ||
                    strncmp(line, "$ WRITE", 7) == 0)
                    run[0] = '\0';               /* this run's cases are over */
            }
            continue;
        }
        if (state == 1) {                        /* the input */
            snprintf(in, sizeof in, "%s", line);
            index_in_run++;
            state = 2;
        } else if (state == 2) {                 /* its output */
            CHECK_CASE(index_in_run <= 11 ? to_vms(in) : to_unix(in), line);
            state = 3;
        }
    }
    fclose(fp);
    if (runs != 5 || cases != 5 * 18) {
        printf("  FAIL: replayed %d runs / %d cases (the console holds 5 / 90)\n", runs, cases);
        g_fail++;
    }
    printf("test_crtl_efs_oracle: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
