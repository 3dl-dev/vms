/*
 * crtl_stdio.h — OVMX addition (vms-47e): the C RTL stdio FILE veneer that
 * makes a DEC C RTL file op (fopen/fwrite/fread/fclose) drive RMS system
 * services (sys$create/$open/$connect/$put/$get/$close) against the real
 * Files-11 ODS-2 volume over the executive ACP — NOT musl POSIX open()/read()/
 * write() into a raw kernel filesystem.
 *
 * WHY THIS EXISTS. On real OpenVMS the DEC C RTL stdio layer is a thin veneer
 * over RMS: fopen mints a FAB/RAB and calls $CREATE/$OPEN+$CONNECT, fwrite is a
 * $PUT, fread a $GET, fclose a $CLOSE (VSI OpenVMS C RTL Reference Manual,
 * "Record Management Services and the DEC C RTL"). The alpha-dec-vms GCC PORT's
 * DECC$SHR, by contrast, whole-archives musl-alpha, so its decc$fopen/decc$fwrite
 * are musl POSIX whose open()/write() are a raw Alpha `callsys` into the
 * Linux-Alpha kernel VFS — they never reach RMS, the executive, or the ODS-2
 * volume (trace-grounded finding, vms-47e; docs/design-gcc-port-surface-gaps-
 * register.md §3.1). This file is the genuine veneer that closes that binding:
 * the SAME source is meant to be compiled into the port's DECC$SHR so
 * decc$fopen re-points here (that alpha wiring is the child sub-project — see
 * the register §3.2), and it is proven here, first, on the layer where a real
 * /dev/vms + mounted ODS-2 + independent ACP reader exist today
 * (tests/qemu/test_syssvc_crtl_rms_veneer.c).
 *
 * BYTE-EXACTNESS (the whole point — a compiler's .OBJ/.EXE round-trip must be
 * byte-identical). Mirrors ovmx_link_rms_io.c exactly:
 *   - WRITE uses FAB$C_FIX with fab$w_mrs=0, so each $PUT writes exactly its
 *     rab$w_rsz bytes with no padding/delimiter (src/vmsrms/rms_seq.c).
 *   - READ uses FAB$C_FIX with fab$w_mrs=1 (one byte per record), so the $GET
 *     loop reads exactly the file's bytes and hits RMS$_EOF precisely at end of
 *     file — no space-pad of a final partial record.
 *
 * FAIL-HONEST (INV-6 / Rule 9): every op returns the real RMS failure. There is
 * NO POSIX/ramfs fallback — a file that cannot be created/opened on the ODS-2
 * volume is a hard NULL/short-count, never a silent success against some other
 * filesystem. That silent fallback is precisely the overclaim this closes.
 *
 * These are OVMX-original names (ovmx_crtl_*), NOT the decc$ ABI: the port
 * wiring aliases decc$fopen -> ovmx_crtl_fopen in the DECC$SHR symbol vector,
 * exactly as ovmx_decc_crtl.c's _malloc32 becomes decc$malloc.
 */
#ifndef __RMS_CRTL_STDIO_H
#define __RMS_CRTL_STDIO_H

#include <stddef.h>

/* Opaque RMS-backed stream handle (returned as an OVMX_CRTL_FILE*). The port
 * wiring casts this to the DEC C `FILE *` the port passes around; only the
 * ovmx_crtl_* ops below ever dereference it. */
typedef struct ovmx_crtl_file OVMX_CRTL_FILE;

/* fopen veneer. mode "r"/"rb" -> sys$open+$connect for a byte-exact read;
 * mode "w"/"wb" -> sys$create+$connect (mints a real ODS-2 version ;N). Returns
 * a stream handle (caller closes with ovmx_crtl_fclose) or NULL on any RMS
 * failure (fail-honest, no POSIX fallback). Unsupported modes -> NULL. */
OVMX_CRTL_FILE *ovmx_crtl_fopen(const char *path, const char *mode);

/* fwrite veneer: one byte-exact sys$put of (size*nmemb) bytes (FAB$C_FIX,
 * mrs=0). Returns the number of whole members written (nmemb on success, 0 on
 * failure) — the C fwrite contract. */
size_t ovmx_crtl_fwrite(const void *ptr, size_t size, size_t nmemb,
                        OVMX_CRTL_FILE *fh);

/* fread veneer: a byte-exact sys$get loop (FAB$C_FIX, mrs=1) filling up to
 * (size*nmemb) bytes, stopping at RMS$_EOF. Returns the number of whole members
 * read — the C fread contract. */
size_t ovmx_crtl_fread(void *ptr, size_t size, size_t nmemb,
                       OVMX_CRTL_FILE *fh);

/* fclose veneer: sys$close + free the handle. Returns 0 on success, EOF(-1) on
 * a close failure or a NULL handle. */
int ovmx_crtl_fclose(OVMX_CRTL_FILE *fh);

#endif /* __RMS_CRTL_STDIO_H */
