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
    printf("Testing lib$getdvi / sys$getdvi (executive device table, vms-dv1)...\n");

    /*
     * These services now READ the executive device table (src/kernel/
     * vms_devtab.c) through vms_kif, not classify_device()+statvfs() on the
     * host. This is a libvms UNIT test with no /dev/vms, so the executive is
     * usually unreachable here; the executive-backed CROSS-PROCESS proof runs
     * in QEMU (tests/qemu/test_syssvc_getdvi.c). What this asserts is the
     * property that holds in BOTH environments: the services never fabricate.
     *
     * Probe the console OPA0: the executive creates at init to learn which
     * environment we are in.
     */
    struct dsc$descriptor_s opa0_d = dsc$init("OPA0:");
    uint32_t devclass = 0;
    struct item_list_3 probe[2] = {
        { sizeof(uint32_t), (uint16_t)DVI$_DEVCLASS, &devclass, NULL },
        { 0, 0, NULL, NULL }
    };
    int have_exec = (sys$getdvi(0, 0, &opa0_d, probe, NULL, NULL, 0, 0)
                     == SS$_NORMAL);

    if (have_exec) {
        check(devclass == DC$_TERM,
              "sys$getdvi reports OPA0: as a terminal read from the executive");

        /* A device the executive does not have is REFUSED, not fabricated --
         * the old fake returned SS$_NORMAL for any name. */
        struct dsc$descriptor_s zz_d = dsc$init("ZZA0:");
        uint32_t zzclass = 0;
        struct item_list_3 zil[2] = {
            { sizeof(uint32_t), (uint16_t)DVI$_DEVCLASS, &zzclass, NULL },
            { 0, 0, NULL, NULL }
        };
        check(sys$getdvi(0, 0, &zz_d, zil, NULL, NULL, 0, 0) != SS$_NORMAL,
              "sys$getdvi refuses a device the executive does not have");
    } else {
        /*
         * No /dev/vms. The proof the fake is gone is that SYS$DISK: -- for
         * which the old host-backed code always returned SS$_NORMAL with
         * invented freeblocks -- now FAILS rather than fabricating.
         */
        struct dsc$descriptor_s sysdisk_d = dsc$init("SYS$DISK:");
        uint32_t item_code = DVI$_FREEBLOCKS;
        uint32_t freeblocks = 0xdead;
        check(lib$getdvi(&item_code, 0, &sysdisk_d, &freeblocks, 0, 0)
                  != SS$_NORMAL,
              "lib$getdvi does NOT fabricate a SYS$DISK answer without an executive");
    }

    /* Argument validation is decided before the executive is consulted, so it
     * holds in either environment. */
    struct dsc$descriptor_s device_d = dsc$init("SYS$DISK:");
    uint32_t freeblocks = 0;
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
    printf("Testing sys$device_scan (executive device table, vms-dv1)...\n");

    /*
     * sys$device_scan now enumerates the EXECUTIVE'S I/O database through
     * vms_kif_devscan(), not the compiled-in scan_devices[] the old fake
     * returned identically on every system. With no /dev/vms it reports a
     * failure rather than that fixed list; the real cross-process enumeration
     * is proven in QEMU (tests/qemu/test_syssvc_getdvi.c).
     */
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
        if (count >= 100) break;
    }

    if (count > 0) {
        /* Executive reachable: it enumerated its real devices and then said
         * it was done, and a name it does not have never matches. */
        check(count < 100, "sys$device_scan terminates");
        check(st == SS$_NOMOREDEV,
              "sys$device_scan exhausts the executive's table with SS$_NOMOREDEV");

        GENERIC_64 ctx2 = { 0 };
        struct dsc$descriptor_s nomatch_d = dsc$init("ZZZNOTADEVICE");
        device_d.dsc$w_length = sizeof(device) - 1;
        check(sys$device_scan(&device_d, &device_d.dsc$w_length, &nomatch_d,
                              NULL, &ctx2) == SS$_NOSUCHDEV,
              "sys$device_scan with no matches returns SS$_NOSUCHDEV");

        /* DVS$_DEVCLASS filter narrows to the console terminal. */
        GENERIC_64 ctx3 = { 0 };
        unsigned int want_class = DC$_TERM;
        ILE3 dvsitms[] = {
            { 4, DVS$_DEVCLASS, &want_class, NULL },
            { 0, 0, NULL, NULL }
        };
        device_d.dsc$w_length = sizeof(device) - 1;
        check(sys$device_scan(&device_d, &device_d.dsc$w_length, &wild_d,
                              dvsitms, &ctx3) == SS$_NORMAL,
              "sys$device_scan with DVS$_DEVCLASS=DC$_TERM finds a terminal");
    } else {
        /* No /dev/vms: a failure, never the old fixed fabricated list. */
        check(st != SS$_NORMAL && st != SS$_NOMOREDEV,
              "sys$device_scan does NOT enumerate a fabricated list without an executive");
    }

    GENERIC_64 ctxbp = { 0 };
    check(sys$device_scan(NULL, NULL, &wild_d, NULL, &ctxbp) == SS$_BADPARAM,
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
