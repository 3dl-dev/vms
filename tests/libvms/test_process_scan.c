/*
 * test_process_scan.c - sys$process_scan item-list handling (no executive needed).
 *
 * $PROCESS_SCAN only builds a context; the processes it selects are walked by
 * $GETJPI in the executive (tests/qemu/test_syssvc_process_scan.c). Here: what it
 * accepts and what it refuses, with the V7.3 $PSCANDEF codes, and that a $GETJPI
 * on a context with no executive present reports the failure, never an empty
 * successful scan.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ssdef.h"
#include "jpidef.h"
#include "pscandef.h"
#include "starlet.h"

static int failures;
#define check(c, m) do { if (c) printf("  OK: %s\n", m); else { printf("  FAIL: %s\n", m); failures++; } } while (0)

struct ps_ile { uint16_t len, code; uint32_t val, flags; };

int main(void)
{
    uint32_t ctx = 0;
    printf("=== test_process_scan ===\n");

    struct ps_ile grp[] = { { 0, PSCAN$_GRP, 1, PSCAN$M_EQL }, { 0, 0, 0, 0 } };
    check(sys$process_scan(NULL, grp) == SS$_BADPARAM, "no context address is SS$_BADPARAM");
    check(sys$process_scan(&ctx, grp) == SS$_NORMAL && ctx != 0, "GRP EQL builds a context");

    struct ps_ile end[] = { { 0, PSCAN$_END, 0, 0 }, { 0, 0, 0, 0 } };
    uint32_t c2 = 0;
    check(sys$process_scan(&c2, end) == SS$_BADPARAM, "PSCAN$_END is not an item: SS$_BADPARAM");

    struct ps_ile state[] = { { 0, PSCAN$_STATE, 1, PSCAN$M_EQL }, { 0, 0, 0, 0 } };
    check(sys$process_scan(&c2, state) == SS$_UNSUPPORTED, "STATE (not in the executive row) is SS$_UNSUPPORTED");

    struct ps_ile node5[] = { { 0, PSCAN$_NODE_CSID, 5, PSCAN$M_EQL }, { 0, 0, 0, 0 } };
    check(sys$process_scan(&c2, node5) == SS$_UNSUPPORTED, "another node's CSID is SS$_UNSUPPORTED");
    struct ps_ile node0[] = { { 0, PSCAN$_NODE_CSID, 0, PSCAN$M_NEQ }, { 0, 0, 0, 0 } };
    check(sys$process_scan(&c2, node0) == SS$_NORMAL, "NODE_CSID NEQ 0 (every node) is accepted");

    static char name[] = "X";
    struct ps_ile str[] = { { 1, PSCAN$_PRCNAM, (uint32_t)(uintptr_t)name, PSCAN$M_EQL }, { 0, 0, 0, 0 } };
    if ((uintptr_t)name < 0x100000000ull)
        check(sys$process_scan(&c2, str) == SS$_NORMAL, "PRCNAM EQL builds a context");

    uint32_t pid = 0;
    struct { uint16_t len, code; void *buf; uint16_t *ret; } jl[2] = { { 4, JPI$_PID, &pid, 0 }, { 0, 0, 0, 0 } };
    uint32_t g = sys$getjpiw(0, &ctx, 0, jl, 0, 0, 0);
    check(!(g & 1) && g != SS$_NOMOREPROC,
          "with no executive a scan reports the failure, not an empty successful scan");

    uint32_t st = SS$_NORMAL;
    for (int i = 0; i < 20 && (st & 1); i++)
        st = sys$process_scan(&c2, grp);
    check(st == SS$_INSFMEM, "contexts are bounded: past the table, SS$_INSFMEM");

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
