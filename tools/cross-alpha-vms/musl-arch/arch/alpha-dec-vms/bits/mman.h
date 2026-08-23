/*
 * mman.h - Alpha mmap flag overrides.
 * OVMX alpha-dec-vms musl port (vms-960).
 *
 * The base include/sys/mman.h carries the generic MAP_* values; Alpha diverges
 * (MAP_FIXED=0x100, MAP_ANON=0x10, MAP_NORESERVE=0x10000, ...) per the public
 * Alpha/Linux ABI (Linux arch/alpha uapi/asm/mman.h). Overridden here.
 *
 * RUNG-1 note: mmap is stubbed (-ENOSYS), so these values reach no kernel yet;
 * documented placeholder aligned to the OVMX/Linux-Alpha runtime lane, to be
 * reconciled with the OVMX Alpha executive at GAP3.
 */
#undef MAP_FIXED
#define MAP_FIXED      0x100
#undef MAP_ANON
#define MAP_ANON       0x10
#undef MAP_NORESERVE
#define MAP_NORESERVE  0x10000
#undef MAP_GROWSDOWN
#define MAP_GROWSDOWN  0x01000
#undef MAP_DENYWRITE
#define MAP_DENYWRITE  0x02000
#undef MAP_EXECUTABLE
#define MAP_EXECUTABLE 0x04000
#undef MAP_LOCKED
#define MAP_LOCKED     0x08000
#undef MAP_POPULATE
#define MAP_POPULATE   0x20000
#undef MAP_NONBLOCK
#define MAP_NONBLOCK   0x40000
#undef MAP_STACK
#define MAP_STACK      0x80000
#undef MAP_HUGETLB
#define MAP_HUGETLB    0x100000
#undef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x200000
#undef MAP_SYNC
