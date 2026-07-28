/*
 * ovmx_rms_io.h — OVMX addition (vms-4ba.5), NOT part of stock tinycc.
 *
 * Epic vms-4ba "self-host S2: a C compiler as an OVMX image" bootstraps tcc
 * as a VMS-native image (TCC.EXE, IMGACT-activated). vms-4ba.4 got it running
 * hosted on raw POSIX open()/read()/write()/close() (via DECC$SHR's musl).
 * This bead (vms-4ba.5) routes tcc's OWN file I/O for the files named on its
 * command line through OVMX's RMS system services instead:
 *
 *   - reading the primary .c source: sys$open + sys$connect + a sys$get loop
 *   - writing the output .o object:  sys$create + sys$connect + a sys$put
 *     loop + sys$close
 *
 * (src/vmsrms/include/rms/{fab,rab,rms}.h — RMS is already one of the six
 * shareables TCC.EXE links via mk_tcc.sh.)
 *
 * SCOPE (deliberately narrow — see mk_tcc.sh / the three call sites in
 * libtcc.c, tccpp.c, tccelf.c for the exact edits): only the PRIMARY input
 * file (tcc_add_file_internal, the .c the user named on the command line)
 * and the final object delivery (tcc_write_elf_file) are routed through RMS.
 * #include header search (tccpp.c's tcc_open(), which probes many candidate
 * system-include paths per header) is left on the stock _tcc_open()/open()
 * path — RMS filespec resolution for arbitrary system directories is
 * unproven and out of this bead's scope; nothing in the DONE conditions
 * requires it.
 *
 * Only compiled in / referenced when OVMX_RMS_IO is defined (mk_tcc.sh).
 */
#ifndef OVMX_RMS_IO_H
#define OVMX_RMS_IO_H

/* Handles returned by ovmx_rms_open_read() start here — comfortably clear of
 * real POSIX fds (0/1/2 stdio, tcc's small header-search fds) so callers can
 * tell an RMS-backed BufferedFile::fd apart from a stock open()'d one with a
 * single ">= OVMX_RMS_FD_BASE" test. */
#define OVMX_RMS_FD_BASE 10000

/* Read side. Mirrors the open()/read()/close() semantics tcc already expects
 * from _tcc_open()/handle_eob()/tcc_close(): fd >= OVMX_RMS_FD_BASE on
 * success, -1 if the open failed; read returns the number of bytes placed in
 * buf (0 at EOF, <0 on a hard error), matching POSIX read()'s contract.
 * Backed by sys$open + sys$connect (open) and a sys$get loop (read).
 */
int ovmx_rms_open_read(const char *filename);
int ovmx_rms_read(int fd, void *buf, int count);
int ovmx_rms_close_read(int fd);

/* Write side. tcc's own ELF assembly (tcc_output_elf/tcc_output_binary in
 * tccelf.c, all fwrite()/fputc() driven) is left untouched: it still writes
 * into a private scratch file via the stock open()/fdopen() sequence. What's
 * OVMX-specific is the FINAL delivery of that completed object to
 * `dest_path` (the path the caller asked for, e.g. "hello.o") — done here via
 * sys$create + sys$connect + a sys$put loop + sys$close, so tcc itself never
 * calls open()/write() on `dest_path`. Returns 0 on success, -1 on failure.
 */
int ovmx_rms_deliver_file(const char *scratch_path, const char *dest_path);

#endif /* OVMX_RMS_IO_H */
