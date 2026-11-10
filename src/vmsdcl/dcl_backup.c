/*
 * dcl_backup.c - BACKUP command implementation
 *
 * Implements VMS-style BACKUP command with simple saveset format.
 *
 * Usage:
 *   BACKUP input-filespec output.BCK /SAVE_SET   - create saveset
 *   BACKUP input.BCK /SAVE_SET output-directory   - restore saveset
 *   BACKUP /LIST input.BCK /SAVE_SET              - list saveset contents
 *
 * Every file is reached through RMS (libc-drop vms-539): the input files by a
 * $PARSE/$SEARCH wildcard walk and block-I/O $READ, the saveset by block-I/O
 * $READ/$WRITE, each restored file by $CREATE (with the record format it was
 * saved with) and block $WRITE. A file RMS cannot reach is reported with its
 * RMS reason, as VMS does ("-RMS-E-FNF, file not found").
 */

#include "dcl/dcl_out.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/cdu.h"
#include "dcl/dcl_rms.h"
#include "ssdef.h"
#include "ovmx_status.h"    /* vms_status_string: the RMS reason line */

/* External functions from dcl_builtin.c */
extern void dcl_error(const char *facility, int severity, const char *ident,
                      const char *fmt, ...);
/* CREATE/DIRECTORY's primitive (dcl_cmd_file.c): IO$_CREATE of each NAME.DIR
 * over the Files-11 ACP -- a restore's missing output directory. */
extern uint32_t dcl_mkdir_acp(const char *vspec);

/* ================================================================== */
/*                     Saveset Format                                  */
/* ================================================================== */

#define BCK_MAGIC       0x42434B00  /* "BCK\0" */
#define BCK_VERSION     2           /* 2: each entry carries its RFM/RAT/MRS */

struct bck_header {
    uint32_t magic;
    uint32_t version;
    uint32_t file_count;
    uint32_t created_date;  /* time_t truncated to 32 bits */
};

/*
 * Entry: name_len(4) name, then (version 2) rfm(1) rat(1) mrs(2), then
 * data_len(4) data. The data is the file's bytes as block I/O reads them; the
 * record attributes let a restore $CREATE the file with the format its bytes
 * are framed in. A version 1 entry carries no attributes: its files were saved
 * from stream-LF text, and restore as that.
 */
struct bck_attr {
    uint8_t  rfm;
    uint8_t  rat;
    uint16_t mrs;
};

/* The RMS reason for a failure, on its own line ("-RMS-E-FNF, file not found"). */
static void bck_rms_reason(uint32_t st)
{
    char sec[256];
    if (st != 0 && vms_status_string(st, sec, sizeof sec) > 0 && sec[0] == '%') {
        sec[0] = '-';
        dcl_err_printf("%s\n", sec);
    }
}

static int bck_openin_failed(const char *spec, uint32_t st)
{
    dcl_error("BACKUP", 2, "OPENIN", "error opening %s as input", spec);
    bck_rms_reason(st);
    return (int)(st ? st : SS$_NOSUCHFILE);
}

static int bck_openout_failed(const char *spec, uint32_t st)
{
    dcl_error("BACKUP", 2, "OPENOUT", "error opening %s as output", spec);
    bck_rms_reason(st);
    return (int)(st ? st : SS$_FILACCERR);
}

static int bck_write_failed(struct dcl_rms_bio *out)
{
    uint32_t st = dcl_rms_bio_status(out);
    dcl_error("BACKUP", 2, "WRITEERR", "error writing %s", dcl_rms_bio_spec(out));
    bck_rms_reason(st);
    dcl_rms_bio_close(out);
    return (int)(st & 1 ? SS$_ABORT : st);
}

/* The spec names a .BCK saveset (the dispatcher's save/restore choice). */
static int has_bck_extension(const char *path)
{
    size_t len = strlen(path);
    if (len < 4) return 0;
    return strcasecmp(path + len - 4, ".BCK") == 0;
}

/* NAME.TYP of a resultant "DEV:[DIR]NAME.TYP;VER", upper case, no version. */
static void bck_name_of(const char *spec, char *out, size_t outsz)
{
    const char *name = spec;
    for (const char *p = spec; *p; p++)
        if (*p == ']' || *p == '>' || *p == ':')
            name = p + 1;
    size_t n = 0;                 /* no strcspn: not in the VMS-native DCL link's surface */
    while (name[n] && name[n] != ';')
        n++;
    if (n >= outsz) n = outsz - 1;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)toupper((unsigned char)name[i]);
    out[n] = '\0';
}

/* Read the whole of `spec` by block I/O into a malloc'd buffer. RMS status. */
static uint32_t bck_read_file(struct dcl_context *ctx, const char *spec,
                              char **data, uint32_t *len)
{
    uint32_t st = 0;
    struct dcl_rms_bio *in = dcl_rms_bio_open(ctx, spec, NULL, &st);
    *data = NULL;
    *len = 0;
    if (!in)
        return st;
    size_t cap = 0, n = 0;
    char *buf = NULL;
    for (;;) {
        if (n + 32768 > cap) {
            size_t ncap = cap ? cap * 2 : 65536;
            char *nb = realloc(buf, ncap);
            if (!nb) { free(buf); dcl_rms_bio_close(in); return RMS$_DME; }
            buf = nb;
            cap = ncap;
        }
        long got = dcl_rms_bio_read(in, buf + n, 32768);
        if (got < 0) {
            st = dcl_rms_bio_status(in);
            free(buf);
            dcl_rms_bio_close(in);
            return st;
        }
        n += (size_t)got;
        if (got < 32768)
            break;
        if (n > 0xFFFFFFFFu - 65536) {            /* entry length is 32 bits */
            free(buf);
            dcl_rms_bio_close(in);
            return RMS$_DME;
        }
    }
    dcl_rms_bio_close(in);
    *data = buf;
    *len = (uint32_t)n;
    return RMS$_NORMAL;
}

/* ================================================================== */
/*                     BACKUP /SAVE_SET (create)                       */
/* ================================================================== */

#define BCK_MAX_FILES 256

static int backup_save(struct dcl_context *ctx, const char *input_spec,
                       const char *output_spec)
{
    /* The input files: a $PARSE/$SEARCH walk of the (wildcard) input spec. */
    char (*files)[1024] = calloc(BCK_MAX_FILES, sizeof(*files));
    struct bck_attr *attrs = calloc(BCK_MAX_FILES, sizeof(*attrs));
    if (!files || !attrs) {
        free(files); free(attrs);
        return SS$_INSFMEM;
    }
    int file_count = 0;
    char eff[1024];
    struct dcl_rms_dir *d = NULL;
    if (dcl_rms_effective_spec(ctx, input_spec, eff, sizeof(eff)) == 0)
        d = dcl_rms_dir_open(ctx, eff);
    if (!d) {
        free(files); free(attrs);
        return bck_openin_failed(input_spec, RMS$_SYN);
    }
    char spec[1024];
    while (file_count < BCK_MAX_FILES &&
           dcl_rms_dir_next(d, spec, sizeof(spec), NULL, NULL, NULL)) {
        struct rms_fileattr fa;
        if (rms_file_attr(spec, &fa) != RMS$_NORMAL || fa.is_directory)
            continue;                              /* a directory file is no data */
        snprintf(files[file_count], sizeof(files[0]), "%s", spec);
        attrs[file_count].rfm = fa.rfm;
        attrs[file_count].rat = fa.rat;
        attrs[file_count].mrs = fa.mrs;
        file_count++;
    }
    uint32_t dst = dcl_rms_dir_status(d);
    dcl_rms_dir_close(d);

    if (file_count == 0) {
        free(files); free(attrs);
        if (dst != RMS$_NMF && dst != RMS$_FNF && dst != 0 && !(dst & 1))
            return bck_openin_failed(input_spec, dst);
        dcl_error("BACKUP", 2, "NOFILES",
                  "no files found matching %s", input_spec);
        return SS$_NOSUCHFILE;
    }

    /* Create the saveset (default type .BCK) for block writes. */
    uint32_t st = 0;
    struct dcl_rms_bio *out = dcl_rms_bio_create(ctx, output_spec, ".BCK",
                                                 FAB$C_FIX, 0, 512, &st);
    if (!out) {
        free(files); free(attrs);
        return bck_openout_failed(output_spec, st);
    }

    struct bck_header hdr;
    hdr.magic = BCK_MAGIC;
    hdr.version = BCK_VERSION;
    hdr.file_count = (uint32_t)file_count;
    hdr.created_date = (uint32_t)time(NULL);
    if (dcl_rms_bio_write(out, &hdr, sizeof(hdr)) != 0) {
        free(files); free(attrs);
        return bck_write_failed(out);
    }

    int saved = 0;
    for (int i = 0; i < file_count; i++) {
        char *data = NULL;
        uint32_t data_len = 0;
        st = bck_read_file(ctx, files[i], &data, &data_len);
        if (!(st & 1)) {
            /* the count in the header is the files the walk found: keep the
             * saveset well-formed with an empty entry, and say why */
            dcl_error("BACKUP", 2, "OPENIN", "error opening %s as input", files[i]);
            bck_rms_reason(st);
            data_len = 0;
        }

        char uname[256];
        bck_name_of(files[i], uname, sizeof(uname));
        uint32_t name_len = (uint32_t)strlen(uname);
        uint8_t at[4] = { attrs[i].rfm, attrs[i].rat,
                          (uint8_t)(attrs[i].mrs & 0xFF), (uint8_t)(attrs[i].mrs >> 8) };

        if (dcl_rms_bio_write(out, &name_len, sizeof(name_len)) != 0 ||
            dcl_rms_bio_write(out, uname, name_len) != 0 ||
            dcl_rms_bio_write(out, at, sizeof(at)) != 0 ||
            dcl_rms_bio_write(out, &data_len, sizeof(data_len)) != 0 ||
            (data_len && dcl_rms_bio_write(out, data, data_len) != 0)) {
            free(data); free(files); free(attrs);
            return bck_write_failed(out);
        }
        free(data);
        if (st & 1) {
            dcl_out_printf("%%BACKUP-S-COPIED, saving %s\n", files[i]);
            saved++;
        }
    }

    st = dcl_rms_bio_close(out);
    free(files); free(attrs);
    if (!(st & 1)) {
        dcl_error("BACKUP", 2, "WRITEERR", "error closing %s", output_spec);
        bck_rms_reason(st);
        return (int)st;
    }

    dcl_out_printf("%%BACKUP-S-SAVESET, saveset %s created, %d file%s\n",
           output_spec, saved, saved == 1 ? "" : "s");

    return SS$_NORMAL;
}

/* ================================================================== */
/*                     Saveset reading (shared by /LIST and restore)   */
/* ================================================================== */

/* Open a saveset and read its header. NULL (message issued, *rc set) on error. */
static struct dcl_rms_bio *bck_open_saveset(struct dcl_context *ctx, const char *spec,
                                            struct bck_header *hdr, int *rc)
{
    uint32_t st = 0;
    struct dcl_rms_bio *in = dcl_rms_bio_open(ctx, spec, ".BCK", &st);
    if (!in) {
        *rc = bck_openin_failed(spec, st);
        return NULL;
    }
    if (dcl_rms_bio_read(in, hdr, sizeof(*hdr)) != (long)sizeof(*hdr) ||
        hdr->magic != BCK_MAGIC) {
        dcl_error("BACKUP", 2, "INVFMT", "not a valid saveset - %s", spec);
        dcl_rms_bio_close(in);
        *rc = SS$_BADPARAM;
        return NULL;
    }
    if (hdr->version != 1 && hdr->version != BCK_VERSION) {
        dcl_error("BACKUP", 2, "INVVER",
                  "unsupported saveset version %u", hdr->version);
        dcl_rms_bio_close(in);
        *rc = SS$_BADPARAM;
        return NULL;
    }
    return in;
}

/* The next entry's name, attributes and data length. 0 at a short read. */
static int bck_read_entry(struct dcl_rms_bio *in, uint32_t version, char name[256],
                          struct bck_attr *attr, uint32_t *data_len)
{
    uint32_t name_len;
    if (dcl_rms_bio_read(in, &name_len, sizeof(name_len)) != (long)sizeof(name_len) ||
        name_len > 255)
        return 0;
    if (dcl_rms_bio_read(in, name, name_len) != (long)name_len)
        return 0;
    name[name_len] = '\0';
    attr->rfm = FAB$C_STMLF;
    attr->rat = FAB$M_CR;
    attr->mrs = 0;
    if (version >= 2) {
        uint8_t at[4];
        if (dcl_rms_bio_read(in, at, sizeof(at)) != (long)sizeof(at))
            return 0;
        attr->rfm = at[0];
        attr->rat = at[1];
        attr->mrs = (uint16_t)(at[2] | (at[3] << 8));
    }
    return dcl_rms_bio_read(in, data_len, sizeof(*data_len)) == (long)sizeof(*data_len);
}

/* Read past `len` bytes of entry data. 0 at a short read. */
static int bck_skip(struct dcl_rms_bio *in, uint32_t len)
{
    char buf[8192];
    while (len > 0) {
        size_t chunk = len < sizeof(buf) ? len : sizeof(buf);
        if (dcl_rms_bio_read(in, buf, chunk) != (long)chunk)
            return 0;
        len -= (uint32_t)chunk;
    }
    return 1;
}

/* ================================================================== */
/*                     BACKUP /LIST (list saveset)                     */
/* ================================================================== */

static int backup_list(struct dcl_context *ctx, const char *saveset_spec)
{
    struct bck_header hdr;
    int rc = SS$_NORMAL;
    struct dcl_rms_bio *in = bck_open_saveset(ctx, saveset_spec, &hdr, &rc);
    if (!in)
        return rc;

    /* Format creation date */
    time_t created = (time_t)hdr.created_date;
    struct tm *tm = localtime(&created);
    static const char *months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };

    dcl_out_printf("\nListing of save set %s\n", saveset_spec);
    dcl_out_printf("Created %2d-%s-%04d %02d:%02d:%02d\n\n",
           tm->tm_mday, months[tm->tm_mon], 1900 + tm->tm_year,
           tm->tm_hour, tm->tm_min, tm->tm_sec);

    dcl_out_printf("%-40s %10s\n", "File name", "Size");
    dcl_out_printf("%-40s %10s\n", "----------------------------------------",
           "----------");

    uint32_t total_size = 0;

    for (uint32_t i = 0; i < hdr.file_count; i++) {
        char name[256];
        struct bck_attr attr;
        uint32_t data_len;
        if (!bck_read_entry(in, hdr.version, name, &attr, &data_len))
            break;

        dcl_out_printf("%-40s %10u\n", name, data_len);
        total_size += data_len;

        if (!bck_skip(in, data_len))
            break;
    }

    dcl_out_printf("\nTotal of %u file%s, %u bytes\n",
           hdr.file_count, hdr.file_count == 1 ? "" : "s", total_size);

    dcl_rms_bio_close(in);
    return SS$_NORMAL;
}

/* ================================================================== */
/*                     BACKUP /RESTORE (restore saveset)               */
/* ================================================================== */

/* "DEV:[DIR]" of the output spec: everything through its last ']' '>' or ':'.
 * 0 when the spec names no directory. */
static int bck_out_dir(struct dcl_context *ctx, const char *spec, char *out, size_t outsz)
{
    if (dcl_rms_effective_spec(ctx, spec, out, outsz) != 0)
        return 0;
    char *end = NULL;
    for (char *p = out; *p; p++)
        if (*p == ']' || *p == '>' || *p == ':')
            end = p;
    if (!end)
        return 0;
    end[1] = '\0';
    return 1;
}

static int backup_restore(struct dcl_context *ctx, const char *saveset_spec,
                          const char *output_spec)
{
    char outdir[1024];
    if (!bck_out_dir(ctx, output_spec, outdir, sizeof(outdir))) {
        dcl_error("BACKUP", 2, "NOTDIR", "%s is not a directory", output_spec);
        return SS$_BADPARAM;
    }

    struct bck_header hdr;
    int rc = SS$_NORMAL;
    struct dcl_rms_bio *in = bck_open_saveset(ctx, saveset_spec, &hdr, &rc);
    if (!in)
        return rc;

    int restored = 0, made_dir = 0;
    for (uint32_t i = 0; i < hdr.file_count; i++) {
        char name[256];
        struct bck_attr attr;
        uint32_t data_len;
        if (!bck_read_entry(in, hdr.version, name, &attr, &data_len))
            break;

        char outfile[1300];
        snprintf(outfile, sizeof(outfile), "%s%s", outdir, name);

        uint32_t st = 0;
        struct dcl_rms_bio *o = dcl_rms_bio_create(ctx, outfile, NULL, attr.rfm,
                                                   attr.rat, attr.mrs, &st);
        if (!o && st == RMS$_DNF && !made_dir) {
            /* the output directory does not exist: create it, as BACKUP does
             * (over the ACP; with no executive this is its honest status) */
            uint32_t cst = dcl_mkdir_acp(outdir);
            made_dir = 1;
            if (cst & 1)
                o = dcl_rms_bio_create(ctx, outfile, NULL, attr.rfm, attr.rat,
                                       attr.mrs, &st);
            else
                st = cst;
        }
        if (!o) {
            dcl_error("BACKUP", 2, "OPENOUT", "error opening %s as output", outfile);
            bck_rms_reason(st);
            if (!bck_skip(in, data_len))
                break;
            continue;
        }

        /* Copy data */
        uint32_t remaining = data_len;
        char buf[8192];
        int ok = 1;
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
            long n = dcl_rms_bio_read(in, buf, chunk);
            if (n <= 0) { ok = 0; break; }
            if (dcl_rms_bio_write(o, buf, (size_t)n) != 0) {
                dcl_error("BACKUP", 2, "WRITEERR", "error writing %s", outfile);
                bck_rms_reason(dcl_rms_bio_status(o));
                ok = 0;
                break;
            }
            remaining -= (uint32_t)n;
        }

        st = dcl_rms_bio_close(o);
        if (!ok || !(st & 1)) {
            if (ok) {
                dcl_error("BACKUP", 2, "WRITEERR", "error closing %s", outfile);
                bck_rms_reason(st);
            }
            break;
        }
        dcl_out_printf("%%BACKUP-S-RESTORED, restoring %s\n", name);
        restored++;
    }

    dcl_rms_bio_close(in);

    dcl_out_printf("%%BACKUP-S-DONE, %d file%s restored\n",
           restored, restored == 1 ? "" : "s");

    return SS$_NORMAL;
}

/* ================================================================== */
/*                     Main BACKUP command dispatcher                   */
/* ================================================================== */

int cmd_backup(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    int has_save    = dcl_has_qualifier(cmd, "SAVE_SET");
    int has_list    = dcl_has_qualifier(cmd, "LIST");

    /*
     * BACKUP /LIST input.BCK/SAVE_SET
     * - list saveset contents
     */
    if (has_list) {
        const char *saveset = NULL;
        if (cmd->param_count >= 1) {
            saveset = cmd->params[0];
        } else {
            dcl_error("BACKUP", 2, "NOFILE",
                      "missing saveset filespec");
            return SS$_BADPARAM;
        }
        return backup_list(ctx, saveset);
    }

    /*
     * Determine save vs restore by looking at which parameter
     * has the .BCK extension or /SAVE_SET qualifier context.
     *
     * BACKUP input-filespec output.BCK /SAVE_SET  - save
     * BACKUP input.BCK /SAVE_SET output-dir       - restore
     */
    if (cmd->param_count < 2) {
        dcl_error("BACKUP", 2, "NOFILE",
                  "missing input and/or output specification");
        return SS$_BADPARAM;
    }

    /* If the first param looks like a .BCK saveset, it's a restore */
    if (has_bck_extension(cmd->params[0])) {
        return backup_restore(ctx, cmd->params[0], cmd->params[1]);
    }

    /* If second param has .BCK, it's a save */
    if (has_bck_extension(cmd->params[1]) || has_save) {
        return backup_save(ctx, cmd->params[0], cmd->params[1]);
    }

    /* Ambiguous */
    dcl_error("BACKUP", 2, "SYNTAX",
              "cannot determine save or restore mode; "
              "use .BCK extension or /SAVE_SET qualifier");
    return SS$_BADPARAM;
}
