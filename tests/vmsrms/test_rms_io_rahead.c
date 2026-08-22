/*
 * test_rms_io_rahead (vms-0f5): the RMS ACP read-ahead is a VMS multiblock
 * window, not one block.
 *
 * The 0.5 authenticity flip moved RMS record I/O from POSIX pread() to the
 * Files-11 ODS-2 ACP, so every refill is an IO$_READVBLK = a guest->executive
 * mode switch. Real OpenVMS RMS reads a MULTIBLOCK COUNT (RMS_DFMBC) of blocks
 * per I/O and serves records from the buffer; OVMX had regressed to a single
 * 512-byte block per QIO, which under emulation (the wasm demo runs pure TCG,
 * where a mode switch is dear) made a byte-at-a-time text/index scan crawl.
 *
 * This test compiles rms_io.c with OVMX_HAVE_ACP against a COUNTING mock of
 * vms_kif_acp_readvb, reads a file one byte at a time (the rms_seq.c STMLF /
 * SYSUAF-index access pattern), and proves BOTH:
 *   (1) every byte read back is correct across window and EOF boundaries, and
 *   (2) the QIO count is ceil(size / RMS_IO_RAHEAD_BYTES) -- the VMS multiblock
 *       rate -- and strictly fewer than the old per-512-byte rate.
 *
 * Host-only and hermetic: no /dev/vms, no libvmsrms link (rms_io.c is compiled
 * straight into this target so there is exactly one definition of the record
 * primitives and the ACP symbols bind to the mock below).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "rms_io.h"
#include "vms_kif.h"     /* struct vms_acp_rw_args / _fileop_args, SS$ via ssdef */
#include "ssdef.h"

/* ---- backing "volume": one in-memory file image, with a QIO counter ------- */

#define FILE_BYTES  20000u

static uint8_t  g_image[FILE_BYTES];
static unsigned g_readvb_qios;      /* IO$_READVBLK calls the mock served       */

static uint8_t pat(size_t i) { return (uint8_t)((i * 31u + 7u) & 0xffu); }

/* Mock IO$_READVBLK: serve from g_image, clamp at valid EOF exactly as the real
 * ACP does (a read starting at/after EOF is SS$_ENDOFFILE; one that starts
 * before EOF and spans past it returns the clamped byte count with success). */
uint32_t vms_kif_acp_readvb(struct vms_acp_rw_args *a)
{
    uint64_t off = (uint64_t)(a->vbn ? a->vbn - 1u : 0) * 512u + a->offset;
    uint32_t efblk = (FILE_BYTES + 511u) / 512u;

    g_readvb_qios++;

    if (off >= FILE_BYTES) {
        a->xferred   = 0;
        a->new_efblk = efblk;
        a->status    = SS$_ENDOFFILE;
        return SS$_ENDOFFILE;
    }
    uint32_t avail = (uint32_t)(FILE_BYTES - off);
    uint32_t xfer  = a->length < avail ? a->length : avail;
    memcpy((void *)(uintptr_t)a->buffer, g_image + off, xfer);
    a->xferred   = xfer;
    a->new_hiblk = efblk;
    a->new_efblk = efblk;
    a->status    = SS$_NORMAL;
    return SS$_NORMAL;
}

/* Write / fileop are unused by this read test but must resolve (rms_io.c
 * references them in the ACP write/ftruncate arms). */
uint32_t vms_kif_acp_writevb(struct vms_acp_rw_args *a) { (void)a; return SS$_NORMAL; }
uint32_t vms_kif_acp_fileop(struct vms_acp_fileop_args *a) { (void)a; return SS$_NORMAL; }

/* --------------------------------------------------------------------------- */

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); fails++; } } while (0)

static rms_file_t make_acp_handle(void)
{
    rms_file_t f;
    memset(&f, 0, sizeof(f));
    f.accessed = 1;
    f.fd       = -1;                 /* fd < 0 => rms_io_read dispatches to ACP */
    f.eof      = FILE_BYTES;
    f.cursor   = 0;
    f.rbuf_len = 0;
    return f;
}

int main(void)
{
    for (size_t i = 0; i < FILE_BYTES; i++)
        g_image[i] = pat(i);

    /* The read-ahead width is the VMS default multiblock count, not a magic
     * size. If someone retunes it, this assertion forces them past the
     * grounding note (vms-5b5) rather than silently drifting off RMS_DFMBC. */
    CHECK(RMS_IO_RAHEAD_BLKS == 16u,
          "RMS_IO_RAHEAD_BLKS=%u, expected the VMS RMS_DFMBC default of 16",
          (unsigned)RMS_IO_RAHEAD_BLKS);
    CHECK(RMS_IO_RAHEAD_BYTES == RMS_IO_RAHEAD_BLKS * 512u, "RAHEAD_BYTES derivation");

    /* ---- 1. byte-at-a-time scan: correctness + QIO count ------------------ */
    rms_file_t f = make_acp_handle();
    g_readvb_qios = 0;
    for (size_t i = 0; i < FILE_BYTES; i++) {
        uint8_t b = 0;
        ssize_t n = rms_io_read(&f, &b, 1);
        CHECK(n == 1, "short read at byte %zu (n=%zd)", i, n);
        CHECK(b == pat(i), "byte %zu: got 0x%02x expected 0x%02x", i, b, pat(i));
        if (b != pat(i)) break;
    }

    unsigned expect_multiblock = (FILE_BYTES + RMS_IO_RAHEAD_BYTES - 1) / RMS_IO_RAHEAD_BYTES;
    unsigned old_per_block      = (FILE_BYTES + 512u - 1) / 512u;
    CHECK(g_readvb_qios == expect_multiblock,
          "QIO count %u, expected %u (ceil(%u/%u))",
          g_readvb_qios, expect_multiblock, FILE_BYTES, (unsigned)RMS_IO_RAHEAD_BYTES);
    CHECK(g_readvb_qios < old_per_block,
          "no win: %u QIOs vs the old per-512-byte rate of %u",
          g_readvb_qios, old_per_block);
    printf("byte-scan of %u bytes: %u QIOs (old per-block would be %u)\n",
           FILE_BYTES, g_readvb_qios, old_per_block);

    /* ---- 2. re-seek inside the live window must NOT issue a new QIO -------- */
    f = make_acp_handle();
    uint8_t b;
    (void)rms_io_read(&f, &b, 1);          /* fills the window at offset 0     */
    unsigned after_fill = g_readvb_qios;
    rms_io_lseek(&f, 100, SEEK_SET);
    (void)rms_io_read(&f, &b, 1);
    CHECK(b == pat(100), "reseek byte: got 0x%02x expected 0x%02x", b, pat(100));
    CHECK(g_readvb_qios == after_fill, "reseek inside window issued a QIO");

    /* ---- 3. a large read (>= a block) bypasses the cache: one direct QIO --- */
    f = make_acp_handle();
    g_readvb_qios = 0;
    uint8_t big[4096];
    ssize_t got = rms_io_read(&f, big, sizeof(big));
    CHECK(got == (ssize_t)sizeof(big), "large read short (got %zd)", got);
    for (size_t i = 0; i < sizeof(big); i++)
        CHECK(big[i] == pat(i), "large-read byte %zu mismatch", i);
    CHECK(g_readvb_qios == 1, "large read used %u QIOs, expected 1", g_readvb_qios);

    if (fails) { fprintf(stderr, "test_rms_io_rahead: %d failure(s)\n", fails); return 1; }
    printf("test_rms_io_rahead: OK\n");
    return 0;
}
