/* dcl_memfile.c - see dcl/dcl_memfile.h (rd vms-97c2). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "dcl/dcl_memfile.h"

struct dcl_pf {
    FILE  *fp;          /* non-NULL: a stdio stream */
    char  *buf;         /* otherwise: the procedure text */
    size_t len;
    size_t pos;
};

struct dcl_pf *dcl_pf_from_mem(char *buf, size_t len)
{
    struct dcl_pf *pf = calloc(1, sizeof(*pf));

    if (!pf)
        return NULL;
    pf->buf = buf;
    pf->len = len;
    return pf;
}

struct dcl_pf *dcl_pf_from_file(FILE *fp)
{
    struct dcl_pf *pf;

    if (!fp)
        return NULL;
    pf = calloc(1, sizeof(*pf));
    if (!pf)
        return NULL;
    pf->fp = fp;
    return pf;
}

char *dcl_pf_gets(char *s, int n, struct dcl_pf *pf)
{
    size_t i = 0;

    if (!pf || !s || n <= 0)
        return NULL;
    if (pf->fp)
        return fgets(s, n, pf->fp);
    if (pf->pos >= pf->len)
        return NULL;
    while ((int)i < n - 1 && pf->pos < pf->len) {
        char c = pf->buf[pf->pos++];

        s[i++] = c;
        if (c == '\n')
            break;
    }
    s[i] = '\0';
    return s;
}

long dcl_pf_tell(struct dcl_pf *pf)
{
    if (!pf)
        return -1;
    return pf->fp ? ftell(pf->fp) : (long)pf->pos;
}

int dcl_pf_seek(struct dcl_pf *pf, long off, int whence)
{
    long base;

    if (!pf)
        return -1;
    if (pf->fp)
        return fseek(pf->fp, off, whence);
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (long)pf->pos; break;
    case SEEK_END: base = (long)pf->len; break;
    default: return -1;
    }
    if (base + off < 0)
        return -1;
    /* Positioning past the end reads nothing, as a file does. */
    pf->pos = (size_t)(base + off) > pf->len ? pf->len : (size_t)(base + off);
    return 0;
}

void dcl_pf_rewind(struct dcl_pf *pf)
{
    (void)dcl_pf_seek(pf, 0, SEEK_SET);
}

int dcl_pf_close(struct dcl_pf *pf)
{
    int rc = 0;

    if (!pf)
        return 0;
    if (pf->fp)
        rc = fclose(pf->fp);
    free(pf->buf);
    free(pf);
    return rc;
}

FILE *dcl_scratch_tmpfile(void)
{
    FILE *tf = tmpfile();

    if (!tf)
        return NULL;
    if (fputc('\n', tf) == EOF || fflush(tf) != 0) {
        fclose(tf);
        return NULL;
    }
    rewind(tf);
    if (ftruncate(fileno(tf), 0) != 0) {
        fclose(tf);
        return NULL;
    }
    rewind(tf);
    return tf;
}

/*
 * Put `len` bytes of input on fd 0 for the duration of an image activation,
 * saving the old fd 0 in si->saved_fd0. No tmpfs: /dev/null (empty), a pipe
 * (<= DCL_SYSINPUT_PIPE_MAX, written whole before the image starts, so it can
 * never block and an image that never reads cannot deadlock us), else a scratch
 * file whose every write is checked. Returns 0, or -1 with fd 0 untouched.
 */
#define DCL_SYSINPUT_PIPE_MAX 4096
int dcl_sysinput_install(struct dcl_sysinput *si, const char *buf, size_t len)
{
    int nfd = -1;

    if (len == 0) {
        nfd = open("/dev/null", O_RDONLY);
    } else if (len <= DCL_SYSINPUT_PIPE_MAX) {
        int pf[2];

        if (pipe(pf) == 0) {
            ssize_t w;

            do { w = write(pf[1], buf, len); } while (w < 0 && errno == EINTR);
            close(pf[1]);
            if (w == (ssize_t)len)
                nfd = pf[0];
            else
                close(pf[0]);
        }
    }
    if (nfd < 0 && len > 0) {
        FILE *tf = tmpfile();

        if (tf) {
            if (fwrite(buf, 1, len, tf) == len && fflush(tf) == 0) {
                rewind(tf);
                nfd = dup(fileno(tf));
            }
            fclose(tf);
        }
    }
    if (nfd < 0)
        return -1;
    si->saved_fd0 = dup(0);
    if (si->saved_fd0 < 0 || dup2(nfd, 0) < 0) {
        if (si->saved_fd0 >= 0) {
            close(si->saved_fd0);
            si->saved_fd0 = -1;
        }
        close(nfd);
        return -1;
    }
    close(nfd);
    return 0;
}

int dcl_write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (w == 0)
            return -EIO;
        off += (size_t)w;
    }
    return 0;
}
