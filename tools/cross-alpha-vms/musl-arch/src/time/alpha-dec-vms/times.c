/*
 * times.c - times() for the OVMX alpha-dec-vms musl port (vms-5303e).
 *
 * Stock musl hands the caller's struct tms straight to the kernel:
 *     return __syscall(SYS_times, tms);
 * The Linux-Alpha kernel writes four 64-bit clock_t members (32 bytes). On this
 * LLP64 port `long` -- and so clock_t -- is 32 bits, so the caller's struct tms
 * is 16 bytes and the kernel's store runs 16 bytes past it. binutils'
 * libiberty get_run_time() (gas main's first call) keeps its struct tms at the
 * top of its frame, and the overrun zeroed the PV and RA slots of main's frame
 * just above: main returned from get_run_time() with pv = 0 and faulted on the
 * next linkage load (fatal signal 11 at as.exe main, gas on OVMX).
 *
 * The kernel gets a buffer of its own width; the 64-bit tick counts are
 * narrowed into the caller's 32-bit members, as the DEC C RTL's clock_t is.
 */
#include <sys/times.h>
#include "syscall.h"

clock_t times(struct tms *tms)
{
	long long k[4];
	long long r = __syscall(SYS_times, k);

	if (tms && r >= 0) {
		tms->tms_utime  = (clock_t)k[0];
		tms->tms_stime  = (clock_t)k[1];
		tms->tms_cutime = (clock_t)k[2];
		tms->tms_cstime = (clock_t)k[3];
	}
	return (clock_t)r;
}
