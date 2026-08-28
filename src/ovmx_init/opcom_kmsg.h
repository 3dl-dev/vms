/*
 * opcom_kmsg.h - /dev/kmsg -> SYS$MANAGER:OPERATOR.LOG bridge (rd vms-32a)
 *
 * vms.ko / vmsfs.ko emit their lifecycle events (module init, the device
 * table forming, the logical-name arena coming up, ...) as ordinary
 * pr_info/pr_warn/pr_err calls, which land in the Linux kernel ring buffer
 * and nowhere a VMS operator can see them. This bridge reads /dev/kmsg (the
 * standard, pollable kernel-log device -- no kernel change, no new /dev/vms
 * channel), reformats records as bare VMS lines ("%FACILITY-<sev>-<ident>,
 * text"), and appends every routed line to SYS$MANAGER:OPERATOR.LOG.
 *
 * THE CONSOLE IS NEVER TOUCHED BY THIS FILE (operator correction,
 * 2026-08-12, round 3 -- the definitive fix). `tests/qemu/
 * test_boot_conformance.sh` pins the EXACT ordered sequence of
 * %FACILITY-SEVERITY-IDENT tokens the boot console may show, derived from
 * the OpenVMS Alpha oracle (vms-1fb) and produced entirely by the boot
 * orchestrator (`ovmx_init`/`PROVISION.EXE`/`SYSTARTUP_VMS.COM`) -- never by
 * a kernel module's own printk. Two earlier cuts of this bridge (routing
 * OVMX-facility kernel-module lines to the console; then also routing
 * SYSKRNL lines to the console) both measurably broke that pinned sequence
 * -- kernel printk, reformatted or not, does not belong on the VMS console
 * (see that test's own header comment, which states this as a design
 * invariant, not a case-by-case judgment call). So: EVERYTHING this bridge
 * routes -- vms.ko/vmsfs.ko's own `OVMX`-facility records AND re-styled
 * `SYSKRNL` lines from the Linux kernel layer underneath -- goes to
 * OPERATOR.LOG only. `opcom_kmsg_classify()` therefore has only two
 * outcomes: `OPCOM_KMSG_DROP` or `OPCOM_KMSG_OPERATOR_LOG`.
 *
 * See docs/design-opcom-executive-logging.md for the full design: the
 * two-vocabulary model (boot-console lines vs. OPCOM records -- this
 * bridge produces neither; see that doc's round-3 section for why
 * OPERATOR.LOG-only is not a vocabulary-B claim either), the route-by-
 * default filter (only routine INFO/DEBUG-level SYSKRNL boilerplate is
 * dropped as genuinely operator-worthless), and the OVMX/SYSKRNL
 * facility+ident choices (Rule 8).
 */

#ifndef OVMX_OPCOM_KMSG_H
#define OVMX_OPCOM_KMSG_H

#include <stddef.h>

/*
 * opcom_kmsg_classify() return values. Named constants, not a bare 0/1, so
 * callers (the reader thread, unit tests) read as intent rather than a
 * magic number.
 */
#define OPCOM_KMSG_DROP         0  /* suppressed -- genuinely zero operator value */
#define OPCOM_KMSG_OPERATOR_LOG 1  /* routed -- appended to OPERATOR.LOG, NEVER the console */

/*
 * opcom_kmsg_classify - decide whether one /dev/kmsg record reaches the
 * operator (via OPERATOR.LOG -- see this header's own top comment for why
 * the console is never a destination here), and if so, in what shape.
 *
 * pri:  the record's syslog priority exactly as /dev/kmsg's leading field
 *       carries it (facility<<3 | level); only the low 3 bits (level) are
 *       used.
 * text: the record's message text -- the bytes after the record's ';'
 *       delimiter, up to (not including) the first '\n' or any KEY=VALUE
 *       continuation line. Not modified.
 * out/outsz: on a routed record (return value == OPCOM_KMSG_OPERATOR_LOG),
 *       filled with "%FACILITY-<S>-<IDENT>, <text>\n" (NUL-terminated,
 *       truncated to fit outsz if necessary).
 *
 * Returns:
 *   OPCOM_KMSG_OPERATOR_LOG -- EITHER a genuine OVMX executive event (text
 *       starts with "vms: "/"vmsfs: "), any level up to kernel debug,
 *       wearing the OVMX facility; OR a SYSKRNL (Linux-kernel-layer) line
 *       at NOTICE severity or more severe (level<=5), re-styled rather
 *       than suppressed (it carries real information: a module-taint
 *       warning, a scheduling-latency warning, ...), wearing the SYSKRNL
 *       facility. Both land in SYS$MANAGER:OPERATOR.LOG only.
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
 * opcom_kmsg_classify()'s destination decision is fully unit-tested; the
 * WRITE mechanism itself, and the console's total silence, are proven
 * end-to-end by tests/qemu/test_job_control_console.sh and
 * tests/qemu/test_boot_conformance.sh against a real boot.
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

/*
 * opcom_kmsg_drain_ringbuffer - one-shot replay of the kernel ring buffer's
 * kmsg records through opcom_kmsg_classify(), handing every routed line (as one
 * stream-LF record's text, no trailing newline) to `emit`.
 *
 * This is how the genuine on-volume SYS$MANAGER:OPERATOR.LOG (Files-11 ODS-2,
 * over the ACP) is seeded with the boot-time vms.ko/vmsfs.ko events post atomic
 * flip (vms-aac, epic vms-208). PID 1 (STARTUP.EXE) is static and carries no
 * RMS, so it cannot write the ACP log itself; PROVISION.EXE -- which links RMS
 * and runs AFTER the SYS$DISK $MOUNT -- calls this with an `emit` that does
 * rms_textfile_append_line(SYS$MANAGER:OPERATOR.LOG, line). Called then, every
 * boot record is durably buffered and the volume is mounted, so the replay is
 * race-free. Best-effort: emits nothing if /dev/kmsg cannot be opened; `emit`
 * may be called zero or many times.
 */
void opcom_kmsg_drain_ringbuffer(void (*emit)(const char *line));

#endif /* OVMX_OPCOM_KMSG_H */
