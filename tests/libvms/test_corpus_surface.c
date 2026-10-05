/*
 * test_corpus_surface.c - routines and conventions added for the R2 corpus
 * lane (vms-44a): omitted-trailing-argument padding, the OTS$ string copy
 * routines, $GET_ENTROPY, $LCKPAG/$ULKPAG, $GETTIM_PREC, and the honest refusals
 * of the extended $SETPRI / $DELPRC / $CREPRC argument lists.
 *
 * Every expectation below is the documented behavior of the VMS routine (OpenVMS
 * RTL Library OTS$ chapter; System Services Reference Manual), not whatever the
 * implementation happens to return.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include "starlet.h"
#include "lib$routines.h"
#include "ots$routines.h"
#include "descrip.h"
#include "ssdef.h"
#include "va_rangedef.h"
#include "ovmx_optargs.h"

static int failures = 0;
static void check(int cond, const char *name)
{
    printf("  %s: %s\n", cond ? "OK" : "FAIL", name);
    if (!cond) failures++;
}

/* ---- OVMX_PAD_n: omitted trailing arguments arrive as zero ---- */
static int rec_a, rec_b, rec_c, rec_d;
static int rec4(int a, int b, int c, int d)
{
    rec_a = a; rec_b = b; rec_c = c; rec_d = d;
    return a + b + c + d;
}
#define rec4(...) OVMX_PAD_4(rec4, __VA_ARGS__)

static void test_pad(void)
{
    printf("omitted trailing arguments\n");
    rec_b = rec_c = rec_d = 99;
    check(rec4(7) == 7 && rec_a == 7 && rec_b == 0 && rec_c == 0 && rec_d == 0,
          "one of four given: the other three are zero");
    rec_c = rec_d = 99;
    check(rec4(1, 2) == 3 && rec_c == 0 && rec_d == 0, "two of four given");
    check(rec4(1, 2, 3, 4) == 10, "all four given: passed through unchanged");
}

/* ---- OTS$SCOPY_DXDX / OTS$SCOPY_R_DX: return the untransferred byte count ---- */
static void test_ots_copy(void)
{
    printf("OTS$SCOPY_*\n");
    static const char text[] = "Software comes from heaven";   /* 26 bytes */
    struct dsc$descriptor_s src = { 26, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)text };

    char exact[26], more[35], less[17];
    struct dsc$descriptor_s d = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_S, NULL };

    d.dsc$w_length = sizeof exact; d.dsc$a_pointer = exact;
    check(ots$scopy_dxdx(&src, &d) == 0 && memcmp(exact, text, 26) == 0, "exact fit: 0 not copied");

    memset(more, '#', sizeof more);
    d.dsc$w_length = sizeof more; d.dsc$a_pointer = more;
    check(ots$scopy_dxdx(&src, &d) == 0 && memcmp(more, text, 26) == 0
          && more[26] == ' ' && more[34] == ' ', "longer fixed destination: space filled, 0 not copied");

    d.dsc$w_length = sizeof less; d.dsc$a_pointer = less;
    check(ots$scopy_dxdx(&src, &d) == 9 && memcmp(less, text, 17) == 0, "shorter destination: 9 bytes not copied");

    struct dsc$descriptor_d dyn = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_D, NULL };
    check(ots$scopy_dxdx(&src, (struct dsc$descriptor_s *)&dyn) == 0
          && dyn.dsc$w_length == 26 && memcmp(dyn.dsc$a_pointer, text, 26) == 0,
          "dynamic destination is resized to the source");
    ots$sfree1_dd((uint64_t *)&dyn);
    check(dyn.dsc$a_pointer == NULL && dyn.dsc$w_length == 0, "OTS$SFREE1_DD empties the descriptor");

    struct dsc$descriptor_d arr[3];
    memset(arr, 0, sizeof arr);
    for (int i = 0; i < 3; i++) {
        arr[i].dsc$b_dtype = DSC$K_DTYPE_T; arr[i].dsc$b_class = DSC$K_CLASS_D;
        check(ots$sget1_dd(64, (uint64_t *)&arr[i]) == SS$_NORMAL && arr[i].dsc$w_length == 64,
              "OTS$SGET1_DD allocates the requested length");
    }
    check(ots$scopy_r_dx(5, "hello", (struct dsc$descriptor_s *)&arr[1]) == 0
          && arr[1].dsc$w_length == 5 && memcmp(arr[1].dsc$a_pointer, "hello", 5) == 0,
          "OTS$SCOPY_R_DX into a dynamic descriptor");
    ots$sfreen_dd(3, (uint64_t *)&arr[0]);
    check(arr[0].dsc$a_pointer == NULL && arr[1].dsc$a_pointer == NULL && arr[2].dsc$a_pointer == NULL,
          "OTS$SFREEN_DD frees every descriptor in the array");
}

static void test_system_services(void)
{
    printf("system services\n");
    unsigned char buf[64], zero[64];
    memset(buf, 0, sizeof buf); memset(zero, 0, sizeof zero);
    check(sys$get_entropy(buf, sizeof buf) == SS$_NORMAL && memcmp(buf, zero, sizeof buf) != 0,
          "$GET_ENTROPY fills the buffer with non-zero bytes");
    check(sys$get_entropy(NULL, 8) == SS$_ACCVIO, "$GET_ENTROPY on a null buffer is ACCVIO");

    uint64_t t0 = 0, t1 = 0;
    check(sys$gettim_prec(&t0) == SS$_NORMAL && t0 > 0x007C95674BEB4000ULL, "$GETTIM_PREC returns a post-1970 VMS time");
    check(sys$gettim(&t1) == SS$_NORMAL && t1 >= t0 && t1 - t0 < 100000000ULL, "$GETTIM_PREC agrees with $GETTIM");

    /* $LCKPAG/$ULKPAG report the page-rounded range they locked. */
    long pg = sysconf(_SC_PAGESIZE);
    char *m = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m != MAP_FAILED) {
        VA_RANGE in = { m + 10, m + 20 }, out = { 0, 0 };
        uint32_t st = sys$lckpag(&in, &out, 0);
        if (st == SS$_NORMAL) {
            check((char *)out.va_range$ps_start_va == m && (char *)out.va_range$ps_end_va == m + pg - 1,
                  "$LCKPAG returns the page-rounded range");
            VA_RANGE un = { 0, 0 };
            check(sys$ulkpag(&out, &un, 0) == SS$_NORMAL && un.va_range$ps_start_va == m, "$ULKPAG releases it");
        } else {
            check(st == SS$_NOPRIV, "$LCKPAG without the lock right is NOPRIV, never a faked lock");
        }
        munmap(m, 2 * pg);
    }
    VA_RANGE bad = { (void *)200, (void *)100 };
    check(sys$lckpag(&bad, NULL, 0) == SS$_BADPARAM, "$LCKPAG with end < start is BADPARAM");

    /* Extended argument lists are refused honestly, not silently dropped. */
    check(sys$setpri(0, 0, 10, 0, 2 /* schedpol */, 0) == SS$_BADPARAM, "$SETPRI refuses a scheduling-policy change");
    uint16_t item[4] = { 4, 1, 0, 0 };
    check(sys$delprc(0, 0, item) == SS$_BADPARAM, "$DELPRC refuses a non-empty item list");
    struct dsc$descriptor_s node = { 4, DSC$K_DTYPE_T, DSC$K_CLASS_S, "NODE" };
    struct dsc$descriptor_s img = { 7, DSC$K_DTYPE_T, DSC$K_CLASS_S, "SYS.EXE" };
    check(sys$creprc(0, &img, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, &node) == SS$_BADPARAM,
          "$CREPRC refuses a remote-node request");
}

int main(void)
{
    test_pad();
    test_ots_copy();
    test_system_services();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
