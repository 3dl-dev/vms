/*
 * ovmx_olb.h - OVMX object-library (.OLB) container format.
 *
 * ============================ Rule 8 (clean-room) ============================
 * VSI does NOT publish the byte-level OpenVMS LBR .OLB on-disk layout in any
 * public document, so OVMX defines its OWN .OLB representation and LABELS it an
 * OVMX design choice here. This is NOT presented as VMS-authentic: an OVMX .OLB
 * is a standard System V / GNU `ar` archive whose members are OVMX .OBJ files
 * (ELF relocatable objects). It is derived only from the public `ar(5)` format
 * and the public VSI LIBRARIAN/LINK utility surface -- never from VSI/HPE
 * source or binaries. See docs/design-self-host-mmk-spine.md section 3.
 *
 * Why an ar container: LINK.EXE already ingests `ar` archives in-process
 * (src/vmslink/link.c, load_archive / load_archive_selective), so an
 * ar-container .OLB is consumed by the OVMX linker with no format bridge, and a
 * stock `ar t FOO.OLB` can inspect one (used as an independent test oracle).
 *
 * This is a header-only module (all functions `static inline`, so an unused one
 * does not warn) shared by the two producers that must agree on the byte
 * layout: LIBRARIAN.EXE (src/vmslink/librarian.c) and the DCL LIBRARY command's
 * OBJECT path (src/vmsdcl/dcl_library.c). LINK.EXE is the sole consumer and has
 * its own `ar` reader in link.c.
 * ============================================================================
 */
#ifndef OVMX_OLB_H
#define OVMX_OLB_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifndef _WIN32
#include <strings.h>   /* strcasecmp */
#endif

#define OLB_AR_MAGIC     "!<arch>\n"
#define OLB_AR_MAGIC_LEN 8
#define OLB_AR_HDR_SIZE  60
#define OLB_NAME_MAX     127   /* generous: VMS module names are <= 31 */

/* One archive member: a named OVMX .OBJ payload. */
struct olb_member {
    char           name[OLB_NAME_MAX + 1]; /* module name, NUL-terminated       */
    unsigned char *data;                   /* payload bytes (owned by caller)   */
    size_t         len;
};

enum {
    OLB_OK          =  0,
    OLB_ERR_OPEN    = -1,   /* cannot open the file                            */
    OLB_ERR_FORMAT  = -2,   /* not a valid ar / .OLB container                 */
    OLB_ERR_IO      = -3,   /* short read/write                                */
    OLB_ERR_MEM     = -4,   /* out of memory                                   */
    OLB_ERR_NOTFOUND= -5    /* named member not present                        */
};

/* Copy a module name into a char[OLB_NAME_MAX + 1], clamped + NUL-terminated
 * (avoids the strncpy truncation ambiguity -Wstringop-truncation warns on). */
static inline void olb_setname(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n > OLB_NAME_MAX) n = OLB_NAME_MAX;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---- internal: read a right-space-padded decimal ar header field. ---- */
static inline uint64_t olb__dec(const char *f, int len)
{
    uint64_t v = 0;
    for (int i = 0; i < len && f[i] >= '0' && f[i] <= '9'; i++)
        v = v * 10 + (uint64_t)(f[i] - '0');
    return v;
}

/* ---- internal: slurp a whole file into a fresh buffer. ---- */
static inline unsigned char *olb__slurp(const char *path, size_t *out_size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(fp); return NULL; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);
    *out_size = (size_t)sz;
    return buf;
}

/*
 * Parse an .OLB (ar container) into a malloc'd array of members. Each member's
 * `data` is an independent malloc copy (caller owns it; free via olb_free).
 * The `/` (symbol index) and `//` (long-name table) special members are
 * consumed for name resolution but are NOT returned as members.
 *
 * Returns OLB_OK and sets the members and count out-params, or a negative
 * OLB_ERR_*.
 * A non-existent file returns OLB_ERR_OPEN; the caller decides whether that is
 * an error (INSERT into a missing library) or an empty set.
 */
static inline int olb_read(const char *path, struct olb_member **members,
                           uint32_t *count)
{
    *members = NULL;
    *count = 0;

    size_t asize = 0;
    unsigned char *abuf = olb__slurp(path, &asize);
    if (!abuf) return OLB_ERR_OPEN;

    if (asize < OLB_AR_MAGIC_LEN ||
        memcmp(abuf, OLB_AR_MAGIC, OLB_AR_MAGIC_LEN) != 0) {
        free(abuf);
        return OLB_ERR_FORMAT;
    }

    /* Long-name string table ("//") once seen. */
    const char *longtab = NULL;
    size_t      longlen = 0;

    struct olb_member *arr = NULL;
    uint32_t n = 0, cap = 0;
    int rc = OLB_OK;

    size_t pos = OLB_AR_MAGIC_LEN;
    while (pos + OLB_AR_HDR_SIZE <= asize) {
        const char *h = (const char *)(abuf + pos);
        if (h[58] != '`' || h[59] != '\n') { rc = OLB_ERR_FORMAT; break; }
        uint64_t msize = olb__dec(h + 48, 10);
        size_t mdata = pos + OLB_AR_HDR_SIZE;
        if (mdata + msize > asize) { rc = OLB_ERR_FORMAT; break; }

        /* Raw 16-byte name field, trailing spaces trimmed. */
        char raw[17];
        memcpy(raw, h, 16); raw[16] = '\0';
        int e = 16; while (e > 0 && raw[e - 1] == ' ') raw[--e] = '\0';

        int is_symtab   = (strcmp(raw, "/") == 0 || strcmp(raw, "/SYM64/") == 0);
        int is_longtab  = (strcmp(raw, "//") == 0);

        if (is_longtab) {
            longtab = (const char *)(abuf + mdata);
            longlen = (size_t)msize;
        } else if (!is_symtab) {
            /* Resolve the member name. */
            char name[OLB_NAME_MAX + 1];
            name[0] = '\0';
            if (raw[0] == '/' && raw[1] >= '0' && raw[1] <= '9' && longtab) {
                /* Long name: "/offset" into the // table, terminated by '/' or \n. */
                size_t off = (size_t)olb__dec(raw + 1, 15);
                size_t j = 0;
                while (off + j < longlen && longtab[off + j] != '/' &&
                       longtab[off + j] != '\n' && j < OLB_NAME_MAX) {
                    name[j] = longtab[off + j];
                    j++;
                }
                name[j] = '\0';
            } else {
                /* Short name: strip a single trailing '/' (GNU terminator). */
                size_t l = strlen(raw);
                if (l > 0 && raw[l - 1] == '/') raw[--l] = '\0';
                olb_setname(name, raw);
            }

            if (n >= cap) {
                uint32_t ncap = cap ? cap * 2 : 16;
                struct olb_member *na = (struct olb_member *)
                    realloc(arr, (size_t)ncap * sizeof(*arr));
                if (!na) { rc = OLB_ERR_MEM; break; }
                arr = na; cap = ncap;
            }
            memset(&arr[n], 0, sizeof(arr[n]));
            olb_setname(arr[n].name, name);
            arr[n].len = (size_t)msize;
            arr[n].data = (unsigned char *)malloc(msize ? (size_t)msize : 1);
            if (!arr[n].data) { rc = OLB_ERR_MEM; break; }
            if (msize) memcpy(arr[n].data, abuf + mdata, (size_t)msize);
            n++;
        }

        pos = mdata + (size_t)msize;
        if (pos & 1) pos++;   /* members are 2-byte aligned */
    }

    free(abuf);
    if (rc != OLB_OK) {
        for (uint32_t i = 0; i < n; i++) free(arr[i].data);
        free(arr);
        return rc;
    }
    *members = arr;
    *count = n;
    return OLB_OK;
}

/* Free a member array returned by olb_read. */
static inline void olb_free(struct olb_member *members, uint32_t count)
{
    if (!members) return;
    for (uint32_t i = 0; i < count; i++) free(members[i].data);
    free(members);
}

/* ---- internal: write a padded ar header field (space-filled, no NUL). ---- */
static inline void olb__field(char *dst, int width, const char *s)
{
    int l = (int)strlen(s);
    if (l > width) l = width;
    memcpy(dst, s, (size_t)l);
    for (int i = l; i < width; i++) dst[i] = ' ';
}

/*
 * Write a complete .OLB (GNU ar container) from an in-memory member array.
 * Long member names (>15 chars) are emitted through a "//" string table, so a
 * stock `ar` can read the result. No `/` symbol index is written -- LINK.EXE
 * resolves by parsing member symbol tables directly (like `ar` without `s`).
 *
 * Returns OLB_OK or a negative OLB_ERR_*.
 */
static inline int olb_write(const char *path, const struct olb_member *members,
                            uint32_t count)
{
    /* Build the long-name string table for any name that will not fit the
     * 16-byte field as "name/" (i.e. strlen > 15). */
    char *longtab = NULL;
    size_t longlen = 0, longcap = 0;
    size_t *longoff = NULL;
    if (count) {
        longoff = (size_t *)calloc(count, sizeof(size_t));
        if (!longoff) return OLB_ERR_MEM;
    }
    for (uint32_t i = 0; i < count; i++) {
        longoff[i] = (size_t)-1;
        size_t l = strlen(members[i].name);
        if (l <= 15) continue;
        if (longlen + l + 2 > longcap) {
            size_t ncap = longcap ? longcap * 2 : 256;
            while (ncap < longlen + l + 2) ncap *= 2;
            char *nt = (char *)realloc(longtab, ncap);
            if (!nt) { free(longtab); free(longoff); return OLB_ERR_MEM; }
            longtab = nt; longcap = ncap;
        }
        longoff[i] = longlen;
        memcpy(longtab + longlen, members[i].name, l);
        longlen += l;
        longtab[longlen++] = '/';
        longtab[longlen++] = '\n';
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) { free(longtab); free(longoff); return OLB_ERR_OPEN; }

    int rc = OLB_OK;
    char hdr[OLB_AR_HDR_SIZE];

    if (fwrite(OLB_AR_MAGIC, 1, OLB_AR_MAGIC_LEN, fp) != OLB_AR_MAGIC_LEN) {
        rc = OLB_ERR_IO; goto done;
    }

    /* Emit the "//" long-name table first (GNU convention) if any. */
    if (longlen) {
        char szbuf[32];
        snprintf(szbuf, sizeof szbuf, "%zu", longlen);
        memset(hdr, ' ', sizeof hdr);
        olb__field(hdr + 0,  16, "//");
        olb__field(hdr + 16, 12, "0");        /* mtime */
        olb__field(hdr + 28, 6,  "0");        /* uid   */
        olb__field(hdr + 34, 6,  "0");        /* gid   */
        olb__field(hdr + 40, 8,  "0");        /* mode  */
        olb__field(hdr + 48, 10, szbuf);      /* size  */
        hdr[58] = '`'; hdr[59] = '\n';
        if (fwrite(hdr, 1, OLB_AR_HDR_SIZE, fp) != OLB_AR_HDR_SIZE) {
            rc = OLB_ERR_IO; goto done;
        }
        if (fwrite(longtab, 1, longlen, fp) != longlen) { rc = OLB_ERR_IO; goto done; }
        if (longlen & 1) { if (fputc('\n', fp) == EOF) { rc = OLB_ERR_IO; goto done; } }
    }

    for (uint32_t i = 0; i < count; i++) {
        char namefield[OLB_NAME_MAX + 4];
        if (longoff[i] != (size_t)-1) {
            snprintf(namefield, sizeof namefield, "/%zu", longoff[i]);
        } else {
            snprintf(namefield, sizeof namefield, "%s/", members[i].name);
        }
        char szbuf[32];
        snprintf(szbuf, sizeof szbuf, "%zu", members[i].len);
        memset(hdr, ' ', sizeof hdr);
        olb__field(hdr + 0,  16, namefield);
        olb__field(hdr + 16, 12, "0");
        olb__field(hdr + 28, 6,  "0");
        olb__field(hdr + 34, 6,  "0");
        olb__field(hdr + 40, 8,  "644");
        olb__field(hdr + 48, 10, szbuf);
        hdr[58] = '`'; hdr[59] = '\n';
        if (fwrite(hdr, 1, OLB_AR_HDR_SIZE, fp) != OLB_AR_HDR_SIZE) {
            rc = OLB_ERR_IO; goto done;
        }
        if (members[i].len &&
            fwrite(members[i].data, 1, members[i].len, fp) != members[i].len) {
            rc = OLB_ERR_IO; goto done;
        }
        if (members[i].len & 1) {
            if (fputc('\n', fp) == EOF) { rc = OLB_ERR_IO; goto done; }
        }
    }

done:
    if (fclose(fp) != 0 && rc == OLB_OK) rc = OLB_ERR_IO;
    free(longtab);
    free(longoff);
    return rc;
}

/* Find a member by (case-insensitive) name; returns index or -1. */
static inline int olb_find(const struct olb_member *members, uint32_t count,
                           const char *name)
{
    for (uint32_t i = 0; i < count; i++) {
#ifdef _WIN32
        if (_stricmp(members[i].name, name) == 0) return (int)i;
#else
        if (strcasecmp(members[i].name, name) == 0) return (int)i;
#endif
    }
    return -1;
}

#endif /* OVMX_OLB_H */
