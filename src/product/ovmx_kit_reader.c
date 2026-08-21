/*
 * ovmx_kit_reader.c - shared reader for the OVMX product kit container.
 * See ovmx_kit_reader.h for why this exists (vms-df9).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "ovmx_kit_reader.h"

/* ---- POSIX fd source backend (ovmx_kit_reader_open) --------------------
 * A kit that is a plain Linux file: factory tooling + the executive-absent
 * host ctest defer. The RMS/ACP backend is supplied by the caller
 * (src/product/product.c) and never touches this file. */
struct kit_posix_ctx { int fd; };

static int kit_posix_pread(void *ctx, void *buf, size_t n, uint64_t off)
{
    struct kit_posix_ctx *c = (struct kit_posix_ctx *)ctx;
    if (lseek(c->fd, (off_t)off, SEEK_SET) < 0)
        return -1;
    /* short read is an error at any offset the kit index/payload names */
    return (read(c->fd, buf, n) == (ssize_t)n) ? 0 : -1;
}

static void kit_posix_close(void *ctx)
{
    struct kit_posix_ctx *c = (struct kit_posix_ctx *)ctx;
    if (c) {
        if (c->fd >= 0)
            close(c->fd);
        free(c);
    }
}

/* Read + validate the header off r->src, caching it in r->hdr. On any failure
 * the source is closed and released (so the caller never double-frees). */
static int kit_reader_finish_open(ovmx_kit_reader_t *r)
{
    memset(&r->hdr, 0, sizeof(r->hdr));

    if (r->src.pread(r->src.ctx, &r->hdr, sizeof(r->hdr), 0) != 0 ||
        memcmp(r->hdr.kh_magic, OVMX_KIT_MAGIC, OVMX_KIT_MAGIC_LEN) != 0) {
        ovmx_kit_reader_close(r);
        return OVMX_KIT_READER_ERR_NOTKIT;
    }

    uint32_t want = r->hdr.kh_checksum;
    struct ovmx_kit_header check = r->hdr;
    check.kh_checksum = 0;
    if (ovmx_kit_checksum(&check, sizeof(check)) != want) {
        ovmx_kit_reader_close(r);
        return OVMX_KIT_READER_ERR_CHKSUM;
    }
    return OVMX_KIT_READER_OK;
}

int ovmx_kit_reader_open_source(ovmx_kit_reader_t *r, const ovmx_kit_source_t *source)
{
    memset(r, 0, sizeof(*r));
    if (!source || !source->pread) {
        if (source && source->close)
            source->close(source->ctx);
        return OVMX_KIT_READER_ERR_OPEN;
    }
    r->src = *source;
    return kit_reader_finish_open(r);
}

int ovmx_kit_reader_open(ovmx_kit_reader_t *r, const char *kitfile)
{
    memset(r, 0, sizeof(*r));

    int fd = open(kitfile, O_RDONLY);
    if (fd < 0)
        return OVMX_KIT_READER_ERR_OPEN;

    struct kit_posix_ctx *c = (struct kit_posix_ctx *)malloc(sizeof(*c));
    if (!c) {
        close(fd);
        return OVMX_KIT_READER_ERR_NOMEM;
    }
    c->fd = fd;
    r->src.ctx   = c;
    r->src.pread = kit_posix_pread;
    r->src.close = kit_posix_close;

    return kit_reader_finish_open(r);
}

int ovmx_kit_reader_entries(ovmx_kit_reader_t *r, struct ovmx_kit_entry **out)
{
    *out = NULL;
    if (r->hdr.kh_file_count == 0)
        return OVMX_KIT_READER_OK;

    struct ovmx_kit_entry *e = calloc(r->hdr.kh_file_count, sizeof(*e));
    if (!e)
        return OVMX_KIT_READER_ERR_NOMEM;

    size_t want = (size_t)r->hdr.kh_file_count * sizeof(*e);
    if (r->src.pread(r->src.ctx, e, want, r->hdr.kh_index_offset) != 0) {
        free(e);
        return OVMX_KIT_READER_ERR_READ;
    }

    *out = e;
    return OVMX_KIT_READER_OK;
}

int ovmx_kit_reader_read_file(ovmx_kit_reader_t *r,
                              const struct ovmx_kit_entry *e,
                              uint8_t **buf_out)
{
    *buf_out = NULL;

    if (e->ke_size == 0)
        return OVMX_KIT_READER_OK;

    uint8_t *buf = malloc(e->ke_size);
    if (!buf)
        return OVMX_KIT_READER_ERR_NOMEM;

    if (r->src.pread(r->src.ctx, buf, e->ke_size, e->ke_offset) != 0) {
        free(buf);
        return OVMX_KIT_READER_ERR_READ;
    }

    if (ovmx_kit_checksum(buf, e->ke_size) != e->ke_checksum) {
        free(buf);
        return OVMX_KIT_READER_ERR_CHKSUM;
    }

    *buf_out = buf;
    return OVMX_KIT_READER_OK;
}

int ovmx_kit_reader_relpath(const char *filespec, char *out, size_t outlen)
{
    const char *lb = strchr(filespec, '[');
    const char *rb = lb ? strchr(lb, ']') : NULL;
    if (!lb || !rb || rb <= lb)
        return OVMX_KIT_READER_ERR_BADSPEC;

    char dir[256];
    size_t dlen = (size_t)(rb - lb - 1);
    if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
    memcpy(dir, lb + 1, dlen);
    dir[dlen] = '\0';
    for (char *p = dir; *p; p++)
        if (*p == '.') *p = '/';

    const char *name = rb + 1;

    if (strcmp(dir, "000000") == 0)
        snprintf(out, outlen, "%s", name);
    else
        snprintf(out, outlen, "%s/%s", dir, name);
    return OVMX_KIT_READER_OK;
}

void ovmx_kit_reader_close(ovmx_kit_reader_t *r)
{
    if (r->src.close)
        r->src.close(r->src.ctx);
    r->src.ctx   = NULL;
    r->src.pread = NULL;
    r->src.close = NULL;
}
