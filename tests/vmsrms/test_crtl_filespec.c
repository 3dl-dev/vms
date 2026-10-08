/*
 * test_crtl_filespec.c - the DEC C file-specification translators (vms-32ae):
 * ovmx_crtl_unix_to_vms / ovmx_crtl_vms_to_unix and the decc$to_vms,
 * decc$from_vms, decc$translate_vms entry points built on them.
 *
 * The UNIX->VMS table is the HP C RTL Reference "File Specification
 * Conversion" rules (see crtl_filespec.c). decc$to_vms is driven exactly the
 * way GCC's VMS-host code drives it (gcc/config/vms/vms-ld.c, vms-ar.c,
 * gcc/vmsdbgout.cc: an action routine that copies the name and returns 0,
 * allow_wild=1, no_directory=1). Host-only: no wildcard in these cases, so no
 * RMS directory search is reached (that runs in the booted alpha gate).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "rms/crtl_filespec.h"

int decc$to_vms(const char *, int (*)(char *, int), int, int);
int decc$from_vms(const char *, int (*)(char *), int);
char *decc$translate_vms(const char *);

static int failures;

static void check(int cond, const char *what)
{
    printf("  %s: %s\n", cond ? "OK" : "FAIL", what);
    if (!cond)
        failures++;
}

static void u2v(const char *in, int mode, const char *want, int want_kind)
{
    char out[256], label[512];
    int k = ovmx_crtl_unix_to_vms(in, out, sizeof out, mode);
    snprintf(label, sizeof label, "unix->vms \"%s\" (mode %d) -> \"%s\" (got \"%s\", kind %d)",
             in, mode, want, k < 0 ? "<error>" : out, k);
    check(k == want_kind && (k < 0 || strcmp(out, want) == 0), label);
}

static void v2u(const char *in, const char *want)
{
    char out[256], label[512];
    int r = ovmx_crtl_vms_to_unix(in, out, sizeof out);
    snprintf(label, sizeof label, "vms->unix \"%s\" -> \"%s\" (got \"%s\")",
             in, want ? want : "<error>", r < 0 ? "<error>" : out);
    check(want ? (r == 0 && strcmp(out, want) == 0) : r < 0, label);
}

/* GCC's action routine (vms-ld.c translate_unix). */
static char filename_buff[256];
static int last_type = -1, calls;
static int translate_unix(char *name, int type)
{
    strcpy(filename_buff, name);
    last_type = type;
    calls++;
    return 0;
}

static char ux_buff[256];
static int from_action(char *name)
{
    strcpy(ux_buff, name);
    return 0;
}

int main(void)
{
    printf("=== crtl filespec translation (vms-32ae) ===\n");

    u2v("/dka0/usr/include/stdio.h", OVMX_FS_AUTO, "dka0:[usr.include]stdio.h", OVMX_FS_FILE);
    u2v("/sys$scratch/ccA1b2.s", OVMX_FS_AUTO, "sys$scratch:ccA1b2.s", OVMX_FS_FILE);
    u2v("/dka0", OVMX_FS_DIR, "dka0:[000000]", OVMX_FS_DIR);
    u2v("/dka0/", OVMX_FS_AUTO, "dka0:[000000]", OVMX_FS_DIR);
    u2v("/dka0/a/b/", OVMX_FS_AUTO, "dka0:[a.b]", OVMX_FS_DIR);
    u2v("src/main.c", OVMX_FS_AUTO, "[.src]main.c", OVMX_FS_FILE);
    u2v("a/b/c.h", OVMX_FS_AUTO, "[.a.b]c.h", OVMX_FS_FILE);
    u2v("hello.c", OVMX_FS_AUTO, "hello.c", OVMX_FS_FILE);
    u2v("./hello.c", OVMX_FS_AUTO, "hello.c", OVMX_FS_FILE);
    u2v("../hello.c", OVMX_FS_AUTO, "[-]hello.c", OVMX_FS_FILE);
    u2v("../../x/y.c", OVMX_FS_AUTO, "[-.-.x]y.c", OVMX_FS_FILE);
    u2v("a/../b.c", OVMX_FS_AUTO, "b.c", OVMX_FS_FILE);
    u2v("/dka0/a/../b/c.c", OVMX_FS_AUTO, "dka0:[b]c.c", OVMX_FS_FILE);
    u2v("/dka0/../x", OVMX_FS_AUTO, "", -1);
    u2v("makefile", OVMX_FS_AUTO, "makefile.", OVMX_FS_FILE);
    u2v("libfoo.so.1", OVMX_FS_AUTO, "libfoo_so.1", OVMX_FS_FILE);
    u2v("dir/", OVMX_FS_AUTO, "[.dir]", OVMX_FS_DIR);
    u2v("dir", OVMX_FS_DIR, "[.dir]", OVMX_FS_DIR);
    u2v(".", OVMX_FS_AUTO, "[]", OVMX_FS_DIR);
    u2v("..", OVMX_FS_AUTO, "[-]", OVMX_FS_DIR);
    u2v("dir/", OVMX_FS_FILE, "", -1);
    u2v("/", OVMX_FS_AUTO, "", -1);
    u2v("", OVMX_FS_AUTO, "", -1);
    u2v("SYS$LOGIN:LOGIN.COM", OVMX_FS_AUTO, "SYS$LOGIN:LOGIN.COM", OVMX_FS_PASSTHRU);
    u2v("[.SRC]MAIN.C;3", OVMX_FS_AUTO, "[.SRC]MAIN.C;3", OVMX_FS_PASSTHRU);

    v2u("DKA0:[USR.INCLUDE]STDIO.H;1", "/DKA0/USR/INCLUDE/STDIO.H");
    v2u("DKA0:[000000]FILE.TXT", "/DKA0/FILE.TXT");
    v2u("DKA0:[000000.A]B.C", "/DKA0/A/B.C");
    v2u("SYS$LOGIN:LOGIN.COM", "/SYS$LOGIN/LOGIN.COM");
    v2u("[.SRC]MAIN.C", "SRC/MAIN.C");
    v2u("[-]X.C", "../X.C");
    v2u("[--.A]X.C", "../../A/X.C");
    v2u("[-.-]X.C", "../../X.C");
    v2u("[MY-DIR]F.C", "/sys$disk/MY-DIR/F.C");
    v2u("MAKEFILE.", "MAKEFILE");
    v2u("A.B;2", "A.B");
    v2u("DKA0:[A.B]", "/DKA0/A/B");
    v2u("NODE::DKA0:[A]B.C", NULL);
    v2u("[A.B", NULL);

    /* decc$to_vms the way GCC's VMS-host code calls it. */
    calls = 0;
    int n = decc$to_vms("/gnu/lib/libgcc.olb", translate_unix, 1, 1);
    check(n == 1 && calls == 1 && strcmp(filename_buff, "gnu:[lib]libgcc.olb") == 0 &&
              last_type == DECC$K_FILE,
          "decc$to_vms(\"/gnu/lib/libgcc.olb\", action, 1, 1) -> gnu:[lib]libgcc.olb (DECC$K_FILE)");
    calls = 0;
    n = decc$to_vms("obj/", translate_unix, 0, 0);
    check(n == 1 && strcmp(filename_buff, "[.obj]") == 0 && last_type == DECC$K_DIRECTORY,
          "decc$to_vms(\"obj/\", ...) -> [.obj] (DECC$K_DIRECTORY)");
    calls = 0;
    n = decc$to_vms("obj/", translate_unix, 1, 1);
    check(n == 0 && calls == 0, "decc$to_vms with no_directory=1 on a directory -> 0, action not called");

    n = decc$from_vms("DKA0:[A.B]C.D;5", from_action, 0);
    check(n == 1 && strcmp(ux_buff, "/DKA0/A/B/C.D") == 0,
          "decc$from_vms(\"DKA0:[A.B]C.D;5\") -> /DKA0/A/B/C.D");

    char *t = decc$translate_vms("SYS$SCRATCH:CC1.S");
    check(t && strcmp(t, "/SYS$SCRATCH/CC1.S") == 0,
          "decc$translate_vms(\"SYS$SCRATCH:CC1.S\") -> /SYS$SCRATCH/CC1.S");
    check(decc$translate_vms("[A.B") == NULL, "decc$translate_vms of a malformed spec -> NULL");

    printf(failures ? "\n%d FAILED\n" : "\nAll crtl filespec tests passed.\n", failures);
    return failures != 0;
}
