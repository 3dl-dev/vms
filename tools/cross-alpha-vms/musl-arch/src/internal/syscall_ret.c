/*
 * syscall_ret.c - OVMX alpha-dec-vms musl port overlay (vms-430).
 *
 * The return-leg counterpart of the syscall-ARGUMENT width fix. On this LLP64
 * port `long` is 32 bits while pointers (and the Linux-Alpha syscall result
 * register) are 64 bits. Stock musl's __syscall_ret is:
 *
 *     long __syscall_ret(unsigned long r)
 *
 * On alpha-dec-vms both the `unsigned long` parameter and the `long` return
 * TRUNCATE a 64-bit value to 32 bits. Every pointer-returning syscall wrapper
 * funnels its raw result through __syscall_ret (mmap: `(void*)__syscall_ret(ret)`;
 * mremap/sbrk/shmat: `(void*)syscall(...)` == `(void*)__syscall_ret(__syscall(...))`),
 * so a genuine mmap address like 0x2000_0000_a000 is chopped to 0xa000 here even
 * if the caller kept it 64-bit -- breaking all mmap-backed malloc (musl mallocng
 * behind decc$_malloc64: buf=0xa000 -> memset(0xa000,...) -> SIGSEGV).
 *
 * Widen both legs to 64-bit (`long long` / `unsigned long long`). The
 * negative-errno detection (`r > -4096UL`) and errno mapping are preserved: on
 * this port a 32-bit truncated negative sign-extends into the 64-bit param, and
 * -4096ULL as unsigned long long is the correct 64-bit sentinel. Non-pointer
 * callers (read/write byte counts, fds) are unaffected -- they read a small
 * value that fits, and __syscall_ret returns it cast to their own type as before.
 *
 * The matching declaration is widened in src/internal/syscall.h (build-musl.sh).
 */
#include <errno.h>
#include "syscall.h"

long long __syscall_ret(unsigned long long r)
{
	if (r > -4096ULL) {
		errno = -r;
		return -1;
	}
	return r;
}
