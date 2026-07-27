/*
 * test_lib_fb3.c - Unit tests for the vms-fb3 conformance-gap batch
 * (round 2)
 *
 * Covers the LIB$/SYS$ routines implemented to close the next-highest-
 * unblock-count symbols from docs/conformance-gap-report.md
 * (missing_function category), following on from vms-cd4:
 *
 *   lib$sys_asctim                  - binary time to ASCII (RTL entry point)
 *   sys$deltva_64                   - delete VA space (64-bit region API)
 *   lib$getdvi                      - simplified device-info wrapper
 *   sys$dgblsc                      - delete global section
 *   sys$device_scan                 - wildcard device enumeration
 *   sys$unwind                      - unwind the condition-handler chain
 *   sys$lkwset / sys$ulwset         - lock/unlock working set (no-op)
 *
 * sys$asctoid and sys$cpu_transitionw are PURITY-DEFERRED (need
 * operator-signed VMS constants) and are intentionally not covered here.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ssdef.h"
#include "descrip.h"
#include "gen64def.h"
#include "va_rangedef.h"
#include "dvidef.h"
#include "dcdef.h"
#include "dvsdef.h"
#include "iledef.h"
#include "lib$routines.h"
#include "starlet.h"

static int failures = 0;
static int total = 0;

static void check(int cond, const char *name)
{
    total++;
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

/* ------------------------------------------------------------------ */
/* lib$sys_asctim                                                      */
/* ------------------------------------------------------------------ */
static void test_sys_asctim(void)
{
    printf("Testing lib$sys_asctim...\n");

    GENERIC_64 now;
    check(sys$gettim(&now.gen64$q_quadword) == SS$_NORMAL,
          "sys$gettim seeds the GENERIC_64 for lib$sys_asctim");

    char outbuf[32];
    memset(outbuf, 0, sizeof(outbuf));
    struct dsc$descriptor_s out_d = {
        sizeof(outbuf) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, outbuf
    };
    uint16_t outlen = 0;

    uint32_t st = lib$sys_asctim(&outlen, &out_d, &now, 0);
    check(st == SS$_NORMAL, "lib$sys_asctim returns SS$_NORMAL");
    check(outlen == 23, "lib$sys_asctim writes a 23-char VMS timestamp");
    check(outbuf[2] == '-' && outbuf[6] == '-',
          "lib$sys_asctim formats DD-MMM-YYYY");

    /* NULL timadr means "current time", matching sys$asctim. */
    st = lib$sys_asctim(&outlen, &out_d, NULL, 0);
    check(st == SS$_NORMAL, "lib$sys_asctim(NULL timadr) returns SS$_NORMAL");
}

/* ------------------------------------------------------------------ */
/* sys$deltva_64                                                       */
/* ------------------------------------------------------------------ */
static void test_deltva_64(void)
{
    printf("Testing sys$deltva_64...\n");

    /* sys$expreg (32-bit) allocates real memory we can then delete
     * through the 64-bit entry point - there's no sys$cretva_64/
     * sys$create_region_64 yet to allocate via the 64-bit family
     * itself (see the starlet.h doc comment). */
    void *retadr[2];
    uint32_t st = sys$expreg(4, retadr, 0, 0);
    check(st == SS$_NORMAL, "sys$expreg allocates memory to delete");

    GENERIC_64 region_id = { 0 };
    void *deleted_addr = NULL;
    uint64_t deleted_len = 0;
    uint64_t bytlen = (uint64_t)((char *)retadr[1] - (char *)retadr[0] + 1);

    st = sys$deltva_64(&region_id, retadr[0], bytlen, 0,
                       &deleted_addr, &deleted_len);
    check(st == SS$_NORMAL, "sys$deltva_64 returns SS$_NORMAL");
    check(deleted_addr == retadr[0], "sys$deltva_64 echoes the start address");
    check(deleted_len == bytlen, "sys$deltva_64 echoes the byte length");

    check(sys$deltva_64(&region_id, NULL, bytlen, 0, NULL, NULL) == SS$_BADPARAM,
          "sys$deltva_64(NULL inadr) returns SS$_BADPARAM");
    check(sys$deltva_64(&region_id, retadr[0], 0, 0, NULL, NULL) == SS$_BADPARAM,
          "sys$deltva_64(0 bytlen) returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* lib$getdvi                                                          */
/* ------------------------------------------------------------------ */
static void test_getdvi(void)
{
    printf("Testing lib$getdvi...\n");

    struct dsc$descriptor_s device_d = dsc$init("SYS$DISK:");
    uint32_t item_code = DVI$_FREEBLOCKS;
    uint32_t freeblocks = 0;

    uint32_t st = lib$getdvi(&item_code, 0, &device_d, &freeblocks, 0, 0);
    check(st == SS$_NORMAL, "lib$getdvi returns SS$_NORMAL");

    /* Cross-check against the sys$getdvi path lib$getdvi wraps. */
    uint32_t direct_freeblocks = 0;
    struct item_list_3 il[2] = {
        { sizeof(uint32_t), (uint16_t)DVI$_FREEBLOCKS, &direct_freeblocks, NULL },
        { 0, 0, NULL, NULL }
    };
    uint32_t dst = sys$getdvi(0, 0, &device_d, il, NULL, NULL, 0, 0);
    check(dst == SS$_NORMAL, "sys$getdvi (direct) returns SS$_NORMAL");
    check(freeblocks == direct_freeblocks,
          "lib$getdvi agrees with a direct sys$getdvi call");

    check(lib$getdvi(NULL, 0, &device_d, &freeblocks, 0, 0) == SS$_BADPARAM,
          "lib$getdvi(NULL item_code) returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* sys$dgblsc                                                           */
/* ------------------------------------------------------------------ */
static void test_dgblsc(void)
{
    printf("Testing sys$dgblsc...\n");

    struct dsc$descriptor_s name_d = dsc$init("TEST_FB3_SECTION");
    uint32_t ident = 0;

    uint32_t st = sys$dgblsc(0, &name_d, &ident);
    check(st == SS$_NORMAL, "sys$dgblsc returns SS$_NORMAL");

    check(sys$dgblsc(0, NULL, &ident) == SS$_BADPARAM,
          "sys$dgblsc(NULL gsdnam) returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* sys$device_scan                                                      */
/* ------------------------------------------------------------------ */
static void test_device_scan(void)
{
    printf("Testing sys$device_scan...\n");

    char device[64];
    struct dsc$descriptor_s device_d = {
        sizeof(device) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, device
    };
    struct dsc$descriptor_s wild_d = dsc$init("*");

    GENERIC_64 ctx = { 0 };
    int count = 0;
    uint32_t st;
    while ((st = sys$device_scan(&device_d, &device_d.dsc$w_length,
                                 &wild_d, NULL, &ctx)) == SS$_NORMAL) {
        count++;
        device_d.dsc$w_length = sizeof(device) - 1;
        check(count < 100, "sys$device_scan terminates (loop guard)");
        if (count >= 100) break;
    }
    check(count > 0, "sys$device_scan('*') enumerates at least one device");
    check(st == SS$_NOMOREDEV,
          "sys$device_scan exhausts the list with SS$_NOMOREDEV");

    /* A pattern that matches nothing should report SS$_NOSUCHDEV. */
    GENERIC_64 ctx2 = { 0 };
    struct dsc$descriptor_s nomatch_d = dsc$init("ZZZNOTADEVICE");
    device_d.dsc$w_length = sizeof(device) - 1;
    st = sys$device_scan(&device_d, &device_d.dsc$w_length, &nomatch_d,
                         NULL, &ctx2);
    check(st == SS$_NOSUCHDEV,
          "sys$device_scan with no matches returns SS$_NOSUCHDEV");

    /* DVS$_DEVCLASS filter narrows the result set. */
    GENERIC_64 ctx3 = { 0 };
    unsigned int want_class = DC$_TERM;
    ILE3 dvsitms[] = {
        { 4, DVS$_DEVCLASS, &want_class, NULL },
        { 0, 0, NULL, NULL }
    };
    device_d.dsc$w_length = sizeof(device) - 1;
    st = sys$device_scan(&device_d, &device_d.dsc$w_length, &wild_d,
                         dvsitms, &ctx3);
    check(st == SS$_NORMAL,
          "sys$device_scan with DVS$_DEVCLASS=DC$_TERM finds a terminal");

    check(sys$device_scan(NULL, NULL, &wild_d, NULL, &ctx3) == SS$_BADPARAM,
          "sys$device_scan(NULL devnam) returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* sys$unwind                                                           */
/* ------------------------------------------------------------------ */
static volatile int handler_ran = 0;
static volatile uint32_t handler_saw_condition = 0;

static uint32_t fb3_test_handler(struct chf$signal_array *sig,
                                 struct chf$mech_array *mech)
{
    (void)mech;
    handler_ran = 1;
    handler_saw_condition = sig->chf$is_sig_name;

    /* Unwind one level (NULL depadr) - pops this handler off the chain
     * so a caller after lib$signal returns can re-establish cleanly. */
    uint32_t st = sys$unwind(NULL, NULL);
    (void)st;

    return SS$_CONTINUE;
}

static void test_unwind(void)
{
    printf("Testing sys$unwind...\n");

    check(sys$unwind(NULL, NULL) == SS$_NORMAL,
          "sys$unwind with an empty handler chain is a safe no-op");

    (void)lib$establish(fb3_test_handler);
    handler_ran = 0;
    handler_saw_condition = 0;

    uint32_t st = lib$signal(SS$_NORMAL, 0);
    check(st == SS$_NORMAL, "lib$signal returns SS$_NORMAL after handler runs");
    check(handler_ran, "the established handler ran and called sys$unwind");
    check(handler_saw_condition == SS$_NORMAL,
          "the handler saw the signaled condition");

    /* sys$unwind(NULL,...) inside the handler popped the handler it was
     * running in, so a second signal should NOT re-invoke it (no
     * handler left on the chain -> default handling, no crash). */
    handler_ran = 0;
    st = lib$signal(SS$_NORMAL, 0);
    check(!handler_ran,
          "sys$unwind's pop is durable - the handler is gone after unwind");
    (void)st;

    /* Explicit-depth form: establish two handlers, unwind to depth 0. */
    (void)lib$establish(fb3_test_handler);
    (void)lib$establish(fb3_test_handler);
    uint32_t depth0 = 0;
    check(sys$unwind(&depth0, NULL) == SS$_NORMAL,
          "sys$unwind(&0, NULL) returns SS$_NORMAL");
    handler_ran = 0;
    lib$signal(SS$_NORMAL, 0);
    check(!handler_ran,
          "sys$unwind(&0,...) cleared the handler chain down to depth 0");
}

/* ------------------------------------------------------------------ */
/* sys$lkwset / sys$ulwset                                              */
/* ------------------------------------------------------------------ */
static void test_lkwset(void)
{
    printf("Testing sys$lkwset / sys$ulwset...\n");

    static char array[512 * 4];
    VA_RANGE inadr, outadr;
    inadr.va_range$ps_start_va = &array[0];
    inadr.va_range$ps_end_va = &array[sizeof(array) - 1];

    uint32_t st = sys$lkwset(&inadr, &outadr, 0);
    check(st == SS$_NORMAL, "sys$lkwset returns SS$_NORMAL");
    check(outadr.va_range$ps_start_va == inadr.va_range$ps_start_va,
          "sys$lkwset echoes the start address");

    st = sys$ulwset(&outadr, NULL, 0);
    check(st == SS$_NORMAL, "sys$ulwset returns SS$_NORMAL");

    check(sys$lkwset(NULL, &outadr, 0) == SS$_BADPARAM,
          "sys$lkwset(NULL inadr) returns SS$_BADPARAM");
    check(sys$ulwset(NULL, NULL, 0) == SS$_BADPARAM,
          "sys$ulwset(NULL inadr) returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */
int main(void)
{
    printf("=== test_lib_fb3: vms-fb3 conformance-gap batch (round 2) ===\n");

    test_sys_asctim();
    test_deltva_64();
    test_getdvi();
    test_dgblsc();
    test_device_scan();
    test_unwind();
    test_lkwset();

    printf("\n%d/%d assertions passed.\n", total - failures, total);

    if (failures == 0)
        printf("All lib_fb3 tests passed.\n");
    else
        printf("FAILED: %d test(s) failed.\n", failures);

    return failures;
}
