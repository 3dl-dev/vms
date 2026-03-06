/*
 * dcl_backup.c - BACKUP command implementation
 *
 * Implements VMS-style BACKUP command with simple saveset format.
 *
 * Usage:
 *   BACKUP input-filespec output.BCK /SAVE_SET   - create saveset
 *   BACKUP input.BCK /SAVE_SET output-directory   - restore saveset
 *   BACKUP /LIST input.BCK /SAVE_SET              - list saveset contents
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <fnmatch.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/cdu.h"
#include "ssdef.h"
#include "vmsfs/filespec.h"

/* External functions from dcl_builtin.c */
extern void dcl_error(const char *facility, int severity, const char *ident,
                      const char *fmt, ...);
extern int dcl_resolve_path(struct dcl_context *ctx, const char *spec,
                            char *linux_path, size_t path_size);
extern int dcl_format_filespec(const char *linux_path, char *vms_spec,
                               size_t spec_size);

/* VMS wildcard match */
extern int vmsfs_wildcard_match(const char *pattern, const char *name);

/* ================================================================== */
/*                     Saveset Format                                  */
/* ================================================================== */

#define BCK_MAGIC       0x42434B00  /* "BCK\0" */
#define BCK_VERSION     1

struct bck_header {
    uint32_t magic;
    uint32_t version;
    uint32_t file_count;
    uint32_t created_date;  /* time_t truncated to 32 bits */
};

/* ================================================================== */
/*                     Helper: check .BCK extension                    */
/* ================================================================== */

static int has_bck_extension(const char *path)
{
    size_t len = strlen(path);
    if (len < 4) return 0;
    const char *ext = path + len - 4;
    return (strcasecmp(ext, ".BCK") == 0);
}

/* ================================================================== */
/*                     BACKUP /SAVE_SET (create)                       */
/* ================================================================== */

static int backup_save(struct dcl_context *ctx, const char *input_spec,
                       const char *output_spec)
{
    char input_path[1024];
    char output_path[1024];

    dcl_resolve_path(ctx, input_spec, input_path, sizeof(input_path));
    dcl_resolve_path(ctx, output_spec, output_path, sizeof(output_path));

    /* Ensure output has .BCK extension */
    if (!has_bck_extension(output_path)) {
        size_t len = strlen(output_path);
        if (len + 4 < sizeof(output_path)) {
            strcat(output_path, ".BCK");
        }
    }

    /* Collect files matching input spec */
    char dir_path[1024];
    const char *pattern;
    char *last_slash = strrchr(input_path, '/');

    if (last_slash) {
        size_t dlen = (size_t)(last_slash - input_path);
        if (dlen >= sizeof(dir_path)) dlen = sizeof(dir_path) - 1;
        memcpy(dir_path, input_path, dlen);
        dir_path[dlen] = '\0';
        pattern = last_slash + 1;
    } else {
        strcpy(dir_path, ".");
        pattern = input_path;
    }

    DIR *dp = opendir(dir_path);
    if (!dp) {
        dcl_error("BACKUP", 2, "OPENIN",
                  "error opening %s - %s", input_spec, strerror(errno));
        return SS$_NOSUCHFILE;
    }

    /* First pass: count matching regular files */
    struct {
        char path[1024];
        char name[256];
    } files[256];
    int file_count = 0;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL && file_count < 256) {
        if (de->d_name[0] == '.') continue;

        /* Match against pattern using fnmatch (case-insensitive) */
        if (fnmatch(pattern, de->d_name, FNM_CASEFOLD) != 0) continue;

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);

        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        strncpy(files[file_count].path, full, sizeof(files[file_count].path) - 1);
        files[file_count].path[sizeof(files[file_count].path) - 1] = '\0';
        strncpy(files[file_count].name, de->d_name, sizeof(files[file_count].name) - 1);
        files[file_count].name[sizeof(files[file_count].name) - 1] = '\0';
        file_count++;
    }
    closedir(dp);

    if (file_count == 0) {
        dcl_error("BACKUP", 2, "NOFILES",
                  "no files found matching %s", input_spec);
        return SS$_NOSUCHFILE;
    }

    /* Create saveset */
    FILE *out = fopen(output_path, "wb");
    if (!out) {
        dcl_error("BACKUP", 2, "OPENOUT",
                  "error creating %s - %s", output_spec, strerror(errno));
        return SS$_FILACCERR;
    }

    /* Write header */
    struct bck_header hdr;
    hdr.magic = BCK_MAGIC;
    hdr.version = BCK_VERSION;
    hdr.file_count = (uint32_t)file_count;
    hdr.created_date = (uint32_t)time(NULL);
    if (fwrite(&hdr, sizeof(hdr), 1, out) != 1) {
        dcl_error("BACKUP", 2, "WRITEERR", "error writing backup header");
        fclose(out);
        return SS$_ABORT;
    }

    /* Write each file entry */
    for (int i = 0; i < file_count; i++) {
        FILE *in = fopen(files[i].path, "rb");
        if (!in) continue;

        struct stat st;
        fstat(fileno(in), &st);
        uint32_t data_len = (uint32_t)st.st_size;

        /* Convert name to uppercase for VMS style */
        char uname[256];
        size_t nlen = strlen(files[i].name);
        for (size_t j = 0; j < nlen && j < sizeof(uname) - 1; j++) {
            uname[j] = (char)toupper((unsigned char)files[i].name[j]);
        }
        uname[nlen < sizeof(uname) ? nlen : sizeof(uname) - 1] = '\0';

        uint32_t name_len = (uint32_t)strlen(uname);

        /* Entry format: name_len(4) + name + data_len(4) + data */
        if (fwrite(&name_len, sizeof(name_len), 1, out) != 1 ||
            fwrite(uname, 1, name_len, out) != name_len ||
            fwrite(&data_len, sizeof(data_len), 1, out) != 1) {
            dcl_error("BACKUP", 2, "WRITEERR", "error writing backup entry");
            fclose(in);
            fclose(out);
            return SS$_ABORT;
        }

        /* Copy file data */
        char buf[8192];
        size_t n;
        int write_err = 0;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
            if (fwrite(buf, 1, n, out) != n) {
                write_err = 1;
                break;
            }
        }
        if (write_err) {
            dcl_error("BACKUP", 2, "WRITEERR", "error writing backup data");
            fclose(in);
            fclose(out);
            return SS$_ABORT;
        }

        fclose(in);

        /* Format VMS-style filespec for output message */
        char vms_spec[512];
        if (dcl_format_filespec(files[i].path, vms_spec, sizeof(vms_spec)) != 0) {
            strncpy(vms_spec, uname, sizeof(vms_spec) - 1);
            vms_spec[sizeof(vms_spec) - 1] = '\0';
        }

        printf("%%BACKUP-S-COPIED, saving %s\n", vms_spec);
    }

    fclose(out);

    printf("%%BACKUP-S-SAVESET, saveset %s created, %d file%s\n",
           output_spec, file_count, file_count == 1 ? "" : "s");

    return SS$_NORMAL;
}

/* ================================================================== */
/*                     BACKUP /LIST (list saveset)                     */
/* ================================================================== */

static int backup_list(struct dcl_context *ctx, const char *saveset_spec)
{
    char saveset_path[1024];
    dcl_resolve_path(ctx, saveset_spec, saveset_path, sizeof(saveset_path));

    /* Ensure .BCK extension */
    if (!has_bck_extension(saveset_path)) {
        size_t len = strlen(saveset_path);
        if (len + 4 < sizeof(saveset_path)) {
            strcat(saveset_path, ".BCK");
        }
    }

    FILE *fp = fopen(saveset_path, "rb");
    if (!fp) {
        dcl_error("BACKUP", 2, "OPENIN",
                  "error opening %s - %s", saveset_spec, strerror(errno));
        return SS$_NOSUCHFILE;
    }

    /* Read header */
    struct bck_header hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1 || hdr.magic != BCK_MAGIC) {
        dcl_error("BACKUP", 2, "INVFMT",
                  "not a valid saveset - %s", saveset_spec);
        fclose(fp);
        return SS$_BADPARAM;
    }

    if (hdr.version != BCK_VERSION) {
        dcl_error("BACKUP", 2, "INVVER",
                  "unsupported saveset version %u", hdr.version);
        fclose(fp);
        return SS$_BADPARAM;
    }

    /* Format creation date */
    time_t created = (time_t)hdr.created_date;
    struct tm *tm = localtime(&created);
    static const char *months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };

    printf("\nListing of save set %s\n", saveset_spec);
    printf("Created %2d-%s-%04d %02d:%02d:%02d\n\n",
           tm->tm_mday, months[tm->tm_mon], 1900 + tm->tm_year,
           tm->tm_hour, tm->tm_min, tm->tm_sec);

    printf("%-40s %10s\n", "File name", "Size");
    printf("%-40s %10s\n", "----------------------------------------",
           "----------");

    uint32_t total_size = 0;

    for (uint32_t i = 0; i < hdr.file_count; i++) {
        uint32_t name_len;
        if (fread(&name_len, sizeof(name_len), 1, fp) != 1) break;
        if (name_len > 255) break;

        char name[256];
        if (fread(name, 1, name_len, fp) != name_len) break;
        name[name_len] = '\0';

        uint32_t data_len;
        if (fread(&data_len, sizeof(data_len), 1, fp) != 1) break;

        printf("%-40s %10u\n", name, data_len);
        total_size += data_len;

        /* Skip file data */
        if (fseek(fp, data_len, SEEK_CUR) != 0) break;
    }

    printf("\nTotal of %u file%s, %u bytes\n",
           hdr.file_count, hdr.file_count == 1 ? "" : "s", total_size);

    fclose(fp);
    return SS$_NORMAL;
}

/* ================================================================== */
/*                     BACKUP /RESTORE (restore saveset)               */
/* ================================================================== */

static int backup_restore(struct dcl_context *ctx, const char *saveset_spec,
                          const char *output_spec)
{
    char saveset_path[1024];
    char output_path[1024];

    dcl_resolve_path(ctx, saveset_spec, saveset_path, sizeof(saveset_path));
    dcl_resolve_path(ctx, output_spec, output_path, sizeof(output_path));

    /* Ensure .BCK extension on saveset */
    if (!has_bck_extension(saveset_path)) {
        size_t len = strlen(saveset_path);
        if (len + 4 < sizeof(saveset_path)) {
            strcat(saveset_path, ".BCK");
        }
    }

    FILE *fp = fopen(saveset_path, "rb");
    if (!fp) {
        dcl_error("BACKUP", 2, "OPENIN",
                  "error opening %s - %s", saveset_spec, strerror(errno));
        return SS$_NOSUCHFILE;
    }

    /* Read header */
    struct bck_header hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1 || hdr.magic != BCK_MAGIC) {
        dcl_error("BACKUP", 2, "INVFMT",
                  "not a valid saveset - %s", saveset_spec);
        fclose(fp);
        return SS$_BADPARAM;
    }

    if (hdr.version != BCK_VERSION) {
        dcl_error("BACKUP", 2, "INVVER",
                  "unsupported saveset version %u", hdr.version);
        fclose(fp);
        return SS$_BADPARAM;
    }

    /* Ensure output directory exists */
    struct stat st;
    if (stat(output_path, &st) != 0) {
        if (mkdir(output_path, 0755) != 0) {
            dcl_error("BACKUP", 2, "CREADIR",
                      "cannot create directory %s - %s",
                      output_spec, strerror(errno));
            fclose(fp);
            return SS$_FILACCERR;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        dcl_error("BACKUP", 2, "NOTDIR",
                  "%s is not a directory", output_spec);
        fclose(fp);
        return SS$_BADPARAM;
    }

    int restored = 0;
    for (uint32_t i = 0; i < hdr.file_count; i++) {
        uint32_t name_len;
        if (fread(&name_len, sizeof(name_len), 1, fp) != 1) break;
        if (name_len > 255) break;

        char name[256];
        if (fread(name, 1, name_len, fp) != name_len) break;
        name[name_len] = '\0';

        uint32_t data_len;
        if (fread(&data_len, sizeof(data_len), 1, fp) != 1) break;

        /* Create output file path (lowercase for Linux) */
        char outfile[1024];
        char lname[256];
        for (size_t j = 0; j <= strlen(name); j++) {
            lname[j] = (char)tolower((unsigned char)name[j]);
        }
        snprintf(outfile, sizeof(outfile), "%s/%s", output_path, lname);

        FILE *outfp = fopen(outfile, "wb");
        if (!outfp) {
            dcl_error("BACKUP", 0, "RESERR",
                      "cannot create %s - %s", name, strerror(errno));
            /* Skip this file's data */
            fseek(fp, data_len, SEEK_CUR);
            continue;
        }

        /* Copy data */
        uint32_t remaining = data_len;
        char buf[8192];
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
            size_t n = fread(buf, 1, chunk, fp);
            if (n == 0) break;
            fwrite(buf, 1, n, outfp);
            remaining -= (uint32_t)n;
        }

        fclose(outfp);
        printf("%%BACKUP-S-RESTORED, restoring %s\n", name);
        restored++;
    }

    fclose(fp);

    printf("%%BACKUP-S-DONE, %d file%s restored\n",
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
