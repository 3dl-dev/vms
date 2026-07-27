/*
 * sys_memory.c - Memory Management System Services
 *
 * Implements VMS virtual memory management on top of Linux mmap/munmap.
 * VMS uses 512-byte pages; Linux uses larger pages (typically 4096),
 * so we translate VMS page counts to byte sizes for mmap.
 *
 * Address ranges are returned as [start, end] pairs in retadr,
 * following the VMS convention where retadr[0] is the first byte
 * and retadr[1] is the last byte of the region.
 */

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include "starlet.h"

/* VMS page size is 512 bytes (Alpha/Itanium may use larger, but 512 is the classic) */
#define VMS_PAGE_SIZE 512

/* Import from sys_assign.c for file-backed sections */
extern int vms$$chan_to_fd(uint16_t chan);

/*
 * sys$expreg - Expand virtual address region.
 *
 * Allocates pagcnt VMS pages (512 bytes each) of anonymous virtual
 * memory. Returns the address range in retadr[0..1].
 *
 * Parameters:
 *   pagcnt - Number of VMS pages to allocate
 *   retadr - Receives [start_addr, end_addr]
 *   acmode - Access mode (ignored)
 *   region - P0 or P1 region (ignored, Linux manages this)
 */
uint32_t sys$expreg(uint32_t pagcnt, void *retadr, uint32_t acmode,
                    uint32_t region) {
    (void)acmode; (void)region;

    if (!retadr || pagcnt == 0) return SS$_BADPARAM;

    size_t size = (size_t)pagcnt * VMS_PAGE_SIZE;
    void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) return SS$_INSFMEM;

    void **ret = (void **)retadr;
    ret[0] = addr;
    ret[1] = (char *)addr + size - 1;

    return SS$_NORMAL;
}

/*
 * sys$cretva - Create virtual address space at a specific range.
 *
 * Attempts to map memory at the address range specified in inadr[0..1].
 * If the exact range is unavailable, falls back to any available address.
 *
 * Parameters:
 *   inadr  - Requested [start_addr, end_addr]
 *   retadr - Actual [start_addr, end_addr] allocated
 *   acmode - Access mode (ignored)
 */
uint32_t sys$cretva(const void *inadr, void *retadr, uint32_t acmode) {
    (void)acmode;
    if (!inadr) return SS$_BADPARAM;

    const void **in = (const void **)inadr;
    uintptr_t start = (uintptr_t)in[0];
    uintptr_t end = (uintptr_t)in[1];
    if (end < start) return SS$_BADPARAM;

    size_t size = end - start + 1;

    /* Try to map at the requested address (non-destructive) */
    void *addr = mmap((void *)start, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                      -1, 0);
    if (addr == MAP_FAILED) {
        /* Fall back to any address */
        addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (addr == MAP_FAILED) return SS$_INSFMEM;
    }

    if (retadr) {
        void **ret = (void **)retadr;
        ret[0] = addr;
        ret[1] = (char *)addr + size - 1;
    }

    return SS$_NORMAL;
}

/*
 * sys$deltva - Delete virtual address space.
 *
 * Unmaps the memory region specified by inadr[0..1].
 */
uint32_t sys$deltva(const void *inadr, void *retadr, uint32_t acmode) {
    (void)acmode;
    if (!inadr) return SS$_BADPARAM;

    const void **in = (const void **)inadr;
    uintptr_t start = (uintptr_t)in[0];
    uintptr_t end = (uintptr_t)in[1];
    if (end < start) return SS$_BADPARAM;

    size_t size = end - start + 1;
    munmap((void *)start, size);

    if (retadr) {
        void **ret = (void **)retadr;
        ret[0] = (void *)start;
        ret[1] = (void *)end;
    }

    return SS$_NORMAL;
}

/*
 * sys$purgws - Purge working set.
 *
 * inadr points to a two-pointer [start, end] range (see VA_RANGE in
 * va_rangedef.h). OVMX processes are demand-paged by the Linux VMM
 * rather than a VMS-style adjustable working set, so there is no
 * resident-page list to trim; validate the range and return success
 * without further action, matching how sys$cretva/sys$deltva above
 * already treat page-residency-adjacent parameters as no-ops.
 */
uint32_t sys$purgws(const void *inadr) {
    if (!inadr) return SS$_BADPARAM;

    const void **in = (const void **)inadr;
    uintptr_t start = (uintptr_t)in[0];
    uintptr_t end = (uintptr_t)in[1];
    if (end < start) return SS$_BADPARAM;

    return SS$_NORMAL;
}

/*
 * sys$crmpsc - Create and Map a Section.
 *
 * Maps a file (identified by chan) into the process address space.
 * If chan is 0, creates an anonymous shared section (global section).
 * The gsdnam descriptor names the global section for sharing.
 *
 * Parameters:
 *   inadr  - Requested address range (or NULL for any address)
 *   retadr - Actual mapped address range
 *   acmode - Access mode (ignored)
 *   flags  - Section flags (ignored for now)
 *   gsdnam - Global section name (ignored for now)
 *   ident  - Section version identification (ignored)
 *   relpag - Relative page in section (ignored)
 *   chan   - I/O channel for file-backed section (0 = anonymous)
 *   pagcnt - Number of VMS pages to map
 *   vbn    - Virtual block number to start mapping from
 *   prot   - Protection mask (ignored)
 *   pfc    - Page fault cluster size (ignored)
 */
uint32_t sys$crmpsc(const void *inadr, void *retadr, uint32_t acmode,
                    uint32_t flags, const struct dsc$descriptor_s *gsdnam,
                    uint64_t *ident, uint32_t relpag, uint16_t chan,
                    uint32_t pagcnt, uint32_t vbn, uint32_t prot,
                    uint32_t pfc) {
    (void)acmode; (void)flags; (void)gsdnam; (void)ident;
    (void)relpag; (void)prot; (void)pfc; (void)inadr;

    size_t size = (size_t)pagcnt * VMS_PAGE_SIZE;
    if (size == 0) return SS$_BADPARAM;

    int fd = -1;
    int mmap_flags = MAP_SHARED;
    off_t offset = 0;

    if (chan != 0) {
        /* File-backed section */
        fd = vms$$chan_to_fd(chan);
        if (fd < 0) return SS$_IVCHAN;
        /* VBN is 1-based, each VMS block is 512 bytes */
        offset = (off_t)(vbn > 0 ? vbn - 1 : 0) * VMS_PAGE_SIZE;
    } else {
        /* Anonymous shared section */
        mmap_flags |= MAP_ANONYMOUS;
    }

    void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE, mmap_flags, fd, offset);
    if (addr == MAP_FAILED) return SS$_INSFMEM;

    if (retadr) {
        void **ret = (void **)retadr;
        ret[0] = addr;
        ret[1] = (char *)addr + size - 1;
    }

    return SS$_NORMAL;
}
