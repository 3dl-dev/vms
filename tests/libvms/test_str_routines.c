/*
 * test_str_routines.c - Unit tests for STR$ string RTL
 *
 * Tests str$copy_dx, str$concat, str$compare, str$trim, str$upcase,
 * str$find_first_substring, str$match_wild using both static (CLASS_S)
 * and dynamic (CLASS_D) descriptors.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "descrip.h"
#include "str$routines.h"
#include "ssdef.h"

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

/* Helper: make a static descriptor from a C string */
static struct dsc$descriptor_s make_s(const char *s)
{
    struct dsc$descriptor_s d;
    vms_init_descriptor(&d, s, (uint16_t)strlen(s));
    return d;
}

/* Helper: extract C string from descriptor (null-terminates into buf) */
static const char *desc_str(const struct dsc$descriptor_s *d, char *buf, size_t bufsz)
{
    return vms_desc_to_cstr(d, buf, bufsz);
}

/* ------------------------------------------------------------------ */
/* str$copy_dx — static → static                                       */
/* ------------------------------------------------------------------ */
static void test_copy_dx_static(void)
{
    printf("Testing str$copy_dx (static to static)...\n");
    char srcbuf[] = "HELLO";
    char dstbuf[16];
    memset(dstbuf, ' ', sizeof(dstbuf));

    struct dsc$descriptor_s src = make_s(srcbuf);
    struct dsc$descriptor_s dst;
    dst.dsc$w_length  = sizeof(dstbuf);
    dst.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst.dsc$b_class   = DSC$K_CLASS_S;
    dst.dsc$a_pointer = dstbuf;

    uint32_t st = str$copy_dx(&dst, &src);
    check(st == SS$_NORMAL, "copy_dx static returns SS$_NORMAL");
    check(memcmp(dstbuf, "HELLO", 5) == 0, "copy_dx: content copied");
}

/* ------------------------------------------------------------------ */
/* str$copy_dx — static → dynamic                                      */
/* ------------------------------------------------------------------ */
static void test_copy_dx_dynamic(void)
{
    printf("Testing str$copy_dx (static to dynamic)...\n");
    char srcbuf[] = "OPENVMS";
    struct dsc$descriptor_s src = make_s(srcbuf);

    struct dsc$descriptor_d dst;
    dst.dsc$w_length  = 0;
    dst.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst.dsc$b_class   = DSC$K_CLASS_D;
    dst.dsc$a_pointer = NULL;

    uint32_t st = str$copy_dx((struct dsc$descriptor_s *)&dst, &src);
    check(st == SS$_NORMAL, "copy_dx dynamic returns SS$_NORMAL");
    check(dst.dsc$w_length == 7, "copy_dx dynamic: length = 7");
    check(dst.dsc$a_pointer != NULL, "copy_dx dynamic: pointer allocated");
    check(memcmp(dst.dsc$a_pointer, "OPENVMS", 7) == 0, "copy_dx dynamic: content");

    str$free1_dx(&dst);
    check(dst.dsc$a_pointer == NULL, "free1_dx clears pointer");
    check(dst.dsc$w_length == 0, "free1_dx clears length");
}

/* ------------------------------------------------------------------ */
/* str$concat                                                          */
/* ------------------------------------------------------------------ */
static void test_concat(void)
{
    printf("Testing str$concat...\n");
    struct dsc$descriptor_s s1 = make_s("HELLO");
    struct dsc$descriptor_s s2 = make_s(" WORLD");

    struct dsc$descriptor_d dst;
    dst.dsc$w_length  = 0;
    dst.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst.dsc$b_class   = DSC$K_CLASS_D;
    dst.dsc$a_pointer = NULL;

    uint32_t st = str$concat((struct dsc$descriptor_s *)&dst, &s1, &s2);
    check(st == SS$_NORMAL, "concat returns SS$_NORMAL");
    check(dst.dsc$w_length == 11, "concat: length = 11");
    check(memcmp(dst.dsc$a_pointer, "HELLO WORLD", 11) == 0, "concat: content");

    str$free1_dx(&dst);

    /* Concat with empty string */
    struct dsc$descriptor_s empty = make_s("");
    struct dsc$descriptor_d dst2;
    dst2.dsc$w_length  = 0;
    dst2.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst2.dsc$b_class   = DSC$K_CLASS_D;
    dst2.dsc$a_pointer = NULL;

    st = str$concat((struct dsc$descriptor_s *)&dst2, &s1, &empty);
    check(st == SS$_NORMAL, "concat with empty returns SS$_NORMAL");
    check(dst2.dsc$w_length == 5, "concat with empty: length = 5");
    check(memcmp(dst2.dsc$a_pointer, "HELLO", 5) == 0, "concat with empty: content");

    str$free1_dx(&dst2);
}

/* ------------------------------------------------------------------ */
/* str$compare                                                         */
/* ------------------------------------------------------------------ */
static void test_compare(void)
{
    printf("Testing str$compare...\n");
    struct dsc$descriptor_s a = make_s("ABC");
    struct dsc$descriptor_s b = make_s("ABC");
    struct dsc$descriptor_s c = make_s("ABD");
    struct dsc$descriptor_s d = make_s("AB");

    check(str$compare(&a, &b) == 0,  "compare equal strings = 0");
    check(str$compare(&a, &c) < 0,   "compare ABC < ABD");
    check(str$compare(&c, &a) > 0,   "compare ABD > ABC");
    check(str$compare(&d, &a) < 0,   "compare AB < ABC (shorter)");
    check(str$compare(&a, &d) > 0,   "compare ABC > AB (longer)");
}

/* ------------------------------------------------------------------ */
/* str$case_blind_compare                                              */
/* ------------------------------------------------------------------ */
static void test_case_blind_compare(void)
{
    printf("Testing str$case_blind_compare...\n");
    struct dsc$descriptor_s lower = make_s("hello");
    struct dsc$descriptor_s upper = make_s("HELLO");
    struct dsc$descriptor_s mixed = make_s("Hello");

    check(str$case_blind_compare(&lower, &upper) == 0, "case blind: hello == HELLO");
    check(str$case_blind_compare(&lower, &mixed) == 0, "case blind: hello == Hello");
}

/* ------------------------------------------------------------------ */
/* str$trim                                                            */
/* ------------------------------------------------------------------ */
static void test_trim(void)
{
    printf("Testing str$trim...\n");
    char srcbuf[] = "HELLO   ";  /* 3 trailing spaces */
    struct dsc$descriptor_s src;
    vms_init_descriptor(&src, srcbuf, 8);  /* length includes spaces */

    struct dsc$descriptor_d dst;
    dst.dsc$w_length  = 0;
    dst.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst.dsc$b_class   = DSC$K_CLASS_D;
    dst.dsc$a_pointer = NULL;

    uint16_t result_len = 0;
    uint32_t st = str$trim((struct dsc$descriptor_s *)&dst, &src, &result_len);
    check(st == SS$_NORMAL, "trim returns SS$_NORMAL");
    check(result_len == 5, "trim: result_len = 5");
    check(dst.dsc$w_length == 5, "trim: descriptor length = 5");
    check(memcmp(dst.dsc$a_pointer, "HELLO", 5) == 0, "trim: content is HELLO");

    str$free1_dx(&dst);

    /* All spaces → empty */
    char spbuf[] = "   ";
    struct dsc$descriptor_s spaces;
    vms_init_descriptor(&spaces, spbuf, 3);

    struct dsc$descriptor_d dst2;
    dst2.dsc$w_length  = 0;
    dst2.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst2.dsc$b_class   = DSC$K_CLASS_D;
    dst2.dsc$a_pointer = NULL;

    uint16_t rlen2 = 99;
    st = str$trim((struct dsc$descriptor_s *)&dst2, &spaces, &rlen2);
    check(st == SS$_NORMAL, "trim all-spaces returns SS$_NORMAL");
    check(rlen2 == 0, "trim all-spaces: result_len = 0");

    str$free1_dx(&dst2);
}

/* ------------------------------------------------------------------ */
/* str$upcase                                                          */
/* ------------------------------------------------------------------ */
static void test_upcase(void)
{
    printf("Testing str$upcase...\n");
    char srcbuf[] = "hello world";
    struct dsc$descriptor_s src = make_s(srcbuf);

    struct dsc$descriptor_d dst;
    dst.dsc$w_length  = 0;
    dst.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst.dsc$b_class   = DSC$K_CLASS_D;
    dst.dsc$a_pointer = NULL;

    uint32_t st = str$upcase((struct dsc$descriptor_s *)&dst, &src);
    check(st == SS$_NORMAL, "upcase returns SS$_NORMAL");
    check(dst.dsc$w_length == 11, "upcase: length preserved");
    check(memcmp(dst.dsc$a_pointer, "HELLO WORLD", 11) == 0, "upcase: content");

    str$free1_dx(&dst);

    /* Already uppercase */
    struct dsc$descriptor_s upper_src = make_s("SYSTEM");
    struct dsc$descriptor_d dst2;
    dst2.dsc$w_length  = 0;
    dst2.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst2.dsc$b_class   = DSC$K_CLASS_D;
    dst2.dsc$a_pointer = NULL;

    st = str$upcase((struct dsc$descriptor_s *)&dst2, &upper_src);
    check(st == SS$_NORMAL, "upcase already-upper returns SS$_NORMAL");
    check(memcmp(dst2.dsc$a_pointer, "SYSTEM", 6) == 0, "upcase already-upper: unchanged");

    str$free1_dx(&dst2);
}

/* ------------------------------------------------------------------ */
/* str$find_first_substring                                            */
/* ------------------------------------------------------------------ */
static void test_find_first_substring(void)
{
    printf("Testing str$find_first_substring...\n");
    struct dsc$descriptor_s src = make_s("HELLO WORLD");
    struct dsc$descriptor_s sub = make_s("WORLD");
    uint32_t index = 0, sub_index = 0;

    uint32_t st = str$find_first_substring(&src, &index, &sub_index, &sub);
    check(st == SS$_NORMAL, "find_first_substring returns SS$_NORMAL");
    check(index == 7, "find_first_substring: index = 7 (1-based)");
    check(sub_index == 1, "find_first_substring: sub_index = 1");

    /* Not found */
    struct dsc$descriptor_s notfound = make_s("XYZ");
    index = 99; sub_index = 99;
    st = str$find_first_substring(&src, &index, &sub_index, &notfound);
    check(st == SS$_NORMAL, "find_first_substring not-found returns SS$_NORMAL");
    check(index == 0, "find_first_substring: index = 0 when not found");
}

/* ------------------------------------------------------------------ */
/* str$match_wild                                                       */
/* ------------------------------------------------------------------ */
static void test_match_wild(void)
{
    printf("Testing str$match_wild...\n");
    struct dsc$descriptor_s cand, pattern;

    cand    = make_s("HELLO.TXT");
    pattern = make_s("*.TXT");
    check(str$match_wild(&cand, &pattern) == STR$_MATCH,
          "match_wild *.TXT matches HELLO.TXT");

    cand    = make_s("HELLO.EXE");
    check(str$match_wild(&cand, &pattern) == STR$_NOMATCH,
          "match_wild *.TXT does not match HELLO.EXE");

    cand    = make_s("HELLO");
    pattern = make_s("HEL%O");
    check(str$match_wild(&cand, &pattern) == STR$_MATCH,
          "match_wild HEL%O matches HELLO");

    cand    = make_s("HELXO");
    check(str$match_wild(&cand, &pattern) == STR$_MATCH,
          "match_wild HEL%O matches HELXO");

    /* Exact match */
    cand    = make_s("SYSTEM");
    pattern = make_s("SYSTEM");
    check(str$match_wild(&cand, &pattern) == STR$_MATCH,
          "match_wild exact match");

    /* No match */
    cand    = make_s("SYSTEM");
    pattern = make_s("SYS");
    check(str$match_wild(&cand, &pattern) == STR$_NOMATCH,
          "match_wild no trailing wildcard: no match");

    /* Wildcard only */
    cand    = make_s("ANYTHING");
    pattern = make_s("*");
    check(str$match_wild(&cand, &pattern) == STR$_MATCH,
          "match_wild * matches anything");
}

/* ------------------------------------------------------------------ */
/* Null parameter guards                                               */
/* ------------------------------------------------------------------ */
static void test_null_params(void)
{
    printf("Testing str$ null parameter handling...\n");

    uint32_t st = str$copy_dx(NULL, NULL);
    check(st == SS$_BADPARAM, "copy_dx null returns SS$_BADPARAM");

    struct dsc$descriptor_s src = make_s("X");
    st = str$concat(NULL, &src, &src);
    check(st == SS$_BADPARAM, "concat null dest returns SS$_BADPARAM");

    st = str$trim(NULL, &src, NULL);
    check(st == SS$_BADPARAM, "trim null dest returns SS$_BADPARAM");

    st = str$upcase(NULL, &src);
    check(st == SS$_BADPARAM, "upcase null dest returns SS$_BADPARAM");
}

int main(void)
{
    printf("=== test_str_routines: STR$ string RTL ===\n");

    test_copy_dx_static();
    test_copy_dx_dynamic();
    test_concat();
    test_compare();
    test_case_blind_compare();
    test_trim();
    test_upcase();
    test_find_first_substring();
    test_match_wild();
    test_null_params();

    if (failures == 0)
        printf("All str_routines tests passed.\n");
    else
        printf("FAILED: %d test(s) failed.\n", failures);

    return failures;
}
