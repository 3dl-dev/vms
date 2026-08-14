/*
 * test_cred_drop.c - Unit tests for the SSH session credential drop (vms-49e).
 *
 * Exercises the REAL product function ovmx_cred_drop_to_uic()
 * (src/vmsssh/cred_drop.c), linked directly into this binary -- not a copy.
 * The syscalls are injected via struct ovmx_cred_syscalls so the security
 * properties are asserted WITHOUT the test process dropping its own uid:
 *
 *   1. correct UIC -> (gid=group, uid=member) derivation;
 *   2. supplementary groups cleared, and setgid applied BEFORE setuid;
 *   3. fail-closed: any failing syscall makes the drop return -1 and does
 *      NOT attempt a later step (a partial drop is a failed drop);
 *   4. a drop that returns 0 from the syscalls but does not verify (getuid/
 *      geteuid/getgid/getegid disagree with the target) still returns -1.
 *
 * This is the SSH mirror of LOGINOUT's inline drop (tools/vms_login.c); the
 * uid/gid == UIC mapping is an OVMX design choice, not VMS-authentic
 * behaviour (CLAUDE.md Rule 8/10).
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "cred_drop.h"

static int pass_count = 0;
static int fail_count = 0;

static void ok(int cond, const char *msg)
{
    if (cond) {
        pass_count++;
        printf("  PASS: %s\n", msg);
    } else {
        fail_count++;
        printf("  FAIL: %s\n", msg);
    }
}

/* ---- Mock syscall state ------------------------------------------ */

#define OP_SETGROUPS 1
#define OP_SETGID    2
#define OP_SETUID    3

static int      call_seq[8];
static int      call_n;
static gid_t    got_gid;
static uid_t    got_uid;
static int      got_setgroups_size;   /* -1 sentinel = not called */

/* Failure injection: which op returns non-zero (0 = none). */
static int      fail_op;
/* Identity the getters report after the drop. */
static uid_t    cur_uid, cur_euid;
static gid_t    cur_gid, cur_egid;

static void mock_reset(void)
{
    call_n = 0;
    got_gid = (gid_t)-1;
    got_uid = (uid_t)-1;
    got_setgroups_size = -1;
    fail_op = 0;
    memset(call_seq, 0, sizeof(call_seq));
    errno = 0;
}

static int mock_setgroups(size_t size, const gid_t *list)
{
    (void)list;
    call_seq[call_n++] = OP_SETGROUPS;
    got_setgroups_size = (int)size;
    if (fail_op == OP_SETGROUPS) { errno = EPERM; return -1; }
    return 0;
}
static int mock_setgid(gid_t gid)
{
    call_seq[call_n++] = OP_SETGID;
    got_gid = gid;
    if (fail_op == OP_SETGID) { errno = EPERM; return -1; }
    return 0;
}
static int mock_setuid(uid_t uid)
{
    call_seq[call_n++] = OP_SETUID;
    got_uid = uid;
    if (fail_op == OP_SETUID) { errno = EPERM; return -1; }
    return 0;
}
static uid_t mock_getuid(void)  { return cur_uid;  }
static uid_t mock_geteuid(void) { return cur_euid; }
static gid_t mock_getgid(void)  { return cur_gid;  }
static gid_t mock_getegid(void) { return cur_egid; }

static struct ovmx_cred_syscalls mock_ops(void)
{
    struct ovmx_cred_syscalls sc = {
        .fn_setgroups = mock_setgroups,
        .fn_setgid    = mock_setgid,
        .fn_setuid    = mock_setuid,
        .fn_getuid    = mock_getuid,
        .fn_geteuid   = mock_geteuid,
        .fn_getgid    = mock_getgid,
        .fn_getegid   = mock_getegid,
    };
    return sc;
}

/* Make the getters report a clean drop to (gid=group, uid=member). */
static void set_verified_identity(uint32_t group, uint32_t member)
{
    cur_uid = cur_euid = (uid_t)member;
    cur_gid = cur_egid = (gid_t)group;
}

/* ---- Tests -------------------------------------------------------- */

/* [200,10]: group=200 (0x0c8), member=10 (0x00a). */
#define G 200u
#define M 10u

static void test_success_and_derivation(void)
{
    printf("test: success path + UIC->uid/gid derivation\n");
    struct ovmx_cred_syscalls sc = mock_ops();
    mock_reset();
    set_verified_identity(G, M);

    int rc = ovmx_cred_drop_to_uic(G, M, &sc);

    ok(rc == 0, "returns 0 when all syscalls succeed and drop verifies");
    ok(got_gid == (gid_t)G, "setgid receives the UIC group (gid=group)");
    ok(got_uid == (uid_t)M, "setuid receives the UIC member (uid=member)");
    ok(got_setgroups_size == 0, "supplementary groups cleared (setgroups(0,...))");
}

static void test_ordering(void)
{
    printf("test: ordering (setgroups, then setgid BEFORE setuid)\n");
    struct ovmx_cred_syscalls sc = mock_ops();
    mock_reset();
    set_verified_identity(G, M);

    (void)ovmx_cred_drop_to_uic(G, M, &sc);

    ok(call_n == 3, "exactly three credential syscalls issued");
    ok(call_seq[0] == OP_SETGROUPS, "setgroups called first");
    ok(call_seq[1] == OP_SETGID,    "setgid called second (before setuid)");
    ok(call_seq[2] == OP_SETUID,    "setuid called last");
}

static void test_setgroups_failure_is_fatal(void)
{
    printf("test: fail-closed when setgroups fails\n");
    struct ovmx_cred_syscalls sc = mock_ops();
    mock_reset();
    set_verified_identity(G, M);
    fail_op = OP_SETGROUPS;

    int rc = ovmx_cred_drop_to_uic(G, M, &sc);

    ok(rc == -1, "returns -1 (fail closed) when setgroups fails");
    ok(call_n == 1, "no further syscalls attempted after setgroups fails");
    ok(got_gid == (gid_t)-1 && got_uid == (uid_t)-1, "neither setgid nor setuid ran");
}

static void test_setgid_failure_is_fatal(void)
{
    printf("test: fail-closed when setgid fails (setuid must NOT run)\n");
    struct ovmx_cred_syscalls sc = mock_ops();
    mock_reset();
    set_verified_identity(G, M);
    fail_op = OP_SETGID;

    int rc = ovmx_cred_drop_to_uic(G, M, &sc);

    ok(rc == -1, "returns -1 (fail closed) when setgid fails");
    ok(got_uid == (uid_t)-1, "setuid NOT called after setgid failure (no root uid retained silently)");
    ok(call_seq[call_n - 1] == OP_SETGID, "stopped at setgid");
}

static void test_setuid_failure_is_fatal(void)
{
    printf("test: fail-closed when setuid fails\n");
    struct ovmx_cred_syscalls sc = mock_ops();
    mock_reset();
    set_verified_identity(G, M);
    fail_op = OP_SETUID;

    int rc = ovmx_cred_drop_to_uic(G, M, &sc);

    ok(rc == -1, "returns -1 (fail closed) when setuid fails");
}

static void test_unverified_drop_is_fatal(void)
{
    printf("test: fail-closed when syscalls 'succeed' but drop does not verify\n");
    struct ovmx_cred_syscalls sc = mock_ops();

    /* uid did not actually change (still root) even though setuid returned 0. */
    mock_reset();
    cur_uid = cur_euid = 0;          /* still root! */
    cur_gid = cur_egid = (gid_t)G;
    ok(ovmx_cred_drop_to_uic(G, M, &sc) == -1,
       "returns -1 when getuid still reports root after a 'successful' setuid");

    /* effective uid differs from real uid (saved-set style leftover). */
    mock_reset();
    cur_uid = (uid_t)M; cur_euid = 0;   /* euid leaked back to root */
    cur_gid = cur_egid = (gid_t)G;
    ok(ovmx_cred_drop_to_uic(G, M, &sc) == -1,
       "returns -1 when euid disagrees with the target uid");

    /* gid did not take. */
    mock_reset();
    cur_uid = cur_euid = (uid_t)M;
    cur_gid = 0; cur_egid = 0;           /* still gid 0 */
    ok(ovmx_cred_drop_to_uic(G, M, &sc) == -1,
       "returns -1 when the gid drop did not verify");
}

int main(void)
{
    printf("=== vmssshd credential-drop unit tests (vms-49e) ===\n\n");

    test_success_and_derivation();
    test_ordering();
    test_setgroups_failure_is_fatal();
    test_setgid_failure_is_fatal();
    test_setuid_failure_is_fatal();
    test_unverified_drop_is_fatal();

    printf("\n=== %d passed, %d failed ===\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
