/*
 * test_rms_meta_org.c - the RMS metadata sidecar stores FAB$B_ORG in the V7.3
 * encoding (FAB$C_REL 16, FAB$C_IDX 32), and a version-1 sidecar written under the
 * old encoding (REL 1, IDX 2) is converted on load, not misread (vms-f811).
 *
 * Creates an indexed file through RMS, checks the sidecar it wrote (version 2, org
 * == FAB$C_IDX == 32), then rewrites that sidecar by hand as a legacy version-1
 * file (version byte 1, org byte 2) and reopens the data file with a FAB that does
 * not say what organization it is: $OPEN must restore FAB$C_IDX from the legacy
 * sidecar. Host-only (the sidecar is the executive-absent POSIX backend's).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "rms/rms.h"
#include "rms/xab.h"
#include "rmsdef.h"
#include "ssdef.h"

int vmsfs_resolve(const char *spec, const char *default_spec, char *result, size_t resultlen)
{
    (void)spec; (void)default_spec; (void)result; (void)resultlen;
    return -1;
}

static int failures = 0;
static void check(int cond, const char *name)
{
    printf("  %s: %s\n", cond ? "OK" : "FAIL", name);
    if (!cond) failures++;
}

#define KEY_SIZE 6
#define REC_SIZE 16

static void wipe(const char *path)
{
    char b[1200];
    unlink(path);
    snprintf(b, sizeof b, "%s.rms_meta", path); unlink(b);
    snprintf(b, sizeof b, "%s.rms_idx", path);  unlink(b);
}

/* sidecar layout (src/vmsrms/rms_core.c struct rms_metadata): magic[4] version[1] org[1] ... */
static int read_sidecar(const char *path, unsigned char *buf, size_t n)
{
    char sc[1200];
    snprintf(sc, sizeof sc, "%s.rms_meta", path);
    FILE *f = fopen(sc, "rb");
    if (!f) return -1;
    size_t r = fread(buf, 1, n, f);
    fclose(f);
    return (int)r;
}

int main(void)
{
    printf("=== test_rms_meta_org ===\n");
    char base[256];
    snprintf(base, sizeof base, "/tmp/test_rms_meta_org_%d", (int)getpid());
    wipe(base);

    struct XABKEY kx = cc$rms_xabkey;
    kx.xab$b_ref = 0; kx.xab$b_dtp = XAB$C_STG; kx.xab$w_pos0 = 0;
    kx.xab$b_siz0 = KEY_SIZE; kx.xab$b_nseg = 1;

    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = base; fab.fab$b_fns = (uint8_t)strlen(base);
    fab.fab$b_fac = FAB$M_PUT | FAB$M_GET;
    fab.fab$b_org = FAB$C_IDX; fab.fab$b_rfm = FAB$C_FIX; fab.fab$w_mrs = REC_SIZE;
    fab.fab$l_xab = &kx;
    check(sys$create(&fab, 0, 0) == RMS$_NORMAL, "create an indexed file");

    char resolved[1024];
    strncpy(resolved, fab._resolved_path, sizeof resolved - 1);
    resolved[sizeof resolved - 1] = '\0';

    struct RAB rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    check(sys$connect(&rab, 0, 0) == RMS$_NORMAL, "connect");
    char rec[REC_SIZE];
    memset(rec, 0, sizeof rec); memcpy(rec, "000001", KEY_SIZE);
    rab.rab$b_rac = RAB$C_SEQ; rab.rab$l_rbf = rec; rab.rab$w_rsz = REC_SIZE;
    check(sys$put(&rab, 0, 0) == RMS$_NORMAL, "put one record");
    sys$disconnect(&rab, 0, 0);
    check(sys$close(&fab, 0, 0) == RMS$_NORMAL, "close");

    unsigned char sc[16];
    int n = read_sidecar(resolved, sc, sizeof sc);
    check(n >= 6, "the sidecar exists");
    check(n >= 6 && sc[4] == 2, "the sidecar is written as version 2");
    check(n >= 6 && sc[5] == FAB$C_IDX && FAB$C_IDX == 32, "the sidecar org byte is FAB$C_IDX == 32 (V7.3)");

    /* Forge the legacy form: version 1, org 2 (the old FAB$C_IDX). */
    {
        char scp[1200];
        snprintf(scp, sizeof scp, "%s.rms_meta", resolved);
        FILE *f = fopen(scp, "r+b");
        check(f != NULL, "the sidecar can be rewritten");
        if (f) {
            unsigned char v1[2] = { 1, 2 };
            fseek(f, 4, SEEK_SET);
            check(fwrite(v1, 1, 2, f) == 2, "rewrite version=1 org=2 (legacy encoding)");
            fclose(f);
        }
    }

    struct XABKEY kx2 = cc$rms_xabkey;
    kx2.xab$b_dtp = XAB$C_STG; kx2.xab$w_pos0 = 0; kx2.xab$b_siz0 = KEY_SIZE; kx2.xab$b_nseg = 1;
    struct FAB rf = cc$rms_fab;           /* org left at the default: the sidecar must supply it */
    rf.fab$l_fna = resolved; rf.fab$b_fns = (uint8_t)strlen(resolved);
    rf.fab$b_fac = FAB$M_GET;
    rf.fab$l_xab = &kx2;
    uint32_t st = sys$open(&rf, 0, 0);
    check(st == RMS$_NORMAL, "open with the legacy sidecar");
    check(rf.fab$b_org == FAB$C_IDX, "the legacy org byte 2 is read as FAB$C_IDX, not left as a bogus org");
    sys$close(&rf, 0, 0);

    wipe(base);
    wipe(resolved);
    printf("\n%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
