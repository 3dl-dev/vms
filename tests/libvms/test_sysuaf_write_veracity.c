/*
 * test_sysuaf_write_veracity.c - INV-DCL veracity gate for SET PASSWORD
 * (vms-e9e, docs/design-dcl-fidelity.md sec 3)
 *
 * THE FACADE THIS GATES. cmd_set_password() (src/vmsdcl/dcl_cmd_set.c) used
 * to print "%SET-I-PASSWORD, password change not fully implemented" and
 * return SS$_NORMAL without touching SYSUAF at all -- a success-toned lie
 * for a no-op (INV-DCL: "a command that prints -S-/-I- while doing nothing
 * is a worse tell than an honest error, because it appears to work").
 *
 * WHAT THIS PROVES. This test drives the exact mechanism cmd_set_password()
 * now calls -- sysuaf_lookup() -> sysuaf_authenticate() -> sha256_hex() ->
 * sysuaf_write_record() -- against a REAL SYSUAF.DAT file resolved through
 * the SAME SYS$SYSTEM: path translation AUTHORIZE and LOGIN use
 * (vmsfs_to_linux_path(), src/vmslnm/lnm_defaults.c's documented
 * process-scope fallback for host tooling with no executive). It asserts
 * the property the item asked for directly: change a password, then the
 * NEW password authenticates and the OLD one does not -- proving a real
 * hash change PERSISTED TO SYSUAF, not a per-process fake (INV-6).
 *
 * WHY THIS IS A HOST TEST, NOT A tests/qemu ONE. sysuaf_write_record() is
 * plain file I/O plus the SAME SYS$SYSTEM: logical-name resolution
 * AUTHORIZE and LOGIN already rely on for host testing (see
 * lnm_seed_system_locating()'s doc comment, src/vmslnm/lnm_defaults.c) --
 * it needs no /dev/vms. This test bootstraps its OWN private VMS namespace
 * (a throwaway temp directory registered as DKA0:, exactly the pattern
 * tools/vms_authorize.c's main() uses) rather than touching the real /vms
 * mount, so it cannot race or collide with any other test or with a real
 * boot's SYSUAF.DAT.
 *
 * THE TRIPWIRE. Before this session, sysuaf_write_record() did not exist:
 * this test does not link. After it exists but before cmd_set_password()
 * is wired to it, the property below would still hold (the library
 * function works standalone) -- the DCL-level facade is what
 * tests/dcl/test_set_password_veracity.sh separately gates, from the
 * SET PASSWORD command's own output/status. This test's job is narrower
 * and more mechanical: prove the WRITER cmd_set_password() now calls
 * actually persists a hash change that flips authentication, not merely
 * that it returns 0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

#include "sysuaf.h"
#include "sha256.h"
#include "vmsfs/device.h"
#include "vms/logical.h"
#include "ovmx_layout.h"

static int g_failures = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        g_failures++;
    }
}

static void mkdir_p(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0777);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0777);
}

int main(void)
{
    printf("test_sysuaf_write_veracity: SET PASSWORD's writer persists a "
           "real hash change to SYSUAF (vms-e9e, INV-DCL)\n");

    /* --- Isolated VMS namespace: a throwaway temp root, never the real
     * /vms mount -- same bootstrap tools/vms_authorize.c's main() does
     * (vmsfs_device_add + lnm_setup_defaults), pointed at a private
     * directory so this test cannot race or collide with anything else. */
    char root[] = "/tmp/sysuaf_write_veracity_XXXXXX";
    if (!mkdtemp(root)) {
        fprintf(stderr, "FATAL: mkdtemp failed\n");
        return 2;
    }

    char sysexe_dir[1100];
    snprintf(sysexe_dir, sizeof(sysexe_dir), "%s/SYS0/SYSCOMMON/SYSEXE", root);
    mkdir_p(sysexe_dir);

    vmsfs_device_add(SYSDISK_DEVICE, root);
    lnm_setup_defaults(lnm_get_manager(), root);

    /* --- Seed a SYSUAF.DAT fixture with two accounts: TESTUSER (the one
     * this test changes) and BYSTANDER (proves every OTHER row survives
     * the targeted rewrite untouched -- sysuaf_write_record()'s own
     * contract). */
    const char *old_pw = "OldPass123";
    const char *new_pw = "NewPass456";
    const char *bystander_pw = "BystanderPW789";

    char old_hash[65], bystander_hash[65];
    sha256_hex((const uint8_t *)old_pw, strlen(old_pw), old_hash);
    sha256_hex((const uint8_t *)bystander_pw, strlen(bystander_pw),
               bystander_hash);

    char sysuaf_path[1200];
    snprintf(sysuaf_path, sizeof(sysuaf_path), "%s/SYSUAF.DAT", sysexe_dir);

    FILE *fp = fopen(sysuaf_path, "w");
    if (!fp) {
        fprintf(stderr, "FATAL: cannot create fixture %s\n", sysuaf_path);
        return 2;
    }
    fprintf(fp, "# fixture SYSUAF.DAT for test_sysuaf_write_veracity\n");
    fprintf(fp, "TESTUSER|%s|200|205|SYS$SYSDEVICE:[USERS.TESTUSER]||"
                "TMPMBX,NETMBX\n", old_hash);
    fprintf(fp, "BYSTANDER|%s|200|206|SYS$SYSDEVICE:[USERS.BYSTANDER]||"
                "TMPMBX,NETMBX\n", bystander_hash);
    fclose(fp);

    /* --- Pre-condition: the file resolves through the real SYS$SYSTEM:
     * translation, and the fixture reads back as written. */
    sysuaf_record_t rec;
    check(sysuaf_lookup("TESTUSER", &rec) == 0,
          "TESTUSER resolves through SYS$SYSTEM:SYSUAF.DAT (real path "
          "translation, not a bypassed file)");
    check(sysuaf_authenticate(&rec, old_pw) == 1,
          "pre-condition: OLD password authenticates before any change");
    check(sysuaf_authenticate(&rec, new_pw) == 0,
          "pre-condition: NEW password does NOT authenticate yet");

    /* --- The change: exactly what cmd_set_password() now does after a
     * verified old-password match -- hash the new password and call the
     * ONE shared writer. */
    char new_hash[65];
    sha256_hex((const uint8_t *)new_pw, strlen(new_pw), new_hash);
    strncpy(rec.password_hash, new_hash, sizeof(rec.password_hash) - 1);
    rec.password_hash[sizeof(rec.password_hash) - 1] = '\0';

    int wrc = sysuaf_write_record(&rec);
    check(wrc == 0, "sysuaf_write_record() reports success");

    /* --- THE VERACITY ASSERTIONS (the item's own acceptance test): a
     * fresh lookup (not the in-memory struct) sees the NEW password
     * authenticate and the OLD one refused -- a real, persisted change,
     * not a per-process fake (INV-6). */
    sysuaf_record_t rec2;
    check(sysuaf_lookup("TESTUSER", &rec2) == 0,
          "TESTUSER still resolves after the rewrite");
    check(sysuaf_authenticate(&rec2, new_pw) == 1,
          "POST-CHANGE: the NEW password now authenticates (real hash "
          "change persisted to SYSUAF)");
    check(sysuaf_authenticate(&rec2, old_pw) == 0,
          "POST-CHANGE: the OLD password no longer authenticates");

    /* --- The bystander row must be untouched: proves the targeted
     * rewrite did not corrupt or drop a neighbour (sysuaf_write_record()'s
     * documented contract, sysuaf.h). */
    sysuaf_record_t bystander;
    check(sysuaf_lookup("BYSTANDER", &bystander) == 0,
          "BYSTANDER still resolves after TESTUSER's rewrite");
    check(sysuaf_authenticate(&bystander, bystander_pw) == 1,
          "BYSTANDER's own password is unchanged by TESTUSER's rewrite");
    check(strcmp(bystander.password_hash, bystander_hash) == 0,
          "BYSTANDER's stored hash byte-for-byte unchanged");

    /* --- Negative control: sysuaf_write_record() for a username that is
     * NOT in the file must fail and must not touch the file (matches the
     * documented contract -- "no matching username" is a -1, unchanged
     * file, never a silent append). */
    sysuaf_record_t ghost;
    memset(&ghost, 0, sizeof(ghost));
    strncpy(ghost.username, "NOSUCHUSER", sizeof(ghost.username) - 1);
    strncpy(ghost.password_hash, "deadbeef", sizeof(ghost.password_hash) - 1);
    check(sysuaf_write_record(&ghost) != 0,
          "sysuaf_write_record() refuses a username with no matching row "
          "(never silently appends)");
    check(sysuaf_lookup("NOSUCHUSER", &rec) != 0,
          "the refused write did not create a NOSUCHUSER row");

    printf("test_sysuaf_write_veracity: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
