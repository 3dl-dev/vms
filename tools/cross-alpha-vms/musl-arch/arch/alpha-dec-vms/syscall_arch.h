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

/*
 * vms-157 -- 64-bit syscall argument ABI, MANDATORY on this LLP64 port.
 *
 * alpha-dec-vms is the OpenVMS "P64"/LLP64 model: int=4, LONG=4, long long=8,
 * pointer=8 (build-musl.sh's preflight asserts exactly this). But the runtime
 * kernel is Linux-Alpha, which is LP64: every syscall argument is a full 64-bit
 * register, and pointers are 64 bits. musl's generic syscall glue assumes
 * sizeof(long)==sizeof(void*) and casts every argument through __scc == (long).
 * On this port `long` is only 32 bits, so that cast TRUNCATES (and sign-extends)
 * any pointer argument -- e.g. a stack-resident iovec at 0x7743_c8937790 becomes
 * 0xffffffff_c8937790 and writev() EFAULTs. syscall_arch.h is included by
 * src/internal/syscall.h BEFORE its `#ifndef __scc` fallback, so we override the
 * cast and the arg type here to the 64-bit `long long`. This is the port's
 * syscall register width, independent of the VMS `long`.
 */
#define __scc(X) ((long long)(X))
typedef long long syscall_arg_t;

long long __vms_alpha_syscall(long long n, long long a1, long long a2,
			      long long a3, long long a4, long long a5,
			      long long a6);

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

/*
 * vms-430 -- 64-bit syscall RETURN ABI, the return-leg counterpart of the
 * 64-bit ARGUMENT ABI above (__scc / syscall_arg_t == long long).
 *
 * On this LLP64 port `long` is only 32 bits, but the Linux-Alpha kernel returns
 * a full 64-bit register: pointer-returning syscalls (mmap/mremap/brk) hand back
 * a 64-bit address. A `long`-typed __syscallN would narrow that address to its
 * low 32 bits (the compiler emits stl/ldl/sextl), so a genuine mmap result like
 * 0x2000_0000_a000 collapses to 0xa000 -- and every mmap-backed malloc (musl
 * mallocng behind decc$_malloc64) then memsets a bogus low address and SIGSEGVs.
 * The backend __vms_alpha_syscall() already returns `long long`; keep that width
 * all the way out by typing the raw return here as `long long` too. (The shared
 * __syscall_ret is widened to match in src/internal/syscall_ret.c + syscall.h.)
 * The argument side (__scc/syscall_arg_t) is unchanged.
 */
static inline long long __syscall0(long long n)
{
	return __vms_alpha_syscall(n, 0, 0, 0, 0, 0, 0);
}
static inline long long __syscall1(long long n, long long a)
{
	return __vms_alpha_syscall(n, a, 0, 0, 0, 0, 0);
}
static inline long long __syscall2(long long n, long long a, long long b)
{
	return __vms_alpha_syscall(n, a, b, 0, 0, 0, 0);
}
static inline long long __syscall3(long long n, long long a, long long b, long long c)
{
	return __vms_alpha_syscall(n, a, b, c, 0, 0, 0);
}
static inline long long __syscall4(long long n, long long a, long long b, long long c,
			     long long d)
{
	return __vms_alpha_syscall(n, a, b, c, d, 0, 0);
}
static inline long long __syscall5(long long n, long long a, long long b, long long c,
			     long long d, long long e)
{
	return __vms_alpha_syscall(n, a, b, c, d, e, 0);
}
static inline long long __syscall6(long long n, long long a, long long b, long long c,
			     long long d, long long e, long long f)
{
	return __vms_alpha_syscall(n, a, b, c, d, e, f);
}

#define IPC_64 0
