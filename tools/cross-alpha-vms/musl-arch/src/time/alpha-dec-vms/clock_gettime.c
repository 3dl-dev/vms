/*
 * clock_gettime.c - the C RTL's clock is the VMS system time (vms-593).
 *
 * musl 1.2.5's src/time/clock_gettime.c, unchanged except for the hook below.
 * On OpenVMS the DEC C RTL's time(), gettimeofday() and clock_gettime(
 * CLOCK_REALTIME) report the system time, which a program never reads from
 * anywhere but the executive ($GETTIM / $GETUTC). Every musl time-of-day reader
 * funnels through __clock_gettime, so this one hook serves them all: when the
 * C RTL time layer is on (src/vmsrms/crtl_time.c, installed by decc$main for a
 * DECC$SHR image, or by a static OVMX image's start-up), the realtime clock is
 * $GETUTC's, never the substrate kernel's clock_gettime / vDSO.
 *
 * __ovmx_clock_hook returns 0 or a negative errno. Before the layer is on (the
 * bootstrap DECC$SHR, which links no system service) the stock path runs. The
 * same file is the x86_64 static rig's overlay (src/crtl-musl-x86_64).
 */
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include "syscall.h"
#include "atomic.h"

#ifdef VDSO_CGT_SYM

static void *volatile vdso_func;

#ifdef VDSO_CGT32_SYM
static void *volatile vdso_func_32;
static int cgt_time32_wrap(clockid_t clk, struct timespec *ts)
{
	long ts32[2];
	int (*f)(clockid_t, long[2]) =
		(int (*)(clockid_t, long[2]))vdso_func_32;
	int r = f(clk, ts32);
	if (!r) {
		/* Fallback to syscalls if time32 overflowed. Maybe
		 * we lucked out and somehow migrated to a kernel with
		 * time64 syscalls available. */
		if (ts32[0] < 0) {
			a_cas_p(&vdso_func, (void *)cgt_time32_wrap, 0);
			return -ENOSYS;
		}
		ts->tv_sec = ts32[0];
		ts->tv_nsec = ts32[1];
	}
	return r;
}
#endif

static int cgt_init(clockid_t clk, struct timespec *ts)
{
	void *p = __vdsosym(VDSO_CGT_VER, VDSO_CGT_SYM);
#ifdef VDSO_CGT32_SYM
	if (!p) {
		void *q = __vdsosym(VDSO_CGT32_VER, VDSO_CGT32_SYM);
		if (q) {
			a_cas_p(&vdso_func_32, 0, q);
			p = cgt_time32_wrap;
		}
	}
#ifdef VDSO_CGT_WORKAROUND
	if (!__vdsosym(VDSO_CGT32_VER, VDSO_CGT32_SYM)) p = 0;
#endif
#endif
	int (*f)(clockid_t, struct timespec *) =
		(int (*)(clockid_t, struct timespec *))p;
	a_cas_p(&vdso_func, (void *)cgt_init, p);
	return f ? f(clk, ts) : -ENOSYS;
}

static void *volatile vdso_func = (void *)cgt_init;

#endif

int (*__ovmx_clock_hook)(clockid_t, struct timespec *) = 0;

int __clock_gettime(clockid_t clk, struct timespec *ts)
{
	int r;

	if (__ovmx_clock_hook && (clk == CLOCK_REALTIME || clk == CLOCK_REALTIME_COARSE)) {
		r = __ovmx_clock_hook(clk, ts);
		if (r) {
			errno = -r;
			return -1;
		}
		return 0;
	}

#ifdef VDSO_CGT_SYM
	int (*f)(clockid_t, struct timespec *) =
		(int (*)(clockid_t, struct timespec *))vdso_func;
	if (f) {
		r = f(clk, ts);
		if (!r) return r;
		if (r == -EINVAL) return __syscall_ret(r);
		/* Fall through on errors other than EINVAL. Some buggy
		 * vdso implementations return ENOSYS for clocks they
		 * can't handle, rather than making the syscall. This
		 * also handles the case where cgt_init fails to find
		 * a vdso function to use. */
	}
#endif

#ifdef SYS_clock_gettime64
	r = -ENOSYS;
	if (sizeof(time_t) > 4)
		r = __syscall(SYS_clock_gettime64, clk, ts);
	if (SYS_clock_gettime == SYS_clock_gettime64 || r!=-ENOSYS)
		return __syscall_ret(r);
	long ts32[2];
	r = __syscall(SYS_clock_gettime, clk, ts32);
#ifdef SYS_gettimeofday
	if (r==-ENOSYS && clk==CLOCK_REALTIME) {
		r = __syscall(SYS_gettimeofday, ts32, 0);
		ts32[1] *= 1000;
	}
#endif
	if (!r) {
		ts->tv_sec = ts32[0];
		ts->tv_nsec = ts32[1];
		return r;
	}
	return __syscall_ret(r);
#else
	r = __syscall(SYS_clock_gettime, clk, ts);
#ifdef SYS_gettimeofday
	if (r == -ENOSYS) {
		if (clk == CLOCK_REALTIME) {
			__syscall(SYS_gettimeofday, ts, 0);
			ts->tv_nsec = (int)ts->tv_nsec * 1000;
			return 0;
		}
		r = -EINVAL;
	}
#endif
	return __syscall_ret(r);
#endif
}

weak_alias(__clock_gettime, clock_gettime);
