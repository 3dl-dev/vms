/* dcl_memfile.c - see dcl/dcl_memfile.h (rd vms-97c2). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
