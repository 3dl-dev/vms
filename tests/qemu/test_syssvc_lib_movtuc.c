/*
 * test_syssvc_lib_movtuc.c - LIB$MOVTUC, move translated until character, in the booted
 * guest (the RTL Library (LIB$) Manual; the VAX MOVTUC instruction it jackets):
 *
 *   - the return is the 1-based source position of the character that TRANSLATED to the
 *     stop character, 0 when none did;
 *   - the comparison is on the translated character, not the source byte, and the stop
 *     character is never written;
 *   - with a fill character the rest of the destination is padded (untranslated); without
 *     one the rest of the destination is left unchanged.
 *
 * No /dev/vms -> honest SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vms_kif.h"
#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"

#define EXIT_SKIP 77

extern char LIB$AB_UPCASE;

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

static struct dsc$descriptor_s text(const char *s)
{
    struct dsc$descriptor_s d = { (uint16_t)strlen(s), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)s };
    return d;
}

int main(void)
{
    char out[10];
    struct dsc$descriptor_s tbl = { 256, DSC$K_DTYPE_T, DSC$K_CLASS_S, &LIB$AB_UPCASE };
    struct dsc$descriptor_s dst = { sizeof out, DSC$K_DTYPE_T, DSC$K_CLASS_S, out };
    struct dsc$descriptor_s fill = text("*");

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_lib_movtuc: LIB$MOVTUC ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_lib_movtuc: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    struct dsc$descriptor_s src = text("abc.de"), dot = text(".");
    memset(out, '#', sizeof out);
    check(lib$movtuc(&src, &dot, &tbl, &dst, &fill) == 4,
          "LIB$MOVTUC returns the stop character's 1-based source position");
    check(memcmp(out, "ABC*******", 10) == 0,
          "the prefix is translated, the stop character not written, the rest filled");

    struct dsc$descriptor_s src2 = text("xyqzw"), q = text("Q");
    memset(out, '#', sizeof out);
    /* negctl: lib-movtuc-stop-untranslated */
    check(lib$movtuc(&src2, &q, &tbl, &dst, &fill) == 3,
          "the stop test is on the TRANSLATED character ('q' upcases to the stop 'Q')");
    check(memcmp(out, "XY********", 10) == 0,
          "only the translated prefix before the stop character is written");

    struct dsc$descriptor_s bang = text("!");
    memset(out, '#', sizeof out);
    check(lib$movtuc(&src2, &bang, &tbl, &dst, &fill) == 0,
          "0 when no character translates to the stop character");
    check(memcmp(out, "XYQZW*****", 10) == 0,
          "with no stop the whole source is translated and padded to the destination length");

    memset(out, '#', sizeof out);
    check(lib$movtuc(&src, &dot, &tbl, &dst, NULL) == 4 && memcmp(out, "ABC#######", 10) == 0,
          "with no fill character the rest of the destination is unchanged");

    printf("=== test_syssvc_lib_movtuc: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
