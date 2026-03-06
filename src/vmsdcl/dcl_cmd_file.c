/*
 * dcl_cmd_file.c - DCL file operation command implementations
 *
 * DIRECTORY, TYPE, COPY, DELETE, RENAME, CREATE, SEARCH, PURGE, APPEND
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "dcl/dcl_cmd.h"
#include "ssdef.h"
#include "vmsfs/filespec.h"
#include "vmsqueue.h"

int cmd_directory(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* Determine the directory to list */
    char linux_dir[1024];
    const char *pattern = NULL;

    if (cmd->param_count >= 1 && cmd->params[0][0] != '\0') {
        dcl_resolve_path(ctx, cmd->params[0], linux_dir, sizeof(linux_dir));
        /* Check if this is a directory or a file pattern */
        struct stat st;
        if (stat(linux_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            /* It's a directory */
        } else {
            /* Might be a wildcard pattern - split dir and pattern */
            char *last_slash = strrchr(linux_dir, '/');
            if (last_slash) {
                pattern = strdup(last_slash + 1);
                *(last_slash + 1) = '\0';
            } else {
                pattern = strdup(linux_dir);
                vmsfs_to_linux_path(ctx->default_dir, linux_dir, sizeof(linux_dir));
            }
        }
    } else {
        vmsfs_to_linux_path(ctx->default_dir, linux_dir, sizeof(linux_dir));
    }

    /* Ensure trailing slash */
    size_t dlen = strlen(linux_dir);
    if (dlen > 0 && linux_dir[dlen - 1] != '/') {
        if (dlen < sizeof(linux_dir) - 1) {
            linux_dir[dlen] = '/';
            linux_dir[dlen + 1] = '\0';
        }
    }

    /* Check qualifiers */
    int show_size = dcl_has_qualifier(cmd, "SIZE");
    int show_date = dcl_has_qualifier(cmd, "DATE");
    int show_full = dcl_has_qualifier(cmd, "FULL");
    int show_brief = dcl_has_qualifier(cmd, "BRIEF");
    int show_owner = dcl_has_qualifier(cmd, "OWNER");
    int show_total = dcl_has_qualifier(cmd, "TOTAL");
    int show_grand_total = dcl_has_qualifier(cmd, "GRAND_TOTAL");
    /* /HEADING is default on; /NOHEADING suppresses it.
     * The parser stores /NOHEADING as name="HEADING" negated=1,
     * and dcl_has_qualifier returns 0 for negated qualifiers.
     * So: if HEADING qualifier is absent → show (default).
     *     if HEADING qualifier is present and not negated → show.
     *     if HEADING qualifier is present and negated → hide.
     * We detect negation by scanning the qualifiers directly. */
    int show_heading = 1;
    for (int qi = 0; qi < cmd->qualifier_count; qi++) {
        if (strcasecmp(cmd->qualifiers[qi].name, "HEADING") == 0) {
            show_heading = !cmd->qualifiers[qi].negated;
            break;
        }
    }
    int show_trailing = dcl_has_qualifier(cmd, "TRAILING");
    int columns = 4;
    const char *col_val = dcl_qualifier_value(cmd, "COLUMNS");
    if (col_val && col_val[0]) {
        char *endp;
        int c = (int)strtol(col_val, &endp, 10);
        if (endp != col_val && *endp == '\0') columns = c;
    }
    if (columns < 1) columns = 1;
    if (columns > 8) columns = 8;

    if (show_full) {
        show_size = 1;
        show_date = 1;
        show_owner = 1;
    }

    /* If /TOTAL or /GRAND_TOTAL, suppress individual file listing */
    int suppress_files = show_total || show_grand_total;

    /* Display header */
    char vms_dir[512];
    /* Remove trailing slash for display */
    char display_dir[1024];
    strncpy(display_dir, linux_dir, sizeof(display_dir) - 1);
    display_dir[sizeof(display_dir) - 1] = '\0';
    size_t ddlen = strlen(display_dir);
    if (ddlen > 1 && display_dir[ddlen - 1] == '/') {
        display_dir[ddlen - 1] = '\0';
    }
    dcl_format_directory(display_dir, vms_dir, sizeof(vms_dir));
    if (show_heading) {
        printf("\nDirectory %s\n\n", vms_dir);
    }

    /* Read directory entries */
    DIR *dir = opendir(linux_dir);
    if (!dir) {
        dcl_error("RMS", 2, "DNF",
                  "directory not found - %s", linux_dir);
        if (pattern) free((void *)pattern);
        return SS$_NOSUCHFILE;
    }

    /* Collect all matching entries for sorting */
    struct dir_entry {
        char vms_name[256];  /* Formatted VMS name (UPPERCASE, with version) */
        char raw_name[256];  /* Original d_name for name part comparison */
        int  version;        /* Numeric version for sort (descending) */
        long blocks;
        struct stat st;
    };

    int capacity = 256;
    struct dir_entry *entries = malloc((size_t)capacity * sizeof(*entries));
    if (!entries) {
        closedir(dir);
        if (pattern) free((void *)pattern);
        return SS$_INSFMEM;
    }
    int entry_count = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) continue;

        /* Apply wildcard filter if pattern specified.
         * Use vmsfs_wildcard_match() which handles VMS % (single-char) and * */
        if (pattern) {
            if (!vmsfs_wildcard_match(pattern, de->d_name)) continue;
        }

        /* Stat the file */
        char full_path[2048];
        snprintf(full_path, sizeof(full_path), "%s%s", linux_dir, de->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        /* Grow array if needed */
        if (entry_count >= capacity) {
            capacity *= 2;
            struct dir_entry *tmp = realloc(entries,
                                            (size_t)capacity * sizeof(*entries));
            if (!tmp) {
                free(entries);
                closedir(dir);
                if (pattern) free((void *)pattern);
                return SS$_INSFMEM;
            }
            entries = tmp;
        }

        struct dir_entry *e = &entries[entry_count];
        e->st = st;
        e->blocks = (st.st_size + 511) / 512;
        strncpy(e->raw_name, de->d_name, sizeof(e->raw_name) - 1);
        e->raw_name[sizeof(e->raw_name) - 1] = '\0';

        /* Format the filename in VMS style (uppercase) */
        size_t ni = 0;
        for (size_t i = 0; de->d_name[i] && ni < sizeof(e->vms_name) - 1; i++) {
            e->vms_name[ni++] = (char)toupper((unsigned char)de->d_name[i]);
        }
        e->vms_name[ni] = '\0';

        /* Determine version number.
         * If the filename already contains ;N, extract it.
         * Otherwise append ;1 for regular files. */
        char *semi = strrchr(e->vms_name, ';');
        if (semi && semi[1] != '\0') {
            /* Already has a version suffix — use it */
            e->version = (int)strtol(semi + 1, NULL, 10);
            /* Don't double-add: nothing to append */
        } else if (S_ISREG(st.st_mode)) {
            /* No version suffix — add ;1 */
            e->version = 1;
            strncat(e->vms_name, ";1",
                    sizeof(e->vms_name) - strlen(e->vms_name) - 1);
        } else {
            e->version = 0;  /* Directories don't have versions per se */
        }

        /* Add .DIR;1 suffix for subdirectories */
        if (S_ISDIR(st.st_mode)) {
            strncat(e->vms_name, ".DIR;1",
                    sizeof(e->vms_name) - strlen(e->vms_name) - 1);
        }

        /* Ensure a dot separator for regular files without one.
         * Insert '.' before ';1' so "FOO;1" becomes "FOO.;1". */
        if (S_ISREG(st.st_mode) && !strchr(de->d_name, '.')) {
            char *s = strrchr(e->vms_name, ';');
            if (s) {
                memmove(s + 1, s, strlen(s) + 1);
                *s = '.';
            }
        }

        entry_count++;
    }
    closedir(dir);

    /* Sort entries: name ascending (ignoring version), version descending.
     * VMS displays files alphabetically, with multiple versions of the same
     * file in descending version order.
     * Sort by vms_name up to (not including) ';', then version descending. */
    for (int i = 0; i < entry_count - 1; i++) {
        for (int j = i + 1; j < entry_count; j++) {
            /* Get name without version for comparison */
            char na[256], nb[256];
            strncpy(na, entries[i].vms_name, sizeof(na) - 1); na[sizeof(na)-1]='\0';
            strncpy(nb, entries[j].vms_name, sizeof(nb) - 1); nb[sizeof(nb)-1]='\0';
            char *sa = strrchr(na, ';'); if (sa) *sa = '\0';
            char *sb = strrchr(nb, ';'); if (sb) *sb = '\0';

            int cmp = strcasecmp(na, nb);
            if (cmp > 0 || (cmp == 0 && entries[i].version < entries[j].version)) {
                struct dir_entry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }

    int file_count = 0;
    long total_blocks = 0;
    int col = 0;
    int col_width = (show_size || show_date) ? 0 : (80 / columns);

    for (int idx = 0; idx < entry_count; idx++) {
        struct dir_entry *e = &entries[idx];
        const char *vms_name = e->vms_name;
        long blocks = e->blocks;
        struct stat *st = &e->st;

        total_blocks += blocks;
        file_count++;

        /* If /TOTAL or /GRAND_TOTAL, skip individual file display */
        if (suppress_files) continue;

        if (show_full) {
            /* Full listing: one file per line with all info */
            printf("%-39s", vms_name);
            printf(" %6ld", blocks);

            struct tm tm;
            localtime_r(&st->st_mtime, &tm);
            printf("  %2d-%s-%04d %02d:%02d:%02d.00",
                   tm.tm_mday, vms_months[tm.tm_mon],
                   1900 + tm.tm_year, tm.tm_hour, tm.tm_min, tm.tm_sec);

            /* Protection: use vmsfs functions for proper VMS format */
            uint16_t vprot = vmsfs_mode_to_protection(st->st_mode);
            char prot_buf[64];
            vmsfs_format_protection(vprot, prot_buf, sizeof(prot_buf));
            printf(" %s", prot_buf);

            /* /OWNER: show file owner UIC */
            if (show_owner) {
                printf(" [%03o,%03o]",
                       (unsigned)(st->st_gid & 0377),
                       (unsigned)(st->st_uid & 0377));
            }
            printf("\n");
        } else if (show_size || show_date || show_owner) {
            /* Size and/or date and/or owner */
            printf("%-39s", vms_name);
            if (show_size) {
                printf(" %6ld", blocks);
            }
            if (show_date) {
                struct tm tm;
                localtime_r(&st->st_mtime, &tm);
                printf("  %2d-%s-%04d %02d:%02d:%02d.00",
                       tm.tm_mday, vms_months[tm.tm_mon],
                       1900 + tm.tm_year, tm.tm_hour, tm.tm_min, tm.tm_sec);
            }
            if (show_owner) {
                printf(" [%03o,%03o]",
                       (unsigned)(st->st_gid & 0377),
                       (unsigned)(st->st_uid & 0377));
            }
            printf("\n");
        } else if (show_brief) {
            /* Brief: just filename */
            printf("%s\n", vms_name);
        } else {
            /* Columnar output */
            if (col_width < 1) col_width = 20;
            printf("%-*s", col_width, vms_name);
            col++;
            if (col >= columns) {
                printf("\n");
                col = 0;
            }
        }
    }

    free(entries);

    /* Finish last line of columnar output */
    if (col > 0 && !show_size && !show_date && !show_full && !show_brief &&
        !show_owner && !suppress_files) {
        printf("\n");
    }

    /* Footer */
    if (show_grand_total) {
        printf("\nGrand total of %d directory, %d file%s, %ld block%s.\n",
               1, file_count, file_count != 1 ? "s" : "",
               total_blocks, total_blocks != 1 ? "s" : "");
    } else {
        printf("\nTotal of %d file%s, %ld block%s.\n",
               file_count, file_count != 1 ? "s" : "",
               total_blocks, total_blocks != 1 ? "s" : "");
    }

    /* /TRAILING: show trailing directory spec */
    if (show_trailing) {
        printf("\nTotal of %d file%s, %ld block%s.\n%s\n",
               file_count, file_count != 1 ? "s" : "",
               total_blocks, total_blocks != 1 ? "s" : "",
               vms_dir);
    }

    if (pattern) free((void *)pattern);
    return SS$_NORMAL;
}

/*
 * TYPE - Display file contents.
 */
int cmd_type(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    FILE *fp = fopen(linux_path, "r");
    if (!fp) {
        dcl_error("RMS", 2, "FNF",
                  "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    /* Check for /PAGE qualifier */
    int paged = dcl_has_qualifier(cmd, "PAGE");
    int line_count = 0;
    int page_size = 24;

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        fputs(line, stdout);
        line_count++;

        if (paged && line_count >= page_size) {
            printf("Press RETURN to continue...");
            fflush(stdout);
            char buf[64];
            if (!fgets(buf, sizeof(buf), stdin)) break;
            line_count = 0;
        }
    }

    fclose(fp);
    return SS$_NORMAL;
}

/*
 * COPY - Copy a file.
 */
int cmd_copy(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE", "missing source and/or destination");
        return SS$_BADPARAM;
    }

    char src_path[1024], dst_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], src_path, sizeof(src_path));
    dcl_resolve_path(ctx, cmd->params[1], dst_path, sizeof(dst_path));

    /* Check if destination is a directory */
    struct stat st;
    if (stat(dst_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* Append source filename to destination directory */
        const char *basename = strrchr(src_path, '/');
        if (basename) basename++; else basename = src_path;
        size_t dlen = strlen(dst_path);
        if (dlen > 0 && dst_path[dlen - 1] != '/') {
            strncat(dst_path, "/", sizeof(dst_path) - dlen - 1);
        }
        strncat(dst_path, basename, sizeof(dst_path) - strlen(dst_path) - 1);
    }

    FILE *src = fopen(src_path, "rb");
    if (!src) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    FILE *dst = fopen(dst_path, "wb");
    if (!dst) {
        fclose(src);
        dcl_error("RMS", 2, "CRE", "cannot create - %s", cmd->params[1]);
        return SS$_FILACCERR;
    }

    char buf[8192];
    size_t n;
    int write_err = 0;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            write_err = 1;
            break;
        }
    }

    fclose(src);
    fclose(dst);

    if (write_err) {
        dcl_error("RMS", 2, "WER", "write error - %s", cmd->params[1]);
        return SS$_ABORT;
    }

    /* /LOG qualifier: report the copy */
    if (dcl_has_qualifier(cmd, "LOG")) {
        char vms_src[256], vms_dst[256];
        dcl_format_filespec(src_path, vms_src, sizeof(vms_src));
        dcl_format_filespec(dst_path, vms_dst, sizeof(vms_dst));
        dcl_error("COPY", 1, "COPIED", "%s copied to %s", vms_src, vms_dst);
    }

    return SS$_NORMAL;
}

/*
 * DELETE - Delete a file, or (with /SYMBOL) delete a symbol.
 */
int cmd_delete(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* /ENTRY=n qualifier: delete a queue entry */
    if (dcl_has_qualifier(cmd, "ENTRY")) {
        const char *entry_str = dcl_qualifier_value(cmd, "ENTRY");
        if (!entry_str || !entry_str[0]) {
            dcl_error("DCL", 2, "NOENTRY", "missing entry number with /ENTRY");
            return SS$_BADPARAM;
        }
        char *endptr;
        long entry_val = strtol(entry_str, &endptr, 10);
        if (endptr == entry_str || *endptr != '\0' || entry_val <= 0) {
            dcl_error("DCL", 2, "BADENTRY", "invalid entry number - %s", entry_str);
            return SS$_BADPARAM;
        }
        uint32_t entry_id = (uint32_t)entry_val;
        int qsts = ensure_queue_init();
        if (!(qsts & 1)) {
            dcl_error("DELETE", 2, "QMANERR", "queue manager initialization failed");
            return qsts;
        }
        qsts = vmsq_delete_entry(entry_id);
        if (!(qsts & 1)) {
            dcl_error("DELETE", 2, "ENTNOTFND", "entry %u not found", entry_id);
            return qsts;
        }
        printf("%%DELETE-S-DELETED, entry %u deleted\n", entry_id);
        return SS$_NORMAL;
    }

    /* /SYMBOL qualifier: delete a symbol from the symbol table */
    if (dcl_has_qualifier(cmd, "SYMBOL")) {
        if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
            dcl_error("DCL", 2, "NOSYM", "missing symbol name");
            return SS$_BADPARAM;
        }

        int scope = DCL_SYM_LOCAL;
        if (dcl_has_qualifier(cmd, "GLOBAL")) {
            scope = DCL_SYM_GLOBAL;
        }

        int ret = dcl_sym_delete(cmd->params[0], scope);
        if (ret != 0) {
            dcl_error("DCL", 0, "NOSUCHSYM",
                      "no symbol \"%s\" found", cmd->params[0]);
        }
        return SS$_NORMAL;
    }

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    /* /CONFIRM prompts before each delete; /NOCONFIRM suppresses.
     * The parser stores /NOCONFIRM as name="CONFIRM" negated=1,
     * so dcl_has_qualifier(cmd, "CONFIRM") returns 0 for /NOCONFIRM. */
    int do_confirm = dcl_has_qualifier(cmd, "CONFIRM");
    int do_log = dcl_has_qualifier(cmd, "LOG");

    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    /* Check if the path contains wildcards */
    if (strchr(linux_path, '*') || strchr(linux_path, '?')) {
        /* Wildcard delete - expand and delete matching files */
        char dir[1024];
        strncpy(dir, linux_path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        char *last_slash = strrchr(dir, '/');
        const char *pat;
        if (last_slash) {
            pat = last_slash + 1;
            *(last_slash + 1) = '\0';
        } else {
            pat = dir;
            vmsfs_to_linux_path(ctx->default_dir, dir, sizeof(dir));
        }

        DIR *dp = opendir(dir);
        if (!dp) {
            dcl_error("RMS", 2, "DNF", "directory not found");
            return SS$_NOSUCHFILE;
        }

        struct dirent *entry;
        int deleted = 0;
        while ((entry = readdir(dp)) != NULL) {
            if (fnmatch(pat, entry->d_name, FNM_CASEFOLD) == 0) {
                char full[2048];
                snprintf(full, sizeof(full), "%s%s", dir, entry->d_name);

                if (do_confirm) {
                    char vms_spec[256];
                    dcl_format_filespec(full, vms_spec, sizeof(vms_spec));
                    printf("DELETE %s ? [N]: ", vms_spec);
                    fflush(stdout);
                    char resp[64];
                    if (!fgets(resp, sizeof(resp), stdin)) break;
                    if (toupper((unsigned char)resp[0]) != 'Y') continue;
                }

                if (unlink(full) == 0) {
                    deleted++;
                    if (do_log) {
                        char vms_spec[256];
                        dcl_format_filespec(full, vms_spec, sizeof(vms_spec));
                        dcl_error("DELETE", 1, "DELETED", "%s deleted", vms_spec);
                    }
                }
            }
        }
        closedir(dp);

        if (deleted == 0) {
            dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
            return SS$_NOSUCHFILE;
        }
    } else {
        /* Single file delete */
        if (do_confirm) {
            char vms_spec[256];
            dcl_format_filespec(linux_path, vms_spec, sizeof(vms_spec));
            printf("DELETE %s ? [N]: ", vms_spec);
            fflush(stdout);
            char resp[64];
            if (!fgets(resp, sizeof(resp), stdin) ||
                toupper((unsigned char)resp[0]) != 'Y') {
                return SS$_NORMAL;
            }
        }

        if (unlink(linux_path) != 0) {
            dcl_error("RMS", 2, "FNF",
                      "file not found - %s", cmd->params[0]);
            return SS$_NOSUCHFILE;
        }

        if (do_log) {
            char vms_spec[256];
            dcl_format_filespec(linux_path, vms_spec, sizeof(vms_spec));
            dcl_error("DELETE", 1, "DELETED", "%s deleted", vms_spec);
        }
    }

    return SS$_NORMAL;
}

/*
 * RENAME - Rename a file.
 */
int cmd_rename(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE", "missing source and/or destination");
        return SS$_BADPARAM;
    }

    char src_path[1024], dst_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], src_path, sizeof(src_path));
    dcl_resolve_path(ctx, cmd->params[1], dst_path, sizeof(dst_path));

    if (rename(src_path, dst_path) != 0) {
        dcl_error("RMS", 2, "RNF",
                  "rename failed - %s", vms_strerror(errno));
        return SS$_FILACCERR;
    }

    /* /LOG qualifier: report the rename */
    if (dcl_has_qualifier(cmd, "LOG")) {
        char vms_src[256], vms_dst[256];
        dcl_format_filespec(src_path, vms_src, sizeof(vms_src));
        dcl_format_filespec(dst_path, vms_dst, sizeof(vms_dst));
        dcl_error("RENAME", 1, "RENAMED", "%s renamed to %s", vms_src, vms_dst);
    }

    return SS$_NORMAL;
}

/*
 * CREATE - Create a new file, or (with /DIRECTORY) create a directory.
 */
int cmd_create(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* /DIRECTORY qualifier: create a directory instead of a file */
    if (dcl_has_qualifier(cmd, "DIRECTORY")) {
        if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
            dcl_error("DCL", 2, "NODIR", "missing directory specification");
            return SS$_BADPARAM;
        }

        char linux_path[1024];
        dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

        /* Remove trailing slash before mkdir */
        size_t plen = strlen(linux_path);
        if (plen > 1 && linux_path[plen - 1] == '/') {
            linux_path[plen - 1] = '\0';
        }

        if (mkdir(linux_path, 0755) != 0) {
            if (errno == EEXIST) {
                dcl_error("DCL", 0, "CREATED",
                          "directory already exists - %s", cmd->params[0]);
                return SS$_NORMAL;
            }
            dcl_error("RMS", 2, "CRE",
                      "cannot create directory - %s: %s",
                      cmd->params[0], vms_strerror(errno));
            return SS$_FILACCERR;
        }

        return SS$_NORMAL;
    }

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    /* Validate against ODS-2 naming rules before resolving the path.
     * Strip any directory spec to get just the filename portion. */
    const char *fname_for_check = cmd->params[0];
    /* Skip past device: and [dir] if present */
    const char *bracket_end = strrchr(fname_for_check, ']');
    if (bracket_end) fname_for_check = bracket_end + 1;
    else {
        const char *col = strrchr(fname_for_check, ':');
        if (col) fname_for_check = col + 1;
    }
    /* Strip version ;N for ODS-2 check */
    char name_check[256];
    strncpy(name_check, fname_for_check, sizeof(name_check) - 1);
    name_check[sizeof(name_check) - 1] = '\0';
    char *semi_check = strrchr(name_check, ';');
    if (semi_check) *semi_check = '\0';

    if (name_check[0] != '\0' && !vmsfs_is_valid_ods2_name(name_check)) {
        dcl_error("RMS", 2, "SYN",
                  "invalid ODS-2 filename - %s", cmd->params[0]);
        return SS$_BADPARAM;
    }

    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    FILE *fp = fopen(linux_path, "w");
    if (!fp) {
        dcl_error("RMS", 2, "CRE", "cannot create - %s", cmd->params[0]);
        return SS$_FILACCERR;
    }

    /* Read lines from SYS$INPUT until Ctrl-Z (EOF) */
    if (ctx->interactive) {
        char line[4096];
        while (1) {
            if (!fgets(line, sizeof(line), stdin)) break;
            fputs(line, fp);
        }
    }

    fclose(fp);
    return SS$_NORMAL;
}

/*
 * SEARCH - Search file for a string (like grep).
 */
int cmd_search(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE",
                  "missing file specification and/or search string");
        return SS$_BADPARAM;
    }

    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    const char *search_str = cmd->params[1];
    int exact = dcl_has_qualifier(cmd, "EXACT");
    int show_numbers = dcl_has_qualifier(cmd, "NUMBERS");
    int show_stats = dcl_has_qualifier(cmd, "STATISTICS");

    FILE *fp = fopen(linux_path, "r");
    if (!fp) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    /* Display header */
    char vms_name[256];
    const char *basename = strrchr(linux_path, '/');
    if (basename) basename++; else basename = linux_path;
    size_t i;
    for (i = 0; i < sizeof(vms_name) - 1 && basename[i]; i++)
        vms_name[i] = (char)toupper((unsigned char)basename[i]);
    vms_name[i] = '\0';

    char line[4096];
    int found = 0;
    int line_num = 0;
    int match_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        int match;

        if (exact) {
            match = (strstr(line, search_str) != NULL);
        } else {
            /* Case-insensitive search */
            char lower_line[4096], lower_search[1024];
            for (i = 0; line[i] && i < sizeof(lower_line) - 1; i++)
                lower_line[i] = (char)tolower((unsigned char)line[i]);
            lower_line[i] = '\0';
            for (i = 0; search_str[i] && i < sizeof(lower_search) - 1; i++)
                lower_search[i] = (char)tolower((unsigned char)search_str[i]);
            lower_search[i] = '\0';
            match = (strstr(lower_line, lower_search) != NULL);
        }

        if (match) {
            if (!found) {
                printf("\n******************************\n%s\n", vms_name);
                found = 1;
            }
            /* Remove trailing newline for cleaner output */
            size_t llen = strlen(line);
            if (llen > 0 && line[llen - 1] == '\n') line[llen - 1] = '\0';
            if (show_numbers) {
                printf("%d: %s\n", line_num, line);
            } else {
                printf("%s\n", line);
            }
            match_count++;
        }
    }

    fclose(fp);

    if (show_stats) {
        printf("\n%d lines matched in 1 file\n", match_count);
    }

    if (!found) {
        dcl_error("SEARCH", 0, "NOMATCHES",
                  "no strings matched");
        return SS$_NORMAL; /* Warning, not error */
    }

    return SS$_NORMAL;
}

/* vmsfs version management API (from vmsfs/version.h) */
extern int vmsfs_purge_versions(const char *linux_dir, const char *basename,
                                const char *ext, int keep_count);
extern int vmsfs_list_versions(const char *linux_dir, const char *basename,
                               const char *ext, int *versions, int max_versions,
                               int *count);

/*
 * PURGE - Delete all but the highest N versions of files.
 *
 * Syntax: PURGE [filespec] [/KEEP=n]
 *   filespec  - VMS file specification (wildcards allowed); defaults to *.*
 *   /KEEP=n   - Number of versions to keep; default is 1
 *
 * Calls vmsfs_purge_versions() for each matching file base name found
 * in the target directory.
 */
int cmd_purge(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    int do_log = dcl_has_qualifier(cmd, "LOG");
    int do_confirm = dcl_has_qualifier(cmd, "CONFIRM");

    /* Determine keep count from /KEEP=n qualifier */
    int keep_count = 1;
    const char *keep_val = dcl_qualifier_value(cmd, "KEEP");
    if (keep_val && keep_val[0]) {
        char *endp;
        keep_count = (int)strtol(keep_val, &endp, 10);
        if (endp == keep_val || *endp != '\0' || keep_count < 1) keep_count = 1;
    }

    /* Determine the target directory and pattern */
    char linux_dir[1024];
    const char *pattern = NULL;

    if (cmd->param_count >= 1 && cmd->params[0][0] != '\0') {
        char resolved[1024];
        dcl_resolve_path(ctx, cmd->params[0], resolved, sizeof(resolved));

        struct stat st;
        if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
            strncpy(linux_dir, resolved, sizeof(linux_dir) - 1);
            linux_dir[sizeof(linux_dir) - 1] = '\0';
        } else {
            /* Split path into directory + filename pattern */
            char *last_slash = strrchr(resolved, '/');
            if (last_slash) {
                pattern = strdup(last_slash + 1);
                *(last_slash + 1) = '\0';
                strncpy(linux_dir, resolved, sizeof(linux_dir) - 1);
                linux_dir[sizeof(linux_dir) - 1] = '\0';
            } else {
                pattern = strdup(resolved);
                vmsfs_to_linux_path(ctx->default_dir, linux_dir, sizeof(linux_dir));
            }
        }
    } else {
        /* Default: current directory, all files (*.*) */
        vmsfs_to_linux_path(ctx->default_dir, linux_dir, sizeof(linux_dir));
    }

    /* Ensure trailing slash */
    size_t dlen = strlen(linux_dir);
    if (dlen > 0 && linux_dir[dlen - 1] != '/') {
        if (dlen < sizeof(linux_dir) - 1) {
            linux_dir[dlen] = '/';
            linux_dir[dlen + 1] = '\0';
        }
    }

    /* Scan the directory and collect unique base names (name without version).
     * For each unique base+ext, call vmsfs_purge_versions(). */
    DIR *dir = opendir(linux_dir);
    if (!dir) {
        dcl_error("RMS", 2, "DNF", "directory not found - %s", linux_dir);
        if (pattern) free((void *)pattern);
        return SS$_NOSUCHFILE;
    }

    /* Track processed bases to avoid duplicate purge calls */
    struct purge_base {
        char name[64];
        char ext[64];
    };
    int cap = 64;
    struct purge_base *bases = malloc((size_t)cap * sizeof(*bases));
    if (!bases) {
        closedir(dir);
        if (pattern) free((void *)pattern);
        return SS$_INSFMEM;
    }
    int base_count = 0;
    int total_deleted = 0;

    struct dirent *de;
    /* Remove trailing slash for opendir (already done) — iterate entries */
    /* Strip the trailing slash from linux_dir to use as dir arg to vmsfs */
    char dir_notrail[1024];
    strncpy(dir_notrail, linux_dir, sizeof(dir_notrail) - 1);
    dir_notrail[sizeof(dir_notrail) - 1] = '\0';
    size_t dtlen = strlen(dir_notrail);
    if (dtlen > 1 && dir_notrail[dtlen - 1] == '/') {
        dir_notrail[dtlen - 1] = '\0';
    }

    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        /* Only process versioned files (must contain ;N) */
        const char *semi = strrchr(de->d_name, ';');
        if (!semi) continue;

        /* Apply wildcard pattern filter if one was given */
        if (pattern) {
            if (!vmsfs_wildcard_match(pattern, de->d_name)) continue;
        }

        /* Extract base (name) and ext from the portion before ';' */
        char base_ext[256];
        size_t belen = (size_t)(semi - de->d_name);
        if (belen >= sizeof(base_ext)) belen = sizeof(base_ext) - 1;
        memcpy(base_ext, de->d_name, belen);
        base_ext[belen] = '\0';

        /* Split base_ext into name and extension */
        char fname[64] = {0};
        char fext[64]  = {0};
        const char *dot = strrchr(base_ext, '.');
        if (dot) {
            size_t nlen = (size_t)(dot - base_ext);
            if (nlen >= sizeof(fname)) nlen = sizeof(fname) - 1;
            memcpy(fname, base_ext, nlen);
            fname[nlen] = '\0';
            strncpy(fext, dot + 1, sizeof(fext) - 1);
            fext[sizeof(fext) - 1] = '\0';
        } else {
            strncpy(fname, base_ext, sizeof(fname) - 1);
            fname[sizeof(fname) - 1] = '\0';
        }

        /* Check if we already processed this base */
        int already = 0;
        for (int k = 0; k < base_count; k++) {
            if (strcasecmp(bases[k].name, fname) == 0 &&
                strcasecmp(bases[k].ext,  fext)  == 0) {
                already = 1;
                break;
            }
        }
        if (already) continue;

        /* Record this base */
        if (base_count >= cap) {
            cap *= 2;
            struct purge_base *tmp = realloc(bases, (size_t)cap * sizeof(*bases));
            if (!tmp) { free(bases); closedir(dir);
                        if (pattern) free((void *)pattern);
                        return SS$_INSFMEM; }
            bases = tmp;
        }
        strncpy(bases[base_count].name, fname, sizeof(bases[0].name) - 1);
        bases[base_count].name[sizeof(bases[0].name) - 1] = '\0';
        strncpy(bases[base_count].ext,  fext,  sizeof(bases[0].ext)  - 1);
        bases[base_count].ext[sizeof(bases[0].ext) - 1] = '\0';
        base_count++;

        /* /CONFIRM: prompt before purging this file */
        if (do_confirm) {
            char upper_name[128];
            size_t ui;
            for (ui = 0; ui < sizeof(upper_name) - 1 && fname[ui]; ui++)
                upper_name[ui] = (char)toupper((unsigned char)fname[ui]);
            upper_name[ui] = '\0';
            char upper_ext[64];
            for (ui = 0; ui < sizeof(upper_ext) - 1 && fext[ui]; ui++)
                upper_ext[ui] = (char)toupper((unsigned char)fext[ui]);
            upper_ext[ui] = '\0';
            printf("PURGE %s%s%s ? [N]: ", upper_name,
                   upper_ext[0] ? "." : "", upper_ext);
            fflush(stdout);
            char resp[64];
            if (!fgets(resp, sizeof(resp), stdin) ||
                toupper((unsigned char)resp[0]) != 'Y')
                continue;
        }

        /* Purge old versions for this file */
        int deleted = vmsfs_purge_versions(dir_notrail, fname,
                                            fext[0] ? fext : NULL, keep_count);
        if (deleted > 0) {
            total_deleted += deleted;
            if (do_log) {
                char upper_name[128];
                size_t ui;
                for (ui = 0; ui < sizeof(upper_name) - 1 && fname[ui]; ui++)
                    upper_name[ui] = (char)toupper((unsigned char)fname[ui]);
                upper_name[ui] = '\0';
                char upper_ext[64];
                for (ui = 0; ui < sizeof(upper_ext) - 1 && fext[ui]; ui++)
                    upper_ext[ui] = (char)toupper((unsigned char)fext[ui]);
                upper_ext[ui] = '\0';
                dcl_error("PURGE", 1, "PURGED",
                          "%s%s%s purged (%d version%s removed)",
                          upper_name, upper_ext[0] ? "." : "", upper_ext,
                          deleted, deleted != 1 ? "s" : "");
            }
        }
    }
    closedir(dir);
    free(bases);
    if (pattern) free((void *)pattern);

    if (total_deleted == 0) {
        printf("%%PURGE-I-NOPURGE, no file versions to purge\n");
    } else {
        printf("%%PURGE-I-PURGED, %d old version%s deleted\n",
               total_deleted, total_deleted != 1 ? "s" : "");
    }
    return SS$_NORMAL;
}

/*
 * APPEND - Append one file to another.
 * Format: APPEND source-filespec destination-filespec
 */
int cmd_append(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE",
                  "missing source and/or destination file specification");
        return SS$_BADPARAM;
    }

    char src_path[1024], dst_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], src_path, sizeof(src_path));
    dcl_resolve_path(ctx, cmd->params[1], dst_path, sizeof(dst_path));

    FILE *src = fopen(src_path, "rb");
    if (!src) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    FILE *dst = fopen(dst_path, "ab");
    if (!dst) {
        fclose(src);
        dcl_error("RMS", 2, "CRE", "cannot open for append - %s", cmd->params[1]);
        return SS$_FILACCERR;
    }

    char buf[8192];
    size_t n;
    int write_err = 0;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            write_err = 1;
            break;
        }
    }

    fclose(src);
    fclose(dst);

    if (write_err) {
        dcl_error("RMS", 2, "WER", "write error - %s", cmd->params[1]);
        return SS$_ABORT;
    }

    return SS$_NORMAL;
}
