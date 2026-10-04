/*
 * test_console_policy.c - the console-level pair that decides whether the
 * EXECUTIVE can be heard on OPA0: at all (rd vms-151).
 *
 * WHAT WENT WRONG, and what this file pins so it cannot happen again.
 * Two OVMX/x86 nodes booted together, elected a founder, formed generation 1,
 * admitted each other and reached MEMBER with real CSIDs -- and neither
 * console printed one word of it. Not a hang, not a fabrication: the
 * executive's OPA0: lines are printk records at OVMX_OPA0_PRINTK_LEVEL, and
 * PID 1's own ovmx_boot_mute_kernel_console() had lowered the console sink to
 * exactly that level. Linux emits a record iff its level is STRICTLY LESS
 * than console_loglevel, so "3 muted at 3" swallowed every one of them. The
 * cluster was real; the console was mute.
 *
 * The two numbers now live together in src/kernel/ovmx_console_policy.h --
 * the REAL header both rinds compile against, not a copy -- and this test
 * states the four facts that header exists to guarantee. Change either
 * constant the wrong way and these go red:
 *
 *   1. the executive's OPA0: level REACHES the console under PID 1's mute --
 *      the regression itself, in one line;
 *   2. bugcheck-class kernel faults (EMERG/ALERT/CRIT) still reach it, which
 *      is what the mute was always allowed to keep;
 *   3. the module lifecycle chatter vms-300 muted (WARNING/NOTICE/INFO/DEBUG)
 *      stays muted -- this fix must not re-open that leak;
 *   4. the OPA0: level is the ERR level and no more alarming, because
 *      opcom_kmsg_classify() derives an OPERATOR.LOG record's SEVERITY from
 *      this same number: an operator line filed as %...-F- would be a false
 *      severity claim about a healthy cluster forming.
 *
 * Pure constants and one inline predicate -- no syscall, no /proc write, and
 * deliberately no call to ovmx_boot_mute_kernel_console() itself: running the
 * real muter in a test would lower the console level of whatever host (or
 * privileged CI container) the suite happens to run on.
 */

#include "ovmx_console_policy.h"

#include <stdio.h>

static int failures;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("ok   %s\n", what);
        return;
    }
    printf("FAIL %s (got %d, want %d)\n", what, got, want);
    failures++;
}

/* The Linux printk levels this policy is expressed in (kern_levels.h's
 * LOGLEVEL_*). Named here rather than included: this test is hosted code and
 * must not pull a kernel header, and the numbers are the syslog(3) severities
 * every Unix has used since the 1980s. */
#define LVL_EMERG   0
#define LVL_ALERT   1
#define LVL_CRIT    2
#define LVL_ERR     3
#define LVL_WARNING 4
#define LVL_NOTICE  5
#define LVL_INFO    6
#define LVL_DEBUG   7

static void reaches(const char *what, int level, int want)
{
    check(what,
          ovmx_console_level_reaches_console(level, OVMX_CONSOLE_MUTE_LEVEL),
          want);
}

int main(void)
{
    printf("=== test_console_policy (rd vms-151) ===\n");

    /* 1. THE REGRESSION: the executive's own OPA0: lines get through. */
    reaches("the executive's OPA0: level reaches the muted console",
            OVMX_OPA0_PRINTK_LEVEL, 1);

    /* 2. a bugcheck-class kernel fault still reaches the operator. */
    reaches("EMERG reaches the muted console", LVL_EMERG, 1);
    reaches("ALERT reaches the muted console", LVL_ALERT, 1);
    reaches("CRIT reaches the muted console", LVL_CRIT, 1);

    /* 3. vms-300's module chatter stays off the console. */
    reaches("WARNING (pr_warn module chatter) stays muted", LVL_WARNING, 0);
    reaches("NOTICE stays muted", LVL_NOTICE, 0);
    reaches("INFO (pr_info lifecycle chatter) stays muted", LVL_INFO, 0);
    reaches("DEBUG stays muted", LVL_DEBUG, 0);

    /* 4. the OPA0: level is ERR -- no more alarming, so the OPERATOR.LOG
     *    record opcom_kmsg_classify() derives from it is not a false -F-. */
    check("the executive's OPA0: level is the ERR level",
          OVMX_OPA0_PRINTK_LEVEL, LVL_ERR);

    /* ... and the predicate itself is the Linux rule, not a tautology: a
     *     record AT the console level is dropped, one below it is printed. */
    check("a record at the console level is dropped",
          ovmx_console_level_reaches_console(OVMX_CONSOLE_MUTE_LEVEL,
                                             OVMX_CONSOLE_MUTE_LEVEL), 0);
    check("a record one level below the console level is printed",
          ovmx_console_level_reaches_console(OVMX_CONSOLE_MUTE_LEVEL - 1,
                                             OVMX_CONSOLE_MUTE_LEVEL), 1);

    printf("\n=== test_console_policy: %d failed ===\n", failures);
    return failures == 0 ? 0 : 1;
}
