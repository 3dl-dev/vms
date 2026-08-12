/*
 * opcom_kmsg.h - /dev/kmsg -> operator surface bridge (rd vms-32a)
 *
 * vms.ko / vmsfs.ko emit their lifecycle events (module init, the device
 * table forming, the logical-name arena coming up, ...) as ordinary
 * pr_info/pr_warn/pr_err calls, which land in the Linux kernel ring buffer
 * and nowhere a VMS operator can see them. This bridge reads /dev/kmsg (the
 * standard, pollable kernel-log device -- no kernel change, no new /dev/vms
 * channel) and reformats records as bare VMS lines
 * ("%FACILITY-<sev>-<ident>, text"). WHERE a routed line goes depends on
 * its facility:
 *
 *   OVMX    -- vms.ko/vmsfs.ko's own records -- OPA0: physical console
 *              (/dev/console), same as before.
 *   SYSKRNL -- re-styled lines from the Linux kernel layer underneath --
 *              SYS$MANAGER:OPERATOR.LOG ONLY, never the console.
 *
 * CONSOLE-VS-LOG SPLIT (operator correction, 2026-08-12, round 2). PR #358
 * (vms-2213) deliberately suppresses routine kernel printk on OPA0: so the
 * boot console matches the OpenVMS oracle. Writing SYSKRNL lines to the
 * console (this bridge's first cut) measurably reopened that leak on a CI
 * runner whose real hardware/kernel emits far more NOTICE/WARNING chatter
 * than this repo's minimal dev QEMU guest -- flooding the console and
 * stalling the boot before Username:. The fix keeps the information (an
 * operator can TYPE/SEARCH OPERATOR.LOG) without putting it back on the
 * live boot console.
 *
 * See docs/design-opcom-executive-logging.md for the full design: the
 * two-vocabulary model (boot-console lines vs. OPCOM records), the
 * route-by-default filter (only routine INFO/DEBUG-level SYSKRNL
 * boilerplate is dropped as genuinely operator-worthless), the OVMX/SYSKRNL
 * facility+ident choices (Rule 8), and the console-vs-log destination
 * split.
 */

#ifndef OVMX_OPCOM_KMSG_H
#define OVMX_OPCOM_KMSG_H

#include <stddef.h>

/*
 * opcom_kmsg_classify() return values -- WHERE a routed record goes, if
 * anywhere. Named constants, not a bare 0/1/2, so callers (the reader
 * thread, unit tests) read as intent rather than magic numbers.
 */
#define OPCOM_KMSG_DROP         0  /* suppressed -- genuinely zero operator value */
#define OPCOM_KMSG_CONSOLE      1  /* OVMX facility -- OPA0: physical console     */
#define OPCOM_KMSG_OPERATOR_LOG 2  /* SYSKRNL facility -- OPERATOR.LOG only       */

/*
 * opcom_kmsg_classify - decide whether one /dev/kmsg record reaches the
 * operator, and if so, where and in what shape.
 *
 * pri:  the record's syslog priority exactly as /dev/kmsg's leading field
 *       carries it (facility<<3 | level); only the low 3 bits (level) are
 *       used.
 * text: the record's message text -- the bytes after the record's ';'
 *       delimiter, up to (not including) the first '\n' or any KEY=VALUE
 *       continuation line. Not modified.
 * out/outsz: on a routed record (return value != OPCOM_KMSG_DROP), filled
 *       with "%FACILITY-<S>-<IDENT>, <text>\n" (NUL-terminated, truncated
 *       to fit outsz if necessary).
 *
 * Returns:
 *   OPCOM_KMSG_CONSOLE      -- a genuine OVMX executive event (text starts
 *       with "vms: "/"vmsfs: "), any level up to kernel debug. Wears the
 *       OVMX facility. Goes to /dev/console, exactly as before this item's
 *       console-vs-log split.
 *   OPCOM_KMSG_OPERATOR_LOG -- a SYSKRNL (Linux-kernel-layer) line at
 *       NOTICE severity or more severe (level<=5). Wears the SYSKRNL
 *       facility, re-styled rather than suppressed (it carries real
 *       information: a module-taint warning, a scheduling-latency
 *       warning, ...). Goes to SYS$MANAGER:OPERATOR.LOG, never the
 *       console.
 *   OPCOM_KMSG_DROP -- *out* is untouched. Kernel debug-level records
 *       (either facility) and routine INFO-level SYSKRNL boilerplate
 *       (device/bus-enumeration chatter with no vms:/vmsfs: prefix and no
 *       elevated severity) -- the "genuinely zero operator value" bucket.
 *
 * Pure function, no I/O -- unit-tested directly (tests/ovmx_init/
 * test_opcom_kmsg.c) without needing a real kernel or /dev/kmsg.
 */
int opcom_kmsg_classify(int pri, const char *text, char *out, size_t outsz);

/*
 * The OPCOM_KMSG_OPERATOR_LOG write path (opcom_kmsg_append_operator_log(),
 * resolving SYS$MANAGER:OPERATOR.LOG through vmsfs -- the SAME accessor
 * src/libvms/syssvc/sys_operator.c's sys$sndopr uses -- with the SAME /tmp
 * fallback for "no system disk mounted yet") is internal to opcom_kmsg.c,
 * not exposed here. It is deliberately NOT unit-tested by writing to a
 * real file: this repo's dev/CI hosts share a single /vms tree across
 * concurrently-running tests and sessions (see tests/integration/
 * test_opcom_record_body.sh's own header), and a bare unit-test binary
 * probing that shared resource on every ctest invocation is exactly the
 * kind of avoidable contention that produces flaky failures elsewhere.
 * opcom_kmsg_classify()'s destination decision (OPCOM_KMSG_OPERATOR_LOG
 * vs. OPCOM_KMSG_CONSOLE vs. OPCOM_KMSG_DROP) is fully unit-tested; the
 * WRITE mechanism itself is proven end-to-end by
 * tests/qemu/test_job_control_console.sh against a real boot.
 */

/*
 * opcom_kmsg_start - launch the /dev/kmsg reader as a detached background
 * thread. Best-effort: if /dev/kmsg cannot be opened, the thread exits
 * quietly and boot is not affected -- this is an operator-visibility aid,
 * not the executive-attach gate (that gate is /dev/vms, see
 * executive_attach() in ovmx_init.c, and is unchanged by this file).
 *
 * Safe to call once, early in bare-metal boot (after /dev is mounted,
 * before or around vms.ko's own load) so already-emitted records are
 * replayed from the start of the kernel ring buffer.
 */
void opcom_kmsg_start(void);

#endif /* OVMX_OPCOM_KMSG_H */
