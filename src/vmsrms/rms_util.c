/*
 * rms_util.c - Shared RMS I/O utility functions
 *
 * Consolidates duplicate read_exact/write_exact implementations from
 * rms_seq.c, rms_rel.c, and rms_idx.c.
 */

#include <unistd.h>
#include "rms_util.h"

/*
 * rms_read_exact - Read exactly 'count' bytes from fd at current position.
 * Returns number of bytes read, or -1 on error.
 */
ssize_t rms_read_exact(int fd, void *buf, size_t count)
{
    size_t total = 0;
    char *p = (char *)buf;

    while (total < count) {
        ssize_t n = read(fd, p + total, count - total);
        if (n < 0) return -1;
        if (n == 0) break;  /* EOF */
        total += (size_t)n;
    }

    return (ssize_t)total;
}

/*
 * rms_write_exact - Write exactly 'count' bytes to fd.
 * Returns 0 on success, -1 on error.
 */
int rms_write_exact(int fd, const void *buf, size_t count)
{
    size_t total = 0;
    const char *p = (const char *)buf;

    while (total < count) {
        ssize_t n = write(fd, p + total, count - total);
        if (n <= 0) return -1;
        total += (size_t)n;
    }

    return 0;
}
