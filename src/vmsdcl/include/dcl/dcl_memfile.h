/*
 * dcl_memfile.h - DCL's command-procedure stream: a seekable line source that
 * is either a stdio FILE (the passthrough fopen path) or a malloc'd buffer.
 *
 * WHY (rd vms-97c2). DCL stages a command procedure it read through RMS in a
 * stream the fgets/fseek script engine drives. That stream was tmpfile(): a
 * file on /tmp, which on NetBSD is a tmpfs, and a tmpfs write is refused with
 * ENOSPC whenever the machine has less than TMPFS_PAGES_RESERVED (4 MB) of
 * memory available -- no matter how small the file or how large the mount's
 * size limit. A 16 MB VAX that has joined a cluster is exactly there, and the
 * unchecked fwrite() turned the refusal into a silently truncated STARTUP.COM
 * that "succeeded" having run nothing. The procedure text is a few KB; it is
 * held in memory and freed by dcl_pf_close().
 *
 * Plain C only: no funopen/fopencookie. DCL.EXE is linked against DECC$SHR on
 * the VMS-native rail, whose C RTL has neither.
 */
#ifndef DCL_MEMFILE_H
#define DCL_MEMFILE_H

#include <stdio.h>
#include <stddef.h>

struct dcl_pf;

/* Takes ownership of `buf` (malloc'd, `len` bytes; NULL when len == 0). NULL on
 * allocation failure, in which case the caller still owns `buf`. */
struct dcl_pf *dcl_pf_from_mem(char *buf, size_t len);
/* Wraps an open stdio stream; dcl_pf_close() fcloses it. */
struct dcl_pf *dcl_pf_from_file(FILE *fp);

char *dcl_pf_gets(char *s, int n, struct dcl_pf *pf);   /* fgets semantics */
long  dcl_pf_tell(struct dcl_pf *pf);
int   dcl_pf_seek(struct dcl_pf *pf, long off, int whence);
void  dcl_pf_rewind(struct dcl_pf *pf);
int   dcl_pf_close(struct dcl_pf *pf);


/* A scratch stream that has PROVED it can take data (rd vms-f47): tmpfile()
 * plus a one-byte write probe, or NULL. On NetBSD /tmp is a tmpfs, whose writes
 * fail with ENOSPC below 4 MB of available memory -- and a tmpfile() created
 * under that condition looks fine until the first fwrite. */
FILE *dcl_scratch_tmpfile(void);

/* write() all of `buf`, retrying EINTR: 0, or -errno (short writes are errors). */
int dcl_write_all(int fd, const char *buf, size_t len);

/* SYS$INPUT-from-procedure redirection state (dcl_cmd.h, vms-1a9). */
struct dcl_sysinput { int saved_fd0; };

/* Put `len` bytes on fd 0 for an image activation, saving the old fd 0 in
 * si->saved_fd0. No tmpfs: /dev/null (empty), a pipe (small), else a scratch
 * file whose writes are checked. 0, or -1 with fd 0 untouched. */
int dcl_sysinput_install(struct dcl_sysinput *si, const char *buf, size_t len);

#endif
