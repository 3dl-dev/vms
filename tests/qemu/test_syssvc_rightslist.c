/*
 * test_syssvc_rightslist.c - F$IDENTIFIER's source is the rights database
 * (vms-2f8), proven against a real /dev/vms (vms-d37).
 *
 * MIGRATED FROM tests/libvms/test_rightslist.c. The host version staged the
 * shipped RIGHTSLIST.DAT/SYSUAF.DAT under a private root, pointed VDA0: at it,
 * and located SYS$SYSTEM:RIGHTSLIST.DAT by resolving the SYS$SYSTEM logical IN
 * THIS PROCESS -- which only worked because lnm_seed_system_locating() reseeded
 * the executive-resident LNM$SYSTEM names into LNM$PROCESS_TABLE when there was
 * no executive. That process-scope fallback is the host-LNM fake CLAUDE.md
 * Rule 9 / INV-6 forbids and vms-fk1 deletes; nothing may depend on it. So this
 * assertion now runs where SYS$SYSTEM is genuinely executive-resident -- the
 * booted QEMU guest -- reading the ACTUAL SHIPPED files at SYS$SYSTEM: (no
 * staged fixture, no argv: /vms in the guest IS the product's own system disk),
 * and honest-skips (77) with no /dev/vms like every other test_syssvc_* suite.
 *
 * WHAT THIS ASSERTS. OVMX shipped SYS$SYSTEM:RIGHTSLIST.DAT from the day the
 * boot path provisioned it, and nothing read it: F$IDENTIFIER answered from two
 * hardcoded names. This drives the reader that replaced them against what
 * boots, so the assertion cannot drift from the shipped data.
 *
 * THE ORACLE is docs/oracle/vax73-rights-database.md -- OpenVMS VAX V7.3, lab
 * node VAX1, 2026-08-05, live DCL plus AUTHORIZE SHOW/IDENTIFIER/FULL. Every
 * value below is measured there. None is chosen here.
 *
 * THE MISSES ON 1..5 ARE ASSERTIONS, NOT FILLER. The shipped RIGHTSLIST.DAT
 * used to read INTERACTIVE:1 BATCH:2 NETWORK:3 LOCAL:4 REMOTE:5, and on real
 * VMS not one of 1..5 is an identifier at all -- each answers the null string.
 * They distinguish "reads the rights database" from "reads the rights database
 * and the rights database is right".
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

#include "rightslist.h"
#include "starlet.h"
#include "descrip.h"
#include "chpdef.h"
#include "armdef.h"
#include "iledef.h"
#include "rmsdef.h"

/* $CREATE_USER_PROFILE walks the holder records with $FIND_HELD (LIBVMSRMS);
 * a strong reference links the reader into this static suite. */
uint32_t sys$find_held(const uint32_t *holder, uint32_t *id, uint32_t *attrib, uint32_t *contxt);
static uint32_t (*volatile const keep_find_held)(const uint32_t *, uint32_t *, uint32_t *,
                                                 uint32_t *) = sys$find_held;
#include "ssdef.h"
#include "ovmx_layout.h"
#include "vmsfs/device.h"
#include "vms/logical.h"
#include "vms_kif.h"

#define EXIT_SKIP 77
#define ODS2_UNIT "VDA300:"    /* vdd: the generated system-disk ODS-2 fixture */

/* Seed the concealed-rooted system logicals pointed at ODS2_UNIT into the
 * process table (identical to test_syssvc_dirlogical_acp.c). This is what the
 * RMS-over-ACP $OPEN composes, so rightslist_name_to_value() reads the REAL
 * shipped RIGHTSLIST.DAT (general identifiers) and sysuaf_lookup_by_uic() reads
 * the REAL SYSUAF.DAT (UIC identifiers) off the mounted ODS-2 volume through the
 * ACP -- not any /vms passthrough. */
static void seed_system_logicals(lnm_manager_t *mgr)
{
    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$SYSDEVICE", ODS2_UNIT,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);
    static const char *sysroot[] = {
        "SYS$SYSDEVICE:[SYS0.]",
        "SYS$SYSDEVICE:[SYS0.SYSCOMMON.]"
    };
    lnm_create_multi(mgr, LNM_PROCESS_TABLE, "SYS$SYSROOT", sysroot, 2,
                     LNM_ATTR_CONCEALED, LNM_MODE_EXEC);
    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$SYSTEM",  "SYS$SYSROOT:[SYSEXE]", 0, LNM_MODE_EXEC);
    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$MANAGER", "SYS$SYSROOT:[SYSMGR]", 0, LNM_MODE_EXEC);
    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$LIBRARY", "SYS$SYSROOT:[SYSLIB]", 0, LNM_MODE_EXEC);
}

static int pass = 0;
static int fail = 0;

static void check(int cond, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void check(int cond, const char *fmt, ...)
{
    char label[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(label, sizeof(label), fmt, ap);
    va_end(ap);

    if (cond) {
        printf("  PASS: %s\n", label);
        pass++;
    } else {
        printf("  FAIL: %s\n", label);
        fail++;
    }
}

/* name -> value, and the value -> name round trip the oracle shows. */
static void check_roundtrip(const char *name, uint32_t want, const char *note)
{
    uint32_t got = 0;
    if (rightslist_name_to_value(name, &got) != 0) {
        check(0, "%s: resolved at all (%s)", name, note);
        return;
    }
    if (got != want)
        printf("    got %u (%%X%08X), wanted %u (%%X%08X)\n",
               got, got, want, want);
    check(got == want, "%s -> %u (%%X%08X) -- %s", name, want, want, note);

    char back[RIGHTSLIST_NAME_MAX] = {0};
    if (rightslist_value_to_name(want, back, sizeof(back)) != 0) {
        check(0, "%u -> \"%s\": reverse resolves (oracle round-trips it)",
              want, name);
        return;
    }
    check(strcmp(back, name) == 0,
          "%u -> \"%s\" (reverse; got \"%s\")", want, name, back);
}

static void check_miss_value(uint32_t value, const char *why)
{
    char buf[RIGHTSLIST_NAME_MAX] = {0};
    int rc = rightslist_value_to_name(value, buf, sizeof(buf));
    if (rc == 0)
        printf("    resolved to \"%s\"\n", buf);
    check(rc != 0, "%u is NOT an identifier -- %s", value, why);
}

/* Probe only -- vms_kif_open()/close() decide skip-vs-run and nothing else. */
static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

int main(void)
{
    printf("=== test_syssvc_rightslist: F$IDENTIFIER reads the rights database "
           "(vms-2f8/vms-d37) ===\n");

    /*
     * This suite requires a real executive: the rights-database reads are only
     * meaningful when SYS$SYSTEM composes through the concealed-rooted chain and
     * the Files-11 ACP reads the shipped RIGHTSLIST.DAT / SYSUAF.DAT off a
     * MOUNTED ODS-2 volume. With no /dev/vms it honest-skips (77), never a fake
     * pass -- the contract every test_syssvc_* is held to (.github/workflows/
     * ci.yml). Nothing is asserted on the skip path.
     */
    if (!executive_present()) {
        printf("=== test_syssvc_rightslist: 0 passed, 0 failed "
               "(SKIPPED: no /dev/vms -- the rights-database reads need the "
               "Files-11 ACP on a mounted ODS-2 volume) ===\n");
        return EXIT_SKIP;
    }

    /*
     * Bootstrap the VMS namespace the way the booted system does: seed the
     * concealed-rooted system logicals on VDA300: and $MOUNT the generated
     * system-disk ODS-2 fixture. rightslist_name_to_value() then opens
     * SYS$SYSTEM:RIGHTSLIST.DAT -> RMS-over-ACP $OPEN composes
     * VDA300:[SYS0.SYSCOMMON.SYSEXE]RIGHTSLIST.DAT and reads the REAL shipped
     * rows off the ODS-2 platter; UIC identifiers derive from SYSUAF.DAT read the
     * same way. NO /vms passthrough (Rule 9 / INV-6).
     */
    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { check(0, "no LNM manager"); goto done; }
    seed_system_logicals(mgr);

    uint32_t mst = vms_kif_acp_mount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(mst),
          "$MOUNT of the system-disk ODS-2 fixture on " ODS2_UNIT " (precondition)");
    if (!$VMS_STATUS_SUCCESS(mst)) goto done;

    /* PROVENANCE: a general identifier resolves while mounted, and the same read
     * FAILS honestly once DISMOUNTED -- so the rows below came off the ACP
     * volume, never a POSIX substitute. Remounted for the assertions. */
    {
        uint32_t v = 0;
        check(rightslist_name_to_value("BATCH", &v) == 0 && v == 0x80000001u,
              "BATCH reads via RMS-over-ACP off the mounted ODS-2 volume");
        vms_kif_acp_dmount(ODS2_UNIT);
        uint32_t v2 = 0;
        check(rightslist_name_to_value("BATCH", &v2) != 0,
              "with the volume DISMOUNTED the rights read fails-honest (no /vms fallback)");
        (void)vms_kif_acp_mount(ODS2_UNIT);
    }

    /* --- general identifiers, from RIGHTSLIST.DAT --------------------- */
    printf("\n general identifiers (RIGHTSLIST.DAT)\n");
    check_roundtrip("BATCH",       0x80000001u, "oracle: DCL prints -2147483647");
    check_roundtrip("DIALUP",      0x80000002u, "oracle: absent from OVMX entirely before vms-2f8");
    check_roundtrip("INTERACTIVE", 0x80000003u, "oracle: DCL prints -2147483645");
    /* negctl: rightslist-general-hex-as-decimal */
    check_roundtrip("LOCAL",       0x80000004u, "oracle: DCL prints -2147483644");
    check_roundtrip("NETWORK",     0x80000005u, "oracle: DCL prints -2147483643");
    check_roundtrip("REMOTE",      0x80000006u, "oracle: DCL prints -2147483642");

    /* --- UIC identifiers, from RIGHTSLIST (vms-930) ------------------- *
     * These now resolve from the world-readable RIGHTSLIST.DAT (one UIC
     * identifier per account, mkrightslist.c), NOT from the protected SYSUAF:
     * the values are unchanged, the SOURCE moved so an unprivileged caller can
     * resolve them without reading SYSUAF. */
    printf("\n UIC identifiers (from RIGHTSLIST.DAT)\n");

    /* DEFAULT DISCRIMINATES THE BASE as well as the source: [200,200] read
     * as octal is 8388736, the oracle's answer; read as decimal it is
     * 13107400 and matches nothing (vms-e60). */
    check_roundtrip("DEFAULT", 8388736u,
                    "oracle %X00800080 = [200,200] OCTAL; decimal gives 13107400");

    /* SYSTEM is a LIVENESS ANCHOR here, not a discriminator: [1,4] is the
     * same number in both bases and cannot distinguish anything about how
     * the UIC was read. Labelled so nobody later mistakes it for the check
     * that discriminates and deletes the one that does. */
    check_roundtrip("SYSTEM", 65540u,
                    "LIVENESS ONLY: identical in both bases, proves no base");

    /* An account with no hardcode anywhere, which is the point: before this
     * change GUEST had no identifier at all in either direction. */
    check_roundtrip("GUEST", (0200u << 16) | 0201u,
                    "a shipped account that was never in any hardcoded table");

    /* --- THE DISCRIMINATING CHECKS ------------------------------------ */
    printf("\n the values the OLD shipped RIGHTSLIST.DAT assigned\n");
    check_miss_value(1, "was INTERACTIVE:1; oracle answers the null string");
    check_miss_value(2, "was BATCH:2; oracle answers the null string");
    check_miss_value(3, "was NETWORK:3; oracle answers the null string");
    check_miss_value(4, "was LOCAL:4; oracle answers the null string");
    check_miss_value(5, "was REMOTE:5; oracle answers the null string");

    /* --- misses, pinned ----------------------------------------------- */
    printf("\n misses\n");
    check_miss_value(1000, "oracle: F$IDENTIFIER(1000,\"NUMBER_TO_NAME\") -> \"\"");
    check_miss_value(0, "oracle: F$IDENTIFIER(0,\"NUMBER_TO_NAME\") -> \"\"");
    check_miss_value(77777, "oracle: F$IDENTIFIER(77777,\"NUMBER_TO_NAME\") -> \"\"");
    check_miss_value(196609, "oracle: F$IDENTIFIER(196609,\"NUMBER_TO_NAME\") -> \"\"");
    check_miss_value(0x80010004u,
                     "oracle: F$IDENTIFIER(%X80010004,\"NUMBER_TO_NAME\") -> \"\"");

    uint32_t v = 0xDEADBEEFu;
    check(rightslist_name_to_value("NOSUCHIDENT", &v) != 0,
          "\"NOSUCHIDENT\" does not resolve (caller renders this as 0)");

    /* Case-insensitive, as DCL upcases before asking. */
    uint32_t lower = 0;
    check(rightslist_name_to_value("local", &lower) == 0 &&
          lower == 0x80000004u,
          "lookup is case-insensitive (\"local\" resolves like \"LOCAL\")");

    /* --- name the old defect explicitly, so a regression says so ------- */
    printf("\n regression naming\n");
    uint32_t loc = 0;
    if (rightslist_name_to_value("LOCAL", &loc) == 0) {
        check(loc != 4u,
              "LOCAL is NOT 4 (the value the shipped file invented for it)");
    } else {
        check(0, "LOCAL resolves, for the regression check");
    }

    /* --- $CREATE_USER_PROFILE + $CHKPRO (vms-d404) ------------------- *
     * The DEFAULT account's profile ([200,200] from the shipped SYSUAF.DAT)
     * as $CHKPRO's subject. Statuses as OpenVMS VAX V7.3 answers the same
     * calls (docs/oracle/semantics/chkpro/vax73.txt, UP.* / CUP.*). */
    printf("\n $CREATE_USER_PROFILE / $CHKPRO\n");
    {
        static char dn[] = "DEFAULT", nn[] = "SP_NO_SUCH_USER";
        struct dsc$descriptor_s dd = { sizeof(dn) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, dn };
        struct dsc$descriptor_s nd = { sizeof(nn) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, nn };
        static uint8_t pro[4096];
        uint32_t plen = 0, st;
        struct dsc$descriptor_s pd;
        uint32_t acc = ARM$M_READ, own = 0, prot = 0;
        uint8_t ace[12] = { 12, 1, 0, 0, ARM$M_READ, 0, 0, 0, 0x80, 0, 0x80, 0 };  /* [200,200] READ */
        ILE3 it[5], ia[6];

        (void)keep_find_held;
        st = sys$create_user_profile(&dd, NULL, 0, NULL, &plen, NULL);
        check(st == SS$_NORMAL && plen > 0 && plen <= sizeof(pro),
              "$CREATE_USER_PROFILE DEFAULT, no buffer: SS$_NORMAL and the length (CUP.LENGTH)");
        st = sys$create_user_profile(&dd, NULL, 0, pro, &plen, NULL);
        check(st == SS$_NORMAL, "$CREATE_USER_PROFILE DEFAULT: SS$_NORMAL (CUP.DEFAULT)");
        check(sys$create_user_profile(&nd, NULL, 0, pro, &plen, NULL) == RMS$_RNF,
              "$CREATE_USER_PROFILE of an unknown user is RMS$_RNF (CUP.NOSUCH)");

        memset(&pd, 0, sizeof(pd));
        pd.dsc$w_length = (uint16_t)plen;
        pd.dsc$a_pointer = (char *)pro;
        memset(it, 0, sizeof(it));
        it[0].ile3$w_length = 4; it[0].ile3$w_code = CHP$_ACCESS; it[0].ile3$ps_bufaddr = &acc;
        it[1].ile3$w_length = 4; it[1].ile3$w_code = CHP$_OWNER;  it[1].ile3$ps_bufaddr = &own;
        it[2].ile3$w_length = 4; it[2].ile3$w_code = CHP$_PROT;   it[2].ile3$ps_bufaddr = &prot;
        memcpy(ia, it, sizeof(it));
        ia[3].ile3$w_length = 12; ia[3].ile3$w_code = CHP$_ACL; ia[3].ile3$ps_bufaddr = ace;
        memset(&ia[4], 0, 2 * sizeof(ILE3));

        own = (1u << 16) | 4u; prot = 0xFF00;            /* [1,4] S:RWED,O:RWED,G,W */
        /* negctl: create-user-profile-uic-dropped */
        check(sys$chkpro(it, NULL, &pd) == SS$_NOPRIV,
              "$CHKPRO: DEFAULT is world to a [1,4] S:RWED,O:RWED,G,W object -- read refused (UP.WORLD.NONE.READ)");
        /* negctl: chkpro-acl-ignored */
        check(sys$chkpro(ia, NULL, &pd) == SS$_NORMAL,
              "$CHKPRO: an ACE (IDENTIFIER=[200,200],ACCESS=READ) grants DEFAULT read (UP.ACL.GRANT)");
        own = (0200u << 16) | 0200u; prot = 0xFF0F;      /* [200,200] S,O:RWED,G,W */
        /* negctl: create-user-profile-uic-dropped */
        check(sys$chkpro(it, NULL, &pd) == SS$_NORMAL,
              "$CHKPRO: DEFAULT owns a [200,200] S,O:RWED,G,W object -- read granted (UP.OWNER.RWED.READ)");
    }

    vms_kif_acp_dmount(ODS2_UNIT);

done:
    printf("=== test_syssvc_rightslist: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
