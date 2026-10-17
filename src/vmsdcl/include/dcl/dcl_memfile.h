/*
 * dcl_memfile.h - a seekable read-only stdio stream over a malloc'd buffer.
 *
 * WHY (rd vms-97c2). DCL stages a command procedure it read through RMS in a
 * stdio stream the fgets/fseek script engine drives. That stream was tmpfile():
 * a file on /tmp, which on NetBSD is a tmpfs, and a tmpfs write is refused with
 * ENOSPC whenever the machine has less than TMPFS_PAGES_RESERVED (4 MB) of
 * memory available -- no matter how small the file or how large the mount's
 * size limit. A 16 MB VAX that has joined a cluster is exactly there, and the
 * unchecked fwrite() turned the refusal into a silently truncated STARTUP.COM
 * that "succeeded" having run nothing. The procedure text is a few KB; it is
 * held in memory, owned by the stream, and freed by fclose().
 */
#ifndef DCL_MEMFILE_H
#define DCL_MEMFILE_H

#include <stdio.h>
#include <stddef.h>

/* Takes ownership of `buf` (malloc'd, `len` bytes; may be NULL when len == 0).
 * Returns a read-only seekable stream, or NULL on failure -- in which case the
 * caller still owns `buf`. fclose() on the stream frees `buf`. */
FILE *dcl_memfile_open(char *buf, size_t len);

#endif
