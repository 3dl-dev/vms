/*
 * test_filescan.c - Unit tests for sys$filescan (vms-4f3)
 *
 * Drives real VMS file specifications through $FILESCAN and asserts the
 * returned position (pointer offset into the source string) and length of
 * each FSCN$_ field, plus the fldflags presence mask and the error paths.
 *
 * Field boundaries follow the documented $FILESCAN rule that each component
 * INCLUDES its punctuation (node "::", device ":", directory "[ ]", type ".",
 * version ";"). See src/libvms/syssvc/sys_filescan.c for provenance.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "iledef.h"
#include "fscndef.h"

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
 * Assert that item-list entry `e` (already filled by $FILESCAN over source
 * string `src`) reports the substring `expect` at the expected place.
 * A NULL `expect` means the field must be absent (length 0, NULL pointer).
 */
static void expect_field(const char *label, const char *src,
                         const ILE2 *e, const char *expect)
{
    char msg[256];
    if (expect == NULL) {
        snprintf(msg, sizeof(msg), "%s absent", label);
        check(e->ile2$w_length == 0 && e->ile2$ps_bufaddr == NULL, msg);
        return;
    }
    size_t elen = strlen(expect);
    snprintf(msg, sizeof(msg), "%s length == %zu", label, elen);
    check(e->ile2$w_length == elen, msg);

    snprintf(msg, sizeof(msg), "%s points into source", label);
    int in_range = e->ile2$ps_bufaddr != NULL &&
                   (const char *)e->ile2$ps_bufaddr >= src &&
                   (const char *)e->ile2$ps_bufaddr <= src + strlen(src);
    check(in_range, msg);

    if (in_range && e->ile2$w_length == elen) {
        snprintf(msg, sizeof(msg), "%s content == \"%s\"", label, expect);
        check(memcmp(e->ile2$ps_bufaddr, expect, elen) == 0, msg);
    }
}

/* Build a descriptor over a C string and run $FILESCAN with a full item list. */
static uint32_t scan_all(const char *spec, ILE2 list[10], uint32_t *flags)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(spec);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)spec;

    list[0] = (ILE2){ 0, FSCN$_NODE,      NULL };
    list[1] = (ILE2){ 0, FSCN$_NODE_ACS,  NULL };
    list[2] = (ILE2){ 0, FSCN$_DEVICE,    NULL };
    list[3] = (ILE2){ 0, FSCN$_ROOT,      NULL };
    list[4] = (ILE2){ 0, FSCN$_DIRECTORY, NULL };
    list[5] = (ILE2){ 0, FSCN$_NAME,      NULL };
    list[6] = (ILE2){ 0, FSCN$_TYPE,      NULL };
    list[7] = (ILE2){ 0, FSCN$_VERSION,   NULL };
    list[8] = (ILE2){ 0, FSCN$_FILESPEC,  NULL };
    list[9] = (ILE2)ILE2_TERMINATOR;

    return sys$filescan(&d, list, flags);
}

/* ------------------------------------------------------------------ */

static void test_full_spec(void)
{
    printf("Test: full spec  VDA0:[SYS0.SYSMGR]LOGIN.COM;3\n");
    const char *spec = "VDA0:[SYS0.SYSMGR]LOGIN.COM;3";
    ILE2 l[10]; uint32_t flags = 0;
    uint32_t st = scan_all(spec, l, &flags);
    check($VMS_STATUS_SUCCESS(st), "status success");

    expect_field("NODE",      spec, &l[0], NULL);
    expect_field("NODE_ACS",  spec, &l[1], NULL);
    expect_field("DEVICE",    spec, &l[2], "VDA0:");
    expect_field("ROOT",      spec, &l[3], NULL);
    expect_field("DIRECTORY", spec, &l[4], "[SYS0.SYSMGR]");
    expect_field("NAME",      spec, &l[5], "LOGIN");
    expect_field("TYPE",      spec, &l[6], ".COM");
    expect_field("VERSION",   spec, &l[7], ";3");
    expect_field("FILESPEC",  spec, &l[8], spec);

    check(flags == (FSCN$M_DEVICE | FSCN$M_DIRECTORY | FSCN$M_NAME |
                    FSCN$M_TYPE | FSCN$M_VERSION), "fldflags mask");
}

static void test_name_type_only(void)
{
    printf("Test: partial  MMK.EXE\n");
    const char *spec = "MMK.EXE";
    ILE2 l[10]; uint32_t flags = 0;
    uint32_t st = scan_all(spec, l, &flags);
    check($VMS_STATUS_SUCCESS(st), "status success");

    expect_field("DEVICE",    spec, &l[2], NULL);
    expect_field("DIRECTORY", spec, &l[4], NULL);
    expect_field("NAME",      spec, &l[5], "MMK");
    expect_field("TYPE",      spec, &l[6], ".EXE");
    expect_field("VERSION",   spec, &l[7], NULL);
    check(flags == (FSCN$M_NAME | FSCN$M_TYPE), "fldflags mask");
}

static void test_wildcard(void)
{
    printf("Test: wildcard  DISK$WORK:[BUILD]*.OBJ;*\n");
    const char *spec = "DISK$WORK:[BUILD]*.OBJ;*";
    ILE2 l[10]; uint32_t flags = 0;
    uint32_t st = scan_all(spec, l, &flags);
    check($VMS_STATUS_SUCCESS(st), "status success");

    expect_field("DEVICE",    spec, &l[2], "DISK$WORK:");
    expect_field("DIRECTORY", spec, &l[4], "[BUILD]");
    expect_field("NAME",      spec, &l[5], "*");
    expect_field("TYPE",      spec, &l[6], ".OBJ");
    expect_field("VERSION",   spec, &l[7], ";*");
}

static void test_node_and_version(void)
{
    printf("Test: node  BOSTON::VDA100:[DIR]FILE.DAT;7\n");
    const char *spec = "BOSTON::VDA100:[DIR]FILE.DAT;7";
    ILE2 l[10]; uint32_t flags = 0;
    uint32_t st = scan_all(spec, l, &flags);
    check($VMS_STATUS_SUCCESS(st), "status success");

    expect_field("NODE",      spec, &l[0], "BOSTON::");
    expect_field("NODE_ACS",  spec, &l[1], NULL);
    expect_field("DEVICE",    spec, &l[2], "VDA100:");
    expect_field("DIRECTORY", spec, &l[4], "[DIR]");
    expect_field("NAME",      spec, &l[5], "FILE");
    expect_field("TYPE",      spec, &l[6], ".DAT");
    expect_field("VERSION",   spec, &l[7], ";7");
    check((flags & FSCN$M_NODE) != 0, "node flag set");
}

static void test_node_access_control(void)
{
    printf("Test: node ACS  MARS\"SMITH PWD\"::VDA0:[X]A.B\n");
    const char *spec = "MARS\"SMITH PWD\"::VDA0:[X]A.B";
    ILE2 l[10]; uint32_t flags = 0;
    uint32_t st = scan_all(spec, l, &flags);
    check($VMS_STATUS_SUCCESS(st), "status success");

    expect_field("NODE",      spec, &l[0], "MARS\"SMITH PWD\"::");
    expect_field("NODE_ACS",  spec, &l[1], "\"SMITH PWD\"");
    expect_field("DEVICE",    spec, &l[2], "VDA0:");
    expect_field("DIRECTORY", spec, &l[4], "[X]");
    expect_field("NAME",      spec, &l[5], "A");
    expect_field("TYPE",      spec, &l[6], ".B");
    check((flags & (FSCN$M_NODE | FSCN$M_NODE_ACS)) ==
              (FSCN$M_NODE | FSCN$M_NODE_ACS), "node + acs flags set");
}

static void test_rooted_directory(void)
{
    printf("Test: rooted  SYS$SYSDEVICE:[SYS0.][SYSEXE]INIT.EXE\n");
    const char *spec = "SYS$SYSDEVICE:[SYS0.][SYSEXE]INIT.EXE";
    ILE2 l[10]; uint32_t flags = 0;
    uint32_t st = scan_all(spec, l, &flags);
    check($VMS_STATUS_SUCCESS(st), "status success");

    expect_field("DEVICE",    spec, &l[2], "SYS$SYSDEVICE:");
    expect_field("ROOT",      spec, &l[3], "[SYS0.]");
    expect_field("DIRECTORY", spec, &l[4], "[SYSEXE]");
    expect_field("NAME",      spec, &l[5], "INIT");
    expect_field("TYPE",      spec, &l[6], ".EXE");
    check((flags & FSCN$M_ROOT) != 0, "root flag set");
}

static void test_device_and_dir_only(void)
{
    printf("Test: dir only  VDA0:[MMK.SRC]\n");
    const char *spec = "VDA0:[MMK.SRC]";
    ILE2 l[10]; uint32_t flags = 0;
    uint32_t st = scan_all(spec, l, &flags);
    check($VMS_STATUS_SUCCESS(st), "status success");

    expect_field("DEVICE",    spec, &l[2], "VDA0:");
    expect_field("ROOT",      spec, &l[3], NULL);
    expect_field("DIRECTORY", spec, &l[4], "[MMK.SRC]");
    expect_field("NAME",      spec, &l[5], NULL);
    expect_field("TYPE",      spec, &l[6], NULL);
    expect_field("VERSION",   spec, &l[7], NULL);
    check(flags == (FSCN$M_DEVICE | FSCN$M_DIRECTORY), "fldflags mask");
}

static void test_leading_type(void)
{
    printf("Test: leading dot  .LOGIN\n");
    const char *spec = ".LOGIN";
    ILE2 l[10]; uint32_t flags = 0;
    uint32_t st = scan_all(spec, l, &flags);
    check($VMS_STATUS_SUCCESS(st), "status success");

    /* Whole string is ".LOGIN": no name, type = ".LOGIN". */
    expect_field("NAME",  spec, &l[5], NULL);
    expect_field("TYPE",  spec, &l[6], ".LOGIN");
    check(flags == FSCN$M_TYPE, "only type present");
}

static void test_errors(void)
{
    printf("Test: error paths\n");
    ILE2 l[3];
    struct dsc$descriptor_s d;
    const char *spec = "VDA0:[X]A.B";
    d.dsc$w_length = (uint16_t)strlen(spec);
    d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)spec;

    /* NULL srcstr */
    l[0] = (ILE2){ 0, FSCN$_NAME, NULL };
    l[1] = (ILE2)ILE2_TERMINATOR;
    check(sys$filescan(NULL, l, NULL) == SS$_BADPARAM, "NULL srcstr -> BADPARAM");

    /* NULL valuelst */
    check(sys$filescan(&d, NULL, NULL) == SS$_BADPARAM, "NULL valuelst -> BADPARAM");

    /* Unknown item code */
    l[0] = (ILE2){ 0, 0x7FFF, NULL };
    l[1] = (ILE2)ILE2_TERMINATOR;
    check(sys$filescan(&d, l, NULL) == SS$_BADPARAM, "bad item code -> BADPARAM");

    /* Unbalanced bracket -> BADPARAM */
    const char *bad = "VDA0:[UNCLOSED";
    d.dsc$w_length = (uint16_t)strlen(bad);
    d.dsc$a_pointer = (char *)bad;
    l[0] = (ILE2){ 0, FSCN$_DIRECTORY, NULL };
    l[1] = (ILE2)ILE2_TERMINATOR;
    check(sys$filescan(&d, l, NULL) == SS$_BADPARAM, "unbalanced bracket -> BADPARAM");
}

int main(void)
{
    printf("=== sys$filescan unit tests (vms-4f3) ===\n");
    test_full_spec();
    test_name_type_only();
    test_wildcard();
    test_node_and_version();
    test_node_access_control();
    test_rooted_directory();
    test_device_and_dir_only();
    test_leading_type();
    test_errors();

    if (failures == 0) {
        printf("\nAll sys$filescan tests PASSED\n");
        return 0;
    }
    printf("\n%d sys$filescan assertion(s) FAILED\n", failures);
    return 1;
}
