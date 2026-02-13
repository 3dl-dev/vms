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
 * F$SEARCH(filespec) - Search for files matching a wildcard.
 */
static int lex_search(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    result[0] = '\0';
    if (!args) return 0;

    char spec[512];
    strncpy(spec, args, sizeof(spec) - 1);
    spec[sizeof(spec) - 1] = '\0';

    /* Trim and unquote */
    char *s = spec;
    while (*s == ' ') s++;
    size_t slen = strlen(s);
    while (slen > 0 && (s[slen - 1] == ' ' || s[slen - 1] == '\t')) s[--slen] = '\0';
    if (slen >= 2 && s[0] == '"' && s[slen - 1] == '"') { s[slen - 1] = '\0'; s++; }

    /* Resolve to Linux path */
    char linux_path[1024];
    dcl_resolve_path(ctx, s, linux_path, sizeof(linux_path));

    /* Check if file exists */
    struct stat st;
    if (stat(linux_path, &st) == 0) {
        /* Return in VMS format */
        char vms_spec[1024];
        dcl_format_directory(ctx->default_linux, vms_spec, sizeof(vms_spec));
        /* For simplicity, return the linux path uppercased */
        const char *basename = strrchr(linux_path, '/');
        if (basename) basename++; else basename = linux_path;
        size_t vi = 0;
        for (size_t i = 0; basename[i] && vi < result_size - 1; i++) {
            result[vi++] = (char)toupper((unsigned char)basename[i]);
        }
        result[vi] = '\0';
    }

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
 * F$CVTIME([time_string [, output_format [, field]]]) - Convert time.
 */
static int lex_cvtime(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    (void)ctx;
    /* Simple implementation: just return current time in VMS format */
    /* A full implementation would parse the arguments */
    format_vms_time(result, result_size);
    if (args && *args) {
        /* TODO: Implement full CVTIME with field extraction */
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

    long code = strtol(s, NULL, 0);

    if (code == (long)SS$_NORMAL || code == 1) {
        strncpy(result, "%SYSTEM-S-NORMAL, normal successful completion", result_size - 1);
    } else if (code == 0) {
        strncpy(result, "%SYSTEM-W-NOSUCHFILE, no such file", result_size - 1);
    } else {
        snprintf(result, result_size, "%%SYSTEM-?-UNKNOWN, message code %%X%08lX", code);
    }

    result[result_size - 1] = '\0';
    return 0;
}

/*
 * F$FAO(control, args...) - Formatted ASCII output.
 * Simplified implementation supporting !AS, !UL, !SL, !XL, !ZL, !/.
 */
static int lex_fao(struct dcl_context *ctx, const char *args,
                   char *result, size_t result_size)
{
    (void)ctx;
    result[0] = '\0';
    if (!args) return 0;

    /* For simplicity, just return the control string with basic substitution */
    strncpy(result, args, result_size - 1);
    result[result_size - 1] = '\0';
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
