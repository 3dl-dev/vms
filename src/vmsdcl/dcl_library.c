/*
 * dcl_library.c - LIBRARY command implementation
 *
 * Manages VMS text libraries (.TLB), help libraries (.HLB), and OBJECT
 * libraries (.OLB).
 * Supports /CREATE, /INSERT, /EXTRACT, /LIST (and, for .OLB, /DELETE).
 *
 * TEXT/HELP library file format (OVMX-specific "LBRO", not VMS binary-compatible):
 *   Header:  magic(4) type(4) module_count(4) reserved(4)
 *   Modules: name(32) offset(4) length(4) per entry
 *   Data:    raw file contents concatenated
 *
 * OBJECT library (.OLB) format: a real object library that LINK.EXE consumes to
 * resolve undefined symbols by pulling members. It is a standard `ar` archive of
 * .OBJ (ELF) members — an OVMX-labeled design choice (Rule 8), NOT the
 * unpublished VMS LBR byte layout. The reader/writer are shared with LIBRARIAN.EXE
 * via src/vmslink/include/ovmx_olb.h. See docs/design-self-host-mmk-spine.md
 * section 3. (vms-ca9)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "ssdef.h"
#include "ovmx_olb.h"

/* External functions */
extern void dcl_error(const char *facility, int severity, const char *ident,
                      const char *fmt, ...);
extern int dcl_resolve_path(struct dcl_context *ctx, const char *spec,
                            char *linux_path, size_t path_size);

/* Library file magic */
#define LBR_MAGIC       0x4C42524F  /* "LBRO" - Library OVMX */

/* Library types */
#define LBR_TYPE_TEXT   1
#define LBR_TYPE_HELP   2
#define LBR_TYPE_OBJECT 3

/* Limits */
#define LBR_MAX_MODULES 1024
#define LBR_NAME_LEN    32

/* On-disk header (16 bytes) */
struct lbr_header {
    uint32_t magic;
    uint32_t type;
    uint32_t module_count;
    uint32_t reserved;
};

/* On-disk module entry (40 bytes) */
struct lbr_module {
    char     name[LBR_NAME_LEN];
    uint32_t offset;
    uint32_t length;
};

/*
 * Determine library type from qualifiers or file extension.
 */
static int lbr_get_type(struct dcl_command *cmd, const char *path)
{
    if (dcl_has_qualifier(cmd, "HELP"))
        return LBR_TYPE_HELP;
    if (dcl_has_qualifier(cmd, "OBJECT"))
        return LBR_TYPE_OBJECT;

    /* Infer from extension */
    const char *dot = strrchr(path, '.');
    if (dot) {
        if (strcasecmp(dot, ".HLB") == 0) return LBR_TYPE_HELP;
        if (strcasecmp(dot, ".OLB") == 0) return LBR_TYPE_OBJECT;
    }
    return LBR_TYPE_TEXT;
}

static const char *lbr_type_name(int type)
{
    switch (type) {
    case LBR_TYPE_TEXT:   return "text";
    case LBR_TYPE_HELP:   return "help";
    case LBR_TYPE_OBJECT: return "object";
    default:              return "unknown";
    }
}

/*
 * LIBRARY /CREATE - create an empty library file
 */
static int lbr_create(struct dcl_context *ctx, struct dcl_command *cmd)
{
    if (cmd->param_count < 1) {
        dcl_error("LIBRARIAN", 2, "NOLIB",
                  "no library file specified");
        return SS$_BADPARAM;
    }

    char lib_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], lib_path, sizeof(lib_path));

    int type = lbr_get_type(cmd, cmd->params[0]);

    FILE *fp = fopen(lib_path, "wb");
    if (!fp) {
        dcl_error("LIBRARIAN", 2, "OPENOUT",
                  "error creating library - %s", cmd->params[0]);
        return SS$_FILACCERR;
    }

    struct lbr_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = LBR_MAGIC;
    hdr.type = (uint32_t)type;
    hdr.module_count = 0;
    hdr.reserved = 0;

    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1) {
        dcl_error("LIBRARIAN", 2, "WRITEERR", "error writing library header");
        fclose(fp);
        return SS$_ABORT;
    }
    fclose(fp);

    printf("%%LIBRARIAN-S-CREATED, %s library %s created\n",
           lbr_type_name(type), cmd->params[0]);
    return SS$_NORMAL;
}

/*
 * Read library header and module table from file.
 * Returns file pointer positioned after module table, or NULL on error.
 * Caller must fclose the returned FILE*.
 */
static FILE *lbr_open(const char *lib_path, struct lbr_header *hdr,
                       struct lbr_module **modules, const char *vms_name)
{
    FILE *fp = fopen(lib_path, "rb");
    if (!fp) {
        dcl_error("LIBRARIAN", 2, "OPENIN",
                  "error opening library - %s", vms_name);
        return NULL;
    }

    if (fread(hdr, sizeof(*hdr), 1, fp) != 1 || hdr->magic != LBR_MAGIC) {
        dcl_error("LIBRARIAN", 2, "ILLHDR",
                  "not a valid OVMX library - %s", vms_name);
        fclose(fp);
        return NULL;
    }

    if (hdr->module_count > LBR_MAX_MODULES) {
        dcl_error("LIBRARIAN", 2, "TOOMANYMOD",
                  "library has too many modules - %s", vms_name);
        fclose(fp);
        return NULL;
    }

    *modules = NULL;
    if (hdr->module_count > 0) {
        *modules = calloc(hdr->module_count, sizeof(struct lbr_module));
        if (!*modules) {
            dcl_error("LIBRARIAN", 4, "INSFMEM", "insufficient memory");
            fclose(fp);
            return NULL;
        }
        if (fread(*modules, sizeof(struct lbr_module), hdr->module_count, fp)
                != hdr->module_count) {
            dcl_error("LIBRARIAN", 2, "READERR",
                      "error reading module table - %s", vms_name);
            free(*modules);
            *modules = NULL;
            fclose(fp);
            return NULL;
        }
    }

    return fp;
}

/*
 * LIBRARY /INSERT - insert a module into a library
 *
 * Usage: LIBRARY library module-name file
 *   or:  LIBRARY /INSERT library module-name file
 */
static int lbr_insert(struct dcl_context *ctx, struct dcl_command *cmd)
{
    if (cmd->param_count < 3) {
        dcl_error("LIBRARIAN", 2, "BADPARAM",
                  "usage: LIBRARY library-file module-name source-file");
        return SS$_BADPARAM;
    }

    char lib_path[1024], src_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], lib_path, sizeof(lib_path));
    dcl_resolve_path(ctx, cmd->params[2], src_path, sizeof(src_path));

    /* Read source file */
    FILE *src = fopen(src_path, "rb");
    if (!src) {
        dcl_error("LIBRARIAN", 2, "OPENIN",
                  "error opening source file - %s", cmd->params[2]);
        return SS$_NOSUCHFILE;
    }

    fseek(src, 0, SEEK_END);
    long src_len = ftell(src);
    if (src_len < 0) {
        dcl_error("LIBRARIAN", 2, "READERR",
                  "error determining file size - %s", cmd->params[2]);
        fclose(src);
        return SS$_ABORT;
    }
    fseek(src, 0, SEEK_SET);

    char *src_data = malloc((size_t)src_len + 1);
    if (!src_data) {
        dcl_error("LIBRARIAN", 4, "INSFMEM", "insufficient memory");
        fclose(src);
        return SS$_ABORT;
    }
    if (src_len > 0) {
        if ((long)fread(src_data, 1, (size_t)src_len, src) != src_len) {
            dcl_error("LIBRARIAN", 2, "READERR",
                      "error reading source file - %s", cmd->params[2]);
            free(src_data);
            fclose(src);
            return SS$_ABORT;
        }
    }
    fclose(src);

    /* Read existing library */
    struct lbr_header hdr;
    struct lbr_module *modules = NULL;
    FILE *fp = lbr_open(lib_path, &hdr, &modules, cmd->params[0]);
    if (!fp) {
        free(src_data);
        return SS$_NOSUCHFILE;
    }

    /* Read existing data section */
    long data_start = (long)(sizeof(struct lbr_header) +
                     hdr.module_count * sizeof(struct lbr_module));
    fseek(fp, 0, SEEK_END);
    long file_end = ftell(fp);
    if (file_end < 0) {
        dcl_error("LIBRARIAN", 2, "READERR",
                  "error determining library size - %s", cmd->params[0]);
        free(src_data);
        free(modules);
        fclose(fp);
        return SS$_ABORT;
    }
    long data_len = file_end - data_start;
    char *data = NULL;

    if (data_len > 0) {
        data = malloc((size_t)data_len);
        if (!data) {
            dcl_error("LIBRARIAN", 4, "INSFMEM", "insufficient memory");
            free(src_data);
            free(modules);
            fclose(fp);
            return SS$_ABORT;
        }
        fseek(fp, data_start, SEEK_SET);
        if ((long)fread(data, 1, (size_t)data_len, fp) != data_len) {
            free(data);
            free(src_data);
            free(modules);
            fclose(fp);
            return SS$_ABORT;
        }
    }
    fclose(fp);

    /* Prepare module name (uppercase, zero-padded) */
    char mod_name[LBR_NAME_LEN];
    memset(mod_name, 0, sizeof(mod_name));
    strncpy(mod_name, cmd->params[1], LBR_NAME_LEN - 1);
    for (int i = 0; mod_name[i]; i++)
        mod_name[i] = (char)toupper((unsigned char)mod_name[i]);

    /* Check for existing module with same name */
    int replacing = 0;
    for (uint32_t i = 0; i < hdr.module_count; i++) {
        if (strncasecmp(modules[i].name, mod_name, LBR_NAME_LEN) == 0) {
            replacing = 1;
            break;
        }
    }

    /* Rebuild library */
    uint32_t new_count = replacing ? hdr.module_count : hdr.module_count + 1;
    if (new_count > LBR_MAX_MODULES) {
        dcl_error("LIBRARIAN", 2, "TOOMANYMOD",
                  "library module limit exceeded");
        free(data);
        free(src_data);
        free(modules);
        return SS$_ABORT;
    }

    struct lbr_module *new_modules = calloc(new_count, sizeof(struct lbr_module));
    if (!new_modules) {
        free(data);
        free(src_data);
        free(modules);
        return SS$_ABORT;
    }

    /* Collect data for all kept modules + new one */
    size_t total_data = 0;
    size_t data_cap = (size_t)data_len + (size_t)src_len + 1024;
    char *new_data = malloc(data_cap);
    if (!new_data) {
        free(new_modules);
        free(data);
        free(src_data);
        free(modules);
        return SS$_ABORT;
    }

    uint32_t idx = 0;
    uint32_t data_base = (uint32_t)(sizeof(struct lbr_header) +
                         new_count * sizeof(struct lbr_module));

    /* Copy existing modules (skip the one being replaced) */
    for (uint32_t i = 0; i < hdr.module_count; i++) {
        if (replacing && strncasecmp(modules[i].name, mod_name, LBR_NAME_LEN) == 0)
            continue;

        uint32_t old_data_base = (uint32_t)(sizeof(struct lbr_header) +
                                 hdr.module_count * sizeof(struct lbr_module));
        uint32_t off_in_data = modules[i].offset - old_data_base;

        memcpy(new_modules[idx].name, modules[i].name, LBR_NAME_LEN);
        new_modules[idx].offset = data_base + (uint32_t)total_data;
        new_modules[idx].length = modules[i].length;

        if (data && off_in_data + modules[i].length <= (uint32_t)data_len) {
            memcpy(new_data + total_data, data + off_in_data, modules[i].length);
        }
        total_data += modules[i].length;
        idx++;
    }

    /* Add new module */
    memcpy(new_modules[idx].name, mod_name, LBR_NAME_LEN);
    new_modules[idx].offset = data_base + (uint32_t)total_data;
    new_modules[idx].length = (uint32_t)src_len;
    memcpy(new_data + total_data, src_data, (size_t)src_len);
    total_data += (size_t)src_len;
    idx++;

    /* Write rebuilt library */
    fp = fopen(lib_path, "wb");
    if (!fp) {
        dcl_error("LIBRARIAN", 2, "OPENOUT",
                  "error writing library - %s", cmd->params[0]);
        free(new_data);
        free(new_modules);
        free(data);
        free(src_data);
        free(modules);
        return SS$_FILACCERR;
    }

    struct lbr_header new_hdr;
    memset(&new_hdr, 0, sizeof(new_hdr));
    new_hdr.magic = LBR_MAGIC;
    new_hdr.type = hdr.type;
    new_hdr.module_count = idx;
    new_hdr.reserved = 0;

    if (fwrite(&new_hdr, sizeof(new_hdr), 1, fp) != 1 ||
        fwrite(new_modules, sizeof(struct lbr_module), idx, fp) != idx ||
        (total_data > 0 && fwrite(new_data, 1, total_data, fp) != total_data)) {
        dcl_error("LIBRARIAN", 2, "WRITEERR",
                  "error writing library - %s", cmd->params[0]);
        fclose(fp);
        free(new_data);
        free(new_modules);
        free(data);
        free(src_data);
        free(modules);
        return SS$_ABORT;
    }
    fclose(fp);

    printf("%%LIBRARIAN-S-%s, module %s %s\n",
           replacing ? "REPLACED" : "INSERTED",
           mod_name,
           replacing ? "replaced" : "inserted");

    free(new_data);
    free(new_modules);
    free(data);
    free(src_data);
    free(modules);
    return SS$_NORMAL;
}

/*
 * LIBRARY /LIST - list modules in a library
 */
static int lbr_list(struct dcl_context *ctx, struct dcl_command *cmd)
{
    if (cmd->param_count < 1) {
        dcl_error("LIBRARIAN", 2, "NOLIB",
                  "no library file specified");
        return SS$_BADPARAM;
    }

    char lib_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], lib_path, sizeof(lib_path));

    struct lbr_header hdr;
    struct lbr_module *modules = NULL;
    FILE *fp = lbr_open(lib_path, &hdr, &modules, cmd->params[0]);
    if (!fp) return SS$_NOSUCHFILE;
    fclose(fp);

    printf("Directory of %s LIBRARY %s\n\n",
           lbr_type_name(hdr.type), cmd->params[0]);

    if (hdr.module_count == 0) {
        printf("  (empty library)\n");
    } else {
        for (uint32_t i = 0; i < hdr.module_count; i++) {
            char name[LBR_NAME_LEN + 1];
            memcpy(name, modules[i].name, LBR_NAME_LEN);
            name[LBR_NAME_LEN] = '\0';
            for (int j = LBR_NAME_LEN - 1; j >= 0 && (name[j] == '\0' || name[j] == ' '); j--)
                name[j] = '\0';
            printf("  %-32s  %u bytes\n", name, modules[i].length);
        }
    }
    printf("\n%u module%s in library\n",
           hdr.module_count, hdr.module_count == 1 ? "" : "s");

    free(modules);
    return SS$_NORMAL;
}

/*
 * LIBRARY /EXTRACT - extract a module from a library
 */
static int lbr_extract(struct dcl_context *ctx, struct dcl_command *cmd)
{
    if (cmd->param_count < 1) {
        dcl_error("LIBRARIAN", 2, "NOLIB",
                  "no library file specified");
        return SS$_BADPARAM;
    }

    const char *extract_name = dcl_qualifier_value(cmd, "EXTRACT");
    if (!extract_name || !extract_name[0]) {
        dcl_error("LIBRARIAN", 2, "NOMODNAM",
                  "no module name specified with /EXTRACT");
        return SS$_BADPARAM;
    }

    char lib_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], lib_path, sizeof(lib_path));

    struct lbr_header hdr;
    struct lbr_module *modules = NULL;
    FILE *fp = lbr_open(lib_path, &hdr, &modules, cmd->params[0]);
    if (!fp) return SS$_NOSUCHFILE;

    /* Find the module (case-insensitive) */
    int found = -1;
    for (uint32_t i = 0; i < hdr.module_count; i++) {
        if (strncasecmp(modules[i].name, extract_name, LBR_NAME_LEN) == 0) {
            found = (int)i;
            break;
        }
    }

    if (found < 0) {
        dcl_error("LIBRARIAN", 2, "KEYNOTFND",
                  "module %s not found in library", extract_name);
        free(modules);
        fclose(fp);
        return SS$_NOSUCHFILE;
    }

    /* Read and output module data */
    fseek(fp, (long)modules[found].offset, SEEK_SET);
    uint32_t remaining = modules[found].length;
    char buf[4096];

    /* Determine output: /OUTPUT=file or stdout */
    const char *output_file = dcl_qualifier_value(cmd, "OUTPUT");
    FILE *out = stdout;
    if (output_file && output_file[0]) {
        char out_path[1024];
        dcl_resolve_path(ctx, output_file, out_path, sizeof(out_path));
        out = fopen(out_path, "w");
        if (!out) {
            dcl_error("LIBRARIAN", 2, "OPENOUT",
                      "error creating output file - %s", output_file);
            free(modules);
            fclose(fp);
            return SS$_FILACCERR;
        }
    }

    while (remaining > 0) {
        size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
        size_t got = fread(buf, 1, chunk, fp);
        if (got == 0) break;
        fwrite(buf, 1, got, out);
        remaining -= (uint32_t)got;
    }

    if (out != stdout) {
        fclose(out);
        printf("%%LIBRARIAN-S-EXTRACTED, module %s extracted to %s\n",
               extract_name, output_file);
    }

    free(modules);
    fclose(fp);
    return SS$_NORMAL;
}

/* ==========================================================================
 * OBJECT library (.OLB) operations — real object-library semantics (vms-ca9).
 *
 * Unlike the TEXT/HELP "LBRO" format above, an OBJECT library is a standard `ar`
 * archive of .OBJ (ELF) members (ovmx_olb.h) so that LINK.EXE resolves undefined
 * symbols by pulling members. These share the exact byte layout LIBRARIAN.EXE
 * writes. Module names are derived from the .OBJ basename (uppercased, extension
 * stripped) — an OVMX choice, since an OVMX .OBJ carries no VMS module-name field.
 * ========================================================================== */

/* Derive a VMS-style module name from a VMS/Unix file spec. */
static void olb_module_name(const char *spec, char *out, size_t out_sz)
{
    const char *base = spec;
    for (const char *p = spec; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ']' || *p == ':' || *p == '>')
            base = p + 1;
    size_t n = 0;
    for (const char *p = base; *p && *p != '.' && *p != ';' && n + 1 < out_sz; p++)
        out[n++] = (char)toupper((unsigned char)*p);
    out[n] = '\0';
}

/* Read a whole file (.OBJ) into a fresh buffer. */
static unsigned char *olb_slurp_obj(const char *linux_path, size_t *len)
{
    FILE *fp = fopen(linux_path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    unsigned char *buf = malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(fp); return NULL; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);
    *len = (size_t)sz;
    return buf;
}

/* Insert (or replace) the .OBJ files params[first..] into an in-memory member
 * array. Takes ownership of allocated data. Returns a VMS status code. */
static int olb_insert_files(struct dcl_context *ctx, struct dcl_command *cmd,
                            int first, struct olb_member **members,
                            uint32_t *count, uint32_t *cap)
{
    for (int i = first; i < cmd->param_count; i++) {
        char src_path[1024];
        dcl_resolve_path(ctx, cmd->params[i], src_path, sizeof(src_path));
        size_t len = 0;
        unsigned char *data = olb_slurp_obj(src_path, &len);
        if (!data) {
            dcl_error("LIBRARIAN", 2, "OPENIN",
                      "error opening object file - %s", cmd->params[i]);
            return SS$_NOSUCHFILE;
        }
        char name[OLB_NAME_MAX + 1];
        olb_module_name(cmd->params[i], name, sizeof name);
        if (name[0] == '\0') {
            free(data);
            dcl_error("LIBRARIAN", 2, "BADNAME",
                      "cannot derive module name - %s", cmd->params[i]);
            return SS$_BADPARAM;
        }

        int replaced = 0;
        int idx = olb_find(*members, *count, name);
        if (idx >= 0) {
            free((*members)[idx].data);
            (*members)[idx].data = data;
            (*members)[idx].len = len;
            replaced = 1;
        } else {
            if (*count >= *cap) {
                uint32_t ncap = *cap ? *cap * 2 : 16;
                struct olb_member *na = realloc(*members,
                    (size_t)ncap * sizeof(**members));
                if (!na) { free(data); return SS$_INSFMEM; }
                *members = na; *cap = ncap;
            }
            memset(&(*members)[*count], 0, sizeof((*members)[0]));
            olb_setname((*members)[*count].name, name);
            (*members)[*count].data = data;
            (*members)[*count].len = len;
            (*count)++;
        }
        printf("%%LIBRARIAN-S-%s, module %s %s\n",
               replaced ? "REPLACED" : "INSERTED", name,
               replaced ? "replaced" : "inserted");
    }
    return SS$_NORMAL;
}

/* Load an existing .OLB into a growable member array; VMS status code. */
static int olb_load(struct dcl_context *ctx, const char *vms_name,
                    const char *lib_path, struct olb_member **members,
                    uint32_t *count, uint32_t *cap)
{
    (void)ctx;
    int rc = olb_read(lib_path, members, count);
    if (rc == OLB_OK) { *cap = *count; return SS$_NORMAL; }
    if (rc == OLB_ERR_OPEN) {
        dcl_error("LIBRARIAN", 2, "OPENIN", "error opening library - %s", vms_name);
        return SS$_NOSUCHFILE;
    }
    if (rc == OLB_ERR_FORMAT) {
        dcl_error("LIBRARIAN", 2, "ILLFORMAT",
                  "not a valid object library - %s", vms_name);
        return SS$_ABORT;
    }
    dcl_error("LIBRARIAN", 2, "READERR", "error reading library - %s", vms_name);
    return SS$_ABORT;
}

static int olb_create(struct dcl_context *ctx, struct dcl_command *cmd)
{
    char lib_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], lib_path, sizeof(lib_path));

    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = olb_insert_files(ctx, cmd, 1, &members, &count, &cap);
    if (!$VMS_STATUS_SUCCESS(st)) { olb_free(members, count); return st; }

    if (olb_write(lib_path, members, count) != OLB_OK) {
        olb_free(members, count);
        dcl_error("LIBRARIAN", 2, "OPENOUT",
                  "error creating library - %s", cmd->params[0]);
        return SS$_FILACCERR;
    }
    olb_free(members, count);
    printf("%%LIBRARIAN-S-CREATED, object library %s created\n", cmd->params[0]);
    return SS$_NORMAL;
}

static int olb_insert(struct dcl_context *ctx, struct dcl_command *cmd)
{
    if (cmd->param_count < 2) {
        dcl_error("LIBRARIAN", 2, "BADPARAM",
                  "usage: LIBRARY library.OLB object-file[ ...]");
        return SS$_BADPARAM;
    }
    char lib_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], lib_path, sizeof(lib_path));

    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = olb_load(ctx, cmd->params[0], lib_path, &members, &count, &cap);
    if (!$VMS_STATUS_SUCCESS(st)) return st;

    st = olb_insert_files(ctx, cmd, 1, &members, &count, &cap);
    if (!$VMS_STATUS_SUCCESS(st)) { olb_free(members, count); return st; }

    if (olb_write(lib_path, members, count) != OLB_OK) {
        olb_free(members, count);
        dcl_error("LIBRARIAN", 2, "OPENOUT",
                  "error writing library - %s", cmd->params[0]);
        return SS$_FILACCERR;
    }
    olb_free(members, count);
    return SS$_NORMAL;
}

static int olb_list_cmd(struct dcl_context *ctx, struct dcl_command *cmd)
{
    char lib_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], lib_path, sizeof(lib_path));

    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = olb_load(ctx, cmd->params[0], lib_path, &members, &count, &cap);
    if (!$VMS_STATUS_SUCCESS(st)) return st;

    printf("\nDirectory of OBJECT library %s\n\n", cmd->params[0]);
    if (count == 0) {
        printf("  (empty library)\n");
    } else {
        for (uint32_t i = 0; i < count; i++)
            printf("  %-31s  %zu bytes\n", members[i].name, members[i].len);
    }
    printf("\n%u module%s in library\n", count, count == 1 ? "" : "s");
    olb_free(members, count);
    return SS$_NORMAL;
}

static int olb_extract_cmd(struct dcl_context *ctx, struct dcl_command *cmd)
{
    const char *extract_name = dcl_qualifier_value(cmd, "EXTRACT");
    if (!extract_name || !extract_name[0]) {
        dcl_error("LIBRARIAN", 2, "NOMODNAM",
                  "no module name specified with /EXTRACT");
        return SS$_BADPARAM;
    }
    char lib_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], lib_path, sizeof(lib_path));

    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = olb_load(ctx, cmd->params[0], lib_path, &members, &count, &cap);
    if (!$VMS_STATUS_SUCCESS(st)) return st;

    char want[OLB_NAME_MAX + 1];
    olb_module_name(extract_name, want, sizeof want);
    int idx = olb_find(members, count, want);
    if (idx < 0) {
        dcl_error("LIBRARIAN", 2, "KEYNOTFND",
                  "module %s not found in library", extract_name);
        olb_free(members, count);
        return SS$_NOSUCHFILE;
    }

    const char *output_file = dcl_qualifier_value(cmd, "OUTPUT");
    FILE *out = stdout;
    if (output_file && output_file[0]) {
        char out_path[1024];
        dcl_resolve_path(ctx, output_file, out_path, sizeof(out_path));
        out = fopen(out_path, "wb");
        if (!out) {
            dcl_error("LIBRARIAN", 2, "OPENOUT",
                      "error creating output file - %s", output_file);
            olb_free(members, count);
            return SS$_FILACCERR;
        }
    }
    if (members[idx].len)
        fwrite(members[idx].data, 1, members[idx].len, out);
    if (out != stdout) {
        fclose(out);
        printf("%%LIBRARIAN-S-EXTRACTED, module %s extracted to %s\n",
               want, output_file);
    }
    olb_free(members, count);
    return SS$_NORMAL;
}

static int olb_delete_cmd(struct dcl_context *ctx, struct dcl_command *cmd)
{
    const char *del_name = dcl_qualifier_value(cmd, "DELETE");
    if (!del_name || !del_name[0]) {
        dcl_error("LIBRARIAN", 2, "NOMODNAM",
                  "no module name specified with /DELETE");
        return SS$_BADPARAM;
    }
    char lib_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], lib_path, sizeof(lib_path));

    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = olb_load(ctx, cmd->params[0], lib_path, &members, &count, &cap);
    if (!$VMS_STATUS_SUCCESS(st)) return st;

    char want[OLB_NAME_MAX + 1];
    olb_module_name(del_name, want, sizeof want);
    int idx = olb_find(members, count, want);
    if (idx < 0) {
        dcl_error("LIBRARIAN", 2, "KEYNOTFND",
                  "module %s not found in library", del_name);
        olb_free(members, count);
        return SS$_NOSUCHFILE;
    }
    free(members[idx].data);
    for (uint32_t k = (uint32_t)idx; k + 1 < count; k++)
        members[k] = members[k + 1];
    count--;

    if (olb_write(lib_path, members, count) != OLB_OK) {
        olb_free(members, count);
        dcl_error("LIBRARIAN", 2, "OPENOUT",
                  "error writing library - %s", cmd->params[0]);
        return SS$_FILACCERR;
    }
    olb_free(members, count);
    printf("%%LIBRARIAN-S-DELETED, module %s deleted\n", want);
    return SS$_NORMAL;
}

/* Route an OBJECT-library (.OLB) LIBRARY command to the ar-container handlers. */
static int olb_dispatch(struct dcl_context *ctx, struct dcl_command *cmd)
{
    if (dcl_has_qualifier(cmd, "CREATE"))  return olb_create(ctx, cmd);
    if (dcl_has_qualifier(cmd, "DELETE"))  return olb_delete_cmd(ctx, cmd);
    if (dcl_has_qualifier(cmd, "EXTRACT")) return olb_extract_cmd(ctx, cmd);
    if (dcl_has_qualifier(cmd, "LIST"))    return olb_list_cmd(ctx, cmd);
    if (dcl_has_qualifier(cmd, "INSERT") || cmd->param_count >= 2)
        return olb_insert(ctx, cmd);
    /* Only the library named: show its directory. */
    return olb_list_cmd(ctx, cmd);
}

/*
 * cmd_library - LIBRARY command dispatcher
 *
 * Routes to the appropriate sub-function based on qualifiers.
 */
int cmd_library(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* OBJECT libraries (.OLB, or /OBJECT) use the real ar-container object-
     * library path so LINK.EXE can pull members (vms-ca9). TEXT/HELP libraries
     * keep the LBRO format below. */
    if (cmd->param_count >= 1 &&
        lbr_get_type(cmd, cmd->params[0]) == LBR_TYPE_OBJECT) {
        return olb_dispatch(ctx, cmd);
    }

    if (dcl_has_qualifier(cmd, "CREATE")) {
        return lbr_create(ctx, cmd);
    }
    if (dcl_has_qualifier(cmd, "LIST")) {
        return lbr_list(ctx, cmd);
    }
    if (dcl_has_qualifier(cmd, "EXTRACT")) {
        return lbr_extract(ctx, cmd);
    }
    if (dcl_has_qualifier(cmd, "INSERT") || cmd->param_count >= 3) {
        return lbr_insert(ctx, cmd);
    }

    /* Default: if only library file specified, show listing */
    if (cmd->param_count >= 1) {
        return lbr_list(ctx, cmd);
    }

    dcl_error("LIBRARIAN", 2, "NOLIB",
              "no library file specified");
    return SS$_BADPARAM;
}
