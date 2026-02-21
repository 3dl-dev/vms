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

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "ssdef.h"
#include <vms/privs.h>

/* External functions */
extern int dcl_translate_logical(const char *name, char *result, size_t result_size);
extern int dcl_resolve_path(struct dcl_context *ctx, const char *spec,
                            char *linux_path, size_t path_size);
extern int dcl_format_directory(const char *linux_path, char *vms_dir, size_t dir_size);

/* VMS month abbreviations */
static const char *vms_months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

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
    start = atoi(p);

    /* Skip to next comma */
    p = strchr(p, ',');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;
    len = atoi(p);

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
    int element = atoi(p);

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
        dcl_format_directory(ctx->default_linux, result, result_size);
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
    if (ctx->process_name[0]) {
        strncpy(result, ctx->process_name, result_size - 1);
    } else {
        strncpy(result, "_FTA0:", result_size - 1);
    }
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
            fsc->matches[fsc->match_count++] = strdup(de->d_name);
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
            strncpy(fsc->dir_linux, ctx->default_linux, sizeof(fsc->dir_linux) - 1);
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
                *cs_out = atoi(end);
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

    if (strcmp(s, "NODENAME") == 0 || strcmp(s, "SCSNODE") == 0) {
        strncpy(result, uts.nodename, result_size - 1);
        for (size_t i = 0; result[i]; i++)
            result[i] = (char)toupper((unsigned char)result[i]);
    } else if (strcmp(s, "VERSION") == 0) {
        strncpy(result, "V7.3", result_size - 1);
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
        { 36,    "SYSTEM", 'E', "NOPRIV",        "no privilege for attempted operation" },
        { 44,    "SYSTEM", 'E', "ABORT",         "abort" },
        { 292,   "SYSTEM", 'E', "INSFMEM",       "insufficient dynamic memory" },
        { 388,   "SYSTEM", 'E', "IVTIME",        "invalid time" },
        { 434,   "SYSTEM", 'E', "DUPLNAM",       "duplicate name" },
        { 444,   "SYSTEM", 'W', "NOLOGNAM",      "no logical name match" },
        { 532,   "SYSTEM", 'W', "NOTALLPRIV",    "not all requested privileges available" },
        { 548,   "SYSTEM", 'E', "IVIDENT",       "invalid identifier" },
        { 556,   "SYSTEM", 'E', "TIMEOUT",       "device timeout" },
        { 580,   "SYSTEM", 'E', "ILLIOFUNC",     "illegal I/O function" },
        { 588,   "SYSTEM", 'E', "NOMORENODE",    "no more cluster nodes" },
        { 596,   "SYSTEM", 'E', "IVLOGNAM",      "invalid logical name" },
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
        { 2096,  "SYSTEM", 'W', "CANCEL",        "I/O operation canceled" },
        { 2160,  "SYSTEM", 'W', "ENDOFFILE",     "end of file" },
        { 2204,  "SYSTEM", 'W', "UNWIND",        "unwind in progress" },
        { 2212,  "SYSTEM", 'E', "NOCMKRNL",      "no CMKRNL privilege" },
        { 2328,  "SYSTEM", 'W', "RESIGNAL",      "resignal condition" },
        { 2340,  "SYSTEM", 'S', "CONTINUE",      "continue execution" },
        { 2540,  "SYSTEM", 'E', "NONEXPR",       "nonexistent process" },
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
            int count = atoi(numstr);
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
 * Reads ctx->privileges (set from VMS_PRIVILEGES env at login).
 */
static int lex_privilege(struct dcl_context *ctx, const char *args,
                         char *result, size_t result_size)
{
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

    if ((ctx->privileges & needed) == needed)
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
    dcl_format_directory(ctx->default_linux, result, result_size);
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
