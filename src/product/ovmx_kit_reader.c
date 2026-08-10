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

int ovmx_kit_reader_open(ovmx_kit_reader_t *r, const char *kitfile)
{
    r->fd = -1;
    memset(&r->hdr, 0, sizeof(r->hdr));

    int fd = open(kitfile, O_RDONLY);
    if (fd < 0)
        return OVMX_KIT_READER_ERR_OPEN;

    if (read(fd, &r->hdr, sizeof(r->hdr)) != (ssize_t)sizeof(r->hdr) ||
        memcmp(r->hdr.kh_magic, OVMX_KIT_MAGIC, OVMX_KIT_MAGIC_LEN) != 0) {
        close(fd);
        return OVMX_KIT_READER_ERR_NOTKIT;
    }

    uint32_t want = r->hdr.kh_checksum;
    struct ovmx_kit_header check = r->hdr;
    check.kh_checksum = 0;
    if (ovmx_kit_checksum(&check, sizeof(check)) != want) {
        close(fd);
        return OVMX_KIT_READER_ERR_CHKSUM;
    }

    r->fd = fd;
    return OVMX_KIT_READER_OK;
}

int ovmx_kit_reader_entries(ovmx_kit_reader_t *r, struct ovmx_kit_entry **out)
{
    *out = NULL;
    if (r->hdr.kh_file_count == 0)
        return OVMX_KIT_READER_OK;

    struct ovmx_kit_entry *e = calloc(r->hdr.kh_file_count, sizeof(*e));
    if (!e)
        return OVMX_KIT_READER_ERR_NOMEM;

    if (lseek(r->fd, (off_t)r->hdr.kh_index_offset, SEEK_SET) < 0) {
        free(e);
        return OVMX_KIT_READER_ERR_READ;
    }

    size_t want = (size_t)r->hdr.kh_file_count * sizeof(*e);
    if (read(r->fd, e, want) != (ssize_t)want) {
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

    if (lseek(r->fd, (off_t)e->ke_offset, SEEK_SET) < 0 ||
        read(r->fd, buf, e->ke_size) != (ssize_t)e->ke_size) {
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
    if (r->fd >= 0) {
        close(r->fd);
        r->fd = -1;
    }
}
