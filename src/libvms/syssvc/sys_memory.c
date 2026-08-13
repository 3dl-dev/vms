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

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * Two shapes here, and the second is the sharper one: some of these services
 * validate their arguments and then return SS$_NORMAL having done nothing at
 * all. A caller cannot tell that apart from success. Which ones, read the
 * lines below -- they say so individually rather than through a count here
 * that would drift the moment one of them is fixed.
 *
 * ALL NINE CITED vms-5b4 UNTIL vms-fab. That is the item that BUILT this
 * register; it closed when the register landed and owned no facade in it, so
 * these nine were parked against finished work. vms-e0a owns them now: global
 * sections that no two processes can name, and three working-set services that
 * report success for work OVMX does not do.
 *
 * OVMX-USERSPACE: sys$expreg (vms-e0a) -- mmap() in the calling process's
 *     address space; no executive page table or process working-set list.
 * OVMX-USERSPACE: sys$cretva (vms-e0a) -- mmap() at the requested address in
 *     the calling process only.
 * OVMX-USERSPACE: sys$deltva (vms-e0a) -- munmap() in the calling process only.
 * OVMX-USERSPACE: sys$deltva_64 (vms-e0a) -- munmap() in the calling process
 *     only; the region_id_64 argument is discarded.
 * OVMX-USERSPACE: sys$crmpsc (vms-e0a) -- MAP_SHARED mmap of an fd taken from
 *     the caller's own pcb->channels[]; the global section NAME (gsdnam) is
 *     discarded, so no two processes can name the same section.
 * OVMX-USERSPACE: sys$dgblsc (vms-e0a) -- validates gsdnam and returns
 *     SS$_NORMAL; no global section database exists to delete from.
 * OVMX-USERSPACE: sys$purgws (vms-e0a) -- validates the range and returns
 *     SS$_NORMAL; nothing is purged and no working set is consulted.
 * OVMX-USERSPACE: sys$lkwset (vms-e0a) -- validates the range, echoes it back
 *     in retadr and returns SS$_NORMAL; no pages are locked.
 * OVMX-USERSPACE: sys$ulwset (vms-e0a) -- the same, for unlocking.
 */

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include "starlet.h"

/* VMS page size is 512 bytes (Alpha/Itanium may use larger, but 512 is the classic) */
#define VMS_PAGE_SIZE 512

/*
 * sys$cretva maps at a requested address *non-destructively* -- it must never
 * clobber an existing mapping (the fallback re-maps at any address instead).
 * Linux expresses that atomically with MAP_FIXED_NOREPLACE (4.17+). NetBSD and
 * other substrates have no such flag; there, pass the requested address as a
 * plain hint (no MAP_FIXED), which the kernel honors when the range is free and
 * otherwise places elsewhere -- the same non-destructive "try the requested
 * range, else any address" contract, with the actual placement reported back in
 * retadr[]. Keyed on the platform macro's own presence (never a transcribed
 * number), so it is correct by construction on every substrate.
 */
#if defined(MAP_FIXED_NOREPLACE)
#  define VMS_CRETVA_FIXED_FLAG  MAP_FIXED_NOREPLACE
#else
#  define VMS_CRETVA_FIXED_FLAG  0
#endif

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
                      MAP_PRIVATE | MAP_ANONYMOUS | VMS_CRETVA_FIXED_FLAG,
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

/*
 * sys$deltva_64 - Delete virtual address space (64-bit region API).
 *
 * See the doc comment in starlet.h: region_id_64 is accepted for
 * call-site compatibility but not validated against a region registry
 * (OVMX doesn't implement sys$create_region_64 yet).
 */
uint32_t sys$deltva_64(const GENERIC_64 *region_id_64, void *inadr_64,
                       uint64_t bytlen_64, uint32_t acmode,
                       void **retadr_64, uint64_t *retlen_64) {
    (void)region_id_64; (void)acmode;

    if (!inadr_64 || bytlen_64 == 0) return SS$_BADPARAM;

    munmap(inadr_64, (size_t)bytlen_64);

    if (retadr_64) *retadr_64 = inadr_64;
    if (retlen_64) *retlen_64 = bytlen_64;

    return SS$_NORMAL;
}

/*
 * sys$dgblsc - Delete global section.
 *
 * See the doc comment in starlet.h: OVMX has no cross-process named
 * global-section registry, so this validates its arguments and returns
 * success (the "mark for deletion, actual deletion happens once all
 * mapping processes delete their VA ranges" semantic is a no-op here
 * because there is no registry entry to mark).
 */
uint32_t sys$dgblsc(uint32_t flags, const struct dsc$descriptor_s *gsdnam,
                    void *ident) {
    (void)flags; (void)ident;

    if (!gsdnam || !gsdnam->dsc$a_pointer) return SS$_BADPARAM;

    return SS$_NORMAL;
}

/*
 * sys$lkwset - Lock pages into working set.
 *
 * See the doc comment in starlet.h: OVMX is demand-paged by the Linux
 * VMM (no adjustable working set to lock pages into), so this validates
 * the range, echoes it to retadr, and returns success.
 */
uint32_t sys$lkwset(const void *inadr, void *retadr, uint32_t acmode) {
    (void)acmode;
    if (!inadr) return SS$_BADPARAM;

    const void **in = (const void **)inadr;
    uintptr_t start = (uintptr_t)in[0];
    uintptr_t end = (uintptr_t)in[1];
    if (end < start) return SS$_BADPARAM;

    if (retadr) {
        void **ret = (void **)retadr;
        ret[0] = (void *)start;
        ret[1] = (void *)end;
    }

    return SS$_NORMAL;
}

/*
 * sys$ulwset - Unlock pages from working set.
 *
 * Companion no-op to sys$lkwset above.
 */
uint32_t sys$ulwset(const void *inadr, void *retadr, uint32_t acmode) {
    (void)acmode;
    if (!inadr) return SS$_BADPARAM;

    const void **in = (const void **)inadr;
    uintptr_t start = (uintptr_t)in[0];
    uintptr_t end = (uintptr_t)in[1];
    if (end < start) return SS$_BADPARAM;

    if (retadr) {
        void **ret = (void **)retadr;
        ret[0] = (void *)start;
        ret[1] = (void *)end;
    }

    return SS$_NORMAL;
}
