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

/* ======================================================================== *
 * vms-3320: the file-op veneer beyond the stdio family. The alpha GCC PORT's
 * DECC$SHR binds these decc$ file-ops to musl-POSIX (raw Linux-Alpha VFS
 * callsys), NOT RMS -- so temp-file minting (open/creat), cleanup + the
 * create->use->delete lifecycle (unlink/remove), atomic output finalization
 * (rename), and directory enumeration (opendir/readdir/closedir) never reach
 * the executive/ODS-2 volume (docs/design-gcc-port-surface-gaps-register.md
 * §3.2). These veneers close that binding to the SAME proven RMS engine the
 * stdio family rides -- sys$create/$open/$erase and, for the atomic rename, the
 * executive ACP MODIFY!M_MOVE primitive (vms-de7) via the new sys$rename RMS
 * service. FAIL-HONEST (INV-6 / Rule 9): every op returns the real RMS/SS$
 * status; there is NO POSIX fallback on the executive-present runtime path.
 *
 * The port wiring aliases decc$open->ovmx_crtl_open, ... in the alpha DECC$SHR
 * symbol vector (src/vmslink/mk_decc_shr.sh ALPHA_CRTL_RMS_USE block), exactly
 * as the stdio family, and IN THEIR SORTED SLOT (never tail-appended -- IMGACT
 * binds by sv# index; vms-b14).
 * ======================================================================== */

/* --- open/creat: POSIX-signature int-fd file minting over RMS -------------
 * These return an int fd (the DEC C decc$open/creat ABI), NOT a FILE*. The fd
 * indexes a small veneer-private RMS-handle table whose values start at a high
 * base (OVMX_CRTL_FD_BASE) so they can NEVER be confused with a musl POSIX fd
 * (an accidental musl read()/close() on one returns EBADF, honest, never a
 * silent wrong success). The `oflag` bits are interpreted with the compile-
 * target's own <fcntl.h> O_* values, so a caller compiled against the same
 * (alpha musl / host) headers passes the flags this veneer expects.
 *
 * open:  O_CREAT set (or any write mode + a missing file via O_CREAT) ->
 *        sys$create (mints a real ODS-2 version ;N with a genuine File ID);
 *        else sys$open. O_RDONLY -> read stream; O_WRONLY/O_RDWR -> write.
 * creat: == open(path, O_CREAT|O_WRONLY|O_TRUNC): always sys$create.
 * Returns the fd (>= OVMX_CRTL_FD_BASE) or -1 on any RMS failure / table full. */
#define OVMX_CRTL_FD_BASE  0x40000000
#define OVMX_CRTL_FD_MAX   64
/* NON-variadic (vms-3320): the DEC C decc$open prototype is variadic (the mode
 * arg is optional), but this veneer IGNORES mode, so it takes two fixed args.
 * A variadic definition SIGSEGV'd on alpha-dec-vms -- the VMS/Alpha varargs ABI
 * (AI register + argument home-area) is a codegen path this cross-toolchain
 * mishandles (x86_64/LP64 was clean, alpha/LP64 crashed on the FIRST call,
 * before any output -- the classic x86_64-green/alpha-red variadic split). A
 * caller that passes a 3rd (mode) arg is harmless: alpha passes it in a register
 * the 2-arg callee never reads. */
int ovmx_crtl_open(const char *path, int oflag);
int ovmx_crtl_creat(const char *path, int mode);

/* ovmx_crtl_fdclose: close an fd minted by ovmx_crtl_open/creat (sys$close +
 * free the table slot). sys$close is what FINALIZES the ODS-2 header/FH2, so
 * the created file's File ID becomes visible to an independent reader -- a
 * created-but-never-closed file's FID only finalizes at clean image exit, which
 * a crash (e.g. alpha vms-c5d) preempts, so a genuine port program MUST close
 * explicitly (vms-3320). This IS vector-substituted onto decc$close in the
 * alpha DECC$SHR (mk_decc_shr.sh), so a port's close(fd) reaches RMS.
 * ⚠ It closes ONLY fds this veneer minted (>= OVMX_CRTL_FD_BASE); a foreign fd
 * (socket/pipe) returns -1 fail-honest (INV-6: no silent POSIX fallback). Sound
 * for a file-only compiler port; a socket-using port image would need the
 * fd-close design extended -- flagged for that future case.
 * Returns 0 on success, -1 on a bad/foreign fd or a close failure. */
int ovmx_crtl_fdclose(int fd);

/* --- unlink/remove: file deletion over sys$erase --------------------------
 * Both remove the named file from the ODS-2 volume via sys$erase (IO$_DELETE:
 * directory-entry removal + header/blocks deallocation). remove() is the ISO C
 * spelling of the same file deletion. Returns 0 on success, -1 on any RMS
 * failure (fail-honest; an independent DIRECTORY then shows the file GONE). */
int ovmx_crtl_unlink(const char *path);
int ovmx_crtl_remove(const char *path);

/* --- rename: ATOMIC directory-entry re-link over sys$rename ---------------
 * Drives the new sys$rename RMS service -> the executive ACP MODIFY!M_MOVE
 * primitive (vms-de7): the file KEEPS its File ID and allocation; only the
 * directory entry is re-linked (old {name,ver} removed, new {name,ver}
 * inserted). This is decc$rename's "atomic output finalization" semantics
 * (compiler writes NAME.tmp, then renames it over the final name) done
 * faithfully -- NOT erase+create (which is non-atomic and mints a NEW FID). An
 * independent DIRECTORY then shows the new name carrying the SAME File ID as
 * the old had. Returns 0 on success, -1 on any RMS failure. */
int ovmx_crtl_rename(const char *oldpath, const char *newpath);

/* --- opendir/readdir/closedir: real ODS-2 directory enumeration -----------
 * A DIR*-equivalent over the sys$parse+sys$search wildcard context (the SAME
 * engine DCL DIRECTORY / F$SEARCH ride). opendir composes "<dirspec>*.*;*" and
 * sys$parses it; each readdir is one sys$search step returning the next real
 * ODS-2 directory entry (its filename + genuine File ID from rms_search_fid);
 * closedir releases the executive wildcard context (rms_search_end) + frees the
 * handle. Fail-honest: opendir NULL on a bad dir; readdir NULL at RMS$_NMF or
 * on error. */
typedef struct ovmx_crtl_dir OVMX_CRTL_DIR;

/* Directory entry. d_name is FIRST (offset 0) so the common `ent->d_name`
 * access is layout-robust; d_namlen and d_fileid follow. d_fileid is the
 * GENUINE ODS-2 File-ID number the executive directory scan returned (an
 * enumeration a musl-POSIX readdir on a raw VFS cannot produce). */
struct ovmx_crtl_dirent {
    char           d_name[256];   /* NAME.TYP;VER of the match (after the ']') */
    unsigned short d_namlen;      /* length of d_name                          */
    unsigned short d_fileid;      /* genuine ODS-2 File-ID number (rms_search_fid) */
};

OVMX_CRTL_DIR           *ovmx_crtl_opendir(const char *name);
struct ovmx_crtl_dirent *ovmx_crtl_readdir(OVMX_CRTL_DIR *dirp);
int                      ovmx_crtl_closedir(OVMX_CRTL_DIR *dirp);

#endif /* __RMS_CRTL_STDIO_H */
