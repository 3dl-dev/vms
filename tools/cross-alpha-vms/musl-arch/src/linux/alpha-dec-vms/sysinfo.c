/*
 * sysinfo.c - sysinfo() for the OVMX alpha-dec-vms musl port (rd vms-465).
 *
 * Stock musl hands the caller's struct sysinfo straight to the kernel:
 *     return syscall(SYS_sysinfo, info);
 * The Linux-Alpha kernel writes its own layout: every `long` member 64 bits.
 * On this LLP64 port `long` is 32 bits, so the members the caller reads sit at
 * other offsets than the kernel wrote them: totalram read the low half of the
 * second load average, mem_unit a stray word. sysconf(_SC_PHYS_PAGES) -- what
 * libiberty's physmem_total() and so GCC's garbage-collector heuristics read
 * -- came back as a few pages, and cc1 ran with --param ggc-min-expand=30
 * ggc-min-heapsize=4096 (the floor for a machine with no memory), collecting
 * after every few kilobytes of growth: the native GCC build on OVMX ran several
 * times slower than its memory allows.
 *
 * The kernel gets a buffer of its own width. The byte counts are narrowed into
 * the caller's 32-bit members with a mem_unit large enough that none
 * overflows, as the sysinfo(2) contract (counts in units of mem_unit bytes)
 * allows.
 */
#include <string.h>
#include <sys/sysinfo.h>
#include "syscall.h"

/* The Linux-Alpha struct sysinfo (include/uapi/linux/sysinfo.h, 64-bit long). */
struct ksysinfo {
	long long uptime;
	unsigned long long loads[3];
	unsigned long long totalram, freeram, sharedram, bufferram;
	unsigned long long totalswap, freeswap;
	unsigned short procs, pad;
	unsigned long long totalhigh, freehigh;
	unsigned int mem_unit;
	char _slack[64];	/* _f[] is empty on a 64-bit kernel; room to spare */
};

int __lsysinfo(struct sysinfo *info)
{
	struct ksysinfo k;
	unsigned long long kunit, unit, big;
	int i, r;

	memset(&k, 0, sizeof k);
	r = syscall(SYS_sysinfo, &k);
	if (r < 0 || !info)
		return r;
	kunit = k.mem_unit ? k.mem_unit : 1;
	big = k.totalram > k.totalswap ? k.totalram : k.totalswap;
	if (k.totalhigh > big)
		big = k.totalhigh;
	/* the smallest power-of-two unit (bytes) that fits the largest count in 32 bits */
	unit = kunit;
	while (big * kunit / unit > 0xFFFFFFFFull)
		unit <<= 1;
#define NARROW(x) ((unsigned long)((x) * kunit / unit))
	memset(info, 0, sizeof *info);
	info->uptime = (unsigned long)k.uptime;
	for (i = 0; i < 3; i++)
		info->loads[i] = (unsigned long)k.loads[i];
	info->totalram = NARROW(k.totalram);
	info->freeram = NARROW(k.freeram);
	info->sharedram = NARROW(k.sharedram);
	info->bufferram = NARROW(k.bufferram);
	info->totalswap = NARROW(k.totalswap);
	info->freeswap = NARROW(k.freeswap);
	info->procs = k.procs;
	info->totalhigh = NARROW(k.totalhigh);
	info->freehigh = NARROW(k.freehigh);
	info->mem_unit = (unsigned)unit;
	return r;
}

weak_alias(__lsysinfo, sysinfo);
