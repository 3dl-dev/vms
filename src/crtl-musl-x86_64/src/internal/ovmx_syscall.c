/*
 * ovmx_syscall.c - the system-call funnel of OVMX's x86_64 C RTL (vms-003b).
 *
 * On OpenVMS the C RTL is a client of RMS: fopen() is $CREATE/$OPEN on the
 * Files-11 volume. The alpha-dec-vms musl port gets that by funnelling every
 * system call through __vms_alpha_syscall, where src/vmsrms/crtl_rms_fd.c hooks
 * the file calls. This is the same funnel for x86_64 musl: every __syscallN
 * (arch/x86_64/syscall_arch.h, overlaid) comes here. With no hook installed the
 * call traps to the kernel unchanged. With the RMS file layer installed, the
 * call is offered to it first; it serves the calls that name an RMS file (or an
 * RMS file's descriptor) and sets *handled, and leaves the rest to the kernel.
 *
 * The file layer speaks the *at() system calls (openat, newfstatat, unlinkat,
 * ...), the only ones alpha has. x86_64 musl also issues the legacy path calls
 * (open, stat, lstat, access, unlink, rmdir, rename, mkdir, readlink, creat,
 * dup2); they are offered to the layer in their *at() form, and a call the layer
 * does not take goes to the kernel exactly as musl made it.
 *
 * P0 HEAP (vms-95b). DEC C's default pointer size is 32: on OpenVMS every address
 * malloc hands out lies in the program region P0 (0x00000000-0x3FFFFFFF), which
 * longword-pointer code -- LIB$INSQHI's 32-bit self-relative queue links, a
 * pointer stored in an int -- relies on. Linux places an address-less mmap far
 * above 4 GB. An image that asks for it (__ovmx_p0_heap = 1, the DEC C 32-bit
 * pointer model) gets every address-less, non-fixed mapping placed in P0 instead,
 * next-fit with MAP_FIXED_NOREPLACE so nothing already mapped is clobbered, and a
 * growing mremap kept there; P0 full is ENOMEM, as on VMS. The x86_64 counterpart
 * of the alpha port's src/mman/alpha-dec-vms/mmap.c.
 *
 * Not covered: musl's cancellable-syscall assembly (src/thread/x86_64/
 * syscall_cp.s), used only when a program links pthread_cancel; a cancellation
 * point there (read/write/open/close) traps directly.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include "syscall.h"

#include <sys/mman.h>
#include <errno.h>

#define OVMX_P0_FLOOR   0x00100000UL
#define OVMX_P0_CEILING 0x40000000UL
#define OVMX_P0_STEP    0x00010000UL
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

int __ovmx_p0_heap = 0;
static unsigned long p0_cursor = OVMX_P0_FLOOR;

long long __ovmx_syscall_raw(long long, long long, long long, long long,
			     long long, long long, long long);

/* Claim len bytes of P0 for an mmap(0, len, prot, flags, fd, off). */
static long p0_map(unsigned long len, long prot, long flags, long fd, long off)
{
	unsigned long need = (len + OVMX_P0_STEP - 1) & -OVMX_P0_STEP;
	unsigned long start = p0_cursor, a = start;
	int wrapped = 0;

	if (!need || need > OVMX_P0_CEILING - OVMX_P0_FLOOR)
		return -ENOMEM;
	for (;;) {
		long r;
		if (a + need > OVMX_P0_CEILING) {
			if (wrapped) return -ENOMEM;
			wrapped = 1;
			a = OVMX_P0_FLOOR;
		}
		if (wrapped && a >= start) return -ENOMEM;
		r = (long)__ovmx_syscall_raw(SYS_mmap, a, len, prot,
					     flags | MAP_FIXED_NOREPLACE, fd, off);
		if (r == (long)a) {
			p0_cursor = a + need;
			return r;
		}
		if (r >= 0) {           /* NOREPLACE taken as a hint: give it back */
			__ovmx_syscall_raw(SYS_munmap, r, len, 0, 0, 0, 0);
			return -ENOMEM;
		}
		if (r != -EEXIST) return r;
		a += OVMX_P0_STEP;
	}
}

/* mremap that may move: grow in place, else onto a P0 range claimed for it. */
static long p0_remap(long old, long old_len, long new_len, long flags)
{
	long r = (long)__ovmx_syscall_raw(SYS_mremap, old, old_len, new_len,
					  flags & ~MREMAP_MAYMOVE, 0, 0);
	long dst;
	if (r >= 0 || new_len <= old_len)
		return r;
	dst = p0_map((unsigned long)new_len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (dst < 0)
		return dst;
	return (long)__ovmx_syscall_raw(SYS_mremap, old, old_len, new_len,
					MREMAP_MAYMOVE | MREMAP_FIXED, dst, 0);
}

long long (*__ovmx_sys_hook)(long long, long long, long long, long long,
			     long long, long long, long long, int *) = 0;

long long __ovmx_syscall_raw(long long n, long long a1, long long a2, long long a3,
			     long long a4, long long a5, long long a6)
{
	unsigned long ret;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	register long r9 __asm__("r9") = a6;
	__asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2),
			      "d"(a3), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
	return (long)ret;
}

long __ovmx_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	if (__ovmx_p0_heap) {
		if (n == SYS_mmap && a1 == 0 && !(a4 & (MAP_FIXED | MAP_FIXED_NOREPLACE)))
			return p0_map((unsigned long)a2, a3, a4, a5, a6);
		if (n == SYS_mremap && (a4 & MREMAP_MAYMOVE) && !(a4 & MREMAP_FIXED))
			return p0_remap(a1, a2, a3, a4);
	}
	if (__ovmx_sys_hook) {
		long long t = n, b1 = a1, b2 = a2, b3 = a3, b4 = a4, b5 = a5, r;
		int handled = 0;

		switch (n) {
		case SYS_open:     t = SYS_openat; b1 = AT_FDCWD; b2 = a1; b3 = a2; b4 = a3; break;
		case SYS_creat:    t = SYS_openat; b1 = AT_FDCWD; b2 = a1;
				   b3 = O_CREAT | O_WRONLY | O_TRUNC; b4 = a2; break;
		case SYS_stat:     t = SYS_newfstatat; b1 = AT_FDCWD; b2 = a1; b3 = a2; b4 = 0; break;
		case SYS_lstat:    t = SYS_newfstatat; b1 = AT_FDCWD; b2 = a1; b3 = a2;
				   b4 = AT_SYMLINK_NOFOLLOW; break;
		case SYS_access:   t = SYS_faccessat; b1 = AT_FDCWD; b2 = a1; b3 = a2; b4 = 0; break;
		case SYS_unlink:   t = SYS_unlinkat; b1 = AT_FDCWD; b2 = a1; b3 = 0; break;
		case SYS_rmdir:    t = SYS_unlinkat; b1 = AT_FDCWD; b2 = a1; b3 = AT_REMOVEDIR; break;
		case SYS_rename:   t = SYS_renameat; b1 = AT_FDCWD; b2 = a1; b3 = AT_FDCWD; b4 = a2; break;
		case SYS_mkdir:    t = SYS_mkdirat; b1 = AT_FDCWD; b2 = a1; b3 = a2; break;
		case SYS_readlink: t = SYS_readlinkat; b1 = AT_FDCWD; b2 = a1; b3 = a2; b4 = a3; break;
		case SYS_dup2:
			if (a1 == a2) { t = SYS_fcntl; b2 = F_GETFD; }
			else { t = SYS_dup3; b3 = 0; }
			break;
		default:
			break;
		}
		r = __ovmx_sys_hook(t, b1, b2, b3, b4, b5, a6, &handled);
		if (handled) {
			if (n == SYS_dup2 && a1 == a2 && r >= 0)
				return a1;
			return (long)r;
		}
	}
	return (long)__ovmx_syscall_raw(n, a1, a2, a3, a4, a5, a6);
}
