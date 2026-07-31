/*
 * test_vmsprocess.c - Unit tests for vmsprocess
 *
 * Tests:
 *   - PCB creation (vms_pcb_init) and cleanup
 *   - Privilege string parsing (parse_privilege_string from privs.h)
 *   - AST queue operations (ast_queue, ast_pending_count)
 *
 * NO EVENT FLAG COVERAGE, DELIBERATELY (vms-2a8). This file used to certify
 * src/vmsprocess/event_flags.c -- a SECOND, per-process implementation of the
 * whole event flag facility living in the PCB. It passed every assertion, and
 * that was the problem: under CLAUDE.md Rule 11 a system facility is
 * executive-resident, and a per-process fake passes every single-process test
 * perfectly. Both the implementation and this coverage of it are deleted; the
 * facility is tested where it now lives, across a REAL process boundary,
 * against a real /dev/vms, in tests/qemu/test_syssvc_ef_mproc.c
 * (A-writes/B-reads) and tests/qemu/test_kmod_eflag_mproc.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     /* getpid() -- was implicitly declared (vms-2a8) */

#include "vms/pcb.h"
#include "vms/privs.h"
#include "vms/ast.h"
#include "vms/process.h"
#include "ssdef.h"

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

/* ------------------------------------------------------------------ */
/* Test: vms_pcb_init / vms_pcb_get / vms_pcb_cleanup                 */
/* ------------------------------------------------------------------ */
static void test_pcb(void)
{
    printf("\n--- PCB creation and cleanup ---\n");

    /* vms_pcb_get should return NULL before init */
    struct vms_pcb *pcb = vms_pcb_get();
    check(pcb == NULL, "vms_pcb_get returns NULL before init");

    /* Initialize PCB for this thread */
    pcb = vms_pcb_init(0);
    check(pcb != NULL, "vms_pcb_init returns non-NULL");

    /* vms_pcb_get should now return the same pointer */
    struct vms_pcb *pcb2 = vms_pcb_get();
    check(pcb2 == pcb, "vms_pcb_get returns same pointer as init");

    /* self-pointer should be valid */
    check(pcb->self == pcb, "PCB self-pointer is correct");

    /* Default mode is user */
    check(pcb->current_mode == PCB_MODE_USER, "initial mode is PCB_MODE_USER");

    /* Initial privileges */
    check(pcb->cur_privs == 0, "initial cur_privs is 0");
    check(pcb->perm_privs == 0, "initial perm_privs is 0");

    /* AST queues should be empty and enabled */
    for (int i = 0; i < 4; i++) {
        check(pcb->ast[i].count == 0, "AST queue initially empty");
        check(pcb->ast[i].enabled == 1, "AST queue initially enabled");
        if (pcb->ast[i].count != 0) break;
    }

    /* Identity fields */
    check(pcb->vms_pid == 0, "initial VMS PID is 0");
    check(pcb->uic == 0, "initial UIC is 0");

    /* Set identity */
    vms_pcb_set_identity(0x100, 0x010004, "TESTUSER", "TEST");
    check(pcb->vms_pid == 0x100, "vms_pid set correctly");
    check(pcb->uic == 0x010004, "UIC set correctly");
    check(strcmp(pcb->username, "TESTUSER") == 0, "username set correctly");
    check(strcmp(pcb->prcnam, "TEST") == 0, "prcnam set correctly");

    /* Set default directory */
    vms_pcb_set_default_dir("SYS$LOGIN:");
    check(strcmp(pcb->default_dir, "SYS$LOGIN:") == 0, "default_dir set correctly");

    /* Cleanup */
    vms_pcb_cleanup();
    /* After cleanup, get should return NULL */
    pcb2 = vms_pcb_get();
    check(pcb2 == NULL, "vms_pcb_get returns NULL after cleanup");
}

/* ------------------------------------------------------------------ */
/* Test: privilege string parsing                                      */
/* ------------------------------------------------------------------ */
static void test_privs(void)
{
    printf("\n--- privilege string parsing ---\n");

    /* Single privilege */
    uint64_t mask = parse_privilege_string("TMPMBX");
    check(mask == PRV$M_TMPMBX, "TMPMBX parses to PRV$M_TMPMBX");

    mask = parse_privilege_string("SYSPRV");
    check(mask == PRV$M_SYSPRV, "SYSPRV parses to PRV$M_SYSPRV");

    mask = parse_privilege_string("BYPASS");
    check(mask == PRV$M_BYPASS, "BYPASS parses to PRV$M_BYPASS");

    /* Case insensitive */
    mask = parse_privilege_string("sysprv");
    check(mask == PRV$M_SYSPRV, "sysprv (lowercase) parses correctly");

    mask = parse_privilege_string("TmPmBx");
    check(mask == PRV$M_TMPMBX, "mixed case TmPmBx parses correctly");

    /* Multiple privileges (comma-separated) */
    mask = parse_privilege_string("TMPMBX,NETMBX");
    check(mask == (PRV$M_TMPMBX | PRV$M_NETMBX), "TMPMBX,NETMBX parses to OR of both");

    mask = parse_privilege_string("OPER,SYSPRV,BYPASS");
    check(mask == (PRV$M_OPER | PRV$M_SYSPRV | PRV$M_BYPASS),
          "OPER,SYSPRV,BYPASS parses to OR of three");

    /* ALL keyword */
    mask = parse_privilege_string("ALL");
    check(mask == PRV$M_ALL, "ALL parses to PRV$M_ALL");

    mask = parse_privilege_string("all");
    check(mask == PRV$M_ALL, "all (lowercase) parses to PRV$M_ALL");

    /* Empty/NULL */
    mask = parse_privilege_string(NULL);
    check(mask == 0, "NULL input returns 0");

    mask = parse_privilege_string("");
    check(mask == 0, "empty string returns 0");

    /* Unknown privilege (should be silently ignored) */
    mask = parse_privilege_string("UNKNOWN_PRIV");
    check(mask == 0, "unknown privilege returns 0");

    /* Mix of known and unknown */
    mask = parse_privilege_string("TMPMBX,UNKNOWN,SYSPRV");
    check(mask == (PRV$M_TMPMBX | PRV$M_SYSPRV),
          "mix of known+unknown: known bits set, unknown ignored");

    /* All individual privileges */
    check(parse_privilege_string("NETMBX") == PRV$M_NETMBX, "NETMBX");
    check(parse_privilege_string("OPER")   == PRV$M_OPER,   "OPER");
    check(parse_privilege_string("SETPRV") == PRV$M_SETPRV, "SETPRV");
    check(parse_privilege_string("CMKRNL") == PRV$M_CMKRNL, "CMKRNL");
    check(parse_privilege_string("CMEXEC") == PRV$M_CMEXEC, "CMEXEC");
    check(parse_privilege_string("SYSNAM") == PRV$M_SYSNAM, "SYSNAM");
    check(parse_privilege_string("GRPNAM") == PRV$M_GRPNAM, "GRPNAM");
    check(parse_privilege_string("DETACH") == PRV$M_DETACH, "DETACH");
    check(parse_privilege_string("ALTPRI") == PRV$M_ALTPRI, "ALTPRI");
    check(parse_privilege_string("WORLD")  == PRV$M_WORLD,  "WORLD");
    check(parse_privilege_string("GROUP")  == PRV$M_GROUP,  "GROUP");
    check(parse_privilege_string("LOG_IO") == PRV$M_LOG_IO, "LOG_IO");
    check(parse_privilege_string("PHY_IO") == PRV$M_PHY_IO, "PHY_IO");
}

/* ------------------------------------------------------------------ */
/* Test: AST queue operations                                          */
/* ------------------------------------------------------------------ */
static volatile int ast_delivered = 0;
static volatile uint32_t ast_param_received = 0;

static void test_ast_handler(uint32_t param)
{
    ast_delivered++;
    ast_param_received = param;
}

static void test_ast(void)
{
    printf("\n--- AST queue operations ---\n");

    struct vms_pcb *pcb = vms_pcb_init(0);
    if (!pcb) { check(0, "vms_pcb_init for AST"); return; }

    ast_init();

    /* No pending ASTs initially */
    check(ast_pending_count() == 0, "no pending ASTs initially");

    /* ASTs enabled initially */
    check(ast_is_enabled() == 1, "ASTs enabled initially");

    /* Queue an AST */
    uint32_t st = ast_queue(test_ast_handler, 0xDEADBEEF, PCB_MODE_USER);
    check($VMS_STATUS_SUCCESS(st), "ast_queue succeeds");
    check(ast_pending_count() == 1, "1 pending AST after queue");

    /* Queue another */
    st = ast_queue(test_ast_handler, 0x12345678, PCB_MODE_USER);
    check($VMS_STATUS_SUCCESS(st), "second ast_queue succeeds");
    check(ast_pending_count() == 2, "2 pending ASTs after second queue");

    /* Disable AST delivery */
    ast_set_enable(0);
    check(ast_is_enabled() == 0, "ASTs disabled after ast_set_enable(0)");

    /* Re-enable */
    ast_set_enable(1);
    check(ast_is_enabled() == 1, "ASTs re-enabled after ast_set_enable(1)");

    /* NULL handler should fail */
    st = ast_queue(NULL, 0, PCB_MODE_USER);
    check(!$VMS_STATUS_SUCCESS(st), "ast_queue with NULL handler fails");

    /* Deliver pending ASTs manually */
    ast_deliver_pending();

    /* After delivery, queue should be drained */
    check(ast_pending_count() == 0, "pending count is 0 after delivery");
    check(ast_delivered >= 2, "AST handler was called at least twice");

    ast_cleanup();
    vms_pcb_cleanup();
}

/* ------------------------------------------------------------------ */
/* Test: UIC format/parse round trip                                   */
/* ------------------------------------------------------------------ */
static void test_uic(void)
{
    printf("\n--- UIC format/parse ---\n");

    char buf[32];

    /* [1,4] packed as (group<<16)|member, per SET UIC / rms_get_session_uic
     * convention already used across the codebase. */
    uint32_t uic = (1u << 16) | 4u;
    vms_format_uic(uic, buf, sizeof(buf));
    check(strcmp(buf, "[001,004]") == 0, "vms_format_uic([1,4]) == \"[001,004]\"");
    check(vms_parse_uic(buf) == uic, "vms_parse_uic round-trips vms_format_uic output");

    /* Bracket-free form also accepted */
    check(vms_parse_uic("1,4") == uic, "vms_parse_uic accepts bracket-free \"1,4\"");

    /* A larger, more realistic group/member pair */
    uint32_t uic2 = (0377u << 16) | 0012u;
    vms_format_uic(uic2, buf, sizeof(buf));
    check(vms_parse_uic(buf) == uic2, "vms_parse_uic round-trips [377,012]");

    /* Zero UIC round-trips too */
    vms_format_uic(0, buf, sizeof(buf));
    check(strcmp(buf, "[000,000]") == 0, "vms_format_uic(0) == \"[000,000]\"");
    check(vms_parse_uic(buf) == 0, "vms_parse_uic(\"[000,000]\") == 0");

    /* Invalid input: NULL and malformed strings return 0 rather than
     * crashing (the header declares no error-status return path). */
    check(vms_parse_uic(NULL) == 0, "vms_parse_uic(NULL) == 0");
    check(vms_parse_uic("garbage") == 0, "vms_parse_uic(\"garbage\") == 0");

    /* NULL/zero-size buffer must not crash */
    vms_format_uic(uic, NULL, sizeof(buf));
    vms_format_uic(uic, buf, 0);
    check(1, "vms_format_uic tolerates NULL/zero-size buffer");
}

/* ------------------------------------------------------------------ */
/* Test: vms_pid_from_linux                                            */
/* ------------------------------------------------------------------ */
static void test_pid_from_linux(void)
{
    printf("\n--- vms_pid_from_linux ---\n");

    check(vms_pid_from_linux(1234) == 1234u, "vms_pid_from_linux(1234) == 1234");
    check(vms_pid_from_linux(getpid()) == (uint32_t)getpid(),
          "vms_pid_from_linux(getpid()) == (uint32_t)getpid()");
}

/* ------------------------------------------------------------------ */
/* Test: vms_get_current_process                                       */
/* ------------------------------------------------------------------ */
static void test_current_process(void)
{
    printf("\n--- vms_get_current_process ---\n");

    struct vms_pcb *pcb = vms_pcb_init(0);
    if (!pcb) { check(0, "vms_pcb_init for current-process test"); return; }

    vms_pcb_set_identity(0x777, (0020u << 16) | 0015u, "TESTUSER", "TESTPRC");

    vms_process_t *proc = vms_get_current_process();
    check(proc != NULL, "vms_get_current_process returns non-NULL");
    check(proc->linux_pid == getpid(), "current process linux_pid == getpid()");
    check(proc->vms_pid == 0x777, "current process vms_pid reflects PCB identity");
    check(proc->uic == ((0020u << 16) | 0015u), "current process uic reflects PCB identity");
    check(strcmp(proc->username, "TESTUSER") == 0, "current process username reflects PCB identity");
    check(strcmp(proc->prcnam, "TESTPRC") == 0, "current process prcnam reflects PCB identity");

    vms_pcb_cleanup();

    /* Without an initialized PCB, still returns a usable struct keyed
     * off the real Linux PID rather than NULL or garbage. */
    vms_process_t *proc2 = vms_get_current_process();
    check(proc2 != NULL, "vms_get_current_process returns non-NULL with no PCB");
    check(proc2->linux_pid == getpid(), "no-PCB current process linux_pid == getpid()");
    check(proc2->vms_pid == vms_pid_from_linux(getpid()),
          "no-PCB current process vms_pid falls back to vms_pid_from_linux(getpid())");
}

int main(void)
{
    printf("=== vmsprocess unit tests ===\n");

    test_pcb();
    test_privs();
    test_uic();
    test_pid_from_linux();

    /* Re-init PCB for subsequent tests that need it */
    test_ast();
    test_current_process();

    if (failures == 0)
        printf("\nAll vmsprocess tests passed.\n");
    else
        printf("\nSome vmsprocess tests FAILED (%d).\n", failures);

    return failures;
}
