/*
 * mmap.c - musl __mmap for alpha-dec-vms with VMS P0 placement (vms-122).
 * Stock musl 1.2.5 src/mman/mmap.c plus: an address-less, non-fixed request is
 * placed in the P0 region (p0_region.h).  The raw result is held in a 64-bit
 * local (the vms-430 LLP64 return-leg width).
 */
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include "syscall.h"
#include "p0_region.h"

static void dummy(void) { }
weak_alias(dummy, __vm_wait);

#ifndef SYSCALL_MMAP2_UNIT   /* musl src/internal/syscall.h */
#define SYSCALL_MMAP2_UNIT 4096ULL
#endif
#define UNIT SYSCALL_MMAP2_UNIT
/* Stock musl spells the shift 8*sizeof(syscall_arg_t)-1; this port's syscall
 * arguments are 64-bit (arch/alpha-dec-vms syscall_arch.h), so it is 63. */
#define OFF_MASK ((-0x2000ULL << 63) | (UNIT-1))

/* Next-fit cursor.  Only a hint: two threads racing for the same candidate
 * are arbitrated by MAP_FIXED_NOREPLACE in the kernel, and the loser moves on. */
static volatile unsigned long long p0_cursor = OVMX_P0_FLOOR;

static long long sys_map(unsigned long long addr, unsigned long long len, int prot,
                         int flags, int fd, long long off)
{
#ifdef SYS_mmap2
	return __syscall(SYS_mmap2, addr, len, prot, flags, fd, off/UNIT);
#else
	return __syscall(SYS_mmap, addr, len, prot, flags, fd, off);
#endif
}

hidden long long __ovmx_p0_map(unsigned long long len, int prot, int flags,
                               int fd, long long off)
{
	unsigned long long need = (len + OVMX_P0_STEP - 1) & -OVMX_P0_STEP;
	if (!need || need > OVMX_P0_CEILING - OVMX_P0_FLOOR)
		return -ENOMEM;
	unsigned long long start = p0_cursor, a = start;
	int wrapped = 0;
	for (;;) {
		if (a + need > OVMX_P0_CEILING) {
			if (wrapped) return -ENOMEM;
			wrapped = 1;
			a = OVMX_P0_FLOOR;
		}
		if (wrapped && a >= start) return -ENOMEM;   /* the whole region was tried */
		long long r = sys_map(a, len, prot, flags | MAP_FIXED_NOREPLACE, fd, off);
		if (r == (long long)a) {
			p0_cursor = a + need;
			return r;
		}
		if (r >= 0) {
			/* A kernel without MAP_FIXED_NOREPLACE treats it as a hint and may
			 * map elsewhere: give that back and fail rather than hand out an
			 * address outside P0. */
			__syscall(SYS_munmap, r, len);
			return -ENOMEM;
		}
		if (r != -EEXIST) return r;      /* a genuine error (EACCES, ENODEV, ...) */
		a += OVMX_P0_STEP;
	}
}

void *__mmap(void *start, size_t len, int prot, int flags, int fd, off_t off)
{
	long long ret;
	if (off & OFF_MASK) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	if (len >= PTRDIFF_MAX) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	if (flags & MAP_FIXED) {
		__vm_wait();
	}
	if (!start && !(flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)))
		ret = __ovmx_p0_map(len, prot, flags, fd, off);
	else
		ret = sys_map((uintptr_t)start, len, prot, flags, fd, off);
	/* Fixup incorrect EPERM from kernel. */
	if (ret == -EPERM && !start && (flags&MAP_ANON) && !(flags&MAP_FIXED))
		ret = -ENOMEM;
	return (void *)__syscall_ret(ret);
}

weak_alias(__mmap, mmap);
