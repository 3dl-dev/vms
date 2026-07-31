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
 * ROUND 3 REWRITE, and why the round-7 replacement was ALSO vacuous.
 * sys$chkpro reads the CALLER's UIC from the live process (getuid/getgid)
 * rather than taking it as an argument, so the round-7 version could only
 * exercise the ONE category this process's REAL (gid,uid) happens to fall
 * into on whatever host runs the suite. Measured on two different hosts:
 * a container running as uid 0 lands group 0, which BOTH the old `uic==0`
 * rule and the new `group<=MAXSYSGROUP` rule call SYSTEM -- no
 * discrimination. A developer host running as an ordinary user (this one:
 * uid 1000, gid 1000) lands group 1000, which BOTH rules agree is NOT
 * system -- also no discrimination. The one credential ctest actually runs
 * as never lands in the (1, MAXSYSGROUP] band where the two rules
 * disagree, so reverting uic_is_system() to `uic == 0` left the round-7
 * test BYTE-IDENTICALLY GREEN (verified below, in the mutation this
 * comment describes).
 *
 * THE FIX: since sys$chkpro cannot be handed a synthetic caller UIC as an
 * argument, this test SYNTHESIZES ONE using an unprivileged Linux user
 * namespace (CLONE_NEWUSER + a single-line uid_map/gid_map, "deny" on
 * setgroups). That mapping requires no capability the test process does
 * not already have -- it is the same mechanism `unshare --user
 * --map-root-user` uses, and it works identically whether the outer
 * process is uid 0 or uid 1000, which is the whole point: the test now
 * drives the (1, MAXSYSGROUP] band ITSELF, in a forked child, regardless
 * of what account is running ctest.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>

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

/*
 * run_chkpro_as - fork a child, map it to a SYNTHETIC (gid, uid) via an
 * unprivileged user namespace, and have it call sys$chkpro() under that
 * identity. The status comes back through a pipe; the child's exit code
 * distinguishes a namespace-setup failure (skip) from a real answer.
 *
 * Returns 1 and fills *out_status on success, 0 if the namespace could not
 * be created on this host (e.g. unprivileged CLONE_NEWUSER disabled by
 * sysctl) -- in which case the caller must not treat that as a boolean
 * chkpro answer.
 */
static int run_chkpro_as(gid_t syn_gid, uid_t syn_uid,
                          uint32_t owner_uic, uint16_t prot, uint16_t access,
                          uint32_t *out_status)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return 0;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return 0; }

    if (pid == 0) {
        close(pipefd[0]);

        /*
         * MUST be read BEFORE unshare(): once the calling thread is in a
         * new, still-unmapped user namespace, getuid()/getgid() report the
         * overflow id (65534, "nobody") rather than the outer real id --
         * there is nothing to map back to yet. The uid_map/gid_map lines
         * below translate FROM the outer real id TO the synthetic one, so
         * the outer id has to be captured while it is still visible.
         */
        uid_t outer_uid = getuid();
        gid_t outer_gid = getgid();

        if (unshare(CLONE_NEWUSER) != 0) _exit(97);

        int fd = open("/proc/self/setgroups", O_WRONLY);
        if (fd >= 0) {
            if (write(fd, "deny", 4) < 0) { close(fd); _exit(97); }
            close(fd);
        }

        char line[64];
        int n;

        fd = open("/proc/self/uid_map", O_WRONLY);
        if (fd < 0) _exit(97);
        n = snprintf(line, sizeof(line), "%u %u 1",
                     (unsigned)syn_uid, (unsigned)outer_uid);
        if (write(fd, line, (size_t)n) < 0) { close(fd); _exit(97); }
        close(fd);

        fd = open("/proc/self/gid_map", O_WRONLY);
        if (fd < 0) _exit(97);
        n = snprintf(line, sizeof(line), "%u %u 1",
                     (unsigned)syn_gid, (unsigned)outer_gid);
        if (write(fd, line, (size_t)n) < 0) { close(fd); _exit(97); }
        close(fd);

        /* Prove the mapping actually landed before trusting the result. */
        if (getuid() != syn_uid || getgid() != syn_gid) _exit(98);

        uint32_t status = chkpro(owner_uic, prot, access);
        if (write(pipefd[1], &status, sizeof(status)) != (ssize_t)sizeof(status))
            _exit(97);
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);
    uint32_t status = 0;
    ssize_t got = read(pipefd[0], &status, sizeof(status));
    close(pipefd[0]);

    int wstatus;
    waitpid(pid, &wstatus, 0);

    if (got != (ssize_t)sizeof(status)) return 0;
    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) return 0;

    *out_status = status;
    return 1;
}

int main(void)
{
    printf("=== vms-2b8: SYSTEM protection category (sys$chkpro) ===\n");

    uint32_t me = vms$get_uic();
    printf("  outer process UIC [%o,%o] (informational only -- the "
           "discriminating cases below run in synthesized namespaces)\n",
           (unsigned)((me >> 16) & 0xFFFFu), (unsigned)(me & 0xFFFFu));

    /*
     * A mask that denies nothing to anybody must be granted regardless of
     * category -- true under any caller identity, so it needs no
     * synthesized UIC.
     */
    check((chkpro(UIC(65534, 65534), 0x0000u, PROT_READ) & 1) != 0,
          "a protection mask that denies nothing grants access");

    /* S:RWE,O:RWE,G:,W: = nothing for group or world */
    uint16_t prot_s_o_only = 0x00FFu;
    uint32_t st;
    int ok;

    /*
     * THE DISCRIMINATING CASE (round 3). A caller with a SYNTHETIC UIC
     * group inside (0, MAXSYSGROUP] -- here group 5, member 5001, neither
     * zero -- must reach a file it does NOT own, whose group differs from
     * its own, through the SYSTEM category. Under the deleted `uic == 0`
     * rule this caller (uic != 0) would fall through to the WORLD nibble
     * and be refused; under the correct `group <= MAXSYSGROUP` rule it is
     * granted via SYSTEM. This is the exact regression LOGINOUT's
     * credential drop made reachable: a SYSTEM-category account whose
     * real UIC is NOT [0,0].
     */
    /*
     * A SKIPPED discriminating case is a FAILING one here, not a pass --
     * this suite exists specifically to prove the (0, MAXSYSGROUP] band,
     * so silently accepting "the mechanism didn't run" would reintroduce
     * exactly the vacuous-test defect this round is fixing, one layer
     * up. If CLONE_NEWUSER is unavailable on a given host, that host
     * cannot run this suite honestly and must say so as a failure.
     */
    ok = run_chkpro_as(5, 5001, UIC(200, 201), prot_s_o_only, PROT_READ, &st);
    check(ok && (st & 1) != 0,
          "SYSTEM-category UIC (group 5, non-zero) reads a foreign "
          "file via SYSTEM, not WORLD");
    if (!ok)
        printf("       (CLONE_NEWUSER/uid_map/gid_map setup failed on "
               "this host -- see run_chkpro_as; treated as a failure, "
               "not skipped)\n");

    ok = run_chkpro_as(5, 5001, UIC(200, 201), prot_s_o_only, PROT_WRITE, &st);
    check(ok && (st & 1) != 0,
          "SYSTEM-category UIC (group 5, non-zero) writes a foreign "
          "file via SYSTEM, not WORLD");

    /*
     * NEGATIVE CONTROL, same mechanism: a group strictly above
     * MAXSYSGROUP (9) must NOT get the SYSTEM category -- it falls to
     * WORLD, which the mask above denies. Pins the boundary at exactly 8,
     * not "any small group".
     */
    ok = run_chkpro_as(9, 9001, UIC(200, 201), prot_s_o_only, PROT_READ, &st);
    check(ok && (st & 1) == 0,
          "group 9 (> MAXSYSGROUP) is refused a foreign file "
          "(WORLD denies)");

    /*
     * root's [0,0] is covered INCIDENTALLY (0 <= MAXSYSGROUP), not by a
     * rule of its own -- sanity check that the boundary still includes
     * the origin the deleted rule hard-coded.
     */
    ok = run_chkpro_as(0, 0, UIC(200, 201), prot_s_o_only, PROT_READ, &st);
    check(ok && (st & 1) != 0,
          "UIC [0,0] reads a foreign file via SYSTEM (incidental "
          "0 <= MAXSYSGROUP coverage)");

    /*
     * The owner category still selects on an exact match, regardless of
     * which category this process is also eligible for. Uses the real
     * (outer) UIC -- owner-match logic does not depend on which category
     * this process falls into.
     */
    check((chkpro(me, prot_s_o_only, PROT_WRITE) & 1) != 0,
          "owner category still selected on an exact UIC match");

    /*
     * The refusal side, synthesized: a caller in a non-SYSTEM group (200)
     * that does not own the object and does not share its group (owner's
     * group is 201) is refused by both group and world nibbles.
     */
    ok = run_chkpro_as(200, 300, UIC(201, 1), prot_s_o_only, PROT_READ, &st);
    check(ok && (st & 1) == 0,
          "non-owner, non-SYSTEM, non-same-group UIC refused a file "
          "with G:,W: denied (WORLD)");

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
