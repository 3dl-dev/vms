/*
 * p0_region.h - VMS P0 placement for the OVMX alpha-dec-vms C RTL (vms-122).
 *
 * On OpenVMS Alpha a process's program region P0 spans 0x00000000-0x3FFFFFFF:
 * the main image is linked at 0x10000 (vms-035), shareable images are mapped
 * above it, and the default heap grows the same region ($EXPREG), so every
 * address malloc hands out is a valid 32-bit (sign-extended longword) pointer
 * -- what DEC C's default /POINTER_SIZE=32 code relies on.  The Linux-Alpha
 * substrate instead places an address-less mmap at TASK_UNMAPPED_BASE
 * (0x200_0000_0000), so the musl heap lived above 4 GB.
 *
 * Every mapping request that leaves the address to the system (start == 0,
 * no MAP_FIXED) is therefore placed in P0 instead: next-fit from a cursor,
 * each candidate claimed with MAP_FIXED_NOREPLACE so an occupied range (an
 * image, a shareable, another mapping) is refused by the kernel and skipped,
 * never clobbered.  When P0 has no room the request fails with ENOMEM, as an
 * expansion of a full P0 region fails on VMS -- a high address would be a
 * pointer a 32-bit caller silently truncates.
 */
#ifndef OVMX_P0_REGION_H
#define OVMX_P0_REGION_H

#define OVMX_P0_FLOOR   0x00100000ULL   /* first heap candidate, above the image base */
#define OVMX_P0_CEILING 0x40000000ULL   /* P1 begins here                             */
#define OVMX_P0_STEP    0x00010000ULL   /* 64 KB: the Alpha VMS allocation granule     */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x200000    /* Linux/Alpha                                 */
#endif

#ifndef hidden   /* musl's internal visibility macro (src/include/features.h) */
#define hidden __attribute__((__visibility__("hidden")))
#endif

/* Claim len bytes in P0 for a mapping with the given prot/flags/fd/off.
 * Returns the raw syscall result: an address, or a negative errno. */
hidden long long __ovmx_p0_map(unsigned long long len, int prot, int flags,
                               int fd, long long off);

#endif
