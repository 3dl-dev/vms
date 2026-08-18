/*
 * test_sysuaf_write_veracity.c - INV-DCL veracity gate for SET PASSWORD
 * (vms-e9e, flipped to the binary $UAFDEF path by vms-d92)
 *
 * THE FACADE THIS GATES. cmd_set_password() (src/vmsdcl/dcl_cmd_set.c) used to
 * print "%SET-I-PASSWORD, password change not fully implemented" and return
 * SS$_NORMAL without touching SYSUAF -- a success-toned lie for a no-op.
 *
 * WHAT THIS PROVES. It drives the exact mechanism cmd_set_password() now calls
 * -- sysuaf_lookup() -> sysuaf_authenticate() -> sysuaf_set_password() ->
 * sysuaf_write_record() -- against a REAL binary SYSUAF.DAT resolved through the
 * SAME SYS$SYSTEM: path translation AUTHORIZE and LOGIN use. It asserts the
 * property directly: change a password, then the NEW password authenticates
 * and the OLD one does not -- a real credential change PERSISTED to the binary
 * $UAFDEF record via the Prolog-3 engine ($UPDATE over the ACP, or the POSIX
 * defer with no /dev/vms), not a per-process fake (INV-6). No ASCII, no SHA-256.
 *
 * WHY A HOST TEST. The whole path needs no /dev/vms: with the executive absent
 * the binary engine's legacy defer serves SYSUAF over POSIX (vms-5f0). The test
 * bootstraps its OWN private VMS namespace (a throwaway temp dir registered as
 * DKA0:, the pattern AUTHORIZE's main() uses) so it cannot race the real /vms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

#include "sysuaf.h"
#include "sysuaf_live.h"   /* ovmx_sysuaf_write_all -- seed the binary fixture */
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

/* Build a binary $UAFDEF record for one account with a Purdy password. */
static void seed_record(sysuaf_rms_record_t *out, const char *user,
                        uint16_t grp, uint16_t mem, const char *pw)
{
    sysuaf_record_t v;
    memset(&v, 0, sizeof(v));
    strncpy(v.username, user, sizeof(v.username) - 1);
    v.uic_group = grp; v.uic_member = mem;
    snprintf(v.default_dir, sizeof(v.default_dir),
             "SYS$SYSDEVICE:[USERS.%s]", user);
    strncpy(v.privileges, "TMPMBX,NETMBX", sizeof(v.privileges) - 1);
    sysuaf_view_to_raw(&v);
    sysuaf_set_password(&v, pw);
    *out = v.raw;
}

int main(void)
{
    printf("test_sysuaf_write_veracity: SET PASSWORD's writer persists a real "
           "credential change to the binary SYSUAF (vms-e9e/vms-d92)\n");

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

    const char *old_pw = "OldPass123";
    const char *new_pw = "NewPass456";
    const char *bystander_pw = "BystanderPW789";

    /* --- Seed the binary SYSUAF fixture: TESTUSER (the account this test
     * changes) + BYSTANDER (proves every OTHER record survives untouched). --- */
    sysuaf_rms_record_t recs[2];
    seed_record(&recs[0], "TESTUSER", 200, 205, old_pw);
    seed_record(&recs[1], "BYSTANDER", 200, 206, bystander_pw);
    uint32_t wst = ovmx_sysuaf_write_all(recs, 2);
    check((wst & 1) != 0,
          "seeded a binary $UAFDEF SYSUAF through the Prolog-3 engine");
    if (!(wst & 1)) {
        fprintf(stderr, "FATAL: could not seed the binary SYSUAF (0x%08x)\n", wst);
        return 2;
    }

    /* Capture BYSTANDER's stored password quadword for a byte-exact
     * unchanged check after TESTUSER's rewrite. */
    uint8_t bystander_pwd_before[8];
    memcpy(bystander_pwd_before, recs[1].uaf$q_pwd, 8);

    /* --- Pre-condition: the record resolves through SYS$SYSTEM: and the OLD
     * password authenticates while the NEW one does not yet. --- */
    sysuaf_record_t rec;
    check(sysuaf_lookup("TESTUSER", &rec) == 0,
          "TESTUSER resolves through SYS$SYSTEM:SYSUAF.DAT (real binary read)");
    check(sysuaf_authenticate(&rec, old_pw) == 1,
          "pre-condition: OLD password authenticates before any change");
    check(sysuaf_authenticate(&rec, new_pw) == 0,
          "pre-condition: NEW password does NOT authenticate yet");

    /* --- The change: exactly what cmd_set_password() now does after a
     * verified old-password match -- Purdy-hash the new password into the
     * record and persist it through the ONE binary writer. --- */
    check(sysuaf_set_password(&rec, new_pw) == 0, "sysuaf_set_password succeeds");
    check(sysuaf_write_record(&rec) == 0, "sysuaf_write_record reports success");

    /* --- THE VERACITY ASSERTIONS: a FRESH lookup (not the in-memory struct)
     * sees the NEW password authenticate and the OLD one refused. --- */
    sysuaf_record_t rec2;
    check(sysuaf_lookup("TESTUSER", &rec2) == 0,
          "TESTUSER still resolves after the rewrite");
    check(sysuaf_authenticate(&rec2, new_pw) == 1,
          "POST-CHANGE: the NEW password now authenticates (real credential "
          "change persisted to the binary SYSUAF)");
    check(sysuaf_authenticate(&rec2, old_pw) == 0,
          "POST-CHANGE: the OLD password no longer authenticates");

    /* --- The bystander record must be untouched: the in-place $UPDATE changed
     * only TESTUSER's record, leaving BYSTANDER's bytes (and its password)
     * exactly as seeded. --- */
    sysuaf_record_t bystander;
    check(sysuaf_lookup("BYSTANDER", &bystander) == 0,
          "BYSTANDER still resolves after TESTUSER's rewrite");
    check(sysuaf_authenticate(&bystander, bystander_pw) == 1,
          "BYSTANDER's own password is unchanged by TESTUSER's rewrite");
    check(memcmp(bystander.raw.uaf$q_pwd, bystander_pwd_before, 8) == 0,
          "BYSTANDER's stored password quadword byte-for-byte unchanged");

    /* Note (vms-d92): the retired ASCII writer REFUSED a username with no
     * matching row; the binary store (ovmx_sysuaf_store_user) is a deliberate
     * UPSERT ($UPDATE if present, else $PUT) because AUTHORIZE ADD needs to
     * insert. That is a different, intended contract, so the old
     * "refuse-unknown / never-append" negative control is not reasserted here. */

    printf("test_sysuaf_write_veracity: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
