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
#include "devdef.h"
#include "descrip.h"
#include "lnmdef.h"
#include "starlet.h"   /* sys$faol / sys$getmsg (F$FAO, F$MESSAGE) */
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

/* ------------------------------------------------------------------------
 * EVALUATED LEXICAL ARGUMENTS (rd vms-8d1, docs/oracle/semantics/lex/).
 * dcl_eval_lexical() hands every function except F$TYPE its arguments
 * already evaluated: a string as a quoted literal (quotes doubled), an
 * integer as decimal text, an omitted argument empty. lex_args() reads that
 * form back.
 * ---------------------------------------------------------------------- */
#define LEX_MAXARG 16
struct lex_arg {
    int  present;
    int  is_string;
    long ival;
    char sval[1024];
};

static int lex_args(const char *args, struct lex_arg *a, int max)
{
    int n = 0;
    const char *p = args ? args : "";
    for (int i = 0; i < max; i++) memset(&a[i], 0, sizeof a[i]);
    if (!*p) return 0;
    for (;;) {
        struct lex_arg tmp; memset(&tmp, 0, sizeof tmp);
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '"') {
            size_t o = 0;
            p++;
            while (*p) {
                if (*p == '"') {
                    if (p[1] == '"') { if (o + 1 < sizeof tmp.sval) tmp.sval[o++] = '"'; p += 2; continue; }
                    p++; break;
                }
                if (o + 1 < sizeof tmp.sval) tmp.sval[o++] = *p;
                p++;
            }
            tmp.sval[o] = '\0';
            tmp.present = 1; tmp.is_string = 1;
            while (*p && *p != ',') p++;
        } else {
            const char *st = p;
            while (*p && *p != ',') p++;
            while (st < p && (*st == ' ' || *st == '\t')) st++;
            if (p > st) { tmp.present = 1; tmp.ival = strtol(st, NULL, 10); }
        }
        if (n < max) a[n] = tmp;
        n++;
        if (*p != ',') break;
        p++;
    }
    return n;
}

/* An argument as a string: an integer becomes its decimal text. */
static const char *lex_str(struct lex_arg *a, char *buf, size_t bsz)
{
    if (!a->present) return "";
    if (a->is_string) return a->sval;
    snprintf(buf, bsz, "%ld", a->ival);
    return buf;
}

/* An argument as an integer: a string that IS an integer gives its value,
 * one beginning T/t/Y/y gives 1 (true), anything else 0 (DCL Dictionary,
 * "Converting String to Integer"; observed LEX.INTEGER.TRUE/YES/JUNK). */
static long lex_int(struct lex_arg *a)
{
    if (!a->present) return 0;
    if (!a->is_string) return a->ival;
    extern long dcl_parse_int(const char *s, int *ok);
    const char *t = a->sval;
    while (*t == ' ' || *t == '\t') t++;
    int ok = 0;
    long v = dcl_parse_int(t, &ok);
    if (ok) return v;
    return (*t == 'T' || *t == 't' || *t == 'Y' || *t == 'y') ? 1 : 0;
}

/* Too many arguments for the function: CLI-W-SYMDEL (observed LEX.TOOMANY). */
static int lex_toomany(struct dcl_context *ctx, int n, int max)
{
    if (n <= max) return 0;
    if (ctx) ctx->last_status = 0x00038130;
    return 1;
}

static int lex_fail(struct dcl_context *ctx, uint32_t code, char *result)
{
    if (ctx) ctx->last_status = code;
    if (result) result[0] = '\0';
    return -1;
}

static void lex_put(char *result, size_t rs, const char *src, size_t n)
{
    if (n >= rs) n = rs - 1;
    memcpy(result, src, n);
    result[n] = '\0';
}

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
    struct lex_arg a[LEX_MAXARG];
    int n = lex_args(args, a, LEX_MAXARG);
    if (lex_toomany(ctx, n, 1)) return lex_fail(ctx, 0x00038130, result);
    char b[32];
    snprintf(result, result_size, "%zu", strlen(lex_str(&a[0], b, sizeof b)));
    return 0;
}

/*
 * F$EXTRACT(start, length, string) - Extract substring.
 */
static int lex_extract(struct dcl_context *ctx, const char *args,
                       char *result, size_t result_size)
{
    /* F$EXTRACT(start, length, string): a negative start gives the null
     * string; a negative length runs to the end (observed LEX.EXTRACT.*). */
    struct lex_arg a[LEX_MAXARG];
    int n = lex_args(args, a, LEX_MAXARG);
    if (lex_toomany(ctx, n, 3)) return lex_fail(ctx, 0x00038130, result);
    long start = lex_int(&a[0]), len = lex_int(&a[1]);
    char b[32];
    const char *str = lex_str(&a[2], b, sizeof b);
    long slen = (long)strlen(str);
    result[0] = '\0';
    if (start < 0 || start >= slen) return 0;
    if (len < 0 || start + len > slen) len = slen - start;
    lex_put(result, result_size, str + start, (size_t)len);
    return 0;
}

/*
 * F$ELEMENT(number, delimiter, string) - Extract element from delimited string.
 */
static int lex_element(struct dcl_context *ctx, const char *args,
                       char *result, size_t result_size)
{
    /* F$ELEMENT(n, delimiter, string): element n (0-based); past the last
     * element, the delimiter itself; a delimiter that is not exactly one
     * character is an error (observed LEX.ELEMENT.*). */
    struct lex_arg a[LEX_MAXARG];
    int n = lex_args(args, a, LEX_MAXARG);
    if (lex_toomany(ctx, n, 3)) return lex_fail(ctx, 0x00038130, result);
    long el = lex_int(&a[0]);
    char b1[32], b2[32];
    const char *d = lex_str(&a[1], b1, sizeof b1), *str = lex_str(&a[2], b2, sizeof b2);
    if (strlen(d) != 1) return lex_fail(ctx, 0x000388FA, result);
    const char *p = str;
    for (long i = 0; i < el && p; i++) {
        p = strchr(p, d[0]);
        if (p) p++;
    }
    if (!p || el < 0) { lex_put(result, result_size, d, 1); return 0; }
    const char *e = strchr(p, d[0]);
    lex_put(result, result_size, p, e ? (size_t)(e - p) : strlen(p));
    return 0;
}

/*
 * F$LOCATE(substring, string) - Find position of substring.
 */
static int lex_locate(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    /* F$LOCATE(substring, string): the offset of the first match, or the
     * string's length when there is none (an empty substring is found at 0). */
    struct lex_arg a[LEX_MAXARG];
    int n = lex_args(args, a, LEX_MAXARG);
    if (lex_toomany(ctx, n, 2)) return lex_fail(ctx, 0x00038130, result);
    char b1[32], b2[32];
    const char *sub = lex_str(&a[0], b1, sizeof b1), *str = lex_str(&a[1], b2, sizeof b2);
    const char *f = strstr(str, sub);
    snprintf(result, result_size, "%ld", f ? (long)(f - str) : (long)strlen(str));
    return 0;
}

/*
 * F$EDIT(string, edit_list) - Edit a string.
 * Supported edits: UPCASE, LOWERCASE, TRIM, COMPRESS, COLLAPSE, UNCOMMENT
 */
static int lex_edit(struct dcl_context *ctx, const char *args,
                    char *result, size_t result_size)
{
    /* F$EDIT(string, edit-list). The edits apply in a fixed order whatever
     * order they are listed in -- UNCOMMENT (from an unquoted "!" on, the
     * blank before it kept), COLLAPSE, COMPRESS, TRIM, then UPCASE /
     * LOWERCASE -- and text inside quotation marks is left alone; an unknown
     * keyword is CLI-W-IVKEYW (observed LEX.EDIT.*). */
    struct lex_arg a[LEX_MAXARG];
    int n = lex_args(args, a, LEX_MAXARG);
    if (lex_toomany(ctx, n, 2)) return lex_fail(ctx, 0x00038130, result);
    char b1[32], b2[32], kw[1024];
    const char *in = lex_str(&a[0], b1, sizeof b1);
    snprintf(kw, sizeof kw, "%s", lex_str(&a[1], b2, sizeof b2));
    int uncomment = 0, collapse = 0, compress = 0, trim = 0, up = 0, low = 0;
    for (char *t = strtok(kw, ","); t; t = strtok(NULL, ",")) {
        while (*t == ' ') t++;
        size_t tl = strlen(t);
        while (tl && t[tl - 1] == ' ') t[--tl] = '\0';
        for (char *q = t; *q; q++) *q = (char)toupper((unsigned char)*q);
        if (!strcmp(t, "UNCOMMENT")) uncomment = 1;
        else if (!strcmp(t, "COLLAPSE")) collapse = 1;
        else if (!strcmp(t, "COMPRESS")) compress = 1;
        else if (!strcmp(t, "TRIM")) trim = 1;
        else if (!strcmp(t, "UPCASE")) up = 1;
        else if (!strcmp(t, "LOWERCASE")) low = 1;
        else return lex_fail(ctx, 0x00038060, result);
    }
    char w[4096];
    snprintf(w, sizeof w, "%s", in);
    if (uncomment) {
        int q = 0;
        for (char *c = w; *c; c++) {
            if (*c == '"') q = !q;
            else if (*c == '!' && !q) { *c = '\0'; break; }
        }
    }
    if (collapse || compress) {
        char o[4096]; size_t k = 0; int q = 0, prevblank = 0;
        for (char *c = w; *c; c++) {
            int blank = (*c == ' ' || *c == '\t');
            if (*c == '"') q = !q;
            if (!q && blank) {
                if (collapse) continue;
                if (prevblank) continue;
                o[k++] = ' '; prevblank = 1; continue;
            }
            prevblank = 0;
            o[k++] = *c;
        }
        o[k] = '\0';
        memcpy(w, o, k + 1);
    }
    if (trim) {
        char *c = w;
        while (*c == ' ' || *c == '\t') c++;
        memmove(w, c, strlen(c) + 1);
        size_t l = strlen(w);
        while (l && (w[l - 1] == ' ' || w[l - 1] == '\t')) w[--l] = '\0';
    }
    if (up || low) {
        int q = 0;
        for (char *c = w; *c; c++) {
            if (*c == '"') { q = !q; continue; }
            if (!q) *c = (char)(up ? toupper((unsigned char)*c) : tolower((unsigned char)*c));
        }
    }
    lex_put(result, result_size, w, strlen(w));
    return 0;
}

/*
 * F$INTEGER(string) - Convert string to integer.
 */
static int lex_integer(struct dcl_context *ctx, const char *args,
                       char *result, size_t result_size)
{
    /* F$INTEGER(expression): an integer stays; a string converts by the DCL
     * rule -- an integer string (decimal, or %X/%O/%D radix: $STATUS is
     * "%Xhhhhhhhh") gives its value, one starting T or Y gives 1, any other
     * string 0 (observed LEX.INTEGER.*). */
    struct lex_arg a[LEX_MAXARG];
    int n = lex_args(args, a, LEX_MAXARG);
    if (lex_toomany(ctx, n, 1)) return lex_fail(ctx, 0x00038130, result);
    snprintf(result, result_size, "%ld", lex_int(&a[0]));
    return 0;
}

/*
 * F$STRING(integer) - Convert integer to string.
 */
static int lex_string(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    /* F$STRING(expression): an integer as decimal text; a string as it is
     * (observed LEX.STRING / LEX.STRING.STR). */
    struct lex_arg a[LEX_MAXARG];
    int n = lex_args(args, a, LEX_MAXARG);
    if (lex_toomany(ctx, n, 1)) return lex_fail(ctx, 0x00038130, result);
    char b[32];
    const char *v = lex_str(&a[0], b, sizeof b);
    lex_put(result, result_size, v, strlen(v));
    return 0;
}

/*
 * F$TRNLNM(logname [, table]) - Translate logical name.
 */
static int lex_trnlnm(struct dcl_context *ctx, const char *args,
                       char *result, size_t result_size)
{
    /* F$TRNLNM(name [, table] [, index] [, mode] [, case] [, item]):
     * $TRNLNM itself -- LNM$DCL_LOGICAL by default, the item asked for
     * (VALUE by default; TABLE is "TRUE" when the name is itself a logical
     * name table, ...), the null string when there is no translation
     * (observed LEX.TRNLNM.*). */
    struct lex_arg a[LEX_MAXARG];
    int n = lex_args(args, a, LEX_MAXARG);
    if (lex_toomany(ctx, n, 6)) return lex_fail(ctx, 0x00038130, result);
    char b0[32], b1[32], b3[32], b4[32], b5[32];
    char name[256], table[256], mode[32], cs[32], item[32];
    snprintf(name, sizeof name, "%s", lex_str(&a[0], b0, sizeof b0));
    snprintf(table, sizeof table, "%s", a[1].present ? lex_str(&a[1], b1, sizeof b1) : "LNM$DCL_LOGICAL");
    snprintf(mode, sizeof mode, "%s", lex_str(&a[3], b3, sizeof b3));
    snprintf(cs, sizeof cs, "%s", lex_str(&a[4], b4, sizeof b4));
    snprintf(item, sizeof item, "%s", a[5].present ? lex_str(&a[5], b5, sizeof b5) : "VALUE");
    for (char *q = mode; *q; q++) *q = (char)toupper((unsigned char)*q);
    for (char *q = cs; *q; q++) *q = (char)toupper((unsigned char)*q);
    for (char *q = item; *q; q++) *q = (char)toupper((unsigned char)*q);
    if (!table[0]) snprintf(table, sizeof table, "LNM$DCL_LOGICAL");
    if (!item[0]) snprintf(item, sizeof item, "VALUE");
    result[0] = '\0';

    uint8_t acmode = 3, *acp = NULL;
    if (mode[0]) {
        if (!strcmp(mode, "USER")) acmode = 3;
        else if (!strcmp(mode, "SUPERVISOR")) acmode = 2;
        else if (!strcmp(mode, "EXECUTIVE")) acmode = 1;
        else if (!strcmp(mode, "KERNEL")) acmode = 0;
        else return lex_fail(ctx, 0x00038060, result);
        acp = &acmode;
    }
    uint32_t attr = LNM$M_CASE_BLIND;
    if (!strcmp(cs, "CASE_SENSITIVE")) attr = 0;
    else if (cs[0] && strcmp(cs, "CASE_BLIND")) return lex_fail(ctx, 0x00038060, result);

    uint32_t index = a[2].present ? (uint32_t)lex_int(&a[2]) : 0;
    char eq[256] = "", tab[64] = "";
    uint16_t eql = 0, tabl = 0;
    uint32_t eattr = 0, emax = 0, elen = 0;
    uint8_t eacm = 0;
    struct item_list_3 it[8];
    memset(it, 0, sizeof it);
    it[0].buflen = 4; it[0].item_code = LNM$_INDEX; it[0].bufaddr = &index;
    it[1].buflen = (uint16_t)(sizeof eq - 1); it[1].item_code = LNM$_STRING; it[1].bufaddr = eq; it[1].retlen = &eql;
    it[2].buflen = 4; it[2].item_code = LNM$_ATTRIBUTES; it[2].bufaddr = &eattr;
    it[3].buflen = 4; it[3].item_code = LNM$_MAX_INDEX; it[3].bufaddr = &emax;
    it[4].buflen = 4; it[4].item_code = LNM$_LENGTH; it[4].bufaddr = &elen;
    it[5].buflen = 1; it[5].item_code = LNM$_ACMODE; it[5].bufaddr = &eacm;
    it[6].buflen = (uint16_t)(sizeof tab - 1); it[6].item_code = LNM$_TABLE; it[6].bufaddr = tab; it[6].retlen = &tabl;
    struct dsc$descriptor_s nd = { (uint16_t)strlen(name), DSC$K_DTYPE_T, DSC$K_CLASS_S, name };
    struct dsc$descriptor_s td = { (uint16_t)strlen(table), DSC$K_DTYPE_T, DSC$K_CLASS_S, table };
    uint32_t st = sys$trnlnm(&attr, &td, &nd, acp, it);
    if (st == SS$_NOSUCHDEV) {
        /* no executive (the host DCL test build): DCL's own logical-name
         * layer answers the plain translation */
        if (!strcmp(item, "VALUE"))
            dcl_translate_logical(name, result, result_size);
        return 0;
    }
    if (!(st & 1))
        return 0;                                  /* no translation: "" */
    eq[eql < sizeof eq ? eql : sizeof eq - 1] = '\0';
    /* A process-permanent file's equivalence carries an ESC NUL IFI header a
     * DCL string cannot hold; F$TRNLNM gives its device ("_OPA0:", rd
     * vms-b14e). */
    if (vms_lnm_is_ppf(eq, eql)) {
        memmove(eq, eq + VMS_LNM_PPF_HDR, (size_t)eql - VMS_LNM_PPF_HDR);
        eq[eql - VMS_LNM_PPF_HDR] = '\0';
    }
    tab[tabl < sizeof tab ? tabl : sizeof tab - 1] = '\0';
    static const char *const modes[4] = { "KERNEL", "EXECUTIVE", "SUPERVISOR", "USER" };
#define TF(c) snprintf(result, result_size, "%s", (c) ? "TRUE" : "FALSE")
    if (!strcmp(item, "VALUE")) lex_put(result, result_size, eq, strlen(eq));
    else if (!strcmp(item, "TABLE")) TF(eattr & LNM$M_TABLE);
    else if (!strcmp(item, "TABLE_NAME")) lex_put(result, result_size, tab, strlen(tab));
    else if (!strcmp(item, "LENGTH")) snprintf(result, result_size, "%u", (unsigned)strlen(eq));
    else if (!strcmp(item, "MAX_INDEX")) snprintf(result, result_size, "%u", emax);
    else if (!strcmp(item, "ACCESS_MODE")) snprintf(result, result_size, "%s", modes[eacm & 3]);
    else if (!strcmp(item, "CONCEALED")) TF(eattr & LNM$M_CONCEALED);
    else if (!strcmp(item, "TERMINAL")) TF(eattr & LNM$M_TERMINAL);
    else if (!strcmp(item, "CONFINE")) TF(eattr & LNM$M_CONFINE);
    else if (!strcmp(item, "NO_ALIAS")) TF(eattr & LNM$M_NO_ALIAS);
    else if (!strcmp(item, "CRELOG")) TF(eattr & LNM$M_CRELOG);
    else return lex_fail(ctx, 0x00038060, result);
#undef TF
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
 * F$USER() - the process UIC in named form ("[SYSTEM]"; semantic oracle
 * LEX.USER on Alpha V8.4 and VAX V7.3, vms-86f).
 *
 * The UIC is read from this process's executive row and rendered by $FAO !%I,
 * the rights-database lookup any caller gets: a UIC no identifier holds comes
 * back in octal numeric form ("[322,13]"). Nothing here chooses a name. The
 * history this replaces (vms-cb5, vms-f39): F$USER once answered the literal
 * SYSTEM, or the HOST Linux account name upcased, for a row the executive had
 * not named -- both fabrications; an executive that cannot answer gives the
 * empty string, never a value picked here.
 */
static int lex_user(struct dcl_context *ctx, const char *args,
                    char *result, size_t result_size)
{
    (void)ctx;
    (void)args;
    result[0] = '\0';
    struct vms_procinfo info;
    memset(&info, 0, sizeof(info));
    if (!(vms_kif_getjpi_self(&info) & 1) || info.uic == 0)
        return 0;
    uint32_t uic = info.uic;
    char ubuf[128];
    uint16_t ulen = 0;
    struct dsc$descriptor_s ctr = { 3, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"!%I" };
    struct dsc$descriptor_s out = { sizeof(ubuf) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, ubuf };
    if (!(sys$fao(&ctr, &ulen, &out, (uint64_t)uic) & 1))
        return 0;
    if (ulen >= result_size) ulen = (uint16_t)(result_size - 1);
    memcpy(result, ubuf, ulen);
    result[ulen] = '\0';
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
    /* A spec with no wildcard keeps no search context: every call answers
     * the file again (observed on OpenVMS Alpha V8.4, F$SEARCH of
     * SYS$SYSTEM:LOGINOUT.EXE three times running, the same file each time:
     * docs/oracle/alpha84-fsearch-nonwild.txt). */
    if (!dcl_rms_dir_wild(fsc->dir))
        fsearch_slot_clear(fsc);
    return 0;
}

/*
 * F$PARSE(filespec [, default [, related [, field]]]) - Parse a filespec.
 */
static int lex_parse(struct dcl_context *ctx, const char *args,
                     char *result, size_t result_size)
{
    /* F$PARSE(filespec [, default-spec] [, related-spec] [, field]
     *         [, parse-type]): RMS $PARSE itself -- the default spec fills
     * what the filespec leaves out, the process default supplies device and
     * directory -- and the field asked for out of the NAM block; a spec
     * $PARSE refuses gives the null string (observed LEX.PARSE.*). */
    struct lex_arg a[LEX_MAXARG];
    int n = lex_args(args, a, LEX_MAXARG);
    if (lex_toomany(ctx, n, 5)) return lex_fail(ctx, 0x00038130, result);
    char b0[32], b1[32], b3[32], b4[32], fld[32], ptype[32];
    char spec[256], dflt[256];
    snprintf(spec, sizeof spec, "%s", lex_str(&a[0], b0, sizeof b0));
    snprintf(dflt, sizeof dflt, "%s", lex_str(&a[1], b1, sizeof b1));
    snprintf(fld, sizeof fld, "%s", lex_str(&a[3], b3, sizeof b3));
    snprintf(ptype, sizeof ptype, "%s", lex_str(&a[4], b4, sizeof b4));
    for (char *q = fld; *q; q++) *q = (char)toupper((unsigned char)*q);
    for (char *q = ptype; *q; q++) *q = (char)toupper((unsigned char)*q);
    result[0] = '\0';

    struct FAB fab = cc$rms_fab;
    struct NAM nam = cc$rms_nam;
    char esa[256];
    fab.fab$l_fna = spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$l_dna = dflt;
    fab.fab$b_dns = (uint8_t)strlen(dflt);
    fab.fab$l_nam = &nam;
    nam.nam$l_esa = esa;
    nam.nam$b_ess = (uint8_t)(sizeof esa - 1);
    if (!strcmp(ptype, "SYNTAX_ONLY")) nam.nam$b_nop |= NAM$M_SYNCHK;
    else if (!strcmp(ptype, "NO_CONCEAL")) nam.nam$b_nop |= NAM$M_NOCONCEAL;
    else if (ptype[0]) return lex_fail(ctx, 0x00038060, result);
    if (!(sys$parse(&fab, 0, 0) & 1))
        return 0;                                  /* null string, not an error */
    rms_search_end(&nam);

    const char *ptr = esa; size_t len = nam.nam$b_esl;
    if (!fld[0]) { /* the whole expanded string */ }
    else if (!strcmp(fld, "NODE"))      { ptr = nam.nam$l_node; len = nam.nam$b_node; }
    else if (!strcmp(fld, "DEVICE"))    { ptr = nam.nam$l_dev;  len = nam.nam$b_dev; }
    else if (!strcmp(fld, "DIRECTORY")) { ptr = nam.nam$l_dir;  len = nam.nam$b_dir; }
    else if (!strcmp(fld, "NAME"))      { ptr = nam.nam$l_name; len = nam.nam$b_name; }
    else if (!strcmp(fld, "TYPE"))      { ptr = nam.nam$l_type; len = nam.nam$b_type; }
    else if (!strcmp(fld, "VERSION"))   { ptr = nam.nam$l_ver;  len = nam.nam$b_ver; }
    else return lex_fail(ctx, 0x00038060, result);
    if (!ptr) len = 0;
    lex_put(result, result_size, ptr ? ptr : "", len);
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
        /* Maximum record size: the header's FAT$W_MAXREC, which $CREATE
         * records from the creator's fab$w_mrs (vms-b447). */
        snprintf(result, result_size, "%u", fa.mrs);
    } else if (strcmp(item, "LRL") == 0) {
        /* Longest record length: the header's FAT$W_RSIZE. */
        snprintf(result, result_size, "%u", fa.lrl);
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
        /* File organization: the high nibble of the header's FAT$B_RTYPE,
         * which $CREATE records from fab$b_org (vms-b447). */
        snprintf(result, result_size, "%s",
                 fa.org == FAB$C_IDX ? "IDX" : fa.org == FAB$C_REL ? "REL" : "SEQ");
    } else if (strcmp(item, "RAT") == 0) {
        /* Record attributes from the FAT fat_rattrib bits, by the names VMS
         * returns: a VAX V7.3 answers "PRN" for a print-carriage-control file
         * and "" for none (tests/lab/captures/decnet-live-brackets-20261008/
         * vax73-dirfull-recfmt.txt, rd vms-a44); CR and FTN by their FAB
         * names alike. A BLK (no-span) bit has no captured spelling and is
         * not rendered (INV-6: no invented output). */
        snprintf(result, result_size, "%s",
                 (fa.rat & 0x01) ? "FTN" : (fa.rat & 0x02) ? "CR"
                 : (fa.rat & 0x04) ? "PRN" : "");
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
        /*
         * Bounded accumulator (vms-5f0 / CodeQL): snprintf() returns the length
         * it WOULD have written, so a raw `pi += snprintf(pb+pi, sizeof(pb)-pi,
         * ...)` could drive pi past sizeof(pb); the next `sizeof(pb)-pi` would
         * then underflow (unsigned) to a huge size and hand snprintf an out-of-
         * bounds pointer + length. Guard every append with `pi < sizeof(pb)` so
         * the subtraction is provably positive, and clamp pi to at most
         * sizeof(pb)-1 on truncation. (The real content is <30 bytes, so this
         * never truncates in practice -- it removes the theoretical overflow.)
         */
        #define PRO_APPEND(...) do {                                        \
            if (pi < sizeof(pb)) {                                          \
                size_t _rem = sizeof(pb) - pi;                              \
                int _n = snprintf(pb + pi, _rem, __VA_ARGS__);             \
                pi += (_n > 0)                                             \
                        ? (((size_t)_n < _rem) ? (size_t)_n : (_rem - 1))  \
                        : 0;                                               \
            }                                                              \
        } while (0)
        PRO_APPEND("(");
        for (int c = 0; c < 4; c++) {
            uint16_t nib = (fa.fileprot >> shift[c]) & 0xF;
            PRO_APPEND("%s%c:", c ? "," : "", cat[c]);
            if (!(nib & 0x01)) PRO_APPEND("R");
            if (!(nib & 0x02)) PRO_APPEND("W");
            if (!(nib & 0x04)) PRO_APPEND("E");
            if (!(nib & 0x08)) PRO_APPEND("D");
        }
        PRO_APPEND(")");
        #undef PRO_APPEND
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
    /* F$TYPE names a symbol: a quoted literal or a number is not one
     * (observed LEX.TYPE.NUMSTR -> ARGREQ, LEX.TYPE.INT -> IVSYMB). */
    if (s[0] == '"') { if (ctx) ctx->last_status = 0x00038268; return -1; }
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_' || s[0] == '$')) {
        if (ctx) ctx->last_status = 0x00038080;
        return -1;
    }
    const char *val = dcl_sym_get(s);
    if (!val) {
        result[0] = '\0'; /* Undefined */
    } else {
        /* INTEGER for an integer symbol or a string that IS a decimal integer
         * ("-12"); STRING otherwise -- including "" and text that merely
         * starts with blanks (observed LEX.STRING / LEX.ELEMENT.2 /
         * LEX.EDIT.UPCASE). */
        const char *q = val;
        if (*q == '-' || *q == '+') q++;
        int digits = 0;
        while (isdigit((unsigned char)*q)) { q++; digits++; }
        strncpy(result, (digits && *q == '\0') ? "INTEGER" : "STRING", result_size - 1);
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
static const char *const cvt_mon[12] = { "JAN","FEB","MAR","APR","MAY","JUN",
                                         "JUL","AUG","SEP","OCT","NOV","DEC" };

/* "d-hh:mm:ss.cc" / "hh:mm:ss.cc" (any trailing part omitted) -> centiseconds. */
static int cvt_parse_delta(const char *t, int64_t *cs)
{
    long d = 0, h = 0, m = 0, sec = 0, c = 0;
    const char *p = t;
    char *e;
    const char *dash = strchr(p, '-');
    if (dash) {
        d = strtol(p, &e, 10);
        if (e != dash) return 0;
        p = dash + 1;
    }
    if (*p) {
        h = strtol(p, &e, 10); if (e == p && *p != ':') return 0; p = e;
        if (*p == ':') { p++; m = strtol(p, &e, 10); p = e; }
        if (*p == ':') { p++; sec = strtol(p, &e, 10); p = e; }
        if (*p == '.') { p++; c = strtol(p, &e, 10); if (e - p == 1) c *= 10; p = e; }
        if (*p) return 0;
    }
    if (h > 23 || m > 59 || sec > 59 || c > 99 || d < 0) return 0;
    *cs = ((((int64_t)d * 24 + h) * 60 + m) * 60 + sec) * 100 + c;
    return 1;
}

/* An absolute time "dd-mmm-yyyy[ |:]hh:mm:ss.cc", any part omitted taking
 * today's / midnight's value, or TODAY / TOMORROW / YESTERDAY, optionally
 * followed by "+delta" or "-delta" (a combination time) -> centiseconds since
 * 1970 on the wall clock. 0 when it is not a valid time (CLI-W-IVATIME). */
static int cvt_parse_abs(const char *in, int64_t *out)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char buf[128];
    snprintf(buf, sizeof buf, "%s", in);
    for (char *q = buf; *q; q++) *q = (char)toupper((unsigned char)*q);
    char *t = buf;
    while (*t == ' ') t++;
    size_t tl = strlen(t);
    while (tl && t[tl - 1] == ' ') t[--tl] = '\0';

    /* split off a combination delta: the first '+', or a '-' after the time */
    int64_t delta = 0; int dsign = 0;
    char *plus = strchr(t, '+');
    if (plus) { *plus = '\0'; if (!cvt_parse_delta(plus + 1, &delta)) return 0; dsign = 1; }

    int day = lt.tm_mday, mon = lt.tm_mon, year = lt.tm_year + 1900;
    int hh = lt.tm_hour, mm = lt.tm_min, ss = lt.tm_sec, cc = (int)(ts.tv_nsec / 10000000);
    int time_given = 0;
    if (!strcmp(t, "") ) {
        /* the current time */
    } else if (!strcmp(t, "TODAY") || !strcmp(t, "TOMORROW") || !strcmp(t, "YESTERDAY")) {
        hh = mm = ss = cc = 0;
        if (t[0] == 'T' && t[2] == 'M') delta += 8640000, dsign = dsign ? dsign : 1;
        if (t[0] == 'Y') { delta = -8640000 + (dsign ? delta : 0); dsign = 1; }
    } else {
        char *sep = NULL;
        char *dpart = t, *tpart = NULL;
        /* a date part has a '-' before any ':' */
        char *dash = strchr(t, '-'), *colon = strchr(t, ':');
        if (dash && (!colon || dash < colon)) {
            sep = strpbrk(t, " ");
            if (!sep) {                       /* dd-mmm-yyyy:hh:mm... */
                char *c2 = colon;
                if (c2) { *c2 = '\0'; tpart = c2 + 1; }
            } else { *sep = '\0'; tpart = sep + 1; }
            /* dd-mmm-yyyy, each part optional */
            char *f1 = dpart, *f2 = strchr(f1, '-'), *f3 = NULL;
            if (f2) { *f2++ = '\0'; f3 = strchr(f2, '-'); if (f3) *f3++ = '\0'; }
            if (*f1) { char *e; day = (int)strtol(f1, &e, 10); if (*e) return 0; }
            if (f2 && *f2) {
                int found = -1;
                for (int i = 0; i < 12; i++) if (!strcmp(f2, cvt_mon[i])) found = i;
                if (found < 0) return 0;
                mon = found;
            }
            if (f3 && *f3) { char *e; year = (int)strtol(f3, &e, 10); if (*e) return 0; }
        } else {
            tpart = t;
        }
        if (tpart) {
            while (*tpart == ' ') tpart++;
            if (*tpart) {
                time_given = 1;
                int64_t c;
                if (!cvt_parse_delta(tpart, &c)) return 0;
                hh = (int)(c / 360000); mm = (int)(c / 6000 % 60);
                ss = (int)(c / 100 % 60); cc = (int)(c % 100);
            }
        }
        if (!time_given && dash) { hh = mm = ss = cc = 0; }
        static const int mdays[12] = {31,29,31,30,31,30,31,31,30,31,30,31};
        if (year < 1858 || year > 9999 || day < 1 || day > mdays[mon]) return 0;
        if (mon == 1 && day == 29 && !((year % 4 == 0 && year % 100) || year % 400 == 0)) return 0;
    }
    struct tm tm = {0};
    tm.tm_year = year - 1900; tm.tm_mon = mon; tm.tm_mday = day;
    tm.tm_hour = hh; tm.tm_min = mm; tm.tm_sec = ss;
    int64_t v = (int64_t)timegm(&tm) * 100 + cc;
    if (dsign) v += delta;
    *out = v;
    return 1;
}

static int lex_cvtime(struct dcl_context *ctx, const char *args,
                      char *result, size_t result_size)
{
    /* F$CVTIME([input-time] [, output-format] [, field]): output format
     * COMPARISON by default ("2026-10-07 13:45:56.78"), or ABSOLUTE
     * ("7-OCT-2026 13:45:56.78"); a combination input "time+delta" adds the
     * delta; a time that is not a time is CLI-W-IVATIME (observed
     * LEX.CVTIME.*). */
    struct lex_arg a[LEX_MAXARG];
    int n = lex_args(args, a, LEX_MAXARG);
    if (lex_toomany(ctx, n, 3)) return lex_fail(ctx, 0x00038130, result);
    char b0[32], b1[32], b2[32], fmt[32], fld[32];
    const char *in = lex_str(&a[0], b0, sizeof b0);
    snprintf(fmt, sizeof fmt, "%s", a[1].present ? lex_str(&a[1], b1, sizeof b1) : "COMPARISON");
    snprintf(fld, sizeof fld, "%s", a[2].present ? lex_str(&a[2], b2, sizeof b2) : "DATETIME");
    for (char *q = fmt; *q; q++) *q = (char)toupper((unsigned char)*q);
    for (char *q = fld; *q; q++) *q = (char)toupper((unsigned char)*q);
    if (!fmt[0]) snprintf(fmt, sizeof fmt, "COMPARISON");
    if (!fld[0]) snprintf(fld, sizeof fld, "DATETIME");
    int abs = !strcmp(fmt, "ABSOLUTE");
    if (!abs && strcmp(fmt, "COMPARISON")) return lex_fail(ctx, 0x00038060, result);

    int64_t v;
    if (!cvt_parse_abs(in, &v)) return lex_fail(ctx, 0x00038290, result);
    time_t secs = (time_t)(v / 100);
    int cc = (int)(v % 100);
    struct tm tm;
    gmtime_r(&secs, &tm);
    static const char *const wd[7] = { "Sunday","Monday","Tuesday","Wednesday",
                                       "Thursday","Friday","Saturday" };
    char date[32], tod[32];
    if (abs) snprintf(date, sizeof date, "%d-%s-%04d", tm.tm_mday, cvt_mon[tm.tm_mon], tm.tm_year + 1900);
    else     snprintf(date, sizeof date, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    snprintf(tod, sizeof tod, "%02d:%02d:%02d.%02d", tm.tm_hour, tm.tm_min, tm.tm_sec, cc);

    if (!strcmp(fld, "DATETIME")) snprintf(result, result_size, "%s %s", date, tod);
    else if (!strcmp(fld, "DATE")) snprintf(result, result_size, "%s", date);
    else if (!strcmp(fld, "TIME")) snprintf(result, result_size, "%s", tod);
    else if (!strcmp(fld, "YEAR")) snprintf(result, result_size, "%04d", tm.tm_year + 1900);
    else if (!strcmp(fld, "MONTH")) {
        if (abs) snprintf(result, result_size, "%s", cvt_mon[tm.tm_mon]);
        else snprintf(result, result_size, "%02d", tm.tm_mon + 1);
    }
    else if (!strcmp(fld, "DAY")) snprintf(result, result_size, abs ? "%d" : "%02d", tm.tm_mday);
    else if (!strcmp(fld, "HOUR")) snprintf(result, result_size, "%02d", tm.tm_hour);
    else if (!strcmp(fld, "MINUTE")) snprintf(result, result_size, "%02d", tm.tm_min);
    else if (!strcmp(fld, "SECOND")) snprintf(result, result_size, "%02d", tm.tm_sec);
    else if (!strcmp(fld, "HUNDREDTH")) snprintf(result, result_size, "%02d", cc);
    else if (!strcmp(fld, "WEEKDAY")) snprintf(result, result_size, "%s", wd[tm.tm_wday]);
    else if (!strcmp(fld, "DAYOFYEAR")) snprintf(result, result_size, "%d", tm.tm_yday + 1);
    else if (!strcmp(fld, "HOUROFYEAR")) snprintf(result, result_size, "%d", tm.tm_yday * 24 + tm.tm_hour);
    else if (!strcmp(fld, "MINUTEOFYEAR")) snprintf(result, result_size, "%d", (tm.tm_yday * 24 + tm.tm_hour) * 60 + tm.tm_min);
    else if (!strcmp(fld, "SECONDOFYEAR")) snprintf(result, result_size, "%d", ((tm.tm_yday * 24 + tm.tm_hour) * 60 + tm.tm_min) * 60 + tm.tm_sec);
    else return lex_fail(ctx, 0x00038060, result);
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
         * identity SSOT (INV-1). Never a hardcoded constant here. vms-28a: real
         * VMS F$GETSYI("VERSION") is the fixed 8-char SPACE-PADDED field
         * ("V7.3    "), byte-confirmed on the live oracle -- emit the padded
         * field, not the trimmed token. */
        char field[OVMX_VMS_VERSION_FIELD_LEN + 1];
        ovmx_compat_version_field(field);
        field[OVMX_VMS_VERSION_FIELD_LEN] = '\0';
        strncpy(result, field, result_size - 1);
    } else if (strcmp(s, "ARCH_NAME") == 0) {
        /* Machine surface: the VMS-style architecture name (SYI$_ARCH_NAME),
         * from the identity SSOT (INV-1) -- the SAME ovmx_hw_arch() the $GETSYI
         * service (sys_misc.c SYI$_ARCH_NAME) returns, so the two surfaces never
         * disagree. Was UNHANDLED here, so F$GETSYI("ARCH_NAME") fell through to
         * "0"; real VMS returns the arch token ("VAX"/"Alpha"/"IA64"/"x86_64").
         * NOT space-padded (byte-confirmed on the live oracle: "Alpha" exact, no
         * padding -- unlike VERSION's fixed 8-char field). rd vms-76c3. */
        strncpy(result, ovmx_hw_arch(), result_size - 1);
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
        /* the 12-character, blank-filled SYSUAF name (observed
         * LEX.GETJPI.USERNAME "SYSTEM      "; rd vms-bd30) -- an unnamed
         * row stays the null string */
        if (info.username[0])
            snprintf(result, result_size, "%-12.12s", info.username);
        else
            result[0] = '\0';
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
        /* an item F$GETJPI does not know: CLI-W-IVKEYW, no value
         * (observed LEX.GETJPI.BADITEM) */
        result[0] = '\0';
        if (ctx) ctx->last_status = 0x00038060;
        return -1;
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
    /* F$MESSAGE(code [, component...]): the message $GETMSG holds for the
     * code -- facility, severity, identification and text by default, or the
     * components named ("FACILITY", "SEVERITY", "IDENT", "TEXT"), FAO
     * directives left in place (observed LEX.MESSAGE.*: the OpenVMS message
     * catalog OVMX's $GETMSG carries, docs/oracle/messages/). */
    struct lex_arg a[LEX_MAXARG];
    int n = lex_args(args, a, LEX_MAXARG);
    if (lex_toomany(ctx, n, 5)) return lex_fail(ctx, 0x00038130, result);
    uint32_t code = (uint32_t)lex_int(&a[0]);
    uint32_t flags = 0;
    for (int i = 1; i < n; i++) {
        char b[32], kw[32];
        snprintf(kw, sizeof kw, "%s", lex_str(&a[i], b, sizeof b));
        for (char *q = kw; *q; q++) *q = (char)toupper((unsigned char)*q);
        if (!strcmp(kw, "TEXT")) flags |= 1;
        else if (!strcmp(kw, "IDENT")) flags |= 2;
        else if (!strcmp(kw, "SEVERITY")) flags |= 4;
        else if (!strcmp(kw, "FACILITY")) flags |= 8;
        else if (kw[0]) return lex_fail(ctx, 0x00038060, result);
    }
    if (!flags) flags = 0xF;
    char out[512];
    struct dsc$descriptor_s od = { (uint16_t)(sizeof out - 1), DSC$K_DTYPE_T, DSC$K_CLASS_S, out };
    uint16_t ol = 0;
    uint32_t st = sys$getmsg(code, &ol, &od, flags, NULL);
    if (!(st & 1) && st != SS$_MSGNOTFND)
        return lex_fail(ctx, st, result);
    lex_put(result, result_size, out, ol);
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
    /* F$FAO(control, arg...): the $FAO directive engine itself ($FAOL), the
     * arguments being integers by value and strings as descriptors -- what
     * DCL hands $FAO (observed LEX.FAO.*: !AS widths, !%S plurals, !n*c
     * repeats, !n(...) repeat groups, !%U). Up to 15 arguments. */
    struct lex_arg a[LEX_MAXARG];
    int n = lex_args(args, a, LEX_MAXARG);
    if (lex_toomany(ctx, n, 16)) return lex_fail(ctx, 0x00038130, result);
    char b0[32];
    const char *ctl = lex_str(&a[0], b0, sizeof b0);
    static struct dsc$descriptor_s sd[LEX_MAXARG];
    uint64_t prm[LEX_MAXARG + 1];
    int k = 0;
    for (int i = 1; i < n && i < LEX_MAXARG; i++) {
        if (a[i].is_string) {
            sd[i].dsc$w_length = (uint16_t)strlen(a[i].sval);
            sd[i].dsc$b_dtype = DSC$K_DTYPE_T;
            sd[i].dsc$b_class = DSC$K_CLASS_S;
            sd[i].dsc$a_pointer = a[i].sval;
            prm[k++] = (uint64_t)(uintptr_t)&sd[i];
        } else {
            prm[k++] = (uint64_t)(uint32_t)a[i].ival;
        }
    }
    prm[k] = 0;
    struct dsc$descriptor_s cd = { (uint16_t)strlen(ctl), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)ctl };
    char out[1024];
    struct dsc$descriptor_s od = { (uint16_t)(sizeof out - 1), DSC$K_DTYPE_T, DSC$K_CLASS_S, out };
    uint16_t ol = 0;
    uint32_t st = sys$faol(&cd, &ol, &od, prm);
    if (!(st & 1) && st != SS$_BUFFEROVF)
        return lex_fail(ctx, st, result);
    lex_put(result, result_size, out, ol);
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

    /* every name in the list must be a privilege (optionally NO-prefixed):
     * F$PRIVILEGE("NOSUCHPRIV") is CLI-W-IVKEYW, not TRUE (observed
     * LEX.PRIVILEGE.BAD) */
    {
        char tl[256];
        snprintf(tl, sizeof tl, "%s", s);
        for (char *t = strtok(tl, ","); t; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            if (!strncmp(t, "NO", 2) && parse_privilege_string(t + 2) != 0) continue;
            if (parse_privilege_string(t) == 0) {
                result[0] = '\0';
                if (ctx) ctx->last_status = 0x00038060;
                return -1;
            }
        }
    }
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
/*
 * WHERE F$PID's PIDS COME FROM (vms-050, INV-6 / Rule 9).
 *
 * F$PID used to snapshot Linux /proc: opendir("/proc"), take every numeric
 * entry as a "PID", and print it as %08X -- so it returned the Linux kernel's
 * pids dressed as VMS process IDs, and on an opendir() failure it fell back to
 * getpid(). Both are fabrication: a VMS pid is EXECUTIVE-allocated (the
 * process table row's vms_pid, distinct per process since #883/vms-d4ef), not
 * the substrate's task pid, and F$PID is the DCL Dictionary's window onto the
 * VMS process list -- successive calls return successive VMS pids, "" when the
 * list is exhausted.
 *
 * It now snapshots the SAME executive process table SHOW SYSTEM and SHOW USERS
 * read: vms_kif_procscan() (src/kernel-core/vms_proctab.c), walked in full,
 * storing each row's vms_pid in cursor order. Every pid F$PID returns is
 * therefore a pid SHOW SYSTEM would also list -- one executive source, not two.
 * Redacted rows are kept: SHOW SYSTEM renders the pid on every row procscan
 * returns (redaction hides identity fields, not existence), so F$PID enumerates
 * the same set.
 *
 * NO EXECUTIVE, NO FABRICATION. Under ctest (and anywhere /dev/vms is absent --
 * the only OVMX runtime is the kernel/QEMU path, Rule 9) the first procscan
 * cannot reach the executive: its status has bit 0 clear and is NOT SS$_NONEXPR
 * (a present-but-empty table). pid_count stays 0, the walk yields "", and
 * nothing is invented -- no /proc read, no getpid() fallback. pid_scan_status
 * carries that first status so lex_pid can surface an honest $STATUS.
 */
#define MAX_PID_LIST 256
static uint32_t pid_list[MAX_PID_LIST];
static int pid_count = 0;
static int pid_index = 0;
static uint32_t pid_scan_status = SS$_NORMAL;

static void populate_pid_list(void)
{
    pid_count = 0;
    pid_index = 0;

    uint32_t scan_index = 0;
    struct vms_procinfo info;
    memset(&info, 0, sizeof(info));

    /* First contact with the executive. A readable table (SS$_NORMAL rows, or
     * SS$_NONEXPR once exhausted) is the honest source; anything else is the
     * executive being unreachable, and leaves the list empty. */
    uint32_t st = vms_kif_procscan(&scan_index, &info);
    pid_scan_status = st;

    while ((st & 1) && pid_count < MAX_PID_LIST) {
        pid_list[pid_count++] = info.vms_pid;
        st = vms_kif_procscan(&scan_index, &info);
    }
}

static int lex_pid(struct dcl_context *ctx, const char *args,
                   char *result, size_t result_size)
{
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
        /* End of the list: the documented "" terminator. $STATUS distinguishes
         * a genuine end-of-walk (SS$_NORMAL) from a list that was empty because
         * the executive was unreachable -- the honest executive-absent signal,
         * never masked as a normal empty walk (INV-6 / Rule 9). */
        result[0] = '\0';
        if (sym_name[0] != '\0')
            dcl_sym_set(sym_name, "", DCL_SYM_LOCAL);
        if (ctx) {
            if (pid_count == 0 && !(pid_scan_status & 1) &&
                pid_scan_status != SS$_NONEXPR)
                ctx->last_status = pid_scan_status;
            else
                ctx->last_status = SS$_NORMAL;
        }
        return 0;
    }

    snprintf(result, result_size, "%08X", (unsigned)pid_list[pid_index++]);
    if (ctx) ctx->last_status = SS$_NORMAL;

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
 * DVI$_DEVCHAR bits F$GETDVI reports (vms-050): the V7.3 DEV$M_ values from
 * devdef.h (rd vms-f811), the SAME ones src/libvms/syssvc/sys_device.c's $GETDVI
 * reader uses, so F$GETDVI and $GETDVI report one DVI$_DEVCHAR longword for the
 * same device, derived from real executive state, never fabricated.
 */
#define GETDVI_DEVCHAR_ALL   DEV$M_ALL  /* device is allocated */
#define GETDVI_DEVCHAR_AVL   DEV$M_AVL  /* device is available */
#define GETDVI_DEVCHAR_MNT   DEV$M_MNT  /* a volume is mounted on it */

/*
 * Resolve a device name to its EXECUTIVE device-table row (vms-050).
 *
 * The lookup is vms_kif_getdvi_devnam() -- the SAME executive reader F$DEVICE
 * and SHOW DEVICE use (src/vmsdcl/dcl_cmd_show.c) -- so the row is the one
 * every process on the node sees, never a per-process guess (Rule 11 / INV-6).
 * The executive table holds only physical unit names (OPA0:, DKA0:, ...), so a
 * logical name -- SYS$SYSDEVICE:, the standard VMS idiom for the system disk --
 * legitimately misses the first lookup; it is then translated through the DCL
 * logical-name translator (the same one lex_trnlnm / SHOW LOGICAL use) and the
 * physical lookup retried with the DEVICE field of the equivalence. A name that
 * resolves to neither a unit nor a logical comes back SS$_NOSUCHDEV -- the
 * honest "no such device", never a fabricated row.
 *
 * This mirrors src/libvms/syssvc/sys_device.c's device_lookup_translated(),
 * which $GETDVI uses; F$GETDVI is documented as a $GETDVI wrapper, so it
 * resolves the device the same way. That helper is static in another image;
 * DCL translates with dcl_translate_logical() here rather than pull in a new
 * cross-image symbol. Bounded against a translation loop.
 */
#define GETDVI_XLATE_MAX_DEPTH 8
static uint32_t getdvi_resolve(const char *devnam_in, struct vms_devinfo *info)
{
    char cur[VMSFS_MAX_DEVICE + 1];
    strncpy(cur, devnam_in, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = '\0';

    (void)vms_kif_open();

    for (int depth = 0; depth < GETDVI_XLATE_MAX_DEPTH; depth++) {
        uint32_t status = vms_kif_getdvi_devnam(cur, info);
        if (status != SS$_NOSUCHDEV)
            return status;   /* SS$_NORMAL, or a real failure (IVDEVNAM/...) */

        /* Not a physical unit: try it as a logical name. The translator keys
         * names WITHOUT a trailing colon, so strip one before the lookup. */
        char key[VMSFS_MAX_DEVICE + 1];
        strncpy(key, cur, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
        size_t klen = strlen(key);
        if (klen > 0 && key[klen - 1] == ':')
            key[--klen] = '\0';
        if (klen == 0)
            return SS$_NOSUCHDEV;

        char equiv[256];
        if (dcl_translate_logical(key, equiv, sizeof(equiv)) != 0)
            return SS$_NOSUCHDEV;   /* no such logical: honest no-such-device */

        vmsfs_filespec_t parts;
        memset(&parts, 0, sizeof(parts));
        if (!$VMS_STATUS_SUCCESS(vmsfs_parse_filespec(equiv, &parts)) ||
            !parts.has_device || parts.device[0] == '\0')
            return SS$_NOSUCHDEV;

        if (strcmp(parts.device, cur) == 0)
            return SS$_NOSUCHDEV;    /* fixed point: refuse to loop forever */
        strncpy(cur, parts.device, sizeof(cur) - 1);
        cur[sizeof(cur) - 1] = '\0';
    }
    return SS$_NOSUCHDEV;
}

/*
 * F$GETDVI(device, item) - Get device information (vms-050).
 *
 * Reads the EXECUTIVE'S I/O database (getdvi_resolve() above, then
 * vms_kif_getvol() for the mounted-volume items), exactly as VMS's F$GETDVI is
 * a wrapper over $GETDVI. Every value returned is the executive's own; when the
 * executive does not track an item (device type is Unknown, a device is not a
 * mounted volume, ...) the honest VMS "not available" answer is returned -- an
 * empty string or 0 per the public DCL Dictionary F$GETDVI semantics -- NEVER a
 * plausible-looking constant. The prior implementation fabricated EXISTS=TRUE
 * for every name, a name-substring-guessed DEVTYPE/DEVCLASS, VOLNAM="OVMXSYS",
 * MOUNTCNT="1" and statvfs("/")-derived block counts; all of that is gone
 * (INV-6).
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

    /* Resolve the device against the executive's device table. */
    struct vms_devinfo info;
    memset(&info, 0, sizeof(info));
    uint32_t status = getdvi_resolve(device, &info);
    int exists = (status == SS$_NORMAL);
    if (exists)
        info.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    /*
     * EXISTS is the one item defined for a device that does NOT exist: it
     * returns the honest Boolean, never an error (public DCL Dictionary). A
     * bogus device name therefore yields "FALSE", not the old unconditional
     * "TRUE".
     */
    if (strcmp(item, "EXISTS") == 0) {
        snprintf(result, result_size, exists ? "TRUE" : "FALSE");
        return 0;
    }

    /*
     * For every other item the device must exist. VMS's F$GETDVI signals the
     * executive's own diagnostic when it does not -- the same messages SHOW
     * DEVICE emits (src/vmsdcl/dcl_cmd_show.c, oracle-pinned). The value stays
     * empty and the call fails.
     */
    if (!exists) {
        if (status == SS$_IVDEVNAM)
            dcl_error("SYSTEM", 0, "IVDEVNAM", "invalid device name");
        else
            dcl_error("SYSTEM", 0, "NOSUCHDEV", "no such device available");
        result[0] = '\0';
        return -1;
    }

    /* Mounted-volume items come from the ACP's executive-global mounted-volume
     * table, read once here (a non-disk or unmounted unit yields mounted==0,
     * which is the honest answer -- empty label, zero counts, never a fake). */
    struct vms_getvol_args vol;
    memset(&vol, 0, sizeof(vol));
    (void)vms_kif_getvol(info.devnam, &vol);

    if (strcmp(item, "DEVNAM") == 0 ||
        strcmp(item, "FULLDEVNAM") == 0 ||
        strcmp(item, "ALLDEVNAM") == 0) {
        /* The executive's own physical name (e.g. "DKA0:"). Single node: the
         * full/all name is the physical name -- a cluster node prefix is a
         * documented remainder. No leading underscore is invented here (the
         * executive does not store one, and F$DEVICE deliberately does not add
         * one either -- see populate_device_list). */
        snprintf(result, result_size, "%s", info.devnam);
    } else if (strcmp(item, "TT_ACCPORNAM") == 0) {
        /* A remote terminal's access port name, node::user (rd vms-2166),
         * from the executive's device row; "" for a local terminal or a
         * non-terminal, as VMS returns. */
        char rpi[64] = "";
        if (!(vms_kif_terminal_getrpi(info.devnam, rpi, sizeof(rpi)) & 1))
            rpi[0] = '\0';
        snprintf(result, result_size, "%s", rpi);
    } else if (strcmp(item, "DEVCLASS") == 0) {
        snprintf(result, result_size, "%u", info.devclass);
    } else if (strcmp(item, "DEVTYPE") == 0) {
        /* Real device type code; 0 = Unknown, which is what the executive
         * genuinely records for OVMX's console and disks (it does not model a
         * VT-model or an RA-model), NOT the old fabricated DT$_VT100/DT$_RA92. */
        snprintf(result, result_size, "%u", info.devtype);
    } else if (strcmp(item, "TT_PAGE") == 0) {
        /* Terminal page length (DCL Dictionary F$GETDVI TT_PAGE), from the
         * executive's row -- on an RTAn:, the originating terminal's, as the
         * CTERM host recorded it (rd vms-14b). Empty for a non-terminal. */
        if (info.devclass == 66 /* DC$_TERM */)
            snprintf(result, result_size, "%u", info.page);
    } else if (strcmp(item, "DEVCHAR") == 0) {
        uint32_t chars = GETDVI_DEVCHAR_AVL;      /* present in the I/O DB */
        if (info.allocated) chars |= GETDVI_DEVCHAR_ALL;
        if (vol.mounted)    chars |= GETDVI_DEVCHAR_MNT;
        snprintf(result, result_size, "%u", chars);
    } else if (strcmp(item, "UNIT") == 0) {
        /* Trailing digits of the physical name (DKA100: -> 100). */
        const char *d = info.devnam;
        const char *q = d + strlen(d);
        while (q > d && (q[-1] == ':')) q--;
        const char *e = q;
        while (q > d && isdigit((unsigned char)q[-1])) q--;
        snprintf(result, result_size, "%u",
                 (q < e) ? (unsigned)strtoul(q, NULL, 10) : 0u);
    } else if (strcmp(item, "REFCNT") == 0) {
        snprintf(result, result_size, "%u", info.refcnt);
    } else if (strcmp(item, "ERRCNT") == 0) {
        snprintf(result, result_size, "%u", info.errcnt);
    } else if (strcmp(item, "OPCNT") == 0) {
        snprintf(result, result_size, "%llu",
                 (unsigned long long)info.opcnt);
    } else if (strcmp(item, "PID") == 0) {
        /* Owner PID, VMS's hexadecimal process-id form; 0 when unowned. */
        snprintf(result, result_size, "%08X", info.owner_pid);
    } else if (strcmp(item, "OWNUIC") == 0) {
        snprintf(result, result_size, "%u", info.owner_uic);
    } else if (strcmp(item, "AVAILABLE") == 0) {
        /* A device the executive lists is available; it models no offline
         * state yet, so "present" is the honest answer (a Boolean item). */
        snprintf(result, result_size, "TRUE");
    } else if (strcmp(item, "MNT") == 0) {
        snprintf(result, result_size, vol.mounted ? "TRUE" : "FALSE");
    } else if (strcmp(item, "VOLNAM") == 0) {
        /* The mounted ODS-2 volume label, or empty when nothing is mounted --
         * never the old fabricated "OVMXSYS"/"VOLUME". */
        if (vol.mounted) {
            vol.volnam[VMS_GETVOL_LABEL_SIZE - 1] = '\0';
            snprintf(result, result_size, "%s", vol.volnam);
        } else {
            result[0] = '\0';
        }
    } else if (strcmp(item, "MAXBLOCK") == 0) {
        /* Volume size in blocks from the SCB, or 0 when not a mounted volume --
         * never the old statvfs("/") figure for the Linux root. */
        snprintf(result, result_size, "%u", vol.mounted ? vol.volsize : 0u);
    } else if (strcmp(item, "FREEBLOCKS") == 0) {
        /* Counted from the volume's storage bitmap at call time; 0 when not a
         * mounted volume or the bitmap could not be read this call. */
        snprintf(result, result_size, "%u",
                 (vol.mounted && vol.free_valid) ? vol.freeblocks : 0u);
    } else if (strcmp(item, "CLUSTER") == 0) {
        snprintf(result, result_size, "%u", vol.mounted ? vol.cluster : 0u);
    } else if (strcmp(item, "MOUNTCNT") == 0) {
        /* Real mount state: 1 for a genuinely mounted volume (single node), 0
         * for one that is not -- never the old unconditional "1". */
        snprintf(result, result_size, "%u", vol.mounted ? 1u : 0u);
    } else {
        /* An item the executive does not track here. Honest empty answer,
         * never a fabricated value (Rule 10). */
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

    /*
     * Optional object-id parameter. In F$GETQUI this identifies WHICH object
     * the caller is asking about (VSI OpenVMS DCL Dictionary, F$GETQUI): for
     * DISPLAY_QUEUE it is the queue NAME (a string, e.g. "SYS$BATCH" or
     * "SYS$PRINT" — the HELPLIB.HLP example shows exactly this form), and for
     * DISPLAY_ENTRY it is the entry NUMBER. We keep the raw string (id_str)
     * so DISPLAY_QUEUE can honour the requested queue, and the numeric
     * derivation (entry_id) for DISPLAY_ENTRY.
     *
     * INV-6 / vms-050: the previous implementation DISCARDED this argument for
     * DISPLAY_QUEUE and pinned the queue to "SYS$BATCH", so a caller asking
     * about SYS$PRINT (or any other/absent queue) was answered with SYS$BATCH's
     * data — a fabrication (right facility, wrong/ignored selection). We now
     * look the requested queue up in the real queue state and, when it does not
     * exist, return the honest no-such-queue status and an empty value rather
     * than another queue's data.
     */
    char id_str[64] = {0};
    uint32_t entry_id = 0;
    if (*p == ',') {
        p++;
        while (*p == ' ') p++;
        i = 0;
        in_quote = 0;
        while (*p && i < sizeof(id_str) - 1) {
            if (*p == '"') { in_quote = !in_quote; p++; continue; }
            if ((*p == ',' || *p == ' ') && !in_quote) break;
            id_str[i++] = *p;
            p++;
        }
        id_str[i] = '\0';
        size_t idlen = strlen(id_str);
        while (idlen > 0 && (id_str[idlen-1] == ' ' || id_str[idlen-1] == '\t'))
            id_str[--idlen] = '\0';
        entry_id = (uint32_t)strtoul(id_str, NULL, 0);
    }

    /* The queue manager is always available on a running VMS system; bring
     * QMAN$MASTER.DAT and the default queues up if a queue command has not
     * already done so, so F$GETQUI can read real state standalone. */
    ensure_queue_init();

    if (strcmp(func, "DISPLAY_QUEUE") == 0) {
        /*
         * Honour the caller's queue selection. No object-id (and no queue
         * named) means no queue is selected — OVMX does not implement the
         * wildcard-context walk — so we fail honestly rather than defaulting
         * to SYS$BATCH.
         */
        struct vms_queue qinfo;
        int rc = SS$_ITEMNOTFOUND;
        if (id_str[0] != '\0')
            rc = vmsq_show_queue(id_str, &qinfo);
        if (rc != SS$_NORMAL) {
            /* No such queue / no queue selected: VMS signals this in $STATUS
             * (JBC$_NOSUCHQUE class; the queue layer reports SS$_ITEMNOTFOUND)
             * and F$GETQUI returns the empty value. NEVER SYS$BATCH's data for
             * a queue the caller did not ask about. */
            if (ctx) ctx->last_status = rc;
            result[0] = '\0';
            return 0;
        }
        if (ctx) ctx->last_status = SS$_NORMAL;
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
        if (rc != SS$_NORMAL) {
            if (ctx) ctx->last_status = rc;
            result[0] = '\0';
            return 0;
        }
        if (ctx) ctx->last_status = SS$_NORMAL;
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
 * connection manager, which the DCL layer reads through /dev/vms -- and there
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
int dcl_eval_lexical_args(struct dcl_context *ctx, const char *raw,
                          char *out, size_t outsz);   /* dcl_exec.c */

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

    /* Look up and call the function. Its arguments are expressions and are
     * evaluated first (dcl_eval_lexical_args), except F$TYPE's, which NAMES
     * a symbol rather than giving a value. */
    for (int i = 0; lex_functions[i].name; i++) {
        if (strcmp(func_name, lex_functions[i].name) == 0) {
            if (strcmp(func_name, "F$TYPE") == 0 || args[0] == '\0')
                return lex_functions[i].handler(ctx, args, result, result_size);
            char vargs[4096];
            if (dcl_eval_lexical_args(ctx, args, vargs, sizeof(vargs)) < 0)
                return -1;
            return lex_functions[i].handler(ctx, vargs, result, result_size);
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
    if (ctx) ctx->last_status = 0x000381C0;   /* CLI-W-IVFNAM */
    return -1;
}
