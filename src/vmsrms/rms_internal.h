#ifndef RMS_INTERNAL_H
#define RMS_INTERNAL_H

/*
 * rms_internal.h - Shared internal helpers for RMS subsystem
 *
 * These functions are shared across rms_seq.c, rms_rel.c, and rms_idx.c
 * to avoid code duplication.  They are NOT part of the public RMS API.
 */

#include <sys/types.h>
#include <stddef.h>

/*
 * rms_read_exact - Read exactly 'count' bytes from fd.
 * Returns number of bytes read, or -1 on error.
 */
ssize_t rms_read_exact(int fd, void *buf, size_t count);

/*
 * rms_write_exact - Write exactly 'count' bytes to fd.
 * Returns 0 on success, -1 on error.
 */
int rms_write_exact(int fd, const void *buf, size_t count);

#endif /* RMS_INTERNAL_H */
