/*
 * test_ctrl_t_status.c - the reflexive Ctrl/T status line (vms-0d75)
 *
 * Exercises dcl_format_ctrl_t_status() -- the pure renderer behind the DCL
 * Ctrl/T (0x14) handler -- against a struct vms_procinfo, which IS the
 * executive's $GETJPI contract (src/kernel/vms_ioctl.h). This is the real
 * data boundary the handler reads through vms_kif_getjpi_self(); the test
 * hands the renderer a row exactly as the executive would fill one, so it
 * proves format + real-field consumption WITHOUT monkeypatching any DCL
 * internal.
 *
 * FORMAT under test (OpenVMS User's Manual, "Ctrl/T"):
 *     NODE::USER   hh:mm:ss   [image   ]CPU=hh:mm:ss.cc PF=n [IO=n ]MEM=n
 *
 * The IO field must be ABSENT when JPI$_DIRIO/BUFIO are unsourced (their
 * fields_valid bits clear) -- that is the INV-6 anti-fabrication guarantee,
 * asserted directly below.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "ssdef.h"
#include "vms_kif.h"        /* struct vms_procinfo, VMS_PI_V_* */
#include "dcl/terminal.h"   /* dcl_format_ctrl_t_status */

static int failures = 0;
#define check(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", (msg)); } \
    else { printf("  FAIL: %s\n", (msg)); failures++; } \
} while (0)

/* Build the local HH:MM:SS the renderer will produce for `now`, the same way
 * (localtime_r), so the assertion is timezone-independent. */
static void expected_hms(time_t now, char *buf, size_t len)
{
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(buf, len, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

int main(void)
{
    printf("Testing dcl_format_ctrl_t_status (vms-0d75 Ctrl/T)...\n");

    /* A row exactly as fill_proc_acct() populates one: CPU/PF/PAGES sourced,
     * DIRIO/BUFIO deliberately unsourced (bits clear). */
    struct vms_procinfo info;
    memset(&info, 0, sizeof(info));
    strncpy(info.username, "MCCARTHY", sizeof(info.username) - 1);
    info.cputim   = 333;    /* 10ms units -> 00:00:03.33 */
    info.pageflts = 778;
    info.pages    = 315;
    info.dirio    = 999;    /* present in the struct but bit is CLEAR ... */
    info.bufio    = 999;    /* ... so the renderer must NOT read them */
    info.fields_valid = VMS_PI_V_CPUTIM | VMS_PI_V_PAGEFLTS | VMS_PI_V_PAGES;

    time_t now = 1723556702;   /* fixed instant; HMS derived below */
    char hms[16];
    expected_hms(now, hms, sizeof(hms));

    char line[256];
    uint32_t st = dcl_format_ctrl_t_status(&info, "GREEN", "", now,
                                           line, sizeof(line));
    check(st & 1, "renderer returns a VMS success status");
    printf("  LINE: [%s]\n", line);

    /* Identity: node::user, VMS's "::" separator, executive username. */
    check(strncmp(line, "GREEN::MCCARTHY", 15) == 0,
          "line begins NODE::USER from the executive row");

    /* Wall-clock time of day. */
    check(strstr(line, hms) != NULL, "current time (hh:mm:ss) present");

    /* CPU=hh:mm:ss.cc from JPI$_CPUTIM (10ms units). */
    check(strstr(line, "CPU=00:00:03.33") != NULL,
          "CPU field rendered from JPI$_CPUTIM (00:00:03.33)");

    /* PF from JPI$_PAGEFLTS, MEM from JPI$_PPGCNT. */
    check(strstr(line, "PF=778") != NULL, "PF field from JPI$_PAGEFLTS (778)");
    check(strstr(line, "MEM=315") != NULL, "MEM field from JPI$_PPGCNT (315)");

    /* INV-6: IO has no faithful source, so it is OMITTED, never fabricated
     * -- and specifically never the plausible-looking dirio+bufio (1998). */
    check(strstr(line, "IO=") == NULL,
          "IO field omitted (JPI$_DIRIO/BUFIO unsourced -- no fabrication)");
    check(strstr(line, "1998") == NULL,
          "no dirio+bufio value leaked into the line");

    /* No image name at the DCL prompt: the field collapses, and the CPU
     * group still follows the time directly. */
    check(strstr(line, "  CPU=") != NULL || strstr(line, " CPU=") != NULL,
          "empty image collapses; CPU group follows time");

    /* Field ordering: CPU precedes PF precedes MEM (VMS layout). */
    const char *pc = strstr(line, "CPU=");
    const char *pp = strstr(line, "PF=");
    const char *pm = strstr(line, "MEM=");
    check(pc && pp && pm && pc < pp && pp < pm,
          "field order is CPU ... PF ... MEM");

    /* A running image IS named in the field when supplied. */
    char line2[256];
    st = dcl_format_ctrl_t_status(&info, "GREEN", "EVE", now,
                                  line2, sizeof(line2));
    check((st & 1) && strstr(line2, "EVE") != NULL &&
          strstr(line2, "EVE") < strstr(line2, "CPU="),
          "image name appears between time and CPU when present");

    /* Honest degrade: a row missing the CPU valid bit omits the CPU token
     * rather than printing a fabricated 00:00:00.00. */
    struct vms_procinfo degraded = info;
    degraded.fields_valid = VMS_PI_V_PAGEFLTS | VMS_PI_V_PAGES;  /* no CPU */
    char line3[256];
    st = dcl_format_ctrl_t_status(&degraded, "GREEN", "", now,
                                  line3, sizeof(line3));
    check((st & 1) && strstr(line3, "CPU=") == NULL,
          "unsourced CPU (bit clear) omitted, not fabricated");
    check(strstr(line3, "PF=778") != NULL,
          "still renders the fields that ARE sourced when CPU is absent");

    /* Bad-argument guard. */
    check(!(dcl_format_ctrl_t_status(NULL, "X", "", now, line, sizeof(line)) & 1),
          "NULL info -> honest error status");

    if (failures == 0) {
        printf("All Ctrl/T status-line tests passed.\n");
        return 0;
    }
    printf("%d Ctrl/T status-line test(s) FAILED.\n", failures);
    return 1;
}
