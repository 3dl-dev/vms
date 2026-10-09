#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

/*
 * OVMX (vms-003b): every __syscallN funnels through __ovmx_syscall
 * (src/internal/ovmx_syscall.c), which offers the call to the C RTL file layer
 * over RMS (src/vmsrms/crtl_rms_fd.c) when an image has installed it, and
 * otherwise traps to the kernel exactly as musl's inline syscall did. The
 * x86_64 counterpart of the alpha-dec-vms port's __vms_alpha_syscall.
 * Original: musl 1.2.4 arch/x86_64/syscall_arch.h (MIT).
 */
long __ovmx_syscall(long, long, long, long, long, long, long);

static __inline long __syscall0(long n)
{
	return __ovmx_syscall(n, 0, 0, 0, 0, 0, 0);
}

static __inline long __syscall1(long n, long a1)
{
	return __ovmx_syscall(n, a1, 0, 0, 0, 0, 0);
}

static __inline long __syscall2(long n, long a1, long a2)
{
	return __ovmx_syscall(n, a1, a2, 0, 0, 0, 0);
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
	return __ovmx_syscall(n, a1, a2, a3, 0, 0, 0);
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	return __ovmx_syscall(n, a1, a2, a3, a4, 0, 0);
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	return __ovmx_syscall(n, a1, a2, a3, a4, a5, 0);
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	return __ovmx_syscall(n, a1, a2, a3, a4, a5, a6);
}

#define VDSO_USEFUL
#define VDSO_CGT_SYM "__vdso_clock_gettime"
#define VDSO_CGT_VER "LINUX_2.6"
#define VDSO_GETCPU_SYM "__vdso_getcpu"
#define VDSO_GETCPU_VER "LINUX_2.6"

#define IPC_64 0
