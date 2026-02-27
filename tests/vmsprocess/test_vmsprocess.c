/*
 * test_vmsprocess.c - Unit tests for vmsprocess
 *
 * Tests:
 *   - PCB creation (vms_pcb_init) and cleanup
 *   - Privilege string parsing (parse_privilege_string from privs.h)
 *   - AST queue operations (ast_queue, ast_pending_count)
 *   - Event flag local operations (eflag_set, eflag_clear, eflag_read)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vms/pcb.h"
#include "vms/privs.h"
#include "vms/ast.h"
#include "vms/eflag.h"
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

    /* Event flags should be clear */
    for (int i = 0; i < PCB_EF_CLUSTERS; i++) {
        check(pcb->ef_clusters[i] == 0, "event flag cluster initially clear");
        if (pcb->ef_clusters[i] != 0) break;  /* Don't spam failures */
    }

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
/* Test: event flag operations                                         */
/* ------------------------------------------------------------------ */
static void test_event_flags(void)
{
    printf("\n--- event flag operations ---\n");

    /* Need a PCB for event flags */
    struct vms_pcb *pcb = vms_pcb_init(0);
    if (!pcb) { check(0, "vms_pcb_init for event flags"); return; }

    eflag_init();

    /* Initial state: all flags clear */
    check(eflag_read(0) == 0, "flag 0 initially clear");
    check(eflag_read(1) == 0, "flag 1 initially clear");
    check(eflag_read(31) == 0, "flag 31 initially clear");

    /* Set flag 0 */
    int prev = eflag_set(0);
    check(prev == SS$_WASCLR, "set flag 0: was clear");
    check(eflag_read(0) == 1, "flag 0 is set after eflag_set");

    /* Set flag 0 again (was already set) */
    prev = eflag_set(0);
    check(prev == SS$_WASSET, "set flag 0 again: was set");

    /* Clear flag 0 */
    prev = eflag_clear(0);
    check(prev == SS$_WASSET, "clear flag 0: was set");
    check(eflag_read(0) == 0, "flag 0 is clear after eflag_clear");

    /* Set multiple flags, check cluster */
    eflag_set(0);
    eflag_set(1);
    eflag_set(5);
    uint32_t cluster = eflag_read_cluster(0);
    check(cluster & (1u << 0), "flag 0 set in cluster");
    check(cluster & (1u << 1), "flag 1 set in cluster");
    check(cluster & (1u << 5), "flag 5 set in cluster");
    check(!(cluster & (1u << 2)), "flag 2 clear in cluster");

    /* Clear all and verify */
    eflag_clear(0);
    eflag_clear(1);
    eflag_clear(5);
    cluster = eflag_read_cluster(0);
    check(cluster == 0, "cluster 0 is all-zero after clearing");

    /* Second cluster (flags 32-63) */
    eflag_set(32);
    check(eflag_read(32) == 1, "flag 32 (cluster 1, bit 0) is set");
    check(eflag_read(0) == 0, "flag 0 (cluster 0) unaffected");
    eflag_clear(32);
    check(eflag_read(32) == 0, "flag 32 cleared");

    /* Out-of-range flag */
    prev = eflag_set(200);  /* > EFN_MAX_TOTAL (128) */
    check(prev == SS$_ILLEFC, "eflag_set out-of-range returns SS$_ILLEFC");

    vms_pcb_cleanup();
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

int main(void)
{
    printf("=== vmsprocess unit tests ===\n");

    test_pcb();
    test_privs();

    /* Re-init PCB for subsequent tests that need it */
    test_event_flags();
    test_ast();

    if (failures == 0)
        printf("\nAll vmsprocess tests passed.\n");
    else
        printf("\nSome vmsprocess tests FAILED (%d).\n", failures);

    return failures;
}
