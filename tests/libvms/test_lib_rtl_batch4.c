/*
 * test_lib_rtl_batch4.c - Unit tests for the LIB$/OTS$ RTL routines added
 * under vms-801 R2.2 batch 4:
 *
 *   lib$scopy_dxdx                       (string copy: S/D/VS destinations)
 *   lib$reserve_ef                       (reserve a specific event flag)
 *   lib$wait                             (real-time delay, IEEE + VAX floats)
 *   ots$divct_r3 ots$mulct_r3
 *   ots$powctct_r3 ots$powctj_r3         (complex arithmetic, IEEE T)
 *   ots$cnvout_t                         (double -> normalized sci text)
 *   ots$cvt_t_t                          (numeric text -> double)
 *
 * Assertions are grounded in the documented behaviour (OpenVMS RTL LIB$
 * and OTS$ manuals).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <complex.h>
#include <time.h>
#include "lib$routines.h"
#include "ots$routines.h"
#include "libdef.h"
#include "libwaitdef.h"
#include "ssdef.h"
#include "descrip.h"

static int failures = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

/* ------------------------------------------------------------------ */
static void test_scopy_dxdx(void)
{
    printf("Testing lib$scopy_dxdx...\n");

    /* S -> D: dynamic destination is sized to the source length. */
    $DESCRIPTOR(src, "test string");
    struct dsc$descriptor_d dyn = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_D, NULL };
    check(lib$scopy_dxdx(&src, (struct dsc$descriptor_s *)&dyn) == SS$_NORMAL,
          "S->D copy returns SS$_NORMAL");
    check(dyn.dsc$w_length == 11 && dyn.dsc$a_pointer &&
          memcmp(dyn.dsc$a_pointer, "test string", 11) == 0,
          "S->D copy allocated and copied the full source");

    /* D -> VS with truncation: only maxstrlen bytes, CURLEN set, LIB$_STRTRU. */
    struct { uint16_t curlen; char body[4]; } vs_obj = { 0, {0,0,0,0} };
    struct dsc$descriptor_vs vs = { 4, DSC$K_DTYPE_VT, DSC$K_CLASS_VS,
                                    (char *)&vs_obj };
    uint32_t st = lib$scopy_dxdx((struct dsc$descriptor_s *)&dyn,
                                 (struct dsc$descriptor_s *)&vs);
    check(st == LIB$_STRTRU, "D->VS truncating copy returns LIB$_STRTRU");
    check(vs_obj.curlen == 4 && memcmp(vs_obj.body, "test", 4) == 0,
          "D->VS copied maxstrlen bytes and set CURLEN");

    /* S -> S shorter destination: truncation; longer destination: space-fill. */
    char fixed[4];
    struct dsc$descriptor_s fx = { sizeof(fixed), DSC$K_DTYPE_T,
                                   DSC$K_CLASS_S, fixed };
    check(lib$scopy_dxdx(&src, &fx) == LIB$_STRTRU,
          "S->S (dest shorter) returns LIB$_STRTRU");
    check(memcmp(fixed, "test", 4) == 0, "S->S truncated to dest capacity");

    char wide[8];
    struct dsc$descriptor_s wd = { sizeof(wide), DSC$K_DTYPE_T,
                                   DSC$K_CLASS_S, wide };
    $DESCRIPTOR(abc, "abc");
    check(lib$scopy_dxdx(&abc, &wd) == SS$_NORMAL,
          "S->S (dest longer) returns SS$_NORMAL");
    check(memcmp(wide, "abc     ", 8) == 0, "S->S space-filled the remainder");

    lib$sfree1_dd((uint64_t *)&dyn);
}

/* ------------------------------------------------------------------ */
static void test_reserve_ef(void)
{
    printf("Testing lib$reserve_ef...\n");

    uint32_t efn = 40;
    check(lib$reserve_ef(&efn) == SS$_NORMAL,
          "lib$reserve_ef reserves a free flag");
    check(lib$reserve_ef(&efn) == LIB$_EF_ALRRES,
          "lib$reserve_ef on an already-reserved flag returns LIB$_EF_ALRRES");

    /* lib$get_ef must skip the reserved flag. */
    uint32_t got = 0;
    check(lib$get_ef(&got) == SS$_NORMAL && got != 40,
          "lib$get_ef does not hand out the reserved flag");

    uint32_t sysflag = 5;   /* below the allocatable pool -> system-reserved */
    check(lib$reserve_ef(&sysflag) == LIB$_EF_RESSYS,
          "lib$reserve_ef on a system-reserved flag returns LIB$_EF_RESSYS");

    lib$free_ef(&efn);
    lib$free_ef(&got);
}

/* ------------------------------------------------------------------ */
static void test_wait(void)
{
    printf("Testing lib$wait...\n");

    struct timespec t0, t1;
    float secs = 0.10f;             /* 100 ms */
    uint32_t ft = LIB$K_IEEE_S;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    check(lib$wait(&secs, 0, &ft) == SS$_NORMAL,
          "lib$wait (IEEE single) returns SS$_NORMAL");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) +
                     (t1.tv_nsec - t0.tv_nsec) / 1e9;
    check(elapsed >= 0.09, "lib$wait actually waited ~100 ms");

    double d = 0.05;                /* IEEE double path */
    uint32_t ftd = LIB$K_IEEE_T;
    check(lib$wait(&d, 0, &ftd) == SS$_NORMAL, "lib$wait (IEEE double) works");

    /* VAX F_floating decode, short safe wait: VAX F 0.03125 s.
     * 0.03125 = 0.5 * 2^-4 -> E=124 (0x7C), F=0 -> natural bits 0x3E000000,
     * stored on media (16-bit words swapped) as the longword 0x00003E00. */
    uint32_t vaxf = 0x00003E00u;
    uint32_t ftf = LIB$K_VAX_F;
    check(lib$wait(&vaxf, 0, &ftf) == SS$_NORMAL,
          "lib$wait (VAX F_floating) returns SS$_NORMAL");

    float neg = -1.0f;
    check(lib$wait(&neg, 0, &ft) == SS$_BADPARAM,
          "lib$wait rejects a negative interval");
}

/* ------------------------------------------------------------------ */
static void test_complex(void)
{
    printf("Testing ots$divct_r3 / mulct_r3 / powctct_r3 / powctj_r3...\n");

    /* (8+4i)/(1+1i) = 6 - 2i */
    double complex q = ots$divct_r3(8.0, 4.0, 1.0, 1.0);
    check(fabs(creal(q) - 6.0) < 1e-9 && fabs(cimag(q) + 2.0) < 1e-9,
          "ots$divct_r3 (8+4i)/(1+1i) == 6-2i");

    /* (8+4i)*(2+3i) = 4 + 32i */
    double complex m = ots$mulct_r3(8.0, 4.0, 2.0, 3.0);
    check(fabs(creal(m) - 4.0) < 1e-9 && fabs(cimag(m) - 32.0) < 1e-9,
          "ots$mulct_r3 (8+4i)*(2+3i) == 4+32i");

    /* (2+3i)^(1+2i) matches cpow */
    double complex ref = cpow(2.0 + 3.0*I, 1.0 + 2.0*I);
    double complex p = ots$powctct_r3(2.0, 3.0, 1.0, 2.0);
    check(cabs(p - ref) < 1e-9, "ots$powctct_r3 matches cpow");

    /* (2+3i)^2 = -5 + 12i */
    double complex pj = ots$powctj_r3(2.0, 3.0, 2);
    check(fabs(creal(pj) + 5.0) < 1e-9 && fabs(cimag(pj) - 12.0) < 1e-9,
          "ots$powctj_r3 (2+3i)^2 == -5+12i");
}

/* ------------------------------------------------------------------ */
static void test_cnvout(void)
{
    printf("Testing ots$cnvout_t...\n");

    double value = 2.71828182;
    char out[16];
    struct dsc$descriptor_s od = { sizeof(out), DSC$K_DTYPE_T,
                                   DSC$K_CLASS_S, out };
    check(ots$cnvout_t(&value, &od, 9) == SS$_NORMAL,
          "ots$cnvout_t returns SS$_NORMAL");
    check(od.dsc$w_length == 15 &&
          memcmp(out, "0.271828182E+01", 15) == 0,
          "ots$cnvout_t formats 2.71828182 as 0.271828182E+01");

    double neg = -1.5;
    char nb[16];
    struct dsc$descriptor_s nd = { sizeof(nb), DSC$K_DTYPE_T,
                                   DSC$K_CLASS_S, nb };
    check(ots$cnvout_t(&neg, &nd, 3) == SS$_NORMAL &&
          memcmp(nb, "-0.150E+01", nd.dsc$w_length) == 0,
          "ots$cnvout_t formats -1.5 (precision 3) as -0.150E+01");
}

/* ------------------------------------------------------------------ */
static void test_cvt_t(void)
{
    printf("Testing ots$cvt_t_t...\n");

    /* VMS exponent introduced by sign with no 'E': 0.12456789+3. */
    $DESCRIPTOR(in, "0.12456789+3");
    double v = 0.0;
    check(ots$cvt_t_t(&in, &v, 0, 0, 0) == SS$_NORMAL,
          "ots$cvt_t_t returns SS$_NORMAL");
    check(fabs(v - 124.56789) < 1e-6,
          "ots$cvt_t_t parses \"0.12456789+3\" as 124.56789");

    $DESCRIPTOR(in2, "  -12.5E2 ");
    double v2 = 0.0;
    check(ots$cvt_t_t(&in2, &v2, 0, 0, 0) == SS$_NORMAL &&
          fabs(v2 + 1250.0) < 1e-6,
          "ots$cvt_t_t parses \"-12.5E2\" (blanks ignored) as -1250");
}

/* ------------------------------------------------------------------ */
int main(void)
{
    printf("=== LIB$/OTS$ RTL batch 4 unit tests (vms-801 R2.2) ===\n\n");

    test_scopy_dxdx();
    test_reserve_ef();
    test_wait();
    test_complex();
    test_cnvout();
    test_cvt_t();

    printf("\n=== %s (%d failure%s) ===\n",
           failures == 0 ? "PASS" : "FAIL",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
