/*
 * dcl_library.c - LIBRARY command implementation
 *
 * Manages VMS text libraries (.TLB) and help libraries (.HLB).
 * Supports /CREATE, /INSERT, /EXTRACT, /LIST operations.
 *
 * Library file format (OVMX-specific, not VMS binary-compatible):
 *   Header:  magic(4) type(4) module_count(4) reserved(4)
 *   Modules: name(32) offset(4) length(4) per entry
 *   Data:    raw file contents concatenated
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "ssdef.h"

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

    fwrite(&hdr, sizeof(hdr), 1, fp);
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

    fwrite(&new_hdr, sizeof(new_hdr), 1, fp);
    fwrite(new_modules, sizeof(struct lbr_module), idx, fp);
    fwrite(new_data, 1, total_data, fp);
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

/*
 * cmd_library - LIBRARY command dispatcher
 *
 * Routes to the appropriate sub-function based on qualifiers.
 */
int cmd_library(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

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
