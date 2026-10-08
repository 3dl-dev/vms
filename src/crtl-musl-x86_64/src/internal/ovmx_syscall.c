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
 * Not covered: musl's cancellable-syscall assembly (src/thread/x86_64/
 * syscall_cp.s), used only when a program links pthread_cancel; a cancellation
 * point there (read/write/open/close) traps directly.
 */
#include <fcntl.h>
#include "syscall.h"

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
