/*
 * syscall_arch.h - Alpha syscall entry.  OVMX alpha-dec-vms musl port.
 *
 * ==========================================================================
 * vms-157 (last rung of P1): the syscall path is now REAL. Every __syscallN
 * below funnels into __vms_alpha_syscall() (src/internal/vms_alpha_syscall.c),
 * which is now an actual Alpha `callsys` (CALL_PAL 0x83) trap into the
 * Linux-Alpha kernel -- no longer the rung-1 -ENOSYS stub. INV-6 still holds:
 * a failed syscall returns a genuine negative errno; nothing is faked.
 * ==========================================================================
 *
 * Alpha/OSF syscall ABI (implemented in vms_alpha_syscall.c):
 *   $0  = syscall number on entry; result on return
 *   $16..$21 (a0..a5) = up to six arguments
 *   entry via  callsys  (CALL_PAL 0x83)
 *   on return $19 (a3) = 0 on success / nonzero on error, $0 = value/errno
 *   (the backend negates v0 on the error path so callers see -errno).
 * The syscall numbers themselves are the authoritative Alpha values in
 * arch/alpha-dec-vms/bits/syscall.h.in.
 */

#include <errno.h>

long __vms_alpha_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6);

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

static inline long __syscall0(long n)
{
	return __vms_alpha_syscall(n, 0, 0, 0, 0, 0, 0);
}
static inline long __syscall1(long n, long a)
{
	return __vms_alpha_syscall(n, a, 0, 0, 0, 0, 0);
}
static inline long __syscall2(long n, long a, long b)
{
	return __vms_alpha_syscall(n, a, b, 0, 0, 0, 0);
}
static inline long __syscall3(long n, long a, long b, long c)
{
	return __vms_alpha_syscall(n, a, b, c, 0, 0, 0);
}
static inline long __syscall4(long n, long a, long b, long c, long d)
{
	return __vms_alpha_syscall(n, a, b, c, d, 0, 0);
}
static inline long __syscall5(long n, long a, long b, long c, long d, long e)
{
	return __vms_alpha_syscall(n, a, b, c, d, e, 0);
}
static inline long __syscall6(long n, long a, long b, long c, long d, long e, long f)
{
	return __vms_alpha_syscall(n, a, b, c, d, e, f);
}

#define IPC_64 0
