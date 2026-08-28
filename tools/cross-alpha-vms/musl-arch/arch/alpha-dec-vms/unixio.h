/*
 * unixio.h -- DEC C UNIX-I/O emulation declarations for the OVMX
 * alpha-dec-vms musl port CRTL include surface (bead: zlib-crtl-rungs).
 *
 * WHAT THIS IS.  On a genuine OpenVMS DEC C toolchain, <unixio.h> is the
 * CRTL header that declares the UNIX-style low-level I/O emulation entry
 * points (open/close/read/write/lseek/... on RMS underneath).  Portable C
 * that targets VMS routinely does `#ifdef VMS #include <unixio.h>` (zlib's
 * zconf.h is the canonical example) because on VMS these decls do NOT come
 * from <unistd.h>.  The alpha-dec-vms GCC port predefines VMS/__VMS, so that
 * `#ifdef VMS` fires -- correctly, it IS a VMS toolchain -- and the compile
 * then needs <unixio.h> on the include path.
 *
 * WHY IT LIVES HERE.  The OVMX alpha-dec-vms port's C run-time is musl
 * (whole-archived into DECC$SHR.EXE).  musl already IMPLEMENTS every one of
 * these UNIX-I/O entry points -- this header only DECLARES them (no bodies),
 * so it is a faithful, minimal shim: it makes the VMS-only header name
 * resolvable without adding or faking any functionality.  It is scoped to the
 * alpha-dec-vms arch overlay (a VMS-target-only header, exactly as on a real
 * DEC C kit), never surfaced for a non-VMS target.
 *
 * Signatures track musl's own <unistd.h>/<fcntl.h> declarations exactly, so a
 * translation unit that includes BOTH <unixio.h> and <unistd.h> sees identical
 * prototypes (no conflicting-declaration diagnostic).  Types come from
 * <sys/types.h> (size_t/ssize_t/off_t/mode_t), same as the real header.
 *
 * CLEAN-ROOM (Rule 8): declarations derived from the public DEC C RTL
 * Reference Manual's <unixio.h> surface + musl's own prototypes.  No VSI/HPE
 * header source was read or copied.
 */
#ifndef _OVMX_UNIXIO_H
#define _OVMX_UNIXIO_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int     open(const char *, int, ...);
int     close(int);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
off_t   lseek(int, off_t, int);
int     dup(int);
int     dup2(int, int);
int     isatty(int);
int     unlink(const char *);
int     access(const char *, int);
int     ftruncate(int, off_t);
int     fsync(int);
int     chdir(const char *);
char   *getcwd(char *, size_t);

#ifdef __cplusplus
}
#endif

#endif /* _OVMX_UNIXIO_H */
