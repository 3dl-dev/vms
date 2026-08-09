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
 * "System" access category). MAXSYSGROUP=8, pinned to TWO independent
 * sources (vms-2b8 round 4; round 3 had only the first, which was this
 * same branch attesting to its own capture -- see
 * src/libvms/include/ovmx_secparam.h, the SINGLE definition both this
 * file and sys_security.c include as of round 5, for the full account):
 *   1. Oracle transcript, VAX2, OpenVMS VAX V7.3, 30-JUL-2026,
 *      `MCR SYSGEN SHOW MAXSYSGROUP` -> Current 8, Default 8. Transcript:
 *      docs/oracle/vax73-privileges.md S7.
 *   2. VSI OpenVMS Wiki, "UIC Protection", fetched 31-JUL-2026: MAXSYSGROUP
 *      is octal 10 by default -- decimal 8 -- from a source independent of
 *      both this tree and the lab.
 * THE BOUNDARY IS ALSO PROVEN BY MUTATION, not just pinned by citation: the
 * round-3 version of this test could not tell MAXSYSGROUP=8 apart from
 * MAXSYSGROUP=5, because its only two cases were group 5 (inside (0,8] AND
 * inside (0,5]) and group 9 (outside both). This version adds a case at
 * the boundary ITSELF (group == MAXSYSGROUP, defined below) plus a
 * compile-time `_Static_assert` on the shared constant; together with the
 * group-9 negative control, the three now agree only when the boundary is
 * exactly 8 -- see those tests' own comments (and the round-5 correction
 * on the boundary case: round 4's claim that it alone proved "exactly 8"
 * was a false "only" it had not tried to break by mutation).
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
 * argument, this test SYNTHESIZES ONE in a forked child, regardless of what
 * account is running ctest, using whichever of two mechanisms this host
 * supports (see run_chkpro_as / synthesize_direct / synthesize_userns
 * below): real setgid()+setuid() if the process holds CAP_SETGID/CAP_SETUID
 * (root, or the file capabilities CI grants this binary), or else an
 * unprivileged Linux user namespace (CLONE_NEWUSER + a single-line
 * uid_map/gid_map, "deny" on setgroups -- the same mechanism `unshare
 * --user --map-root-user` uses).
 *
 * BOTH ARE NEEDED, MEASURED (vms-2b8 round 12): the user-namespace path
 * alone left this suite unable to run honestly at all on the GitHub Actions
 * "Build & Test" job -- Ubuntu 24.04 restricts unprivileged CLONE_NEWUSER
 * by AppArmor default, so all six discriminating assertions below failed
 * with "CLONE_NEWUSER/uid_map/gid_map setup failed on this host", not
 * skipped (per this suite's own rule that a skipped discriminating case is
 * a failing one). Reproduced locally by disabling unprivileged user
 * namespaces (`sysctl user.max_user_namespaces=0`) and confirmed fixed by
 * granting the two capabilities to this test's binary instead of relying
 * on the namespace.
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
#include <sys/stat.h>
#include "ovmx_secparam.h"
#include "ovmx_fileprot.h"

extern uint32_t sys$chkpro(void *objpro);
extern uint32_t vms$get_uic(void);
extern uint16_t vmsfs_mode_to_protection(mode_t mode);

/* Requested-access flags, aliased onto the single pinned encoding in
 * ovmx_fileprot.h (vms-f81) rather than hand-copied -- a SET protection
 * bit DENIES the access. Round 5 of this comment said "as sys_security.c
 * defines them", which was itself the bug: sys_security.c's PROT$M_READ
 * used to be 0x08 (the DELETE bit under the pinned encoding), and this
 * file copied that wrong value, so the two could never disagree no
 * matter how sys$chkpro's shift order drifted. Both now derive from the
 * one grounded header. */
#define PROT_READ      VMS_PROT_R
#define PROT_WRITE     VMS_PROT_W

#define UIC(g, m)      (((uint32_t)(g) << 16) | (uint32_t)(m))

/* MAXSYSGROUP is NOT redefined here (vms-2b8 round 5): it is the same
 * OVMX_MAXSYSGROUP constant sys_security.c enforces, from the single
 * pinned definition in src/libvms/include/ovmx_secparam.h. Round 4 had
 * two hand-maintained copies of the literal 8 (this file and
 * sys_security.c) that happened to agree by coincidence, not by
 * construction -- an editor changing one had no way to know the other
 * existed. This alias keeps the rest of the file's `MAXSYSGROUP` call
 * sites unchanged while making the numeric value a byproduct of one
 * #define instead of two.
 *
 * SELF-CAUGHT REGRESSION, same round: aliasing MAXSYSGROUP straight to
 * OVMX_MAXSYSGROUP would make the "boundary itself" test below
 * SELF-REFERENTIAL -- it drives run_chkpro_as(MAXSYSGROUP, ...), so
 * whatever OVMX_MAXSYSGROUP is currently defined as, that call always
 * probes exactly the production boundary and is granted by definition.
 * That still catches a `<` vs `<=` regression in uic_is_system() (an
 * off-by-one at the boundary), but it can NO LONGER catch OVMX_MAXSYSGROUP
 * itself drifting away from the oracle-pinned value 8 -- exactly the
 * hazard "derive it once" was supposed to close, reintroduced by closing
 * it carelessly. Verified by mutation: with only the alias (no assertion
 * below), changing ovmx_secparam.h's value to 9 left the boundary-itself
 * case GREEN (7/8, only the independently-hardcoded group-9 negative
 * control went red) -- proof the alias alone had silently stopped pinning
 * the number.
 *
 * FIX: a compile-time pin, the same pattern this codebase already uses
 * for other oracle-sourced constants (e.g. dcl_lexical.c's
 * `_Static_assert(SS$_ILLIOFUNC == 244, ...)`, vms_ioctl.h's PRV$V_WORLD
 * assert). If OVMX_MAXSYSGROUP is ever changed without updating this
 * test's own literal probe values (5, 8's replacement via the alias, and
 * 9), the BUILD fails here, loudly, instead of the runtime test silently
 * tracking whatever the new value is.
 */
#define MAXSYSGROUP    OVMX_MAXSYSGROUP

_Static_assert(OVMX_MAXSYSGROUP == 8,
               "MAXSYSGROUP moved away from the oracle-pinned value 8 "
               "(src/libvms/include/ovmx_secparam.h) -- this test's group "
               "5/9 probe values were chosen around 8 specifically and "
               "must be re-picked, not silently carried forward, if the "
               "pin is ever legitimately re-derived");

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
 * protection_for_mode - create a REAL file at `unix_mode`, run it through
 * vmsfs_mode_to_protection() (src/vmsfs/vmsfs_protect.c -- the producer
 * side of vms-f81's cross-module contract), and hand back the resulting
 * VMS protection word. Exits the process on any setup failure: a helper
 * that silently returned garbage on error would make the caller's
 * assertions meaningless.
 */
static uint16_t protection_for_mode(mode_t unix_mode)
{
    char path[] = "/tmp/ovmx_test_protection_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("protection_for_mode: mkstemp");
        exit(1);
    }
    if (fchmod(fd, unix_mode) != 0) {
        perror("protection_for_mode: fchmod");
        close(fd);
        unlink(path);
        exit(1);
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        perror("protection_for_mode: fstat");
        close(fd);
        unlink(path);
        exit(1);
    }
    close(fd);
    unlink(path);
    return vmsfs_mode_to_protection(st.st_mode);
}

/*
 * fork_and_chkpro_as - fork a child that runs `synthesize` to become
 * (syn_gid, syn_uid) and, if it succeeds, calls sys$chkpro() under that
 * identity and reports the status back through a pipe.
 *
 * `synthesize` returns 1 if the child is now really running as
 * (syn_gid, syn_uid) (verified by the caller via getuid()/getgid(), not
 * trusted from the mechanism's own return code), 0 if this host does not
 * support the mechanism it tried. Either way it must leave the CHILD
 * process's own credentials as its only side effect -- it runs in a
 * throwaway fork(), so there is nothing to undo.
 *
 * Returns 1 and fills *out_status on success, 0 if `synthesize` could not
 * establish the identity on this host -- in which case the caller must not
 * treat that as a boolean chkpro answer.
 */
static int fork_and_chkpro_as(int (*synthesize)(gid_t, uid_t),
                               gid_t syn_gid, uid_t syn_uid,
                               uint32_t owner_uic, uint16_t prot,
                               uint16_t access, uint32_t *out_status)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return 0;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return 0; }

    if (pid == 0) {
        close(pipefd[0]);

        if (!synthesize(syn_gid, syn_uid)) _exit(97);
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

/*
 * synthesize_direct - become (syn_gid, syn_uid) with plain setgid()+setuid(),
 * no namespace. Succeeds only if this process already holds CAP_SETGID and
 * CAP_SETUID (root, or file capabilities -- see the "Grant..." step in
 * .github/workflows/ci.yml, which sets exactly these two on this test's own
 * binary so the GitHub-hosted runner can run this suite honestly). This is
 * the SAME mechanism tests/qemu/test_syssvc_ident.c's run_dcl(drop=1) uses
 * under QEMU (which runs as root); here it is reached by capability grant
 * instead of by uid.
 *
 * On a process without those capabilities both calls fail EPERM and change
 * nothing -- setgid()/setuid() are each atomic (all-or-nothing), so a failed
 * attempt here never leaves the child in a half-changed state for the
 * synthesize_userns fallback below to inherit. (It cannot inherit anything
 * anyway: each mechanism gets its OWN fresh fork() from
 * fork_and_chkpro_as(), which is the only reason this order-independence is
 * true by construction rather than by care.)
 */
static int synthesize_direct(gid_t syn_gid, uid_t syn_uid)
{
    return setgid(syn_gid) == 0 && setuid(syn_uid) == 0;
}

/*
 * synthesize_userns - become (syn_gid, syn_uid) via an unprivileged user
 * namespace (CLONE_NEWUSER + a single-line uid_map/gid_map, "deny" on
 * setgroups). That mapping requires no capability this process does not
 * already have -- it is the same mechanism `unshare --user
 * --map-root-user` uses. FALLBACK ONLY (vms-2b8 round 12): Ubuntu 24.04
 * restricts unprivileged CLONE_NEWUSER via AppArmor by default, which is
 * exactly the GitHub Actions ubuntu-latest runner this suite's "Build &
 * Test" CI job uses -- MEASURED: this suite's six discriminating
 * assertions failed there with "CLONE_NEWUSER/uid_map/gid_map setup
 * failed on this host" while synthesize_direct's file-capability grant
 * (below) reproduced the same failure locally (temporarily
 * `sysctl user.max_user_namespaces=0`) and fixed it. synthesize_userns
 * stays as the path for a host that has neither root nor the granted
 * capabilities but does allow unprivileged user namespaces (many
 * developer machines, by default kernel policy).
 */
static int synthesize_userns(gid_t syn_gid, uid_t syn_uid)
{
    /*
     * MUST be read BEFORE unshare(): once the calling thread is in a
     * new, still-unmapped user namespace, getuid()/getgid() report the
     * overflow id (65534, "nobody") rather than the outer real id --
     * there is nothing to map back to yet. The uid_map/gid_map lines
     * below translate FROM the outer real id TO the synthetic one, so
     * the outer id has to be captured while it is still visible. This
     * child was JUST forked (fork_and_chkpro_as gives synthesize_direct
     * and synthesize_userns each their own fork()), so these are the
     * unmodified outer credentials, not anything a prior attempt in this
     * same process could have touched.
     */
    uid_t outer_uid = getuid();
    gid_t outer_gid = getgid();

    if (unshare(CLONE_NEWUSER) != 0) return 0;

    int fd = open("/proc/self/setgroups", O_WRONLY);
    if (fd >= 0) {
        if (write(fd, "deny", 4) < 0) { close(fd); return 0; }
        close(fd);
    }

    char line[64];
    int n;

    fd = open("/proc/self/uid_map", O_WRONLY);
    if (fd < 0) return 0;
    n = snprintf(line, sizeof(line), "%u %u 1",
                 (unsigned)syn_uid, (unsigned)outer_uid);
    if (write(fd, line, (size_t)n) < 0) { close(fd); return 0; }
    close(fd);

    fd = open("/proc/self/gid_map", O_WRONLY);
    if (fd < 0) return 0;
    n = snprintf(line, sizeof(line), "%u %u 1",
                 (unsigned)syn_gid, (unsigned)outer_gid);
    if (write(fd, line, (size_t)n) < 0) { close(fd); return 0; }
    close(fd);

    return 1;
}

/*
 * run_chkpro_as - synthesize a caller identity of (syn_gid, syn_uid) and
 * call sys$chkpro() under it, trying synthesize_direct first and falling
 * back to synthesize_userns. Each attempt is its own fork() (via
 * fork_and_chkpro_as), so a mechanism that is unavailable on this host
 * leaves nothing behind for the next one to react to.
 *
 * Returns 1 and fills *out_status if EITHER mechanism produced a verified
 * (syn_gid, syn_uid) process, 0 if neither is available on this host -- in
 * which case the caller must not treat that as a boolean chkpro answer.
 */
static int run_chkpro_as(gid_t syn_gid, uid_t syn_uid,
                          uint32_t owner_uic, uint16_t prot, uint16_t access,
                          uint32_t *out_status)
{
    if (fork_and_chkpro_as(synthesize_direct, syn_gid, syn_uid,
                           owner_uic, prot, access, out_status))
        return 1;

    return fork_and_chkpro_as(synthesize_userns, syn_gid, syn_uid,
                              owner_uic, prot, access, out_status);
}

int main(void)
{
    printf("=== vms-2b8: SYSTEM protection category (sys$chkpro) ===\n");

    uint32_t me = vms$get_uic();
    printf("  outer process UIC [%o,%o] (informational only -- the "
           "discriminating cases below run under a synthesized identity, "
           "see run_chkpro_as)\n",
           (unsigned)((me >> 16) & 0xFFFFu), (unsigned)(me & 0xFFFFu));

    /*
     * A mask that denies nothing to anybody must be granted regardless of
     * category -- true under any caller identity, so it needs no
     * synthesized UIC.
     */
    check((chkpro(UIC(65534, 65534), 0x0000u, PROT_READ) & 1) != 0,
          "a protection mask that denies nothing grants access");

    /* S:RWED,O:RWED,G:,W: = nothing for group or world. 0xFF00 under the
     * pinned encoding (ovmx_fileprot.h): System@bits3-0=0x0 (full grant),
     * Owner@bits7-4=0x0 (full grant), Group@bits11-8=0xF (all denied),
     * World@bits15-12=0xF (all denied) -- the same 0xFF00 literal
     * src/vmsrms/rms_core.c's rms_get_default_protection() returns. */
    uint16_t prot_s_o_only = 0xFF00u;
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
     * THE BOUNDARY ITSELF (round 4; corrected round 5). Group 5 above is
     * inside (0, MAXSYSGROUP] under ANY boundary from 5 to 8 inclusive, so
     * it cannot tell those apart -- and neither can group 9 below, taken
     * by itself, which is outside all of them.
     *
     * TWO SEPARATE MECHANISMS NOW PIN "EXACTLY 8", NOT ONE, and each
     * covers a failure the other cannot see:
     *   1. The `_Static_assert(OVMX_MAXSYSGROUP == 8, ...)` above pins the
     *      NUMBER, at compile time. If ovmx_secparam.h's constant ever
     *      drifts from 8, the build fails before this test runs at all.
     *   2. This runtime check pins the SEMANTICS: it proves uic_is_system()
     *      uses `<=` at the boundary, not `<` -- a bug the static_assert
     *      cannot see, because it is about sys_security.c's comparison
     *      operator, not about the constant's value. Round 4's wording
     *      ("granted ONLY if the boundary is really 8") OVERCLAIMED what
     *      this runtime check alone shows: taken in isolation from the
     *      static_assert, a caller at group == MAXSYSGROUP is granted
     *      whichever value MAXSYSGROUP happens to hold, which pins nothing
     *      about the NUMBER 8 by itself -- that was a false "only" round 4
     *      did not try to break by mutation before writing it (round 5
     *      reproduced the gap: with only the alias and no static_assert,
     *      mutating OVMX_MAXSYSGROUP to 9 left this exact assertion GREEN).
     * The group-9 negative control below is now a THIRD, independent
     * check: since it drives a hardcoded literal 9, not the MAXSYSGROUP
     * symbol, it fails if the true enforced boundary is ever 9 or above,
     * regardless of what the constant claims.
     */
    ok = run_chkpro_as(MAXSYSGROUP, 8001, UIC(200, 201), prot_s_o_only,
                        PROT_READ, &st);
    check(ok && (st & 1) != 0,
          "UIC at group == MAXSYSGROUP exactly is granted SYSTEM access "
          "(the <= vs < half of the pin; the static_assert above pins the "
          "number 8, this pins the operator)");

    /*
     * NEGATIVE CONTROL, independent of the MAXSYSGROUP symbol (hardcoded
     * literal 9, not an alias): a group strictly above the oracle-pinned
     * boundary must NOT get the SYSTEM category -- it falls to WORLD,
     * which the mask above denies. This is what actually catches
     * uic_is_system() enforcing a boundary ABOVE 8 (the static_assert
     * cannot: it checks the #define, not what sys_security.c does with
     * it). Verified by mutation (round 4): changing OVMX_MAXSYSGROUP to 9
     * turns exactly this assertion red, 7/8, with every other case
     * (including the boundary-itself case above, which moves WITH the
     * symbol) unchanged.
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

    /*
     * ============================================================
     * vms-f81 -- the cross-module contract itself: a REAL file, at a
     * REAL Unix mode, run through vmsfs_mode_to_protection() (the
     * PRODUCER, src/vmsfs/vmsfs_protect.c) and then sys$chkpro() (the
     * CONSUMER, sys_security.c), for every category. Everything above
     * this point hand-built its own protection words and so could not
     * have caught the two modules disagreeing about how a word is laid
     * out -- only about what sys$chkpro does once handed one. This is
     * the test that was missing: it fails if EITHER module's nibble
     * shift or intra-nibble bit order drifts from ovmx_fileprot.h,
     * because it round-trips through both.
     *
     * mode-644 SYSTEM-read is THE originally reported symptom (a mode-644
     * file denies root/SYSTEM read): vmsfs_mode_to_protection packed the
     * System nibble at shift 0, sys$chkpro (pre-fix) read it at shift 12
     * -- the bit it actually inspected for a "System caller reading a
     * 644 file" was 644's WORLD nibble reinterpreted through the wrong
     * shift, not System's real (always-granted) nibble, and the bug
     * being a false DENIAL rather than a false GRANT depended on which
     * mode happened to be under test, not on the encoding being right.
     * ============================================================
     */
    uint32_t owner_uic_ex = UIC(300, 301);

    /* mode 644 = rw-r--r-- */
    uint16_t prot644 = protection_for_mode(0644);
    ok = run_chkpro_as(5, 5001, owner_uic_ex, prot644, PROT_READ, &st);
    check(ok && (st & 1) != 0,
          "vms-f81 HEADLINE CASE: SYSTEM-category caller reads a "
          "mode-644 file (vmsfs_mode_to_protection -> sys$chkpro) -- "
          "GRANTED");
    ok = run_chkpro_as(300, 301, owner_uic_ex, prot644, PROT_READ, &st);
    check(ok && (st & 1) != 0,
          "mode 644: OWNER reads own file -- GRANTED");
    ok = run_chkpro_as(300, 301, owner_uic_ex, prot644, VMS_PROT_E, &st);
    check(ok && (st & 1) == 0,
          "mode 644: OWNER has no execute bit -- DENIED");
    ok = run_chkpro_as(300, 999, owner_uic_ex, prot644, PROT_READ, &st);
    check(ok && (st & 1) != 0,
          "mode 644: GROUP (same group, different member) reads -- "
          "GRANTED");
    ok = run_chkpro_as(300, 999, owner_uic_ex, prot644, PROT_WRITE, &st);
    check(ok && (st & 1) == 0,
          "mode 644: GROUP has no write bit -- DENIED");
    ok = run_chkpro_as(400, 401, owner_uic_ex, prot644, PROT_READ, &st);
    check(ok && (st & 1) != 0,
          "mode 644: WORLD (unrelated UIC) reads -- GRANTED");
    ok = run_chkpro_as(400, 401, owner_uic_ex, prot644, PROT_WRITE, &st);
    check(ok && (st & 1) == 0,
          "mode 644: WORLD has no write bit -- DENIED");

    /* mode 600 = rw------- */
    uint16_t prot600 = protection_for_mode(0600);
    ok = run_chkpro_as(5, 5001, owner_uic_ex, prot600, PROT_READ, &st);
    check(ok && (st & 1) != 0,
          "mode 600: SYSTEM-category caller reads -- GRANTED "
          "(SYSTEM always full access)");
    ok = run_chkpro_as(300, 301, owner_uic_ex, prot600, PROT_READ, &st);
    check(ok && (st & 1) != 0,
          "mode 600: OWNER reads own file -- GRANTED");
    ok = run_chkpro_as(300, 999, owner_uic_ex, prot600, PROT_READ, &st);
    check(ok && (st & 1) == 0,
          "mode 600: GROUP has no access -- DENIED");
    ok = run_chkpro_as(400, 401, owner_uic_ex, prot600, PROT_READ, &st);
    check(ok && (st & 1) == 0,
          "mode 600: WORLD has no access -- DENIED");

    /* mode 755 = rwxr-xr-x */
    uint16_t prot755 = protection_for_mode(0755);
    ok = run_chkpro_as(300, 999, owner_uic_ex, prot755, VMS_PROT_E, &st);
    check(ok && (st & 1) != 0,
          "mode 755: GROUP executes -- GRANTED");
    ok = run_chkpro_as(300, 999, owner_uic_ex, prot755, PROT_WRITE, &st);
    check(ok && (st & 1) == 0,
          "mode 755: GROUP has no write bit -- DENIED");
    ok = run_chkpro_as(400, 401, owner_uic_ex, prot755, VMS_PROT_E, &st);
    check(ok && (st & 1) != 0,
          "mode 755: WORLD executes -- GRANTED");

    /* mode 640 = rw-r----- */
    uint16_t prot640 = protection_for_mode(0640);
    ok = run_chkpro_as(300, 999, owner_uic_ex, prot640, PROT_READ, &st);
    check(ok && (st & 1) != 0,
          "mode 640: GROUP reads -- GRANTED");
    ok = run_chkpro_as(400, 401, owner_uic_ex, prot640, PROT_READ, &st);
    check(ok && (st & 1) == 0,
          "mode 640: WORLD has no access -- DENIED");
    ok = run_chkpro_as(7, 7001, owner_uic_ex, prot640, PROT_READ, &st);
    check(ok && (st & 1) != 0,
          "mode 640: SYSTEM-category caller reads -- GRANTED");

    printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
