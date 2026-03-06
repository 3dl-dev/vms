/*
 * rms_util.h - Shared RMS I/O utility functions
 *
 * Provides read_exact/write_exact helpers used by sequential, relative,
 * and indexed file implementations.
 */

#ifndef RMS_UTIL_H
#define RMS_UTIL_H

#include <stddef.h>
#include <sys/types.h>

/*
 * rms_read_exact - Read exactly 'count' bytes from fd at current position.
 * Returns number of bytes read, or -1 on error.  Returns less than count
 * only on EOF.
 */
ssize_t rms_read_exact(int fd, void *buf, size_t count);

/*
 * rms_write_exact - Write exactly 'count' bytes to fd.
 * Returns 0 on success, -1 on error.
 */
int rms_write_exact(int fd, const void *buf, size_t count);

#endif /* RMS_UTIL_H */
