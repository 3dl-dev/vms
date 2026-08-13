/*
 * librarian.c - LIBRARIAN.EXE, the OVMX object-library utility.
 *
 * Bead vms-ca9 (self-host spine #3, epic vms-59a). LIBRARIAN.EXE produces and
 * maintains OVMX object libraries (.OLB) that LINK.EXE consumes to resolve
 * undefined symbols by pulling the needed members -- the object-library half of
 * the OVMX-native toolchain (TCC.EXE -> LIBRARIAN.EXE -> LINK.EXE -> IMGACT.EXE).
 *
 * It is a SYS$SYSTEM: image, installed alongside LINK.EXE and OVMXDUMP. Like
 * those two it is a bootstrap host tool of the toolchain (see
 * src/vmslink/CMakeLists.txt) -- it reads/writes .OLB and .OBJ files, it is not
 * itself a VMS runtime image, and it runs during `cmake --build` and under the
 * DCL LIBRARY command's utility path.
 *
 * .OLB FORMAT (Rule 8): the .OLB container is a standard `ar` archive of OVMX
 * .OBJ (ELF) members -- an OVMX-labeled design choice, NOT the VMS-authentic LBR
 * byte layout (which VSI does not publish). The reader/writer live in the shared
 * header src/vmslink/include/ovmx_olb.h; see its banner and
 * docs/design-self-host-mmk-spine.md section 3.
 *
 * COMMAND SURFACE (public VSI LIBRARIAN / DCL LIBRARY utility surface, argv
 * form -- the same direct-argv convention LINK.EXE uses, since the CLI$/CLD
 * callable path is a separate prerequisite, docs/design-self-host-mmk-spine.md
 * section 1.3 P1):
 *
 *   LIBRARIAN /CREATE  lib.olb [file.obj ...]   create (and insert any files)
 *   LIBRARIAN /INSERT  lib.olb  file.obj ...     insert/replace object modules
 *   LIBRARIAN /DELETE  lib.olb  module ...       delete named modules
 *   LIBRARIAN /LIST    lib.olb                    list module directory
 *   LIBRARIAN /EXTRACT lib.olb  module [-o out]   extract a module to a file
 *
 * A module name is derived from the .OBJ file's basename, extension stripped and
 * upper-cased (an OVMX choice: an OVMX .OBJ is an ELF object with no embedded VMS
 * module-name field). All operations return VMS status codes internally; the
 * process exit is 0 on success / 1 on failure so `cmake --build`, MMS/MMK and
 * ctest see conventional shell status.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "ovmx_olb.h"
#include "ssdef.h"

/* Derive a VMS-style module name from a file path: basename, drop the last
 * extension, upper-case, clamp to OLB_NAME_MAX. */
static void module_name_from_path(const char *path, char *out, size_t out_sz)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ']' || *p == ':')
            base = p + 1;
    size_t n = 0;
    for (const char *p = base; *p && *p != '.' && n + 1 < out_sz; p++)
        out[n++] = (char)toupper((unsigned char)*p);
    out[n] = '\0';
}

/* Read a whole .OBJ file into a fresh buffer. */
static unsigned char *read_obj(const char *path, size_t *len)
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
    *len = (size_t)sz;
    return buf;
}

/* Append or replace a member in the array (case-insensitive by name). Takes
 * ownership of `data` on success. Returns a VMS status code. */
static int members_upsert(struct olb_member **members, uint32_t *count,
                          uint32_t *cap, const char *name,
                          unsigned char *data, size_t len, int *replaced)
{
    int idx = olb_find(*members, *count, name);
    if (idx >= 0) {
        free((*members)[idx].data);
        (*members)[idx].data = data;
        (*members)[idx].len = len;
        *replaced = 1;
        return SS$_NORMAL;
    }
    if (*count >= *cap) {
        uint32_t ncap = *cap ? *cap * 2 : 16;
        struct olb_member *na = (struct olb_member *)
            realloc(*members, (size_t)ncap * sizeof(**members));
        if (!na) return SS$_INSFMEM;
        *members = na; *cap = ncap;
    }
    memset(&(*members)[*count], 0, sizeof((*members)[0]));
    olb_setname((*members)[*count].name, name);
    (*members)[*count].data = data;
    (*members)[*count].len = len;
    (*count)++;
    *replaced = 0;
    return SS$_NORMAL;
}

/* Load an existing library into a growable member array (cap tracked). */
static int load_library(const char *path, struct olb_member **members,
                        uint32_t *count, uint32_t *cap)
{
    int rc = olb_read(path, members, count);
    if (rc == OLB_OK) { *cap = *count; return SS$_NORMAL; }
    if (rc == OLB_ERR_OPEN) {
        fprintf(stderr, "%%LIBRAR-E-OPENIN, error opening %s as input\n", path);
        return SS$_NOSUCHFILE;
    }
    if (rc == OLB_ERR_FORMAT) {
        fprintf(stderr, "%%LIBRAR-E-ILLFORMAT, %s is not a valid object library\n", path);
        return SS$_ABORT;
    }
    fprintf(stderr, "%%LIBRAR-E-READERR, error reading %s\n", path);
    return SS$_ABORT;
}

static int insert_files(struct olb_member **members, uint32_t *count,
                        uint32_t *cap, char **files, int nfiles)
{
    for (int i = 0; i < nfiles; i++) {
        size_t len = 0;
        unsigned char *data = read_obj(files[i], &len);
        if (!data) {
            fprintf(stderr, "%%LIBRAR-E-OPENIN, error opening %s as input\n", files[i]);
            return SS$_NOSUCHFILE;
        }
        char name[OLB_NAME_MAX + 1];
        module_name_from_path(files[i], name, sizeof name);
        if (name[0] == '\0') {
            free(data);
            fprintf(stderr, "%%LIBRAR-E-BADNAME, cannot derive a module name from %s\n", files[i]);
            return SS$_BADPARAM;
        }
        int replaced = 0;
        int st = members_upsert(members, count, cap, name, data, len, &replaced);
        if (!$VMS_STATUS_SUCCESS(st)) { free(data); return st; }
        printf("%%LIBRAR-S-%s, module %s %s\n",
               replaced ? "REPLACED" : "INSERTED", name,
               replaced ? "replaced" : "inserted");
    }
    return SS$_NORMAL;
}

static int cmd_create(char *lib, char **files, int nfiles)
{
    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = insert_files(&members, &count, &cap, files, nfiles);
    if (!$VMS_STATUS_SUCCESS(st)) { olb_free(members, count); return st; }
    int rc = olb_write(lib, members, count);
    olb_free(members, count);
    if (rc != OLB_OK) {
        fprintf(stderr, "%%LIBRAR-E-OPENOUT, error creating library %s\n", lib);
        return SS$_FILACCERR;
    }
    printf("%%LIBRAR-S-CREATED, object library %s created (%u module%s)\n",
           lib, count, count == 1 ? "" : "s");
    return SS$_NORMAL;
}

static int cmd_insert(char *lib, char **files, int nfiles)
{
    if (nfiles < 1) {
        fprintf(stderr, "%%LIBRAR-E-NOFILES, no object file(s) specified to insert\n");
        return SS$_BADPARAM;
    }
    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = load_library(lib, &members, &count, &cap);
    if (!$VMS_STATUS_SUCCESS(st)) return st;
    st = insert_files(&members, &count, &cap, files, nfiles);
    if (!$VMS_STATUS_SUCCESS(st)) { olb_free(members, count); return st; }
    int rc = olb_write(lib, members, count);
    olb_free(members, count);
    if (rc != OLB_OK) {
        fprintf(stderr, "%%LIBRAR-E-OPENOUT, error writing library %s\n", lib);
        return SS$_FILACCERR;
    }
    return SS$_NORMAL;
}

static int cmd_delete(char *lib, char **names, int nnames)
{
    if (nnames < 1) {
        fprintf(stderr, "%%LIBRAR-E-NOMODS, no module name(s) specified to delete\n");
        return SS$_BADPARAM;
    }
    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = load_library(lib, &members, &count, &cap);
    if (!$VMS_STATUS_SUCCESS(st)) return st;

    for (int i = 0; i < nnames; i++) {
        char want[OLB_NAME_MAX + 1];
        size_t j = 0;
        for (const char *p = names[i]; *p && j + 1 < sizeof want; p++)
            want[j++] = (char)toupper((unsigned char)*p);
        want[j] = '\0';
        int idx = olb_find(members, count, want);
        if (idx < 0) {
            fprintf(stderr, "%%LIBRAR-E-MODNOTFND, module %s not found in %s\n", want, lib);
            olb_free(members, count);
            return SS$_NOSUCHFILE;
        }
        free(members[idx].data);
        for (uint32_t k = (uint32_t)idx; k + 1 < count; k++)
            members[k] = members[k + 1];
        count--;
        printf("%%LIBRAR-S-DELETED, module %s deleted\n", want);
    }

    int rc = olb_write(lib, members, count);
    olb_free(members, count);
    if (rc != OLB_OK) {
        fprintf(stderr, "%%LIBRAR-E-OPENOUT, error writing library %s\n", lib);
        return SS$_FILACCERR;
    }
    return SS$_NORMAL;
}

static int cmd_list(char *lib)
{
    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = load_library(lib, &members, &count, &cap);
    if (!$VMS_STATUS_SUCCESS(st)) return st;

    printf("\nDirectory of OBJECT library %s\n\n", lib);
    if (count == 0) {
        printf("  (empty library)\n");
    } else {
        for (uint32_t i = 0; i < count; i++)
            printf("  %-31s  %zu bytes\n", members[i].name, members[i].len);
    }
    printf("\n%u module%s in library\n\n", count, count == 1 ? "" : "s");
    olb_free(members, count);
    return SS$_NORMAL;
}

static int cmd_extract(char *lib, const char *modname, const char *outfile)
{
    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = load_library(lib, &members, &count, &cap);
    if (!$VMS_STATUS_SUCCESS(st)) return st;

    char want[OLB_NAME_MAX + 1];
    size_t j = 0;
    for (const char *p = modname; *p && j + 1 < sizeof want; p++)
        want[j++] = (char)toupper((unsigned char)*p);
    want[j] = '\0';

    int idx = olb_find(members, count, want);
    if (idx < 0) {
        fprintf(stderr, "%%LIBRAR-E-MODNOTFND, module %s not found in %s\n", want, lib);
        olb_free(members, count);
        return SS$_NOSUCHFILE;
    }

    char defname[OLB_NAME_MAX + 8];
    if (!outfile) {
        snprintf(defname, sizeof defname, "%s.OBJ", want);
        outfile = defname;
    }
    FILE *out = fopen(outfile, "wb");
    if (!out) {
        fprintf(stderr, "%%LIBRAR-E-OPENOUT, error creating %s\n", outfile);
        olb_free(members, count);
        return SS$_FILACCERR;
    }
    int rc = OLB_OK;
    if (members[idx].len &&
        fwrite(members[idx].data, 1, members[idx].len, out) != members[idx].len)
        rc = OLB_ERR_IO;
    if (fclose(out) != 0) rc = OLB_ERR_IO;
    olb_free(members, count);
    if (rc != OLB_OK) {
        fprintf(stderr, "%%LIBRAR-E-WRITEERR, error writing %s\n", outfile);
        return SS$_ABORT;
    }
    printf("%%LIBRAR-S-EXTRACTED, module %s extracted to %s\n", want, outfile);
    return SS$_NORMAL;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: LIBRARIAN /CREATE  lib.olb [file.obj ...]\n"
        "       LIBRARIAN /INSERT  lib.olb  file.obj ...\n"
        "       LIBRARIAN /DELETE  lib.olb  module ...\n"
        "       LIBRARIAN /LIST    lib.olb\n"
        "       LIBRARIAN /EXTRACT lib.olb  module [-o outfile]\n");
}

int main(int argc, char **argv)
{
    if (argc < 3) { usage(); return 1; }

    const char *op = argv[1];
    char *lib = argv[2];
    char **rest = &argv[3];
    int nrest = argc - 3;

    int st;
    if (strcasecmp(op, "/CREATE") == 0 || strcasecmp(op, "-c") == 0) {
        st = cmd_create(lib, rest, nrest);
    } else if (strcasecmp(op, "/INSERT") == 0 || strcasecmp(op, "-r") == 0) {
        st = cmd_insert(lib, rest, nrest);
    } else if (strcasecmp(op, "/DELETE") == 0 || strcasecmp(op, "-d") == 0) {
        st = cmd_delete(lib, rest, nrest);
    } else if (strcasecmp(op, "/LIST") == 0 || strcasecmp(op, "-t") == 0) {
        st = cmd_list(lib);
    } else if (strcasecmp(op, "/EXTRACT") == 0 || strcasecmp(op, "-x") == 0) {
        const char *mod = nrest >= 1 ? rest[0] : NULL;
        const char *out = NULL;
        for (int i = 1; i + 1 < nrest; i++)
            if (strcmp(rest[i], "-o") == 0) out = rest[i + 1];
        if (!mod) { fprintf(stderr, "%%LIBRAR-E-NOMOD, no module name given\n"); return 1; }
        st = cmd_extract(lib, mod, out);
    } else {
        usage();
        return 1;
    }

    return $VMS_STATUS_SUCCESS(st) ? 0 : 1;
}
