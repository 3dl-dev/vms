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
 * The layout lives in dcl/hlb.h, shared with the HELP reader (dcl_help.c) so
 * HELP can read the compiled .HLB directly. A HELP library stores one module
 * per level-1 HELP key: LIBRARY/HELP/CREATE compiles a numbered-level .HLP
 * source into this key-indexed binary form (VSI OpenVMS DCL Dictionary: LIBRARY,
 * HELP -- unpublished .HLB byte layout is an OVMX design choice, Rule 8).
 *
 * OBJECT library (.OLB) format: a real object library that LINK.EXE consumes to
 * resolve undefined symbols by pulling members. It is a standard `ar` archive of
 * .OBJ (ELF) members — an OVMX-labeled design choice (Rule 8), NOT the
 * unpublished VMS LBR byte layout. The reader/writer are shared with LIBRARIAN.EXE
 * via src/vmslink/include/ovmx_olb.h. See docs/design-self-host-mmk-spine.md
 * section 3. (vms-ca9)
 */


#include "dcl/dcl_out.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/hlb.h"
#include "dcl/dcl_rms.h"     /* every library, source and output file: RMS (vms-8ca, vms-f8cd) */
#include "rms/rms.h"
#include "ssdef.h"
#include "starlet.h"          /* sys$getmsg: the RMS reason under OPENIN/OPENOUT */
#include "descrip.h"
#include "msgdef.h"
#include "climsgdef.h"
#include "ovmx_olb.h"

/* External functions */
extern void dcl_error(const char *facility, int severity, const char *ident,
                      const char *fmt, ...);

/* The LBRO on-disk format (magic, type/limit constants, header + module
 * structs) is defined in dcl/hlb.h so the HELP reader (dcl_help.c) shares it. */

/* ==========================================================================
 * File access -- RMS only (rd vms-f8cd).
 *
 * Every file LIBRARY touches -- the library itself (.TLB/.HLB/.OLB/.A), the
 * source modules and objects put in it, the /EXTRACT /OUTPUT file -- is
 * reached through RMS, as VMS LIBRARIAN reaches them: a library or an object is
 * read whole by block I/O ($OPEN FAC=BIO, $READ), a text module or a .HLP source
 * record by record ($GET), and a library is written as one byte-exact image
 * ($CREATE, fixed-length records with no record size, $PUT). An update
 * (/INSERT, /REPLACE, /DELETE, a HELP merge) supersedes the version the library
 * has, as VMS LIBRARIAN updates a library in place; /CREATE, /COMPRESS and an
 * /OUTPUT file are a new version. A file RMS cannot reach is reported VMS-style:
 * the LIBRARIAN message, then the RMS reason (-RMS-E-FNF ...), never a host
 * errno. On the product runtime RMS reaches the Files-11 ACP over /dev/vms; with
 * no executive (host ctest) RMS's own POSIX backend serves the same calls.
 * ========================================================================== */

/* Why the last file operation was refused: the RMS status and its secondary
 * value (STV), reported under the LIBRARIAN message as VMS does. */
static uint32_t g_lbr_why, g_lbr_whystv;

/* The refusal's messages, chained ('-' for '%'), after the LIBRARIAN message. */
static void lbr_print_reasons(void)
{
    uint32_t sts[2] = { g_lbr_why, g_lbr_whystv };
    for (int i = 0; i < 2; i++) {
        char m[256];
        uint16_t n = 0;
        struct dsc$descriptor_s md = { sizeof m - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, m };
        if (!sts[i] || !(sys$getmsg(sts[i], &n, &md, MSG$M_ALL, NULL) & 1))
            continue;
        m[n] = '\0';
        if (m[0] == '%') m[0] = '-';
        dcl_err_printf("%s\n", m);
    }
}

/* Read a whole file by RMS block I/O into a fresh buffer (*len bytes). NULL
 * when RMS refuses it (g_lbr_why says why). */
static unsigned char *lbr_file_read(struct dcl_context *ctx, const char *spec,
                                    size_t *len)
{
    uint32_t st = 0;
    g_lbr_why = g_lbr_whystv = 0;
    struct dcl_rms_bio *b = dcl_rms_bio_open(ctx, spec, NULL, &st);
    if (!b) { g_lbr_why = st; return NULL; }
    size_t cap = 64 * 1024, n = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) { dcl_rms_bio_close(b); g_lbr_why = RMS$_DME; return NULL; }
    for (;;) {
        if (n == cap) {
            unsigned char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); dcl_rms_bio_close(b); g_lbr_why = RMS$_DME; return NULL; }
            buf = nb;
            cap *= 2;
        }
        size_t want = cap - n;
        long got = dcl_rms_bio_read(b, buf + n, want);
        if (got < 0) {
            g_lbr_why = dcl_rms_bio_status(b);
            free(buf);
            dcl_rms_bio_close(b);
            return NULL;
        }
        n += (size_t)got;
        if ((size_t)got < want) break;          /* short: end of file */
    }
    dcl_rms_bio_close(b);
    *len = n;
    return buf;
}

/* Read a text file record by record ($GET) as its lines, each ended by a
 * newline -- the text a TEXT-library module or a .HLP source holds. NULL when
 * RMS refuses it (g_lbr_why says why). */
static char *lbr_text_read(struct dcl_context *ctx, const char *spec, size_t *len)
{
    uint32_t st = 0;
    g_lbr_why = g_lbr_whystv = 0;
    struct dcl_rms_reader *r = dcl_rms_read_open(ctx, spec, &st);
    if (!r) { g_lbr_why = st; return NULL; }
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    char *rec = malloc(32768);
    if (!buf || !rec) {
        free(buf); free(rec); dcl_rms_read_close(r);
        g_lbr_why = RMS$_DME;
        return NULL;
    }
    for (;;) {
        int eof = 0;
        int rl = dcl_rms_read_record(r, rec, 32768, &eof);
        if (rl < 0) {
            if (eof) break;
            g_lbr_why = RMS$_RER;
            free(buf); free(rec); dcl_rms_read_close(r);
            return NULL;
        }
        while (n + (size_t)rl + 2 > cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf); free(rec); dcl_rms_read_close(r);
                g_lbr_why = RMS$_DME;
                return NULL;
            }
            buf = nb;
            cap *= 2;
        }
        memcpy(buf + n, rec, (size_t)rl);
        n += (size_t)rl;
        buf[n++] = '\n';
    }
    free(rec);
    dcl_rms_read_close(r);
    buf[n] = '\0';
    *len = n;
    return buf;
}

/* Write `len` bytes as `spec`; 0 on success, -1 (g_lbr_why/g_lbr_whystv say
 * why). Through RMS the file is fixed-length with no record size, so each $PUT
 * lands exactly its own bytes (the byte-exact write LINK.EXE's image and
 * LIBRARIAN.EXE's .OLB use). in_place: supersede the version the file has now
 * (the highest); otherwise a new version. */
static int lbr_file_write(struct dcl_context *ctx, const char *spec,
                          const unsigned char *buf, size_t len, int in_place)
{
    g_lbr_why = g_lbr_whystv = 0;
    uint32_t st = 0;
    struct dcl_rms_writer *w = NULL;
    if (in_place) {
        char eff[1024], cur[1024];
        struct dcl_rms_dir *d = NULL;
        if (dcl_rms_effective_spec(ctx, spec, eff, sizeof(eff)) == 0)
            d = dcl_rms_dir_open(ctx, eff);
        if (d && dcl_rms_dir_next(d, cur, sizeof(cur), NULL, NULL, NULL))
            w = dcl_rms_write_supersede(ctx, cur, FAB$C_FIX, 0, 0, &st);
        else
            w = dcl_rms_write_create(ctx, spec, FAB$C_FIX, 0, 0, &st);
        if (d) dcl_rms_dir_close(d);
    } else {
        w = dcl_rms_write_create(ctx, spec, FAB$C_FIX, 0, 0, &st);
    }
    if (!w) { g_lbr_why = st; return -1; }
    int rc = 0;
    for (size_t done = 0; done < len && rc == 0; ) {
        size_t chunk = len - done > 32768 ? 32768 : len - done;
        rc = dcl_rms_write_record(w, (const char *)buf + done, chunk);
        done += chunk;
    }
    if (rc) g_lbr_why = dcl_rms_write_status(w, &g_lbr_whystv);
    uint32_t cst = dcl_rms_write_close(w);
    if (!(cst & 1)) { if (!rc) g_lbr_why = cst; rc = -1; }
    return rc;
}

/* Write text (lines ended by newlines) as a new version of `spec`, one
 * variable-length carriage-control record ($PUT) per line -- an extracted text
 * or help module, the file VMS LIBRARIAN /EXTRACT /OUTPUT makes. 0 or -1. */
static int lbr_text_write(struct dcl_context *ctx, const char *spec,
                          const char *data, size_t len)
{
    uint32_t st = 0;
    g_lbr_why = g_lbr_whystv = 0;
    struct dcl_rms_writer *w = dcl_rms_write_create(ctx, spec, FAB$C_VAR,
                                                    FAB$M_CR, 0, &st);
    if (!w) { g_lbr_why = st; return -1; }
    int rc = 0;
    size_t i = 0;
    while (i < len && rc == 0) {
        size_t e = i;
        while (e < len && data[e] != '\n') e++;
        rc = dcl_rms_write_record(w, data + i, e - i);
        i = e + 1;
    }
    if (rc) g_lbr_why = dcl_rms_write_status(w, &g_lbr_whystv);
    uint32_t cst = dcl_rms_write_close(w);
    if (!(cst & 1)) { if (!rc) g_lbr_why = cst; rc = -1; }
    return rc;
}

/* ==========================================================================
 * TEXT / HELP libraries -- the LBRO container (dcl/hlb.h).
 * ========================================================================== */

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
        /* .A: the object library a Unix-style `ar` makes and the linker searches
         * (the GCC port's ar wrapper runs `library /replace/create libx.a x.o`) */
        if (strcasecmp(dot, ".A") == 0) return LBR_TYPE_OBJECT;
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

/* Build an LBRO image: header, module table, then each module's data. */
static unsigned char *lbr_build(uint32_t type, const struct lbr_module *mods,
                                const char *const *datas, uint32_t count,
                                size_t *out_len)
{
    size_t total = sizeof(struct lbr_header) + (size_t)count * sizeof(struct lbr_module);
    for (uint32_t i = 0; i < count; i++) total += mods[i].length;
    unsigned char *img = calloc(1, total ? total : 1);
    if (!img) return NULL;

    struct lbr_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = LBR_MAGIC;
    hdr.type = type;
    hdr.module_count = count;
    hdr.reserved = 0;
    memcpy(img, &hdr, sizeof(hdr));

    size_t off = sizeof(hdr) + (size_t)count * sizeof(struct lbr_module);
    for (uint32_t i = 0; i < count; i++) {
        struct lbr_module m = mods[i];
        m.offset = (uint32_t)off;
        memcpy(img + sizeof(hdr) + i * sizeof(struct lbr_module), &m, sizeof(m));
        if (m.length && datas[i])
            memcpy(img + off, datas[i], m.length);
        off += m.length;
    }
    *out_len = total;
    return img;
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

    int type = lbr_get_type(cmd, cmd->params[0]);

    size_t len = 0;
    unsigned char *img = lbr_build((uint32_t)type, NULL, NULL, 0, &len);
    if (!img) {
        dcl_error("LIBRARIAN", 4, "INSFMEM", "insufficient memory");
        return SS$_ABORT;
    }
    int rc = lbr_file_write(ctx, cmd->params[0], img, len, 0);
    free(img);
    if (rc != 0) {
        dcl_error("LIBRARIAN", 2, "OPENOUT",
                  "error creating library - %s", cmd->params[0]);
        lbr_print_reasons();
        return SS$_FILACCERR;
    }

    dcl_out_printf("%%LIBRARIAN-S-CREATED, %s library %s created\n",
           lbr_type_name(type), cmd->params[0]);
    return SS$_NORMAL;
}

/*
 * Read a library (whole, through RMS) and its header and module table.
 * On success *img/*img_len hold the file's bytes and *modules the table (both
 * the caller's to free); module data lives at img + modules[i].offset.
 */
static int lbr_load(struct dcl_context *ctx, const char *vms_name,
                    struct lbr_header *hdr, struct lbr_module **modules,
                    unsigned char **img, size_t *img_len)
{
    *modules = NULL;
    *img = NULL;
    size_t len = 0;
    unsigned char *buf = lbr_file_read(ctx, vms_name, &len);
    if (!buf) {
        dcl_error("LIBRARIAN", 2, "OPENIN",
                  "error opening library - %s", vms_name);
        lbr_print_reasons();
        return SS$_NOSUCHFILE;
    }

    if (len < sizeof(*hdr)) {
        dcl_error("LIBRARIAN", 2, "ILLHDR",
                  "not a valid OVMX library - %s", vms_name);
        free(buf);
        return SS$_NOSUCHFILE;
    }
    memcpy(hdr, buf, sizeof(*hdr));
    if (hdr->magic != LBR_MAGIC) {
        dcl_error("LIBRARIAN", 2, "ILLHDR",
                  "not a valid OVMX library - %s", vms_name);
        free(buf);
        return SS$_NOSUCHFILE;
    }

    if (hdr->module_count > LBR_MAX_MODULES) {
        dcl_error("LIBRARIAN", 2, "TOOMANYMOD",
                  "library has too many modules - %s", vms_name);
        free(buf);
        return SS$_NOSUCHFILE;
    }

    if (hdr->module_count > 0) {
        size_t tlen = (size_t)hdr->module_count * sizeof(struct lbr_module);
        if (len < sizeof(*hdr) + tlen) {
            dcl_error("LIBRARIAN", 2, "READERR",
                      "error reading module table - %s", vms_name);
            free(buf);
            return SS$_NOSUCHFILE;
        }
        *modules = calloc(hdr->module_count, sizeof(struct lbr_module));
        if (!*modules) {
            dcl_error("LIBRARIAN", 4, "INSFMEM", "insufficient memory");
            free(buf);
            return SS$_NOSUCHFILE;
        }
        memcpy(*modules, buf + sizeof(*hdr), tlen);
    }

    *img = buf;
    *img_len = len;
    return SS$_NORMAL;
}

/* The bytes of module m that the library image really holds (a table entry
 * reaching past the end of the file yields only what is there). */
static size_t lbr_module_avail(const struct lbr_module *m, size_t img_len)
{
    if (m->offset >= img_len) return 0;
    size_t avail = img_len - m->offset;
    return m->length < avail ? m->length : avail;
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

    /* Read the source module, record by record */
    size_t src_len = 0;
    char *src_data = lbr_text_read(ctx, cmd->params[2], &src_len);
    if (!src_data) {
        dcl_error("LIBRARIAN", 2, "OPENIN",
                  "error opening source file - %s", cmd->params[2]);
        lbr_print_reasons();
        return SS$_NOSUCHFILE;
    }

    /* Read existing library */
    struct lbr_header hdr;
    struct lbr_module *modules = NULL;
    unsigned char *img = NULL;
    size_t img_len = 0;
    int st = lbr_load(ctx, cmd->params[0], &hdr, &modules, &img, &img_len);
    if (!$VMS_STATUS_SUCCESS(st)) {
        free(src_data);
        return st;
    }

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
        free(img);
        free(src_data);
        free(modules);
        return SS$_ABORT;
    }

    struct lbr_module *new_modules = calloc(new_count, sizeof(struct lbr_module));
    const char **datas = calloc(new_count, sizeof(*datas));
    char **owned = calloc(new_count, sizeof(*owned));
    if (!new_modules || !datas || !owned) {
        free(new_modules); free(datas); free(owned);
        free(img);
        free(src_data);
        free(modules);
        return SS$_ABORT;
    }

    /* Keep existing modules (skip the one being replaced) */
    uint32_t idx = 0;
    int oom = 0;
    for (uint32_t i = 0; i < hdr.module_count; i++) {
        if (replacing && strncasecmp(modules[i].name, mod_name, LBR_NAME_LEN) == 0)
            continue;
        memcpy(new_modules[idx].name, modules[i].name, LBR_NAME_LEN);
        new_modules[idx].length = modules[i].length;
        size_t avail = lbr_module_avail(&modules[i], img_len);
        if (avail == modules[i].length) {
            datas[idx] = (const char *)img + modules[i].offset;
        } else {
            /* a truncated entry: what is there, the rest zero */
            owned[idx] = calloc(1, modules[i].length ? modules[i].length : 1);
            if (!owned[idx]) { oom = 1; break; }
            if (avail) memcpy(owned[idx], img + modules[i].offset, avail);
            datas[idx] = owned[idx];
        }
        idx++;
    }

    /* Add new module */
    if (!oom) {
        memcpy(new_modules[idx].name, mod_name, LBR_NAME_LEN);
        new_modules[idx].length = (uint32_t)src_len;
        datas[idx] = src_data;
        idx++;
    }

    size_t out_len = 0;
    unsigned char *out = oom ? NULL
                             : lbr_build(hdr.type, new_modules, datas, idx, &out_len);
    for (uint32_t i = 0; i < new_count; i++) free(owned[i]);
    free(owned);
    free(datas);
    free(new_modules);
    free(img);
    free(src_data);
    free(modules);
    if (!out) {
        dcl_error("LIBRARIAN", 4, "INSFMEM", "insufficient memory");
        return SS$_ABORT;
    }

    /* Write rebuilt library, superseding the version it has */
    int rc = lbr_file_write(ctx, cmd->params[0], out, out_len, 1);
    free(out);
    if (rc != 0) {
        dcl_error("LIBRARIAN", 2, "OPENOUT",
                  "error writing library - %s", cmd->params[0]);
        lbr_print_reasons();
        return SS$_FILACCERR;
    }

    dcl_out_printf("%%LIBRARIAN-S-%s, module %s %s\n",
           replacing ? "REPLACED" : "INSERTED",
           mod_name,
           replacing ? "replaced" : "inserted");
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

    struct lbr_header hdr;
    struct lbr_module *modules = NULL;
    unsigned char *img = NULL;
    size_t img_len = 0;
    int st = lbr_load(ctx, cmd->params[0], &hdr, &modules, &img, &img_len);
    if (!$VMS_STATUS_SUCCESS(st)) return st;
    free(img);

    dcl_out_printf("Directory of %s LIBRARY %s\n\n",
           lbr_type_name(hdr.type), cmd->params[0]);

    if (hdr.module_count == 0) {
        dcl_out_printf("  (empty library)\n");
    } else {
        for (uint32_t i = 0; i < hdr.module_count; i++) {
            char name[LBR_NAME_LEN + 1];
            memcpy(name, modules[i].name, LBR_NAME_LEN);
            name[LBR_NAME_LEN] = '\0';
            for (int j = LBR_NAME_LEN - 1; j >= 0 && (name[j] == '\0' || name[j] == ' '); j--)
                name[j] = '\0';
            dcl_out_printf("  %-32s  %u bytes\n", name, modules[i].length);
        }
    }
    dcl_out_printf("\n%u module%s in library\n",
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

    struct lbr_header hdr;
    struct lbr_module *modules = NULL;
    unsigned char *img = NULL;
    size_t img_len = 0;
    int st = lbr_load(ctx, cmd->params[0], &hdr, &modules, &img, &img_len);
    if (!$VMS_STATUS_SUCCESS(st)) return st;

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
        free(img);
        return SS$_NOSUCHFILE;
    }

    const char *data = (const char *)img + modules[found].offset;
    size_t dlen = lbr_module_avail(&modules[found], img_len);

    /* Output: /OUTPUT=file (a text file, through RMS) or SYS$OUTPUT */
    const char *output_file = dcl_qualifier_value(cmd, "OUTPUT");
    if (output_file && output_file[0]) {
        if (lbr_text_write(ctx, output_file, data, dlen) != 0) {
            dcl_error("LIBRARIAN", 2, "OPENOUT",
                      "error creating output file - %s", output_file);
            lbr_print_reasons();
            free(modules);
            free(img);
            return SS$_FILACCERR;
        }
        dcl_out_printf("%%LIBRARIAN-S-EXTRACTED, module %s extracted to %s\n",
               extract_name, output_file);
    } else if (dlen) {
        dcl_out_write(data, dlen);
    }

    free(modules);
    free(img);
    return SS$_NORMAL;
}

/* ==========================================================================
 * HELP library (.HLB) compilation — LIBRARY/HELP/CREATE lib.HLB src.HLP.
 *
 * A VMS HELP library is keyed by its level-1 HELP topics: LIBRARY compiles a
 * numbered-level .HLP source into one module per level-1 key (VSI OpenVMS DCL
 * Dictionary: LIBRARY; Command Definition, Librarian, and Message Utilities
 * Manual: Librarian -- "each help module is identified by its level-1 key").
 * The module name is the level-1 key; the module data is that key's subtree
 * (the "1 KEY" line through the line before the next "1 " line), so the reader
 * reconstructs the exact numbered-level text (dcl_help.c help_open_hlb). The
 * container bytes are the OVMX "LBRO" format (dcl/hlb.h), an OVMX design choice
 * (Rule 8) -- the VMS .HLB byte layout is unpublished.
 * ========================================================================== */

/* One compiled HELP module: a level-1 key and the raw source bytes of its
 * subtree (owned copy). */
struct hlp_module {
    char   name[LBR_NAME_LEN]; /* uppercased level-1 key, NUL/space padded */
    char  *data;
    size_t length;
};

/* True for a level-1 HELP key line: '1' in column 1 followed by whitespace. */
static int hlp_is_level1(const char *line, size_t len)
{
    return len >= 2 && line[0] == '1' && (line[1] == ' ' || line[1] == '\t');
}

/* Fill an uppercased, NUL/space-padded module name from a level-1 key line
 * ("1 COPY ...") -- the key is the first whitespace token after the digit. */
static void hlp_key_name(const char *line, size_t len, char out[LBR_NAME_LEN])
{
    memset(out, 0, LBR_NAME_LEN);
    size_t i = 1;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    size_t k = 0;
    while (i < len && line[i] != ' ' && line[i] != '\t' && k < LBR_NAME_LEN - 1)
        out[k++] = (char)toupper((unsigned char)line[i++]);
}

/* Split a .HLP source buffer into level-1-keyed modules. Returns a malloc'd
 * array (*out_mods, *out_count), or SS$ error. Bytes before the first level-1
 * key are ignored (VMS treats them as a leading comment). */
static int hlp_split_source(const char *src, size_t src_len,
                            struct hlp_module **out_mods, uint32_t *out_count)
{
    *out_mods = NULL;
    *out_count = 0;

    struct hlp_module *mods = NULL;
    uint32_t count = 0, cap = 0;

    size_t i = 0;
    while (i < src_len) {
        /* Extent of the current physical line [i, eol), with nl at line_end. */
        size_t eol = i;
        while (eol < src_len && src[eol] != '\n') eol++;
        size_t line_end = (eol < src_len) ? eol + 1 : eol; /* include the '\n' */
        size_t line_len = eol - i;

        if (hlp_is_level1(src + i, line_len)) {
            if (count == cap) {
                uint32_t ncap = cap ? cap * 2 : 16;
                struct hlp_module *nm = realloc(mods, ncap * sizeof(*nm));
                if (!nm) { goto oom; }
                mods = nm;
                cap = ncap;
            }
            if (count >= LBR_MAX_MODULES) {
                dcl_error("LIBRARIAN", 2, "TOOMANYMOD",
                          "help source has too many level-1 keys");
                goto fail;
            }
            hlp_key_name(src + i, line_len, mods[count].name);
            mods[count].data = NULL;
            mods[count].length = 0;
            count++;
        }

        if (count > 0) {
            /* Append this physical line (with its newline) to the current
             * module's data. */
            struct hlp_module *m = &mods[count - 1];
            char *nd = realloc(m->data, m->length + (line_end - i) + 1);
            if (!nd) goto oom;
            m->data = nd;
            memcpy(m->data + m->length, src + i, line_end - i);
            m->length += (line_end - i);
            m->data[m->length] = '\0';
        }
        i = line_end;
    }

    if (count == 0) {
        dcl_error("LIBRARIAN", 2, "NOMODS",
                  "help source contains no level-1 keys");
        free(mods);
        return SS$_BADPARAM;
    }

    *out_mods = mods;
    *out_count = count;
    return SS$_NORMAL;

oom:
    dcl_error("LIBRARIAN", 4, "INSFMEM", "insufficient memory");
fail:
    for (uint32_t j = 0; j < count; j++) free(mods[j].data);
    free(mods);
    return SS$_ABORT;
}

static void hlp_free_modules(struct hlp_module *mods, uint32_t count)
{
    for (uint32_t j = 0; j < count; j++) free(mods[j].data);
    free(mods);
}

/* Write an LBRO HELP container from a module array: a new version of the
 * library, or (in_place) superseding the version it has. */
static int hlp_write_library(struct dcl_context *ctx, const char *vms_name,
                             struct hlp_module *mods, uint32_t count, int in_place)
{
    struct lbr_module *idx = calloc(count ? count : 1, sizeof(*idx));
    const char **datas = calloc(count ? count : 1, sizeof(*datas));
    if (!idx || !datas) {
        free(idx); free(datas);
        dcl_error("LIBRARIAN", 4, "INSFMEM", "insufficient memory");
        return SS$_ABORT;
    }
    for (uint32_t j = 0; j < count; j++) {
        memcpy(idx[j].name, mods[j].name, LBR_NAME_LEN);
        idx[j].length = (uint32_t)mods[j].length;
        datas[j] = mods[j].data;
    }
    size_t len = 0;
    unsigned char *img = lbr_build(LBR_TYPE_HELP, idx, datas, count, &len);
    free(idx);
    free(datas);
    if (!img) {
        dcl_error("LIBRARIAN", 4, "INSFMEM", "insufficient memory");
        return SS$_ABORT;
    }
    int rc = lbr_file_write(ctx, vms_name, img, len, in_place);
    free(img);
    if (rc != 0) {
        dcl_error("LIBRARIAN", 2, "OPENOUT",
                  "error creating library - %s", vms_name);
        lbr_print_reasons();
        return SS$_FILACCERR;
    }
    return SS$_NORMAL;
}

/*
 * LIBRARY/HELP/CREATE lib.HLB src.HLP  -- compile a .HLP source into a .HLB
 * (create=1, fresh library), or LIBRARY/HELP lib.HLB src.HLP (create=0, merge:
 * modules for level-1 keys already present are replaced, new keys appended).
 */
static int hlp_compile(struct dcl_context *ctx, struct dcl_command *cmd,
                       int create)
{
    if (cmd->param_count < 2) {
        dcl_error("LIBRARIAN", 2, "BADPARAM",
                  "usage: LIBRARY/HELP/CREATE library-file.HLB source-file.HLP");
        return SS$_BADPARAM;
    }

    /* The .HLP source, record by record */
    size_t src_len = 0;
    char *src = lbr_text_read(ctx, cmd->params[1], &src_len);
    if (!src) {
        dcl_error("LIBRARIAN", 2, "OPENIN",
                  "error opening source file - %s", cmd->params[1]);
        lbr_print_reasons();
        return SS$_NOSUCHFILE;
    }
    int st;

    struct hlp_module *newmods = NULL;
    uint32_t newcount = 0;
    st = hlp_split_source(src, src_len, &newmods, &newcount);
    free(src);
    if (!$VMS_STATUS_SUCCESS(st)) return st;

    struct hlp_module *final = newmods;
    uint32_t final_count = newcount;
    struct hlp_module *merged = NULL;

    int in_place = 0;
    if (!create) {
        /* Merge onto the existing library: keep old modules whose key is not
         * being replaced, then all the new ones. A library that cannot be read
         * is reported (OPENIN and the RMS reason) and, as before, created. */
        struct lbr_header hdr;
        struct lbr_module *old = NULL;
        unsigned char *img = NULL;
        size_t img_len = 0;
        if ($VMS_STATUS_SUCCESS(lbr_load(ctx, cmd->params[0], &hdr, &old,
                                         &img, &img_len))) {
            in_place = 1;
            uint32_t mcap = hdr.module_count + newcount;
            merged = calloc(mcap ? mcap : 1, sizeof(*merged));
            if (!merged) {
                dcl_error("LIBRARIAN", 4, "INSFMEM", "insufficient memory");
                free(old); free(img);
                hlp_free_modules(newmods, newcount);
                return SS$_ABORT;
            }
            uint32_t mc = 0;
            for (uint32_t i = 0; i < hdr.module_count; i++) {
                int replaced = 0;
                for (uint32_t k = 0; k < newcount; k++)
                    if (strncasecmp(old[i].name, newmods[k].name, LBR_NAME_LEN) == 0) {
                        replaced = 1; break;
                    }
                if (replaced) continue;
                if (lbr_module_avail(&old[i], img_len) != old[i].length) {
                    dcl_error("LIBRARIAN", 2, "READERR",
                              "error reading library - %s", cmd->params[0]);
                    free(old); free(img);
                    hlp_free_modules(merged, mc);
                    hlp_free_modules(newmods, newcount);
                    return SS$_ABORT;
                }
                memcpy(merged[mc].name, old[i].name, LBR_NAME_LEN);
                merged[mc].length = old[i].length;
                merged[mc].data = malloc(old[i].length ? old[i].length : 1);
                if (!merged[mc].data) {
                    dcl_error("LIBRARIAN", 4, "INSFMEM", "insufficient memory");
                    free(old); free(img);
                    hlp_free_modules(merged, mc);
                    hlp_free_modules(newmods, newcount);
                    return SS$_ABORT;
                }
                if (old[i].length)
                    memcpy(merged[mc].data, img + old[i].offset, old[i].length);
                mc++;
            }
            for (uint32_t k = 0; k < newcount; k++) {
                memcpy(merged[mc].name, newmods[k].name, LBR_NAME_LEN);
                merged[mc].data = newmods[k].data;   /* transfer ownership */
                merged[mc].length = newmods[k].length;
                newmods[k].data = NULL;
                mc++;
            }
            free(old);
            free(img);
            hlp_free_modules(newmods, newcount); /* frees only NULLed shells */
            final = merged;
            final_count = mc;
        }
        /* If the library does not exist yet, fall through and create it. */
    }

    st = hlp_write_library(ctx, cmd->params[0], final, final_count, in_place);
    hlp_free_modules(final, final_count);

    if ($VMS_STATUS_SUCCESS(st))
        dcl_out_printf("%%LIBRARIAN-S-%s, help library %s %s (%u module%s)\n",
               create ? "CREATED" : "INSERTED", cmd->params[0],
               create ? "created" : "updated", final_count,
               final_count == 1 ? "" : "s");
    return st;
}

/* Route a HELP-library (.HLB, or /HELP) LIBRARY command. */
static int hlp_dispatch(struct dcl_context *ctx, struct dcl_command *cmd)
{
    if (dcl_has_qualifier(cmd, "LIST"))    return lbr_list(ctx, cmd);
    if (dcl_has_qualifier(cmd, "EXTRACT")) return lbr_extract(ctx, cmd);
    if (dcl_has_qualifier(cmd, "CREATE")) {
        /* With a source file, compile it; otherwise create an empty library. */
        if (cmd->param_count >= 2) return hlp_compile(ctx, cmd, 1);
        return lbr_create(ctx, cmd);
    }
    /* LIBRARY/HELP lib.HLB src.HLP (no /CREATE) = insert/merge. */
    if (dcl_has_qualifier(cmd, "INSERT") || cmd->param_count >= 2)
        return hlp_compile(ctx, cmd, 0);
    /* Only the library named: show its directory. */
    if (cmd->param_count >= 1) return lbr_list(ctx, cmd);
    dcl_error("LIBRARIAN", 2, "NOLIB", "no library file specified");
    return SS$_BADPARAM;
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

/* Load the library `spec` into a member array: OLB_OK, OLB_ERR_OPEN, ... */
static int olb_lib_read(struct dcl_context *ctx, const char *spec,
                        struct olb_member **members, uint32_t *count)
{
    size_t len = 0;
    unsigned char *buf = lbr_file_read(ctx, spec, &len);
    if (!buf) { *members = NULL; *count = 0; return OLB_ERR_OPEN; }
    return olb_parse(buf, len, members, count);
}

/* Write the member array as the library `spec`: in place (the version it has,
 * superseded) for /INSERT, /REPLACE and /DELETE, as VMS LIBRARIAN updates a
 * library; a new version for /CREATE and /COMPRESS. A new version per update
 * filled the cc1 build's volume with copies of LIBBACKEND.A (rd vms-465). */
static int olb_lib_write(struct dcl_context *ctx, const char *spec,
                         const struct olb_member *members, uint32_t count,
                         int in_place)
{
    unsigned char *img = NULL;
    size_t len = 0;
    int rc = olb_build(members, count, &img, &len);
    if (rc != OLB_OK) return rc;
    rc = lbr_file_write(ctx, spec, img, len, in_place) == 0 ? OLB_OK : OLB_ERR_IO;
    free(img);
    return rc;
}

/* Insert (or replace) the .OBJ files params[first..] into an in-memory member
 * array. Takes ownership of allocated data. Returns a VMS status code. */
static int olb_insert_files(struct dcl_context *ctx, struct dcl_command *cmd,
                            int first, struct olb_member **members,
                            uint32_t *count, uint32_t *cap)
{
    /* the whole input list, past P8 included (rd vms-465) */
    for (int i = first; i < dcl_param_total(cmd); i++) {
        const char *in = dcl_param_at(cmd, i);
        size_t len = 0;
        unsigned char *data = lbr_file_read(ctx, in, &len);
        if (!data) {
            dcl_error("LIBRARIAN", 2, "OPENIN",
                      "error opening object file - %s", in);
            lbr_print_reasons();
            return SS$_NOSUCHFILE;
        }
        char name[OLB_NAME_MAX + 1];
        olb_module_name(in, name, sizeof name);
        if (name[0] == '\0') {
            free(data);
            dcl_error("LIBRARIAN", 2, "BADNAME",
                      "cannot derive module name - %s", in);
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
        dcl_out_printf("%%LIBRARIAN-S-%s, module %s %s\n",
               replaced ? "REPLACED" : "INSERTED", name,
               replaced ? "replaced" : "inserted");
    }
    return SS$_NORMAL;
}

/* Load an existing .OLB into a growable member array; VMS status code. */
static int olb_load(struct dcl_context *ctx, const char *vms_name,
                    struct olb_member **members, uint32_t *count, uint32_t *cap)
{
    int rc = olb_lib_read(ctx, vms_name, members, count);
    if (rc == OLB_OK) { *cap = *count; return SS$_NORMAL; }
    if (rc == OLB_ERR_OPEN) {
        dcl_error("LIBRARIAN", 2, "OPENIN", "error opening library - %s", vms_name);
        lbr_print_reasons();
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
    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = olb_insert_files(ctx, cmd, 1, &members, &count, &cap);
    if (!$VMS_STATUS_SUCCESS(st)) { olb_free(members, count); return st; }

    if (olb_lib_write(ctx, cmd->params[0], members, count, 0) != OLB_OK) {
        olb_free(members, count);
        dcl_error("LIBRARIAN", 2, "OPENOUT",
                  "error creating library - %s", cmd->params[0]);
        lbr_print_reasons();
        return SS$_FILACCERR;
    }
    olb_free(members, count);
    dcl_out_printf("%%LIBRARIAN-S-CREATED, object library %s created\n", cmd->params[0]);
    return SS$_NORMAL;
}

static int olb_insert(struct dcl_context *ctx, struct dcl_command *cmd)
{
    if (cmd->param_count < 2) {
        dcl_error("LIBRARIAN", 2, "BADPARAM",
                  "usage: LIBRARY library.OLB object-file[ ...]");
        return SS$_BADPARAM;
    }
    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = olb_load(ctx, cmd->params[0], &members, &count, &cap);
    if (!$VMS_STATUS_SUCCESS(st)) return st;

    st = olb_insert_files(ctx, cmd, 1, &members, &count, &cap);
    if (!$VMS_STATUS_SUCCESS(st)) { olb_free(members, count); return st; }

    if (olb_lib_write(ctx, cmd->params[0], members, count, 1) != OLB_OK) {
        olb_free(members, count);
        dcl_error("LIBRARIAN", 2, "OPENOUT",
                  "error writing library - %s", cmd->params[0]);
        lbr_print_reasons();
        return SS$_FILACCERR;
    }
    olb_free(members, count);
    return SS$_NORMAL;
}

static int olb_list_cmd(struct dcl_context *ctx, struct dcl_command *cmd)
{
    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = olb_load(ctx, cmd->params[0], &members, &count, &cap);
    if (!$VMS_STATUS_SUCCESS(st)) return st;

    dcl_out_printf("\nDirectory of OBJECT library %s\n\n", cmd->params[0]);
    if (count == 0) {
        dcl_out_printf("  (empty library)\n");
    } else {
        for (uint32_t i = 0; i < count; i++)
            dcl_out_printf("  %-31s  %zu bytes\n", members[i].name, members[i].len);
    }
    dcl_out_printf("\n%u module%s in library\n", count, count == 1 ? "" : "s");
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
    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = olb_load(ctx, cmd->params[0], &members, &count, &cap);
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
    if (output_file && output_file[0]) {
        if (lbr_file_write(ctx, output_file, members[idx].data,
                           members[idx].len, 0) != 0) {
            dcl_error("LIBRARIAN", 2, "OPENOUT",
                      "error creating output file - %s", output_file);
            lbr_print_reasons();
            olb_free(members, count);
            return SS$_FILACCERR;
        }
        dcl_out_printf("%%LIBRARIAN-S-EXTRACTED, module %s extracted to %s\n",
               want, output_file);
    } else if (members[idx].len) {
        dcl_out_write(members[idx].data, members[idx].len);
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
    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = olb_load(ctx, cmd->params[0], &members, &count, &cap);
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

    if (olb_lib_write(ctx, cmd->params[0], members, count, 1) != OLB_OK) {
        olb_free(members, count);
        dcl_error("LIBRARIAN", 2, "OPENOUT",
                  "error writing library - %s", cmd->params[0]);
        lbr_print_reasons();
        return SS$_FILACCERR;
    }
    olb_free(members, count);
    dcl_out_printf("%%LIBRARIAN-S-DELETED, module %s deleted\n", want);
    return SS$_NORMAL;
}

/*
 * LIBRARY/OBJECT/COMPRESS [/OUTPUT=file] lib.OLB -- rewrite the library to
 * reclaim space. The container is read member by member and written out whole
 * (to /OUTPUT when given, else over the input), exactly the rewrite VMS does.
 */
static int olb_compress(struct dcl_context *ctx, struct dcl_command *cmd)
{
    const char *outspec = dcl_qualifier_value(cmd, "OUTPUT");
    const char *out_spec = (outspec && outspec[0]) ? outspec : cmd->params[0];

    struct olb_member *members = NULL;
    uint32_t count = 0, cap = 0;
    int st = olb_load(ctx, cmd->params[0], &members, &count, &cap);
    if (!$VMS_STATUS_SUCCESS(st)) return st;

    if (olb_lib_write(ctx, out_spec, members, count, 0) != OLB_OK) {
        olb_free(members, count);
        dcl_error("LIBRARIAN", 2, "OPENOUT",
                  "error writing library - %s", outspec && outspec[0] ? outspec : cmd->params[0]);
        lbr_print_reasons();
        return SS$_FILACCERR;
    }
    olb_free(members, count);
    dcl_out_printf("%%LIBRARIAN-S-COMPRESSED, object library %s compressed (%u module%s)\n",
           cmd->params[0], count, count == 1 ? "" : "s");
    return SS$_NORMAL;
}

/* Route an OBJECT-library (.OLB) LIBRARY command to the ar-container handlers. */
static int olb_dispatch(struct dcl_context *ctx, struct dcl_command *cmd)
{
    /* Only an insert/replace/create takes an input list past P8 (rd vms-465). */
    if (cmd->overflow_count > 0 &&
        (dcl_has_qualifier(cmd, "DELETE") || dcl_has_qualifier(cmd, "EXTRACT") ||
         dcl_has_qualifier(cmd, "LIST"))) {
        dcl_error("DCL", 0, "MAXPARM",
                  "too many parameters - reenter command with fewer parameters");
        return CLI$_MAXPARM;
    }
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

    /* /VAX selects the VAX object-library format. OVMX writes one format per
     * target architecture, so /VAX is honoured only where that format IS the
     * target's (the VAX rail); elsewhere it would silently yield a non-VAX
     * library, so it is refused. */
#if !defined(__vax__) && !defined(__vax)
    if (dcl_has_qualifier(cmd, "VAX")) {
        dcl_error("DCL", 0, "IVQUAL",
                  "unrecognized qualifier - check validity, spelling, and placement\n \\VAX\\");
        return CLI$_IVQUAL;
    }
#endif

    /* /COMPRESS rewrites an OBJECT library (see olb_compress). Combined with
     * another operation, or on a TEXT/HELP library, it is not implemented. */
    if (dcl_has_qualifier(cmd, "COMPRESS")) {
        /* /COMPRESS=BLOCK=0 lexes its "=0" tail into a stray numeric parameter
         * ahead of the library: the library is the first non-numeric one. */
        int li = -1;
        for (int i = 0; i < cmd->param_count; i++) {
            const char *q = cmd->params[i];
            int numeric = (*q != '\0');
            for (; *q; q++)
                if (*q < '0' || *q > '9') { numeric = 0; break; }
            if (!numeric) { li = i; break; }
        }
        int nlib = 0;
        for (int i = 0; i < cmd->param_count; i++)
            if (i == li || !(cmd->params[i][0] >= '0' && cmd->params[i][0] <= '9')) nlib++;
        int other = dcl_has_qualifier(cmd, "CREATE") || dcl_has_qualifier(cmd, "INSERT") ||
                    dcl_has_qualifier(cmd, "REPLACE") || dcl_has_qualifier(cmd, "DELETE") ||
                    dcl_has_qualifier(cmd, "EXTRACT") || dcl_has_qualifier(cmd, "LIST") ||
                    li < 0 || nlib != 1;
        if (!other) {
            if (li > 0) {
                char tmp[sizeof cmd->params[0]];
                memcpy(tmp, cmd->params[li], sizeof tmp);
                memcpy(cmd->params[li], cmd->params[0], sizeof tmp);
                memcpy(cmd->params[0], tmp, sizeof tmp);
            }
            if (lbr_get_type(cmd, cmd->params[0]) == LBR_TYPE_OBJECT)
                return olb_compress(ctx, cmd);
        }
        dcl_error("DCL", 0, "IVQUAL",
                  "unrecognized qualifier - check validity, spelling, and placement\n \\COMPRESS\\");
        return CLI$_IVQUAL;
    }

    /* OBJECT libraries (.OLB, or /OBJECT) use the real ar-container object-
     * library path so LINK.EXE can pull members (vms-ca9). TEXT/HELP libraries
     * keep the LBRO format below. */
    if (cmd->param_count >= 1 &&
        lbr_get_type(cmd, cmd->params[0]) == LBR_TYPE_OBJECT) {
        return olb_dispatch(ctx, cmd);
    }

    /* TEXT/HELP libraries take no list past P8 (rd vms-465). */
    if (cmd->overflow_count > 0) {
        dcl_error("DCL", 0, "MAXPARM",
                  "too many parameters - reenter command with fewer parameters");
        return CLI$_MAXPARM;
    }

    /* HELP libraries (.HLB, or /HELP) compile a numbered-level .HLP source into
     * a key-indexed .HLB that the HELP reader consumes (vms-01b). */
    if (cmd->param_count >= 1 &&
        lbr_get_type(cmd, cmd->params[0]) == LBR_TYPE_HELP) {
        return hlp_dispatch(ctx, cmd);
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
