/*
 * ovmx_rms_io.h — OVMX addition (vms-0b6b), NOT part of stock GNU binutils.
 *
 * Epic vms-da0 "GCC-as-VMS-oracle" / bead vms-0b6b: GNU `as` (gas), built as
 * a VMS-native image (AS.EXE, IMGACT-activated), is F1 of the production-
 * compiler forcing-function lane. This mirrors third-party/tcc/ovmx/
 * ovmx_rms_io.{c,h} (bead vms-4ba.5) almost exactly — same RMS calls, same
 * scratch-then-deliver shape for the write side — applied to gas instead of
 * tcc:
 *
 *   - reading the primary .s source:   sys$open + sys$connect + a sys$get loop
 *   - writing the output .o object:    sys$create + sys$connect + a sys$put
 *     loop + sys$close, delivering a private scratch file BFD already
 *     finished writing (see gas/output-file.c's OVMX_RMS_IO seam — BFD's own
 *     ELF writer is left completely untouched; only the FINAL delivery of the
 *     completed object to the caller's requested path is RMS-routed)
 *
 * (src/vmsrms/include/rms/{fab,rab,rms}.h — RMS is one of the six OVMX
 * shareables AS.EXE links via mk_as.sh, same graph TCC.EXE links.)
 *
 * SCOPE (deliberately narrow, same spirit as tcc's ovmx_rms_io.h): only the
 * PRIMARY input file (gas/input-file.c's input_file_open(), the .s named on
 * the command line — NOT .include'd files, NOT stdin) and the final object
 * delivery (gas/output-file.c's output_file_close()) are routed through RMS.
 * gas's own #NO_APP/#APP leading-comment sniff in input_file_open() (a
 * single-character stdio getc()/ungetc() peek, done before the real read
 * loop starts) is SKIPPED under OVMX_RMS_IO rather than reimplemented over
 * chunked RMS reads — see the seam in input-file.c for why, and the vms-0b6b
 * report for what this means for GCC-emitted (as opposed to hand-written)
 * .s input.
 *
 * Only compiled in / referenced when OVMX_RMS_IO is defined (mk_as.sh).
 */
#ifndef OVMX_RMS_IO_H
#define OVMX_RMS_IO_H

/* Handles returned by ovmx_rms_open_read() start here — comfortably clear of
 * real POSIX fds so callers can tell an RMS-backed read apart from a stock
 * fopen()'d FILE* with a single ">= OVMX_RMS_FD_BASE" test, same convention
 * as tcc's shim. */
#define OVMX_RMS_FD_BASE 10000

/* Read side. fd >= OVMX_RMS_FD_BASE on success, -1 if the open failed; read
 * returns bytes placed in buf (0 at EOF, <0 on a hard error) — matches
 * gas/input-file.c's input_file_get() contract (fread()-shaped). Backed by
 * sys$open + sys$connect (open) and a sys$get loop (read). */
int ovmx_rms_open_read(const char *filename);
int ovmx_rms_read(int fd, void *buf, int count);
int ovmx_rms_close_read(int fd);

/* Write side. gas/BFD's own ELF object writer (bfd_openw/bfd_close,
 * gas/output-file.c) is left untouched: it still writes into a private
 * scratch file via BFD's normal open/write sequence. What's OVMX-specific is
 * the FINAL delivery of that completed object to `dest_path` (the path the
 * caller asked for via `as -o`), done here via sys$create + sys$connect + a
 * sys$put loop + sys$close, so BFD itself never touches `dest_path` directly.
 * Returns 0 on success, -1 on failure. */
int ovmx_rms_deliver_file(const char *scratch_path, const char *dest_path);

#endif /* OVMX_RMS_IO_H */
