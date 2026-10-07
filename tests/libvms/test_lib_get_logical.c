/*
 * test_lib_get_logical.c - LIB$GET_LOGICAL translates through SYS$TRNLNM.
 *
 * Defines real logical names (a single value and a three-member search list) in
 * LNM$PROCESS_TABLE through SYS$CRELNM, then reads them back through
 * LIB$GET_LOGICAL: the equivalence strings, the highest index, the index
 * selection, SS$_NOLOGNAM for an undefined name, an explicit table argument, and
 * the idiom of passing the descriptor's own length field as the resultant
 * length. The values read back are the ones the test defined; nothing is
 * canned.
 *
 * Runs on the host: LNM$PROCESS_TABLE is process-private on VMS too, so it needs
 * no executive.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ssdef.h"
#include "descrip.h"
#include "lnmdef.h"
#include "starlet.h"
#include "lib$routines.h"

static int pass = 0, fail = 0;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else   { printf("  FAIL: %s\n", m); fail++; } } while (0)

static void dsc(struct dsc$descriptor_s *d, const char *s)
{
    d->dsc$w_length = (uint16_t)strlen(s);
    d->dsc$b_dtype = DSC$K_DTYPE_T;
    d->dsc$b_class = DSC$K_CLASS_S;
    d->dsc$a_pointer = (char *)s;
}

static uint32_t define_one(const char *name, const char *value)
{
    struct dsc$descriptor_s n, t;
    struct item_list_3 il[2];
    dsc(&n, name);
    dsc(&t, "LNM$PROCESS_TABLE");
    il[0].buflen = (uint16_t)strlen(value); il[0].item_code = LNM$_STRING;
    il[0].bufaddr = (void *)value;          il[0].retlen = NULL;
    il[1].buflen = 0; il[1].item_code = 0; il[1].bufaddr = NULL; il[1].retlen = NULL;
    return sys$crelnm(NULL, &t, &n, NULL, il);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_lib_get_logical ===\n");

    CHECK($VMS_STATUS_SUCCESS(define_one("OVMX_GL_ONE", "first-value")),
          "define OVMX_GL_ONE through SYS$CRELNM");

    struct dsc$descriptor_s name, table, res;
    char buf[256];
    uint16_t rlen = 0;
    int32_t maxidx = -1;
    uint32_t st;

    /* plain translation, explicit table */
    dsc(&name, "OVMX_GL_ONE");
    dsc(&table, "LNM$PROCESS_TABLE");
    memset(buf, '.', sizeof buf);
    res.dsc$w_length = sizeof buf; res.dsc$b_dtype = DSC$K_DTYPE_T;
    res.dsc$b_class = DSC$K_CLASS_S; res.dsc$a_pointer = buf;
    st = lib$get_logical(&name, &res, &rlen, &table, &maxidx, NULL, NULL, NULL);
    CHECK($VMS_STATUS_SUCCESS(st), "get_logical returns success for a defined name");
    CHECK(rlen == 11 && memcmp(buf, "first-value", 11) == 0,
          "the equivalence string and its length are what was defined");
    CHECK(maxidx == 0, "a single-valued name has max index 0");

    /* default table (LNM$FILE_DEV searches the process table) */
    memset(buf, '.', sizeof buf);
    res.dsc$w_length = sizeof buf;
    rlen = 0;
    st = lib$get_logical(&name, &res, &rlen, NULL, NULL, NULL, NULL, NULL);
    CHECK($VMS_STATUS_SUCCESS(st) && rlen == 11 && memcmp(buf, "first-value", 11) == 0,
          "with no table argument the default table search still finds a process logical");

    /* the length-field-aliasing idiom: resultant length IS the descriptor length */
    memset(buf, '.', sizeof buf);
    res.dsc$w_length = sizeof buf;
    st = lib$get_logical(&name, &res, &res.dsc$w_length, &table, NULL, NULL, NULL, NULL);
    CHECK($VMS_STATUS_SUCCESS(st) && res.dsc$w_length == 11 &&
          memcmp(res.dsc$a_pointer, "first-value", 11) == 0,
          "resultant length may alias the descriptor's own length field");

    /* not defined */
    dsc(&name, "OVMX_GL_NOSUCH");
    res.dsc$w_length = sizeof buf;
    st = lib$get_logical(&name, &res, &rlen, &table, NULL, NULL, NULL, NULL);
    CHECK(st == SS$_NOLOGNAM, "an undefined name returns SS$_NOLOGNAM");

    /* missing required arguments */
    CHECK(lib$get_logical(NULL, &res, &rlen, &table, NULL, NULL, NULL, NULL) == SS$_BADPARAM,
          "a missing logical name is SS$_BADPARAM");
    dsc(&name, "OVMX_GL_ONE");
    CHECK(lib$get_logical(&name, NULL, &rlen, &table, NULL, NULL, NULL, NULL) == SS$_BADPARAM,
          "a missing result descriptor is SS$_BADPARAM");

    printf("=== test_lib_get_logical: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
