/*
 * test_lib_rtl_batch2.c - Unit tests for the LIB$/OTS$ RTL routines added
 * under vms-801 R2.2 batch 2:
 *
 *   ots$cvt_l_tu ots$cvt_l_tb ots$cvt_l_tl                (int -> text)
 *   ots$cvt_tu_l ots$cvt_to_l ots$cvt_tz_l ots$cvt_tb_l   (text -> int)
 *   ots$cvt_tl_l                                          (logical text)
 *   ots$move3 ots$move5 ots$powjj                         (move / power)
 *   lib$add_times lib$sub_times lib$cvt_vectim            (date/time)
 *   lib$scopy_r_dx lib$cvt_dx_dx                          (string/desc copy)
 *   lib$movtc lib$tra_asc_ebc lib$tra_ebc_asc             (translation)
 *   lib$currency lib$digit_sep lib$radix_point            (locale)
 *   (plus sys$bintim's new delta-time parsing)
 *
 * Assertions are grounded in the documented behaviour (OpenVMS RTL
 * OTS$/LIB$ manuals) and self-consistent corpus values
 * ("11000000111001"b == "30071"o == "3039"h == 12345).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "lib$routines.h"
#include "ots$routines.h"
#include "libdef.h"
#include "ssdef.h"
#include "stsdef.h"
#include "descrip.h"
#include "starlet.h"

extern char LIB$AB_UPCASE;

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
static void test_powjj(void)
{
    printf("Testing ots$powjj...\n");
    check(ots$powjj(8, 3) == 512, "8**3 == 512");
    check(ots$powjj(2, 10) == 1024, "2**10 == 1024");
    check(ots$powjj(5, 0) == 1, "5**0 == 1");
    check(ots$powjj(7, 1) == 7, "7**1 == 7");
    check(ots$powjj(2, -1) == 0, "2**-1 == 0 (integer)");
    check(ots$powjj(-1, 3) == -1, "(-1)**3 == -1");
    check(ots$powjj(-2, 3) == -8, "(-2)**3 == -8");
}

/* ------------------------------------------------------------------ */
static void test_int_to_text(void)
{
    printf("Testing ots$cvt_l_tu / _tb / _tl...\n");
    char b[32];
    struct dsc$descriptor_s d = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_S, b };

    uint32_t v = 12345;
    d.dsc$w_length = 5;
    check(ots$cvt_l_tu(&v, &d, 0, 4) == SS$_NORMAL && memcmp(b, "12345", 5) == 0,
          "cvt_l_tu 12345 -> \"12345\"");

    d.dsc$w_length = 8;
    ots$cvt_l_tu(&v, &d, 0, 4);
    check(memcmp(b, "   12345", 8) == 0, "cvt_l_tu right-justified in field");

    d.dsc$w_length = 14;
    ots$cvt_l_tb(&v, &d, 0, 4);
    check(memcmp(b, "11000000111001", 14) == 0, "cvt_l_tb 12345 -> binary");

    int32_t sv = -54321;
    d.dsc$w_length = 6;
    ots$cvt_l_tl(&sv, &d);
    check(memcmp(b, "-54321", 6) == 0, "cvt_l_tl -54321 -> \"-54321\"");

    d.dsc$w_length = 6;      /* min_digits leading-zero pad */
    ots$cvt_l_tu(&v, &d, 6, 4);
    check(memcmp(b, "012345", 6) == 0, "cvt_l_tu min_digits pads with zeros");
}

/* ------------------------------------------------------------------ */
static void test_text_to_int(void)
{
    printf("Testing ots$cvt_tu_l / _to_l / _tz_l / _tb_l / _tl_l...\n");
    uint32_t out = 0;

    struct dsc$descriptor_s dec = { 5, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"12345" };
    check(ots$cvt_tu_l(&dec, &out, 4, 0) == SS$_NORMAL && out == 12345,
          "cvt_tu_l \"12345\" -> 12345");

    struct dsc$descriptor_s oct = { 5, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"30071" };
    check(ots$cvt_to_l(&oct, &out, 4, 0) == SS$_NORMAL && out == 12345,
          "cvt_to_l \"30071\"(oct) -> 12345");

    struct dsc$descriptor_s hex = { 4, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"3039" };
    check(ots$cvt_tz_l(&hex, &out, 4, 0) == SS$_NORMAL && out == 12345,
          "cvt_tz_l \"3039\"(hex) -> 12345");

    struct dsc$descriptor_s bin = { 14, DSC$K_DTYPE_T, DSC$K_CLASS_S,
                                    (char *)"11000000111001" };
    check(ots$cvt_tb_l(&bin, &out, 4, 0) == SS$_NORMAL && out == 12345,
          "cvt_tb_l \"11000000111001\"(bin) -> 12345");

    struct dsc$descriptor_s neg = { 6, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"-54321" };
    check(ots$cvt_tu_l(&neg, &out, 4, 0) == SS$_NORMAL && (int32_t)out == -54321,
          "cvt_tu_l \"-54321\" -> -54321");

    struct dsc$descriptor_s tru = { 6, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)".True." };
    struct dsc$descriptor_s fls = { 7, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)".False." };
    check(ots$cvt_tl_l(&tru, &out, 4, 0) == SS$_NORMAL && (out & 1u),
          "cvt_tl_l \".True.\" -> true");
    check(ots$cvt_tl_l(&fls, &out, 4, 0) == SS$_NORMAL && out == 0,
          "cvt_tl_l \".False.\" -> false");

    struct dsc$descriptor_s bad = { 3, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"9AB" };
    check(!$VMS_STATUS_SUCCESS(ots$cvt_to_l(&bad, &out, 4, 0)),
          "cvt_to_l rejects non-octal digits");
}

/* ------------------------------------------------------------------ */
static void test_move(void)
{
    printf("Testing ots$move3 / ots$move5...\n");
    char src[] = "ABCDEFGH";
    char dst[16];

    memset(dst, '?', sizeof(dst));
    ots$move3(8, src, dst);
    check(memcmp(dst, "ABCDEFGH", 8) == 0, "move3 copies 8 bytes");

    memset(dst, '?', sizeof(dst));
    ots$move5(8, src, '.', 12, dst);
    check(memcmp(dst, "ABCDEFGH....", 12) == 0, "move5 copies then fills");

    memset(dst, '?', sizeof(dst));
    ots$move5(8, src, '.', 4, dst);
    check(memcmp(dst, "ABCD", 4) == 0 && dst[4] == '?', "move5 truncates to destlen");
}

/* ------------------------------------------------------------------ */
static void test_datetime(void)
{
    printf("Testing lib$cvt_vectim / lib$add_times / lib$sub_times...\n");

    uint16_t vec[7] = { 2003, 2, 1, 3, 4, 5, 6 };
    uint64_t t = 0;
    check(lib$cvt_vectim(vec, &t) == SS$_NORMAL, "cvt_vectim returns SS$_NORMAL");
    uint16_t nt[7];
    sys$numtim(nt, &t);
    check(nt[0] == 2003 && nt[1] == 2 && nt[2] == 1 && nt[3] == 3 &&
          nt[4] == 4 && nt[5] == 5 && nt[6] == 6, "cvt_vectim round-trips via numtim");

    /* Delta-time parsing was added to sys$bintim for these */
    uint64_t abs1 = 0, delta = 0, sum = 0;
    struct dsc$descriptor_s abs1_d = { 23, DSC$K_DTYPE_T, DSC$K_CLASS_S,
                                       (char *)"25-Sep-2003 19:00:00.00" };
    struct dsc$descriptor_s delta_d = { 13, DSC$K_DTYPE_T, DSC$K_CLASS_S,
                                        (char *)"1 01:00:00.00" };
    check(sys$bintim(&abs1_d, &abs1) == SS$_NORMAL, "bintim parses absolute time");
    check(sys$bintim(&delta_d, &delta) == SS$_NORMAL, "bintim parses delta time");
    check((int64_t)delta < 0, "delta time stored negative");
    check((int64_t)delta == -(int64_t)(25ULL * 3600ULL * 10000000ULL),
          "delta magnitude = 25 hours");

    check(lib$add_times(&abs1, &delta, &sum) == SS$_NORMAL, "add_times returns SS$_NORMAL");
    sys$numtim(nt, &sum);
    check(nt[2] == 26 && nt[1] == 9 && nt[0] == 2003 && nt[3] == 20 &&
          nt[4] == 0 && nt[5] == 0,
          "add_times abs+delta = 26-Sep-2003 20:00:00");

    /* absolute - absolute -> delta (negative magnitude) */
    uint64_t diff = 0;
    check(lib$sub_times(&sum, &abs1, &diff) == SS$_NORMAL, "sub_times abs-abs OK");
    check((int64_t)diff == -(int64_t)(25ULL * 3600ULL * 10000000ULL),
          "sub_times abs-abs = -25h delta");

    /* absolute - delta -> earlier absolute */
    uint64_t earlier = 0;
    check(lib$sub_times(&sum, &delta, &earlier) == SS$_NORMAL, "sub_times abs-delta OK");
    sys$numtim(nt, &earlier);
    check(nt[2] == 25 && nt[3] == 19, "sub_times abs-delta = 25-Sep 19:00");

    /* abs - abs with time1 < time2 is a negative time */
    uint64_t neg = 0;
    check(lib$sub_times(&abs1, &sum, &neg) == LIB$_NEGTIM, "sub_times negative -> LIB$_NEGTIM");
}

/* ------------------------------------------------------------------ */
static void test_string_copy(void)
{
    printf("Testing lib$scopy_r_dx / lib$cvt_dx_dx...\n");

    uint16_t len = 5;
    struct dsc$descriptor_d out = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_D, NULL };
    check(lib$scopy_r_dx(&len, "Hello", (struct dsc$descriptor_s *)&out) == SS$_NORMAL,
          "scopy_r_dx returns SS$_NORMAL");
    check(out.dsc$w_length == 5 && memcmp(out.dsc$a_pointer, "Hello", 5) == 0,
          "scopy_r_dx allocated + copied");
    lib$sfree1_dd((uint64_t *)&out);

    uint64_t q = 289575739823571ULL;
    struct dsc$descriptor_s q_d = { sizeof(q), DSC$K_DTYPE_Q, DSC$K_CLASS_S, (char *)&q };
    struct dsc$descriptor_d out2 = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_D, NULL };
    uint16_t rl = 0;
    check(lib$cvt_dx_dx(&q_d, &out2, &rl) == SS$_NORMAL, "cvt_dx_dx Q->T OK");
    check(rl == 15 && memcmp(out2.dsc$a_pointer, "289575739823571", 15) == 0,
          "cvt_dx_dx formatted quadword decimal");
    lib$sfree1_dd((uint64_t *)&out2);
}

/* ------------------------------------------------------------------ */
static void test_translate(void)
{
    printf("Testing lib$movtc / lib$tra_asc_ebc / lib$tra_ebc_asc...\n");

    char outbuf[10];
    struct dsc$descriptor_s src = { 6, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"abcXYz" };
    struct dsc$descriptor_s fill = { 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)" " };
    struct dsc$descriptor_s tbl = { 256, DSC$K_DTYPE_T, DSC$K_CLASS_S, &LIB$AB_UPCASE };
    struct dsc$descriptor_s dst = { 10, DSC$K_DTYPE_T, DSC$K_CLASS_S, outbuf };
    check(lib$movtc(&src, &fill, &tbl, &dst) == SS$_NORMAL, "movtc returns SS$_NORMAL");
    check(memcmp(outbuf, "ABCXYZ", 6) == 0, "movtc uppercased via table");
    check(outbuf[6] == ' ' && outbuf[9] == ' ', "movtc fills excess with fill char");

    const char *msg = "Hello, World! 0123";
    uint16_t n = (uint16_t)strlen(msg);
    char ebc[64], back[64];
    struct dsc$descriptor_s a_d = { n, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)msg };
    struct dsc$descriptor_s e_d = { n, DSC$K_DTYPE_T, DSC$K_CLASS_S, ebc };
    struct dsc$descriptor_s b_d = { n, DSC$K_DTYPE_T, DSC$K_CLASS_S, back };
    check(lib$tra_asc_ebc(&a_d, &e_d) == SS$_NORMAL, "tra_asc_ebc OK");
    check((unsigned char)ebc[0] != (unsigned char)msg[0], "EBCDIC differs from ASCII");
    check(lib$tra_ebc_asc(&e_d, &b_d) == SS$_NORMAL, "tra_ebc_asc OK");
    check(memcmp(back, msg, n) == 0, "ASCII->EBCDIC->ASCII round trip");
}

/* ------------------------------------------------------------------ */
static void test_locale(void)
{
    printf("Testing lib$currency / lib$digit_sep / lib$radix_point...\n");
    char b[16];
    struct dsc$descriptor_s d = { sizeof(b), DSC$K_DTYPE_T, DSC$K_CLASS_S, b };
    uint16_t rl = 0;

    d.dsc$w_length = sizeof(b);
    check(lib$currency(&d, &rl) == SS$_NORMAL && rl == 1 && b[0] == '$',
          "currency -> \"$\"");
    d.dsc$w_length = sizeof(b);
    check(lib$digit_sep(&d, &rl) == SS$_NORMAL && rl == 1 && b[0] == ',',
          "digit_sep -> \",\"");
    d.dsc$w_length = sizeof(b);
    check(lib$radix_point(&d, &rl) == SS$_NORMAL && rl == 1 && b[0] == '.',
          "radix_point -> \".\"");
}

/* ------------------------------------------------------------------ */
int main(void)
{
    printf("=== LIB$/OTS$ RTL batch 2 (vms-801 R2.2) unit tests ===\n");
    test_powjj();
    test_int_to_text();
    test_text_to_int();
    test_move();
    test_datetime();
    test_string_copy();
    test_translate();
    test_locale();

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
