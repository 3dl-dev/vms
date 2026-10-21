/*
 * test_syssvc_process_scan.c - $PROCESS_SCAN selects processes for $GETJPI from the
 * executive's process rows (OpenVMS System Services Reference, $PROCESS_SCAN;
 * item codes and flags: the V7.3 $PSCANDEF, src/libvms/include/pscandef.h).
 *
 * This process names itself PSCANMOM and starts a subprocess named PSCANKID. Then:
 *   - PRCNAM EQL "PSCANKID" selects exactly that process, then SS$_NOMOREPROC;
 *   - PRCNAM "PSCAN*" with PSCAN$M_WILDCARD selects both, and nothing else;
 *   - "pscankid" selects nothing without PSCAN$M_CASE_BLIND, the child with it;
 *   - PRCNAM EQL "PSCANKID" PSCAN$M_OR PRCNAM EQL "PSCANMOM" selects both;
 *   - a criterion the executive's row cannot answer (PSCAN$_STATE) is
 *     SS$_UNSUPPORTED, not silently ignored.
 *
 * No /dev/vms -> honest SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "jpidef.h"
#include "pscandef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

struct ps_ile { uint16_t len, code; uint32_t val, flags; };

/* String criteria travel as a 32-bit address (the item list's longword). */
static char s_kid[] = "PSCANKID", s_mom[] = "PSCANMOM", s_wild[] = "PSCAN*", s_lower[] = "pscankid";

#define MAXSEL 16
/* Run a scan; collect the PIDs it selects. Returns the $PROCESS_SCAN status. */
static uint32_t scan(struct ps_ile *items, uint32_t *pids, int *n)
{
    uint32_t ctx = 0, pid = 0;
    uint16_t rl = 0;
    struct { uint16_t len, code; void *buf; uint16_t *ret; } jl[2] = {
        { 4, JPI$_PID, &pid, &rl }, { 0, 0, 0, 0 } };
    *n = 0;
    uint32_t st = sys$process_scan(&ctx, items);
    if (!(st & 1))
        return st;
    for (int guard = 0; guard < 256; guard++) {
        uint32_t g = sys$getjpiw(0, &ctx, 0, jl, 0, 0, 0);
        if (g == SS$_NOMOREPROC)
            break;
        if (!(g & 1)) { *n = -1; break; }
        if (*n < MAXSEL) pids[(*n)++] = pid;
    }
    return st;
}

static int has(const uint32_t *p, int n, uint32_t v)
{
    for (int i = 0; i < n; i++) if (p[i] == v) return 1;
    return 0;
}

int main(void)
{
    uint32_t self = 0, kid = 0, sel[MAXSEL];
    int n, go[2], ready[2];

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_process_scan: $PROCESS_SCAN + $GETJPI ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_process_scan: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    check((uintptr_t)s_kid < 0x100000000ull, "the criterion strings lie in the low 4 GB");

    struct dsc$descriptor_s mom = { 8, DSC$K_DTYPE_T, DSC$K_CLASS_S, s_mom };
    check(sys$setprn(&mom) & 1, "this process is named PSCANMOM");
    check(ovmx_test_self(&self) & 1, "this process's PID");

    if (pipe(go) != 0 || pipe(ready) != 0)
        return 1;
    pid_t child = ovmx_fork_subprocess();
    if (child == 0) {
        char c = 'N';
        uint32_t me = 0;
        struct dsc$descriptor_s k = { 8, DSC$K_DTYPE_T, DSC$K_CLASS_S, s_kid };
        if ((sys$setprn(&k) & 1) && (ovmx_test_self(&me) & 1))
            c = 'Y';
        (void)!write(ready[1], &c, 1);
        (void)!write(ready[1], &me, sizeof me);
        (void)!read(go[0], &c, 1);
        _exit(0);
    }
    {
        char c = 'N';
        (void)!read(ready[0], &c, 1);
        (void)!read(ready[0], &kid, sizeof kid);
        check(c == 'Y' && kid != 0, "a subprocess named PSCANKID is running");
    }

    struct ps_ile eql[] = { { 8, PSCAN$_PRCNAM, (uint32_t)(uintptr_t)s_kid, PSCAN$M_EQL }, { 0, 0, 0, 0 } };
    check(scan(eql, sel, &n) == SS$_NORMAL, "$PROCESS_SCAN accepts PRCNAM EQL");
    /* negctl: pscan-criteria-ignored */
    check(n == 1 && sel[0] == kid, "PRCNAM EQL \"PSCANKID\" selects exactly the subprocess, then SS$_NOMOREPROC");

    struct ps_ile wild[] = { { 6, PSCAN$_PRCNAM, (uint32_t)(uintptr_t)s_wild, PSCAN$M_WILDCARD }, { 0, 0, 0, 0 } };
    (void)scan(wild, sel, &n);
    /* negctl: pscan-criteria-ignored */
    check(n == 2 && has(sel, n, kid) && has(sel, n, self), "PRCNAM \"PSCAN*\" with WILDCARD selects both, nothing else");

    struct ps_ile lower[] = { { 8, PSCAN$_PRCNAM, (uint32_t)(uintptr_t)s_lower, PSCAN$M_EQL }, { 0, 0, 0, 0 } };
    (void)scan(lower, sel, &n);
    /* negctl: pscan-criteria-ignored */
    check(n == 0, "\"pscankid\" selects nothing without CASE_BLIND");
    struct ps_ile blind[] = { { 8, PSCAN$_PRCNAM, (uint32_t)(uintptr_t)s_lower, PSCAN$M_EQL | PSCAN$M_CASE_BLIND }, { 0, 0, 0, 0 } };
    (void)scan(blind, sel, &n);
    /* negctl: pscan-criteria-ignored */
    check(n == 1 && sel[0] == kid, "\"pscankid\" with CASE_BLIND selects the subprocess");

    struct ps_ile orl[] = { { 8, PSCAN$_PRCNAM, (uint32_t)(uintptr_t)s_kid, PSCAN$M_EQL | PSCAN$M_OR },
                            { 8, PSCAN$_PRCNAM, (uint32_t)(uintptr_t)s_mom, PSCAN$M_EQL }, { 0, 0, 0, 0 } };
    (void)scan(orl, sel, &n);
    /* negctl: pscan-criteria-ignored */
    check(n == 2 && has(sel, n, kid) && has(sel, n, self), "PRCNAM KID OR PRCNAM MOM selects both");

    struct ps_ile state[] = { { 0, PSCAN$_STATE, 1, PSCAN$M_EQL }, { 0, 0, 0, 0 } };
    uint32_t ctx = 0;
    check(sys$process_scan(&ctx, state) == SS$_UNSUPPORTED,
          "a criterion the executive's row cannot answer (STATE) is SS$_UNSUPPORTED");

    (void)!write(go[1], "x", 1);
    waitpid(child, NULL, 0);

    printf("=== test_syssvc_process_scan: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
