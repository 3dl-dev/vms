/*
 * test_disk_transparency.c - VERACITY test for MOUNT disk transparency
 * (vms-f83, Engine B / vms-8ad).
 *
 * ============================================================
 * TWO GAPS THIS PROVES.
 *
 * PART 1 -- MOUNT auto-defines DISK$<volume-label>. VMS MOUNT defines a
 * logical name DISK$<label> whose equivalence is the mounted device, so a
 * volume is addressable by its label independent of the physical unit --
 * DISK$MYVOL:[DIR]FILE resolves wherever MYVOL is mounted (VSI OpenVMS System
 * Manager's Manual, Vol. 1, "Mounting Volumes"; VSI OpenVMS DCL Dictionary,
 * MOUNT). The label is READ FROM THE VOLUME, never fabricated from the command
 * line: an unreadable label yields NO DISK$ logical (CLAUDE.md INV-DCL). This
 * suite drives the exact helpers cmd_mount()/cmd_dismount() call
 * (dcl_disk_logical.c) -- the naming rule, the DEFINE, the DISK$label:[dir]file
 * resolution, and the DISMOUNT removal -- against LNM$PROCESS, which is
 * genuinely process-private on VMS too, so it is authentic on the host with no
 * /dev/vms (the full MOUNT -> mount(2) -> DISK$ path, which needs a real
 * /dev/vms + a real disk, is the paired positive in tests/qemu/
 * test_mount_e2e.sh). These helpers do not exist on the pre-fix build, so the
 * PART 1 assertions FAIL there and PASS once MOUNT defines DISK$.
 *
 * PART 2 -- a concealed rooted device is addressable as a virtual disk root:
 * FOO:[000000] names the root of the concealed device (FOO -> VDA100:[USERS.])
 * -- i.e. VDA100:[USERS] -- and FOO:[000000]FILE a file at that root, rather
 * than appending a literal "000000" subdirectory (VSI OpenVMS Guide to OpenVMS
 * File Applications, concealed/rooted devices; VSI OpenVMS User's Manual,
 * "Rooted Directories"). This composes on the concealed/rooted machinery
 * vms-d8e (#340) landed; this suite guards the [000000]-addresses-the-root
 * case specifically (vms-d8e's own test covers [SMITH]/[SMITH.PROJ] subdirs,
 * not the root itself).
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "ssdef.h"
#include "vms/logical.h"
#include "vmsfs/filespec.h"
#include "vmsfs/device.h"
#include "dcl/disk_logical.h"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static int ci_contains(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (strncasecmp(p, needle, nl) == 0)
            return 1;
    return 0;
}

/* F$TRNLNM through LNM$FILE_DEV -- the search list DCL translates a device
 * field through. Returns the equivalence (empty if not defined). */
static void trnlnm(lnm_manager_t *mgr, const char *name, char *out, size_t n)
{
    out[0] = '\0';
    uint16_t len = 0;
    if (lnm_translate(mgr, LNM_FILE_DEV, name, out, n, &len, NULL) != SS$_NORMAL)
        out[0] = '\0';
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_disk_transparency (MOUNT DISK$<label> + concealed rooted disk root, vms-f83) ===\n");

    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { printf("  FAIL: no LNM manager\n"); return 2; }

    /* ============================================================
     * PART 1 -- DISK$<label>
     * ============================================================ */

    /* The DISK$ naming rule: DISK$ + UPPERCASE(label), trailing pad stripped. */
    {
        char nm[32];
        CHECK(dcl_disk_logical_name("work", nm, sizeof(nm)) == SS$_NORMAL &&
              strcmp(nm, "DISK$WORK") == 0,
              "VERACITY: dcl_disk_logical_name(\"work\") == \"DISK$WORK\" (uppercased)");
        CHECK(dcl_disk_logical_name("WORK        ", nm, sizeof(nm)) == SS$_NORMAL &&
              strcmp(nm, "DISK$WORK") == 0,
              "dcl_disk_logical_name trims the space padding vmsfs writes into hb_volname");
    }

    /* INV-DCL honesty: an unusable label yields NO name (no fabricated DISK$). */
    {
        char nm[32];
        CHECK(!(dcl_disk_logical_name("", nm, sizeof(nm)) & 1),
              "INV-DCL: an empty label produces no DISK$ name (honest failure, not DISK$)");
        CHECK(!(dcl_disk_logical_name("            ", nm, sizeof(nm)) & 1),
              "INV-DCL: an all-blank label produces no DISK$ name");
        CHECK(!(dcl_disk_logical_name("TOOLONGALABEL13", nm, sizeof(nm)) & 1),
              "INV-DCL: a >12-char label (not a valid volume label) produces no DISK$ name");
    }

    /* A real directory tree standing in for the mounted volume VDA100:. */
    char base[] = "/tmp/ovmxf83_XXXXXX";
    if (!mkdtemp(base)) { perror("mkdtemp"); return 2; }
    char file_root[600];
    snprintf(file_root, sizeof(file_root), "%s/mounttst.txt", base);
    { FILE *f = fopen(file_root, "w"); if (f) { fputs("payload\n", f); fclose(f); } }

    /* Emulate what a successful MOUNT VDA100: (label WORK) establishes: the
     * filespec translator entry + the device logical, then the DISK$<label>
     * logical from dcl_mount_define_disk() -- the exact call cmd_mount() makes. */
    vmsfs_device_add("VDA100", base);
    lnm_create(mgr, LNM_PROCESS_TABLE, "VDA100", base,
               LNM_ATTR_TERMINAL, LNM_MODE_USER);

    /* Before the DISK$ define, F$TRNLNM(DISK$WORK) is empty -- the pre-fix
     * state (MOUNT never defined it). */
    {
        char eq[256];
        trnlnm(mgr, "DISK$WORK", eq, sizeof(eq));
        CHECK(eq[0] == '\0',
              "before MOUNT defines it, F$TRNLNM(\"DISK$WORK\") is empty (the pre-fix state)");
    }

    uint32_t dst = dcl_mount_define_disk(mgr, LNM_PROCESS_TABLE, "WORK", "VDA100:");
    CHECK(dst == SS$_NORMAL || dst == SS$_SUPERSEDE,
          "dcl_mount_define_disk(\"WORK\", \"VDA100:\") succeeds (the DEFINE MOUNT does)");

    /* F$TRNLNM(DISK$WORK) now resolves to the device. */
    {
        char eq[256];
        trnlnm(mgr, "DISK$WORK", eq, sizeof(eq));
        printf("  INFO: F$TRNLNM(\"DISK$WORK\") -> \"%s\"\n", eq);
        CHECK(strcmp(eq, "VDA100:") == 0,
              "VERACITY: F$TRNLNM(\"DISK$WORK\") resolves to VDA100: after MOUNT");
    }

    /* DISK$WORK:[000000]MOUNTTST.TXT opens the file -- proof the DISK$ name is
     * usable in a filespec, not just a bare translation. */
    {
        char resolved[1024] = "";
        uint32_t st = vmsfs_to_linux_path("DISK$WORK:[000000]MOUNTTST.TXT",
                                          resolved, sizeof(resolved));
        printf("  INFO: DISK$WORK:[000000]MOUNTTST.TXT -> %s\n", resolved);
        CHECK($VMS_STATUS_SUCCESS(st), "DISK$WORK:[000000]MOUNTTST.TXT translates");
        struct stat sb;
        CHECK(stat(resolved, &sb) == 0,
              "VERACITY: DISK$WORK:[000000]MOUNTTST.TXT names the file that actually exists on the volume");
    }

    /* DISMOUNT removes it: dcl_mount_remove_disk, then F$TRNLNM is empty again. */
    {
        uint32_t rst = dcl_mount_remove_disk(mgr, LNM_PROCESS_TABLE, "WORK");
        CHECK(rst & 1, "dcl_mount_remove_disk(\"WORK\") succeeds (the DISMOUNT does)");
        char eq[256];
        trnlnm(mgr, "DISK$WORK", eq, sizeof(eq));
        CHECK(eq[0] == '\0',
              "VERACITY: F$TRNLNM(\"DISK$WORK\") is empty again after DISMOUNT removes DISK$WORK");
    }

    /* ============================================================
     * PART 2 -- concealed rooted device as a virtual disk root
     * ============================================================ */

    /* A rooted subtree base/users/... standing in for the concealed device's
     * root, plus a file AT the root (base/users/rootfile.dat). */
    char dir_users[600], file_at_root[800];
    snprintf(dir_users, sizeof(dir_users), "%s/users", base);
    snprintf(file_at_root, sizeof(file_at_root), "%s/users/rootfile.dat", base);
    mkdir(dir_users, 0755);
    { FILE *f = fopen(file_at_root, "w"); if (f) { fputs("root\n", f); fclose(f); } }

    /* USERDISK: concealed AND rooted -> VDA100:[USERS.] */
    uint32_t c = lnm_create(mgr, LNM_PROCESS_TABLE, "USERDISK",
                            "VDA100:[USERS.]", LNM_ATTR_CONCEALED, LNM_MODE_USER);
    CHECK(c == SS$_NORMAL || c == SS$_SUPERSEDE,
          "DEFINE/TRANS=CONCEALED USERDISK VDA100:[USERS.] (a rooted concealed device) succeeds");

    /* USERDISK:[000000] addresses the ROOT of the concealed device -- the
     * physical .../users -- not .../users/000000. */
    {
        char resolved[1024] = "";
        uint32_t st = vmsfs_to_linux_path("USERDISK:[000000]", resolved, sizeof(resolved));
        printf("  INFO: USERDISK:[000000] -> %s\n", resolved);
        CHECK($VMS_STATUS_SUCCESS(st), "USERDISK:[000000] translates");
        CHECK(ci_contains(resolved, "/users") && !ci_contains(resolved, "/000000"),
              "VERACITY: USERDISK:[000000] addresses the concealed device's root (.../users), no literal 000000 subdir");
        struct stat sb;
        CHECK(stat(resolved, &sb) == 0 && S_ISDIR(sb.st_mode),
              "USERDISK:[000000] names the root directory that actually exists");
    }

    /* USERDISK:[000000]ROOTFILE.DAT names a file AT the virtual disk root. */
    {
        char resolved[1024] = "";
        uint32_t st = vmsfs_to_linux_path("USERDISK:[000000]ROOTFILE.DAT",
                                          resolved, sizeof(resolved));
        printf("  INFO: USERDISK:[000000]ROOTFILE.DAT -> %s\n", resolved);
        CHECK($VMS_STATUS_SUCCESS(st), "USERDISK:[000000]ROOTFILE.DAT translates");
        CHECK(!ci_contains(resolved, "/000000"),
              "VERACITY: USERDISK:[000000]ROOTFILE.DAT does not append a literal 000000 directory");
        struct stat sb;
        CHECK(stat(resolved, &sb) == 0,
              "VERACITY: USERDISK:[000000]ROOTFILE.DAT names the file at the virtual disk root");
    }

    /* Cleanup best-effort. */
    (void)lnm_delete(mgr, LNM_PROCESS_TABLE, "USERDISK", LNM_MODE_USER);
    (void)lnm_delete(mgr, LNM_PROCESS_TABLE, "VDA100", LNM_MODE_USER);
    (void)vmsfs_device_remove("VDA100");

    printf("=== test_disk_transparency: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
