/*
 * test_str_routines.c - Unit tests for STR$ string RTL
 *
 * Comprehensive tests for all str$ routines: copy_dx, copy_r, concat,
 * compare, compare_eql, case_blind_compare, trim, upcase, position,
 * find_first_substring, left, right, len_extr, element, match_wild,
 * get1_dx, free1_dx, analyze_sdesc, append, prefix, replace.
 * Uses both static (CLASS_S) and dynamic (CLASS_D) descriptors.
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
/* str$copy_r — copy raw buffer into descriptor                        */
/* ------------------------------------------------------------------ */
static void test_copy_r(void)
{
    printf("Testing str$copy_r...\n");

    /* Raw to dynamic */
    struct dsc$descriptor_d dst;
    dst.dsc$w_length  = 0;
    dst.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst.dsc$b_class   = DSC$K_CLASS_D;
    dst.dsc$a_pointer = NULL;

    uint16_t len = 5;
    uint32_t st = str$copy_r((struct dsc$descriptor_s *)&dst, &len, "RAWCP");
    check(st == SS$_NORMAL, "copy_r raw->dynamic returns SS$_NORMAL");
    check(dst.dsc$w_length == 5, "copy_r raw->dynamic: length = 5");
    check(memcmp(dst.dsc$a_pointer, "RAWCP", 5) == 0, "copy_r raw->dynamic: content");
    str$free1_dx(&dst);

    /* Raw to static with space padding */
    char buf[8];
    memset(buf, 'X', 8);
    struct dsc$descriptor_s sdst;
    sdst.dsc$w_length  = 8;
    sdst.dsc$b_dtype   = DSC$K_DTYPE_T;
    sdst.dsc$b_class   = DSC$K_CLASS_S;
    sdst.dsc$a_pointer = buf;

    len = 3;
    st = str$copy_r(&sdst, &len, "ABC");
    check(st == SS$_NORMAL, "copy_r raw->static returns SS$_NORMAL");
    check(memcmp(buf, "ABC     ", 8) == 0, "copy_r raw->static: content + space padding");

    /* Null parameter */
    st = str$copy_r(NULL, &len, "X");
    check(st == SS$_BADPARAM, "copy_r null dest returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* str$compare_eql — equality test                                     */
/* ------------------------------------------------------------------ */
static void test_compare_eql(void)
{
    printf("Testing str$compare_eql...\n");
    struct dsc$descriptor_s a = make_s("SAME");
    struct dsc$descriptor_s b = make_s("SAME");
    struct dsc$descriptor_s c = make_s("DIFF");
    struct dsc$descriptor_s d = make_s("SA");

    check(str$compare_eql(&a, &b) == 0, "compare_eql equal => 0");
    check(str$compare_eql(&a, &c) == 1, "compare_eql not equal => 1");
    check(str$compare_eql(&a, &d) == 1, "compare_eql different length => 1");
}

/* ------------------------------------------------------------------ */
/* str$position — substring position with start offset                  */
/* ------------------------------------------------------------------ */
static void test_position(void)
{
    printf("Testing str$position...\n");
    struct dsc$descriptor_s src = make_s("ABCABC");
    struct dsc$descriptor_s sub = make_s("BC");

    uint32_t start = 1;
    uint32_t pos = str$position(&src, &sub, &start);
    check(pos == 2, "position BC in ABCABC from 1 => 2");

    start = 3;
    pos = str$position(&src, &sub, &start);
    check(pos == 5, "position BC in ABCABC from 3 => 5");

    struct dsc$descriptor_s notfound = make_s("XY");
    start = 1;
    pos = str$position(&src, &notfound, &start);
    check(pos == 0, "position XY not found => 0");

    /* Start beyond string */
    start = 100;
    pos = str$position(&src, &sub, &start);
    check(pos == 0, "position start beyond string => 0");
}

/* ------------------------------------------------------------------ */
/* str$left — left substring extraction                                */
/* ------------------------------------------------------------------ */
static void test_left(void)
{
    printf("Testing str$left...\n");
    struct dsc$descriptor_s src = make_s("ABCDEF");

    struct dsc$descriptor_d dst;
    dst.dsc$w_length  = 0;
    dst.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst.dsc$b_class   = DSC$K_CLASS_D;
    dst.dsc$a_pointer = NULL;

    uint16_t endp = 3;
    uint32_t st = str$left((struct dsc$descriptor_s *)&dst, &src, &endp);
    check(st == SS$_NORMAL, "left returns SS$_NORMAL");
    check(dst.dsc$w_length == 3, "left 3: length = 3");
    check(memcmp(dst.dsc$a_pointer, "ABC", 3) == 0, "left 3: content = ABC");
    str$free1_dx(&dst);

    /* Beyond length clamps */
    dst.dsc$w_length  = 0;
    dst.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst.dsc$b_class   = DSC$K_CLASS_D;
    dst.dsc$a_pointer = NULL;

    endp = 100;
    st = str$left((struct dsc$descriptor_s *)&dst, &src, &endp);
    check(st == SS$_NORMAL, "left beyond length returns SS$_NORMAL");
    check(dst.dsc$w_length == 6, "left beyond length: clamped to 6");
    str$free1_dx(&dst);
}

/* ------------------------------------------------------------------ */
/* str$right — right substring extraction                              */
/* ------------------------------------------------------------------ */
static void test_right(void)
{
    printf("Testing str$right...\n");
    struct dsc$descriptor_s src = make_s("ABCDEF");

    struct dsc$descriptor_d dst;
    dst.dsc$w_length  = 0;
    dst.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst.dsc$b_class   = DSC$K_CLASS_D;
    dst.dsc$a_pointer = NULL;

    uint16_t startp = 4;
    uint32_t st = str$right((struct dsc$descriptor_s *)&dst, &src, &startp);
    check(st == SS$_NORMAL, "right returns SS$_NORMAL");
    check(dst.dsc$w_length == 3, "right from 4: length = 3");
    check(memcmp(dst.dsc$a_pointer, "DEF", 3) == 0, "right from 4: content = DEF");
    str$free1_dx(&dst);

    /* Beyond length => empty */
    dst.dsc$w_length  = 0;
    dst.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst.dsc$b_class   = DSC$K_CLASS_D;
    dst.dsc$a_pointer = NULL;

    startp = 100;
    st = str$right((struct dsc$descriptor_s *)&dst, &src, &startp);
    check(st == SS$_NORMAL, "right beyond length returns SS$_NORMAL");
    check(dst.dsc$w_length == 0, "right beyond length: empty");
    str$free1_dx(&dst);
}

/* ------------------------------------------------------------------ */
/* str$len_extr — extract by position and length                       */
/* ------------------------------------------------------------------ */
static void test_len_extr(void)
{
    printf("Testing str$len_extr...\n");
    struct dsc$descriptor_s src = make_s("ABCDEF");

    struct dsc$descriptor_d dst;
    dst.dsc$w_length  = 0;
    dst.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst.dsc$b_class   = DSC$K_CLASS_D;
    dst.dsc$a_pointer = NULL;

    uint32_t sp = 2, len = 3;
    uint32_t st = str$len_extr((struct dsc$descriptor_s *)&dst, &src, &sp, &len);
    check(st == SS$_NORMAL, "len_extr returns SS$_NORMAL");
    check(dst.dsc$w_length == 3, "len_extr pos=2 len=3: length = 3");
    check(memcmp(dst.dsc$a_pointer, "BCD", 3) == 0, "len_extr pos=2 len=3: content = BCD");
    str$free1_dx(&dst);

    /* Extraction clamps at end */
    dst.dsc$w_length  = 0;
    dst.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst.dsc$b_class   = DSC$K_CLASS_D;
    dst.dsc$a_pointer = NULL;

    sp = 5; len = 100;
    st = str$len_extr((struct dsc$descriptor_s *)&dst, &src, &sp, &len);
    check(st == SS$_NORMAL, "len_extr clamp at end returns SS$_NORMAL");
    check(dst.dsc$w_length == 2, "len_extr clamp: length = 2");
    check(memcmp(dst.dsc$a_pointer, "EF", 2) == 0, "len_extr clamp: content = EF");
    str$free1_dx(&dst);

    /* Start beyond end => empty */
    dst.dsc$w_length  = 0;
    dst.dsc$b_dtype   = DSC$K_DTYPE_T;
    dst.dsc$b_class   = DSC$K_CLASS_D;
    dst.dsc$a_pointer = NULL;

    sp = 100; len = 5;
    st = str$len_extr((struct dsc$descriptor_s *)&dst, &src, &sp, &len);
    check(st == SS$_NORMAL, "len_extr beyond end returns SS$_NORMAL");
    check(dst.dsc$w_length == 0, "len_extr beyond end: empty");
    str$free1_dx(&dst);
}

/* ------------------------------------------------------------------ */
/* str$element — delimited element extraction                          */
/* ------------------------------------------------------------------ */
static void test_element(void)
{
    printf("Testing str$element...\n");
    struct dsc$descriptor_s src = make_s("one,two,three");
    struct dsc$descriptor_s delim = make_s(",");

    struct dsc$descriptor_d dst;

    /* Element 0 */
    dst.dsc$w_length = 0; dst.dsc$b_dtype = DSC$K_DTYPE_T;
    dst.dsc$b_class = DSC$K_CLASS_D; dst.dsc$a_pointer = NULL;
    uint32_t elem = 0;
    uint32_t st = str$element((struct dsc$descriptor_s *)&dst, &elem, &delim, &src);
    check(st == SS$_NORMAL, "element 0 returns SS$_NORMAL");
    check(dst.dsc$w_length == 3, "element 0: length = 3");
    check(memcmp(dst.dsc$a_pointer, "one", 3) == 0, "element 0: content = 'one'");
    str$free1_dx(&dst);

    /* Element 1 */
    dst.dsc$w_length = 0; dst.dsc$b_dtype = DSC$K_DTYPE_T;
    dst.dsc$b_class = DSC$K_CLASS_D; dst.dsc$a_pointer = NULL;
    elem = 1;
    st = str$element((struct dsc$descriptor_s *)&dst, &elem, &delim, &src);
    check(st == SS$_NORMAL, "element 1 returns SS$_NORMAL");
    check(memcmp(dst.dsc$a_pointer, "two", 3) == 0, "element 1: content = 'two'");
    str$free1_dx(&dst);

    /* Element 2 */
    dst.dsc$w_length = 0; dst.dsc$b_dtype = DSC$K_DTYPE_T;
    dst.dsc$b_class = DSC$K_CLASS_D; dst.dsc$a_pointer = NULL;
    elem = 2;
    st = str$element((struct dsc$descriptor_s *)&dst, &elem, &delim, &src);
    check(st == SS$_NORMAL, "element 2 returns SS$_NORMAL");
    check(memcmp(dst.dsc$a_pointer, "three", 5) == 0, "element 2: content = 'three'");
    str$free1_dx(&dst);

    /* Element beyond range returns delimiter */
    dst.dsc$w_length = 0; dst.dsc$b_dtype = DSC$K_DTYPE_T;
    dst.dsc$b_class = DSC$K_CLASS_D; dst.dsc$a_pointer = NULL;
    elem = 10;
    st = str$element((struct dsc$descriptor_s *)&dst, &elem, &delim, &src);
    check(st == SS$_NORMAL, "element beyond range returns SS$_NORMAL");
    check(dst.dsc$w_length == 1 && dst.dsc$a_pointer[0] == ',',
          "element beyond range: returns delimiter");
    str$free1_dx(&dst);
}

/* ------------------------------------------------------------------ */
/* str$get1_dx — allocate dynamic descriptor storage                   */
/* ------------------------------------------------------------------ */
static void test_get1_dx(void)
{
    printf("Testing str$get1_dx...\n");
    struct dsc$descriptor_d d;
    d.dsc$w_length  = 0;
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_D;
    d.dsc$a_pointer = NULL;

    uint16_t len = 64;
    uint32_t st = str$get1_dx(&len, &d);
    check(st == SS$_NORMAL, "get1_dx returns SS$_NORMAL");
    check(d.dsc$w_length == 64, "get1_dx: length = 64");
    check(d.dsc$a_pointer != NULL, "get1_dx: pointer allocated");

    str$free1_dx(&d);
    check(d.dsc$w_length == 0 && d.dsc$a_pointer == NULL,
          "free1_dx after get1_dx: cleaned up");
}

/* ------------------------------------------------------------------ */
/* str$analyze_sdesc — descriptor analysis                             */
/* ------------------------------------------------------------------ */
static void test_analyze_sdesc(void)
{
    printf("Testing str$analyze_sdesc...\n");

    /* Static descriptor */
    struct dsc$descriptor_s s = make_s("Test");
    uint16_t len = 0;
    char *addr = NULL;
    uint32_t st = str$analyze_sdesc(&s, &len, &addr);
    check(st == SS$_NORMAL, "analyze_sdesc static returns SS$_NORMAL");
    check(len == 4, "analyze_sdesc static: length = 4");
    check(addr == s.dsc$a_pointer, "analyze_sdesc static: correct address");

    /* Dynamic descriptor */
    struct dsc$descriptor_d dd;
    dd.dsc$w_length = 0; dd.dsc$b_dtype = DSC$K_DTYPE_T;
    dd.dsc$b_class = DSC$K_CLASS_D; dd.dsc$a_pointer = NULL;
    str$copy_dx((struct dsc$descriptor_s *)&dd, &s);
    len = 0; addr = NULL;
    st = str$analyze_sdesc((struct dsc$descriptor_s *)&dd, &len, &addr);
    check(st == SS$_NORMAL, "analyze_sdesc dynamic returns SS$_NORMAL");
    check(len == 4, "analyze_sdesc dynamic: length = 4");
    check(addr == dd.dsc$a_pointer, "analyze_sdesc dynamic: correct address");
    str$free1_dx(&dd);

    /* Unsupported class */
    struct dsc$descriptor_s bad;
    bad.dsc$w_length = 10;
    bad.dsc$b_dtype = DSC$K_DTYPE_T;
    bad.dsc$b_class = DSC$K_CLASS_VS;
    bad.dsc$a_pointer = NULL;
    len = 0; addr = NULL;
    st = str$analyze_sdesc(&bad, &len, &addr);
    check(st == STR$_ILLSTRCLA, "analyze_sdesc unsupported class => STR$_ILLSTRCLA");
    check(len == 0 && addr == NULL, "analyze_sdesc unsupported: zeroed outputs");
}

/* ------------------------------------------------------------------ */
/* str$append — append to dynamic descriptor                           */
/* ------------------------------------------------------------------ */
static void test_append(void)
{
    printf("Testing str$append...\n");

    /* Basic append */
    struct dsc$descriptor_s init = make_s("Hello");
    struct dsc$descriptor_s suffix = make_s(" World");
    struct dsc$descriptor_d d;
    d.dsc$w_length = 0; d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_D; d.dsc$a_pointer = NULL;
    str$copy_dx((struct dsc$descriptor_s *)&d, &init);

    uint32_t st = str$append((struct dsc$descriptor_s *)&d, &suffix);
    check(st == SS$_NORMAL, "append returns SS$_NORMAL");
    check(d.dsc$w_length == 11, "append: length = 11");
    check(memcmp(d.dsc$a_pointer, "Hello World", 11) == 0,
          "append: content = 'Hello World'");
    str$free1_dx(&d);

    /* Append empty is no-op */
    struct dsc$descriptor_s empty = make_s("");
    d.dsc$w_length = 0; d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_D; d.dsc$a_pointer = NULL;
    str$copy_dx((struct dsc$descriptor_s *)&d, &init);
    st = str$append((struct dsc$descriptor_s *)&d, &empty);
    check(st == SS$_NORMAL, "append empty returns SS$_NORMAL");
    check(d.dsc$w_length == 5, "append empty: length unchanged");
    str$free1_dx(&d);

    /* Append rejects static descriptor */
    char buf[8] = "Hello";
    struct dsc$descriptor_s sdst;
    sdst.dsc$w_length = 5; sdst.dsc$b_dtype = DSC$K_DTYPE_T;
    sdst.dsc$b_class = DSC$K_CLASS_S; sdst.dsc$a_pointer = buf;
    st = str$append(&sdst, &suffix);
    check(st == SS$_BADPARAM, "append rejects CLASS_S => SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* str$prefix — prepend to dynamic descriptor                          */
/* ------------------------------------------------------------------ */
static void test_prefix(void)
{
    printf("Testing str$prefix...\n");

    struct dsc$descriptor_s init = make_s("World");
    struct dsc$descriptor_s pfx = make_s("Hello ");
    struct dsc$descriptor_d d;
    d.dsc$w_length = 0; d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_D; d.dsc$a_pointer = NULL;
    str$copy_dx((struct dsc$descriptor_s *)&d, &init);

    uint32_t st = str$prefix((struct dsc$descriptor_s *)&d, &pfx);
    check(st == SS$_NORMAL, "prefix returns SS$_NORMAL");
    check(d.dsc$w_length == 11, "prefix: length = 11");
    check(memcmp(d.dsc$a_pointer, "Hello World", 11) == 0,
          "prefix: content = 'Hello World'");
    str$free1_dx(&d);

    /* Prefix rejects static */
    char buf[8] = "World";
    struct dsc$descriptor_s sdst;
    sdst.dsc$w_length = 5; sdst.dsc$b_dtype = DSC$K_DTYPE_T;
    sdst.dsc$b_class = DSC$K_CLASS_S; sdst.dsc$a_pointer = buf;
    st = str$prefix(&sdst, &pfx);
    check(st == SS$_BADPARAM, "prefix rejects CLASS_S => SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* str$replace — replace portion of string                             */
/* ------------------------------------------------------------------ */
static void test_replace(void)
{
    printf("Testing str$replace...\n");

    /* Replace middle */
    struct dsc$descriptor_s src = make_s("ABCDEF");
    struct dsc$descriptor_s rep = make_s("XY");
    struct dsc$descriptor_d dst;
    dst.dsc$w_length = 0; dst.dsc$b_dtype = DSC$K_DTYPE_T;
    dst.dsc$b_class = DSC$K_CLASS_D; dst.dsc$a_pointer = NULL;

    uint32_t sp = 3, ep = 4;
    uint32_t st = str$replace((struct dsc$descriptor_s *)&dst, &src, &sp, &ep, &rep);
    check(st == SS$_NORMAL, "replace returns SS$_NORMAL");
    check(dst.dsc$w_length == 6, "replace middle: length = 6");
    check(memcmp(dst.dsc$a_pointer, "ABXYEF", 6) == 0,
          "replace middle: content = ABXYEF");
    str$free1_dx(&dst);

    /* Replace with longer string */
    struct dsc$descriptor_s rep2 = make_s("1234");
    dst.dsc$w_length = 0; dst.dsc$b_dtype = DSC$K_DTYPE_T;
    dst.dsc$b_class = DSC$K_CLASS_D; dst.dsc$a_pointer = NULL;
    sp = 2; ep = 3;
    st = str$replace((struct dsc$descriptor_s *)&dst, &src, &sp, &ep, &rep2);
    check(st == SS$_NORMAL, "replace with longer returns SS$_NORMAL");
    check(dst.dsc$w_length == 8, "replace with longer: length = 8");
    check(memcmp(dst.dsc$a_pointer, "A1234DEF", 8) == 0,
          "replace with longer: content = A1234DEF");
    str$free1_dx(&dst);
}

/* ------------------------------------------------------------------ */
/* Additional str$match_wild edge cases                                */
/* ------------------------------------------------------------------ */
static void test_match_wild_extra(void)
{
    printf("Testing str$match_wild edge cases...\n");
    struct dsc$descriptor_s cand, pattern;

    /* * matches empty prefix */
    cand    = make_s(".TXT");
    pattern = make_s("*.TXT");
    check(str$match_wild(&cand, &pattern) == STR$_MATCH,
          "match_wild * matches empty prefix");

    /* Multiple wildcards */
    cand    = make_s("ABC_DEF.TXT");
    pattern = make_s("A*_*.TXT");
    check(str$match_wild(&cand, &pattern) == STR$_MATCH,
          "match_wild multiple * wildcards");

    /* % doesn't match empty */
    cand    = make_s("AC");
    pattern = make_s("A%C");
    check(str$match_wild(&cand, &pattern) == STR$_NOMATCH,
          "match_wild %% doesn't match empty gap");

    /* Pattern longer than candidate */
    cand    = make_s("AB");
    pattern = make_s("ABCDE");
    check(str$match_wild(&cand, &pattern) == STR$_NOMATCH,
          "match_wild pattern longer than candidate");
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

    st = str$copy_dx(NULL, &src);
    check(st == SS$_BADPARAM, "copy_dx null dest only returns SS$_BADPARAM");

    struct dsc$descriptor_d d;
    d.dsc$w_length = 0; d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_D; d.dsc$a_pointer = NULL;
    st = str$copy_dx((struct dsc$descriptor_s *)&d, NULL);
    check(st == SS$_BADPARAM, "copy_dx null src only returns SS$_BADPARAM");
}

int main(void)
{
    printf("=== test_str_routines: STR$ string RTL ===\n");

    test_copy_dx_static();
    test_copy_dx_dynamic();
    test_copy_r();
    test_concat();
    test_compare();
    test_compare_eql();
    test_case_blind_compare();
    test_trim();
    test_upcase();
    test_find_first_substring();
    test_position();
    test_left();
    test_right();
    test_len_extr();
    test_element();
    test_get1_dx();
    test_analyze_sdesc();
    test_append();
    test_prefix();
    test_replace();
    test_match_wild();
    test_match_wild_extra();
    test_null_params();

    if (failures == 0)
        printf("All str_routines tests passed.\n");
    else
        printf("FAILED: %d test(s) failed.\n", failures);

    return failures;
}
