/*
 * vms_alpha_syscall.c - the honest RUNG-1 syscall stub for the OVMX
 * alpha-dec-vms musl port (vms-960).
 *
 * INV-6: this returns a real error (-ENOSYS). It NEVER fakes success. Every
 * syscall-dependent libc routine that reaches here fails with ENOSYS and the
 * caller's __syscall_ret() turns that into errno=ENOSYS / return -1.
 *
 * The real backend (the OVMX Alpha executive) lands in GAP3 (vms-8954): it will
 * replace this translation unit with the actual trap/executive-facility path.
 * The intended Alpha syscall ABI is documented in
 * arch/alpha-dec-vms/syscall_arch.h.
 *
 * It is deliberately impossible for this to appear "wired": grep for
 * __vms_alpha_syscall to find the single point where the executive backend
 * gets soldered in.
 */

#include <errno.h>

long __vms_alpha_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	(void)n; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
	/* RUNG 1: no executive backend yet. Fail honestly. */
	return -ENOSYS;
}
