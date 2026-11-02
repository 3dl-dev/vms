/*
 * sys_memory.c - Memory Management System Services
 *
 * $EXPREG/$CRETVA/$DELTVA are the executive's (rd vms-bc53, VMS_IOCTL_VA):
 * it creates the pages and charges PGFLQUOTA. The section and working-set
 * services below are still the userspace forms their markers declare.
 * VMS counts in 512-byte pagelets; the executive rounds to its page size.
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
 * OVMX-PARTIAL: sys$expreg (vms-bc53) -- exec: the executive picks the range
 *     at the end of P0, maps the demand-zero pages and charges PGFLQUOTA
 *     (SS$_EXQUOTA/SS$_VASFULL); SS$_NOSUCHDEV with no executive.
 * OVMX-LOCAL: sys$expreg -- the pagelet count -> byte length conversion and the
 *     retadr pair are formatted in the calling process.
 * OVMX-PARTIAL: sys$cretva (vms-bc53) -- exec: the executive creates the pages
 *     over the range and charges them; SS$_PAGOWNVIO for pages it did not make.
 * OVMX-LOCAL: sys$cretva -- the inadr/retadr pairs are read and written in the
 *     calling process.
 * OVMX-PARTIAL: sys$deltva (vms-bc53) -- exec: the executive deletes the pages
 *     it created in the range and returns their charge.
 * OVMX-LOCAL: sys$deltva -- the inadr/retadr pairs are read and written in the
 *     calling process.
 * OVMX-PARTIAL: sys$deltva_64 (vms-bc53) -- exec: as sys$deltva.
 * OVMX-LOCAL: sys$deltva_64 -- the region_id_64 argument is not checked against
 *     a region registry (no $CREATE_REGION_64 yet).
 * OVMX-USERSPACE: sys$crmpsc (vms-e0a) -- MAP_SHARED mmap of an fd taken from
 *     the caller's own pcb->channels[]; the global section NAME (gsdnam) is
 *     discarded, so no two processes can name the same section.
 * OVMX-PARTIAL: sys$dgblsc (vms-d91) -- exec: vms_kif_gblsc_delete answers;
 *     SS$_NOSUCHDEV with no executive (one runtime, no userspace answer for
 *     system-wide state).
 * OVMX-LOCAL: sys$dgblsc -- the SS$_NOSUCHSEC answer itself is not looked up in
 *     the executive: no executive global-section registry exists yet and
 *     sys$crmpsc discards the section name, so no named global section can
 *     exist to delete.
 * OVMX-USERSPACE: sys$purgws (vms-e0a) -- validates the range and returns
 *     SS$_NORMAL; nothing is purged and no working set is consulted.
 * OVMX-USERSPACE: sys$lkwset (vms-e0a) -- mlock() of the page-rounded range in
 *     the calling process only (via sys$lckpag); no VMS working-set list or
 *     WSQUOTA exists, so the lock is the Linux VMM's, not an executive entry.
 * OVMX-USERSPACE: sys$ulwset (vms-e0a) -- munlock() of the same (via sys$ulkpag).
 * OVMX-USERSPACE: sys$lckpag (vms-44a) -- mlock() of the page-rounded range in
 *     the calling process only; the lock is the Linux VMM's, not an executive
 *     balance-set entry, so no other process or $GETJPI can see it.
 * OVMX-USERSPACE: sys$ulkpag (vms-44a) -- munlock() of the same.
 * OVMX-USERSPACE: sys$purge_ws (vms-e0a) -- the 64-bit-range form of $PURGWS;
 *     validates the range and returns SS$_NORMAL, nothing is purged.
 */

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "starlet.h"
#include "va_rangedef.h"
#include "vms_kif.h"

/* VMS page size is 512 bytes (Alpha/Itanium may use larger, but 512 is the classic) */
#define VMS_PAGE_SIZE 512

/* Import from sys_assign.c for file-backed sections */
extern int vms$$chan_to_fd(uint16_t chan);

/*
 * The process virtual address space services (rd vms-bc53). The executive
 * (src/kernel/vms_va.c, VMS_IOCTL_VA) creates and deletes the pages and
 * charges them to the process's PGFLQUOTA; these entry points only translate
 * the VMS argument forms. No executive -> SS$_NOSUCHDEV, never a userspace
 * mmap standing in for it (INV-6).
 *
 * pagcnt is in PAGELETS (512 bytes), as on Alpha; the executive rounds to its
 * own page size and retadr reports what was actually created.
 */
static uint32_t va_call(uint32_t op, uint32_t region, uint64_t base,
                        uint64_t length, uint64_t *rbase, uint64_t *rlen)
{
    struct vms_va_args a;
    uint32_t st;

    memset(&a, 0, sizeof(a));
    a.op = op;
    a.region = region;
    a.base = base;
    a.length = length;
    st = vms_kif_va(&a);
    if (st & 1) {
        if (rbase) *rbase = a.base;
        if (rlen)  *rlen = a.length;
    }
    return st;
}

static void va_retadr(void *retadr, uint64_t base, uint64_t len)
{
    void **ret = (void **)retadr;

    if (!retadr)
        return;
    if (len == 0) {             /* nothing affected: VMS returns -1 in both */
        ret[0] = (void *)(intptr_t)-1;
        ret[1] = (void *)(intptr_t)-1;
        return;
    }
    ret[0] = (void *)(uintptr_t)base;
    ret[1] = (void *)(uintptr_t)(base + len - 1);
}

/*
 * sys$expreg - Expand virtual address region.
 *
 *   pagcnt - pagelets to add to the end of the region
 *   retadr - receives [first byte, last byte] created
 *   acmode - access mode of the pages (the executive records no page owner
 *            mode yet; see vms_va.c)
 *   region - 0 = P0 (program region); 1 = P1 -> SS$_UNSUPPORTED
 */
uint32_t sys$expreg(uint32_t pagcnt, void *retadr, uint32_t acmode,
                    uint32_t region) {
    uint64_t b = 0, l = 0;
    uint32_t st;

    (void)acmode;
    if (pagcnt == 0) return SS$_BADPARAM;

    st = va_call(VMS_VA_EXPREG, region ? VMS_VA_REGION_P1 : VMS_VA_REGION_P0,
                 0, (uint64_t)pagcnt * VMS_PAGE_SIZE, &b, &l);
    if (st & 1)
        va_retadr(retadr, b, l);
    return st;
}

/*
 * sys$cretva - Create virtual address space over inadr[0..1].
 *
 * The executive creates demand-zero pages over the page-rounded range,
 * replacing pages it created earlier; pages it did not create (the image,
 * shareables, the substrate) are refused SS$_PAGOWNVIO. retadr gets the range
 * created.
 */
uint32_t sys$cretva(const void *inadr, void *retadr, uint32_t acmode) {
    uint64_t b = 0, l = 0;
    uint32_t st;

    (void)acmode;
    if (!inadr) return SS$_BADPARAM;

    const void **in = (const void **)inadr;
    uintptr_t start = (uintptr_t)in[0];
    uintptr_t end = (uintptr_t)in[1];
    if (end < start) return SS$_BADPARAM;

    st = va_call(VMS_VA_CRETVA, 0, start, (uint64_t)(end - start) + 1, &b, &l);
    if (st & 1)
        va_retadr(retadr, b, l);
    return st;
}

/*
 * sys$deltva - Delete virtual address space in inadr[0..1].
 *
 * The executive deletes the pages it created in the range and skips the
 * rest, as VMS skips pages that do not exist. retadr gets the range deleted
 * (-1, -1 when nothing was).
 */
uint32_t sys$deltva(const void *inadr, void *retadr, uint32_t acmode) {
    uint64_t b = 0, l = 0;
    uint32_t st;

    (void)acmode;
    if (!inadr) return SS$_BADPARAM;

    const void **in = (const void **)inadr;
    uintptr_t start = (uintptr_t)in[0];
    uintptr_t end = (uintptr_t)in[1];
    if (end < start) return SS$_BADPARAM;

    st = va_call(VMS_VA_DELTVA, 0, start, (uint64_t)(end - start) + 1, &b, &l);
    if (st & 1)
        va_retadr(retadr, b, l);
    return st;
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
 * The executive deletes the pages it created in [inadr_64, inadr_64 +
 * bytlen_64), as sys$deltva. region_id_64 is accepted for call-site
 * compatibility but not validated against a region registry (OVMX does not
 * implement sys$create_region_64 yet).
 */
uint32_t sys$deltva_64(const GENERIC_64 *region_id_64, void *inadr_64,
                       uint64_t bytlen_64, uint32_t acmode,
                       void **retadr_64, uint64_t *retlen_64) {
    uint64_t b = 0, l = 0;
    uint32_t st;

    (void)region_id_64; (void)acmode;

    if (!inadr_64 || bytlen_64 == 0) return SS$_BADPARAM;

    st = va_call(VMS_VA_DELTVA, 0, (uintptr_t)inadr_64, bytlen_64, &b, &l);
    if (st & 1) {
        if (retadr_64) *retadr_64 = (void *)(uintptr_t)b;
        if (retlen_64) *retlen_64 = l;
    }
    return st;
}

/*
 * sys$dgblsc - Delete global section.
 *
 * See the doc comment in starlet.h: OVMX has no cross-process named
 * global-section registry (sys$crmpsc discards gsdnam), so no section by
 * that name can exist. Honest answer per the VMS System Services Reference:
 * SS$_NOSUCHSEC, "the specified global section does not exist" -- never a
 * success that deleted nothing (vms-d91).
 */
uint32_t sys$dgblsc(uint32_t flags, const struct dsc$descriptor_s *gsdnam,
                    void *ident) {
    (void)flags; (void)ident;

    if (!gsdnam || !gsdnam->dsc$a_pointer) return SS$_BADPARAM;

    /* Global sections are system-wide state: the executive answers (vms-d91). */
    char nm[256];
    uint32_t n = gsdnam->dsc$w_length < sizeof nm - 1 ? gsdnam->dsc$w_length : (uint32_t)sizeof nm - 1;
    memcpy(nm, gsdnam->dsc$a_pointer, n);
    nm[n] = 0;
    return vms_kif_gblsc_delete(nm);
}

/*
 * sys$lkwset / sys$ulwset - lock / unlock pages in the working set (vms-18c2).
 *
 * The Linux VMM has no VMS working-set list, so the nearest real lock is the
 * resident-page lock $LCKPAG already performs: the pages are mlock()ed (or
 * munlock()ed) for real, the page-rounded range comes back in retadr, a host
 * refusal is SS$_NOPRIV and an unmapped range SS$_ACCVIO. Not a validated
 * no-op any more. (SS$_LKWSETFUL is still never returned: no WSQUOTA.)
 */
uint32_t sys$lkwset(const void *inadr, void *retadr, uint32_t acmode) {
    return sys$lckpag(inadr, retadr, acmode);
}

uint32_t sys$ulwset(const void *inadr, void *retadr, uint32_t acmode) {
    return sys$ulkpag(inadr, retadr, acmode);
}

/*
 * Page-rounded range helper for $LCKPAG/$ULKPAG: VMS locks whole pages, and the
 * range it reports back is the page-rounded one actually locked.
 */
static uint32_t pag_range(const VA_RANGE *in, VA_RANGE *out, size_t *len)
{
    uintptr_t pg = (uintptr_t)sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)in->va_range$ps_start_va;
    uintptr_t end   = (uintptr_t)in->va_range$ps_end_va;
    if (end < start)
        return SS$_BADPARAM;
    uintptr_t lo = start & ~(pg - 1);
    uintptr_t hi = end | (pg - 1);
    out->va_range$ps_start_va = (void *)lo;
    out->va_range$ps_end_va   = (void *)hi;
    *len = (size_t)(hi - lo + 1);
    return SS$_NORMAL;
}

/*
 * sys$lckpag - Lock pages in memory.
 *
 * Locks the pages containing the inadr range into physical memory (mlock) and
 * returns the page-rounded range actually locked in retadr.  A refusal by the
 * host (no CAP_IPC_LOCK / RLIMIT_MEMLOCK) is SS$_NOPRIV -- the VMS PSWAPM
 * requirement -- and an unmapped range is SS$_ACCVIO; never a faked lock.
 */
uint32_t sys$lckpag(const void *inadr, void *retadr, uint32_t acmode)
{
    (void)acmode;
    if (!inadr)
        return SS$_BADPARAM;
    VA_RANGE got;
    size_t len;
    uint32_t st = pag_range((const VA_RANGE *)inadr, &got, &len);
    if (!(st & 1))
        return st;
#if defined(__linux__) || defined(__NetBSD__)
    if (mlock(got.va_range$ps_start_va, len) < 0) {
        if (errno == ENOMEM) return SS$_ACCVIO;
        return SS$_NOPRIV;
    }
#else
    (void)len;
    return SS$_NOSUCHDEV;       /* no page-lock entry point on this substrate */
#endif
    if (retadr)
        *(VA_RANGE *)retadr = got;
    return SS$_NORMAL;
}

/* sys$ulkpag - Unlock pages locked by $LCKPAG (munlock). */
uint32_t sys$ulkpag(const void *inadr, void *retadr, uint32_t acmode)
{
    (void)acmode;
    if (!inadr)
        return SS$_BADPARAM;
    VA_RANGE got;
    size_t len;
    uint32_t st = pag_range((const VA_RANGE *)inadr, &got, &len);
    if (!(st & 1))
        return st;
#if defined(__linux__) || defined(__NetBSD__)
    if (munlock(got.va_range$ps_start_va, len) < 0)
        return (errno == ENOMEM) ? SS$_ACCVIO : SS$_NOPRIV;
#else
    (void)len;
    return SS$_NOSUCHDEV;
#endif
    if (retadr)
        *(VA_RANGE *)retadr = got;
    return SS$_NORMAL;
}

/*
 * sys$purge_ws - Purge working set (64-bit range form).  See sys$purgws: no
 * adjustable working set exists under the Linux VMM; the range is validated and
 * nothing is purged.  The second argument is the 64-bit byte count of the range.
 */
uint32_t sys$purge_ws(const void *inadr, uint64_t count)
{
    (void)count;
    return sys$purgws(inadr);
}
