/*
 * mremap.c - musl __mremap for alpha-dec-vms with VMS P0 placement (vms-122).
 * A mapping that must move (MREMAP_MAYMOVE without MREMAP_FIXED) is first
 * grown in place; if it cannot be, it is moved to a range claimed in P0
 * (p0_region.h) instead of wherever the kernel would put it (above 4 GB).
 * The result travels through __syscall/__syscall_ret at full width: the stock
 * file returned it through syscall()'s 32-bit `long` on this LLP64 port.
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include "syscall.h"
#include "p0_region.h"

static void dummy(void) { }
weak_alias(dummy, __vm_wait);

void *__mremap(void *old_addr, size_t old_len, size_t new_len, int flags, ...)
{
	va_list ap;
	void *new_addr = 0;
	long long r;

	if (new_len >= PTRDIFF_MAX) {
		errno = ENOMEM;
		return MAP_FAILED;
	}

	if (flags & MREMAP_FIXED) {
		__vm_wait();
		va_start(ap, flags);
		new_addr = va_arg(ap, void *);
		va_end(ap);
		r = __syscall(SYS_mremap, old_addr, old_len, new_len, flags, new_addr);
		return (void *)__syscall_ret(r);
	}

	/* In place first (no move). */
	r = __syscall(SYS_mremap, old_addr, old_len, new_len, 0, 0);
	if (r >= 0 || !(flags & MREMAP_MAYMOVE))
		return (void *)__syscall_ret(r);

	/* Move into P0: claim the destination, then move onto it (MREMAP_FIXED
	 * replaces the placeholder atomically). */
	long long dst = __ovmx_p0_map(new_len, PROT_NONE,
	                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (dst < 0)
		return (void *)__syscall_ret(dst);
	r = __syscall(SYS_mremap, old_addr, old_len, new_len,
	              MREMAP_MAYMOVE | MREMAP_FIXED, dst);
	if (r < 0)
		__syscall(SYS_munmap, dst, new_len);
	return (void *)__syscall_ret(r);
}

weak_alias(__mremap, mremap);
