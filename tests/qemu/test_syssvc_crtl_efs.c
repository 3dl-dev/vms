/*
 * test_syssvc_crtl_efs.c - the DEC C RTL's DECC$EFS_* switches read from their
 * logical names, and a UNIX name with ODS-5 characters reaching a real ODS-5
 * file through the translator, RMS and the executive ACP (rd vms-da9b, epic
 * vms-810), against a real /dev/vms.
 *
 * The volume is the real OpenVMS Alpha V8.4 ODS-5 disk the harness attaches as
 * VDA500: (DKA500: on Alpha), tests/lab/captures/ods5-alpha-20261022/ods5t.dsk;
 * its [TEST]c++config.h holds the record "#define CXXCONFIG 1" the real system
 * wrote. The translations are the ones the real C RTL produced
 * (CONSOLE-5.txt there).
 *
 * PROVES
 *   1. with DECC$EFS_CHARSET and DECC$EFS_CASE_PRESERVE defined as ENABLE in the
 *      process's logical names, the C RTL's switches read 1 at first use;
 *   2. the UNIX name /<unit>/TEST/c++config.h translates to
 *      <unit>:[TEST]c^+^+config.h, and RMS opens it on the ODS-5 volume and reads
 *      the real record;
 *   3. negative control, in a fresh process with no feature logicals: the same
 *      UNIX name is refused (EINVAL), as the real C RTL refused it at its
 *      defaults, and the switches read 0.
 * No /dev/vms -> SKIP (77).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

#include "starlet.h"
#include "ssdef.h"
#include "descrip.h"
#include "lnmdef.h"
#include "ppropdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "rms/rms.h"
#include "rms/fab.h"
#include "rms/rab.h"
#include "rms/crtl_filespec.h"
#include "rms/crtl_features.h"

#define EXIT_SKIP 77
static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

static uint32_t deflog(const char *name, const char *val)
{
    struct dsc$descriptor_s td = { 17, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"LNM$PROCESS_TABLE" };
    struct dsc$descriptor_s nd = { (uint16_t)strlen(name), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)name };
    struct item_list_3 il[2];
    memset(il, 0, sizeof il);
    il[0].buflen = (uint16_t)strlen(val);
    il[0].item_code = LNM$_STRING;
    il[0].bufaddr = (void *)val;
    return sys$crelnm(NULL, &td, &nd, NULL, il);
}

static int feat(const char *name)
{
    return ovmx_crtl_feature_get_value(ovmx_crtl_feature_get_index(name), 1);
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

int main(void)
{
    const char *unit = NULL;
    char ux[128], vspec[256], want[128], buf[256], b[32];
    uint32_t mj, mn, chan = 0;
    int k;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_crtl_efs: DECC$EFS_* switches + an ODS-5 file by its UNIX name (rd vms-da9b) ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) { printf("  FAIL: pcb\n"); return 1; }
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_crtl_efs: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    if (vms_kif_disk_resolve("VDA500:", b, sizeof b, &mj, &mn) == SS$_NORMAL) unit = "VDA500";
    else if (vms_kif_disk_resolve("DKA500:", b, sizeof b, &mj, &mn) == SS$_NORMAL) unit = "DKA500";
    check(unit != NULL, "the ODS-5 fixture disk is attached (VDA500:/DKA500:)");
    if (!unit) { printf("=== test_syssvc_crtl_efs: %d passed, %d failed ===\n", pass, fail); return 1; }
    snprintf(b, sizeof b, "%s:", unit);
    check((vms_kif_acp_mount(b) & 1) && (vms_kif_acp_assign(b, &chan) & 1), "$MOUNT the ODS-5 volume");
    snprintf(ux, sizeof ux, "/%s/TEST/c++config.h", unit);

    /* 3. negative control first, in a fresh process: no feature logicals */
    {
        int p[2], r[2] = { -9, -9 };
        pid_t kid;
        if (pipe(p) == 0 && (kid = fork()) == 0) {
            char o[256];
            int v[2];
            close(p[0]);
            v[0] = feat("DECC$EFS_CHARSET");
            v[1] = ovmx_crtl_unix_to_vms(ux, o, sizeof o, OVMX_FS_AUTO) < 0 && errno == EINVAL;
            (void)!write(p[1], v, sizeof v);
            _exit(0);
        }
        close(p[1]);
        (void)!read(p[0], r, sizeof r);
        close(p[0]);
        wait(NULL);
        check(r[0] == 0, "with no feature logical, DECC$EFS_CHARSET reads 0 (the DEC C default)");
        /* negctl: crtl-efs-default-accepts-plus */
        check(r[1] == 1, "... and the UNIX name with '+' is refused (EINVAL), as the real C RTL refused it");
    }

    /* 1. the switches take their value from the logical names */
    check((deflog("DECC$EFS_CHARSET", "ENABLE") & 1) && (deflog("DECC$EFS_CASE_PRESERVE", "ENABLE") & 1),
          "DEFINE DECC$EFS_CHARSET / DECC$EFS_CASE_PRESERVE ENABLE (process table)");
    check(feat("DECC$EFS_CHARSET") == 1 && feat("DECC$EFS_CASE_PRESERVE") == 1,
          "the C RTL reads both switches as 1 from their logical names");

    /* 2. translate and open */
    k = ovmx_crtl_unix_to_vms(ux, vspec, sizeof vspec, OVMX_FS_AUTO);
    snprintf(want, sizeof want, "%s:[TEST]c^+^+config.h", unit);
    check(k == OVMX_FS_FILE && strcmp(vspec, want) == 0,
          "the UNIX name translates to <unit>:[TEST]c^+^+config.h (escaped, case kept)");
    (void)sys$set_process_propertiesw(NULL, NULL, NULL, PPROP$C_PARSE_STYLE_PERM,
                                      PARSE_STYLE$C_EXTENDED, NULL, 0, 0);
    check((read_first(vspec, buf, sizeof buf) & 1) && strcmp(buf, "#define CXXCONFIG 1") == 0,
          "RMS opens it on the ODS-5 volume and reads the real record");
    (void)sys$set_process_propertiesw(NULL, NULL, NULL, PPROP$C_PARSE_STYLE_PERM,
                                      PARSE_STYLE$C_TRADITIONAL, NULL, 0, 0);

    printf("=== test_syssvc_crtl_efs: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
