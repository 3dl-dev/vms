/*
 * bench_lnm_peropen.c - How many logical-name translations does ONE file open
 *                       actually perform? (vms-ln0)
 *
 * bench_lnm_cost.c measures the cost of a SINGLE translation. That is only
 * half the ruling: the per-open cost is (translations per open) x (cost per
 * translation), and nobody had measured the first factor either.
 *
 * This program links the REAL src/vmsfs/ translation pipeline and the REAL
 * src/vmslnm/ manager, and interposes on lnm_translate() with the linker's
 * --wrap facility. Nothing is stubbed: __wrap_lnm_translate() increments a
 * counter, records the name, and forwards to __real_lnm_translate(). The
 * numbers below are therefore the actual call counts of the shipping
 * vmsfs_to_linux_path() path, not a reading of the source.
 *
 * It also reports which table each translation would have to reach, because
 * that is what decides how many of them cost anything under option A:
 * LNM$PROCESS hits stay in userspace under every option, so only the
 * translations that MISS LNM$PROCESS become executive round trips.
 *
 * Requires no /dev/vms: it measures counts, not kernel latency.
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vms/logical.h"
#include "ssdef.h"
#include "vmsfs/filespec.h"

/* ------------------------------------------------ lnm_translate interposer */

#define MAX_TRACE 64

static int g_calls;
static int g_calls_needing_exec;   /* translations not satisfied by LNM$PROCESS */
static char g_trace[MAX_TRACE][128];
static int g_trace_n;
static lnm_manager_t *g_mgr;

extern uint32_t __real_lnm_translate(lnm_manager_t *mgr, const char *table_name,
                                     const char *logical_name, char *result,
                                     size_t result_size, uint16_t *result_length,
                                     uint32_t *attributes);

uint32_t __wrap_lnm_translate(lnm_manager_t *mgr, const char *table_name,
                              const char *logical_name, char *result,
                              size_t result_size, uint16_t *result_length,
                              uint32_t *attributes)
{
    uint32_t st = __real_lnm_translate(mgr, table_name, logical_name, result,
                                       result_size, result_length, attributes);

    g_calls++;

    /* Would LNM$PROCESS alone have answered this? Ask the real manager for
     * the process table specifically. If yes, the translation never leaves
     * the process under ANY of the three options. */
    const char *where;
    char probe[LNM_MAX_VALUE + 1];
    uint16_t plen = 0;
    uint32_t pst = __real_lnm_translate(mgr, LNM_PROCESS_TABLE, logical_name,
                                        probe, sizeof(probe), &plen, NULL);
    if (pst == SS$_NORMAL) {
        where = "LNM$PROCESS (stays in-process)";
    } else if (st == SS$_NORMAL) {
        where = "job/group/system (EXECUTIVE ROUND TRIP under option A)";
        g_calls_needing_exec++;
    } else {
        where = "not found (still an EXECUTIVE ROUND TRIP under option A)";
        g_calls_needing_exec++;
    }

    if (g_trace_n < MAX_TRACE) {
        snprintf(g_trace[g_trace_n], sizeof(g_trace[0]), "%-20s -> %s",
                 logical_name, where);
        g_trace_n++;
    }
    return st;
}

/* --------------------------------------------------------------- the cases */

extern int vmsfs_to_linux_path(const char *vms_spec, char *linux_path,
                               size_t path_size);

static int total_calls;
static int total_exec;
static int cases;

static void measure(const char *label, const char *spec)
{
    char out[1024];

    g_calls = 0;
    g_calls_needing_exec = 0;
    g_trace_n = 0;

    int st = vmsfs_to_linux_path(spec, out, sizeof(out));

    printf("  %-34s %-40s\n", label, spec);
    printf("      -> %s (status=%d)\n", out, st);
    printf("      lnm_translate calls: %d   of which reach the executive: %d\n",
           g_calls, g_calls_needing_exec);
    for (int i = 0; i < g_trace_n; i++)
        printf("        %s\n", g_trace[i]);
    printf("\n");

    total_calls += g_calls;
    total_exec += g_calls_needing_exec;
    cases++;
}

int main(void)
{
    printf("=== vms-ln0: logical-name translations per file open ===\n");
    printf("Interposed on lnm_translate via -Wl,--wrap; real vmsfs + real vmslnm.\n\n");

    g_mgr = lnm_get_manager();
    if (!g_mgr) {
        printf("FAIL: lnm_get_manager() returned NULL\n");
        return 1;
    }
    lnm_setup_defaults(g_mgr, NULL);

    /* A process-level override, exactly as SET DEFAULT / login would make. */
    lnm_create(g_mgr, LNM_PROCESS_TABLE, "SYS$LOGIN", "DKA0:[USERS.SYSTEM]",
               0, LNM_MODE_USER);

    /* Representative opens, in rough order of how often DCL/RMS does them. */
    measure("system image (SYS$SYSTEM)", "SYS$SYSTEM:LOGINOUT.EXE");
    measure("shareable (SYS$LIBRARY)",   "SYS$LIBRARY:DECC$SHR.EXE");
    measure("login-relative file",       "SYS$LOGIN:LOGIN.COM");
    measure("explicit device",           "DKA0:[USERS.SYSTEM]FOO.DAT");
    measure("help library (SYS$HELP)",   "SYS$HELP:HELPLIB.HLB");
    measure("device-less, dir only",     "[USERS.SYSTEM]BAR.TXT");

    printf("TOTALS over %d representative opens:\n", cases);
    printf("  lnm_translate calls           : %d  (mean %.2f per open)\n",
           total_calls, (double)total_calls / (double)cases);
    printf("  calls reaching the executive  : %d  (mean K = %.2f per open)\n",
           total_exec, (double)total_exec / (double)cases);
    printf("\nK is the multiplier to apply to the per-translation ioctl cost\n");
    printf("measured by bench_lnm_cost.\n");
    printf("\nPASS: bench_lnm_peropen\n");
    return 0;
}
