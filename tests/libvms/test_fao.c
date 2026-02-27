/*
 * test_fao.c - Unit tests for sys$fao / sys$faol
 *
 * Tests FAO directive formatting: !AS, !AD, !UL, !SL, !XL, !OL, !ZL,
 * field widths, repeat counts, and edge cases.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include "starlet.h"
#include "descrip.h"
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

/*
 * Helper: call sys$faol with a C string format and arguments,
 * placing output into buf (null-terminated).
 * Returns the VMS status code.
 */
static uint32_t fao(const char *fmt, char *buf, size_t bufsz,
                    const uint64_t *args)
{
    struct dsc$descriptor_s ctrl;
    struct dsc$descriptor_s out;
    uint16_t outlen = 0;

    vms_init_descriptor(&ctrl, fmt, (uint16_t)strlen(fmt));

    out.dsc$w_length  = (uint16_t)(bufsz - 1);
    out.dsc$b_dtype   = DSC$K_DTYPE_T;
    out.dsc$b_class   = DSC$K_CLASS_S;
    out.dsc$a_pointer = buf;

    uint32_t status = sys$faol(&ctrl, &outlen, &out, args);

    /* Null-terminate so we can use strcmp */
    if (outlen < bufsz)
        buf[outlen] = '\0';
    else
        buf[bufsz - 1] = '\0';

    return status;
}

/* ------------------------------------------------------------------ */
/* Literal passthrough                                                  */
/* ------------------------------------------------------------------ */
static void test_literal(void)
{
    printf("Testing FAO literal passthrough...\n");
    char buf[64];
    uint64_t args[1] = {0};

    /* Note: '!' is the FAO directive prefix.  A trailing '!' with nothing
     * after it is an invalid directive and returns SS$_BADPARAM.  Use a
     * plain string that contains no '!' characters. */
    uint32_t st = fao("Hello, World.", buf, sizeof(buf), args);
    check(st == SS$_NORMAL, "status SS$_NORMAL");
    check(strcmp(buf, "Hello, World.") == 0, "literal text preserved");

    /* Literal with '!!' (escaped exclamation mark) */
    st = fao("Hello!!", buf, sizeof(buf), args);
    check(st == SS$_NORMAL, "literal with !! status SS$_NORMAL");
    check(strcmp(buf, "Hello!") == 0, "!! produces single !");
}

/* ------------------------------------------------------------------ */
/* !UL — unsigned longword                                             */
/* ------------------------------------------------------------------ */
static void test_ul(void)
{
    printf("Testing FAO !UL directive...\n");
    char buf[64];
    uint64_t args[2] = {42, 0};

    fao("!UL", buf, sizeof(buf), args);
    check(strcmp(buf, "42") == 0, "!UL basic value");

    uint64_t args2[2] = {0, 0};
    fao("!UL", buf, sizeof(buf), args2);
    check(strcmp(buf, "0") == 0, "!UL zero");

    uint64_t args3[2] = {4294967295ULL, 0};
    fao("!UL", buf, sizeof(buf), args3);
    check(strcmp(buf, "4294967295") == 0, "!UL UINT32_MAX");
}

/* ------------------------------------------------------------------ */
/* !SL — signed longword                                               */
/* ------------------------------------------------------------------ */
static void test_sl(void)
{
    printf("Testing FAO !SL directive...\n");
    char buf[64];

    uint64_t args_pos[2] = {123, 0};
    fao("!SL", buf, sizeof(buf), args_pos);
    check(strcmp(buf, "123") == 0, "!SL positive");

    /* Negative: pass the two's-complement representation */
    uint64_t args_neg[2] = {(uint64_t)(uint32_t)(-42), 0};
    fao("!SL", buf, sizeof(buf), args_neg);
    check(strcmp(buf, "-42") == 0, "!SL negative");
}

/* ------------------------------------------------------------------ */
/* !XL — hex longword                                                  */
/* ------------------------------------------------------------------ */
static void test_xl(void)
{
    printf("Testing FAO !XL directive...\n");
    char buf[64];

    uint64_t args[2] = {0xDEADBEEFULL, 0};
    fao("!XL", buf, sizeof(buf), args);
    check(strcmp(buf, "DEADBEEF") == 0, "!XL hex uppercase");

    uint64_t args2[2] = {0, 0};
    fao("!XL", buf, sizeof(buf), args2);
    check(strcmp(buf, "0") == 0, "!XL zero");

    uint64_t args3[2] = {255, 0};
    fao("!XL", buf, sizeof(buf), args3);
    check(strcmp(buf, "FF") == 0, "!XL 255 -> FF");
}

/* ------------------------------------------------------------------ */
/* !OL — octal longword                                                */
/* ------------------------------------------------------------------ */
static void test_ol(void)
{
    printf("Testing FAO !OL directive...\n");
    char buf[64];

    uint64_t args[2] = {8, 0};
    fao("!OL", buf, sizeof(buf), args);
    check(strcmp(buf, "10") == 0, "!OL 8 -> octal 10");

    uint64_t args2[2] = {0777, 0};
    fao("!OL", buf, sizeof(buf), args2);
    check(strcmp(buf, "777") == 0, "!OL 0777 -> 777");
}

/* ------------------------------------------------------------------ */
/* !ZL — zero-filled hex longword (8 digits)                           */
/* ------------------------------------------------------------------ */
static void test_zl(void)
{
    printf("Testing FAO !ZL directive...\n");
    char buf[64];

    /* !ZL = zero-filled DECIMAL longword (8 decimal digits).
     * VMS !ZL is NOT hex — it formats the value as a zero-padded decimal
     * integer with field width 8.  Value 255 decimal = "00000255". */
    uint64_t args[2] = {255, 0};
    fao("!ZL", buf, sizeof(buf), args);
    check(strlen(buf) == 8, "!ZL always 8 chars");
    check(strcmp(buf, "00000255") == 0, "!ZL zero-padded decimal (255)");

    uint64_t args2[2] = {0, 0};
    fao("!ZL", buf, sizeof(buf), args2);
    check(strcmp(buf, "00000000") == 0, "!ZL zero value");

    uint64_t args3[2] = {42, 0};
    fao("!ZL", buf, sizeof(buf), args3);
    check(strcmp(buf, "00000042") == 0, "!ZL zero-padded decimal (42)");
}

/* ------------------------------------------------------------------ */
/* !AS — ASCII string from descriptor                                  */
/* ------------------------------------------------------------------ */
static void test_as(void)
{
    printf("Testing FAO !AS directive...\n");
    char buf[64];
    char str[] = "HELLO";
    struct dsc$descriptor_s desc;
    vms_init_descriptor(&desc, str, (uint16_t)strlen(str));

    uint64_t args[2] = {(uint64_t)(uintptr_t)&desc, 0};
    fao("!AS", buf, sizeof(buf), args);
    check(strcmp(buf, "HELLO") == 0, "!AS from descriptor");

    /* Empty descriptor */
    struct dsc$descriptor_s empty_desc;
    vms_init_descriptor(&empty_desc, "", 0);
    uint64_t args2[2] = {(uint64_t)(uintptr_t)&empty_desc, 0};
    fao("!AS", buf, sizeof(buf), args2);
    check(strcmp(buf, "") == 0, "!AS empty descriptor");
}

/* ------------------------------------------------------------------ */
/* !AD — ASCII counted string (length, pointer)                        */
/* ------------------------------------------------------------------ */
static void test_ad(void)
{
    printf("Testing FAO !AD directive...\n");
    char buf[64];
    char str[] = "VMS";

    uint64_t args[3] = {3, (uint64_t)(uintptr_t)str, 0};
    fao("!AD", buf, sizeof(buf), args);
    check(strcmp(buf, "VMS") == 0, "!AD counted string");

    /* Zero length */
    uint64_t args2[3] = {0, (uint64_t)(uintptr_t)str, 0};
    fao("!AD", buf, sizeof(buf), args2);
    check(strcmp(buf, "") == 0, "!AD zero length");
}

/* ------------------------------------------------------------------ */
/* !/ — newline, !_ — tab, !! — literal !                             */
/* ------------------------------------------------------------------ */
static void test_special(void)
{
    printf("Testing FAO special directives (!/ !_ !!)...\n");
    char buf[64];
    uint64_t args[1] = {0};

    fao("!/", buf, sizeof(buf), args);
    check(buf[0] == '\n', "!/ produces newline");

    fao("!_", buf, sizeof(buf), args);
    check(buf[0] == '\t', "!_ produces tab");

    fao("!!", buf, sizeof(buf), args);
    check(buf[0] == '!', "!! produces literal !");
}

/* ------------------------------------------------------------------ */
/* Multiple directives in one format string                            */
/* ------------------------------------------------------------------ */
static void test_multiple(void)
{
    printf("Testing FAO multiple directives...\n");
    char buf[128];
    char name[] = "SYSTEM";
    struct dsc$descriptor_s name_desc;
    vms_init_descriptor(&name_desc, name, (uint16_t)strlen(name));

    uint64_t args[3] = {(uint64_t)(uintptr_t)&name_desc, 42, 0};
    fao("User: !AS pid=!UL", buf, sizeof(buf), args);
    check(strcmp(buf, "User: SYSTEM pid=42") == 0, "multiple directives");
}

/* ------------------------------------------------------------------ */
/* Buffer overflow — static descriptor too small                       */
/* ------------------------------------------------------------------ */
static void test_overflow(void)
{
    printf("Testing FAO buffer overflow detection...\n");
    char buf[4];  /* deliberately tiny */
    struct dsc$descriptor_s ctrl;
    struct dsc$descriptor_s out;
    uint16_t outlen = 0;
    const char *fmt = "TOOLONGSTRING";

    vms_init_descriptor(&ctrl, fmt, (uint16_t)strlen(fmt));
    out.dsc$w_length  = 3;  /* only 3 bytes available */
    out.dsc$b_dtype   = DSC$K_DTYPE_T;
    out.dsc$b_class   = DSC$K_CLASS_S;
    out.dsc$a_pointer = buf;

    uint64_t args[1] = {0};
    uint32_t st = sys$faol(&ctrl, &outlen, &out, args);
    check(st == SS$_BUFFEROVF, "overflow returns SS$_BUFFEROVF");
}

/* ------------------------------------------------------------------ */
/* Null / invalid parameter checks                                     */
/* ------------------------------------------------------------------ */
static void test_badparam(void)
{
    printf("Testing FAO null/invalid parameter handling...\n");
    char buf[64];
    struct dsc$descriptor_s out;
    uint16_t outlen = 0;

    out.dsc$w_length  = sizeof(buf) - 1;
    out.dsc$b_dtype   = DSC$K_DTYPE_T;
    out.dsc$b_class   = DSC$K_CLASS_S;
    out.dsc$a_pointer = buf;

    uint32_t st = sys$faol(NULL, &outlen, &out, NULL);
    check(st == SS$_BADPARAM, "null ctrstr returns SS$_BADPARAM");

    struct dsc$descriptor_s ctrl;
    vms_init_descriptor(&ctrl, "test", 4);
    st = sys$faol(&ctrl, &outlen, NULL, NULL);
    check(st == SS$_BADPARAM, "null outbuf returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* Dynamic descriptor output                                           */
/* ------------------------------------------------------------------ */
static void test_dynamic_outbuf(void)
{
    printf("Testing FAO dynamic output descriptor...\n");
    struct dsc$descriptor_s ctrl;
    struct dsc$descriptor_d out;
    uint16_t outlen = 0;
    /* Must not end with '!' — that would be an invalid FAO directive.
     * Use "!!" to get a literal '!' at the end, or avoid it entirely. */
    const char *fmt = "Hello.";

    vms_init_descriptor(&ctrl, fmt, (uint16_t)strlen(fmt));

    /* Initialize dynamic descriptor */
    out.dsc$w_length  = 0;
    out.dsc$b_dtype   = DSC$K_DTYPE_T;
    out.dsc$b_class   = DSC$K_CLASS_D;
    out.dsc$a_pointer = malloc(16);
    if (out.dsc$a_pointer)
        out.dsc$w_length = 16;

    uint64_t args[1] = {0};
    uint32_t st = sys$faol(&ctrl, &outlen,
                            (struct dsc$descriptor_s *)&out, args);
    check(st == SS$_NORMAL, "dynamic outbuf status");
    check(outlen == 6, "dynamic outbuf outlen");
    check(memcmp(out.dsc$a_pointer, "Hello.", 6) == 0, "dynamic outbuf content");

    free(out.dsc$a_pointer);
}

int main(void)
{
    printf("=== test_fao: sys$fao / sys$faol ===\n");

    test_literal();
    test_ul();
    test_sl();
    test_xl();
    test_ol();
    test_zl();
    test_as();
    test_ad();
    test_special();
    test_multiple();
    test_overflow();
    test_badparam();
    test_dynamic_outbuf();

    if (failures == 0)
        printf("All sys_fao tests passed.\n");
    else
        printf("FAILED: %d test(s) failed.\n", failures);

    return failures;
}
