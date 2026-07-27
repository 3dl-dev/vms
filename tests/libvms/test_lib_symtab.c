/*
 * test_lib_symtab.c - Unit tests for the vms-cd4 conformance-gap batch
 *
 * Covers the LIB$/SYS$ routines implemented to close the highest-
 * unblock-count symbols from docs/conformance-gap-report.md
 * (missing_function category):
 *
 *   lib$sget1_dd / lib$sfree1_dd / lib$sfreen_dd - dynamic descriptors
 *   lib$set_logical / lib$delete_logical         - logical names
 *   lib$set_symbol / lib$delete_symbol / lib$get_symbol - CLI symbols
 *   sys$purgws                                   - working set purge
 *   sys$getutc                                   - UTC time
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"
#include "libclidef.h"
#include "starlet.h"
#include "va_rangedef.h"

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
/* lib$sget1_dd / lib$sfree1_dd                                       */
/* ------------------------------------------------------------------ */
static void test_dyndesc_1(void)
{
    printf("Testing lib$sget1_dd / lib$sfree1_dd...\n");

    struct dsc$descriptor_d d = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_D, NULL };
    uint32_t len = 16;

    uint32_t st = lib$sget1_dd(&len, &d);
    check(st == SS$_NORMAL, "lib$sget1_dd returns SS$_NORMAL");
    check(d.dsc$w_length == 16, "lib$sget1_dd sets dsc$w_length");
    check(d.dsc$a_pointer != NULL, "lib$sget1_dd allocates a buffer");
    check(d.dsc$b_class == DSC$K_CLASS_D, "lib$sget1_dd keeps CLASS_D");

    memcpy(d.dsc$a_pointer, "hello world", 11);

    st = lib$sfree1_dd((uint64_t *)&d);
    check(st == SS$_NORMAL, "lib$sfree1_dd returns SS$_NORMAL");
    check(d.dsc$a_pointer == NULL, "lib$sfree1_dd clears the pointer");
    check(d.dsc$w_length == 0, "lib$sfree1_dd clears the length");

    check(lib$sget1_dd(&len, NULL) == SS$_BADPARAM,
          "lib$sget1_dd(NULL) returns SS$_BADPARAM");
    check(lib$sfree1_dd(NULL) == SS$_BADPARAM,
          "lib$sfree1_dd(NULL) returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* lib$sfreen_dd                                                      */
/* ------------------------------------------------------------------ */
static void test_dyndesc_n(void)
{
    printf("Testing lib$sfreen_dd...\n");

    #define NDESC 5
    struct dsc$descriptor_d arr[NDESC];
    for (int i = 0; i < NDESC; i++) {
        arr[i] = (struct dsc$descriptor_d){ 0, DSC$K_DTYPE_T, DSC$K_CLASS_D, NULL };
        uint32_t len = 8 + i;
        check(lib$sget1_dd(&len, &arr[i]) == SS$_NORMAL,
              "lib$sget1_dd allocates array element");
    }

    uint32_t n = NDESC;
    uint32_t st = lib$sfreen_dd(&n, arr);
    check(st == SS$_NORMAL, "lib$sfreen_dd returns SS$_NORMAL");

    int all_freed = 1;
    for (int i = 0; i < NDESC; i++) {
        if (arr[i].dsc$a_pointer != NULL || arr[i].dsc$w_length != 0)
            all_freed = 0;
    }
    check(all_freed, "lib$sfreen_dd frees every element");

    check(lib$sfreen_dd(NULL, arr) == SS$_BADPARAM,
          "lib$sfreen_dd(NULL, arr) returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* lib$set_logical / lib$delete_logical                               */
/* ------------------------------------------------------------------ */
static void test_logical(void)
{
    printf("Testing lib$set_logical / lib$delete_logical...\n");

    struct dsc$descriptor_s log_d = dsc$init("TEST_LIB_SYMTAB_LOGICAL");
    struct dsc$descriptor_s eqv_d = dsc$init("some equivalence value");
    struct dsc$descriptor_s tbl_d = dsc$init("LNM$PROCESS_TABLE");

    uint32_t st = lib$set_logical(&log_d, &eqv_d, &tbl_d, NULL, NULL);
    check(st == SS$_NORMAL || st == SS$_SUPERSEDE,
          "lib$set_logical returns SS$_NORMAL/SS$_SUPERSEDE");

    /* Verify it round-trips through sys$trnlnm. */
    char outbuf[64];
    memset(outbuf, 0, sizeof(outbuf));
    struct dsc$descriptor_s outdesc = {
        sizeof(outbuf) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, outbuf
    };
    struct item_list_3 il[2] = {
        { (uint16_t)(sizeof(outbuf) - 1), LNM$_STRING, outbuf, NULL },
        { 0, 0, NULL, NULL }
    };
    uint32_t tst = sys$trnlnm(NULL, &tbl_d, &log_d, NULL, il);
    check(tst == SS$_NORMAL, "sys$trnlnm finds the logical lib$set_logical defined");
    check(strncmp(outbuf, "some equivalence value", 23) == 0,
          "sys$trnlnm returns the equivalence value lib$set_logical set");
    (void)outdesc;

    st = lib$delete_logical(&log_d, &tbl_d);
    check(st == SS$_NORMAL, "lib$delete_logical returns SS$_NORMAL");

    st = lib$delete_logical(&log_d, &tbl_d);
    check(st == SS$_NOLOGNAM,
          "lib$delete_logical on a deleted name returns SS$_NOLOGNAM");

    check(lib$set_logical(NULL, &eqv_d, &tbl_d, NULL, NULL) == SS$_BADPARAM,
          "lib$set_logical(NULL lognam) returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* lib$set_symbol / lib$delete_symbol / lib$get_symbol                */
/* ------------------------------------------------------------------ */
static void test_symbol(void)
{
    printf("Testing lib$set_symbol / lib$delete_symbol / lib$get_symbol...\n");

    struct dsc$descriptor_s sym_d = dsc$init("TEST_LIB_SYMTAB_SYMBOL");
    struct dsc$descriptor_s val_d = dsc$init("a symbol value");
    uint32_t local = LIB$K_CLI_LOCAL_SYM;

    uint32_t st = lib$set_symbol(&sym_d, &val_d, &local);
    check(st == SS$_NORMAL, "lib$set_symbol returns SS$_NORMAL");

    char buf[64];
    memset(buf, 0, sizeof(buf));
    struct dsc$descriptor_s buf_d = {
        sizeof(buf) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, buf
    };
    uint16_t vlen = 0;
    uint32_t table_out = 0;
    st = lib$get_symbol(&sym_d, &buf_d, &vlen, &table_out);
    check(st == SS$_NORMAL, "lib$get_symbol finds the symbol lib$set_symbol set");
    check(vlen == 14, "lib$get_symbol returns the correct value length");
    check(strncmp(buf, "a symbol value", 14) == 0,
          "lib$get_symbol returns the correct value");
    check(table_out == LIB$K_CLI_LOCAL_SYM,
          "lib$get_symbol reports LIB$K_CLI_LOCAL_SYM");

    /* Reserved $STATUS symbol should always be readable. */
    struct dsc$descriptor_s status_sym = dsc$init("$STATUS");
    memset(buf, 0, sizeof(buf));
    buf_d.dsc$w_length = sizeof(buf) - 1;
    st = lib$get_symbol(&status_sym, &buf_d, NULL, NULL);
    check(st == SS$_NORMAL, "lib$get_symbol($STATUS) returns SS$_NORMAL");

    st = lib$delete_symbol(&sym_d, &local);
    check(st == SS$_NORMAL, "lib$delete_symbol returns SS$_NORMAL");

    st = lib$get_symbol(&sym_d, &buf_d, NULL, &local);
    check(st == LIB$_NOSUCHSYM,
          "lib$get_symbol after delete returns LIB$_NOSUCHSYM");

    st = lib$delete_symbol(&sym_d, &local);
    check(st == LIB$_NOSUCHSYM,
          "lib$delete_symbol on a deleted symbol returns LIB$_NOSUCHSYM");

    check(lib$set_symbol(NULL, &val_d, &local) == SS$_BADPARAM,
          "lib$set_symbol(NULL symbol) returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* sys$purgws                                                         */
/* ------------------------------------------------------------------ */
static void test_purgws(void)
{
    printf("Testing sys$purgws...\n");

    VA_RANGE range;
    range.va_range$ps_start_va = (void *)0x0;
    range.va_range$ps_end_va   = (void *)0x7FFFFFFF;

    uint32_t st = sys$purgws(&range);
    check(st == SS$_NORMAL, "sys$purgws returns SS$_NORMAL");

    check(sys$purgws(NULL) == SS$_BADPARAM,
          "sys$purgws(NULL) returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* sys$getutc                                                         */
/* ------------------------------------------------------------------ */
static void test_getutc(void)
{
    printf("Testing sys$getutc...\n");

    uint64_t t1 = 0, t2 = 0;
    uint32_t st = sys$getutc(&t1);
    check(st == SS$_NORMAL, "sys$getutc returns SS$_NORMAL");
    check(t1 != 0, "sys$getutc returns a non-zero VMS time");

    /* sys$gettim and sys$getutc should agree closely (both derive
     * from CLOCK_REALTIME with no local-time offset - see the
     * sys$getutc doc comment in starlet.h). */
    st = sys$gettim(&t2);
    check(st == SS$_NORMAL, "sys$gettim returns SS$_NORMAL");
    uint64_t delta = (t1 > t2) ? (t1 - t2) : (t2 - t1);
    check(delta < 10000000ULL /* 1 second, in 100ns ticks */,
          "sys$getutc and sys$gettim agree within 1 second");

    check(sys$getutc(NULL) == SS$_BADPARAM,
          "sys$getutc(NULL) returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */
int main(void)
{
    printf("=== test_lib_symtab: vms-cd4 conformance-gap batch ===\n");

    test_dyndesc_1();
    test_dyndesc_n();
    test_logical();
    test_symbol();
    test_purgws();
    test_getutc();

    printf("\n%d/%d assertions passed.\n", total - failures, total);

    if (failures == 0)
        printf("All lib_symtab tests passed.\n");
    else
        printf("FAILED: %d test(s) failed.\n", failures);

    return failures;
}
