/*
 * test_syssvc_rms_ods5.c - RMS on an ODS-5 volume, through the public services,
 * against a real /dev/vms (rd vms-b9c, epic vms-810).
 *
 * The volume is the real OpenVMS Alpha V8.4 ODS-5 disk the harness attaches as
 * VDA500: (DKA500: on Alpha): tests/lab/captures/ods5-alpha-20261022/ods5t.dsk.
 * Expected names are what the real system printed for that very volume
 * (CONSOLE.txt there: DIRECTORY/FILE_ID of [TEST]) and what its F$SEARCH /
 * COPY did in session 3 (RESULTS.md R7-R10).
 *
 * PROVES
 *   1. under PARSE_STYLE=EXTENDED, $PARSE + $SEARCH VDA500:[TEST]*.*;* returns
 *      the real directory's names in its order, case kept and ^-escaped as VMS
 *      writes them (a^_b.txt, a^.b^.c.d, c^+^+config.h, ...);
 *   2. $OPEN of c^+^+config.h, and of C^+^+CONFIG.H, reads the record the real
 *      system wrote;
 *   3. $CREATE of a mixed-case extended name keeps its case on the ODS-5 volume;
 *      under TRADITIONAL the created name is upper case;
 *   4. on the ODS-2 volume, EXTENDED $CREATE of c^+^+config.h is refused
 *      (RMS$_CRE, the XQP's SS$_BADFILENAME on the real system) and a lower-case
 *      name is stored upper case (LOWER.TXT);
 * NOT HERE (tracked): the UCS-2 name ^U4E2D.txt (rd vms-f1a2).
 * No /dev/vms -> SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "starlet.h"
#include "ssdef.h"
#include "rmsdef.h"
#include "ppropdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "rms/rms.h"
#include "rms/fab.h"
#include "rms/nam.h"
#include "rms/rab.h"

#define EXIT_SKIP 77
static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

static const char *g_unit;

static void style(uint32_t prop, uint32_t v)
{
    (void)sys$set_process_propertiesw(NULL, NULL, NULL, prop, v, NULL, 0, 0);
}

/* $PARSE + $SEARCH loop: the resultant strings, in order. */
static int search(const char *spec, char out[][300], int max)
{
    struct FAB fab = cc$rms_fab;
    struct NAM nam = cc$rms_nam;
    char esa[256], rsa[256];
    int n = 0;

    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$l_nam = &nam;
    nam.nam$l_esa = esa; nam.nam$b_ess = sizeof esa - 1;
    nam.nam$l_rsa = rsa; nam.nam$b_rss = sizeof rsa - 1;
    if (!(sys$parse(&fab, 0, 0) & 1))
        return -1;
    while (n < max && (sys$search(&fab, 0, 0) & 1)) {
        memcpy(out[n], rsa, nam.nam$b_rsl);
        out[n][nam.nam$b_rsl] = '\0';
        n++;
    }
    return n;
}

static uint32_t read_first(const char *spec, char *buf, size_t sz)
{
    struct FAB fab = cc$rms_fab;
    struct RAB rab = cc$rms_rab;
    uint32_t st;

    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_fac = FAB$M_GET;
    st = sys$open(&fab, 0, 0);
    if (!(st & 1))
        return st;
    rab.rab$l_fab = &fab;
    rab.rab$l_ubf = buf;
    rab.rab$w_usz = (uint16_t)(sz - 1);
    st = sys$connect(&rab, 0, 0);
    if (st & 1)
        st = sys$get(&rab, 0, 0);
    if (st & 1)
        buf[rab.rab$w_rsz] = '\0';
    sys$close(&fab, 0, 0);
    return st;
}

static uint32_t create_empty(const char *spec)
{
    struct FAB fab = cc$rms_fab;
    uint32_t st;

    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = FAB$C_VAR;
    fab.fab$b_fac = FAB$M_PUT;
    st = sys$create(&fab, 0, 0);
    if (st & 1)
        sys$close(&fab, 0, 0);
    return st;
}

static void erase(const char *spec)
{
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    (void)sys$erase(&fab, 0, 0);
}

int main(void)
{
    static const char *const real[] = {
        "a^_b.txt;1", "a^.b^.c.d;1", "c^+^+config.h;1", "caf\xe9.txt;1", "libstdc^+^+.a;1",
        "MixedCase.Txt;1", "PLAIN.TXT;1", "README.;1",
        "this_is_a_long_extended_file_name_well_past_the_ods2_limit_of_39.header;1" };
    static char res[32][300];
    char spec[300], want[300], buf[256];
    uint32_t st, chan = 0;
    int n, i, ok;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_rms_ods5: RMS on a real Alpha V8.4 ODS-5 volume (rd vms-b9c) ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) { printf("  FAIL: pcb\n"); return 1; }
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_rms_ods5: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    {
        char b[32]; uint32_t mj, mn;
        if (vms_kif_disk_resolve("VDA500:", b, sizeof b, &mj, &mn) == SS$_NORMAL) g_unit = "VDA500:";
        else if (vms_kif_disk_resolve("DKA500:", b, sizeof b, &mj, &mn) == SS$_NORMAL) g_unit = "DKA500:";
    }
    check(g_unit != NULL, "the ODS-5 fixture disk is attached (VDA500:/DKA500:)");
    if (!g_unit) { printf("=== test_syssvc_rms_ods5: %d passed, %d failed ===\n", pass, fail); return 1; }
    check((vms_kif_acp_mount(g_unit) & 1) && (vms_kif_acp_assign(g_unit, &chan) & 1),
          "$MOUNT the ODS-5 volume");

    style(PPROP$C_PARSE_STYLE_PERM, PARSE_STYLE$C_EXTENDED);

    /* 1. the real directory, as the real system lists it */
    snprintf(spec, sizeof spec, "%s[TEST]*.*;*", g_unit);
    n = search(spec, res, 32);
    ok = n == (int)(sizeof real / sizeof real[0]);
    for (i = 0; ok && i < n; i++) {
        snprintf(want, sizeof want, "%s[TEST]%s", g_unit, real[i]);
        ok = strcmp(res[i], want) == 0;
    }
    if (!ok)
        for (i = 0; i < n; i++)
            printf("    search[%d] = '%s'\n", i, res[i]);
    check(ok, "EXTENDED $SEARCH [TEST]*.*;* returns the real names in order, case kept, ^-escaped");
    snprintf(spec, sizeof spec, "%s[TEST]C^+^+CONFIG.H", g_unit);
    n = search(spec, res, 4);
    snprintf(want, sizeof want, "%s[TEST]c^+^+config.h;1", g_unit);
    check(n == 1 && strcmp(res[0], want) == 0,
          "$SEARCH C^+^+CONFIG.H finds c^+^+config.h;1 (real R6: case-blind, stored case returned)");

    /* 2. read the record the real system wrote */
    snprintf(spec, sizeof spec, "%s[TEST]c^+^+config.h", g_unit);
    st = read_first(spec, buf, sizeof buf);
    check((st & 1) && strcmp(buf, "#define CXXCONFIG 1") == 0,
          "$OPEN + $GET c^+^+config.h reads '#define CXXCONFIG 1'");
    snprintf(spec, sizeof spec, "%s[TEST]C^+^+CONFIG.H", g_unit);
    st = read_first(spec, buf, sizeof buf);
    check((st & 1) && strcmp(buf, "#define CXXCONFIG 1") == 0, "... and so does C^+^+CONFIG.H");

    /* 3. create, case kept (EXTENDED) / upper-cased (TRADITIONAL) */
    snprintf(spec, sizeof spec, "%s[TEST]New^+File.Txt", g_unit);
    st = create_empty(spec);
    check(st & 1, "EXTENDED $CREATE New^+File.Txt on the ODS-5 volume");
    snprintf(spec, sizeof spec, "%s[TEST]new*.*", g_unit);
    n = search(spec, res, 4);
    snprintf(want, sizeof want, "%s[TEST]New^+File.Txt;1", g_unit);
    check(n == 1 && strcmp(res[0], want) == 0, "... is listed as New^+File.Txt;1 (case kept)");
    snprintf(spec, sizeof spec, "%s[TEST]New^+File.Txt;1", g_unit);
    erase(spec);

    style(PPROP$C_PARSE_STYLE_TEMP, PARSE_STYLE$C_TRADITIONAL);
    snprintf(spec, sizeof spec, "%s[TEST]lower2.txt", g_unit);
    st = create_empty(spec);
    style(PPROP$C_PARSE_STYLE_TEMP, PARSE_STYLE$C_EXTENDED);
    snprintf(spec, sizeof spec, "%s[TEST]lower2.*", g_unit);
    n = search(spec, res, 4);
    snprintf(want, sizeof want, "%s[TEST]LOWER2.TXT;1", g_unit);
    check((st & 1) && n == 1 && strcmp(res[0], want) == 0,
          "TRADITIONAL $CREATE lower2.txt makes LOWER2.TXT;1 (the style upper-cases)");
    snprintf(spec, sizeof spec, "%s[TEST]LOWER2.TXT;1", g_unit);
    erase(spec);

    /* 4. the ODS-2 volume refuses extended names, upper-cases the rest */
    {
        const char *u2 = NULL;
        char b[32]; uint32_t mj, mn;
        if (vms_kif_disk_resolve("VDA0:", b, sizeof b, &mj, &mn) == SS$_NORMAL) u2 = "VDA0:";
        else if (vms_kif_disk_resolve("DKA0:", b, sizeof b, &mj, &mn) == SS$_NORMAL) u2 = "DKA0:";
        check(u2 && (vms_kif_acp_mount(u2) & 1), "$MOUNT the real-VAX ODS-2 volume");
        if (u2) {
            snprintf(spec, sizeof spec, "%s[OVMXDIR]c^+^+config.h", u2);
            st = create_empty(spec);
            /* negctl: acp-ods2-newname-unchecked */
            check(st == RMS$_CRE, "EXTENDED $CREATE c^+^+config.h on ODS-2 is RMS$_CRE (real: -SYSTEM-W-BADFILENAME)");
            snprintf(spec, sizeof spec, "%s[OVMXDIR]lower.txt", u2);
            st = create_empty(spec);
            snprintf(spec, sizeof spec, "%s[OVMXDIR]lower.*", u2);
            n = search(spec, res, 4);
            snprintf(want, sizeof want, "%s[OVMXDIR]LOWER.TXT;1", u2);
            check((st & 1) && n == 1 && strcmp(res[0], want) == 0,
                  "EXTENDED $CREATE lower.txt on ODS-2 is stored LOWER.TXT;1 (real R10)");
            snprintf(spec, sizeof spec, "%s[OVMXDIR]LOWER.TXT;1", u2);
            erase(spec);
        }
    }

    style(PPROP$C_PARSE_STYLE_PERM, PARSE_STYLE$C_TRADITIONAL);
    printf("=== test_syssvc_rms_ods5: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
