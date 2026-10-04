/*
 * ovmx_console_policy.h - WHICH KERNEL-LOG LEVELS REACH THE NODE'S CONSOLE
 * on the Linux substrate, and the one invariant that binds the two halves of
 * that decision together (rd vms-151).
 *
 * THE BUG THIS HEADER EXISTS TO PREVENT (MEASURED 2026-09-23, two OVMX/x86
 * nodes forming a cluster from cold on the in-browser rig). Two files, each
 * right on its own terms, disagreed by exactly one level:
 *
 *   - exec_kbackend_linux.h emits the executive's OPA0: lines -- the
 *     "%CNXMAN, ..." / "%PEA0, ..." operator messages the connection manager
 *     composes -- with printk at KERN_ERR, chosen (exec_kbackend.h SS18)
 *     because KERN_INFO is suppressed at the console level the harness reads;
 *
 *   - ovmx_boot_linux.c's ovmx_boot_mute_kernel_console() (vms-300) lowered
 *     the console sink to 3 to stop routine module chatter from interleaving
 *     with the VMS boot banner.
 *
 * Linux prints a record on the console iff its level is STRICTLY LESS than
 * console_loglevel, so a sink of 3 swallows level 3 -- every OPA0: line the
 * executive has ever written. The cluster formed correctly and silently: both
 * nodes reached MEMBER with real CSIDs, and neither console ever said so. It
 * looked exactly like a hang. Nothing in either file was wrong on its own;
 * what was missing was a place where the two numbers have to agree.
 *
 * So both constants live HERE, with the invariant asserted at compile time,
 * and each side uses the constant rather than a literal of its own. The
 * header is deliberately pure #defines plus one inline predicate: it is
 * included from kernel context (the vms.ko rind, -nostdinc) AND from PID 1's
 * boot rind (hosted userland, src/ovmx_init/CMakeLists.txt already has
 * src/kernel on its include path), so it may depend on nothing from either.
 *
 * LINUX ONLY, by construction. exec_console_printf() on the NetBSD-VAX
 * substrate is printf(9), which no console level filters, and
 * ovmx_boot_mute_kernel_console() is a documented no-op there (vms-f2e) --
 * there is no pair of numbers to keep in step, so there is nothing for that
 * substrate to include.
 */

#ifndef OVMX_CONSOLE_POLICY_H
#define OVMX_CONSOLE_POLICY_H

/*
 * THE LEVEL THE EXECUTIVE'S OPA0: LINES ARE EMITTED AT (== Linux LOGLEVEL_ERR
 * / KERN_ERR). Not a severity claim about the message: a membership
 * announcement is not an error. It is the level this substrate reserves for
 * "the VMS executive is speaking to the operator", kept ABOVE the module
 * chatter vms-300 muted and BELOW the console sink so the operator sees it.
 * The severity an OPERATOR.LOG record wears is derived from this same level by
 * opcom_kmsg_classify(), which is the other reason not to reach for a more
 * alarming level here: an OPA0: line at CRIT would be filed as %...-F-.
 */
#define OVMX_OPA0_PRINTK_LEVEL   3

/*
 * THE CONSOLE SINK PID 1 SETS (ovmx_boot_mute_kernel_console()). Records with
 * a level < this reach the console; everything at or above it is dropped from
 * the console SINK only (never from /dev/kmsg, which the OPERATOR.LOG bridge
 * keeps reading in full -- vms-32a).
 *
 * 4 = EMERG/ALERT/CRIT reach the console (bugcheck-class kernel faults, as a
 * VAX would bugcheck to its own console), and so does ERR -- which is
 * OVMX_OPA0_PRINTK_LEVEL, the executive's operator lines, plus the handful of
 * genuine pr_err() failures in vms.ko's init path (a slab cache that could not
 * be created, /dev/vms that could not be registered), which an operator must
 * see for the same reason. WARNING/NOTICE/INFO/DEBUG -- the module lifecycle
 * chatter vms-300 was raised about -- stay muted, exactly as before.
 */
#define OVMX_CONSOLE_MUTE_LEVEL  4

/*
 * Linux's own console rule (kernel/printk/printk.c: a record is emitted iff
 * `level < console_loglevel`), written once so the tests and the two rinds
 * ask the question the same way instead of re-deriving the comparison.
 */
static inline int ovmx_console_level_reaches_console(int level,
						     int console_level)
{
	return level < console_level;
}

/*
 * THE INVARIANT. If this ever fails to hold, the executive goes mute on the
 * console again -- which is not a build error anywhere else, and was not
 * noticed for months.
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(OVMX_OPA0_PRINTK_LEVEL < OVMX_CONSOLE_MUTE_LEVEL,
	       "the executive's OPA0: lines must survive PID 1's console mute");
#endif

#endif /* OVMX_CONSOLE_POLICY_H */
