/*
 * test_ssh_ident.c - Unit tests for the SSH session identity fail-honest
 * decision (vms-6ae).
 *
 * Exercises the REAL product function ovmx_ssh_establish_identity()
 * (src/vmsssh/ssh_ident.c), linked directly into this binary -- not a copy.
 * The executive call (vms_kif_setident) is injected via
 * struct ovmx_ident_syscalls so the security-critical policy is asserted
 * WITHOUT a live /dev/vms and without libssh:
 *
 *   1. executive REFUSES (even/error status) -> return -1 (session DENIED);
 *   2. executive ACCEPTS (odd/success status) -> return 0 (session proceeds);
 *   3. /dev/vms absent (setident returns SS$_NOSUCHDEV, an even status)
 *      -> return -1 (DENIED, never a local privileged fallback);
 *   4. a NULL syscall table (no way to reach the executive) fails closed -1;
 *   5. the arguments (username/uic/privs) are passed through unaltered -- the
 *      decision does not weaken the executive's inputs;
 *   6. the raw executive status is reported out for the caller's diagnostic.
 *
 * This is the audit-§3.7 regression guard: before vms-6ae, a refusal (case 1
 * and 3) logged a WARNING and PROCEEDED to build a local privileged PCB and
 * exec DCL. The product decision now returns -1, which vmssshd.c turns into
 * %OVMX-F-NOIDENT + _exit(1) (no PCB, no shell) -- mirroring LOGINOUT
 * (tools/vms_login.c). See docs/audit-vms-040-executive-boundary.md §3.7.
 */

#include <stdio.h>
#include <string.h>

#include "ssh_ident.h"

/* SS$_NOSUCHDEV: what vms_kif_setident returns when /dev/vms is absent.
 * Even value (error) -- the exact value is not load-bearing to this test;
 * that it is EVEN is. */
#define SS_NOSUCHDEV  0x00000660u

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

/* ---- Mock setident ------------------------------------------------ */

static uint32_t mock_return;        /* status the mock reports */
static int      mock_called;        /* how many times invoked */
static char     seen_username[64];
static uint32_t seen_uic;
static uint64_t seen_privs;

static uint32_t mock_setident(const char *username, uint32_t uic,
                              uint64_t authorized_privs)
{
    mock_called++;
    strncpy(seen_username, username ? username : "", sizeof(seen_username) - 1);
    seen_username[sizeof(seen_username) - 1] = '\0';
    seen_uic   = uic;
    seen_privs = authorized_privs;
    return mock_return;
}

static void mock_reset(uint32_t ret)
{
    mock_return = ret;
    mock_called = 0;
    seen_username[0] = '\0';
    seen_uic = 0;
    seen_privs = 0;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    const struct ovmx_ident_syscalls sc = { .fn_setident = mock_setident };
    uint32_t status;
    int rc;

    printf("=== ssh_ident fail-honest tests (vms-6ae) ===\n");

    /* ---- 1. Executive REFUSES (even status) -> DENY (-1) ---- */
    printf("[1] executive refuses (even status) -> session DENIED\n");
    mock_reset(0x00000000u);   /* even -> refused */
    status = 0xdeadbeef;
    rc = ovmx_ssh_establish_identity("FIELD", 0x00010004u, 0xffffffffffffffffull,
                                     &sc, &status);
    ok(rc == -1, "refusal (status 0) returns -1 (deny)");
    ok(mock_called == 1, "setident was actually called");
    ok(status == 0x00000000u, "raw refusal status reported to caller");

    /* An arbitrary non-zero EVEN status is also a refusal. */
    mock_reset(0x0000002Cu);   /* SS$_ACCVIO-shaped even value */
    rc = ovmx_ssh_establish_identity("FIELD", 0x00010004u, 0ull, &sc, &status);
    ok(rc == -1, "even non-zero status also returns -1 (deny)");

    /* ---- 2. Executive ACCEPTS (odd status) -> PROCEED (0) ---- */
    printf("[2] executive accepts (odd status) -> session PROCEEDS\n");
    mock_reset(0x00000001u);   /* SS$_NORMAL, odd -> accepted */
    status = 0;
    rc = ovmx_ssh_establish_identity("SYSTEM", 0x00010004u, 0xffffffffffffffffull,
                                     &sc, &status);
    ok(rc == 0, "accept (odd status) returns 0 (proceed)");
    ok(status == 0x00000001u, "raw accept status reported to caller");

    /* ---- 3. /dev/vms absent (SS$_NOSUCHDEV, even) -> DENY ---- */
    printf("[3] /dev/vms absent (SS$_NOSUCHDEV) -> session DENIED\n");
    mock_reset(SS_NOSUCHDEV);
    rc = ovmx_ssh_establish_identity("SYSTEM", 0x00010004u, 0ull, &sc, &status);
    ok(rc == -1, "SS$_NOSUCHDEV returns -1 (no local privileged fallback)");
    ok(status == SS_NOSUCHDEV, "SS$_NOSUCHDEV reported to caller");

    /* ---- 4. No syscall table -> fail closed ---- */
    printf("[4] unreachable executive (NULL table) -> fail closed\n");
    rc = ovmx_ssh_establish_identity("SYSTEM", 0x00010004u, 0ull, NULL, &status);
    ok(rc == -1, "NULL syscall table returns -1 (deny)");
    {
        struct ovmx_ident_syscalls empty = { .fn_setident = NULL };
        rc = ovmx_ssh_establish_identity("SYSTEM", 0x00010004u, 0ull, &empty,
                                         &status);
        ok(rc == -1, "NULL fn_setident returns -1 (deny)");
    }

    /* ---- 5. Arguments passed through unaltered ---- */
    printf("[5] inputs are passed to the executive unaltered\n");
    mock_reset(0x00000001u);
    (void)ovmx_ssh_establish_identity("MARY", 0x0002000Au,
                                      0x00000000AABBCCDDull, &sc, &status);
    ok(strcmp(seen_username, "MARY") == 0, "username passed through");
    ok(seen_uic == 0x0002000Au, "uic passed through");
    ok(seen_privs == 0x00000000AABBCCDDull, "privilege mask passed through");

    /* ---- 6. out_status may be NULL ---- */
    printf("[6] out_status NULL is tolerated\n");
    mock_reset(0x00000000u);
    rc = ovmx_ssh_establish_identity("FIELD", 0, 0ull, &sc, NULL);
    ok(rc == -1, "refusal with NULL out_status still denies");

    /* ------------------------------------------------------------ */
    printf("\n=== %d passed, %d failed ===\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}
