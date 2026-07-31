/*
 * dcl_lexical.c - DCL Lexical Functions (F$xxx)
 *
 * Implements the VMS DCL lexical functions that return information
 * about strings, times, files, processes, and the environment.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <pwd.h>
#include <fnmatch.h>
#include <sys/statvfs.h>

#include "vmsqueue.h"

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/dcl_cmd.h"
#include "dcl/symbol.h"
#include "ssdef.h"
/* Kernel-interface client: F$DEVICE enumerates the executive's device
 * table through it (vms-fb9). See the note above populate_device_list. */
#include "vms_kif.h"
#include <vms/privs.h>
#include "vmsfs/filespec.h"
#include "sysgen_params.h"
#include "ovmx_identity.h"

/* External functions */
extern int dcl_translate_logical(const char *name, char *result, size_t result_size);
extern int dcl_resolve_path(struct dcl_context *ctx, const char *spec,
                            char *linux_path, size_t path_size);
extern int dcl_format_directory(const char *linux_path, char *vms_dir, size_t dir_size);

/*
 * Format current time in VMS format: DD-MMM-YYYY HH:MM:SS.CC
 */
static void format_vms_time(char *buf, size_t bufsize)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int centisec = (int)(ts.tv_nsec / 10000000);
    snprintf(buf, bufsize, "%2d-%s-%04d %02d:%02d:%02d.%02d",
             tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
             tm.tm_hour, tm.tm_min, tm.tm_sec, centisec);
}

/*
 * F$TIME() - Return current date/time in VMS format.
 */
static int lex_time(struct dcl_context *ctx, const char *args,
                    char *result, size_t result_size)
{
    (void)ctx;
    (void)args;
    format_vms_time(result, result_size);
    return 0;
}

/*
 * F$LENGTH(string) - Return length of string.
 */
static int lex_length(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    (void)ctx;
    if (!args) { snprintf(result, result_size, "0"); return 0; }

    /* Strip surrounding quotes if present */
    char str[4096];
    strncpy(str, args, sizeof(str) - 1);
    str[sizeof(str) - 1] = '\0';

    /* Trim whitespace */
    char *s = str;
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    s[len] = '\0';

    /* Remove quotes */
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        s[len - 1] = '\0';
        s++;
        len -= 2;
    }

    snprintf(result, result_size, "%zu", strlen(s));
    return 0;
}

/*
 * F$EXTRACT(start, length, string) - Extract substring.
 */
static int lex_extract(struct dcl_context *ctx, const char *args,
                       char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) return 0;

    /* Parse: start, length, string */
    int start = 0, len = 0;
    char str[4096] = {0};

    /* Find the commas */
    const char *p = args;
    while (*p == ' ') p++;
    start = (int)strtol(p, NULL, 10);

    /* Skip to next comma */
    p = strchr(p, ',');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;
    len = (int)strtol(p, NULL, 10);

    /* Skip to the string argument */
    p = strchr(p, ',');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;

    /* Copy string, removing quotes */
    strncpy(str, p, sizeof(str) - 1);
    size_t slen = strlen(str);
    while (slen > 0 && (str[slen - 1] == ' ' || str[slen - 1] == '\t'))
        str[--slen] = '\0';
    if (slen >= 2 && str[0] == '"' && str[slen - 1] == '"') {
        str[slen - 1] = '\0';
        memmove(str, str + 1, slen - 1);
        slen -= 2;
    }

    if (start < 0) start = 0;
    if (start >= (int)slen) { result[0] = '\0'; return 0; }
    if (len < 0) len = 0;
    if (start + len > (int)slen) len = (int)slen - start;

    if ((size_t)len >= result_size) len = (int)(result_size - 1);
    memcpy(result, str + start, (size_t)len);
    result[len] = '\0';

    return 0;
}

/*
 * F$ELEMENT(number, delimiter, string) - Extract element from delimited string.
 */
static int lex_element(struct dcl_context *ctx, const char *args,
                       char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) return 0;

    const char *p = args;
    while (*p == ' ') p++;
    int element = (int)strtol(p, NULL, 10);

    p = strchr(p, ',');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;

    /* Get delimiter */
    char delim = ',';
    if (*p == '"') {
        p++;
        delim = *p;
        p++;
        if (*p == '"') p++;
    } else {
        delim = *p;
        p++;
    }

    p = strchr(p, ',');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;

    /* Get string */
    char str[4096];
    strncpy(str, p, sizeof(str) - 1);
    str[sizeof(str) - 1] = '\0';
    size_t slen = strlen(str);
    while (slen > 0 && (str[slen - 1] == ' ' || str[slen - 1] == '\t'))
        str[--slen] = '\0';
    if (slen >= 2 && str[0] == '"' && str[slen - 1] == '"') {
        str[slen - 1] = '\0';
        memmove(str, str + 1, slen - 1);
        slen -= 2;
    }

    /* Find the nth element */
    int cur = 0;
    const char *start = str;
    const char *end = str;
    while (*end) {
        if (*end == delim) {
            if (cur == element) break;
            cur++;
            start = end + 1;
        }
        end++;
    }

    if (cur == element || (element == 0 && cur == 0)) {
        size_t elen = (size_t)(end - start);
        if (elen >= result_size) elen = result_size - 1;
        memcpy(result, start, elen);
        result[elen] = '\0';
    } else {
        /* Element not found - return delimiter string */
        result[0] = delim;
        result[1] = '\0';
    }

    return 0;
}

/*
 * F$LOCATE(substring, string) - Find position of substring.
 */
static int lex_locate(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    (void)ctx;
    if (!args) { snprintf(result, result_size, "0"); return 0; }

    /* Parse: substring, string */
    char substr[1024] = {0};
    char str[4096] = {0};

    const char *p = args;
    while (*p == ' ') p++;

    /* Get substring (possibly quoted) */
    if (*p == '"') {
        p++;
        size_t si = 0;
        while (*p && *p != '"' && si < sizeof(substr) - 1) {
            substr[si++] = *p++;
        }
        substr[si] = '\0';
        if (*p == '"') p++;
    } else {
        size_t si = 0;
        while (*p && *p != ',' && si < sizeof(substr) - 1) {
            substr[si++] = *p++;
        }
        substr[si] = '\0';
    }

    p = strchr(p, ',');
    if (!p) { snprintf(result, result_size, "0"); return 0; }
    p++;
    while (*p == ' ') p++;

    /* Get string */
    if (*p == '"') {
        p++;
        size_t si = 0;
        while (*p && *p != '"' && si < sizeof(str) - 1) {
            str[si++] = *p++;
        }
        str[si] = '\0';
    } else {
        strncpy(str, p, sizeof(str) - 1);
        size_t slen = strlen(str);
        while (slen > 0 && (str[slen - 1] == ' ' || str[slen - 1] == '\t'))
            str[--slen] = '\0';
    }

    const char *found = strstr(str, substr);
    if (found) {
        snprintf(result, result_size, "%td", found - str);
    } else {
        snprintf(result, result_size, "%zu", strlen(str));
    }

    return 0;
}

/*
 * F$EDIT(string, edit_list) - Edit a string.
 * Supported edits: UPCASE, LOWERCASE, TRIM, COMPRESS, COLLAPSE, UNCOMMENT
 */
static int lex_edit(struct dcl_context *ctx, const char *args,
                    char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) return 0;

    /* Parse: string, edit_list */
    char str[4096] = {0};
    char edits[256] = {0};

    const char *p = args;
    while (*p == ' ') p++;

    /* Get string */
    if (*p == '"') {
        p++;
        size_t si = 0;
        while (*p && si < sizeof(str) - 1) {
            if (*p == '"') {
                if (*(p + 1) == '"') {
                    str[si++] = '"';
                    p += 2;
                } else {
                    p++;
                    break;
                }
            } else {
                str[si++] = *p++;
            }
        }
        str[si] = '\0';
    } else {
        size_t si = 0;
        while (*p && *p != ',' && si < sizeof(str) - 1) {
            str[si++] = *p++;
        }
        str[si] = '\0';
    }

    p = strchr(p, ',');
    if (p) {
        p++;
        while (*p == ' ') p++;
        strncpy(edits, p, sizeof(edits) - 1);
        /* Remove surrounding quotes from edit list */
        size_t elen = strlen(edits);
        while (elen > 0 && (edits[elen - 1] == ' ' || edits[elen - 1] == '\t'))
            edits[--elen] = '\0';
        if (elen >= 2 && edits[0] == '"' && edits[elen - 1] == '"') {
            edits[elen - 1] = '\0';
            memmove(edits, edits + 1, elen - 1);
        }
    }

    /* Apply edits */
    char temp[4096];
    strncpy(temp, str, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    /* Uppercase the edit list for comparison */
    char upper_edits[256];
    size_t i;
    for (i = 0; i < sizeof(upper_edits) - 1 && edits[i]; i++) {
        upper_edits[i] = (char)toupper((unsigned char)edits[i]);
    }
    upper_edits[i] = '\0';

    if (strstr(upper_edits, "UPCASE")) {
        for (i = 0; temp[i]; i++) {
            temp[i] = (char)toupper((unsigned char)temp[i]);
        }
    }

    if (strstr(upper_edits, "LOWERCASE")) {
        for (i = 0; temp[i]; i++) {
            temp[i] = (char)tolower((unsigned char)temp[i]);
        }
    }

    if (strstr(upper_edits, "TRIM")) {
        /* Remove leading and trailing spaces/tabs */
        char *s = temp;
        while (*s == ' ' || *s == '\t') s++;
        if (s != temp) memmove(temp, s, strlen(s) + 1);
        size_t len = strlen(temp);
        while (len > 0 && (temp[len - 1] == ' ' || temp[len - 1] == '\t'))
            temp[--len] = '\0';
    }

    if (strstr(upper_edits, "COMPRESS")) {
        /* Replace multiple spaces/tabs with single space */
        char buf[4096];
        size_t bi = 0;
        int in_space = 0;
        for (i = 0; temp[i] && bi < sizeof(buf) - 1; i++) {
            if (temp[i] == ' ' || temp[i] == '\t') {
                if (!in_space) { buf[bi++] = ' '; in_space = 1; }
            } else {
                buf[bi++] = temp[i]; in_space = 0;
            }
        }
        buf[bi] = '\0';
        strncpy(temp, buf, sizeof(temp) - 1);
    }

    if (strstr(upper_edits, "COLLAPSE")) {
        /* Remove all spaces and tabs */
        char buf[4096];
        size_t bi = 0;
        for (i = 0; temp[i] && bi < sizeof(buf) - 1; i++) {
            if (temp[i] != ' ' && temp[i] != '\t')
                buf[bi++] = temp[i];
        }
        buf[bi] = '\0';
        strncpy(temp, buf, sizeof(temp) - 1);
    }

    if (strstr(upper_edits, "UNCOMMENT")) {
        /* Remove everything from ! to end of string */
        char *bang = strchr(temp, '!');
        if (bang) *bang = '\0';
        /* Trim trailing whitespace after removing comment */
        size_t len = strlen(temp);
        while (len > 0 && (temp[len - 1] == ' ' || temp[len - 1] == '\t'))
            temp[--len] = '\0';
    }

    strncpy(result, temp, result_size - 1);
    result[result_size - 1] = '\0';
    return 0;
}

/*
 * F$INTEGER(string) - Convert string to integer.
 */
static int lex_integer(struct dcl_context *ctx, const char *args,
                       char *result, size_t result_size)
{
    (void)ctx;
    if (!args) { snprintf(result, result_size, "0"); return 0; }

    char str[256];
    strncpy(str, args, sizeof(str) - 1);
    str[sizeof(str) - 1] = '\0';

    /* Trim and unquote */
    char *s = str;
    while (*s == ' ') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = '\0';
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') { s[len - 1] = '\0'; s++; }

    long val = strtol(s, NULL, 0);
    snprintf(result, result_size, "%ld", val);
    return 0;
}

/*
 * F$STRING(integer) - Convert integer to string.
 */
static int lex_string(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    (void)ctx;
    if (!args) { result[0] = '\0'; return 0; }

    char str[256];
    strncpy(str, args, sizeof(str) - 1);
    str[sizeof(str) - 1] = '\0';
    char *s = str;
    while (*s == ' ') s++;

    long val = strtol(s, NULL, 0);
    snprintf(result, result_size, "%ld", val);
    return 0;
}

/*
 * F$TRNLNM(logname [, table]) - Translate logical name.
 */
static int lex_trnlnm(struct dcl_context *ctx, const char *args,
                       char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) return 0;

    char logname[256] = {0};
    const char *p = args;
    while (*p == ' ') p++;

    /* Get logical name (possibly quoted) */
    if (*p == '"') {
        p++;
        size_t si = 0;
        while (*p && *p != '"' && si < sizeof(logname) - 1) {
            logname[si++] = *p++;
        }
        logname[si] = '\0';
    } else {
        size_t si = 0;
        while (*p && *p != ',' && *p != ' ' && si < sizeof(logname) - 1) {
            logname[si++] = *p++;
        }
        logname[si] = '\0';
    }

    dcl_translate_logical(logname, result, result_size);
    return 0;
}

/*
 * F$ENVIRONMENT(item) - Get environment information.
 */
static int lex_environment(struct dcl_context *ctx, const char *args,
                           char *result, size_t result_size)
{
    result[0] = '\0';
    if (!args) return 0;

    char item[64];
    strncpy(item, args, sizeof(item) - 1);
    item[sizeof(item) - 1] = '\0';

    /* Trim and unquote */
    char *s = item;
    while (*s == ' ') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = '\0';
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') { s[len - 1] = '\0'; s++; }

    /* Uppercase for comparison */
    for (size_t i = 0; s[i]; i++)
        s[i] = (char)toupper((unsigned char)s[i]);

    if (strcmp(s, "DEFAULT") == 0) {
        strncpy(result, ctx->default_dir, result_size - 1);
        result[result_size - 1] = '\0';
    } else if (strcmp(s, "PROCEDURE") == 0) {
        if (ctx->proc_depth >= 0 && ctx->proc_stack[ctx->proc_depth].filename[0]) {
            strncpy(result, ctx->proc_stack[ctx->proc_depth].filename, result_size - 1);
        } else {
            strncpy(result, "", result_size - 1);
        }
    } else if (strcmp(s, "PROMPT") == 0) {
        strncpy(result, ctx->prompt, result_size - 1);
    } else if (strcmp(s, "VERIFY_PROCEDURE") == 0 || strcmp(s, "VERIFY_IMAGE") == 0) {
        snprintf(result, result_size, "%s", ctx->verify ? "TRUE" : "FALSE");
    } else if (strcmp(s, "INTERACTIVE") == 0) {
        snprintf(result, result_size, "%s", ctx->interactive ? "TRUE" : "FALSE");
    } else if (strcmp(s, "DEPTH") == 0) {
        snprintf(result, result_size, "%d", ctx->proc_depth + 1);
    } else if (strcmp(s, "TERMINAL") == 0) {
        strncpy(result, ctx->terminal.device_name, result_size - 1);
        result[result_size - 1] = '\0';
    } else {
        strncpy(result, "", result_size - 1);
    }

    return 0;
}

/*
 * F$PROCESS() - Return process name.
 */
static int lex_process(struct dcl_context *ctx, const char *args,
                       char *result, size_t result_size)
{
    (void)args;
    /* No "_FTA0:" fallback (vms-fb9): F$PROCESS() reports the process name
     * as it is. Filling an empty one in with an invented VMS device name
     * made every DCL process claim the same identity, which nothing else
     * could see or contradict -- the shape the operator rejected in the
     * VMS_PRCNAM ruling (CLAUDE.md rule 10). The real name is
     * executive-owned state OVMX does not have yet. */
    strncpy(result, ctx->process_name, result_size - 1);
    result[result_size - 1] = '\0';
    return 0;
}

/*
 * F$MODE() - Return process mode.
 */
static int lex_mode(struct dcl_context *ctx, const char *args,
                    char *result, size_t result_size)
{
    (void)args;
    strncpy(result, ctx->interactive ? "INTERACTIVE" : "BATCH", result_size - 1);
    result[result_size - 1] = '\0';
    return 0;
}

/*
 * F$USER() - Return username.
 */
static int lex_user(struct dcl_context *ctx, const char *args,
                    char *result, size_t result_size)
{
    (void)args;
    if (ctx->username[0]) {
        strncpy(result, ctx->username, result_size - 1);
    } else {
        struct passwd *pw = getpwuid(getuid());
        if (pw) {
            size_t i;
            for (i = 0; i < result_size - 1 && pw->pw_name[i]; i++) {
                result[i] = (char)toupper((unsigned char)pw->pw_name[i]);
            }
            result[i] = '\0';
        } else {
            strncpy(result, "SYSTEM", result_size - 1);
        }
    }
    result[result_size - 1] = '\0';
    return 0;
}

/*
 * F$VERIFY([new]) - Get/set verify mode.
 */
static int lex_verify(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    int old_verify = ctx->verify;

    if (args && args[0]) {
        char val[64];
        strncpy(val, args, sizeof(val) - 1);
        val[sizeof(val) - 1] = '\0';
        char *s = val;
        while (*s == ' ') s++;
        if (*s == '0' || strcasecmp(s, "FALSE") == 0) {
            ctx->verify = 0;
        } else {
            ctx->verify = 1;
        }
    }

    snprintf(result, result_size, "%d", old_verify);
    return 0;
}

/*
 * F$SEARCH wildcard context — tracks iterative directory scans.
 * One slot per unique filespec (up to 8 concurrent searches).
 */
#define FSEARCH_MAX_CTX  8
#define FSEARCH_MAX_MATCHES 512

static struct fsearch_ctx {
    char    filespec[512];  /* The VMS filespec pattern that opened this ctx */
    char    dir_linux[512]; /* Linux directory being scanned */
    char    pattern[256];   /* Wildcard filename pattern (fnmatch) */
    char   *matches[FSEARCH_MAX_MATCHES];
    int     match_count;
    int     match_pos;
} fsearch_slots[FSEARCH_MAX_CTX];
static int fsearch_initialized = 0;

static void fsearch_init(void)
{
    if (!fsearch_initialized) {
        memset(fsearch_slots, 0, sizeof(fsearch_slots));
        fsearch_initialized = 1;
    }
}

static struct fsearch_ctx *fsearch_find(const char *filespec)
{
    for (int i = 0; i < FSEARCH_MAX_CTX; i++) {
        if (fsearch_slots[i].filespec[0] &&
            strcmp(fsearch_slots[i].filespec, filespec) == 0)
            return &fsearch_slots[i];
    }
    return NULL;
}

static struct fsearch_ctx *fsearch_alloc(const char *filespec)
{
    /* Evict first free or oldest (slot 0) */
    for (int i = 0; i < FSEARCH_MAX_CTX; i++) {
        if (!fsearch_slots[i].filespec[0]) {
            strncpy(fsearch_slots[i].filespec, filespec,
                    sizeof(fsearch_slots[i].filespec) - 1);
            return &fsearch_slots[i];
        }
    }
    /* No free slot — evict slot 0, shift */
    struct fsearch_ctx *evict = &fsearch_slots[0];
    for (int j = 0; j < evict->match_count; j++) {
        free(evict->matches[j]);
        evict->matches[j] = NULL;
    }
    memmove(&fsearch_slots[0], &fsearch_slots[1],
            sizeof(fsearch_slots[0]) * (FSEARCH_MAX_CTX - 1));
    memset(&fsearch_slots[FSEARCH_MAX_CTX - 1], 0,
           sizeof(fsearch_slots[0]));
    strncpy(fsearch_slots[FSEARCH_MAX_CTX - 1].filespec, filespec,
            sizeof(fsearch_slots[0].filespec) - 1);
    return &fsearch_slots[FSEARCH_MAX_CTX - 1];
}

static void fsearch_populate(struct fsearch_ctx *fsc)
{
    /* Free previous matches */
    for (int i = 0; i < fsc->match_count; i++) {
        free(fsc->matches[i]);
        fsc->matches[i] = NULL;
    }
    fsc->match_count = 0;
    fsc->match_pos   = 0;

    DIR *d = opendir(fsc->dir_linux);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && fsc->match_count < FSEARCH_MAX_MATCHES) {
        if (de->d_name[0] == '.') continue;  /* skip hidden */
        if (fnmatch(fsc->pattern, de->d_name, FNM_CASEFOLD) == 0) {
            char *dup = strdup(de->d_name);
            if (!dup) {
                /* Out of memory — free what we have and bail */
                for (int j = 0; j < fsc->match_count; j++) {
                    free(fsc->matches[j]);
                    fsc->matches[j] = NULL;
                }
                fsc->match_count = 0;
                closedir(d);
                return;
            }
            fsc->matches[fsc->match_count++] = dup;
        }
    }
    closedir(d);
}

/*
 * F$SEARCH(filespec) - Iterative wildcard file search.
 *
 * First call with a given filespec scans the directory and returns
 * the first match. Subsequent calls return subsequent matches.
 * Returns "" when exhausted.
 */
static int lex_search(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    result[0] = '\0';
    if (!args) return 0;

    fsearch_init();

    char spec[512];
    strncpy(spec, args, sizeof(spec) - 1);
    spec[sizeof(spec) - 1] = '\0';

    /* Trim and unquote */
    char *s = spec;
    while (*s == ' ') s++;
    size_t slen = strlen(s);
    while (slen > 0 && (s[slen - 1] == ' ' || s[slen - 1] == '\t')) s[--slen] = '\0';
    if (slen >= 2 && s[0] == '"' && s[slen - 1] == '"') {
        s[slen - 1] = '\0'; s++; slen -= 2;
    }

    /* Uppercase for VMS canon */
    char spec_upper[512];
    for (size_t i = 0; s[i] && i < sizeof(spec_upper) - 1; i++)
        spec_upper[i] = (char)toupper((unsigned char)s[i]);
    spec_upper[slen] = '\0';

    struct fsearch_ctx *fsc = fsearch_find(spec_upper);
    if (!fsc) {
        /* New filespec — open a fresh context */
        fsc = fsearch_alloc(spec_upper);
        if (!fsc) return 0;

        /* Resolve the filespec to a linux path */
        char linux_path[1024];
        dcl_resolve_path(ctx, s, linux_path, sizeof(linux_path));

        /* Split into directory + pattern */
        char *slash = strrchr(linux_path, '/');
        if (slash) {
            size_t dlen = (size_t)(slash - linux_path);
            memcpy(fsc->dir_linux, linux_path, dlen);
            fsc->dir_linux[dlen] = '\0';
            strncpy(fsc->pattern, slash + 1, sizeof(fsc->pattern) - 1);
        } else {
            vmsfs_to_linux_path(ctx->default_dir, fsc->dir_linux, sizeof(fsc->dir_linux));
            strncpy(fsc->pattern, linux_path, sizeof(fsc->pattern) - 1);
        }

        /* Replace VMS wildcards with shell wildcards */
        /* VMS uses * and % — % matches exactly one char (like shell ?) */
        char pat2[256];
        size_t pi = 0;
        for (size_t i = 0; fsc->pattern[i] && pi < sizeof(pat2) - 1; i++) {
            char c = fsc->pattern[i];
            if (c == '%')
                pat2[pi++] = '?';
            else
                pat2[pi++] = c;
        }
        pat2[pi] = '\0';
        strncpy(fsc->pattern, pat2, sizeof(fsc->pattern) - 1);

        fsearch_populate(fsc);
    }

    /* Return next match */
    if (fsc->match_pos >= fsc->match_count) {
        /* Exhausted — clear context so a fresh call restarts */
        for (int j = 0; j < fsc->match_count; j++) {
            free(fsc->matches[j]);
            fsc->matches[j] = NULL;
        }
        fsc->match_count = 0;
        fsc->match_pos   = 0;
        fsc->filespec[0] = '\0';
        return 0;
    }

    /* Build VMS-format result: DIR:[filename];1 */
    char vms_dir[512];
    dcl_format_directory(fsc->dir_linux, vms_dir, sizeof(vms_dir));

    /* Strip trailing ] to append filename, then close */
    size_t vlen = strlen(vms_dir);
    if (vlen > 0 && vms_dir[vlen - 1] == ']') vms_dir[vlen - 1] = '\0';

    const char *fname = fsc->matches[fsc->match_pos++];
    /* Uppercase the filename part */
    char fname_upper[256];
    for (size_t i = 0; fname[i] && i < sizeof(fname_upper) - 1; i++)
        fname_upper[i] = (char)toupper((unsigned char)fname[i]);
    fname_upper[strlen(fname)] = '\0';

    snprintf(result, result_size, "%s]%s;1", vms_dir, fname_upper);
    return 0;
}

/*
 * F$PARSE(filespec [, default [, related [, field]]]) - Parse a filespec.
 */
static int lex_parse(struct dcl_context *ctx, const char *args,
                     char *result, size_t result_size)
{
    result[0] = '\0';
    if (!args) return 0;

    /* Simple implementation: parse the first argument as a filespec */
    char spec[512] = {0};
    const char *p = args;
    while (*p == ' ') p++;
    if (*p == '"') {
        p++;
        size_t si = 0;
        while (*p && *p != '"' && si < sizeof(spec) - 1)
            spec[si++] = *p++;
        spec[si] = '\0';
    } else {
        size_t si = 0;
        while (*p && *p != ',' && si < sizeof(spec) - 1)
            spec[si++] = *p++;
        spec[si] = '\0';
    }

    /* For now, just resolve and format */
    char linux_path[1024];
    dcl_resolve_path(ctx, spec, linux_path, sizeof(linux_path));

    /* Check for field argument */
    /* Skip to 4th argument if present */
    int comma_count = 0;
    p = args;
    while (*p) {
        if (*p == ',') {
            comma_count++;
            if (comma_count == 3) { p++; break; }
        }
        p++;
    }

    if (comma_count >= 3) {
        while (*p == ' ') p++;
        char field[64];
        strncpy(field, p, sizeof(field) - 1);
        field[sizeof(field) - 1] = '\0';
        /* Trim and unquote */
        size_t flen = strlen(field);
        while (flen > 0 && (field[flen - 1] == ' ' || field[flen - 1] == '\t'))
            field[--flen] = '\0';
        if (flen >= 2 && field[0] == '"' && field[flen - 1] == '"') {
            field[flen - 1] = '\0';
            memmove(field, field + 1, flen - 1);
        }
        for (size_t i = 0; field[i]; i++)
            field[i] = (char)toupper((unsigned char)field[i]);

        if (strcmp(field, "NAME") == 0) {
            const char *bn = strrchr(linux_path, '/');
            if (bn) bn++; else bn = linux_path;
            char *dot = strrchr((char *)bn, '.');
            if (dot) {
                size_t nlen = (size_t)(dot - bn);
                if (nlen >= result_size) nlen = result_size - 1;
                memcpy(result, bn, nlen);
                result[nlen] = '\0';
            } else {
                strncpy(result, bn, result_size - 1);
            }
            /* Uppercase */
            for (size_t i = 0; result[i]; i++)
                result[i] = (char)toupper((unsigned char)result[i]);
        } else if (strcmp(field, "TYPE") == 0) {
            const char *bn = strrchr(linux_path, '/');
            if (bn) bn++; else bn = linux_path;
            const char *dot = strrchr(bn, '.');
            if (dot) {
                strncpy(result, dot, result_size - 1);
                for (size_t i = 0; result[i]; i++)
                    result[i] = (char)toupper((unsigned char)result[i]);
            }
        } else if (strcmp(field, "DIRECTORY") == 0) {
            const char *bn = strrchr(linux_path, '/');
            if (bn) {
                char dirpath[1024];
                size_t dlen = (size_t)(bn - linux_path);
                memcpy(dirpath, linux_path, dlen);
                dirpath[dlen] = '\0';
                dcl_format_directory(dirpath, result, result_size);
            }
        } else if (strcmp(field, "DEVICE") == 0) {
            strncpy(result, "SYS$DISK:", result_size - 1);
        } else if (strcmp(field, "NODE") == 0) {
            result[0] = '\0';
        } else {
            /* Return full filespec */
            char vms[1024];
            dcl_format_directory(linux_path, vms, sizeof(vms));
            strncpy(result, vms, result_size - 1);
        }
    } else {
        /* No field specified - return full filespec */
        char vms[1024];
        dcl_format_directory(linux_path, vms, sizeof(vms));
        strncpy(result, vms, result_size - 1);
    }

    result[result_size - 1] = '\0';
    return 0;
}

/*
 * F$FILE_ATTRIBUTES(filespec, item) - Get file attributes.
 */
static int lex_file_attributes(struct dcl_context *ctx, const char *args,
                               char *result, size_t result_size)
{
    result[0] = '\0';
    if (!args) return 0;

    /* Parse: filespec, item */
    char spec[512] = {0};
    char item[64] = {0};

    const char *p = args;
    while (*p == ' ') p++;
    if (*p == '"') {
        p++;
        size_t si = 0;
        while (*p && *p != '"' && si < sizeof(spec) - 1)
            spec[si++] = *p++;
        spec[si] = '\0';
        if (*p == '"') p++;
    } else {
        size_t si = 0;
        while (*p && *p != ',' && si < sizeof(spec) - 1)
            spec[si++] = *p++;
        spec[si] = '\0';
    }

    p = strchr(p, ',');
    if (p) {
        p++;
        while (*p == ' ') p++;
        strncpy(item, p, sizeof(item) - 1);
        size_t ilen = strlen(item);
        while (ilen > 0 && (item[ilen - 1] == ' ' || item[ilen - 1] == '\t'))
            item[--ilen] = '\0';
        if (ilen >= 2 && item[0] == '"' && item[ilen - 1] == '"') {
            item[ilen - 1] = '\0';
            memmove(item, item + 1, ilen - 1);
        }
    }

    /* Uppercase item */
    for (size_t i = 0; item[i]; i++)
        item[i] = (char)toupper((unsigned char)item[i]);

    /* Resolve filespec */
    char linux_path[1024];
    dcl_resolve_path(ctx, spec, linux_path, sizeof(linux_path));

    struct stat st;
    if (stat(linux_path, &st) != 0) {
        strncpy(result, "0", result_size - 1);
        return 0;
    }

    if (strcmp(item, "EOF") == 0 || strcmp(item, "ALQ") == 0 ||
        strcmp(item, "MRS") == 0) {
        /* Size in blocks (512 bytes) */
        long blocks = (st.st_size + 511) / 512;
        snprintf(result, result_size, "%ld", blocks);
    } else if (strcmp(item, "CDT") == 0 || strcmp(item, "RDT") == 0) {
        /* Creation/revision date in VMS format */
        struct tm tm;
        localtime_r(&st.st_mtime, &tm);
        snprintf(result, result_size, "%2d-%s-%04d %02d:%02d:%02d.00",
                 tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else if (strcmp(item, "KNOWN") == 0) {
        snprintf(result, result_size, "TRUE");
    } else if (strcmp(item, "ORG") == 0) {
        snprintf(result, result_size, "SEQ");
    } else if (strcmp(item, "RAT") == 0) {
        snprintf(result, result_size, "CR");
    } else if (strcmp(item, "RFM") == 0) {
        snprintf(result, result_size, "STMLF");
    } else if (strcmp(item, "PRO") == 0) {
        /* Protection string */
        snprintf(result, result_size, "(S:RWED,O:RWED,G:RE,W:)");
    } else {
        snprintf(result, result_size, "0");
    }

    result[result_size - 1] = '\0';
    return 0;
}

/*
 * F$TYPE(symbol) - Return type of a symbol ("STRING" or "INTEGER").
 */
static int lex_type(struct dcl_context *ctx, const char *args,
                    char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) return 0;

    char name[256];
    strncpy(name, args, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char *s = name;
    while (*s == ' ') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = '\0';
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') { s[len - 1] = '\0'; s++; }

    const char *val = dcl_sym_get(s);
    if (!val) {
        result[0] = '\0'; /* Undefined */
    } else {
        /* Try to determine type */
        char *endp;
        strtol(val, &endp, 0);
        if (*endp == '\0' || *endp == ' ') {
            strncpy(result, "INTEGER", result_size - 1);
        } else {
            strncpy(result, "STRING", result_size - 1);
        }
    }
    result[result_size - 1] = '\0';
    return 0;
}

/*
 * parse_vms_time() - Parse "DD-MON-YYYY HH:MM:SS.CC" into struct tm + centisec.
 * Returns 1 on success, 0 on failure (use current time).
 */
static int parse_vms_time(const char *ts, struct tm *tm_out, int *cs_out)
{
    /* Attempt strptime on "DD-MON-YYYY HH:MM:SS.CC" */
    static const char *fmts[] = {
        "%d-%b-%Y %H:%M:%S",
        "%d-%b-%Y",
        NULL
    };
    for (int i = 0; fmts[i]; i++) {
        memset(tm_out, 0, sizeof(*tm_out));
        char *end = strptime(ts, fmts[i], tm_out);
        if (end) {
            *cs_out = 0;
            if (*end == '.') {
                end++;
                *cs_out = (int)strtol(end, NULL, 10);
            }
            return 1;
        }
    }
    return 0;
}

/*
 * F$CVTIME([time_string [, output_format [, field]]]) - Convert/extract time.
 *
 * Supported formats: ABSOLUTE (DD-MON-YYYY HH:MM:SS.CC), COMPARISON (sortable).
 * Fields: DATE, TIME, DATETIME, WEEKDAY, MONTH, DAY, HOUR, MINUTE, SECOND.
 */
static int lex_cvtime(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';

    /* Parse up to 3 comma-separated args: input_time, output_format, field */
    char a_time[64]   = "";
    char a_format[32] = "ABSOLUTE";
    char a_field[32]  = "";

    if (args && *args) {
        char buf[256];
        strncpy(buf, args, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        /* arg 0 */
        char *p = buf;
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        /* trim + unquote */
        while (*p == ' ') p++;
        size_t l = strlen(p);
        while (l > 0 && (p[l-1] == ' ' || p[l-1] == '\t')) p[--l] = '\0';
        if (l >= 2 && p[0] == '"' && p[l-1] == '"') { p[l-1] = '\0'; p++; l -= 2; }
        strncpy(a_time, p, sizeof(a_time) - 1);

        if (comma) {
            p = comma + 1;
            comma = strchr(p, ',');
            if (comma) *comma = '\0';
            while (*p == ' ') p++;
            l = strlen(p);
            while (l > 0 && (p[l-1] == ' ' || p[l-1] == '\t')) p[--l] = '\0';
            if (l >= 2 && p[0] == '"' && p[l-1] == '"') { p[l-1] = '\0'; p++; l -= 2; }
            if (*p) {
                for (size_t i = 0; p[i] && i < sizeof(a_format)-1; i++)
                    a_format[i] = (char)toupper((unsigned char)p[i]);
                a_format[l] = '\0';
            }

            if (comma) {
                p = comma + 1;
                while (*p == ' ') p++;
                l = strlen(p);
                while (l > 0 && (p[l-1] == ' ' || p[l-1] == '\t')) p[--l] = '\0';
                if (l >= 2 && p[0] == '"' && p[l-1] == '"') { p[l-1] = '\0'; p++; l -= 2; }
                for (size_t i = 0; p[i] && i < sizeof(a_field)-1; i++)
                    a_field[i] = (char)toupper((unsigned char)p[i]);
                a_field[l] = '\0';
            }
        }
    }

    /* Get the time value */
    struct tm tm;
    int centisec = 0;

    if (a_time[0]) {
        if (!parse_vms_time(a_time, &tm, &centisec)) {
            /* Fall back to current time */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            localtime_r(&ts.tv_sec, &tm);
            centisec = (int)(ts.tv_nsec / 10000000);
        }
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        localtime_r(&ts.tv_sec, &tm);
        centisec = (int)(ts.tv_nsec / 10000000);
    }

    /* Build the full formatted strings for both formats */
    static const char *weekday_names[] = {
        "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
    };

    if (strcmp(a_format, "COMPARISON") == 0) {
        /* Sortable: YYYY-MM-DD HH:MM:SS.CC */
        if (!a_field[0] || strcmp(a_field, "DATETIME") == 0) {
            snprintf(result, result_size, "%04d-%02d-%02d %02d:%02d:%02d.%02d",
                     1900 + tm.tm_year, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, centisec);
        } else if (strcmp(a_field, "DATE") == 0) {
            snprintf(result, result_size, "%04d-%02d-%02d",
                     1900 + tm.tm_year, tm.tm_mon + 1, tm.tm_mday);
        } else if (strcmp(a_field, "TIME") == 0) {
            snprintf(result, result_size, "%02d:%02d:%02d.%02d",
                     tm.tm_hour, tm.tm_min, tm.tm_sec, centisec);
        } else if (strcmp(a_field, "WEEKDAY") == 0) {
            snprintf(result, result_size, "%s", weekday_names[tm.tm_wday]);
        } else if (strcmp(a_field, "MONTH") == 0) {
            snprintf(result, result_size, "%d", tm.tm_mon + 1);
        } else if (strcmp(a_field, "DAY") == 0) {
            snprintf(result, result_size, "%d", tm.tm_mday);
        } else if (strcmp(a_field, "HOUR") == 0) {
            snprintf(result, result_size, "%d", tm.tm_hour);
        } else if (strcmp(a_field, "MINUTE") == 0) {
            snprintf(result, result_size, "%d", tm.tm_min);
        } else if (strcmp(a_field, "SECOND") == 0) {
            snprintf(result, result_size, "%d", tm.tm_sec);
        } else {
            snprintf(result, result_size, "%04d-%02d-%02d %02d:%02d:%02d.%02d",
                     1900 + tm.tm_year, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, centisec);
        }
    } else {
        /* ABSOLUTE format: DD-MON-YYYY HH:MM:SS.CC */
        if (!a_field[0] || strcmp(a_field, "DATETIME") == 0) {
            snprintf(result, result_size, "%2d-%s-%04d %02d:%02d:%02d.%02d",
                     tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, centisec);
        } else if (strcmp(a_field, "DATE") == 0) {
            snprintf(result, result_size, "%2d-%s-%04d",
                     tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year);
        } else if (strcmp(a_field, "TIME") == 0) {
            snprintf(result, result_size, "%02d:%02d:%02d.%02d",
                     tm.tm_hour, tm.tm_min, tm.tm_sec, centisec);
        } else if (strcmp(a_field, "WEEKDAY") == 0) {
            snprintf(result, result_size, "%s", weekday_names[tm.tm_wday]);
        } else if (strcmp(a_field, "MONTH") == 0) {
            snprintf(result, result_size, "%d", tm.tm_mon + 1);
        } else if (strcmp(a_field, "DAY") == 0) {
            snprintf(result, result_size, "%d", tm.tm_mday);
        } else if (strcmp(a_field, "HOUR") == 0) {
            snprintf(result, result_size, "%d", tm.tm_hour);
        } else if (strcmp(a_field, "MINUTE") == 0) {
            snprintf(result, result_size, "%d", tm.tm_min);
        } else if (strcmp(a_field, "SECOND") == 0) {
            snprintf(result, result_size, "%d", tm.tm_sec);
        } else {
            snprintf(result, result_size, "%2d-%s-%04d %02d:%02d:%02d.%02d",
                     tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, centisec);
        }
    }

    return 0;
}

/*
 * F$GETSYI(item) - Get system information.
 */
static int lex_getsyi(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) return 0;

    char item[64];
    strncpy(item, args, sizeof(item) - 1);
    item[sizeof(item) - 1] = '\0';
    char *s = item;
    while (*s == ' ') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = '\0';
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') { s[len - 1] = '\0'; s++; }
    for (size_t i = 0; s[i]; i++) s[i] = (char)toupper((unsigned char)s[i]);

    struct utsname uts;
    uname(&uts);

    if (strcmp(s, "NODENAME") == 0) {
        /* Linux hostname — distinct from the configured SCSNODE (vms-ci.8) */
        strncpy(result, uts.nodename, result_size - 1);
        for (size_t i = 0; result[i]; i++)
            result[i] = (char)toupper((unsigned char)result[i]);
    } else if (strcmp(s, "SCSNODE") == 0) {
        /* Configured cluster node identity (SYSGEN SCSNODE) — falls back
         * to the OVMX default when SYSGEN is unconfigured. */
        char node[SYSGEN_STRVAL_LEN];
        if (sysgen_read_string("SCSNODE", node, sizeof(node)) != 0) {
            strncpy(node, "OVMX", sizeof(node) - 1);
            node[sizeof(node) - 1] = '\0';
        }
        strncpy(result, node, result_size - 1);
        for (size_t i = 0; result[i]; i++)
            result[i] = (char)toupper((unsigned char)result[i]);
    } else if (strcmp(s, "SCSSYSTEMID") == 0) {
        uint32_t sysid = 0;   /* OVMX default when unconfigured */
        (void)sysgen_read_param("SCSSYSTEMID", &sysid);
        snprintf(result, result_size, "%u", sysid);
    } else if (strcmp(s, "VERSION") == 0) {
        /* Machine surface: the true-to-arch VMS-compat token, from the
         * identity SSOT (INV-1). Never a hardcoded constant here. */
        strncpy(result, ovmx_compat_version(), result_size - 1);
    } else if (strcmp(s, "HW_NAME") == 0) {
        strncpy(result, uts.machine, result_size - 1);
        for (size_t i = 0; result[i]; i++)
            result[i] = (char)toupper((unsigned char)result[i]);
    } else if (strcmp(s, "BOOTTIME") == 0) {
        format_vms_time(result, result_size);
    } else {
        strncpy(result, "0", result_size - 1);
    }

    result[result_size - 1] = '\0';
    return 0;
}

/*
 * F$GETJPI(pid, item) - Get process information.
 */
static int lex_getjpi(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    result[0] = '\0';
    if (!args) return 0;

    /* Parse: pid, item */
    const char *p = args;
    while (*p == ' ') p++;

    /* Skip pid argument */
    p = strchr(p, ',');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;

    char item[64];
    strncpy(item, p, sizeof(item) - 1);
    item[sizeof(item) - 1] = '\0';
    char *s = item;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = '\0';
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') { s[len - 1] = '\0'; s++; len -= 2; }
    for (size_t i = 0; s[i]; i++) s[i] = (char)toupper((unsigned char)s[i]);

    if (strcmp(s, "USERNAME") == 0) {
        return lex_user(ctx, NULL, result, result_size);
    } else if (strcmp(s, "PRCNAM") == 0) {
        return lex_process(ctx, NULL, result, result_size);
    } else if (strcmp(s, "PID") == 0) {
        snprintf(result, result_size, "%08X", (unsigned)getpid());
    } else if (strcmp(s, "MODE") == 0) {
        return lex_mode(ctx, NULL, result, result_size);
    } else if (strcmp(s, "CURPRIV") == 0 || strcmp(s, "AUTHPRIV") == 0) {
        /*
         * ADDED vms-2b8 round 4 (CURPRIV only, and as a DECIMAL INTEGER);
         * CORRECTED round 5 to a privilege-NAME string, and AUTHPRIV
         * added alongside it. Before round 4, CURPRIV fell into the
         * `else` branch below and silently returned "0" -- the
         * illegal-third-answer shape: neither matching VMS nor hiding
         * the item, just a plausible-looking zero.
         *
         * FORMAT PINNED TO PUBLIC DOCUMENTATION (round 5; round 4's
         * commit carries no citation for the decimal-integer format it
         * chose, and it was wrong): the HP/VSI OpenVMS DCL Dictionary entry for
         * F$GETJPI documents CURPRIV and AUTHPRIV as returning a String,
         * and the VSI OpenVMS Wiki's F$GETJPI page shows a live example
         * of that string --
         *   "CMKRNL,CMEXEC,SYSNAM,GRPNAM,ALLSPOOL,DETACH,DIAGNOSE,...,
         *    SECURITY"
         * -- a comma-separated list of privilege names in ASCENDING BIT
         * POSITION (CMKRNL bit 0, CMEXEC bit 1, ... SETPRV bit 14,
         * TMPMBX bit 15, WORLD bit 16, ...), NOT alphabetical -- the
         * order dcl_cmd_show.c's SHOW PROCESS/PRIVILEGES table uses is a
         * different VMS display convention for a different command, and
         * copying it here would silently reproduce the wrong one. Two
         * independent public sources (digiater.nl's mirror of the DCL
         * Dictionary for the data type; the VSI Wiki for the format),
         * neither derived from the other or from this tree.
         *
         * Reads the same live executive source as SHOW PROCESS/
         * PRIVILEGES and F$PRIVILEGE (vms_kif_getjpi_self(), masked to
         * VMS_PRV_M_ENFORCED). The NAMES themselves are not a second,
         * hand-maintained list: the loop below walks bit positions 0..63
         * in ascending order and looks each SET, enforced bit up in
         * vms_priv_names[] (dcl_cmd_show.c, declared in dcl/dcl_cmd.h) --
         * the SAME canonical name table SHOW PROCESS/PRIVILEGES reads.
         * A bit added to VMS_PRV_M_ENFORCED (src/kernel/vms_ioctl.h)
         * therefore gets a name here with no second edit, and the walk
         * order is ascending bit position for free, matching the
         * oracle's own CURPRIV example order (CMKRNL before CMEXEC) --
         * NOT the alphabetical order dcl_cmd_show.c uses for SHOW
         * PROCESS/PRIVILEGES, a different VMS display convention for a
         * different command (vms-2b8 round 6: a hand-maintained second
         * list here, kept in sync with VMS_PRV_M_ENFORCED "by hand", is
         * exactly the drift this program exists to kill).
         *
         * CURPRIV reads cur_privs, AUTHPRIV reads perm_privs (the
         * "authorized" mask SHOW PROCESS/PRIVILEGES's own Authorized:
         * block reads) -- two different struct fields in the source.
         * WHAT IS NOT CLAIMED: that this distinction is proven by any
         * test on this runtime. VMS_IOCTL_SETIDENT sets cur_privs =
         * perm_privs = args.authorized_privs (vms_proctab.c), and
         * nothing DCL currently calls moves them apart again -- $SETPRV
         * is the operation that would, and vms_kif_setprv() has no
         * product caller (OVMX-UNWIRED in vms_kif.h, pending vms-pv1).
         * So on THIS runtime cur_privs and perm_privs are always equal,
         * and no UAT assertion that only checks CURPRIV and AUTHPRIV
         * render the same string can tell "AUTHPRIV correctly reads
         * perm_privs" apart from "AUTHPRIV reads cur_privs by mistake" --
         * both produce an identical pass. A test that actually
         * discriminates the two fields needs a session where they
         * diverge, which needs vms-pv1's $SETPRV wiring; until then this
         * is a true statement about the source, not a proven one.
         *
         * NOT CLAIMED either: that this string is never empty. A process
         * registered without CAP_SYS_ADMIN gets perm_privs = cur_privs =
         * 0 at vms_proc_register() (src/kernel/vms_module.c) -- nothing
         * in the enforced set, so both items would legitimately render
         * as "". No session on the current UAT harness reaches that
         * state (there is no credential-drop path into an interactive
         * DCL session yet -- vms-475), so it is not exercised here; it
         * is a consequence of the code this comment does not need to
         * re-derive by mutation to state honestly.
         *
         * Like the other items in this function (USERNAME, PRCNAM, PID,
         * MODE), the pid argument is parsed but not honored -- this
         * answers only for the calling process, consistent with every
         * existing item here, not a new restriction.
         *
         * VERIFIED BY MUTATION (vms-2b8 round 6), not by inspection:
         * adding VMS_PRV_M_TMPMBX to VMS_PRV_M_ENFORCED
         * (src/kernel/vms_ioctl.h) -- the ONLY edit made for this test,
         * nothing in this file touched -- rebuilt vms.ko + vmsdcl and
         * re-booted a real QEMU image; F$GETJPI CURPRIV/AUTHPRIV both
         * came back "CMKRNL,CMEXEC,SETPRV,TMPMBX,WORLD", TMPMBX inserted
         * in the correct ascending-bit-position slot (between SETPRV
         * bit 14 and WORLD bit 16) with no second table to update.
         * Reverted after confirming.
         */
        struct vms_procinfo info;
        memset(&info, 0, sizeof(info));
        uint32_t jst = vms_kif_getjpi_self(&info);
        result[0] = '\0';
        if (jst & 1) {
            uint64_t raw = (strcmp(s, "CURPRIV") == 0) ? info.cur_privs
                                                         : info.perm_privs;
            uint64_t enforced = raw & VMS_PRV_M_ENFORCED;
            size_t rl = 0;
            for (int bit = 0; bit < 64; bit++) {
                uint64_t b = (uint64_t)1 << bit;
                if (!(enforced & b))
                    continue;
                int found = 0;
                for (int i = 0; vms_priv_names[i].name; i++) {
                    if (vms_priv_names[i].bit != b)
                        continue;
                    rl += (size_t)snprintf(result + rl, result_size - rl,
                                           "%s%s", rl ? "," : "",
                                           vms_priv_names[i].name);
                    found = 1;
                    break;
                }
                /*
                 * COVERAGE CHECK (vms-2b8 round 7, message corrected
                 * round 8). Names here are DERIVED from
                 * VMS_PRV_M_ENFORCED by walking the mask and looking
                 * each set bit up in vms_priv_names[] -- that
                 * derivation was the round-6 fix, replacing a
                 * hand-maintained second list that could drift out of
                 * sync with the mask silently. But the lookup itself
                 * had the same silent-drift shape one level down: if a
                 * bit is ever added to VMS_PRV_M_ENFORCED (src/kernel/
                 * vms_ioctl.h) with no matching row added to
                 * vms_priv_names[] (dcl_cmd_show.c), the `found` guard
                 * above stays false and the bit is just OMITTED from
                 * the rendered string -- CURPRIV/AUTHPRIV would report
                 * an incomplete privilege list with no diagnostic,
                 * which is exactly the "reads as correct, isn't" shape
                 * Rule 10 exists to kill, one layer down from the
                 * defect the derivation itself fixed.
                 *
                 * RULE 10 CHOICE, MADE EXPLICIT (round 8): this is not
                 * a condition VMS itself can ever face -- on VMS there
                 * is exactly one privilege table, so "an enforced bit
                 * has no name" cannot arise. It is not a "privilege
                 * VMS grants but OVMX doesn't enforce" either (that is
                 * the SET TIME/SET PROCESS PRIORITY case above, a
                 * different defect with its own HIDE wording). It is a
                 * two-C-files-disagreeing bug local to this build, so
                 * there is no real "refused privileged operation"
                 * status honestly describes it and nothing to MATCH.
                 * That leaves HIDE, and the round-7 mistake was
                 * choosing the "invent a plausible-looking VMS status"
                 * shape of HIDE (SS$_BUGCHECK, rendered with the
                 * SYSTEM facility and the same %FACILITY-S-IDENT shape
                 * src/libvms/status.c uses for genuine VMS condition
                 * values) for a condition that is not remotely a
                 * bugcheck -- a bugcheck is the executive detecting it
                 * cannot preserve system integrity; this is a build
                 * defect DCL detected in its own static table. Round 8
                 * takes the other HIDE option: report it as what it
                 * is, an OVMX-facility diagnostic, not a VMS one --
                 * same convention as %OVMX-I-NOSETPRV (dcl_cmd_set.c):
                 * the facility name reads OVMX, not SYSTEM, so it is not
                 * formatted as genuine VMS console output.
                 *
                 * PROVEN BY MUTATION, not by inspection (vms-2b8 round
                 * 7, reproduced round 8): temporarily OR-ing an
                 * unnamed bit (1ULL << 40) into VMS_PRV_M_ENFORCED
                 * (src/kernel/vms_ioctl.h, no row for it in
                 * vms_priv_names[]) and rebuilding vms.ko + vmsdcl,
                 * then running F$GETJPI CURPRIV as SYSTEM
                 * (SYSUAF-authorized ALL, so cur_privs has bit 40 set)
                 * fires this check and aborts the session instead of
                 * silently omitting the name. Reverted after
                 * confirming each time. The same mutation is now also
                 * a manifest entry in tests/qemu/facility_defects.sh
                 * so the facility sweep exercises it without a human
                 * remembering to run it by hand.
                 */
                if (!found) {
                    dcl_error("OVMX", 4, "TABLEDESYNC",
                              "internal build defect, not a VMS "
                              "condition -- VMS_PRV_M_ENFORCED "
                              "(src/kernel/vms_ioctl.h) bit %d has no "
                              "row in vms_priv_names[] "
                              "(src/vmsdcl/dcl_cmd_show.c); this is not "
                              "a bugcheck and not a VMS status, it is "
                              "OVMX's two privilege tables disagreeing "
                              "(vms-2b8)", bit);
                    abort();
                }
            }
        }
    } else {
        strncpy(result, "0", result_size - 1);
    }

    result[result_size - 1] = '\0';
    return 0;
}

/*
 * F$MESSAGE(code) - Get message text for a status code.
 *
 * Returns "%FACILITY-S-IDENT, text" matching VMS format.
 * Severity: 0=W 1=S 2=E 3=I 4=F
 */
static int lex_message(struct dcl_context *ctx, const char *args,
                       char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) return 0;

    char str[64];
    strncpy(str, args, sizeof(str) - 1);
    str[sizeof(str) - 1] = '\0';
    char *s = str;
    while (*s == ' ') s++;
    /* Trim trailing */
    size_t sl = strlen(s);
    while (sl > 0 && (s[sl-1] == ' ' || s[sl-1] == '\t')) s[--sl] = '\0';
    if (sl >= 2 && s[0] == '"' && s[sl-1] == '"') { s[sl-1] = '\0'; s++; }

    unsigned long code = strtoul(s, NULL, 0);

    /*
     * This table is keyed by NUMBER, so it does not follow a corrected
     * constant the way a consumer that names the symbol does. Bind the
     * corrected rows to the values the product actually returns, so a
     * future change breaks the build instead of leaving F$MESSAGE unable
     * to name a status OVMX hands out (vms-9fc: SS$_ILLIOFUNC moved
     * 580 -> 244 and this table was left behind, still rendering
     * "illegal I/O function" for what the oracle calls VASFULL).
     */
    _Static_assert(SS$_ILLIOFUNC == 244,
                   "F$MESSAGE's ILLIOFUNC row must carry the SS$_ILLIOFUNC "
                   "value sys$qio returns");
    _Static_assert(SS$_INSFMEM == 292,
                   "F$MESSAGE's INSFMEM row must carry SS$_INSFMEM");

    /* Inline lookup table for common SS$ condition codes */
    static const struct {
        unsigned long code;
        const char *facility;
        char sev;          /* S W E I F */
        const char *ident;
        const char *text;
    } msg_table[] = {
        { 1,     "SYSTEM", 'S', "NORMAL",       "normal successful completion" },
        { 4,     "SYSTEM", 'W', "BUFFEROVF",    "buffer overflow" },
        { 8,     "SYSTEM", 'E', "ERROR",         "error" },
        { 9,     "SYSTEM", 'S', "WASSET",        "previous state was set" },
        { 12,    "SYSTEM", 'E', "ACCVIO",        "access violation" },
        { 20,    "SYSTEM", 'E', "BADPARAM",      "bad parameter value" },
        { 28,    "SYSTEM", 'E', "EXQUOTA",       "exceeded quota" },
        /* ORACLE-PINNED (vms-6a7): docs/oracle/vax73-privileges.md §1.
         * F$MESSAGE(36) on VAX1 (OpenVMS VAX V7.3) renders
         * "%SYSTEM-F-NOPRIV, insufficient privilege or object protection
         * violation". BOTH fields here were wrong: the severity was 'E'
         * where 36 & 7 == 4 == F, and the text was an OVMX sentence VMS
         * has never printed. F$MESSAGE is the DCL surface that is
         * supposed to round-trip the oracle exactly. */
        { 36,    "SYSTEM", 'F', "NOPRIV",
          "insufficient privilege or object protection violation" },
        { 44,    "SYSTEM", 'E', "ABORT",         "abort" },
        /* ORACLE-PINNED (vms-8019): value and severity taken from the
         * reference lab OpenVMS VAX V7.3 node VAX1 -- $SSDEF in
         * SYS$LIBRARY:STARLET.MLB gives SS$_DUPLNAM 148, and
         * F$MESSAGE(148) renders "%SYSTEM-F-DUPLNAM, duplicate name".
         * Replaces 434/'E', which the same oracle disproves. */
        { 148,   "SYSTEM", 'F', "DUPLNAM",       "duplicate name" },
        /* ORACLE-PINNED (vms-9fc): $SSDEF SS$_ILLIOFUNC 244;
         * F$MESSAGE(244) -> "%SYSTEM-F-ILLIOFUNC, illegal I/O function
         * code". This row did not exist: the table carried ILLIOFUNC at
         * 580, which the same oracle run shows is SS$_VASFULL, so
         * F$MESSAGE could not name the status sys$qio actually returns
         * for an unimplemented function code. */
        { 244,   "SYSTEM", 'F', "ILLIOFUNC",     "illegal I/O function code" },
        /* ORACLE-PINNED (vms-68c), docs/oracle/vax73-event-flags.md:
         *   $EQU SS$_ILLEFC 236; F$MESSAGE(236) ->
         *     "%SYSTEM-F-ILLEFC, illegal event flag cluster"
         *   $EQU SS$_UNASEFC 564; F$MESSAGE(564) ->
         *     "%SYSTEM-F-UNASEFC, unassociated event flag cluster"
         * Neither row existed. Both statuses became reachable through the
         * public API when sys$setef/$clref/$readef/$ascefc were wired to
         * the executive (vms-2a8), and a status DCL's F$MESSAGE cannot name
         * is a half-applied correction -- the ILLIOFUNC lesson above.
         * There is deliberately NO row for SS$_WASCLR: it is 1 on VMS, the
         * same value as SS$_NORMAL, which is already the first row. */
        { 236,   "SYSTEM", 'F', "ILLEFC",        "illegal event flag cluster" },
        { 564,   "SYSTEM", 'F', "UNASEFC",       "unassociated event flag cluster" },
        /* ORACLE-PINNED (vms-2a8), docs/oracle/vax73-event-flags.md §1
         * method 2: F$MESSAGE(292) -> "%SYSTEM-F-INSFMEM, insufficient
         * dynamic memory". The severity here was 'E'; it is 'F'. The
         * value itself was never in doubt -- the severity field of the
         * status says so independently (292 & 7 == 4 == STS$K_SEVERE) --
         * so OVMX was rendering a status whose own bits contradict the
         * letter it printed. Same run that pinned ILLEFC/UNASEFC below. */
        { 292,   "SYSTEM", 'F', "INSFMEM",       "insufficient dynamic memory" },
        /* ORACLE-PINNED (vms-8019): $SSDEF SS$_IVLOGNAM 340;
         * F$MESSAGE(340) -> "%SYSTEM-F-IVLOGNAM, invalid logical name".
         * Replaces 596/'E' -- 596 is SS$_VOLINV on the oracle. */
        { 340,   "SYSTEM", 'F', "IVLOGNAM",      "invalid logical name" },
        { 388,   "SYSTEM", 'E', "IVTIME",        "invalid time" },
        { 444,   "SYSTEM", 'W', "NOLOGNAM",      "no logical name match" },
        /* ORACLE-PINNED (vms-2b8), docs/oracle/vax73-privileges.md §1.
         * MEASURED on OpenVMS VAX V7.3 node VAX1, 2026-07-30:
         *   F$MESSAGE(532)  -> %SYSTEM-F-RESULTOVF, resultant string overflow
         *   F$MESSAGE(1664) -> %SYSTEM-W-NOTALLPRIV, not all requested
         *                      privileges authorized
         * 532 was mapped to NOTALLPRIV here (and in ssdef.h), so OVMX's
         * F$MESSAGE answered a different condition than VMS's for both
         * codes. Note the text too: "authorized", not "available". */
        { 532,   "SYSTEM", 'F', "RESULTOVF",     "resultant string overflow" },
        { 548,   "SYSTEM", 'E', "IVIDENT",       "invalid identifier" },
        { 556,   "SYSTEM", 'E', "TIMEOUT",       "device timeout" },
        /* ORACLE-PINNED (vms-9fc): $SSDEF SS$_VASFULL 580;
         * F$MESSAGE(580) -> "%SYSTEM-F-VASFULL, virtual address space is
         * full". This slot used to be mislabelled ILLIOFUNC/'E', which
         * meant F$MESSAGE(580) rendered "illegal I/O function" for a
         * status that means address-space exhaustion. */
        { 580,   "SYSTEM", 'F', "VASFULL",       "virtual address space is full" },
        { 588,   "SYSTEM", 'E', "NOMORENODE",    "no more cluster nodes" },
        /* ORACLE-PINNED (vms-8019): $SSDEF SS$_VOLINV 596;
         * F$MESSAGE(596) -> "%SYSTEM-F-VOLINV, volume is not software
         * enabled". This slot used to be mislabelled IVLOGNAM. */
        { 596,   "SYSTEM", 'F', "VOLINV",        "volume is not software enabled" },
        { 602,   "SYSTEM", 'E', "IVCHAN",        "invalid channel" },
        { 608,   "SYSTEM", 'E', "IVDEVNAM",      "invalid device name" },
        { 620,   "SYSTEM", 'E', "IVSSRQ",        "invalid system service request" },
        { 636,   "SYSTEM", 'E', "SSFAIL",        "system service failure" },
        { 676,   "SYSTEM", 'F', "BUGCHECK",      "internal consistency failure" },
        { 708,   "SYSTEM", 'E', "DEADLOCK",      "deadlock detected" },
        { 712,   "SYSTEM", 'E', "VALNOTVALID",   "value block not valid" },
        { 716,   "SYSTEM", 'E', "PARNOTGRANT",   "parent lock not granted" },
        { 836,   "SYSTEM", 'S', "CREATED",       "object created" },
        { 844,   "SYSTEM", 'S', "SUPERSEDE",     "object superseded" },
        /* ORACLE-PINNED (vms-2b8), docs/oracle/vax73-privileges.md §1 --
         * the correct home for NOTALLPRIV, measured on VAX1 2026-07-30. */
        { 1664,  "SYSTEM", 'W', "NOTALLPRIV",    "not all requested privileges authorized" },
        { 2096,  "SYSTEM", 'W', "CANCEL",        "I/O operation canceled" },
        { 2160,  "SYSTEM", 'W', "ENDOFFILE",     "end of file" },
        { 2204,  "SYSTEM", 'W', "UNWIND",        "unwind in progress" },
        { 2212,  "SYSTEM", 'E', "NOCMKRNL",      "no CMKRNL privilege" },
        /* ORACLE-PINNED (vms-8019): $SSDEF SS$_NONEXPR 2280;
         * F$MESSAGE(2280) -> "%SYSTEM-W-NONEXPR, nonexistent process".
         * Replaces 2540/'E' -- F$MESSAGE(2540) on the oracle is
         * "%SYSTEM-F-RIGHTSFULL, rights list is full". */
        { 2280,  "SYSTEM", 'W', "NONEXPR",       "nonexistent process" },
        { 2328,  "SYSTEM", 'W', "RESIGNAL",      "resignal condition" },
        { 2340,  "SYSTEM", 'S', "CONTINUE",      "continue execution" },
        { 2552,  "SYSTEM", 'W', "OPINCOMPL",     "operation incomplete" },
        { 2584,  "SYSTEM", 'W', "SUSPENDED",     "process suspended" },
        { 2588,  "SYSTEM", 'W', "NOTQUEUED",     "not queued" },
        { 2632,  "SYSTEM", 'E', "INCOMPAT",      "incompatible attributes" },
        { 2680,  "SYSTEM", 'E', "NOSUCHDEV",     "no such device" },
        { 2688,  "SYSTEM", 'E', "DEVNOTMOUNT",   "device not mounted" },
        { 2696,  "SYSTEM", 'E', "NOSUCHFILE",    "no such file" },
        { 2700,  "SYSTEM", 'W', "NOTRAN",        "no translation for logical name" },
        { 2704,  "SYSTEM", 'E', "DEVINACT",      "device inactive" },
        { 2720,  "SYSTEM", 'W', "CVTUNGRANT",    "convert ungrantable" },
        { 2732,  "SYSTEM", 'E', "NOSLOT",        "no PCB slot available" },
        { 2736,  "SYSTEM", 'E', "FILALRACC",     "file already accessed" },
        { 2748,  "SYSTEM", 'E', "EXENQLM",       "exceeded enqueue limit" },
        { 2756,  "SYSTEM", 'E', "EXASTLM",       "exceeded AST limit" },
        { 2764,  "SYSTEM", 'E', "EXBYTLM",       "exceeded byte count limit" },
        { 35820, "SYSTEM", 'W', "ITEMNOTFOUND",  "item not found" },
        { 0, NULL, 0, NULL, NULL }
    };

    for (int i = 0; msg_table[i].facility; i++) {
        if (msg_table[i].code == code) {
            snprintf(result, result_size, "%%%s-%c-%s, %s",
                     msg_table[i].facility, msg_table[i].sev,
                     msg_table[i].ident, msg_table[i].text);
            return 0;
        }
    }

    /* Unknown code */
    snprintf(result, result_size, "%%SYSTEM-?-UNKNOWN, message code %%X%08lX", code);
    result[result_size - 1] = '\0';
    return 0;
}

/*
 * fao_next_arg() - Extract and consume next comma-delimited arg from *pp.
 * Strips quotes and whitespace. Returns the arg in out_buf (out_size).
 * Returns 1 if an arg was available, 0 if none left.
 */
static int fao_next_arg(const char **pp, char *out_buf, size_t out_size)
{
    const char *p = *pp;
    if (!p || !*p) return 0;

    /* Skip comma + spaces */
    while (*p == ',') p++;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 0;

    /* Copy until next unquoted comma */
    size_t oi = 0;
    int in_quote = 0;
    while (*p && (in_quote || *p != ',') && oi < out_size - 1) {
        if (*p == '"') {
            if (in_quote && *(p+1) == '"') {
                if (oi < out_size - 1) out_buf[oi++] = '"';
                p += 2;
                continue;
            }
            in_quote = !in_quote;
            p++;
            continue;
        }
        out_buf[oi++] = *p++;
    }
    out_buf[oi] = '\0';

    /* Trim trailing spaces */
    while (oi > 0 && (out_buf[oi-1] == ' ' || out_buf[oi-1] == '\t'))
        out_buf[--oi] = '\0';

    /* Advance past comma */
    if (*p == ',') p++;
    *pp = p;
    return 1;
}

/*
 * F$FAO(control_string, args...) - Formatted ASCII output.
 *
 * Supported directives:
 *   !UL  unsigned longword decimal
 *   !SL  signed longword decimal
 *   !UW  unsigned word decimal
 *   !XL  longword hex (8 digits)
 *   !XW  word hex (4 digits)
 *   !OL  longword octal
 *   !ZL  zero-padded 10-digit decimal
 *   !AS  ASCII string arg
 *   !AC  counted ASCII string (first byte = length)
 *   !/   newline
 *   !!   literal !
 *   !_   tab
 *   !n*c repeat character c n times (e.g. !72*-)
 */
static int lex_fao(struct dcl_context *ctx, const char *args,
                   char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) return 0;

    /* Extract control string (first arg) */
    char ctrl[1024] = {0};
    const char *p = args;

    /* Skip leading whitespace */
    while (*p == ' ' || *p == '\t') p++;

    /* Get control string (quoted or unquoted) */
    if (*p == '"') {
        p++;
        size_t ci = 0;
        while (*p && ci < sizeof(ctrl) - 1) {
            if (*p == '"') {
                if (*(p+1) == '"') { ctrl[ci++] = '"'; p += 2; continue; }
                p++; break;
            }
            ctrl[ci++] = *p++;
        }
        ctrl[ci] = '\0';
    } else {
        size_t ci = 0;
        while (*p && *p != ',' && ci < sizeof(ctrl) - 1)
            ctrl[ci++] = *p++;
        ctrl[ci] = '\0';
        /* Trim */
        while (ci > 0 && (ctrl[ci-1]==' '||ctrl[ci-1]=='\t')) ctrl[--ci]='\0';
    }

    /* Skip past control string to remaining args */
    if (*p == ',') p++;

    /* Process the control string */
    size_t ri = 0;
    const char *c = ctrl;
    while (*c && ri < result_size - 1) {
        if (*c != '!') {
            result[ri++] = *c++;
            continue;
        }
        c++; /* consume '!' */

        /* Check for repeat: !n*ch */
        if (*c >= '1' && *c <= '9') {
            char numstr[16];
            size_t ni = 0;
            while (*c >= '0' && *c <= '9' && ni < sizeof(numstr)-1)
                numstr[ni++] = *c++;
            numstr[ni] = '\0';
            int count = (int)strtol(numstr, NULL, 10);
            if (*c == '*') {
                c++;
                char fill = *c ? *c++ : ' ';
                for (int k = 0; k < count && ri < result_size - 1; k++)
                    result[ri++] = fill;
            }
            /* If no '*', just skip the number (malformed) */
            continue;
        }

        if (*c == '/') {
            /* newline */
            result[ri++] = '\n';
            c++;
        } else if (*c == '!') {
            result[ri++] = '!';
            c++;
        } else if (*c == '_') {
            result[ri++] = '\t';
            c++;
        } else if (c[0] == 'U' && c[1] == 'L') {
            char arg[128] = "0"; fao_next_arg(&p, arg, sizeof(arg));
            unsigned long v = strtoul(arg, NULL, 0);
            ri += (size_t)snprintf(result+ri, result_size-ri, "%lu", v);
            c += 2;
        } else if (c[0] == 'S' && c[1] == 'L') {
            char arg[128] = "0"; fao_next_arg(&p, arg, sizeof(arg));
            long v = strtol(arg, NULL, 0);
            ri += (size_t)snprintf(result+ri, result_size-ri, "%ld", v);
            c += 2;
        } else if (c[0] == 'U' && c[1] == 'W') {
            char arg[128] = "0"; fao_next_arg(&p, arg, sizeof(arg));
            unsigned long v = strtoul(arg, NULL, 0) & 0xFFFF;
            ri += (size_t)snprintf(result+ri, result_size-ri, "%lu", v);
            c += 2;
        } else if (c[0] == 'X' && c[1] == 'L') {
            char arg[128] = "0"; fao_next_arg(&p, arg, sizeof(arg));
            unsigned long v = strtoul(arg, NULL, 0);
            ri += (size_t)snprintf(result+ri, result_size-ri, "%08lX", v);
            c += 2;
        } else if (c[0] == 'X' && c[1] == 'W') {
            char arg[128] = "0"; fao_next_arg(&p, arg, sizeof(arg));
            unsigned long v = strtoul(arg, NULL, 0) & 0xFFFF;
            ri += (size_t)snprintf(result+ri, result_size-ri, "%04lX", v);
            c += 2;
        } else if (c[0] == 'O' && c[1] == 'L') {
            char arg[128] = "0"; fao_next_arg(&p, arg, sizeof(arg));
            unsigned long v = strtoul(arg, NULL, 0);
            ri += (size_t)snprintf(result+ri, result_size-ri, "%lo", v);
            c += 2;
        } else if (c[0] == 'Z' && c[1] == 'L') {
            char arg[128] = "0"; fao_next_arg(&p, arg, sizeof(arg));
            unsigned long v = strtoul(arg, NULL, 0);
            ri += (size_t)snprintf(result+ri, result_size-ri, "%010lu", v);
            c += 2;
        } else if (c[0] == 'A' && c[1] == 'S') {
            char arg[1024] = ""; fao_next_arg(&p, arg, sizeof(arg));
            ri += (size_t)snprintf(result+ri, result_size-ri, "%s", arg);
            c += 2;
        } else if (c[0] == 'A' && c[1] == 'C') {
            /* Counted string: first byte = length, rest = chars */
            char arg[1024] = ""; fao_next_arg(&p, arg, sizeof(arg));
            if (arg[0]) {
                int cnt = (unsigned char)arg[0];
                size_t slen2 = strlen(arg+1);
                if (cnt > (int)slen2) cnt = (int)slen2;
                ri += (size_t)snprintf(result+ri, result_size-ri, "%.*s", cnt, arg+1);
            }
            c += 2;
        } else {
            /* Unknown directive — emit literally */
            result[ri++] = '!';
        }
    }

    result[ri] = '\0';
    return 0;
}

/*
 * F$PRIVILEGE(priv_list) - Check whether the current process holds all
 * of the listed privileges.
 *
 * Privilege list is comma-separated: "SYSPRV,TMPMBX"
 * Returns "TRUE" if ALL listed privileges are held, "FALSE" otherwise.
 *
 * READS THE EXECUTIVE FRESH, EVERY CALL (vms-2b8 round 4) -- deliberately
 * NOT ctx->privileges. This is the round-3 fix's own bug, found by
 * measurement on a real QEMU boot, not by inspection:
 *
 *   $ SHOW PROCESS/PRIVILEGES        -> Authorized: CMEXEC CMKRNL SETPRV WORLD
 *   $ BEFORE = F$PRIVILEGE("SETPRV") -> "TRUE"
 *   $ SET PROCESS/PRIVILEGES=(OPER)
 *   $ SHOW PROCESS/PRIVILEGES        -> UNCHANGED: still CMEXEC CMKRNL SETPRV WORLD
 *   $ AFTER = F$PRIVILEGE("SETPRV")  -> "FALSE"   <-- same process, same moment
 *
 * Root cause: SET PROCESS/PRIVILEGES (src/vmsdcl/dcl_cmd_set.c,
 * cmd_set_process()) REPLACES ctx->privileges outright with whatever
 * string was asked for -- a local, unauthenticated self-assertion with no
 * connection to the executive. Masking that value to VMS_PRV_M_ENFORCED
 * (the round-3 fix) closed the OVER-claim direction (F$PRIVILEGE saying
 * TRUE for something SHOW PROCESS/PRIVILEGES correctly omits) but left
 * this UNDER-claim direction wide open: a single SET PROCESS/PRIVILEGES
 * call for an unrelated, unenforced name (OPER) silently discarded every
 * bit ctx->privileges used to carry, including SETPRV/WORLD/CMKRNL/CMEXEC
 * -- privileges the executive still genuinely holds and SHOW
 * PROCESS/PRIVILEGES still correctly reports. Two surfaces describing the
 * same process at the same instant disagreed either way; masking a stale,
 * mutable local copy cannot fix that, because the copy itself is the
 * defect. The two surfaces can only be made to agree by construction: read
 * the SAME live source SHOW PROCESS/PRIVILEGES reads
 * (vms_kif_getjpi_self()), every call, so there is no local state left to
 * desynchronize. See src/kernel/vms_ioctl.h's VMS_PRV_M_ENFORCED comment
 * for what is actually enforced and why GROUP is absent from it.
 *
 * `ctx` is retained in the signature only because this function is called
 * through a common lexical-function pointer table; it is otherwise unused.
 */
static int lex_privilege(struct dcl_context *ctx, const char *args,
                         char *result, size_t result_size)
{
    (void)ctx;
    strncpy(result, "FALSE", result_size - 1);
    result[result_size - 1] = '\0';
    if (!args) return 0;

    char priv_str[256];
    strncpy(priv_str, args, sizeof(priv_str) - 1);
    priv_str[sizeof(priv_str) - 1] = '\0';

    /* Trim + unquote */
    char *s = priv_str;
    while (*s == ' ') s++;
    size_t l = strlen(s);
    while (l > 0 && (s[l-1]==' '||s[l-1]=='\t')) s[--l]='\0';
    if (l >= 2 && s[0]=='"' && s[l-1]=='"') { s[l-1]='\0'; s++; l-=2; }
    /* Uppercase */
    for (size_t i = 0; s[i]; i++) s[i] = (char)toupper((unsigned char)s[i]);

    uint64_t needed = parse_privilege_string(s);
    if (needed == 0) {
        /* No recognized privilege → TRUE (empty list) */
        strncpy(result, "TRUE", result_size - 1);
        return 0;
    }

    /*
     * Fail closed: a privilege check that cannot reach the executive has
     * no basis to claim TRUE for anything. Leaves result at "FALSE" (the
     * default set above).
     */
    struct vms_procinfo info;
    memset(&info, 0, sizeof(info));
    uint32_t jst = vms_kif_getjpi_self(&info);
    if (!(jst & 1))
        return 0;

    uint64_t enforced_held = info.cur_privs & VMS_PRV_M_ENFORCED;
    if ((enforced_held & needed) == needed)
        strncpy(result, "TRUE", result_size - 1);
    else
        strncpy(result, "FALSE", result_size - 1);

    result[result_size - 1] = '\0';
    return 0;
}

/*
 * F$DIRECTORY() - Return current default directory in VMS format.
 * Equivalent to F$ENVIRONMENT("DEFAULT").
 */
static int lex_directory(struct dcl_context *ctx, const char *args,
                         char *result, size_t result_size)
{
    (void)args;
    strncpy(result, ctx->default_dir, result_size - 1);
    result[result_size - 1] = '\0';
    return 0;
}

/*
 * F$UNIQUE() - Return a unique number suitable for temporary file names.
 * Uses a monotonically increasing counter seeded with PID + time.
 */
static int lex_unique(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    (void)ctx;
    (void)args;
    static unsigned long unique_counter = 0;
    if (unique_counter == 0) {
        /* Seed with PID and time so different processes get different ranges */
        unique_counter = (unsigned long)getpid() * 1000UL +
                         (unsigned long)time(NULL) % 1000UL;
    }
    snprintf(result, result_size, "%lu", unique_counter++);
    return 0;
}

/*
 * F$PID(context) - Return next PID in process list.
 *
 * Context is a symbol name used to track iteration state.
 * First call with empty context: returns first PID, sets context.
 * Subsequent calls: returns next PID.
 * When no more processes: returns "".
 * PIDs returned as 8-digit hex strings (VMS format).
 */
#define MAX_PID_LIST 256
static pid_t pid_list[MAX_PID_LIST];
static int pid_count = 0;
static int pid_index = 0;

static void populate_pid_list(void)
{
    pid_count = 0;
    pid_index = 0;
    DIR *d = opendir("/proc");
    if (!d) {
        /* Fallback: just return our own PID */
        pid_list[pid_count++] = getpid();
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && pid_count < MAX_PID_LIST) {
        /* Only numeric entries are PIDs */
        char *endp;
        long p = strtol(ent->d_name, &endp, 10);
        if (*endp == '\0' && p > 0) {
            pid_list[pid_count++] = (pid_t)p;
        }
    }
    closedir(d);
}

static int lex_pid(struct dcl_context *ctx, const char *args,
                   char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) { populate_pid_list(); }

    /* Parse the context symbol name */
    char sym_name[256] = {0};
    if (args) {
        const char *p = args;
        while (*p == ' ') p++;
        strncpy(sym_name, p, sizeof(sym_name) - 1);
        size_t len = strlen(sym_name);
        while (len > 0 && (sym_name[len-1] == ' ' || sym_name[len-1] == '\t'))
            sym_name[--len] = '\0';
        if (len >= 2 && sym_name[0] == '"' && sym_name[len-1] == '"') {
            sym_name[len-1] = '\0';
            memmove(sym_name, sym_name + 1, len - 1);
        }
    }

    /* Check if context symbol is empty or "0" → fresh scan */
    const char *ctx_val = NULL;
    if (sym_name[0] != '\0') {
        ctx_val = dcl_sym_get(sym_name);
    }
    if (!ctx_val || ctx_val[0] == '\0' || strcmp(ctx_val, "0") == 0) {
        populate_pid_list();
    }

    if (pid_index >= pid_count) {
        /* No more processes */
        result[0] = '\0';
        if (sym_name[0] != '\0')
            dcl_sym_set(sym_name, "", DCL_SYM_LOCAL);
        return 0;
    }

    snprintf(result, result_size, "%08X", (unsigned)pid_list[pid_index++]);

    /* Update context symbol with current index */
    if (sym_name[0] != '\0') {
        char idx_str[16];
        snprintf(idx_str, sizeof(idx_str), "%d", pid_index);
        dcl_sym_set(sym_name, idx_str, DCL_SYM_LOCAL);
    }

    return 0;
}

/*
 * F$CONTEXT(context_sym, ctx_type, sel_item, sel_value, sel_comp)
 * Sets up selection criteria for F$PID iteration.
 * Returns "" on success.
 */
static int lex_context(struct dcl_context *ctx, const char *args,
                       char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) return 0;

    /* Parse: context_sym, ctx_type, sel_item, sel_value, sel_comp */
    /* We accept and acknowledge the parameters but F$PID returns all processes */
    const char *p = args;
    char sym_name[256] = {0};

    while (*p == ' ') p++;
    /* Extract first arg (context symbol name) */
    size_t i = 0;
    int in_quote = 0;
    while (*p && i < sizeof(sym_name) - 1) {
        if (*p == '"') { in_quote = !in_quote; p++; continue; }
        if (*p == ',' && !in_quote) break;
        sym_name[i++] = *p++;
    }
    sym_name[i] = '\0';
    /* Trim */
    size_t len = strlen(sym_name);
    while (len > 0 && (sym_name[len-1] == ' ' || sym_name[len-1] == '\t'))
        sym_name[--len] = '\0';

    /* Initialize context symbol to "0" to signal fresh scan on next F$PID */
    if (sym_name[0] != '\0')
        dcl_sym_set(sym_name, "0", DCL_SYM_LOCAL);

    return 0;
}

/*
 * F$DEVICE(search_name, dev_class, dev_type [, stream_id])
 * Iterative device name lookup.
 */
#define MAX_DEV_LIST 64
static char dev_list[MAX_DEV_LIST][64];
static int dev_count = 0;
static int dev_index = 0;

/*
 * Fill dev_list from the executive's device table (vms-fb9).
 *
 * WHAT THIS USED TO BE: a hardcoded array -- "_OPA0:", "_FTA0:",
 * "SYS$SYSDEVICE:" -- unioned with /proc/mounts, where every line whose
 * device began with "/dev/" was uppercased into "_SDA1:"-style VMS names.
 * F$DEVICE therefore enumerated devices that did not exist, and could not
 * enumerate one that did. Same defect as SHOW DEVICE had, one layer over:
 * a reader with its own private idea of what the system contains.
 *
 * The device list is executive-resident (CLAUDE.md rule 11). $DEVICE_SCAN
 * over it is the only source here, and the names are the executive's own
 * physical form -- not re-decorated with a leading underscore, because
 * whether F$DEVICE returns "OPA0:" or "_OPA0:" on real VMS is not recorded
 * in anything this work has, and inventing the difference would be inventing
 * VMS behaviour (rule 10).
 *
 * The executive binding is not error-checked, for the same reason
 * src/libvms/syssvc/sys_lock.c's bind_to_executive() is not: the state it
 * would test for is one OVMX is never in (src/ovmx_init/ovmx_init.c refuses
 * to boot without /dev/vms), and the only thing such a branch could do here
 * is put back a private list.
 */
static void populate_device_list(const char *pattern)
{
    uint32_t index = 0;
    struct vms_devinfo info;

    dev_count = 0;
    dev_index = 0;

    (void)vms_kif_open();

    while (dev_count < MAX_DEV_LIST &&
           vms_kif_devscan(&index, &info) == SS$_NORMAL) {
        info.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

        if (pattern[0] != '\0' && pattern[0] != '*' &&
            fnmatch(pattern, info.devnam, FNM_CASEFOLD) != 0)
            continue;

        strncpy(dev_list[dev_count], info.devnam, 63);
        dev_list[dev_count][63] = '\0';
        dev_count++;
    }
}

static int lex_device(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) {
        populate_device_list("*");
    }

    /* Parse: search_name [, dev_class [, dev_type [, stream_id]]] */
    char search[256] = "*";
    if (args) {
        const char *p = args;
        while (*p == ' ') p++;
        size_t i = 0;
        while (*p && *p != ',' && i < sizeof(search) - 1) {
            if (*p != '"') search[i++] = *p;
            p++;
        }
        search[i] = '\0';
        /* Trim */
        size_t len = strlen(search);
        while (len > 0 && (search[len-1] == ' ' || search[len-1] == '\t'))
            search[--len] = '\0';
    }

    /* On first call or when search pattern changes, repopulate */
    if (dev_count == 0 || dev_index >= dev_count) {
        populate_device_list(search);
    }

    if (dev_index >= dev_count) {
        result[0] = '\0';
        return 0;
    }

    strncpy(result, dev_list[dev_index++], result_size - 1);
    result[result_size - 1] = '\0';
    return 0;
}

/*
 * F$GETDVI(device, item) - Get device information.
 */
static int lex_getdvi(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) return 0;

    /* Parse: device, item */
    const char *p = args;
    while (*p == ' ') p++;

    char device[128] = {0};
    size_t i = 0;
    int in_quote = 0;
    while (*p && i < sizeof(device) - 1) {
        if (*p == '"') { in_quote = !in_quote; p++; continue; }
        if (*p == ',' && !in_quote) break;
        device[i++] = (char)toupper((unsigned char)*p);
        p++;
    }
    device[i] = '\0';
    /* Trim */
    size_t dlen = strlen(device);
    while (dlen > 0 && (device[dlen-1] == ' ' || device[dlen-1] == '\t'))
        device[--dlen] = '\0';

    if (*p == ',') p++;
    while (*p == ' ') p++;

    char item[64] = {0};
    i = 0;
    in_quote = 0;
    while (*p && i < sizeof(item) - 1) {
        if (*p == '"') { in_quote = !in_quote; p++; continue; }
        if (*p == ',' && !in_quote) break;
        item[i++] = (char)toupper((unsigned char)*p);
        p++;
    }
    item[i] = '\0';
    size_t ilen = strlen(item);
    while (ilen > 0 && (item[ilen-1] == ' ' || item[ilen-1] == '\t'))
        item[--ilen] = '\0';

    /* Determine which Linux path to stat */
    const char *stat_path = "/";
    int is_terminal = 0;
    if (strstr(device, "OPA0") || strstr(device, "FTA0") ||
        strstr(device, "TT") || strstr(device, "FT")) {
        is_terminal = 1;
    }

    if (strcmp(item, "DEVNAM") == 0) {
        /* Return canonical device name */
        if (strstr(device, "SYSDEVICE") || strstr(device, "SYS$SYSDEVICE")) {
            snprintf(result, result_size, "_SYS$SYSDEVICE:");
        } else {
            snprintf(result, result_size, "_%s:", device);
        }
    } else if (strcmp(item, "EXISTS") == 0) {
        /* Check if device exists — always TRUE for known devices */
        snprintf(result, result_size, "TRUE");
    } else if (strcmp(item, "DEVCLASS") == 0) {
        if (is_terminal)
            snprintf(result, result_size, "66"); /* DC$_TERM */
        else
            snprintf(result, result_size, "1");  /* DC$_DISK */
    } else if (strcmp(item, "DEVTYPE") == 0) {
        if (is_terminal)
            snprintf(result, result_size, "112"); /* DT$_VT100 */
        else
            snprintf(result, result_size, "44");  /* DT$_RA92 */
    } else if (strcmp(item, "VOLNAM") == 0) {
        if (strstr(device, "SYSDEVICE") || device[0] == '\0')
            snprintf(result, result_size, "OVMXSYS");
        else
            snprintf(result, result_size, "VOLUME");
    } else if (strcmp(item, "FREEBLOCKS") == 0) {
        struct statvfs st;
        if (statvfs(stat_path, &st) == 0) {
            unsigned long free_blocks = (unsigned long)(st.f_bavail * st.f_frsize / 512);
            snprintf(result, result_size, "%lu", free_blocks);
        } else {
            snprintf(result, result_size, "0");
        }
    } else if (strcmp(item, "MAXBLOCK") == 0) {
        struct statvfs st;
        if (statvfs(stat_path, &st) == 0) {
            unsigned long total_blocks = (unsigned long)(st.f_blocks * st.f_frsize / 512);
            snprintf(result, result_size, "%lu", total_blocks);
        } else {
            snprintf(result, result_size, "0");
        }
    } else if (strcmp(item, "MOUNTCNT") == 0) {
        snprintf(result, result_size, "1");
    } else {
        result[0] = '\0';
    }

    return 0;
}

/*
 * F$IDENTIFIER(id, conversion) - Convert between UIC and identifier names.
 */
static int lex_identifier(struct dcl_context *ctx, const char *args,
                          char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) return 0;

    /* Parse: id, conversion */
    const char *p = args;
    while (*p == ' ') p++;

    char id_str[256] = {0};
    size_t i = 0;
    int in_quote = 0;
    while (*p && i < sizeof(id_str) - 1) {
        if (*p == '"') { in_quote = !in_quote; p++; continue; }
        if (*p == ',' && !in_quote) break;
        id_str[i++] = *p;
        p++;
    }
    id_str[i] = '\0';
    size_t ilen = strlen(id_str);
    while (ilen > 0 && (id_str[ilen-1] == ' ' || id_str[ilen-1] == '\t'))
        id_str[--ilen] = '\0';

    if (*p == ',') p++;
    while (*p == ' ') p++;

    char conv[64] = {0};
    i = 0;
    in_quote = 0;
    while (*p && i < sizeof(conv) - 1) {
        if (*p == '"') { in_quote = !in_quote; p++; continue; }
        conv[i++] = (char)toupper((unsigned char)*p);
        p++;
    }
    conv[i] = '\0';
    size_t clen = strlen(conv);
    while (clen > 0 && (conv[clen-1] == ' ' || conv[clen-1] == '\t'))
        conv[--clen] = '\0';

    /* Uppercase the id for comparison */
    char id_upper[256];
    strncpy(id_upper, id_str, sizeof(id_upper) - 1);
    id_upper[sizeof(id_upper) - 1] = '\0';
    for (size_t j = 0; id_upper[j]; j++)
        id_upper[j] = (char)toupper((unsigned char)id_upper[j]);

    if (strcmp(conv, "NAME_TO_NUMBER") == 0) {
        /* Convert username to UIC number */
        /* Hardcoded well-known identities */
        if (strcmp(id_upper, "SYSTEM") == 0) {
            snprintf(result, result_size, "%d", (1 << 16) | 4); /* [1,4] */
        } else if (strcmp(id_upper, "DEFAULT") == 0) {
            snprintf(result, result_size, "%d", (200 << 16) | 1); /* [200,1] */
        } else {
            /* Try /etc/passwd lookup */
            struct passwd *pw = getpwnam(id_str);
            if (pw) {
                /* Map uid,gid to VMS UIC format [group,member] */
                snprintf(result, result_size, "%d",
                         (int)((pw->pw_gid << 16) | (pw->pw_uid & 0xFFFF)));
            } else {
                snprintf(result, result_size, "0");
            }
        }
    } else if (strcmp(conv, "NUMBER_TO_NAME") == 0) {
        /* Convert UIC number to username */
        long uic = strtol(id_str, NULL, 0);
        int member = (int)(uic & 0xFFFF);
        int group = (int)((uic >> 16) & 0xFFFF);

        if (group == 1 && member == 4) {
            snprintf(result, result_size, "SYSTEM");
        } else {
            /* Try /etc/passwd lookup by uid */
            struct passwd *pw = getpwuid((uid_t)member);
            if (pw) {
                strncpy(result, pw->pw_name, result_size - 1);
                result[result_size - 1] = '\0';
                for (size_t j = 0; result[j]; j++)
                    result[j] = (char)toupper((unsigned char)result[j]);
            } else {
                snprintf(result, result_size, "[%d,%d]", group, member);
            }
        }
    } else {
        result[0] = '\0';
    }

    return 0;
}

/*
 * F$GETQUI(func, item [, id]) - Get queue information.
 */
static int lex_getqui(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) return 0;

    /* Parse: func, item [, id] */
    const char *p = args;
    while (*p == ' ') p++;

    char func[64] = {0};
    size_t i = 0;
    int in_quote = 0;
    while (*p && i < sizeof(func) - 1) {
        if (*p == '"') { in_quote = !in_quote; p++; continue; }
        if (*p == ',' && !in_quote) break;
        func[i++] = (char)toupper((unsigned char)*p);
        p++;
    }
    func[i] = '\0';
    size_t flen = strlen(func);
    while (flen > 0 && (func[flen-1] == ' ' || func[flen-1] == '\t'))
        func[--flen] = '\0';

    if (*p == ',') p++;
    while (*p == ' ') p++;

    char item_str[64] = {0};
    i = 0;
    in_quote = 0;
    while (*p && i < sizeof(item_str) - 1) {
        if (*p == '"') { in_quote = !in_quote; p++; continue; }
        if (*p == ',' && !in_quote) break;
        item_str[i++] = (char)toupper((unsigned char)*p);
        p++;
    }
    item_str[i] = '\0';
    size_t itlen = strlen(item_str);
    while (itlen > 0 && (item_str[itlen-1] == ' ' || item_str[itlen-1] == '\t'))
        item_str[--itlen] = '\0';

    /* Optional: id parameter */
    uint32_t entry_id = 0;
    if (*p == ',') {
        p++;
        while (*p == ' ') p++;
        char id_buf[32] = {0};
        i = 0;
        while (*p && *p != ',' && *p != ' ' && i < sizeof(id_buf) - 1) {
            if (*p != '"') id_buf[i++] = *p;
            p++;
        }
        id_buf[i] = '\0';
        entry_id = (uint32_t)strtoul(id_buf, NULL, 0);
    }

    if (strcmp(func, "DISPLAY_QUEUE") == 0) {
        /* Try to show queue info via vmsqueue API */
        struct vms_queue qinfo;
        const char *qname = "SYS$BATCH";
        int rc = vmsq_show_queue(qname, &qinfo);
        if (rc != 1) {  /* SS$_NORMAL = 1 */
            result[0] = '\0';
            return 0;
        }
        if (strcmp(item_str, "QUEUE_NAME") == 0) {
            strncpy(result, qinfo.name, result_size - 1);
            result[result_size - 1] = '\0';
        } else if (strcmp(item_str, "ENTRY_NUMBER") == 0) {
            snprintf(result, result_size, "%u", qinfo.entry_count);
        } else {
            result[0] = '\0';
        }
    } else if (strcmp(func, "DISPLAY_ENTRY") == 0) {
        struct vms_queue_entry entry;
        int rc = vmsq_show_entry(entry_id, &entry);
        if (rc != 1) {
            result[0] = '\0';
            return 0;
        }
        if (strcmp(item_str, "JOB_NAME") == 0) {
            strncpy(result, entry.job_name, result_size - 1);
            result[result_size - 1] = '\0';
        } else if (strcmp(item_str, "USERNAME") == 0) {
            strncpy(result, entry.username, result_size - 1);
            result[result_size - 1] = '\0';
        } else if (strcmp(item_str, "ENTRY_NUMBER") == 0) {
            snprintf(result, result_size, "%u", entry.entry_id);
        } else {
            result[0] = '\0';
        }
    } else {
        result[0] = '\0';
    }

    return 0;
}

/*
 * F$CVSI(bit_pos, length, source) - Extract signed bit field from string.
 */
static int lex_cvsi(struct dcl_context *ctx, const char *args,
                    char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) return 0;

    /* Parse: bit_pos, length, source */
    const char *p = args;
    while (*p == ' ') p++;
    int bit_pos = (int)strtol(p, NULL, 10);

    p = strchr(p, ',');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;
    int length = (int)strtol(p, NULL, 10);

    p = strchr(p, ',');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;

    /* Extract source string */
    char source[4096] = {0};
    strncpy(source, p, sizeof(source) - 1);
    size_t slen = strlen(source);
    while (slen > 0 && (source[slen-1] == ' ' || source[slen-1] == '\t'))
        source[--slen] = '\0';
    if (slen >= 2 && source[0] == '"' && source[slen-1] == '"') {
        source[slen-1] = '\0';
        memmove(source, source + 1, slen - 1);
        slen -= 2;
    }

    if (bit_pos < 0 || length <= 0 || length > 32) {
        snprintf(result, result_size, "0");
        return 0;
    }

    /* Treat source as byte array, extract bit field */
    const unsigned char *bytes = (const unsigned char *)source;
    size_t byte_len = slen;
    uint32_t value = 0;

    for (int b = 0; b < length && b < 32; b++) {
        int abs_bit = bit_pos + b;
        int byte_idx = abs_bit / 8;
        int bit_idx = abs_bit % 8;
        if (byte_idx >= 0 && (size_t)byte_idx < byte_len) {
            if (bytes[byte_idx] & (1U << bit_idx))
                value |= (1U << b);
        }
    }

    /* Sign extend */
    if (length < 32 && (value & (1U << (length - 1)))) {
        value |= ~((1U << length) - 1);
    }

    snprintf(result, result_size, "%d", (int32_t)value);
    return 0;
}

/*
 * F$CVUI(bit_pos, length, source) - Extract unsigned bit field from string.
 */
static int lex_cvui(struct dcl_context *ctx, const char *args,
                    char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) return 0;

    /* Parse: bit_pos, length, source */
    const char *p = args;
    while (*p == ' ') p++;
    int bit_pos = (int)strtol(p, NULL, 10);

    p = strchr(p, ',');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;
    int length = (int)strtol(p, NULL, 10);

    p = strchr(p, ',');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;

    /* Extract source string */
    char source[4096] = {0};
    strncpy(source, p, sizeof(source) - 1);
    size_t slen = strlen(source);
    while (slen > 0 && (source[slen-1] == ' ' || source[slen-1] == '\t'))
        source[--slen] = '\0';
    if (slen >= 2 && source[0] == '"' && source[slen-1] == '"') {
        source[slen-1] = '\0';
        memmove(source, source + 1, slen - 1);
        slen -= 2;
    }

    if (bit_pos < 0 || length <= 0 || length > 32) {
        snprintf(result, result_size, "0");
        return 0;
    }

    /* Treat source as byte array, extract bit field (unsigned) */
    const unsigned char *bytes = (const unsigned char *)source;
    size_t byte_len = slen;
    uint32_t value = 0;

    for (int b = 0; b < length && b < 32; b++) {
        int abs_bit = bit_pos + b;
        int byte_idx = abs_bit / 8;
        int bit_idx = abs_bit % 8;
        if (byte_idx >= 0 && (size_t)byte_idx < byte_len) {
            if (bytes[byte_idx] & (1U << bit_idx))
                value |= (1U << b);
        }
    }

    snprintf(result, result_size, "%u", value);
    return 0;
}

/*
 * Dispatch table for lexical functions.
 */
typedef int (*lex_func_t)(struct dcl_context *ctx, const char *args,
                          char *result, size_t result_size);

static const struct {
    const char *name;
    lex_func_t handler;
} lex_functions[] = {
    { "F$TIME",             lex_time },
    { "F$LENGTH",           lex_length },
    { "F$EXTRACT",          lex_extract },
    { "F$ELEMENT",          lex_element },
    { "F$LOCATE",           lex_locate },
    { "F$EDIT",             lex_edit },
    { "F$INTEGER",          lex_integer },
    { "F$STRING",           lex_string },
    { "F$TRNLNM",          lex_trnlnm },
    { "F$LOGICAL",          lex_trnlnm },  /* Alias */
    { "F$ENVIRONMENT",      lex_environment },
    { "F$PROCESS",          lex_process },
    { "F$MODE",             lex_mode },
    { "F$USER",             lex_user },
    { "F$VERIFY",           lex_verify },
    { "F$SEARCH",           lex_search },
    { "F$PARSE",            lex_parse },
    { "F$FILE_ATTRIBUTES",  lex_file_attributes },
    { "F$TYPE",             lex_type },
    { "F$CVTIME",           lex_cvtime },
    { "F$GETSYI",           lex_getsyi },
    { "F$GETJPI",           lex_getjpi },
    { "F$MESSAGE",          lex_message },
    { "F$FAO",              lex_fao },
    { "F$PRIVILEGE",        lex_privilege },
    { "F$DIRECTORY",        lex_directory },
    { "F$UNIQUE",           lex_unique },
    { "F$PID",              lex_pid },
    { "F$CONTEXT",          lex_context },
    { "F$DEVICE",           lex_device },
    { "F$GETDVI",           lex_getdvi },
    { "F$IDENTIFIER",       lex_identifier },
    { "F$GETQUI",           lex_getqui },
    { "F$CVSI",             lex_cvsi },
    { "F$CVUI",             lex_cvui },
    { NULL, NULL }
};

/*
 * Evaluate a lexical function call.
 *
 * Input: "F$FUNCNAME(args)"
 * Output: result string
 * Returns 0 on success, -1 on error.
 */
int dcl_eval_lexical(struct dcl_context *ctx, const char *expr,
                     char *result, size_t result_size)
{
    if (!expr || !result || result_size == 0) return -1;
    result[0] = '\0';

    /* Find function name */
    char func_name[64] = {0};
    const char *p = expr;
    while (*p == ' ') p++;

    size_t ni = 0;
    while (*p && *p != '(' && ni < sizeof(func_name) - 1) {
        func_name[ni++] = (char)toupper((unsigned char)*p);
        p++;
    }
    func_name[ni] = '\0';

    /* Find arguments (inside parentheses) */
    char args[4096] = {0};
    if (*p == '(') {
        p++;
        int depth = 1;
        size_t ai = 0;
        while (*p && depth > 0 && ai < sizeof(args) - 1) {
            if (*p == '(') depth++;
            else if (*p == ')') {
                depth--;
                if (depth == 0) break;
            }
            args[ai++] = *p++;
        }
        args[ai] = '\0';
    }

    /* Look up and call the function */
    for (int i = 0; lex_functions[i].name; i++) {
        if (strcmp(func_name, lex_functions[i].name) == 0) {
            return lex_functions[i].handler(ctx, args, result, result_size);
        }
    }

    /* Unknown function */
    snprintf(result, result_size, "");
    return -1;
}
