/*
 * test_identity.c - System-identity SSOT + login banners (INV-1, rd vms-e652)
 *
 * Covers the two halves of INV-1:
 *
 *  (a) ONE module owns system identity (ovmx_identity.h), with the dual
 *      identity ruled in D1: an OVMX product version shown to HUMANS
 *      (badged "OpenVMS-compatible" per INV-0) and a true-to-arch
 *      VMS-compat token read by MACHINES (F$GETSYI VERSION). Includes the
 *      IRON RULE: on an arch with no VMS lineage (ARM), OVMX must NOT
 *      report a fabricated VMS version.
 *
 *  (b) The login banners come from the SYS$ANNOUNCE / SYS$WELCOME logical
 *      names a manager defines at boot -- NOT a compiled-in printf. A
 *      sysadmin who types DEFINE/SYSTEM SYS$WELCOME "..." must see the
 *      banner change, and must get the built-in back when they deassign.
 *
 * Uses OVMX_SYSGEN_PATH to point the SCSNODE reader at a private temp file
 * (same technique as test_sysgen_identity.c) so no /vms mount is needed
 * (vms-d34).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "ssdef.h"
#include "sysgen_params.h"
#include "vms/logical.h"
#include "ovmx_layout.h"
#include "ovmx_identity.h"
#include "ovmx_banner.h"

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
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Run a banner function against a temp stream and capture what it wrote.
 * Returns the captured text in out (always NUL-terminated).
 */
static void capture_banner(void (*fn)(FILE *), char *out, size_t out_len)
{
    out[0] = '\0';

    FILE *tmp = tmpfile();
    if (!tmp) {
        fprintf(stderr, "test_identity: tmpfile() failed\n");
        exit(1);
    }

    fn(tmp);
    fflush(tmp);
    rewind(tmp);

    size_t n = fread(out, 1, out_len - 1, tmp);
    out[n] = '\0';
    fclose(tmp);
}

static uint32_t define_logical(const char *name, const char *value)
{
    return lnm_create(lnm_get_manager(), LNM_SYSTEM_TABLE, name, value,
                      0, LNM_MODE_EXEC);
}

static uint32_t undefine_logical(const char *name)
{
    return lnm_delete(lnm_get_manager(), LNM_SYSTEM_TABLE, name, LNM_MODE_EXEC);
}

/* ------------------------------------------------------------------ */
/* (a) Identity SSOT                                                   */
/* ------------------------------------------------------------------ */

static void test_brand_identity(void)
{
    printf("Brand identity (human surfaces, INV-0):\n");

    check(strcmp(ovmx_product_version(), OVMX_PRODUCT_VERSION) == 0,
          "ovmx_product_version() is the OVMX product version");

    const char *banner = ovmx_product_banner();

    check(strstr(banner, "OVMX") != NULL,
          "human banner names OVMX");
    check(strstr(banner, "OpenVMS-compatible") != NULL,
          "human banner carries the INV-0 'OpenVMS-compatible' badge");

    /*
     * INV-0's hard line: a human surface may say it is OpenVMS-COMPATIBLE,
     * but must never present itself AS VSI's product. "OpenVMS V7.3" as the
     * opening claim is exactly the passing-off the ruling forbids.
     */
    check(strncmp(banner, "OpenVMS", 7) != 0,
          "human banner does not open by claiming to BE OpenVMS");
    check(strstr(banner, "(tm)") == NULL,
          "human banner does not assert VSI's trademark symbol");
}

static void test_compat_identity(void)
{
    printf("VMS-compat identity (machine surfaces, true-to-arch):\n");

    const char *arch    = ovmx_hw_arch();
    const char *compat  = ovmx_compat_version();
    int         lineage = ovmx_arch_has_vms_lineage();

#if defined(__x86_64__)
    check(strcmp(arch, "X86_64") == 0, "hw arch reports X86_64");
    check(lineage == 1, "x86-64 is a VMS-lineage arch");
    /* D1: the V9.2-x family is operator-ruled for x86-64. */
    check(strncmp(compat, "V9.2", 4) == 0,
          "x86-64 compat version is in the ruled V9.2-x family");
#elif defined(__aarch64__)
    check(strcmp(arch, "AARCH64") == 0, "hw arch reports AARCH64");
    check(lineage == 0, "ARM has no VMS lineage");
    /*
     * IRON RULE -- never lie to the metal. OpenVMS never ran on ARM, so
     * OVMX-on-ARM must answer with its own honest version, not a
     * fabricated VSI one.
     */
    check(strcmp(compat, OVMX_PRODUCT_VERSION) == 0,
          "ARM reports OVMX's own version, not a fabricated VMS version");
    check(strncmp(compat, "V9.2", 4) != 0,
          "ARM does not claim an x86-64 VMS version");
#endif

    /* Whatever the arch, the compat token must be non-empty and versiony. */
    check(compat[0] == 'V', "compat version is a V-prefixed VMS-style token");

    /* The pre-INV-1 hardcoded value must be gone from every arch path. */
    check(strcmp(compat, "V7.3") != 0,
          "compat version is no longer the hardcoded V7.3");
}

static void test_node_identity(const char *sysgen_path)
{
    printf("Node identity:\n");

    char node[OVMX_IDENTITY_MAXLEN];

    /* With SCSNODE configured, the display name is the configured name. */
    setenv("OVMX_SYSGEN_PATH", sysgen_path, 1);
    ovmx_node_name(node, sizeof(node));
    check(strcmp(node, "TESTND") == 0,
          "ovmx_node_name() returns the configured SCSNODE");

    /*
     * INV-4: the Linux hostname must never reach a VMS-facing display.
     * (Only meaningful when the two actually differ, which they do here.)
     */
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    check(strcmp(node, host) != 0,
          "ovmx_node_name() is not the Linux hostname");

    /* With no SYSGEN store, fall back to the OVMX default -- never empty,
     * never a hostname. */
    setenv("OVMX_SYSGEN_PATH", "/nonexistent/ovmx-test-sysgen.dat", 1);
    ovmx_node_name(node, sizeof(node));
    check(strcmp(node, OVMX_DEFAULT_NODENAME) == 0,
          "ovmx_node_name() falls back to the OVMX default node name");

    setenv("OVMX_SYSGEN_PATH", sysgen_path, 1);
}

static void test_node_name_buffer_safety(void)
{
    printf("Node identity buffer safety:\n");

    /* Undersized buffer must truncate and stay NUL-terminated. */
    char tiny[4];
    memset(tiny, 'x', sizeof(tiny));
    ovmx_node_name(tiny, sizeof(tiny));
    check(tiny[sizeof(tiny) - 1] == '\0', "undersized buffer is NUL-terminated");
    check(strlen(tiny) <= sizeof(tiny) - 1, "undersized buffer does not overflow");

    /* Degenerate arguments must not crash. */
    ovmx_node_name(NULL, 16);
    char one[1] = { 'z' };
    ovmx_node_name(one, 0);
    check(one[0] == 'z', "zero-length buffer is left untouched");
    check(1, "NULL/zero-length arguments do not crash");
}

/* ------------------------------------------------------------------ */
/* (b) Login banners come from logicals, not printf                    */
/* ------------------------------------------------------------------ */

/*
 * NOTE (vms-48ab): SYS$WELCOME/SYS$ANNOUNCE are DEFINE/SYSTEM logicals --
 * LNM$SYSTEM is executive-resident (vms-d37/vms-96e2), and this binary runs
 * on the host with no /dev/vms. define_logical()/undefine_logical() above
 * therefore now fail honestly with SS$_NOSUCHDEV (no per-process fallback,
 * CLAUDE.md Rule 9 / INV-6) rather than silently taking effect. What used to
 * be "define it and see the override" host coverage below is now "attempt
 * to define it with no executive, and prove the banner does NOT silently
 * change" -- itself a real INV-6 regression guard at the DCL-consumer level.
 * The "with a real executive, the override actually works" proof (including
 * the '@file' multi-line form) moved to tests/qemu/test_syssvc_lnm_system.c.
 */

static void test_welcome_banner(void)
{
    printf("SYS$WELCOME (post-login banner):\n");

    char got[OVMX_BANNER_MAXLEN];

    /* Undefined -> built-in default, sourced from the identity SSOT. */
    undefine_logical("SYS$WELCOME");
    capture_banner(ovmx_banner_welcome, got, sizeof(got));
    check(strstr(got, "OVMX") != NULL,
          "undefined SYS$WELCOME falls back to the built-in OVMX banner");
    check(strstr(got, "OpenVMS-compatible") != NULL,
          "built-in welcome carries the INV-0 badge");
    check(strstr(got, "V7.3") == NULL,
          "built-in welcome no longer shows the hardcoded V7.3");

    /* No executive here, so DEFINE/SYSTEM SYS$WELCOME must fail honestly,
     * and the banner must NOT silently switch to the attempted override. */
    uint32_t st = define_logical("SYS$WELCOME", "Welcome to the lab system");
    check(st == SS$_NOSUCHDEV,
          "DEFINE/SYSTEM SYS$WELCOME fails SS$_NOSUCHDEV with no /dev/vms (no local fallback)");
    capture_banner(ovmx_banner_welcome, got, sizeof(got));
    check(strstr(got, "OVMX") != NULL,
          "a SYS$WELCOME define that failed honestly does NOT change the banner");

    /* Deassigning (also honest-fails) leaves the built-in in place too. */
    st = undefine_logical("SYS$WELCOME");
    check(st == SS$_NOSUCHDEV,
          "DEASSIGN/SYSTEM SYS$WELCOME fails SS$_NOSUCHDEV with no /dev/vms (no local fallback)");
    capture_banner(ovmx_banner_welcome, got, sizeof(got));
    check(strstr(got, "OVMX") != NULL,
          "the built-in banner is unaffected throughout (no executive was ever reached)");
}

static void test_announce_banner(void)
{
    printf("SYS$ANNOUNCE (pre-login banner):\n");

    char got[OVMX_BANNER_MAXLEN];

    /* VMS default is silence -- there is no built-in announcement. */
    undefine_logical("SYS$ANNOUNCE");
    capture_banner(ovmx_banner_announce, got, sizeof(got));
    check(got[0] == '\0',
          "undefined SYS$ANNOUNCE prints nothing (VMS default is silence)");

    /* No executive here, so DEFINE/SYSTEM SYS$ANNOUNCE must fail honestly,
     * and the pre-login banner must stay silent, not silently announce. */
    uint32_t st = define_logical("SYS$ANNOUNCE", "Unauthorized access is prohibited.");
    check(st == SS$_NOSUCHDEV,
          "DEFINE/SYSTEM SYS$ANNOUNCE fails SS$_NOSUCHDEV with no /dev/vms (no local fallback)");
    capture_banner(ovmx_banner_announce, got, sizeof(got));
    check(got[0] == '\0',
          "a SYS$ANNOUNCE define that failed honestly leaves the pre-login banner silent");

    undefine_logical("SYS$ANNOUNCE");
}

/* ------------------------------------------------------------------ */

static void write_test_db(const char *path)
{
    struct sysgen_file db;
    memset(&db, 0, sizeof(db));
    db.magic   = SYSGEN_MAGIC;
    db.version = SYSGEN_VERSION;
    db.count   = 1;

    struct sysgen_param *scsnode = &db.params[0];
    strncpy(scsnode->name, "SCSNODE", sizeof(scsnode->name) - 1);
    scsnode->type = SYSGEN_TYPE_STRING;
    strncpy(scsnode->str_current, "TESTND", sizeof(scsnode->str_current) - 1);
    strncpy(scsnode->str_default, "OVMX", sizeof(scsnode->str_default) - 1);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "test_identity: cannot open %s for write\n", path);
        exit(1);
    }
    if (fwrite(&db, sizeof(db), 1, fp) != 1) {
        fprintf(stderr, "test_identity: write failed\n");
        exit(1);
    }
    fclose(fp);
}

int main(void)
{
    printf("=== test_identity (INV-1: identity SSOT + login banners) ===\n");

    char path[] = "/tmp/ovmx_sysgen_ident_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "test_identity: mkstemp failed\n");
        return 1;
    }
    close(fd);
    write_test_db(path);
    setenv("OVMX_SYSGEN_PATH", path, 1);

    /* Banner resolution searches LNM$FILE_DEV, so the tables must exist. */
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);

    test_brand_identity();
    test_compat_identity();
    test_node_identity(path);
    test_node_name_buffer_safety();
    test_welcome_banner();
    test_announce_banner();

    unlink(path);

    printf("test_identity: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
