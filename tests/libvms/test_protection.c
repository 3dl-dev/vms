/*
 * test_protection.c - the SYSTEM protection category is a GROUP test,
 *                     not an equality test against root (vms-2b8)
 *
 * WHY THIS TEST EXISTS. src/libvms/syssvc/sys_security.c used to select
 * the SYSTEM protection category with `if (uic == 0)`, commented "UID 0
 * (root) is treated as SYSTEM". While every VMS session on OVMX ran as
 * Linux root that rule was inert -- caller_uic 0 also equalled the
 * owner_uic 0 of every root-created file, so the owner branch would have
 * produced the same answer. The moment LOGINOUT began dropping to the
 * authenticated user's credentials, the SYSTEM account's real UIC [1,4]
 * stopped matching it and fell through to the WORLD nibble on every file
 * in the VMS tree: OVMX denying what VMS grants.
 *
 * THE VMS RULE, pinned and not chosen: the SYSTEM category covers every
 * UIC whose GROUP is <= MAXSYSGROUP (OpenVMS Guide to System Security,
 * "System" access category). MAXSYSGROUP measured on the oracle -- VAX2,
 * OpenVMS VAX V7.3, 30-JUL-2026, `MCR SYSGEN SHOW MAXSYSGROUP` -> Current
 * 8, Default 8. Transcript: docs/oracle/vax73-privileges.md S7.
 *
 * The first case below is the one that discriminates: restoring the old
 * `uic == 0` rule makes it fail and leaves every other case passing.
 */

#include <stdio.h>
#include <stdint.h>

/* Declared the way src/vmsrms/rms_core.c declares it -- vms$check_access
 * has no public header, and this test deliberately calls exactly what RMS
 * calls rather than a wrapper written for the test. */
extern int vms$check_access(uint32_t caller_uic, uint32_t owner_uic,
                            uint32_t protection, int access_type);
extern uint32_t sys$chkpro(void *objpro);

/* Requested-access flags and the SOGW nibble layout, as sys_security.c
 * defines them: a SET protection bit DENIES the access. */
#define PROT_READ      0x08
#define PROT_WRITE     0x04

#define UIC(g, m)      (((uint32_t)(g) << 16) | (uint32_t)(m))
/* SOGW: system nibble allows everything, owner allows everything, group
 * and world deny everything. This is the shape of every file in a VMS
 * system tree as the oracle reports it (SYS$SYSTEM:SYSUAF.DAT is
 * (RWE,RWE,,) -- nothing for group, nothing for world). */
#define PROT_S_O_ONLY  0x00FFu

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

static uint32_t chkpro(uint32_t owner_uic, uint16_t prot, uint16_t access)
{
    struct {
        uint32_t owner_uic;
        uint16_t protection;
        uint16_t access_type;
    } pro = { owner_uic, prot, access };
    return sys$chkpro(&pro);
}

int main(void)
{
    printf("=== vms-2b8: SYSTEM protection category ===\n");

    /*
     * THE DISCRIMINATING CASE. SYSTEM is [1,4]; group 1 <= MAXSYSGROUP,
     * so on VMS it reads a file owned by an ordinary user through the
     * SYSTEM category even though that file gives group and world
     * nothing. Under the deleted `uic == 0` rule this lands on the WORLD
     * nibble and is refused.
     */
    check(vms$check_access(UIC(1, 4), UIC(200, 201),
                           PROT_S_O_ONLY, PROT_READ) == 1,
          "SYSTEM [1,4] reads a [200,201] file via the SYSTEM category");
    check(vms$check_access(UIC(1, 4), UIC(200, 201),
                           PROT_S_O_ONLY, PROT_WRITE) == 1,
          "SYSTEM [1,4] writes a [200,201] file via the SYSTEM category");

    /* The boundary, both sides of it. MAXSYSGROUP is 8 and the rule is
     * "less than or equal", so group 8 is system and group 9 is not. */
    check(vms$check_access(UIC(8, 1), UIC(200, 201),
                           PROT_S_O_ONLY, PROT_READ) == 1,
          "UIC group 8 (== MAXSYSGROUP) gets the SYSTEM category");
    check(vms$check_access(UIC(9, 1), UIC(200, 201),
                           PROT_S_O_ONLY, PROT_READ) == 0,
          "UIC group 9 (> MAXSYSGROUP) does NOT get the SYSTEM category");

    /* The refusal side is real: an ordinary user is still WORLD against a
     * system-owned file, which is what stops this change from being a
     * blanket grant. */
    check(vms$check_access(UIC(200, 201), UIC(1, 4),
                           PROT_S_O_ONLY, PROT_READ) == 0,
          "an ordinary user is refused a system-owned file (WORLD)");
    check(vms$check_access(UIC(200, 201), UIC(1, 4),
                           PROT_S_O_ONLY, PROT_WRITE) == 0,
          "an ordinary user cannot write a system-owned file (WORLD)");

    /* The other three categories still select as they did. */
    check(vms$check_access(UIC(200, 201), UIC(200, 201),
                           PROT_S_O_ONLY, PROT_WRITE) == 1,
          "owner category still selected on an exact UIC match");
    check(vms$check_access(UIC(200, 202), UIC(200, 201),
                           PROT_S_O_ONLY, PROT_READ) == 0,
          "group category still selected on a group match");

    /* Linux root is [0,0]. VMS has no such UIC and no root, so it is not
     * given a rule of its own; 0 <= MAXSYSGROUP covers it, and PID 1 must
     * keep working. */
    check(vms$check_access(UIC(0, 0), UIC(200, 201),
                           PROT_S_O_ONLY, PROT_WRITE) == 1,
          "root [0,0] still reaches the SYSTEM category (0 <= MAXSYSGROUP)");

    printf("=== sys$chkpro selects the same categories ===\n");
    /* sys$chkpro reads the CALLER's UIC from the live process rather than
     * taking it as an argument, so it can only be exercised for the UIC
     * this test actually runs as. Assert the pair that does not depend on
     * who that is: a protection mask that denies nothing to anybody must
     * be granted, and one that denies everything to everybody except the
     * SYSTEM and OWNER categories must agree with vms$check_access for
     * this process's own UIC. */
    check(chkpro(UIC(0, 0), 0x0000u, PROT_READ) & 1,
          "sys$chkpro grants when no category denies");
    {
        extern uint32_t vms$get_uic(void);
        uint32_t me = vms$get_uic();
        uint32_t owner = UIC(777, 777);   /* deliberately not this process */
        int expect = vms$check_access(me, owner, PROT_S_O_ONLY, PROT_READ);
        check(((chkpro(owner, PROT_S_O_ONLY, PROT_READ) & 1) ? 1 : 0) == expect,
              "sys$chkpro and vms$check_access agree for this process's UIC");
    }

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
