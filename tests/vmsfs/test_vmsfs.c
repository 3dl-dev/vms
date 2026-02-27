/*
 * test_vmsfs.c - Unit tests for vmsfs
 *
 * Tests:
 *   - vmsfs_parse_filespec: component extraction from VMS filespecs
 *   - vmsfs_compose_filespec: round-trip through parse+compose
 *   - vmsfs_translate_directory: [DIR.SUBDIR] -> dir/subdir
 *   - vmsfs_wildcard_match: VMS wildcard patterns
 *   - vmsfs_get_highest_version / vmsfs_list_versions: version management
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "vmsfs/filespec.h"
#include "vmsfs/version.h"
#include "ssdef.h"
#include "rmsdef.h"

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

/* Helper: create an empty file at path */
static void touch(const char *path)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
}

/* ------------------------------------------------------------------ */
/* Test: vmsfs_parse_filespec                                          */
/* ------------------------------------------------------------------ */
static void test_parse_filespec(void)
{
    printf("\n--- vmsfs_parse_filespec ---\n");

    vmsfs_filespec_t r;
    int st;

    /* Basic name.type;version */
    st = vmsfs_parse_filespec("LOGIN.COM;1", &r);
    check(st == SS$_NORMAL, "parse LOGIN.COM;1 returns SS$_NORMAL");
    check(r.has_name && strcmp(r.name, "LOGIN") == 0, "name is LOGIN");
    check(r.has_type && strcmp(r.type, "COM") == 0, "type is COM");
    check(r.has_version && r.version == 1, "version is 1");
    check(!r.has_device, "no device");
    check(!r.has_directory, "no directory");
    check(!r.has_node, "no node");

    /* With device */
    st = vmsfs_parse_filespec("SYS$LOGIN:STARTUP.COM;2", &r);
    check(st == SS$_NORMAL, "parse SYS$LOGIN:STARTUP.COM;2");
    check(r.has_device && strcmp(r.device, "SYS$LOGIN") == 0, "device is SYS$LOGIN");
    check(r.has_name && strcmp(r.name, "STARTUP") == 0, "name is STARTUP");
    check(r.has_type && strcmp(r.type, "COM") == 0, "type is COM");
    check(r.version == 2, "version is 2");

    /* With device and directory */
    st = vmsfs_parse_filespec("DKA0:[USERS.BARON]LOGIN.COM;1", &r);
    check(st == SS$_NORMAL, "parse DKA0:[USERS.BARON]LOGIN.COM;1");
    check(r.has_device && strcmp(r.device, "DKA0") == 0, "device is DKA0");
    check(r.has_directory && strcmp(r.directory, "USERS.BARON") == 0,
          "directory is USERS.BARON");
    check(r.has_name && strcmp(r.name, "LOGIN") == 0, "name is LOGIN");

    /* With node */
    st = vmsfs_parse_filespec("REMOTE::DKA0:[DIR]FILE.DAT;0", &r);
    check(st == SS$_NORMAL, "parse with node REMOTE::");
    check(r.has_node && strcmp(r.node, "REMOTE") == 0, "node is REMOTE");
    check(r.has_device && strcmp(r.device, "DKA0") == 0, "device is DKA0 with node");

    /* Version ;0 means highest */
    st = vmsfs_parse_filespec("TEST.DAT;0", &r);
    check(st == SS$_NORMAL, "parse TEST.DAT;0");
    check(r.has_version && r.version == 0, "version 0 (highest)");

    /* No version specified */
    st = vmsfs_parse_filespec("NOVERSION.TXT", &r);
    check(st == SS$_NORMAL, "parse NOVERSION.TXT");
    check(!r.has_version, "no version flag when not specified");

    /* Wildcard: name */
    st = vmsfs_parse_filespec("*.COM", &r);
    check(st == SS$_NORMAL, "parse *.COM");
    check(r.is_wildcard, "is_wildcard set for *.COM");

    /* Wildcard: percent */
    st = vmsfs_parse_filespec("TEST%.DAT", &r);
    check(st == SS$_NORMAL, "parse TEST%.DAT");
    check(r.is_wildcard, "is_wildcard set for TEST%.DAT");

    /* Empty spec is valid */
    st = vmsfs_parse_filespec("", &r);
    check(st == SS$_NORMAL, "parse empty string is valid");

    /* NULL input */
    st = vmsfs_parse_filespec(NULL, &r);
    check(st == SS$_BADPARAM, "parse NULL returns SS$_BADPARAM");

    /* NULL result */
    st = vmsfs_parse_filespec("TEST.DAT", NULL);
    check(st == SS$_BADPARAM, "parse with NULL result returns SS$_BADPARAM");

    /* ODS-2 uppercase: parser upcases components */
    st = vmsfs_parse_filespec("myfile.dat;1", &r);
    check(st == SS$_NORMAL, "parse lowercase myfile.dat;1");
    check(strcmp(r.name, "MYFILE") == 0, "name is uppercased to MYFILE");
    check(strcmp(r.type, "DAT") == 0, "type is uppercased to DAT");
}

/* ------------------------------------------------------------------ */
/* Test: vmsfs_compose_filespec - reconstruct a filespec from parts   */
/* ------------------------------------------------------------------ */
static void test_compose_filespec(void)
{
    printf("\n--- vmsfs_compose_filespec ---\n");

    vmsfs_filespec_t r;
    char out[512];
    int st;

    /* Round-trip: parse then compose */
    st = vmsfs_parse_filespec("DKA0:[USERS.BARON]LOGIN.COM;1", &r);
    check(st == SS$_NORMAL, "parse for compose round-trip");

    st = vmsfs_compose_filespec(&r, out, sizeof(out));
    check(st == SS$_NORMAL, "compose returns SS$_NORMAL");
    check(strcmp(out, "DKA0:[USERS.BARON]LOGIN.COM;1") == 0,
          "compose output matches original");

    /* Simple name.type;version */
    memset(&r, 0, sizeof(r));
    r.has_name = 1; strcpy(r.name, "TEST");
    r.has_type = 1; strcpy(r.type, "DAT");
    r.has_version = 1; r.version = 3;

    st = vmsfs_compose_filespec(&r, out, sizeof(out));
    check(st == SS$_NORMAL, "compose TEST.DAT;3");
    check(strcmp(out, "TEST.DAT;3") == 0, "compose output is TEST.DAT;3");

    /* No version */
    memset(&r, 0, sizeof(r));
    r.has_name = 1; strcpy(r.name, "NOVERSION");
    r.has_type = 1; strcpy(r.type, "COM");

    st = vmsfs_compose_filespec(&r, out, sizeof(out));
    check(st == SS$_NORMAL, "compose NOVERSION.COM (no version)");
    check(strcmp(out, "NOVERSION.COM") == 0, "no version string in output");

    /* NULL inputs */
    st = vmsfs_compose_filespec(NULL, out, sizeof(out));
    check(st == SS$_BADPARAM, "compose NULL parts returns SS$_BADPARAM");
    st = vmsfs_compose_filespec(&r, NULL, sizeof(out));
    check(st == SS$_BADPARAM, "compose NULL result returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* Test: vmsfs_translate_directory                                     */
/* ------------------------------------------------------------------ */
static void test_translate_directory(void)
{
    printf("\n--- vmsfs_translate_directory ---\n");

    char out[512];
    int st;

    /* Simple directory */
    st = vmsfs_translate_directory("USERS", out, sizeof(out));
    check(st == SS$_NORMAL, "translate USERS");
    check(strcmp(out, "USERS") == 0, "USERS -> USERS");

    /* Multi-level with dots */
    st = vmsfs_translate_directory("USERS.BARON.SUBDIR", out, sizeof(out));
    check(st == SS$_NORMAL, "translate USERS.BARON.SUBDIR");
    check(strcmp(out, "USERS/BARON/SUBDIR") == 0, "USERS.BARON.SUBDIR -> USERS/BARON/SUBDIR");

    /* MFD [000000] = root */
    st = vmsfs_translate_directory("000000", out, sizeof(out));
    check(st == SS$_NORMAL, "translate 000000 (MFD)");
    check(out[0] == '\0', "000000 -> empty string (root)");

    /* Parent directory [-] */
    st = vmsfs_translate_directory("-", out, sizeof(out));
    check(st == SS$_NORMAL, "translate - (parent)");
    check(strcmp(out, "..") == 0, "- -> ..");

    /* Parent with subdir [-.SUB] */
    st = vmsfs_translate_directory("-.SUB", out, sizeof(out));
    check(st == SS$_NORMAL, "translate -.SUB (parent+sub)");
    check(strcmp(out, "../SUB") == 0, "-.SUB -> ../SUB");

    /* Relative [.SUB] */
    st = vmsfs_translate_directory(".SUB", out, sizeof(out));
    check(st == SS$_NORMAL, "translate .SUB (relative)");
    check(strcmp(out, "SUB") == 0, ".SUB -> SUB");

    /* NULL inputs */
    st = vmsfs_translate_directory(NULL, out, sizeof(out));
    check(st == SS$_BADPARAM, "translate NULL dir returns SS$_BADPARAM");
    st = vmsfs_translate_directory("USERS", NULL, sizeof(out));
    check(st == SS$_BADPARAM, "translate NULL out returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* Test: vmsfs_wildcard_match                                          */
/* ------------------------------------------------------------------ */
static void test_wildcard_match(void)
{
    printf("\n--- vmsfs_wildcard_match ---\n");

    /* Exact match (case-insensitive) */
    check(vmsfs_wildcard_match("LOGIN.COM", "login.com") == 1,
          "exact match case-insensitive");
    check(vmsfs_wildcard_match("LOGIN.COM", "LOGOUT.COM") == 0,
          "exact non-match");

    /* Star wildcard */
    check(vmsfs_wildcard_match("*.COM", "LOGIN.COM") == 1, "*.COM matches LOGIN.COM");
    check(vmsfs_wildcard_match("*.COM", "STARTUP.COM") == 1, "*.COM matches STARTUP.COM");
    check(vmsfs_wildcard_match("*.COM", "LOGIN.DAT") == 0, "*.COM does not match LOGIN.DAT");
    check(vmsfs_wildcard_match("*", "ANYTHING") == 1, "* matches anything");
    check(vmsfs_wildcard_match("LOG*", "LOGIN") == 1, "LOG* matches LOGIN");
    check(vmsfs_wildcard_match("LOG*", "NOLOG") == 0, "LOG* does not match NOLOG");

    /* Percent wildcard (exactly one char) */
    check(vmsfs_wildcard_match("LOG%.COM", "LOGI.COM") == 1,
          "LOG%.COM matches LOGI.COM");
    check(vmsfs_wildcard_match("LOG%.COM", "LOGIT.COM") == 0,
          "LOG%.COM does not match LOGIT.COM (2 chars)");
    check(vmsfs_wildcard_match("LOG%.COM", "LOG.COM") == 0,
          "LOG%.COM does not match LOG.COM (0 chars)");

    /* NULL inputs */
    check(vmsfs_wildcard_match(NULL, "test") == 0, "NULL pattern returns 0");
    check(vmsfs_wildcard_match("*", NULL) == 0, "NULL name returns 0");

    /* Trailing star */
    check(vmsfs_wildcard_match("SYS*", "SYSLOG") == 1, "SYS* matches SYSLOG");
    check(vmsfs_wildcard_match("SYS*", "SYS") == 1, "SYS* matches SYS (zero chars)");
}

/* ------------------------------------------------------------------ */
/* Test: vmsfs_get_highest_version / vmsfs_list_versions              */
/* ------------------------------------------------------------------ */
static void test_versions(void)
{
    printf("\n--- version management ---\n");

    /* Create a temp directory with versioned files */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/test_vmsfs_%d", (int)getpid());
    mkdir(tmpdir, 0755);

    /*
     * Files are stored as "name.ext;N" on disk.
     * vmsfs_get_highest_version / vmsfs_list_versions expect:
     *   basename = "test", ext = "dat" (WITHOUT leading dot)
     * The implementation appends them as "test.dat;N".
     */
    char path[512];
    snprintf(path, sizeof(path), "%s/test.dat;1", tmpdir); touch(path);
    snprintf(path, sizeof(path), "%s/test.dat;2", tmpdir); touch(path);
    snprintf(path, sizeof(path), "%s/test.dat;5", tmpdir); touch(path);

    int highest = vmsfs_get_highest_version(tmpdir, "test", "dat");
    check(highest == 5, "highest version is 5");

    /* No files for other.dat */
    highest = vmsfs_get_highest_version(tmpdir, "other", "dat");
    check(highest <= 0, "highest version for non-existent file <= 0");

    /* List versions (returns SS$_NORMAL = 1 on success, not 0) */
    int versions[32];
    int count = 0;
    int rc = vmsfs_list_versions(tmpdir, "test", "dat", versions, 32, &count);
    check($VMS_STATUS_SUCCESS(rc), "vmsfs_list_versions returns success");
    check(count == 3, "list_versions finds 3 versions");

    /* Versions should include 1, 2, 5 */
    int found1 = 0, found2 = 0, found5 = 0;
    for (int i = 0; i < count; i++) {
        if (versions[i] == 1) found1 = 1;
        if (versions[i] == 2) found2 = 1;
        if (versions[i] == 5) found5 = 1;
    }
    check(found1, "version 1 in list");
    check(found2, "version 2 in list");
    check(found5, "version 5 in list");

    /* Purge versions, keep highest 1 */
    rc = vmsfs_purge_versions(tmpdir, "test", "dat", 1);
    check(rc >= 0, "vmsfs_purge_versions returns >= 0");

    /* After purge, only version 5 should remain */
    highest = vmsfs_get_highest_version(tmpdir, "test", "dat");
    check(highest == 5, "highest version still 5 after purge");

    rc = vmsfs_list_versions(tmpdir, "test", "dat", versions, 32, &count);
    check($VMS_STATUS_SUCCESS(rc), "list_versions after purge returns success");
    check(count == 1, "only 1 version remains after purge-keep-1");

    /* Clean up */
    snprintf(path, sizeof(path), "%s/test.dat;5", tmpdir);
    unlink(path);
    /* Also try to remove any remaining ones */
    snprintf(path, sizeof(path), "%s/test.dat;1", tmpdir); unlink(path);
    snprintf(path, sizeof(path), "%s/test.dat;2", tmpdir); unlink(path);
    rmdir(tmpdir);
}

int main(void)
{
    printf("=== vmsfs unit tests ===\n");

    test_parse_filespec();
    test_compose_filespec();
    test_translate_directory();
    test_wildcard_match();
    test_versions();

    if (failures == 0)
        printf("\nAll vmsfs tests passed.\n");
    else
        printf("\nSome vmsfs tests FAILED (%d).\n", failures);

    return failures;
}
