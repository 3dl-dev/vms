/*
 * vms_stdio.c - Buffered I/O (replaces glibc stdio)
 *
 * Provides vms_fopen/fclose/fread/fwrite/fgets/fprintf/printf
 * over raw file descriptors via vms_sys_* syscall wrappers.
 */

#include "vms_stdio.h"
#include "vms_syscall.h"
#include "vms_string.h"
#include "vms_snprintf.h"
#include "vms_futex.h"

/* ================================================================
 * Standard streams (statically allocated, not heap)
 * ================================================================ */

vms_file_t vms_stdin_obj = {
    .fd = 0, .flags = VMS_FILE_READ, .buf_mode = 1 /* line buffered */
};
vms_file_t vms_stdout_obj = {
    .fd = 1, .flags = VMS_FILE_WRITE, .buf_mode = 1 /* line buffered */
};
vms_file_t vms_stderr_obj = {
    .fd = 2, .flags = VMS_FILE_WRITE, .buf_mode = 2 /* unbuffered */
};

void vms_stdio_init(void)
{
    /* Already initialized statically; hook for future TLS-based init */
}

/* ================================================================
 * Internal helpers
 * ================================================================ */

/* Simple memory-mapped allocator for file handles (avoid malloc dependency) */
#define MAX_VMS_FILES 64
static vms_file_t vms_file_pool[MAX_VMS_FILES];
static int vms_file_used[MAX_VMS_FILES];
static vms_mutex_t vms_file_pool_lock = VMS_MUTEX_INIT;

static vms_file_t *alloc_file(void)
{
    vms_mutex_lock(&vms_file_pool_lock);
    for (int i = 0; i < MAX_VMS_FILES; i++) {
        if (!vms_file_used[i]) {
            vms_file_used[i] = 1;
            vms_memset(&vms_file_pool[i], 0, sizeof(vms_file_t));
            vms_file_pool[i].flags = VMS_FILE_ALLOC;
            vms_mutex_unlock(&vms_file_pool_lock);
            return &vms_file_pool[i];
        }
    }
    vms_mutex_unlock(&vms_file_pool_lock);
    return NULL;
}

static void free_file(vms_file_t *f)
{
    if (f->flags & VMS_FILE_ALLOC) {
        vms_mutex_lock(&vms_file_pool_lock);
        for (int i = 0; i < MAX_VMS_FILES; i++) {
            if (&vms_file_pool[i] == f) {
                vms_file_used[i] = 0;
                vms_mutex_unlock(&vms_file_pool_lock);
                return;
            }
        }
        vms_mutex_unlock(&vms_file_pool_lock);
    }
}

static int flush_write_buf(vms_file_t *f)
{
    if (f->buf_pos > 0) {
        int written = 0;
        while (written < f->buf_pos) {
            vms_ssize_t n = vms_sys_write(f->fd, f->buf + written,
                                           (vms_size_t)(f->buf_pos - written));
            if (n < 0) {
                f->error = 1;
                return -1;
            }
            written += (int)n;
        }
        f->buf_pos = 0;
    }
    return 0;
}

static int fill_read_buf(vms_file_t *f)
{
    vms_ssize_t n = vms_sys_read(f->fd, f->buf, VMS_STDIO_BUFSZ);
    if (n < 0) {
        f->error = 1;
        return -1;
    }
    if (n == 0) {
        f->eof = 1;
        return 0;
    }
    f->buf_pos = 0;
    f->buf_len = (int)n;
    return (int)n;
}

/* ================================================================
 * File open/close
 * ================================================================ */

vms_file_t *vms_fopen(const char *path, const char *mode)
{
    int flags = 0;
    int fflags = 0;

    if (mode[0] == 'r') {
        flags = VMS_O_RDONLY;
        fflags = VMS_FILE_READ;
        if (mode[1] == '+') {
            flags = VMS_O_RDWR;
            fflags |= VMS_FILE_WRITE;
        }
    } else if (mode[0] == 'w') {
        flags = VMS_O_WRONLY | VMS_O_CREAT | VMS_O_TRUNC;
        fflags = VMS_FILE_WRITE;
        if (mode[1] == '+') {
            flags = VMS_O_RDWR | VMS_O_CREAT | VMS_O_TRUNC;
            fflags |= VMS_FILE_READ;
        }
    } else if (mode[0] == 'a') {
        flags = VMS_O_WRONLY | VMS_O_CREAT | VMS_O_APPEND;
        fflags = VMS_FILE_WRITE | VMS_FILE_APPEND;
        if (mode[1] == '+') {
            flags = VMS_O_RDWR | VMS_O_CREAT | VMS_O_APPEND;
            fflags |= VMS_FILE_READ;
        }
    } else {
        return NULL;
    }

    int fd = vms_sys_openat(VMS_AT_FDCWD, path, flags, 0666);
    if (fd < 0)
        return NULL;

    vms_file_t *f = alloc_file();
    if (!f) {
        vms_sys_close(fd);
        return NULL;
    }

    f->fd = fd;
    f->flags |= fflags;
    f->buf_mode = 0; /* fully buffered for files */
    return f;
}

int vms_fclose(vms_file_t *f)
{
    if (!f)
        return VMS_EOF;

    if (f->flags & VMS_FILE_WRITE)
        flush_write_buf(f);

    int ret = vms_sys_close(f->fd);
    free_file(f);
    return (ret < 0) ? VMS_EOF : 0;
}

/* ================================================================
 * Read operations
 * ================================================================ */

vms_size_t vms_fread(void *ptr, vms_size_t size, vms_size_t nmemb, vms_file_t *f)
{
    if (size == 0 || nmemb == 0)
        return 0;

    vms_size_t total = size * nmemb;
    vms_size_t done = 0;
    char *dest = (char *)ptr;

    while (done < total) {
        /* Consume from buffer first */
        if (f->buf_pos < f->buf_len) {
            int avail = f->buf_len - f->buf_pos;
            vms_size_t want = total - done;
            vms_size_t copy = (want < (vms_size_t)avail) ? want : (vms_size_t)avail;
            vms_memcpy(dest + done, f->buf + f->buf_pos, copy);
            f->buf_pos += (int)copy;
            done += copy;
        } else {
            /* Buffer empty, refill */
            if (fill_read_buf(f) <= 0)
                break;
        }
    }

    return done / size;
}

char *vms_fgets(char *s, int size, vms_file_t *f)
{
    if (size <= 0)
        return NULL;

    int i = 0;
    while (i < size - 1) {
        /* Get a byte from buffer */
        if (f->buf_pos >= f->buf_len) {
            if (fill_read_buf(f) <= 0)
                break;
        }
        char c = f->buf[f->buf_pos++];
        s[i++] = c;
        if (c == '\n')
            break;
    }

    if (i == 0)
        return NULL;

    s[i] = '\0';
    return s;
}

int vms_fgetc(vms_file_t *f)
{
    if (f->buf_pos >= f->buf_len) {
        if (fill_read_buf(f) <= 0)
            return VMS_EOF;
    }
    return (unsigned char)f->buf[f->buf_pos++];
}

/* ================================================================
 * Write operations
 * ================================================================ */

vms_size_t vms_fwrite(const void *ptr, vms_size_t size, vms_size_t nmemb, vms_file_t *f)
{
    if (size == 0 || nmemb == 0)
        return 0;

    vms_size_t total = size * nmemb;
    const char *src = (const char *)ptr;

    /* Unbuffered: write directly */
    if (f->buf_mode == 2) {
        vms_size_t done = 0;
        while (done < total) {
            vms_ssize_t n = vms_sys_write(f->fd, src + done, total - done);
            if (n < 0) {
                f->error = 1;
                break;
            }
            done += (vms_size_t)n;
        }
        return done / size;
    }

    for (vms_size_t i = 0; i < total; i++) {
        f->buf[f->buf_pos++] = src[i];

        int do_flush = 0;
        if (f->buf_pos >= VMS_STDIO_BUFSZ)
            do_flush = 1;
        else if (f->buf_mode == 1 && src[i] == '\n')
            do_flush = 1;

        if (do_flush) {
            if (flush_write_buf(f) < 0)
                return i / size;
        }
    }

    return nmemb;
}

int vms_fputc(int c, vms_file_t *f)
{
    unsigned char ch = (unsigned char)c;
    if (vms_fwrite(&ch, 1, 1, f) != 1)
        return VMS_EOF;
    return ch;
}

int vms_fputs(const char *s, vms_file_t *f)
{
    vms_size_t len = vms_strlen(s);
    if (vms_fwrite(s, 1, len, f) != len)
        return VMS_EOF;
    return 0;
}

/* ================================================================
 * Flush / seek / status
 * ================================================================ */

int vms_fflush(vms_file_t *f)
{
    if (!f) {
        /* Flush all writable streams */
        flush_write_buf(vms_stdout);
        flush_write_buf(vms_stderr);
        return 0;
    }
    if (f->flags & VMS_FILE_WRITE)
        return flush_write_buf(f);
    return 0;
}

int vms_feof(vms_file_t *f)
{
    return f->eof;
}

int vms_ferror(vms_file_t *f)
{
    return f->error;
}

int vms_fileno(vms_file_t *f)
{
    return f->fd;
}

int vms_fseek(vms_file_t *f, long offset, int whence)
{
    if (f->flags & VMS_FILE_WRITE)
        flush_write_buf(f);

    /* Invalidate read buffer */
    f->buf_pos = 0;
    f->buf_len = 0;
    f->eof = 0;

    vms_off_t ret = vms_sys_lseek(f->fd, offset, whence);
    return (ret < 0) ? -1 : 0;
}

long vms_ftell(vms_file_t *f)
{
    vms_off_t pos = vms_sys_lseek(f->fd, 0, VMS_SEEK_CUR);
    if (pos < 0)
        return -1;

    /* Adjust for buffered data */
    if (f->flags & VMS_FILE_READ)
        pos -= (f->buf_len - f->buf_pos);
    else if (f->flags & VMS_FILE_WRITE)
        pos += f->buf_pos;

    return (long)pos;
}

/* ================================================================
 * Formatted output
 * ================================================================ */

int vms_vfprintf(vms_file_t *f, const char *fmt, va_list ap)
{
    char tmp[2048];
    va_list ap2;
    va_copy(ap2, ap);
    int len = vms_vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (len > 0) {
        if (len < (int)sizeof(tmp)) {
            /* Output fits in stack buffer */
            vms_fwrite(tmp, 1, (vms_size_t)len, f);
        } else {
            /* Output was truncated; allocate a larger buffer via mmap */
            vms_size_t need = (vms_size_t)len + 1;
            char *big = (char *)vms_sys_mmap(NULL, need,
                                              VMS_PROT_READ | VMS_PROT_WRITE,
                                              VMS_MAP_PRIVATE | VMS_MAP_ANONYMOUS,
                                              -1, 0);
            if (big != VMS_MAP_FAILED) {
                vms_vsnprintf(big, need, fmt, ap2);
                vms_fwrite(big, 1, (vms_size_t)len, f);
                vms_sys_munmap(big, need);
            } else {
                /* Fallback: write the truncated buffer */
                vms_fwrite(tmp, 1, sizeof(tmp) - 1, f);
            }
        }
    }
    va_end(ap2);
    return len;
}

int vms_fprintf(vms_file_t *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vms_vfprintf(f, fmt, ap);
    va_end(ap);
    return ret;
}

int vms_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vms_vfprintf(vms_stdout, fmt, ap);
    va_end(ap);
    return ret;
}
