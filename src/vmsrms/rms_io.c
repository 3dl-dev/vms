/*
 * rms_io.c - RMS block-I/O substrate (vms-bc7, epic vms-208).
 *
 * Implements the positioned-I/O vocabulary rms_io.h declares -- the POSIX-fd
 * cursor semantics the seq/rel/idx record engines depend on -- over TWO
 * backends behind one interface, so the record engines are substrate-agnostic:
 *
 *   __linux__  : the Files-11 (ODS-2) ACP. A record at {byte-offset, length}
 *                becomes {VBN, byte-offset, length} IO$_READVBLK/IO$_WRITEVBLK
 *                on an ACP file-class channel window, ridden over /dev/vms via
 *                vms_kif_acp_readvb / vms_kif_acp_writevb / vms_kif_acp_fileop.
 *                Fail-honest, NO POSIX fallback (CLAUDE.md Rule 9 / INV-6).
 *
 *   otherwise  : ordinary POSIX (pread/pwrite/ftruncate on a bare fd held in
 *                the handle). This is the netbsd-vax standalone cross's path
 *                (src/vmsrms/CMakeLists.txt: "RMS file I/O is ordinary POSIX");
 *                VAX keeps it until its own ACP re-target (vms-d5d). The FAB no
 *                longer carries _linux_fd on any platform -- the fd now lives
 *                inside the rms_file handle.
 *
 * ACP byte/VBN model (src/kernel-core/vmsfs_acp.c, src/kernel/vms_acp.h):
 *   - A file's VALID byte length is (efblk-1)*512 + ffbyte; the handle mirrors
 *     it in f->eof, seeded at ACCESS/CREATE and grown on every write.
 *   - IO$_READVBLK clamps at valid bytes (a read at/after EOF is SS$_ENDOFFILE
 *     with 0 transferred -> POSIX read() returning 0 at EOF).
 *   - IO$_WRITEVBLK past EOF implicitly extends (BITMAP.SYS alloc + FH2 grow)
 *     and sets valid = end-of-write -> POSIX write() growing the file.
 * A byte cursor C maps to vbn = C/512 + 1, offset = C%512; the ACP handles a
 * length spanning block boundaries in one call (up to ACP_RW_MAX_XFER = 1 MiB,
 * so a larger transfer loops here -- record I/O never approaches it).
 */

#include <string.h>

#include "rms_io.h"

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#define RMS_IO_BLK       512u

/* ==========================================================================
 * lseek / fsync are backend-independent (cursor arithmetic; write-through).
 * ========================================================================== */

off_t rms_io_lseek(rms_file_t *f, off_t offset, int whence)
{
    uint64_t base;

    if (!f)
        return (off_t)-1;

    switch (whence) {
        case SEEK_SET: base = 0;         break;
        case SEEK_CUR: base = f->cursor; break;
        case SEEK_END: base = f->eof;    break;
        default:       return (off_t)-1;
    }

    if (offset < 0 && (uint64_t)(-offset) > base)
        return (off_t)-1;                       /* negative position rejected */

    f->cursor = base + (uint64_t)offset;
    return (off_t)f->cursor;
}

#if defined(__linux__)
/* ==========================================================================
 * Backend A: Files-11 ODS-2 ACP (the product runtime, Rule 9).
 * ========================================================================== */

#include "vms_kif.h"     /* pulls ../kernel/vms_ioctl.h -> vms_acp.h structs   */
#include "ssdef.h"

#define RMS_IO_MAX_XFER  (1u << 20)   /* mirror ACP_RW_MAX_XFER (1 MiB) */

ssize_t rms_io_read(rms_file_t *f, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    size_t done = 0;

    if (!f || !f->accessed)
        return -1;
    if (n == 0)
        return 0;

    while (done < n) {
        struct vms_acp_rw_args a;
        uint32_t chunk = (n - done) > RMS_IO_MAX_XFER
                             ? RMS_IO_MAX_XFER : (uint32_t)(n - done);
        uint32_t st;

        memset(&a, 0, sizeof(a));
        a.chan   = f->chan;
        a.vbn    = (uint32_t)(f->cursor / RMS_IO_BLK) + 1u;
        a.offset = (uint32_t)(f->cursor % RMS_IO_BLK);
        a.length = chunk;
        a.buffer = (uint64_t)(uintptr_t)(p + done);

        st = vms_kif_acp_readvb(&a);
        if (st == SS$_ENDOFFILE) {
            f->eof = (uint64_t)(a.new_efblk ? (a.new_efblk - 1u) : 0) * RMS_IO_BLK;
            break;                       /* at/after EOF: return what we have */
        }
        if (!$VMS_STATUS_SUCCESS(st))
            return -1;
        if (a.xferred == 0)
            break;

        f->cursor += a.xferred;
        done      += a.xferred;
        if (a.new_hiblk)
            f->hiblk = a.new_hiblk;
        if (a.xferred < chunk)
            break;                       /* short read == hit EOF this chunk */
    }

    return (ssize_t)done;
}

ssize_t rms_io_write(rms_file_t *f, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t done = 0;

    if (!f || !f->accessed || !f->writable)
        return -1;
    if (n == 0)
        return 0;

    while (done < n) {
        struct vms_acp_rw_args a;
        uint32_t chunk = (n - done) > RMS_IO_MAX_XFER
                             ? RMS_IO_MAX_XFER : (uint32_t)(n - done);
        uint32_t st;

        memset(&a, 0, sizeof(a));
        a.chan   = f->chan;
        a.vbn    = (uint32_t)(f->cursor / RMS_IO_BLK) + 1u;
        a.offset = (uint32_t)(f->cursor % RMS_IO_BLK);
        a.length = chunk;
        a.buffer = (uint64_t)(uintptr_t)(p + done);

        st = vms_kif_acp_writevb(&a);
        if (!$VMS_STATUS_SUCCESS(st))
            return done ? (ssize_t)done : -1;

        f->cursor += a.xferred;
        done      += a.xferred;
        f->hiblk   = a.new_hiblk;
        if (f->cursor > f->eof)
            f->eof = f->cursor;          /* ACP set valid = end-of-write */
        if (a.xferred < chunk)
            return done ? (ssize_t)done : -1;
    }

    return (ssize_t)done;
}

int rms_io_ftruncate(rms_file_t *f, off_t length)
{
    if (!f || !f->accessed || length < 0)
        return -1;
    if ((uint64_t)length == f->eof)
        return 0;

    if ((uint64_t)length > f->eof) {
        /* Grow: a single zero byte at the last position; IO$_WRITEVBLK's
         * implicit extend allocates + zero-fills the shortfall and sets valid
         * to the new length -- ftruncate() growth, ACP-only (relative-file
         * cell pre-allocation). */
        uint64_t save = f->cursor;
        uint8_t zero = 0;
        ssize_t r;
        f->cursor = (uint64_t)length - 1u;
        r = rms_io_write(f, &zero, 1);
        f->cursor = save;
        return (r == 1) ? 0 : -1;
    }

    /* Shrink: IO$_MODIFY truncate to the new EOF VBN/first-free-byte, by FID. */
    {
        struct vms_acp_fileop_args a;
        uint32_t st;
        uint32_t efblk = (uint32_t)(((uint64_t)length + RMS_IO_BLK - 1) / RMS_IO_BLK);
        uint16_t ffbyte = (uint16_t)((uint64_t)length -
                          (uint64_t)(efblk ? (efblk - 1u) : 0) * RMS_IO_BLK);

        memset(&a, 0, sizeof(a));
        a.chan         = f->chan;
        a.func         = VMS_ACP_FOP_MODIFY;
        a.fidmode      = 1;
        a.fid_num      = f->fid_num;
        a.fid_seq      = f->fid_seq;
        a.fid_rvn      = f->fid_rvn;
        a.fid_nmx      = f->fid_nmx;
        a.trunc_efblk  = efblk ? efblk : 1u;
        a.trunc_ffbyte = ffbyte;

        st = vms_kif_acp_fileop(&a);
        if (!$VMS_STATUS_SUCCESS(st))
            return -1;
        f->eof   = (uint64_t)length;
        f->hiblk = a.new_hiblk;
        return 0;
    }
}

int rms_io_fsync(rms_file_t *f)
{
    /* IO$_WRITEVBLK is write-through (the ACP writes each block synchronously),
     * so there is nothing buffered to flush. */
    (void)f;
    return 0;
}

#else
/* ==========================================================================
 * Backend B: POSIX (netbsd-vax standalone cross; VAX re-targets under vms-d5d).
 * The fd lives in the handle; positioned I/O uses the cursor so the engines'
 * lseek+read/write idiom maps to pread/pwrite without an implicit kernel fd
 * position (which multiple RABs on one FAB would otherwise race).
 * ========================================================================== */

#include <unistd.h>

ssize_t rms_io_read(rms_file_t *f, void *buf, size_t n)
{
    ssize_t r;
    if (!f || f->fd < 0)
        return -1;
    r = pread(f->fd, buf, n, (off_t)f->cursor);
    if (r > 0)
        f->cursor += (uint64_t)r;
    return r;
}

ssize_t rms_io_write(rms_file_t *f, const void *buf, size_t n)
{
    ssize_t r;
    if (!f || f->fd < 0)
        return -1;
    r = pwrite(f->fd, buf, n, (off_t)f->cursor);
    if (r > 0) {
        f->cursor += (uint64_t)r;
        if (f->cursor > f->eof)
            f->eof = f->cursor;
    }
    return r;
}

int rms_io_ftruncate(rms_file_t *f, off_t length)
{
    if (!f || f->fd < 0 || length < 0)
        return -1;
    if (ftruncate(f->fd, length) < 0)
        return -1;
    f->eof = (uint64_t)length;
    return 0;
}

int rms_io_fsync(rms_file_t *f)
{
    if (!f || f->fd < 0)
        return -1;
    return fsync(f->fd);
}

#include <stdlib.h>

rms_file_t *rms_io_posix_wrap(int fd)
{
    rms_file_t *f = (rms_file_t *)calloc(1, sizeof(*f));
    off_t end;
    if (!f)
        return NULL;
    f->fd = fd;
    f->accessed = 1;
    f->writable = 1;
    end = lseek(fd, 0, SEEK_END);
    f->eof = (end > 0) ? (uint64_t)end : 0;
    f->cursor = 0;
    return f;
}

void rms_io_posix_unwrap(rms_file_t *f)
{
    if (!f)
        return;
    if (f->fd >= 0)
        close(f->fd);
    free(f);
}

int rms_io_posix_fd(rms_file_t *f)
{
    return f ? f->fd : -1;
}

#endif /* backend select */

/* ==========================================================================
 * *_exact wrappers (backend-independent -- built on rms_io_read/write above).
 * ========================================================================== */

ssize_t rms_io_read_exact(rms_file_t *f, void *buf, size_t count)
{
    size_t total = 0;
    uint8_t *p = (uint8_t *)buf;

    while (total < count) {
        ssize_t n = rms_io_read(f, p + total, count - total);
        if (n < 0)
            return -1;
        if (n == 0)
            break;                       /* EOF */
        total += (size_t)n;
    }
    return (ssize_t)total;
}

int rms_io_write_exact(rms_file_t *f, const void *buf, size_t count)
{
    size_t total = 0;
    const uint8_t *p = (const uint8_t *)buf;

    while (total < count) {
        ssize_t n = rms_io_write(f, p + total, count - total);
        if (n <= 0)
            return -1;
        total += (size_t)n;
    }
    return 0;
}
