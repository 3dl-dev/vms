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
/* <pwd.h> is no longer used by any live code in this file: lex_user() reads
 * the executive's row and lex_identifier() reads neither the executive nor
 * the host. The include stays because tests/qemu/facility_defects.sh's
 * dcl-fuser-host-login-name, dcl-fident-num2name-host-passwd and
 * dcl-fident-name2num-host-passwd controls restore the getpwuid()/getpwnam()
 * calls verbatim, and a mutation that will not compile is a broken fixture
 * rather than a gate that bites. (A fourth control on this file,
 * dcl-fident-num2name-bracketed-uic, needs no passwd call: it restores an
 * INVENTED value rather than a leaked one.) */
#include <pwd.h>
#include <fnmatch.h>
#include <sys/statvfs.h>

#include "vmsqueue.h"

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/dcl_cmd.h"
#include "dcl/dcl_rms.h"          /* vms-481: F$ file lexicals reach files via RMS/ACP */
#include "dcl/symbol.h"
#include "ssdef.h"
/* Kernel-interface client: F$DEVICE enumerates the executive's device
 * table through it (vms-fb9), and F$SETPRV routes its privilege mutation
 * through vms_kif_setprv() here -- the SAME already-wired executive edge
 * DCL.EXE's native link resolves for F$GETJPI/F$DEVICE, NOT a new
 * cross-shareable-image call into libvms's sys$ vector. See the note above
 * populate_device_list. */
#include "vms_kif.h"
#include <vms/privs.h>
#include "vmsfs/filespec.h"
#include "sysgen_params.h"
#include "ovmx_identity.h"
/* The rights database: F$IDENTIFIER READS it rather than answering from a
 * table of its own (vms-2f8). See src/libvms/rtl/rightslist.c. */
#include "rightslist.h"

/* External functions */
extern void dcl_error(const char *facility, int severity, const char *ident,
                      const char *fmt, ...);
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
    int was_quoted = 0;
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') { s[len - 1] = '\0'; s++; was_quoted = 1; }

    /* F$INTEGER takes an EXPRESSION: an unquoted argument that names a defined
     * symbol is evaluated to that symbol's value first (VSI OpenVMS DCL
     * Dictionary, F$INTEGER). MMK's end-of-command marker computes
     * MMK____status = F$INTEGER($STATUS); without this it would read 0 (an even,
     * "failed" status) and MMK would abort the build after the first command. */
    if (!was_quoted && s[0] != '\0') {
        const char *sv = dcl_sym_get(s);
        if (sv) { strncpy(str, sv, sizeof(str) - 1); str[sizeof(str) - 1] = '\0'; s = str; }
    }

    /* Radix-aware: $STATUS is stored VMS-style as "%X00000001", so
     * F$INTEGER($STATUS) must read a "%X" value (and %O/%D/%B, 0x) as an
     * integer. Reference: VSI OpenVMS DCL Dictionary, F$INTEGER + radix
     * qualifiers. Falls back to the previous strtol for anything else. */
    extern long dcl_parse_int(const char *s, int *ok);
    int iok;
    long val = dcl_parse_int(s, &iok);
    if (!iok) val = strtol(s, NULL, 0);
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
    /*
     * THE FALLBACK IS DELETED, NOT CORRECTED (vms-cb5, CLAUDE.md Rule 10).
     *
     * Two fallbacks stood here, taken when the executive holds no user name
     * for this process (ctx->username is seeded from the executive's process
     * table in dcl_main.c and from nowhere else):
     *
     *     struct passwd *pw = getpwuid(getuid());
     *     if (pw) result = upcase(pw->pw_name);
     *     else    result = "SYSTEM";
     *
     * MEASURED, not reasoned (tests/qemu/test_syssvc_ident.c, scenario C, on
     * a real /dev/vms under QEMU): an UNPRIVILEGED process whose attempt to
     * become SYSTEM the executive had just REFUSED with SS$_NOPRIV, running
     * in an initramfs with no /etc/passwd, got
     *
     *     $ IDENT_U = F$GETJPI("","USERNAME")
     *     $ SHOW SYMBOL IDENT_U
     *       IDENT_U = "SYSTEM"
     *
     * while SHOW PROCESS -- reading the SAME field of the SAME executive row
     * in the SAME process at the SAME moment -- honestly printed `User:`
     * blank. So the display told the truth and the programmatic path
     * fabricated the most privileged name on the system, for the one process
     * that had just been refused it. It degraded UPWARD, which is the
     * signature of this defect class.
     *
     * Why no replacement value is chosen here: VMS has no process without a
     * user name -- the name lives in the executive's process table, and on
     * the oracle a subprocess carries its creator's (VAX1, OpenVMS VAX V7.3:
     * SPAWN answers "%DCL-S-SPAWNED, process SYSTEM_1 spawned"; the capture
     * is cited at tests/uat/vms_session_qemu.sh) -- so this is a condition
     * VMS never faces, and Rule 10 forbids inventing a plausible-looking
     * handler for one. The honest answer is the one the executive gave,
     * which for an unnamed process is the empty string. That is not a value
     * this file picks: it is what SHOW PROCESS already prints for this exact
     * case (src/vmsdcl/dcl_cmd_show.c), so the fix makes the two readers of
     * the fact agree instead of introducing a third answer.
     *
     * getpwuid() had to go with it and is not the lesser half. A Linux
     * account name upcased is not a VMS user name; substituting one is the
     * same fabrication wearing a more convincing costume, and it is the
     * branch taken on a system that does have an /etc/passwd. Measured on
     * this repo's own build host, with the branch restored: F$USER()
     * answered "BARON", the developer's login name (vms-f39).
     *
     * THE UNNAMED ROW IS REACHABLE ON THE ONE RUNTIME TARGET, and the repo
     * says so elsewhere -- an earlier draft of this comment claimed the
     * opposite ("no interactive session reaches DCL with an unnamed row")
     * and it was false. vms_proc_register() in src/kernel/vms_module.c
     * derives a fresh row for each new task and inherits nothing from the
     * parent (src/kernel/vms_proctab.c), so a SPAWN from an ordinary console
     * login yields a DCL whose row has no name. That blank is PINNED, not
     * denied, by tests/uat/vms_session_qemu.sh ('User: +Process ID:') and it
     * is $CREPRC identity propagation's to fix (vms-afd), not this
     * function's. What this function owes that state is an honest answer,
     * which is what is below.
     */
    if (ctx->username[0])
        strncpy(result, ctx->username, result_size - 1);
    else
        result[0] = '\0';
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
 * F$LICENSE(license-name) - Report whether a product license is loaded.
 *
 * VMS SEMANTICS (operation), grounded to the public VSI OpenVMS DCL
 * Dictionary (docs.vmssoftware.com, "F$LICENSE Lexical Function") and the
 * VSI OpenVMS Wiki lexical-function list: F$LICENSE(name) returns the
 * integer 1 when the named license is active/loaded and 0 when it is not.
 * (It is a modern-VMS lexical: absent on OpenVMS VAX V7.3 -- the lab-2 VAX1
 * oracle answers %DCL-W-IVFNAM for it, 11-AUG-2026 -- and present from the
 * Alpha/I64 line onward, which is OVMX's platform target.)
 *
 * OVMX POLICY (ours, an OVMX design choice -- operator ruling 2026-08-11,
 * licensing-stance-grant-all): grant-all. OVMX does not gate, meter, or
 * enforce licenses; the facility exists ONLY so software that queries a
 * license and refuses to run without one passes. So this returns 1 for ANY
 * product name -- grant-by-query, not a fixed PAK list -- which is the whole
 * point: a checker asking F$LICENSE("<anything>") is told the license is
 * loaded. This is NOT VMS-authentic behavior (real VMS returns 0 for an
 * unloaded product); it is OVMX's deliberate always-grant compatibility
 * stance, and it is labeled as ours here rather than presented as VMS.
 */
static int lex_license(struct dcl_context *ctx, const char *args,
                       char *result, size_t result_size)
{
    (void)ctx;
    (void)args;  /* grant-by-query: any named product reports loaded */
    snprintf(result, result_size, "1");
    return 0;
}

/*
 * F$SEARCH wildcard context — tracks iterative directory scans.
 * One slot per unique filespec (up to 8 concurrent searches).
 */
#define FSEARCH_MAX_CTX  8

/*
 * vms-481: F$SEARCH iterates the Files-11 ODS-2 ACP wildcard directory context
 * (sys$parse + sys$search over /dev/vms) instead of an opendir()/readdir()
 * snapshot of a /vms passthrough. One slot per unique filespec holds the live
 * executive search handle; each F$SEARCH call returns the NEXT match (genuine
 * ODS-2 order, with a real File ID behind it), "" when the directory is
 * exhausted (RMS$_NMF), restarting on the next call for the same spec.
 */
static struct fsearch_ctx {
    char    filespec[512];          /* the VMS filespec pattern (search key) */
    struct dcl_rms_dir *dir;        /* the executive wildcard search over it */
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

static void fsearch_slot_clear(struct fsearch_ctx *fsc)
{
    if (fsc->dir) { dcl_rms_dir_close(fsc->dir); fsc->dir = NULL; }
    fsc->filespec[0] = '\0';
}

static struct fsearch_ctx *fsearch_alloc(const char *filespec)
{
    for (int i = 0; i < FSEARCH_MAX_CTX; i++) {
        if (!fsearch_slots[i].filespec[0]) {
            strncpy(fsearch_slots[i].filespec, filespec,
                    sizeof(fsearch_slots[i].filespec) - 1);
            fsearch_slots[i].dir = NULL;
            return &fsearch_slots[i];
        }
    }
    /* No free slot — evict slot 0 (releasing its executive channel), shift. */
    fsearch_slot_clear(&fsearch_slots[0]);
    memmove(&fsearch_slots[0], &fsearch_slots[1],
            sizeof(fsearch_slots[0]) * (FSEARCH_MAX_CTX - 1));
    memset(&fsearch_slots[FSEARCH_MAX_CTX - 1], 0, sizeof(fsearch_slots[0]));
    strncpy(fsearch_slots[FSEARCH_MAX_CTX - 1].filespec, filespec,
            sizeof(fsearch_slots[0].filespec) - 1);
    return &fsearch_slots[FSEARCH_MAX_CTX - 1];
}

/*
 * F$SEARCH(filespec) - Iterative wildcard file search over the ACP.
 *
 * First call with a given filespec opens the executive search and returns the
 * first match; subsequent calls return subsequent matches. Returns "" when
 * exhausted.
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

    /* Uppercase for the slot key (ODS-2 is case-insensitive). */
    char spec_upper[512];
    for (size_t i = 0; s[i] && i < sizeof(spec_upper) - 1; i++)
        spec_upper[i] = (char)toupper((unsigned char)s[i]);
    spec_upper[slen < sizeof(spec_upper) ? slen : sizeof(spec_upper) - 1] = '\0';

    struct fsearch_ctx *fsc = fsearch_find(spec_upper);
    if (!fsc) {
        fsc = fsearch_alloc(spec_upper);
        if (!fsc) return 0;
        fsc->dir = dcl_rms_dir_open(ctx, s);
        if (!fsc->dir) { fsearch_slot_clear(fsc); return 0; }
    }

    /* Next match. The resultant is already a full VMS spec
     * "DEV:[DIR]NAME.TYP;VER" from the executive search. */
    if (!dcl_rms_dir_next(fsc->dir, result, result_size, NULL, NULL, NULL)) {
        fsearch_slot_clear(fsc);   /* exhausted -- a fresh call restarts */
        result[0] = '\0';
        return 0;
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

    /* vms-481: F$PARSE is a SYNTACTIC VMS operation -- it fills in the device
     * and directory defaults and returns the requested field of the resulting
     * filespec. Build the effective VMS spec (device/dir defaulted from the
     * process default) WITHOUT touching the file system -- no stat() on a /vms
     * passthrough (VSI OpenVMS DCL Dictionary, F$PARSE; clean-room Rule 8). */
    char vspec[1024];
    dcl_rms_effective_spec(ctx, spec, vspec, sizeof(vspec));

    /* Split the VMS spec "DEV:[DIR]NAME.TYP;VER" into components. */
    char dev[64] = "SYS$DISK:", dir[512] = "", nm[256] = "", typ[128] = "";
    {
        const char *cur = vspec;
        const char *lb = strchr(vspec, '[');
        const char *colon = strchr(vspec, ':');
        if (colon && (!lb || colon < lb)) {
            size_t dl = (size_t)(colon - vspec) + 1;
            if (dl < sizeof(dev)) { memcpy(dev, vspec, dl); dev[dl] = '\0'; }
            cur = colon + 1;
        }
        lb = strchr(cur, '[');
        const char *rb = lb ? strchr(lb, ']') : NULL;
        if (lb && rb && rb > lb) {
            size_t dl = (size_t)(rb - lb + 1);
            if (dl >= sizeof(dir)) dl = sizeof(dir) - 1;
            memcpy(dir, lb, dl); dir[dl] = '\0';
            cur = rb + 1;
        }
        /* NAME.TYP;VER */
        char nt[384];
        strncpy(nt, cur, sizeof(nt) - 1); nt[sizeof(nt) - 1] = '\0';
        char *semi = strchr(nt, ';'); if (semi) *semi = '\0';
        char *dot = strrchr(nt, '.');
        if (dot) {
            size_t nl = (size_t)(dot - nt);
            if (nl >= sizeof(nm)) nl = sizeof(nm) - 1;
            memcpy(nm, nt, nl); nm[nl] = '\0';
            strncpy(typ, dot, sizeof(typ) - 1); typ[sizeof(typ) - 1] = '\0';
        } else {
            strncpy(nm, nt, sizeof(nm) - 1); nm[sizeof(nm) - 1] = '\0';
        }
        for (size_t i = 0; nm[i]; i++)  nm[i]  = (char)toupper((unsigned char)nm[i]);
        for (size_t i = 0; typ[i]; i++) typ[i] = (char)toupper((unsigned char)typ[i]);
    }

    /* Check for the 4th (field) argument. */
    int comma_count = 0;
    p = args;
    while (*p) {
        if (*p == ',') { comma_count++; if (comma_count == 3) { p++; break; } }
        p++;
    }

    if (comma_count >= 3) {
        while (*p == ' ') p++;
        char field[64];
        strncpy(field, p, sizeof(field) - 1);
        field[sizeof(field) - 1] = '\0';
        size_t flen = strlen(field);
        while (flen > 0 && (field[flen - 1] == ' ' || field[flen - 1] == '\t'))
            field[--flen] = '\0';
        if (flen >= 2 && field[0] == '"' && field[flen - 1] == '"') {
            field[flen - 1] = '\0';
            memmove(field, field + 1, flen - 1);
        }
        for (size_t i = 0; field[i]; i++)
            field[i] = (char)toupper((unsigned char)field[i]);

        if (strcmp(field, "NAME") == 0)
            strncpy(result, nm, result_size - 1);
        else if (strcmp(field, "TYPE") == 0)
            strncpy(result, typ, result_size - 1);
        else if (strcmp(field, "DIRECTORY") == 0)
            strncpy(result, dir, result_size - 1);
        else if (strcmp(field, "DEVICE") == 0)
            strncpy(result, dev, result_size - 1);
        else if (strcmp(field, "NODE") == 0)
            result[0] = '\0';
        else
            strncpy(result, vspec, result_size - 1);
    } else {
        /* No field specified - return the full expanded filespec. */
        strncpy(result, vspec, result_size - 1);
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

    /* vms-481: read the genuine ODS-2 file header through the ACP (rms_file_attr
     * -> IO$_ACCESS ATR list), not stat() on a /vms passthrough. Fail-honest: a
     * file the ACP cannot access yields F$FILE_ATTRIBUTES's "not found" ("0"). */
    struct rms_fileattr fa;
    if (dcl_rms_attr(ctx, spec, &fa) != RMS$_NORMAL) {
        strncpy(result, "0", result_size - 1);
        result[result_size - 1] = '\0';
        return 0;
    }

    if (strcmp(item, "EOF") == 0) {
        /* End-of-file block: the EOF VBN from the file header FAT. */
        snprintf(result, result_size, "%u", fa.efblk);
    } else if (strcmp(item, "ALQ") == 0) {
        /* Allocation quantity: highest allocated VBN. */
        snprintf(result, result_size, "%u", fa.hiblk);
    } else if (strcmp(item, "MRS") == 0) {
        snprintf(result, result_size, "%u", fa.mrs);
    } else if (strcmp(item, "CDT") == 0 || strcmp(item, "RDT") == 0) {
        /* Creation/revision date from the header's ODS-2 64-bit time. A VMS
         * binary time is 100-ns ticks since 17-NOV-1858; Unix subtracts the
         * 3506716800-second offset (public/documented, clean-room Rule 8). */
        const uint8_t *vt = (strcmp(item, "CDT") == 0) ? fa.credate : fa.revdate;
        uint64_t ticks; memcpy(&ticks, vt, 8);
        if (ticks == 0) { strncpy(result, "", result_size - 1); }
        else {
            long long secs = (long long)(ticks / 10000000ULL) - 3506716800LL;
            long cc = (long)((ticks % 10000000ULL) / 100000ULL);
            if (secs < 0) secs = 0;
            time_t tt = (time_t)secs;
            struct tm tm; localtime_r(&tt, &tm);
            snprintf(result, result_size, "%2d-%s-%04d %02d:%02d:%02d.%02ld",
                     tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, cc);
        }
    } else if (strcmp(item, "KNOWN") == 0) {
        snprintf(result, result_size, "TRUE");
    } else if (strcmp(item, "ORG") == 0) {
        /* Sequential unless the FAT record format is not a record org OVMX
         * distinguishes here (indexed/relative org is not carried in the ATR
         * subset). */
        snprintf(result, result_size, "SEQ");
    } else if (strcmp(item, "RAT") == 0) {
        /* Record attributes from the FAT fat_rattrib bits. */
        char rbuf[16]; size_t ri = 0;
        if (fa.rat & 0x02) rbuf[ri++] = 'C';   /* CR  (ODS2_RAT_CR)  */
        if (fa.rat & 0x01 && ri < sizeof(rbuf) - 1) rbuf[ri++] = 'F'; /* FTN */
        if (fa.rat & 0x04 && ri < sizeof(rbuf) - 1) rbuf[ri++] = 'P'; /* PRN */
        if (fa.rat & 0x08 && ri < sizeof(rbuf) - 1) rbuf[ri++] = 'B'; /* BLK */
        rbuf[ri] = '\0';
        snprintf(result, result_size, "%s", rbuf);
    } else if (strcmp(item, "RFM") == 0) {
        /* Record format from the FAT fat_rtype (FAB$C_* codes). */
        static const char *rfm_name[] = {
            "UDF", "FIX", "VAR", "VFC", "STM", "STMLF", "STMCR"
        };
        const char *nm = (fa.rfm <= 6) ? rfm_name[fa.rfm] : "UDF";
        snprintf(result, result_size, "%s", nm);
    } else if (strcmp(item, "PRO") == 0) {
        /* Protection string from the genuine ODS-2 protection word (a clear bit
         * = access allowed; VMS convention). */
        static const char cat[4] = { 'S', 'O', 'G', 'W' };
        static const int  shift[4] = { 0, 4, 8, 12 };
        char pb[80]; size_t pi = 0;
        pi += (size_t)snprintf(pb + pi, sizeof(pb) - pi, "(");
        for (int c = 0; c < 4; c++) {
            uint16_t nib = (fa.fileprot >> shift[c]) & 0xF;
            pi += (size_t)snprintf(pb + pi, sizeof(pb) - pi, "%s%c:",
                                   c ? "," : "", cat[c]);
            if (!(nib & 0x01)) pi += (size_t)snprintf(pb + pi, sizeof(pb)-pi, "R");
            if (!(nib & 0x02)) pi += (size_t)snprintf(pb + pi, sizeof(pb)-pi, "W");
            if (!(nib & 0x04)) pi += (size_t)snprintf(pb + pi, sizeof(pb)-pi, "E");
            if (!(nib & 0x08)) pi += (size_t)snprintf(pb + pi, sizeof(pb)-pi, "D");
        }
        snprintf(pb + pi, sizeof(pb) - pi, ")");
        snprintf(result, result_size, "%s", pb);
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
    } else if (strcmp(s, "ALLOCLASS") == 0) {
        /* vms-9cf: the allocation class for shared cluster devices, a SYSGEN
         * parameter the operator authors via SYSGEN/SYSMAN. Reads the same
         * OVMXVMSSYS.PAR store SCSSYSTEMID does; 0 is the documented default
         * when unconfigured. This is the DCL reader surface that reflects the
         * authored ALLOCLASS after a WRITE CURRENT + reboot (the store is read
         * fresh on each F$GETSYI, so it is genuine adoption, not a fake). */
        uint32_t alloclass = 0;   /* OVMX/VMS default when unconfigured */
        (void)sysgen_read_param("ALLOCLASS", &alloclass);
        snprintf(result, result_size, "%u", alloclass);
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
 * F$GETJPI(pid, item) - Get process information from the executive.
 *
 * HONORS ITS pid ARGUMENT (vms-9e2). Until this change the pid argument
 * was parsed and DISCARDED: USERNAME came from ctx->username, PRCNAM from
 * ctx->process_name, PID from getpid(). So F$GETJPI("OTHERPROC","PID")
 * confidently returned the CALLER's PID -- a fabricated answer about a
 * different process (CLAUDE.md Rule 11), which also left F$GETJPI
 * disagreeing with SHOW SYSTEM / SHOW PROCESS, both of which already read
 * the executive (vms-8019 / vms-70eb). Two identity surfaces, one process,
 * different answers -- the exact drift the parity program exists to kill.
 *
 * The target is now resolved in the executive -- the SAME source
 * sys$getjpi (src/libvms/syssvc/sys_process.c) and SHOW PROCESS
 * (src/vmsdcl/dcl_cmd_show.c) read -- and every item is answered FROM that
 * row:
 *   - a null pid ("", the DCL Dictionary's documented "current process"
 *     form) -> vms_kif_getjpi_self(): the caller's OWN executive row, not
 *     ctx. This is the same live read SHOW PROCESS uses, so the two agree
 *     by construction rather than by a cached copy that can desynchronise.
 *   - otherwise the pid is a HEXADECIMAL string (the format the DCL
 *     Dictionary documents and F$PID / SHOW SYSTEM print) ->
 *     vms_kif_getjpi_pid(): the executive resolves it and APPLIES the
 *     oracle-measured authorization (docs/oracle/vax73-privileges.md §5):
 *     a same-group read needs no privilege; a cross-group read needs
 *     WORLD; GROUP does NOT lift it; and a refused read comes back
 *     SS$_NOPRIV with NO row (not a redacted subset -- §5.3). A pid the
 *     executive does not carry is SS$_NONEXPR (§5.1).
 *
 * FAILS HONESTLY. When the executive cannot resolve or authorize the
 * target -- or /dev/vms is absent (INV-6: Docker/CI) -- the VMS status is
 * recorded in $STATUS (ctx->last_status) and an EMPTY value is returned.
 * NOTHING is answered from getpid()/ctx: a confident wrong answer about
 * the caller is worse than an honest failure, and reintroducing the ctx
 * fallback reintroduces the facade.
 *
 * F$GETJPI takes only a PID (or null). It does NOT resolve process NAMES
 * -- the classic DCL idiom loops F$PID and reads each PRCNAM; adding name
 * resolution here would invent a capability VMS's F$GETJPI does not have
 * (Rule 10), so a non-hex pid is SS$_NONEXPR rather than a name lookup.
 *
 * Grounding (Rule 8): F$GETJPI format + item semantics from the public
 * VSI OpenVMS DCL Dictionary (F$GETJPI) and the $GETJPI item codes; the
 * authorization/redaction facts from docs/oracle/vax73-privileges.md §5.
 */
static int lex_getjpi(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    result[0] = '\0';
    if (!args) return 0;

    /* Split "<pid>,<item>" at the top-level comma. */
    const char *comma = strchr(args, ',');
    if (!comma) return 0;

    /* --- target (pid) --- trim surrounding whitespace and one quote pair */
    char target[64];
    size_t tl = (size_t)(comma - args);
    if (tl >= sizeof(target)) tl = sizeof(target) - 1;
    memcpy(target, args, tl);
    target[tl] = '\0';
    {
        char *t = target;
        while (*t == ' ' || *t == '\t') t++;
        size_t l = strlen(t);
        while (l > 0 && (t[l - 1] == ' ' || t[l - 1] == '\t')) t[--l] = '\0';
        if (l >= 2 && t[0] == '"' && t[l - 1] == '"') { t[l - 1] = '\0'; t++; l -= 2; }
        memmove(target, t, l + 1);
    }

    /* --- item --- */
    char item[64];
    const char *p = comma + 1;
    while (*p == ' ') p++;
    strncpy(item, p, sizeof(item) - 1);
    item[sizeof(item) - 1] = '\0';
    char *s = item;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = '\0';
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') { s[len - 1] = '\0'; s++; len -= 2; }
    for (size_t i = 0; s[i]; i++) s[i] = (char)toupper((unsigned char)s[i]);

    /*
     * Resolve the executive row for the TARGET, once, up front. Every item
     * below reads this row -- no PCB, no ctx, no getpid(). A failed resolve
     * or authorization is the honest end of the call (INV-6 / §5).
     */
    struct vms_procinfo info;
    memset(&info, 0, sizeof(info));
    uint32_t st;
    if (target[0] == '\0') {
        st = vms_kif_getjpi_self(&info);
    } else {
        char *endp = NULL;
        unsigned long pid = strtoul(target, &endp, 16);
        if (endp == target || *endp != '\0') {
            /* Not a hex PID -- F$GETJPI does not resolve names (see header). */
            if (ctx) ctx->last_status = SS$_NONEXPR;
            return -1;
        }
        st = vms_kif_getjpi_pid((uint32_t)pid, &info);
    }
    if (!(st & 1)) {
        /* Honest failure: $STATUS carries it, the value stays empty. */
        if (ctx) ctx->last_status = st;
        return -1;
    }

    if (strcmp(s, "USERNAME") == 0) {
        strncpy(result, info.username, result_size - 1);
    } else if (strcmp(s, "PRCNAM") == 0) {
        strncpy(result, info.prcnam, result_size - 1);
    } else if (strcmp(s, "PID") == 0) {
        snprintf(result, result_size, "%08X", (unsigned)info.vms_pid);
    } else if (strcmp(s, "MODE") == 0) {
        /*
         * JPI$_MODE (INTERACTIVE/BATCH/NETWORK/OTHER) is not carried in the
         * executive process row; OVMX can only answer it for the caller's
         * own DCL session (F$MODE semantics). For another process it is
         * UNSOURCED -- report nothing rather than the caller's mode, which
         * would be the same cross-process fabrication this item removes.
         */
        if (target[0] == '\0')
            return lex_mode(ctx, NULL, result, result_size);
        result[0] = '\0';
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
         * PRIVILEGES and F$PRIVILEGE (the resolved row above -- the
         * caller's own for a null pid, the named process's otherwise --
         * masked to VMS_PRV_M_ENFORCED). The NAMES themselves are not a second,
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
         * Like every other item in this function since vms-9e2, CURPRIV/
         * AUTHPRIV now HONOR the pid argument: the mask is read from the
         * resolved row above, so F$GETJPI(<other pid>,"CURPRIV") reports
         * that process's enforced privileges (subject to §5 authorization),
         * not the caller's -- it no longer answers only for the caller.
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
        /*
         * The privilege masks come from the SAME resolved row every other
         * item above reads (vms-9e2). CURPRIV/AUTHPRIV of another process
         * therefore honor the pid argument and inherit the executive's §5
         * authorization for free: a cross-group read without WORLD already
         * failed as SS$_NOPRIV before reaching here, so a rendered mask can
         * only ever belong to a process the caller was allowed to read.
         */
        result[0] = '\0';
        {
            uint64_t raw = (strcmp(s, "CURPRIV") == 0) ? info.cur_privs
                                                         : info.perm_privs;
            uint64_t enforced = raw & VMS_PRV_M_ENFORCED;
            size_t rl = 0;
            for (int bit = 0; bit < 64; bit++) {
                uint64_t b = (uint64_t)1 << bit;
                if (!(enforced & b))
                    continue;
                /*
                 * COVERAGE (vms-2b8 round 9; supersedes the runtime
                 * abort() rounds 7-8 put here). Whether every bit
                 * VMS_PRV_M_ENFORCED can set has a row in
                 * vms_priv_names[] is a COMPILE-TIME fact -- both are
                 * static, compile-time-constant data in this same
                 * binary, so the answer cannot vary across runs or
                 * callers the way a genuine runtime condition could.
                 * Rounds 7-8 guarded it with a runtime abort() anyway,
                 * which is Rule 10's forbidden third answer: a
                 * plausible-looking handler for a condition that is
                 * already settled before the program runs. The
                 * corrected HIDE answer for a compile-time fact is a
                 * compile-time proof, not a runtime check --
                 * src/libvms/prv_agreement.c now static-asserts this
                 * coverage, with its own negative control. A future
                 * edit that adds an unnamed bit to VMS_PRV_M_ENFORCED
                 * fails the BUILD there, before anything boots, so the
                 * lookup below needs no fallback: every bit reaching
                 * this loop is guaranteed to have a row.
                 */
                for (int i = 0; vms_priv_names[i].name; i++) {
                    if (vms_priv_names[i].bit != b)
                        continue;
                    /*
                     * CodeQL cpp/unclear-buffer-write (round 13):
                     * snprintf returns the length it WOULD have
                     * written, not what fit, so accumulating it into
                     * `rl` unguarded lets `rl` exceed `result_size` on
                     * truncation -- the next iteration then computes
                     * `result_size - rl` as a size_t underflow and
                     * writes far past `result`. Every caller of
                     * dcl_eval_lexical() today passes a DCL_MAX_VALUE
                     * (4096-byte) buffer, and this table's full
                     * comma-joined render is 267 bytes for all 37 rows
                     * (measured: tests/libvms/test_priv_render_bounds.c)
                     * -- this branch is not reachable through any call
                     * site in this tree. But dcl_eval_lexical() is an `extern`
                     * function whose contract is the result_size
                     * parameter, not "callers happen to pass 4096", so
                     * the accumulation must bound-check what it
                     * actually wrote. On a would-be truncation, stop
                     * appending further names rather than trust a
                     * length it never measured.
                     */
                    if (rl >= result_size)
                        break;
                    int n = snprintf(result + rl, result_size - rl,
                                     "%s%s", rl ? "," : "",
                                     vms_priv_names[i].name);
                    if (n < 0 || (size_t)n >= result_size - rl) {
                        rl = result_size > 0 ? result_size - 1 : 0;
                        bit = 64; /* stop the outer bit scan too */
                        break;
                    }
                    rl += (size_t)n;
                    break;
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
        /*
         * THE RIGHTS DATABASE ANSWERS THIS, NOT THIS FUNCTION (vms-2f8).
         *
         * What stood here was a hardcoded pair: SYSTEM -> 65540 and
         * DEFAULT -> 8388736, everything else the miss. Both VALUES were
         * right -- an earlier round pinned them to the oracle -- and the
         * function was still fabricating in the sense CLAUDE.md Rule 11
         * means: a user-visible VMS command producing an answer itself
         * instead of reading the facility that owns it. The visible cost was
         * that the six environmental identifiers VMS has (BATCH, DIALUP,
         * INTERACTIVE, LOCAL, NETWORK, REMOTE) and OVMX's own non-SYSTEM
         * accounts had no identifier at all, while SYS$SYSTEM:RIGHTSLIST.DAT
         * sat provisioned on the system disk with no reader.
         *
         * It reads it now. src/libvms/rtl/rightslist.c resolves general
         * identifiers from RIGHTSLIST.DAT and UIC identifiers from SYSUAF,
         * which is where the oracle shows both kinds coming from; every
         * value is measured in docs/oracle/vax73-rights-database.md.
         *
         * THE MISS IS UNCHANGED AND STILL PINNED: an identifier that is not
         * valid converts to a ZERO in this direction. Measured --
         * F$IDENTIFIER("NOSUCHIDENT","NAME_TO_NUMBER") -> 0 -- and the
         * public HP/VSI DCL Dictionary says the same. A rights database
         * that cannot be opened at all takes this same path, deliberately:
         * a missing facility answers the miss, it does not resurrect a
         * built-in table (Rule 9 -- fail honestly, never fake).
         *
         * NO HOST PASSWD LOOKUP (vms-f39, Rule 10). This once read:
         *
         *     struct passwd *pw = getpwnam(id_str);
         *     if (pw) result = (pw->pw_gid << 16) | (pw->pw_uid & 0xFFFF);
         *
         * so F$IDENTIFIER("baron","NAME_TO_NUMBER") answered with the
         * developer's Linux account dressed as a VMS UIC. Deleted, not
         * replaced -- and note that what replaces the whole branch now is a
         * VMS facility rather than another host one.
         */
        uint32_t ident_value;
        if (rightslist_name_to_value(id_upper, &ident_value) == 0) {
            snprintf(result, result_size, "%d", (int)ident_value);
        } else {
            /* The miss, pinned: an identifier that is not valid converts to
             * a ZERO in this direction. See the block above -- and note this
             * is also the line the dcl-fident-name2num-host-passwd negative
             * control restores the deleted getpwnam() defect onto, so its
             * text and indentation are load-bearing. */
            snprintf(result, result_size, "0");
        }
    } else if (strcmp(conv, "NUMBER_TO_NAME") == 0) {
        /* Convert UIC number to username */
        long uic = strtol(id_str, NULL, 0);
        int member = (int)(uic & 0xFFFF);
        int group = (int)((uic >> 16) & 0xFFFF);

        /*
         * 'group' and 'member' ARE DELIBERATELY KEPT THOUGH THIS FUNCTION NO
         * LONGER BRANCHES ON THEM. Two negative controls in
         * tests/qemu/facility_defects.sh restore the deleted defects onto the
         * miss line below -- dcl-fident-num2name-host-passwd needs 'member'
         * for its getpwuid() and dcl-fident-num2name-bracketed-uic needs both
         * for its "[%d,%d]". Deleting them here would leave two mutations
         * that do not compile, and a mutation that does not compile is a
         * broken fixture rather than a gate that bites (see this file's
         * header note on <pwd.h>, which keeps that include for the same
         * reason).
         */
        (void)group;
        (void)member;

        /*
         * THE RIGHTS DATABASE ANSWERS THIS TOO (vms-2f8).
         *
         * What stood here was a single hardcoded case, [1,4] -> "SYSTEM",
         * with everything else falling to the miss. That round declined to
         * add DEFAULT's reverse mapping because the oracle had been asked
         * that pair only in the NAME_TO_NUMBER direction and symmetry is not
         * evidence -- the right call on the evidence it had. IT HAS NOW BEEN
         * ASKED (docs/oracle/vax73-rights-database.md §2):
         *
         *     F$IDENTIFIER(8388736,"NUMBER_TO_NAME")  ->  "DEFAULT"
         *
         * and every identifier the oracle holds round-trips. So the mapping
         * is not added on symmetry; it falls out of reading the database,
         * which is where VMS reads it from.
         *
         * THE MISS VALUE IS THE NULL STRING, AND IT IS PINNED. Measured
         * against OpenVMS VAX V7.3, every input shape tried:
         *
         *     F$IDENTIFIER(1000,"NUMBER_TO_NAME")        ->  ""
         *     F$IDENTIFIER(0,"NUMBER_TO_NAME")           ->  ""
         *     F$IDENTIFIER(77777,"NUMBER_TO_NAME")       ->  ""
         *     F$IDENTIFIER(196609,"NUMBER_TO_NAME")      ->  ""
         *     F$IDENTIFIER(%X80010004,"NUMBER_TO_NAME")  ->  ""
         *     F$IDENTIFIER(1..5,"NUMBER_TO_NAME")        ->  ""
         *
         * That last row is the one this change turns on. 1..5 were the values
         * OVMX's own shipped RIGHTSLIST.DAT assigned to INTERACTIVE, BATCH,
         * NETWORK, LOCAL and REMOTE; on real VMS not one of them is an
         * identifier. Wiring this function to the file as it stood would have
         * shipped five wrong answers while looking like it had started
         * reading a real facility, so the file was corrected in the same
         * commit that made anything read it.
         *
         * Real VMS emits no bracketed UIC from F$IDENTIFIER for any input, so
         * the "[%d,%d]" rendering that once stood on the miss line was a
         * plausible-looking answer to a condition VMS never gives that answer
         * to -- CLAUDE.md Rule 10's illegal third answer.
         */
        char ident_name[RIGHTSLIST_NAME_MAX];
        if (rightslist_value_to_name((uint32_t)uic, ident_name,
                                     sizeof(ident_name)) == 0) {
            snprintf(result, result_size, "%s", ident_name);
        } else {
            /* The miss. Also the line both num2name negative controls
             * restore their defect onto -- text and indentation are
             * load-bearing. */
            result[0] = '\0';
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

/* ----------------------------------------------------------------------
 * Small shared arg helpers for the completeness lexicals below.
 * ---------------------------------------------------------------------- */

/* Copy args[from..to-comma] into out, trimming blanks and one quote pair.
 * Returns a pointer past the comma consumed (or NULL at end of string). */
static const char *lex_next_arg(const char *p, char *out, size_t outsz)
{
    out[0] = '\0';
    if (!p) return NULL;
    while (*p == ' ' || *p == '\t') p++;
    const char *comma = strchr(p, ',');
    size_t n = comma ? (size_t)(comma - p) : strlen(p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    /* right-trim */
    size_t l = strlen(out);
    while (l > 0 && (out[l - 1] == ' ' || out[l - 1] == '\t')) out[--l] = '\0';
    /* unquote one pair */
    if (l >= 2 && out[0] == '"' && out[l - 1] == '"') {
        out[l - 1] = '\0';
        memmove(out, out + 1, l - 1);
    }
    return comma ? comma + 1 : NULL;
}

static void lex_upcase(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

/*
 * F$DELTA_TIME(start-time, end-time [,format]) - elapsed time between two
 * absolute times, as a delta-time string.
 *
 * GROUNDING (Rule 8): the public VSI OpenVMS DCL Dictionary "F$DELTA_TIME"
 * entry (docs.vmssoftware.com) and the VSI OpenVMS Wiki F$DELTA_TIME page.
 * Both arguments are absolute time strings; the end time must be the same as
 * or later than the start time; the result is a delta-time string of the form
 * "DDD HH:MM:SS.CC" (the "DCL" format variant uses a hyphen: "DDD-HH:MM:SS.CC").
 * This is a modern (Alpha/I64) lexical -- the lab-2 VAX V7.3 oracle answers
 * %DCL-W-IVFNAM for it (11-AUG-2026) -- so it is implemented for OVMX's
 * platform target, purely computationally, like F$LICENSE. No plumbing is
 * needed beyond the two strings, so there is no honest-error path other than
 * a malformed time (SS$_IVTIME) or end < start.
 */
static int lex_delta_time(struct dcl_context *ctx, const char *args,
                          char *result, size_t result_size)
{
    result[0] = '\0';
    char a_start[64], a_end[64], a_fmt[32];
    const char *p = lex_next_arg(args, a_start, sizeof(a_start));
    p = lex_next_arg(p, a_end, sizeof(a_end));
    (void)lex_next_arg(p, a_fmt, sizeof(a_fmt));

    if (a_start[0] == '\0' || a_end[0] == '\0') {
        /* Both times are required (%DCL-W-ARGREQ on the VMS oracle). */
        dcl_error("DCL", 0, "ARGREQ",
                  "missing argument - supply all required arguments");
        return -1;
    }

    struct tm tm_s, tm_e;
    int cs_s = 0, cs_e = 0;
    if (!parse_vms_time(a_start, &tm_s, &cs_s) ||
        !parse_vms_time(a_end, &tm_e, &cs_e)) {
        if (ctx) ctx->last_status = SS$_IVTIME;
        return -1;
    }
    time_t es = mktime(&tm_s);
    time_t ee = mktime(&tm_e);
    if (es == (time_t)-1 || ee == (time_t)-1) {
        if (ctx) ctx->last_status = SS$_IVTIME;
        return -1;
    }

    /* Total elapsed in centiseconds; end must be >= start. */
    long long total_cs = ((long long)ee - (long long)es) * 100 +
                         (long long)(cs_e - cs_s);
    if (total_cs < 0) {
        /* end earlier than start -- the documented constraint is violated. */
        if (ctx) ctx->last_status = SS$_IVTIME;
        return -1;
    }

    long long cc   = total_cs % 100;               total_cs /= 100;   /* -> s  */
    long long ss   = total_cs % 60;                total_cs /= 60;    /* -> min */
    long long mm   = total_cs % 60;                total_cs /= 60;    /* -> hr  */
    long long hh   = total_cs % 24;                total_cs /= 24;    /* -> day */
    long long days = total_cs;

    lex_upcase(a_fmt);
    char sep = (strcmp(a_fmt, "DCL") == 0) ? '-' : ' ';
    snprintf(result, result_size, "%lld%c%02lld:%02lld:%02lld.%02lld",
             days, sep, hh, mm, ss, cc);
    return 0;
}

/*
 * F$CUNITS(number [,from-units [,to-units]]) - convert a storage quantity
 * between BLOCKS/BYTES/KB/MB/GB/TB.
 *
 * GROUNDING (Rule 8): the public VSI OpenVMS DCL Dictionary "F$CUNITS" entry
 * and the VSI OpenVMS Wiki F$CUNITS page. A disk block is 512 bytes; KB/MB/
 * GB/TB are binary (1024-based). from-units defaults to BLOCKS; the single
 * documented no-to-units example -- F$CUNITS(1024) -> "512KB" (1024 blocks =
 * 524288 bytes = 512 KB) -- fixes the omitted-to-units default at KB, which
 * is the reading this implements. The result is the truncated integer count
 * followed by the destination unit label (e.g. "512KB", "1BLOCKS", "0GB").
 * BYTES is a destination unit only, and only from BLOCKS. A modern lexical
 * (VAX V7.3 oracle answers %DCL-W-IVFNAM); purely computational.
 */
static int lex_cunits(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    result[0] = '\0';
    char a_num[64], a_from[16], a_to[16];
    const char *p = lex_next_arg(args, a_num, sizeof(a_num));
    p = lex_next_arg(p, a_from, sizeof(a_from));
    (void)lex_next_arg(p, a_to, sizeof(a_to));

    if (a_num[0] == '\0') {
        dcl_error("DCL", 0, "ARGREQ",
                  "missing argument - supply all required arguments");
        return -1;
    }
    if (a_from[0] == '\0') strcpy(a_from, "BLOCKS");
    if (a_to[0]   == '\0') strcpy(a_to,   "KB");
    lex_upcase(a_from);
    lex_upcase(a_to);

    /* Multiplier to bytes for each keyword. A negative marker means the
     * keyword is not legal in that position. */
    struct { const char *name; long long mult; int from_ok; int to_ok; }
    units[] = {
        { "B",      1LL,                        1, 1 },
        { "BYTES",  1LL,                        0, 1 },  /* dest only */
        { "BLOCKS", 512LL,                      1, 1 },
        { "KB",     1024LL,                     1, 1 },
        { "MB",     1024LL*1024,                1, 1 },
        { "GB",     1024LL*1024*1024,           1, 1 },
        { "TB",     1024LL*1024*1024*1024,      1, 1 },
        { NULL, 0, 0, 0 }
    };
    long long from_mult = -1, to_mult = -1;
    int from_ok = 0, to_ok = 0, to_is_bytes = 0;
    for (int i = 0; units[i].name; i++) {
        if (strcmp(a_from, units[i].name) == 0) { from_mult = units[i].mult; from_ok = units[i].from_ok; }
        if (strcmp(a_to,   units[i].name) == 0) { to_mult = units[i].mult; to_ok = units[i].to_ok;
                                                  to_is_bytes = (strcmp(units[i].name, "BYTES") == 0); }
    }
    /* Illegal combinations: unknown keyword, a from-only used as to, or
     * BYTES as a destination from anything but BLOCKS. */
    if (from_mult < 0 || to_mult < 0 || !from_ok || !to_ok ||
        (to_is_bytes && strcmp(a_from, "BLOCKS") != 0)) {
        if (ctx) ctx->last_status = SS$_BADPARAM;
        return -1;
    }

    char *endp = NULL;
    /* strtol (not strtoll): long is 64-bit on OVMX's x86_64/aarch64/axp
     * targets, so it covers the same range, and it is already a DECC$SHR
     * universal the DCL.EXE native link resolves -- strtoll is not exported
     * and would break the LINK.EXE graph (vms-61f). */
    long long number = (long long)strtol(a_num, &endp, 10);
    if (endp == a_num) { if (ctx) ctx->last_status = SS$_BADPARAM; return -1; }

    long long bytes = number * from_mult;
    long long out   = bytes / to_mult;   /* integer truncation, per the doc */
    snprintf(result, result_size, "%lld%s", out, a_to);
    return 0;
}

/*
 * F$SETPRV(priv-states) - enable or disable process privileges, returning the
 * PRIOR state of each named privilege.
 *
 * GROUNDING (Rule 8): the public VSI OpenVMS DCL Dictionary "F$SETPRV" entry.
 * priv-states is a comma-separated list of privilege keywords, each optionally
 * prefixed NO to disable. The return value is a comma-separated list, in the
 * SAME order, giving each named privilege's state BEFORE the call: the keyword
 * if it was enabled, NOkeyword if it was disabled. Confirmed against the lab-2
 * VAX V7.3 oracle (11-AUG-2026): F$SETPRV("NOOPER,GROUP") -> "OPER,GROUP" for
 * a process holding both.
 *
 * INV-6 / EXECUTIVE: the privilege mutation is the executive's, not a
 * userspace fake. The prior mask is read with vms_kif_getjpi_self() and the
 * change is applied with vms_kif_setprv() -- the SAME kernel-interface client
 * edge sys$setprv itself uses (VMS_IOCTL_SETPRV -> vms_ioctl_setprv, which
 * authorizes the grant against this process's AUTHORIZED mask). Calling
 * vms_kif_setprv directly (rather than the sys$setprv wrapper) keeps F$SETPRV
 * on the executive edge DCL.EXE's native link already resolves for F$GETJPI/
 * F$DEVICE, adding no new cross-shareable-image import. With no /dev/vms the
 * getjpi read fails and F$SETPRV returns the honest VMS error via $STATUS --
 * never a fabricated privilege string.
 */
static int lex_setprv(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    result[0] = '\0';
    char list[512];
    (void)lex_next_arg(args, list, sizeof(list));
    if (list[0] == '\0') {
        dcl_error("DCL", 0, "ARGREQ",
                  "missing argument - supply all required arguments");
        return -1;
    }

    /* Read the PRIOR privilege mask from the executive (INV-6). */
    struct vms_procinfo info;
    memset(&info, 0, sizeof(info));
    uint32_t st = vms_kif_getjpi_self(&info);
    if (!(st & 1)) {
        if (ctx) ctx->last_status = st;   /* honest VMS error, no fake string */
        return -1;
    }
    uint64_t prior = info.cur_privs;

    uint64_t enable_mask = 0, disable_mask = 0;
    size_t rl = 0;
    result[0] = '\0';

    /* Walk the comma-separated tokens in order. */
    char *save = NULL;
    for (char *tok = strtok_r(list, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t tl = strlen(tok);
        while (tl > 0 && (tok[tl - 1] == ' ' || tok[tl - 1] == '\t')) tok[--tl] = '\0';
        if (tl == 0) continue;
        lex_upcase(tok);

        int disable = 0;
        const char *pname = tok;
        /* NO-prefix means disable, but only if the remainder is a real priv. */
        if (tl > 2 && strncmp(tok, "NO", 2) == 0) {
            const char *rest = tok + 2;
            for (int i = 0; vms_priv_names[i].name; i++)
                if (strcmp(rest, vms_priv_names[i].name) == 0) { disable = 1; pname = rest; break; }
        }

        uint64_t mask = 0;
        for (int i = 0; vms_priv_names[i].name; i++)
            if (strcmp(pname, vms_priv_names[i].name) == 0) { mask = vms_priv_names[i].bit; break; }
        if (mask == 0) continue;   /* unknown keyword: VMS ignores in the list */

        if (disable) disable_mask |= mask; else enable_mask |= mask;

        /* Prior state of this privilege, in the argument's order. */
        int was_on = (prior & mask) != 0;
        int n = snprintf(result + rl, result_size - rl, "%s%s%s",
                         rl ? "," : "", was_on ? "" : "NO", pname);
        if (n > 0 && (size_t)n < result_size - rl) rl += (size_t)n;
    }

    /* Apply the change through the executive (vms_kif_setprv: mask, enable,
     * permanent, prev). Authorization failures (SS$_NOTALLPRIV for an
     * unauthorized enable) are NOT fatal to F$SETPRV -- VMS still returns the
     * prior-state string; the privilege simply does not take. */
    if (enable_mask)  vms_kif_setprv(enable_mask, 1, 0, NULL);
    if (disable_mask) vms_kif_setprv(disable_mask, 0, 0, NULL);
    return 0;
}

/*
 * F$CSID(context-symbol) - return the cluster system id (CSID) of each
 * VMScluster member in turn, updating the context symbol.
 *
 * GROUNDING (Rule 8): the public VSI OpenVMS DCL Dictionary "F$CSID" entry.
 * The argument is required (the lab-2 VAX V7.3 oracle answers %DCL-W-ARGREQ
 * for F$CSID() with none). On a cluster the function walks members, returning
 * each CSID as an 8-hex-digit string and "" when the list is exhausted.
 *
 * OVMX SCOPE (honest, not fabricated): the SCS membership table lives in the
 * cluster daemon (src/vmsscs), which the DCL layer does not read -- and there
 * is no other DCL-reachable cluster-id interface. From the DCL layer OVMX
 * therefore presents as a NON-clustered node, whose defined F$CSID answer is
 * an empty list: the first call returns "". This is the true non-cluster state,
 * NOT an invented CSID. Reading real member CSIDs (the clustered case) needs
 * an executive/SCS membership query that does not exist here yet -- tracked as
 * a follow-up (see PR).
 */
static int lex_csid(struct dcl_context *ctx, const char *args,
                    char *result, size_t result_size)
{
    (void)ctx;
    (void)result_size;
    result[0] = '\0';
    char ctxsym[64];
    (void)lex_next_arg(args, ctxsym, sizeof(ctxsym));
    if (ctxsym[0] == '\0') {
        dcl_error("DCL", 0, "ARGREQ",
                  "missing argument - supply all required arguments");
        return -1;
    }
    /* Non-clustered node: no members visible -> empty list (end-of-scan). */
    result[0] = '\0';
    return 0;
}

/*
 * F$MULTIPATH(device-name, item, context-symbol) - return an item of multipath
 * information for a multipath-capable device.
 *
 * GROUNDING (Rule 8): the public VSI OpenVMS DCL Dictionary "F$MULTIPATH"
 * entry and the VSI OpenVMS Wiki. Valid on Alpha/Integrity only; item
 * MP_PATHNAME returns a path name string, the context symbol is initialized to
 * 0 before the first call, and the end of the path list is signaled by the
 * return of a BLANK path name. A modern lexical (VAX V7.3 oracle answers
 * %DCL-W-IVFNAM).
 *
 * OVMX SCOPE (honest): OVMX has no multipath-capable devices, so every device
 * has an empty path list. The authentic VMS response for a device with no
 * further paths is a blank return, which is exactly what OVMX returns here --
 * the true "no multipath" state, not a fabricated path.
 */
static int lex_multipath(struct dcl_context *ctx, const char *args,
                         char *result, size_t result_size)
{
    (void)ctx;
    (void)result_size;
    result[0] = '\0';
    char dev[64], item[32], ctxsym[64];
    const char *p = lex_next_arg(args, dev, sizeof(dev));
    p = lex_next_arg(p, item, sizeof(item));
    (void)lex_next_arg(p, ctxsym, sizeof(ctxsym));
    if (dev[0] == '\0' || item[0] == '\0') {
        dcl_error("DCL", 0, "ARGREQ",
                  "missing argument - supply all required arguments");
        return -1;
    }
    /* No multipath devices on OVMX -> blank path name (end of list). */
    result[0] = '\0';
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
    { "F$LICENSE",          lex_license },
    { "F$SETPRV",           lex_setprv },
    { "F$CSID",             lex_csid },
    { "F$DELTA_TIME",       lex_delta_time },
    { "F$MULTIPATH",        lex_multipath },
    { "F$CUNITS",           lex_cunits },
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

    /*
     * Unknown lexical function -- the authentic VMS diagnostic, not a silent
     * empty string (INV-DCL: never fake success/emptiness). Grounded to the
     * lab-2 VAX V7.3 oracle (vaxlab-1, 11-AUG-2026): typing an undefined
     * lexical answers, verbatim,
     *
     *   %DCL-W-IVFNAM, invalid lexical function name - check validity and spelling
     *    \F$BOGUS(\
     *
     * i.e. a two-line message whose continuation echoes the offending token
     * (the function name up to and including the opening paren) between
     * backslashes. dcl_error emits line 1 (%DCL-W-IVFNAM); the fprintf below
     * reproduces the continuation line. The value stays empty and the call
     * fails.
     */
    dcl_error("DCL", 0, "IVFNAM",
              "invalid lexical function name - check validity and spelling");
    fprintf(stderr, " \\%s(\\\n", func_name);
    result[0] = '\0';
    return -1;
}
