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

static long long sys_map(unsigned long long addr, unsigned long long len, int prot,
                         int flags, int fd, long long off)
{
#ifdef SYS_mmap2
	return __syscall(SYS_mmap2, addr, len, prot, flags, fd, off/UNIT);
#else
	return __syscall(SYS_mmap, addr, len, prot, flags, fd, off);
#endif
}

/*
 * Lowest-address first fit, as P0 grows on VMS: the region is filled from the
 * bottom, so what is freed is reused low and the top of P0 stays one
 * contiguous range for a large request (rd vms-465). The kernel does the
 * search: on Linux/Alpha an mmap with a hint and no MAP_FIXED takes the
 * lowest free gap at or above the hint (arch_get_unmapped_area, bottom-up),
 * so a hint of the P0 floor IS address-ordered first fit. The old next-fit
 * cursor spread the mappings over all of P0: GCC's cc1plus on gimple-match-8
 * left 600 MB free in ~2000 holes of at most 9 MB and failed a 42 MB request.
 *
 * The gap is claimed with room for the 64 KB granule (a PROT_NONE
 * placeholder), the mapping is put at the aligned address inside it
 * (MAP_FIXED replaces the placeholder), and the slack is given back. A gap
 * the kernel finds above P0 means P0 has none: ENOMEM, as an expansion of a
 * full P0 region fails on VMS.
 */
hidden long long __ovmx_p0_map(unsigned long long len, int prot, int flags,
                               int fd, long long off)
{
	unsigned long long need = (len + OVMX_P0_STEP - 1) & -OVMX_P0_STEP;
	if (!need || need > OVMX_P0_CEILING - OVMX_P0_FLOOR)
		return -ENOMEM;
	unsigned long long span = need + OVMX_P0_STEP;
	long long g = sys_map(OVMX_P0_FLOOR, span, PROT_NONE,
	                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (g < 0)
		return g;
	unsigned long long gap = (unsigned long long)g;
	unsigned long long a = (gap + OVMX_P0_STEP - 1) & -OVMX_P0_STEP;
	if (gap < OVMX_P0_FLOOR || a + need > OVMX_P0_CEILING) {
		__syscall(SYS_munmap, gap, span);
		return -ENOMEM;
	}
	long long r = sys_map(a, len, prot, flags | MAP_FIXED, fd, off);
	if (r != (long long)a) {
		__syscall(SYS_munmap, gap, span);
		return r < 0 ? r : -ENOMEM;
	}
	/* give back the placeholder around the mapping */
	if (a > gap)
		__syscall(SYS_munmap, gap, a - gap);
	unsigned long long mend = (a + len + 8191) & -8192ULL;   /* Alpha page */
	if (gap + span > mend)
		__syscall(SYS_munmap, mend, gap + span - mend);
	return r;
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
