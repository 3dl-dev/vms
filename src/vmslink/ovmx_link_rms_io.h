/*
 * ovmx_link_rms_io.h — OVMX addition (vms-b5a), the RMS I/O shim that makes
 * LINK.EXE-as-an-OVMX-image read its input object and write its output image
 * through OVMX's RMS system services instead of raw POSIX open()/read()/
 * write()/close(). Mirrors third-party/tcc/ovmx/ovmx_rms_io.{c,h} (vms-4ba.5),
 * which did the same for TCC.EXE.
 *
 * WHY A SEPARATE SHIM (not tcc's): tcc's shim carries tcc-specific
 * BufferedFile::fd slot semantics and a scratch-file DELIVER step (tcc writes
 * its ELF into a private scratch, then RMS-delivers the finished file). LINK.EXE
 * is simpler in both directions:
 *   - read:  it slurps a whole object file into one malloc buffer (link.c's
 *            slurp()), so the shim exposes a whole-file read, not an fd/read/
 *            close triple.
 *   - write: it already holds the complete output image in memory (link.c's
 *            `img`/`file_sz`), so the shim writes that buffer directly via a
 *            sys$put loop — no scratch file, no reopen.
 *
 * BYTE-EXACTNESS (this is the whole point — link output must run):
 *   - WRITE uses FAB$C_FIX with fab$w_mrs=0, so rms_seq_put() takes each put's
 *     own rab$w_rsz as the record length and writes exactly that many bytes
 *     with no padding/delimiter (src/vmsrms/rms_seq.c) — the identical,
 *     proven byte-exact path tcc's ovmx_rms_deliver_file uses.
 *   - READ uses FAB$C_FIX with fab$w_mrs=1 (one byte per record). This is
 *     deliberate: a larger FIX record size makes rms_seq_get() SPACE-PAD the
 *     final partial record and report rsz=recsize (src/vmsrms/rms_seq.c
 *     FAB$C_FIX case), which rounds a binary object's size up to the record
 *     boundary and corrupts its tail — fatal for a linker. mrs=1 reads exactly
 *     the file's bytes and hits RMS$_EOF precisely at end of file. The harness
 *     (run_link_native.sh) cross-checks the RMS-read byte total against the
 *     object's real on-disk size to prove this.
 *
 * SCOPE (mirrors tcc's vms-4ba.5 scoping decision exactly): only the PRIMARY
 * input object read + the OUTPUT image write are routed through RMS. The
 * --use producer shareable-image reads (link.c's load_producer) are left on
 * the stock open()/read() path — they are large library inputs, the direct
 * analog of the header-search reads tcc deliberately left on the stock path.
 * Nothing in the proof requires them through RMS.
 *
 * Only compiled in / referenced when OVMX_RMS_IO is defined (mk_link.sh).
 */
#ifndef OVMX_LINK_RMS_IO_H
#define OVMX_LINK_RMS_IO_H

#include <stddef.h>
#include <stdint.h>

/* Read a whole file into a fresh malloc buffer via RMS (sys$open + sys$connect
 * + a byte-exact sys$get loop to RMS$_EOF + sys$close). On success returns the
 * buffer (caller frees) and sets *out_size to the exact byte count. Returns
 * NULL on any failure. */
uint8_t *ovmx_link_rms_slurp(const char *path, size_t *out_size);

/* Peek the first `n` bytes of a file via RMS (open/connect/get/close). Returns
 * the number of bytes placed in buf (0..n), or -1 on open failure. Used by
 * file_is_archive() to sniff the `ar` magic without slurping a whole archive. */
int ovmx_link_rms_peek(const char *path, void *buf, int n);

/* Write `size` bytes to `path` via RMS (sys$create + sys$connect + a byte-exact
 * sys$put loop + sys$close). sys$create mints a VMS version suffix, so the file
 * actually appears on disk as "<path>;1". Returns 0 on success, -1 on failure. */
int ovmx_link_rms_write(const char *path, const uint8_t *buf, size_t size);

#endif /* OVMX_LINK_RMS_IO_H */
