/*
 * opcom_kmsg.h - /dev/kmsg -> operator-console bridge (rd vms-32a)
 *
 * vms.ko / vmsfs.ko emit their lifecycle events (module init, the device
 * table forming, the logical-name arena coming up, ...) as ordinary
 * pr_info/pr_warn/pr_err calls, which land in the Linux kernel ring buffer
 * and nowhere a VMS operator can see them. This bridge reads /dev/kmsg (the
 * standard, pollable kernel-log device -- no kernel change, no new /dev/vms
 * channel) and reformats records as bare VMS console lines
 * ("%FACILITY-<sev>-<ident>, text"), written to the OPA0: physical console
 * (/dev/console). vms.ko/vmsfs.ko's own records wear the OVMX facility;
 * lines from the Linux kernel layer underneath that clear the routing bar
 * (operator correction, 2026-08-12: RE-STYLED, not suppressed -- see
 * below) wear SYSKRNL.
 *
 * See docs/design-opcom-executive-logging.md for the full design: the
 * two-vocabulary model (boot-console lines vs. OPCOM records), the
 * route-by-default filter (only routine INFO/DEBUG-level SYSKRNL
 * boilerplate is dropped as genuinely operator-worthless; everything else,
 * including re-styled SYSKRNL warnings/notices, is routed), and the
 * OVMX/SYSKRNL facility+ident choices (Rule 8 -- these events have no
 * real-VMS equivalent wording, so the format is VMS-faithful and the
 * content stays honestly OVMX's/the kernel layer's own).
 */

#ifndef OVMX_OPCOM_KMSG_H
#define OVMX_OPCOM_KMSG_H

#include <stddef.h>

/*
 * opcom_kmsg_classify - decide whether one /dev/kmsg record reaches the
 * operator console, and if so, format it.
 *
 * pri:  the record's syslog priority exactly as /dev/kmsg's leading field
 *       carries it (facility<<3 | level); only the low 3 bits (level) are
 *       used.
 * text: the record's message text -- the bytes after the record's ';'
 *       delimiter, up to (not including) the first '\n' or any KEY=VALUE
 *       continuation line. Not modified.
 * out/outsz: on a routed record, filled with "%FACILITY-<S>-<IDENT>, <text>\n"
 *       (NUL-terminated, truncated to fit outsz if necessary).
 *
 * Returns 1 and fills *out* when the record clears the routing bar:
 *   - a genuine OVMX executive event (text starts with "vms: "/"vmsfs: "),
 *     any level up to kernel debug -- wears the OVMX facility; or
 *   - a SYSKRNL (Linux-kernel-layer) line at NOTICE severity or more
 *     severe (level<=5) -- wears the SYSKRNL facility, re-styled rather
 *     than suppressed (it carries real information: a module-taint
 *     warning, a scheduling-latency warning, ...).
 * Returns 0 -- *out* is untouched -- for kernel debug-level records
 * (either facility) and routine INFO-level SYSKRNL boilerplate (device/
 * bus-enumeration chatter with no vms:/vmsfs: prefix and no elevated
 * severity) -- the "genuinely zero operator value" bucket.
 *
 * Pure function, no I/O -- unit-tested directly (tests/ovmx_init/
 * test_opcom_kmsg.c) without needing a real kernel or /dev/kmsg.
 */
int opcom_kmsg_classify(int pri, const char *text, char *out, size_t outsz);

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
