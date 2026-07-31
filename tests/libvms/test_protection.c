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
 * WHAT CHANGED FROM THE PREVIOUS VERSION OF THIS TEST (vms-2b8, operator
 * ruling 2026-07-31): it used to drive both sys$chkpro AND
 * vms$check_access(), a second, parallel implementation of the same
 * category logic that src/vmsrms/rms_core.c called as an RMS pre-check.
 * vms$check_access() is DELETED (see sys_security.c's comment at the
 * deletion site): it could not enforce anything on the real runtime and
 * could only produce false denials. Testing a deleted function is not an
 * option, so this file now drives ONLY sys$chkpro -- the one function
 * that survives, and the one every category decision on OVMX now goes
 * through in one place instead of two that could (and did) disagree.
 *
 * ALSO DELETED, NOT CARRIED FORWARD: the previous version's closing
 * assertion "sys$chkpro and vms$check_access agree for this process's
 * UIC" was TAUTOLOGICAL -- both functions called the same static
 * uic_is_system() over the same arithmetic, so the assertion compared a
 * function with itself and could not have failed no matter which one (or
 * neither) was correct. It stayed green under every mutation the vms-2b8
 * round-7 adversary tried. A test that cannot fail is not coverage.
 *
 * sys$chkpro reads the CALLER's UIC from the live process (getuid/getgid)
 * rather than taking it as an argument, so this test can only exercise the
 * category sys$chkpro selects for the UID/GID it actually runs as -- it
 * cannot independently probe "SYSTEM reads a GUEST file" or "GUEST is
 * refused a SYSTEM file" the way the deleted two-argument
 * vms$check_access() could. What it CAN and does assert, discriminating on
 * whether this process's real (gid,uid) falls inside or outside
 * MAXSYSGROUP, is exactly the boundary this item fixed.
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

extern uint32_t sys$chkpro(void *objpro);
extern uint32_t vms$get_uic(void);

/* Requested-access flags and the SOGW nibble layout, as sys_security.c
 * defines them: a SET protection bit DENIES the access. */
#define PROT_READ      0x08
#define PROT_WRITE     0x04

#define UIC(g, m)      (((uint32_t)(g) << 16) | (uint32_t)(m))
#define MAXSYSGROUP    8u

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
    printf("=== vms-2b8: SYSTEM protection category (sys$chkpro) ===\n");

    uint32_t me = vms$get_uic();
    uint32_t me_group = (me >> 16) & 0xFFFFu;
    int me_is_system = (me_group <= MAXSYSGROUP);

    printf("  running as UIC [%o,%o] (group %u, %s MAXSYSGROUP=%u)\n",
           (unsigned)me_group, (unsigned)(me & 0xFFFFu), (unsigned)me_group,
           me_is_system ? "<=" : ">", MAXSYSGROUP);

    /*
     * A mask that denies nothing to anybody must be granted regardless of
     * category -- true whether or not this process is in the SYSTEM
     * category, so it holds under every ctest UID/GID this suite runs as.
     */
    check((chkpro(UIC(65534, 65534), 0x0000u, PROT_READ) & 1) != 0,
          "a protection mask that denies nothing grants access");

    /*
     * THE DISCRIMINATING CASE. If this process's real UIC group is
     * <= MAXSYSGROUP it must reach a file it does NOT own, that gives
     * group and world nothing, through the SYSTEM category. Under the
     * deleted `uic == 0` rule a non-root SYSTEM-group process (any real
     * UID other than 0) would land on the WORLD nibble here and be
     * refused -- this is exactly that regression, made reachable now that
     * LOGINOUT drops sessions off root.
     */
    uint32_t not_me_owner = UIC((me_group == 200 ? 201 : 200), 201);
    /* S:RWE,O:RWE,G:,W: = nothing for group or world */
    uint16_t prot_s_o_only = 0x00FFu;

    if (me_is_system) {
        check((chkpro(not_me_owner, prot_s_o_only, PROT_READ) & 1) != 0,
              "SYSTEM-category UIC reads a foreign file via SYSTEM, not WORLD");
        check((chkpro(not_me_owner, prot_s_o_only, PROT_WRITE) & 1) != 0,
              "SYSTEM-category UIC writes a foreign file via SYSTEM, not WORLD");
    } else {
        check((chkpro(not_me_owner, prot_s_o_only, PROT_READ) & 1) == 0,
              "non-SYSTEM-category UIC is refused a foreign file (WORLD denies)");
    }

    /* The owner category still selects on an exact match, regardless of
     * which category this process is also eligible for. */
    check((chkpro(me, prot_s_o_only, PROT_WRITE) & 1) != 0,
          "owner category still selected on an exact UIC match");

    /* The refusal side is real for a process that is NOT in the SYSTEM
     * category and does NOT own the object: group and world both deny. */
    if (!me_is_system) {
        uint32_t other_owner = UIC(me_group, (me & 0xFFFFu) == 1 ? 2 : 1);
        check((chkpro(other_owner, prot_s_o_only, PROT_READ) & 1) == 0,
              "non-owner, non-SYSTEM UIC refused a file with G:,W: (WORLD)");
    }

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
