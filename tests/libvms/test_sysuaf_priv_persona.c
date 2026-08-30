/*
 * test_sysuaf_priv_persona.c - the login persona privilege mask comes from
 * the BINARY $UAFDEF quadword, not a lossy re-parse of the rendered name
 * string (vms-26a).
 *
 * THE REGRESSION. The atomic flip (vms-d0c) moved SYSUAF to a binary $UAFDEF
 * record read over the ACP. LOGINOUT (tools/vms_login.c) and vmssshd built the
 * persona privilege mask it stamps via vms_kif_setident() by RE-PARSING the
 * rendered privilege NAME string rec->privileges with parse_privilege_string()
 * (src/vmsprocess/include/vms/privs.h). But that string is produced by
 * sysuaf_format_privileges() from the FULL 37-name table (VMS_PRIV_NAME_LIST,
 * which HAS a MOUNT row), while parse_privilege_string()'s own table knows only
 * a 17-name subset with NO MOUNT row. The seed SYSTEM record (tools/mksysuaf.c)
 * carries uaf$q_priv = all-bits; that renders to the EXPANDED name list, never
 * the literal "ALL" the parser special-cases -- so the re-parse silently
 * dropped MOUNT (and ~19 other privileges). SYSTEM logged in without MOUNT and
 * `MOUNT VDA100:` returned %SYSTEM-F-NOPRIV.
 *
 * THE FIX. sysuaf_record_privileges() (sysuaf.h, header-inline) returns the
 * authoritative uaf$q_priv quadword straight from the binary record. This test
 * builds the SYSTEM record EXACTLY as mksysuaf.c does (view_to_raw, then the
 * all-bits mask memset), reads it back through sysuaf_raw_to_view (the path
 * sysuaf_lookup uses), and asserts:
 *
 *   1. sysuaf_record_privileges() -- the mask LOGINOUT now stamps -- carries
 *      MOUNT (and equals the on-disk all-bits quadword). THE FIX.
 *   2. The OLD path, parse_privilege_string(rec.privileges), does NOT carry
 *      MOUNT for this record. THE REGRESSION WITNESS: proves the divergence is
 *      real and that reverting the login sites to the string re-parse would
 *      reintroduce the NOPRIV.
 *
 * No privilege is faked or hardcoded: MOUNT reaches the persona only because
 * the binary SYSUAF record genuinely authorizes it (INV-6 / CLAUDE.md Rule 9).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "sysuaf.h"
#include "prvdef.h"
#include "vms/privs.h"   /* parse_privilege_string -- the OLD, lossy path */

static int failures = 0;

static void check(int cond, const char *name)
{
    if (cond) { printf("  OK: %s\n", name); }
    else      { printf("  FAIL: %s\n", name); failures++; }
}

/* Build the seed SYSTEM record byte-for-byte the way tools/mksysuaf.c does:
 * fill the view, sysuaf_view_to_raw(), then stamp the all-bits privilege
 * quadword directly (SYSTEM/"ALL"). */
static void build_system_seed(sysuaf_record_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    strncpy(rec->username, "SYSTEM", sizeof(rec->username) - 1);
    rec->uic_group  = 1;
    rec->uic_member = 4;
    strncpy(rec->default_dir, "SYS$SYSDEVICE:[SYSMGR]",
            sizeof(rec->default_dir) - 1);
    /* priv == "ALL" in mksysuaf: privileges string left empty here. */

    sysuaf_view_to_raw(rec);

    /* SYSTEM/ALL: every privilege bit set (authorized + default). */
    memset(rec->raw.uaf$q_priv, 0xff, sizeof(rec->raw.uaf$q_priv));
    memset(rec->raw.uaf$q_def_priv, 0xff, sizeof(rec->raw.uaf$q_def_priv));
}

int main(void)
{
    /* On-disk record as mksysuaf writes it. */
    sysuaf_record_t seed;
    build_system_seed(&seed);

    /* Read it back exactly as sysuaf_lookup() presents a record: raw_to_view
     * fills the text view fields (rec.privileges, the rendered name string)
     * FROM the binary record. */
    sysuaf_record_t rec;
    sysuaf_raw_to_view(&seed.raw, &rec);

    printf("=== vms-26a: persona privilege mask from binary $UAFDEF (SYSTEM) ===\n");
    printf("  rec.privileges (rendered name string) = \"%s\"\n", rec.privileges);

    uint64_t binary_mask = sysuaf_record_privileges(&rec);
    uint64_t string_mask = parse_privilege_string(rec.privileges);
    printf("  sysuaf_record_privileges() = 0x%016llx\n",
           (unsigned long long)binary_mask);
    printf("  parse_privilege_string()   = 0x%016llx (OLD lossy path)\n",
           (unsigned long long)string_mask);

    /* THE FIX: the mask LOGINOUT now stamps carries MOUNT and matches the
     * on-disk all-bits quadword. */
    check((binary_mask & PRV$M_MOUNT) != 0,
          "sysuaf_record_privileges() carries PRV$M_MOUNT (the persona MOUNT bit)");
    check(binary_mask == 0xffffffffffffffffull,
          "sysuaf_record_privileges() == the on-disk all-bits uaf$q_priv");

    /* THE REGRESSION WITNESS: the old string re-parse drops MOUNT for this
     * record, which is exactly what produced %SYSTEM-F-NOPRIV. If this ever
     * starts carrying MOUNT the divergence closed some other way -- but the
     * login sites must still use the binary mask, so keep asserting it. */
    check((string_mask & PRV$M_MOUNT) == 0,
          "parse_privilege_string() drops PRV$M_MOUNT (proves the divergence)");

    /* The rendered string does name MOUNT (the formatter's 37-name table has
     * it) -- so the loss is purely in the re-parser, not the render. */
    check(strstr(rec.privileges, "MOUNT") != NULL,
          "rendered name string names MOUNT (formatter is complete; re-parser is not)");

    if (failures) {
        printf("FAILED (%d)\n", failures);
        return 1;
    }
    printf("PASSED\n");
    return 0;
}
