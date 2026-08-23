/*
 * syscall_arch.h - Alpha syscall entry.  OVMX alpha-dec-vms musl port
 * (vms-960, RUNG 1): HONEST STUB, not a real syscall path.
 *
 * ==========================================================================
 * RUNG 1 STATUS: UNIMPLEMENTED-BUT-HONEST.
 * Every __syscallN routes to __vms_alpha_syscall(), which returns -ENOSYS.
 * Nothing is faked: a syscall-dependent libc function that reaches the kernel
 * will fail with ENOSYS and return -1, exactly as an unimplemented syscall
 * must.  This is INV-6 compliant (fail honestly; never fake success).
 * ==========================================================================
 *
 * The REAL backend (GAP3 / vms-8954, the OVMX Alpha executive) will replace
 * __vms_alpha_syscall() with the actual trap sequence into the executive.
 *
 * Intended real Alpha ABI (documented now so GAP3 has the contract; NOT wired
 * at rung 1): the classic Alpha/OSF syscall convention is
 *   $0  = syscall number on entry; result on return
 *   $16..$21 (a0..a5) = up to six arguments
 *   entry via  callsys  (CALL_PAL 0x83)
 *   on return $19 (a3) = 0 on success / nonzero on error, $0 = value/errno
 * The OVMX executive backend may instead present a $QIO/executive-facility
 * interface; see src/libvmssys/vms_kif.h for the executive-facility contract
 * that the raw POSIX-over-executive backend will build on. That decision is
 * GAP3's, not rung 1's.
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
