/* dcl_memfile.c - see dcl/dcl_memfile.h (rd vms-97c2). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include "dcl/dcl_memfile.h"

struct dcl_memfile {
    char  *buf;
    size_t len;
    size_t pos;
};

static size_t mf_read_bytes(struct dcl_memfile *m, char *out, size_t n)
{
    size_t left = m->len - m->pos;

    if (n > left)
        n = left;
    memcpy(out, m->buf + m->pos, n);
    m->pos += n;
    return n;
}

static int mf_seek_to(struct dcl_memfile *m, long long off, int whence, long long *res)
{
    long long base;

    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (long long)m->pos; break;
    case SEEK_END: base = (long long)m->len; break;
    default: errno = EINVAL; return -1;
    }
    if (base + off < 0) {
        errno = EINVAL;
        return -1;
    }
    /* Reading past the end just reads nothing, as a file does. */
    m->pos = (size_t)(base + off) > m->len ? m->len : (size_t)(base + off);
    *res = (long long)m->pos;
    return 0;
}

static void mf_free(struct dcl_memfile *m)
{
    free(m->buf);
    free(m);
}

#if defined(__NetBSD__)

static int nb_read(void *c, char *b, int n)
{
    return (int)mf_read_bytes((struct dcl_memfile *)c, b, (size_t)n);
}

static off_t nb_seek(void *c, off_t off, int whence)
{
    long long r;

    if (mf_seek_to((struct dcl_memfile *)c, off, whence, &r) != 0)
        return -1;
    return (off_t)r;
}

static int nb_close(void *c)
{
    mf_free((struct dcl_memfile *)c);
    return 0;
}

FILE *dcl_memfile_open(char *buf, size_t len)
{
    struct dcl_memfile *m = calloc(1, sizeof(*m));
    FILE *fp;

    if (!m)
        return NULL;
    m->buf = buf;
    m->len = len;
    fp = funopen(m, nb_read, NULL, nb_seek, nb_close);
    if (!fp)
        free(m);                  /* the caller keeps buf */
    return fp;
}

#else  /* glibc / musl: fopencookie */

static ssize_t lx_read(void *c, char *b, size_t n)
{
    return (ssize_t)mf_read_bytes((struct dcl_memfile *)c, b, n);
}

static int lx_seek(void *c, off_t *off, int whence)
{
    long long r;

    if (mf_seek_to((struct dcl_memfile *)c, *off, whence, &r) != 0)
        return -1;
    *off = (off_t)r;
    return 0;
}

static int lx_close(void *c)
{
    mf_free((struct dcl_memfile *)c);
    return 0;
}

FILE *dcl_memfile_open(char *buf, size_t len)
{
    static const cookie_io_functions_t io = {
        .read = lx_read, .write = NULL, .seek = lx_seek, .close = lx_close,
    };
    struct dcl_memfile *m = calloc(1, sizeof(*m));
    FILE *fp;

    if (!m)
        return NULL;
    m->buf = buf;
    m->len = len;
    fp = fopencookie(m, "r", io);
    if (!fp)
        free(m);
    return fp;
}

#endif
