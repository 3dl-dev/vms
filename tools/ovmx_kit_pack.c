/*
 * ovmx_kit_pack.c - Pack an OVMX product kit file from a staged tree.
 *
 * Factory BUILD tooling (docs/design-vms-faithful-install.md sec 3.3 policy;
 * same class as tools/vmsfs_master.c): never shipped on the media, never
 * given a VMS name, never run at boot. Consuming a kit at install time
 * (PRODUCT INSTALL, the product database) is vms-df9's job -- this tool only
 * produces and inspects the container.
 *
 * Format: src/libvms/include/ovmx_kit_format.h (OVMX-invented container,
 * labeled per Rule 8 -- see that header's comment for what's behavior-
 * derived vs invented). Spec: docs/design-ovmx-kit-format.md.
 *
 * Modes:
 *   ovmx_kit_pack pack <kit-file> <staging-dir> <product-name> [producer]
 *       Walk <staging-dir> (its direct children are the top-level VMS
 *       directories, e.g. SYSEXE/SYSLIB/SYSMGR/SYSHLP under SYS$COMMON:)
 *       and emit <kit-file>. <product-name> is a literal like "X86VMS VMS"
 *       -- the caller (build script) composes the full name from
 *       OVMX_PRODUCT_NAME + this, so the "OVMX" vendor token and the
 *       version both come from ovmx_identity.h, never a second literal.
 *       Producer defaults to OVMX_PRODUCT_NAME.
 *
 *   ovmx_kit_pack list <kit-file>
 *       Print the kit's identification and its file manifest (filespec,
 *       size, protection, owner UIC) -- the "PRODUCT SHOW PRODUCT"-shaped
 *       inspection this format needs to be useful before vms-df9 exists.
 *
 *   ovmx_kit_pack extract <kit-file> <output-dir>
 *       Round-trip reader: write every payload file back out under
 *       <output-dir>, reconstructing the staged-tree layout from each
 *       entry's filespec. Used to prove a packed kit carries the bytes,
 *       not just a manifest of promises (pack -> extract -> diff -r).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "ovmx_kit_format.h"
#include "ovmx_identity.h"
#include "ovmx_kit_reader.h"

/* ================================================================
 * Shared helpers
 * ================================================================ */

static void upcase(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z')
            *s -= 32;
}

/* ================================================================
 * pack mode
 * ================================================================ */

struct pack_entry {
    char     filespec[OVMX_KIT_FILESPEC_MAX];
    char     host_path[4096];
    uint64_t size;
};

struct pack_list {
    struct pack_entry *items;
    size_t count;
    size_t cap;
};

static void pack_list_add(struct pack_list *l, const char *filespec,
                          const char *host_path, uint64_t size)
{
    if (l->count == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 32;
        struct pack_entry *p = realloc(l->items, nc * sizeof(*p));
        if (!p) { fprintf(stderr, "%%KITPACK-F-NOMEM, out of memory\n"); exit(1); }
        l->items = p;
        l->cap = nc;
    }
    struct pack_entry *e = &l->items[l->count++];
    snprintf(e->filespec, sizeof(e->filespec), "%s", filespec);
    snprintf(e->host_path, sizeof(e->host_path), "%s", host_path);
    e->size = size;
}

/*
 * Build the VMS bracket component for a relative directory path
 * ("" -> "000000", "SYSEXE" -> "SYSEXE", "SYSEXE/SUB" -> "SYSEXE.SUB"),
 * upcased. OVMX-invented mapping (Rule 8): real VMS directory nesting uses
 * the same dot-separated bracket syntax, but the choice to mirror the
 * staged Linux tree 1:1 into it is this build tool's policy, not a fact
 * about PCSI.
 */
static void reldir_to_bracket(const char *reldir, char *out, size_t outlen)
{
    if (!reldir || reldir[0] == '\0') {
        snprintf(out, outlen, "000000");
        return;
    }
    snprintf(out, outlen, "%s", reldir);
    for (char *p = out; *p; p++)
        if (*p == '/') *p = '.';
    upcase(out);
}

/* Recursively walk a host directory, adding every regular file found. */
static void walk_stage(const char *root, const char *reldir, struct pack_list *l)
{
    char full[4096];
    if (reldir[0] == '\0')
        snprintf(full, sizeof(full), "%s", root);
    else
        snprintf(full, sizeof(full), "%s/%s", root, reldir);

    DIR *d = opendir(full);
    if (!d) {
        fprintf(stderr, "%%KITPACK-F-OPENDIR, cannot open %s: %s\n",
                full, strerror(errno));
        exit(1);
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        char childrel[4096];
        if (reldir[0] == '\0')
            snprintf(childrel, sizeof(childrel), "%s", ent->d_name);
        else
            snprintf(childrel, sizeof(childrel), "%s/%s", reldir, ent->d_name);

        char childfull[8192];
        snprintf(childfull, sizeof(childfull), "%s/%s", root, childrel);

        struct stat st;
        if (lstat(childfull, &st) != 0) {
            fprintf(stderr, "%%KITPACK-W-STAT, cannot stat %s: %s\n",
                    childfull, strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            walk_stage(root, childrel, l);
        } else if (S_ISREG(st.st_mode)) {
            /* Split childrel into parent-dir part and basename. */
            const char *slash = strrchr(childrel, '/');
            char dirpart[4096];
            const char *base;
            if (slash) {
                size_t dl = (size_t)(slash - childrel);
                memcpy(dirpart, childrel, dl);
                dirpart[dl] = '\0';
                base = slash + 1;
            } else {
                dirpart[0] = '\0';
                base = childrel;
            }

            char bracket[256];
            reldir_to_bracket(dirpart, bracket, sizeof(bracket));

            char name[sizeof(((struct pack_entry *)0)->host_path)];
            snprintf(name, sizeof(name), "%s", base);
            upcase(name);

            char filespec[OVMX_KIT_FILESPEC_MAX];
            int n = snprintf(filespec, sizeof(filespec), "SYS$COMMON:[%s]%s",
                             bracket, name);
            if (n < 0 || n >= (int)sizeof(filespec)) {
                fprintf(stderr, "%%KITPACK-E-SPECLONG, filespec too long: %s\n",
                        childrel);
                exit(1);
            }

            pack_list_add(l, filespec, childfull, (uint64_t)st.st_size);
        } else {
            fprintf(stderr, "%%KITPACK-W-SKIP, skipping non-regular %s\n", childfull);
        }
    }
    closedir(d);
}

static int entry_cmp(const void *a, const void *b)
{
    const struct pack_entry *ea = a;
    const struct pack_entry *eb = b;
    return strcmp(ea->filespec, eb->filespec);
}

static int do_pack(const char *kitfile, const char *stagedir,
                   const char *product_suffix, const char *producer)
{
    struct pack_list l = {0};
    walk_stage(stagedir, "", &l);

    if (l.count == 0) {
        fprintf(stderr, "%%KITPACK-F-EMPTY, no files found under %s\n", stagedir);
        return 1;
    }

    /* Deterministic order -> reproducible kit builds. */
    qsort(l.items, l.count, sizeof(l.items[0]), entry_cmp);

    /* Product name: OVMX vendor token (ovmx_identity.h) + the caller's
     * arch/product suffix, e.g. "OVMX" + " X86VMS VMS" -- never a second
     * hardcoded version literal (that comes from OVMX_PRODUCT_VERSION). */
    char product_name[OVMX_KIT_NAME_MAX];
    int pn = snprintf(product_name, sizeof(product_name), "%s %s",
                      OVMX_PRODUCT_NAME, product_suffix);
    if (pn < 0 || pn >= (int)sizeof(product_name)) {
        fprintf(stderr, "%%KITPACK-F-NAMELONG, product name too long\n");
        free(l.items);
        return 1;
    }

    if (!producer || producer[0] == '\0')
        producer = OVMX_PRODUCT_NAME;

    struct ovmx_kit_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.kh_magic, OVMX_KIT_MAGIC, OVMX_KIT_MAGIC_LEN);
    hdr.kh_format_version = OVMX_KIT_FORMAT_VERSION;
    snprintf(hdr.kh_product_name, sizeof(hdr.kh_product_name), "%s", product_name);
    snprintf(hdr.kh_producer, sizeof(hdr.kh_producer), "%s", producer);
    snprintf(hdr.kh_product_version, sizeof(hdr.kh_product_version), "%s",
             OVMX_PRODUCT_VERSION);
    hdr.kh_build_time = (uint64_t)time(NULL);
    hdr.kh_file_count = (uint32_t)l.count;
    hdr.kh_index_offset = sizeof(struct ovmx_kit_header);
    hdr.kh_payload_offset = hdr.kh_index_offset +
        (uint64_t)l.count * sizeof(struct ovmx_kit_entry);

    int fd = open(kitfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "%%KITPACK-F-CREATE, cannot create %s: %s\n",
                kitfile, strerror(errno));
        free(l.items);
        return 1;
    }

    /* Build entries (offsets require a first pass over sizes). */
    struct ovmx_kit_entry *entries = calloc(l.count, sizeof(*entries));
    if (!entries) {
        fprintf(stderr, "%%KITPACK-F-NOMEM, out of memory\n");
        close(fd); free(l.items);
        return 1;
    }

    uint64_t off = hdr.kh_payload_offset;
    for (size_t i = 0; i < l.count; i++) {
        struct pack_entry *pe = &l.items[i];
        struct ovmx_kit_entry *ke = &entries[i];
        snprintf(ke->ke_filespec, sizeof(ke->ke_filespec), "%s", pe->filespec);
        ke->ke_protection = OVMX_KIT_PROT_DEFAULT;
        ke->ke_uic_group  = OVMX_KIT_UIC_GROUP_DEFAULT;
        ke->ke_uic_member = OVMX_KIT_UIC_MEMBER_DEFAULT;
        ke->ke_size   = pe->size;
        ke->ke_offset = off;
        off += pe->size;
    }

    /* Header checksum computed with the field itself zeroed. */
    hdr.kh_checksum = 0;
    hdr.kh_checksum = ovmx_kit_checksum(&hdr, sizeof(hdr));

    int rc = 0;
    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) { rc = 1; goto done; }
    if (write(fd, entries, l.count * sizeof(*entries)) !=
        (ssize_t)(l.count * sizeof(*entries))) { rc = 1; goto done; }

    for (size_t i = 0; i < l.count; i++) {
        struct pack_entry *pe = &l.items[i];
        struct ovmx_kit_entry *ke = &entries[i];

        if (pe->size == 0)
            continue;

        int in = open(pe->host_path, O_RDONLY);
        if (in < 0) {
            fprintf(stderr, "%%KITPACK-F-OPENIN, cannot read %s: %s\n",
                    pe->host_path, strerror(errno));
            rc = 1; goto done;
        }
        uint8_t *buf = malloc(pe->size);
        if (!buf) {
            fprintf(stderr, "%%KITPACK-F-NOMEM, out of memory\n");
            close(in); rc = 1; goto done;
        }
        uint64_t got = 0;
        while (got < pe->size) {
            ssize_t k = read(in, buf + got, pe->size - got);
            if (k <= 0) break;
            got += (uint64_t)k;
        }
        close(in);
        if (got != pe->size) {
            fprintf(stderr, "%%KITPACK-F-SHORT, short read on %s\n", pe->host_path);
            free(buf); rc = 1; goto done;
        }
        ke->ke_checksum = ovmx_kit_checksum(buf, pe->size);
        if (write(fd, buf, pe->size) != (ssize_t)pe->size) {
            fprintf(stderr, "%%KITPACK-F-WRITE, write failed on %s: %s\n",
                    kitfile, strerror(errno));
            free(buf); rc = 1; goto done;
        }
        free(buf);
    }

    /* Rewrite the index now that per-file checksums are known. */
    if (lseek(fd, (off_t)hdr.kh_index_offset, SEEK_SET) < 0) { rc = 1; goto done; }
    if (write(fd, entries, l.count * sizeof(*entries)) !=
        (ssize_t)(l.count * sizeof(*entries))) { rc = 1; goto done; }

    printf("%%KITPACK-I-DONE, packed %s (%s %s), %zu files, %llu bytes\n",
           kitfile, hdr.kh_product_name, hdr.kh_product_version, l.count,
           (unsigned long long)(off));

done:
    if (fsync(fd) < 0)
        fprintf(stderr, "%%KITPACK-W-FSYNC, sync warning: %s\n", strerror(errno));
    close(fd);
    free(entries);
    free(l.items);
    return rc;
}

/* ================================================================
 * list / extract: shared reader
 *
 * The actual open/validate/read-entries/read-file-content logic lives in
 * src/product/ovmx_kit_reader.c (vms-df9) -- PRODUCT.EXE needs the exact
 * same sequence to install a kit's payload, and duplicating it here a
 * second time is the "hand-roll a second kit parser" vms-df9's spec
 * forbids. This file keeps only the %KITPACK-shaped error rendering and
 * its own list/extract presentation logic.
 * ================================================================ */

typedef ovmx_kit_reader_t kit_reader_t;

static int reader_open(kit_reader_t *r, const char *kitfile)
{
    int rc = ovmx_kit_reader_open(r, kitfile);
    switch (rc) {
    case OVMX_KIT_READER_OK:
        return 0;
    case OVMX_KIT_READER_ERR_OPEN:
        fprintf(stderr, "%%KITPACK-F-OPENIN, cannot open %s: %s\n",
                kitfile, strerror(errno));
        return -1;
    case OVMX_KIT_READER_ERR_NOTKIT:
        fprintf(stderr, "%%KITPACK-F-NOTKIT, %s is not an OVMX kit file\n", kitfile);
        return -1;
    case OVMX_KIT_READER_ERR_CHKSUM:
        fprintf(stderr, "%%KITPACK-F-CHKSUM, %s header checksum mismatch\n", kitfile);
        return -1;
    default:
        fprintf(stderr, "%%KITPACK-F-OPENIN, cannot open %s\n", kitfile);
        return -1;
    }
}

static int read_entries(kit_reader_t *r, struct ovmx_kit_entry **out)
{
    int rc = ovmx_kit_reader_entries(r, out);
    if (rc == OVMX_KIT_READER_ERR_NOMEM) {
        fprintf(stderr, "%%KITPACK-F-NOMEM, out of memory\n");
        return -1;
    }
    if (rc != OVMX_KIT_READER_OK) {
        fprintf(stderr, "%%KITPACK-F-READ, short read of kit index\n");
        return -1;
    }
    return 0;
}

/* Render a VMS SOGW protection word as "(S:RWED,O:RWED,G:RE,W:RE)". */
static void protection_to_text(uint16_t prot, char *out, size_t outlen)
{
    static const struct { unsigned shift; char c; } cats[4] = {
        { VMS_PROT_SYSTEM_SHIFT, 'S' }, { VMS_PROT_OWNER_SHIFT, 'O' },
        { VMS_PROT_GROUP_SHIFT, 'G' },  { VMS_PROT_WORLD_SHIFT, 'W' },
    };
    char buf[64] = "(";
    for (int i = 0; i < 4; i++) {
        unsigned nib = (prot >> cats[i].shift) & 0x0F;
        char frag[8];
        int p = 0;
        frag[p++] = cats[i].c;
        frag[p++] = ':';
        if (!(nib & VMS_PROT_R)) frag[p++] = 'R';
        if (!(nib & VMS_PROT_W)) frag[p++] = 'W';
        if (!(nib & VMS_PROT_E)) frag[p++] = 'E';
        if (!(nib & VMS_PROT_D)) frag[p++] = 'D';
        frag[p] = '\0';
        strncat(buf, frag, sizeof(buf) - strlen(buf) - 1);
        if (i < 3) strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
    }
    strncat(buf, ")", sizeof(buf) - strlen(buf) - 1);
    snprintf(out, outlen, "%s", buf);
}

static int do_list(const char *kitfile)
{
    kit_reader_t r;
    if (reader_open(&r, kitfile) < 0)
        return 1;

    char name[OVMX_KIT_NAME_MAX + 1], prod[OVMX_KIT_PRODUCER_MAX + 1],
         ver[OVMX_KIT_VERSION_MAX + 1];
    snprintf(name, sizeof(name), "%s", r.hdr.kh_product_name);
    snprintf(prod, sizeof(prod), "%s", r.hdr.kh_producer);
    snprintf(ver, sizeof(ver), "%s", r.hdr.kh_product_version);

    time_t bt = (time_t)r.hdr.kh_build_time;
    char timebuf[64];
    struct tm tmv;
    gmtime_r(&bt, &tmv);
    strftime(timebuf, sizeof(timebuf), "%d-%b-%Y %H:%M:%S UTC", &tmv);

    printf("Product:    %s\n", name);
    printf("Producer:   %s\n", prod);
    printf("Version:    %s\n", ver);
    printf("Built:      %s\n", timebuf);
    printf("Files:      %u\n\n", r.hdr.kh_file_count);

    struct ovmx_kit_entry *entries;
    if (read_entries(&r, &entries) < 0) { ovmx_kit_reader_close(&r); return 1; }

    for (uint32_t i = 0; i < r.hdr.kh_file_count; i++) {
        struct ovmx_kit_entry *e = &entries[i];
        char pt[64];
        protection_to_text(e->ke_protection, pt, sizeof(pt));
        printf("%-40s %10llu bytes  [%u,%u]  %s\n",
               e->ke_filespec, (unsigned long long)e->ke_size,
               e->ke_uic_group, e->ke_uic_member, pt);
    }

    free(entries);
    ovmx_kit_reader_close(&r);
    return 0;
}

static int do_extract(const char *kitfile, const char *outdir)
{
    kit_reader_t r;
    if (reader_open(&r, kitfile) < 0)
        return 1;

    struct ovmx_kit_entry *entries;
    if (read_entries(&r, &entries) < 0) { ovmx_kit_reader_close(&r); return 1; }

    int rc = 0;
    for (uint32_t i = 0; i < r.hdr.kh_file_count; i++) {
        struct ovmx_kit_entry *e = &entries[i];
        char rel[4096];
        if (ovmx_kit_reader_relpath(e->ke_filespec, rel, sizeof(rel)) != 0) {
            fprintf(stderr, "%%KITPACK-E-BADSPEC, malformed filespec: %s\n",
                    e->ke_filespec);
            rc = 1; break;
        }

        char path[8192];
        snprintf(path, sizeof(path), "%s/%s", outdir, rel);

        /* mkdir -p the parent directory chain. */
        char parent[8192];
        snprintf(parent, sizeof(parent), "%s", path);
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = '\0';
            char accum[8192] = "";
            char *tok, *save;
            char tmp[8192];
            snprintf(tmp, sizeof(tmp), "%s", parent);
            for (tok = strtok_r(tmp, "/", &save); tok;
                 tok = strtok_r(NULL, "/", &save)) {
                strncat(accum, "/", sizeof(accum) - strlen(accum) - 1);
                strncat(accum, tok, sizeof(accum) - strlen(accum) - 1);
                if (mkdir(accum, 0755) != 0 && errno != EEXIST) {
                    fprintf(stderr, "%%KITPACK-F-MKDIR, cannot create %s: %s\n",
                            accum, strerror(errno));
                    rc = 1; break;
                }
            }
            if (rc) break;
        }

        int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) {
            fprintf(stderr, "%%KITPACK-F-CREATE, cannot write %s: %s\n",
                    path, strerror(errno));
            rc = 1; break;
        }

        uint8_t *buf = NULL;
        int rrc = ovmx_kit_reader_read_file(&r, e, &buf);
        if (rrc != OVMX_KIT_READER_OK) {
            if (rrc == OVMX_KIT_READER_ERR_CHKSUM)
                fprintf(stderr, "%%KITPACK-F-CHKSUM, content checksum mismatch: %s\n",
                        e->ke_filespec);
            else if (rrc == OVMX_KIT_READER_ERR_NOMEM)
                fprintf(stderr, "%%KITPACK-F-NOMEM, out of memory\n");
            else
                fprintf(stderr, "%%KITPACK-F-READ, short read for %s\n", e->ke_filespec);
            close(out); rc = 1; break;
        }
        if (buf) {
            if (write(out, buf, e->ke_size) != (ssize_t)e->ke_size) {
                fprintf(stderr, "%%KITPACK-F-WRITE, write failed for %s\n", path);
                free(buf); close(out); rc = 1; break;
            }
            free(buf);
        }
        close(out);
    }

    if (rc == 0)
        printf("%%KITPACK-I-EXTRACT, extracted %u files from %s -> %s\n",
               r.hdr.kh_file_count, kitfile, outdir);

    free(entries);
    ovmx_kit_reader_close(&r);
    return rc;
}

/* ================================================================
 * main
 * ================================================================ */

static void usage(void)
{
    fprintf(stderr,
        "Usage:\n"
        "  ovmx_kit_pack pack    <kit-file> <staging-dir> <product-name> [producer]\n"
        "  ovmx_kit_pack list    <kit-file>\n"
        "  ovmx_kit_pack extract <kit-file> <output-dir>\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(); return 1; }

    if (strcmp(argv[1], "pack") == 0) {
        if (argc < 5 || argc > 6) { usage(); return 1; }
        const char *producer = (argc == 6) ? argv[5] : NULL;
        return do_pack(argv[2], argv[3], argv[4], producer);
    } else if (strcmp(argv[1], "list") == 0) {
        if (argc != 3) { usage(); return 1; }
        return do_list(argv[2]);
    } else if (strcmp(argv[1], "extract") == 0) {
        if (argc != 4) { usage(); return 1; }
        return do_extract(argv[2], argv[3]);
    }

    usage();
    return 1;
}
