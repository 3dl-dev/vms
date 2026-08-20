/*
 * test_accounting_veracity.c - INV-DCL veracity gate for SET ACCOUNTING
 * (vms-17d, docs/design-dcl-fidelity.md sec 5)
 *
 * THE FACADE THIS GATES. cmd_set_accounting() (src/vmsdcl/dcl_cmd_set.c)
 * used to set ctx->accounting_enabled -- a PER-DCL-CONTEXT bool no other
 * process, later session, or reboot could observe -- and print
 * "%SET-I-INTSET, accounting enabled/disabled" as if that controlled
 * anything. Meanwhile ovmx_accounting_record_login() (the writer login/SSH
 * actually calls) ran UNCONDITIONALLY, so SET ACCOUNTING/DISABLE never
 * stopped a single record from being written. INV-DCL's banned
 * fake-success class: the printed text and the SS$_NORMAL status both
 * claimed the operation took effect, and nothing did.
 *
 * WHAT THIS PROVES. This test drives the exact mechanism cmd_set_accounting()
 * now calls -- ovmx_accounting_set_enabled() -- and the exact mechanism
 * every login path calls -- ovmx_accounting_record_login() -- against a REAL
 * SYS$MANAGER: resolved through the SAME vmsfs_to_linux_path() translation
 * AUTHORIZE/LOGIN/DCL use (VMS_ACCOUNTING_STATE_PATH and OVMX_LASTLOGIN_DIR,
 * both under SYS$MANAGER:, ovmx_layout.h). It asserts the property the item
 * asked for directly: with accounting DISABLED, a login does NOT create a
 * lastlogin record; with accounting ENABLED, it DOES -- proving a real,
 * persisted, system-wide gate (INV-6: not a per-process fake).
 *
 * WHY THIS IS A HOST TEST, NOT A tests/qemu ONE. Both functions under test
 * are plain file I/O plus SYS$MANAGER: path resolution -- no /dev/vms
 * needed, same reasoning as test_sysuaf_write_veracity.c. This test
 * bootstraps its OWN private VMS namespace (a throwaway temp directory
 * registered as DKA0:, the same pattern that test uses) rather than
 * touching the real /vms mount, so it cannot race or collide with any
 * other test or a real boot's SYS$MANAGER:ACCOUNTNG.ENB.
 *
 * WHAT THIS DOES NOT COVER: the DCL-surface facade text itself (SET
 * ACCOUNTING no longer silently accepting /ENABLE=(class,...), SHOW
 * ACCOUNTING agreeing with SET ACCOUNTING) is covered separately by
 * tests/dcl/test_set_accounting_veracity.sh, which cannot see this
 * function-level property because a bare per-process bool would also make
 * that script's SET/SHOW text match up within one session. This test's
 * job is narrower: prove the flag ovmx_accounting_set_enabled() writes is
 * the SAME flag ovmx_accounting_record_login() reads, and that gate
 * genuinely suppresses the write.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>

#include "ovmx_accounting.h"
#include "vmsfs/device.h"
#include "vms/logical.h"
#include "ovmx_layout.h"

static int g_failures = 0;

/*
 * ovmx_accounting writes the per-user record as the flat file
 * SYS$MANAGER:LASTLOGIN_<USER>.DAT (lastlogin_spec, ovmx_accounting.c). Reached
 * over RMS $CREATE it lands in SYS0/SYSCOMMON/SYSMGR as lastlogin_<user>.dat
 * (vmsfs lower-cases the filename) with an RMS version suffix (";1", ...). This
 * checks the record's on-disk PRESENCE version-agnostically -- a prefix match
 * on the SYSMGR directory -- so the assertion is "a record file exists", not a
 * brittle exact-name/exact-version stat. Returns 1 if present, 0 if not. */
static int lastlogin_record_exists(const char *root, const char *user)
{
    char dirpath[1300];
    snprintf(dirpath, sizeof(dirpath),
             "%s/SYS0/SYSCOMMON/SYSMGR", root);

    char prefix[128];
    snprintf(prefix, sizeof(prefix), "lastlogin_%s.dat", user);
    for (char *p = prefix; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    DIR *d = opendir(dirpath);
    if (!d)
        return 0;
    int found = 0;
    struct dirent *e;
    size_t plen = strlen(prefix);
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, prefix, plen) == 0) {   /* name or name;VER */
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

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
    printf("test_accounting_veracity: SET ACCOUNTING gates a REAL, "
           "system-wide flag ovmx_accounting_record_login() honours "
           "(vms-17d, INV-DCL)\n");

    /* --- Isolated VMS namespace: a throwaway temp root, never the real
     * /vms mount -- same bootstrap tools/vms_authorize.c's main() and
     * test_sysuaf_write_veracity.c use. */
    char root[] = "/tmp/accounting_veracity_XXXXXX";
    if (!mkdtemp(root)) {
        fprintf(stderr, "FATAL: mkdtemp failed\n");
        return 2;
    }

    char sysmgr_dir[1100];
    snprintf(sysmgr_dir, sizeof(sysmgr_dir), "%s/SYS0/SYSCOMMON/SYSMGR", root);
    mkdir_p(sysmgr_dir);

    vmsfs_device_add(SYSDISK_DEVICE, root);
    lnm_setup_defaults(lnm_get_manager(), root);

    const char *user = "ACCTUSER";

    /* --- Pre-condition: no state file yet -> accounting defaults to
     * ENABLED (matches real OpenVMS, where accounting runs from system
     * startup unless a manager explicitly disables it -- NOT the old
     * per-process bool's implicit "disabled", which was just an
     * uninitialised zero, never a considered default). */
    check(ovmx_accounting_is_enabled() == 1,
          "pre-condition: no state file yet -> accounting defaults ENABLED "
          "(matches OpenVMS ACC$START-at-boot, not an arbitrary zero)");

    /* --- THE GATE, disabled side: SET ACCOUNTING/DISABLE's exact call,
     * then the exact call login/SSH makes. No record should appear. */
    check(ovmx_accounting_set_enabled(0) == 0,
          "ovmx_accounting_set_enabled(0) reports success");
    check(ovmx_accounting_is_enabled() == 0,
          "a fresh read (not cached state) confirms DISABLED persisted");

    check(ovmx_accounting_record_login(user) == 0,
          "record_login() while disabled reports success (never fails "
          "the caller's login just because accounting is off)");

    /* The record lands as SYS0/SYSCOMMON/SYSMGR/lastlogin_<user>.dat;VER
     * (lastlogin_spec -> RMS $CREATE; see lastlogin_record_exists). */
    check(lastlogin_record_exists(root, user) == 0,
          "POST-CHECK: no lastlogin record was written while accounting "
          "is DISABLED -- the exact property the old per-process bool "
          "could never deliver, because record_login() ran unconditionally");

    time_t t_probe = 0;
    check(ovmx_accounting_get_lastlogin(user, &t_probe) != 0,
          "ovmx_accounting_get_lastlogin() agrees: no record exists");

    /* --- THE GATE, enabled side: flip it back on, the SAME login call now
     * does write a record. */
    check(ovmx_accounting_set_enabled(1) == 0,
          "ovmx_accounting_set_enabled(1) reports success");
    check(ovmx_accounting_is_enabled() == 1,
          "a fresh read confirms ENABLED persisted");

    check(ovmx_accounting_record_login(user) == 0,
          "record_login() while enabled reports success");
    check(lastlogin_record_exists(root, user) == 1,
          "POST-CHECK: a lastlogin record WAS written now that accounting "
          "is ENABLED -- same call, only the flag changed");

    time_t t_after = 0;
    check(ovmx_accounting_get_lastlogin(user, &t_after) == 0 &&
          t_after > 0,
          "ovmx_accounting_get_lastlogin() reads back a real timestamp");

    printf("test_accounting_veracity: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
