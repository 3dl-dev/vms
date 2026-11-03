/*
 * link_cli.c - LINK.EXE takes its command the VMS way (rd vms-3cb1).
 *
 *   $ LINK[/qualifiers] file-spec[/file-qualifiers][,...]
 *
 * Included by link.c when it is built as the shipped image (OVMX_LINK_IMAGE).
 * The command line is obtained from the invoking CLI with LIB$GET_FOREIGN and
 * parsed by CLI$DCL_PARSE against the LINK command definition (link_cld.h);
 * every qualifier and file is then read with CLI$PRESENT / CLI$GET_VALUE, and
 * the answers fill the struct link_cmd that link_run() acts on. There is no
 * other way into this image: no argument vector is read, and no build flag
 * exists in it (the shipped-image strings gate checks that).
 *
 * Semantics follow the public OpenVMS Linker Utility Manual (LINK Command
 * Reference, Options Files):
 *   /EXECUTABLE[=file]   an executable image (the default); the name defaults to
 *                        the first input file's name, type .EXE
 *   /SHAREABLE[=file]    after the verb: a shareable image, whose universals the
 *                        SYMBOL_VECTOR= and GSMATCH= options declare
 *   /MAP[=file] /BRIEF   a link map, default name first-input.MAP
 *   file/OPTIONS         an options file
 *   file/LIBRARY         an object library, searched for what the link still
 *                        needs where it appears in the command
 *   file/INCLUDE=(m,...) those modules of the library, linked unconditionally
 *   file/SHAREABLE       a shareable image to link against (an options-file
 *                        form on OpenVMS; taken on the command line as well)
 *   /NOSYSLIB /NOSYSSHR  no implicit search of STARLET / the system shareables
 *   /NOINFORMATIONALS    no %LINK-I- messages
 * A qualifier the CLD defines that this linker does not honour is refused with
 * %LINK-F-NOTSUPP, never accepted and ignored. /DEBUG and /TRACEBACK draw a
 * %LINK-W-IGNORED: the image carries the debug sections its objects carry.
 */

#include <ctype.h>
#include <stdint.h>
#include "link_cmd.h"
#include "descrip.h"
#include "clitable.h"
#include "libclidef.h"
#include "climsgdef.h"

#ifndef OVMX_LINK_CLI_ARGV
uint32_t lib$get_foreign(struct dsc$descriptor_s *result,
                         const struct dsc$descriptor_s *prompt,
                         uint16_t *result_len, ...);
#endif

#include "link_cld.h"

static void lcli_fatal(const char *code, const char *what)
{
    fprintf(stderr, "%%LINK-F-%s, %s\n", code, what);
    exit(EXIT_FAILURE);
}

static struct dsc$descriptor_s lcli_desc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length = (uint16_t)strlen(s);
    d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static uint32_t lcli_present(const char *entity)
{
    struct dsc$descriptor_s d = lcli_desc(entity);
    return cli$present(&d);
}

/* The next value of ENTITY, heap-allocated, or NULL when there is none. *more
 * is set when a further value follows in the list. */
static char *lcli_value(const char *entity, int *more)
{
    struct dsc$descriptor_s d = lcli_desc(entity);
    char buf[1024];
    struct dsc$descriptor_s v;
    v.dsc$w_length = sizeof buf - 1;
    v.dsc$b_dtype = DSC$K_DTYPE_T;
    v.dsc$b_class = DSC$K_CLASS_S;
    v.dsc$a_pointer = buf;
    uint16_t len = 0;
    uint32_t st = cli$get_value(&d, &v, &len);
    if (more) *more = (st == CLI$_COMMA || st == CLI$_CONCAT);
    if (!(st & 1)) return NULL;
    char *r = malloc((size_t)len + 1);
    if (!r) die("oom");
    memcpy(r, buf, len);
    r[len] = 0;
    return r;
}

static char *lcli_dup(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (!p) die("oom");
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* `q` abbreviates `full` if it is a non-empty prefix of it (VMS: unique prefix). */
static int lcli_abbrev(const char *q, size_t qn, const char *full)
{
    if (qn == 0 || qn > strlen(full)) return 0;
    for (size_t i = 0; i < qn; i++)
        if (toupper((unsigned char)q[i]) != toupper((unsigned char)full[i])) return 0;
    return 1;
}

/* The file-name part's type, after the last ']' or ':'. */
static const char *lcli_type(const char *spec)
{
    const char *base = spec;
    for (const char *p = spec; *p; p++)
        if (*p == ']' || *p == ':') base = p + 1;
    return strrchr(base, '.');
}

/* Add the VMS default type when the name has none (lower case: RMS ignores the
 * case; a case-sensitive host finds the files a build writes in lower case). */
static char *lcli_default_type(const char *spec, const char *type)
{
    if (lcli_type(spec)) return lcli_dup(spec, strlen(spec));
    size_t n = strlen(spec) + strlen(type);
    char *p = malloc(n + 1);
    if (!p) die("oom");
    strcpy(p, spec); strcat(p, type);
    return p;
}

/* NAME.TYPE in the current directory, from the file name of SPEC. */
static char *lcli_derived_name(const char *spec, const char *type)
{
    const char *base = spec;
    for (const char *q = spec; *q; q++) if (*q == ']' || *q == ':' || *q == '/') base = q + 1;
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    char *p = malloc(n + strlen(type) + 1);
    if (!p) die("oom");
    memcpy(p, base, n); strcpy(p + n, type);
    return p;
}

struct lcli_state {
    struct link_cmd *c;
    const char     **clusters;     /* CLUSTER= option files, declaration order */
    int              nclusters, cap_clusters;
    const char      *first_input;  /* the default image / map name derive from it */
    int              depth;        /* options-file nesting */
};

static void lcli_add_object(struct lcli_state *st, const char *spec)
{
    char *o = lcli_default_type(spec, ".obj");
    if (!st->first_input) st->first_input = o;
    lc_input(st->c, o);
}

static void lcli_read_options(struct lcli_state *st, const char *path);

/* One options-file file line: file[/LIBRARY|/SHAREABLE|/INCLUDE=(m,...)|
 * /SELECTIVE_SEARCH][, file ...]. The linker reads its options files itself, on
 * OpenVMS too; the CLI is not involved. */
static void lcli_option_files(struct lcli_state *st, const char *path, char *s)
{
    char *seg = s;
    while (seg && *seg) {
        /* the next top-level comma (not inside an /INCLUDE=(...) list) */
        char *comma = NULL;
        int depth = 0;
        for (char *e = seg; *e; e++) {
            if (*e == '(') depth++;
            else if (*e == ')' && depth) depth--;
            else if (*e == ',' && !depth) { comma = e; break; }
        }
        if (comma) *comma = 0;
        char *q = strchr(seg, '/');
        int lib = 0, shr = 0, nincl = 0;
        char *incl[64];
        if (q) {
            *q = 0;
            for (char *t = q + 1; t && *t;) {
                char *nx = strchr(t, '/');
                if (nx) *nx = 0;
                char *eqv = strchr(t, '=');
                size_t qn = eqv ? (size_t)(eqv - t) : strlen(t);
                while (qn && isspace((unsigned char)t[qn - 1])) qn--;
                if (lcli_abbrev(t, qn, "LIBRARY") && qn >= 3) lib = 1;
                else if (lcli_abbrev(t, qn, "SHAREABLE") && qn >= 3) shr = 1;
                else if (lcli_abbrev(t, qn, "INCLUDE") && qn >= 3 && eqv) {
                    char *v = eqv + 1;
                    while (*v == '(' || isspace((unsigned char)*v)) v++;
                    for (char *m = strtok(v, ",)"); m; m = strtok(NULL, ",)")) {
                        while (isspace((unsigned char)*m)) m++;
                        size_t ml = strlen(m);
                        while (ml && isspace((unsigned char)m[ml - 1])) m[--ml] = 0;
                        if (ml && nincl < 64) incl[nincl++] = lcli_dup(m, ml);
                    }
                } else
                    fprintf(stderr, "%%LINK-W-IGNORED, options file %s: /%.*s on %s is not acted on "
                            "by this linker\n", path, (int)qn, t, seg);
                t = nx ? nx + 1 : NULL;
            }
        }
        while (*seg && isspace((unsigned char)*seg)) seg++;
        size_t sl = strlen(seg);
        while (sl && isspace((unsigned char)seg[sl - 1])) seg[--sl] = 0;
        if (*seg) {
            if (shr) lc_use(st->c, lcli_default_type(seg, ".exe"));
            else if (lib || nincl) {
                char *l = lcli_default_type(seg, ".olb");
                for (int k = 0; k < nincl; k++) lc_include(st->c, l, incl[k]);
                if (lib) lc_library(st->c, l);
            } else lcli_add_object(st, seg);
        }
        seg = comma ? comma + 1 : NULL;
    }
}

/* An options-file keyword this linker acts on. Returns 1 when handled. */
static int lcli_option_keyword(struct lcli_state *st, const char *line)
{
    const char *eq = strchr(line, '=');
    size_t kn = eq ? (size_t)(eq - line) : strlen(line);
    while (kn && isspace((unsigned char)line[kn - 1])) kn--;
    if (kn == 0) return 0;
    const char *val = eq ? eq + 1 : "";
    while (*val && isspace((unsigned char)*val)) val++;
    /* SYMBOL_VECTOR=(name=PROCEDURE|DATA|...,...): the universals of a shareable
     * image, in vector order; several SYMBOL_VECTOR options append. */
    if (lcli_abbrev(line, kn, "SYMBOL_VECTOR") && kn >= 3) {
        char *v = lcli_dup(val, strlen(val));
        char *b = v;
        size_t n = strlen(b);
        if (*b == '(') { b++; n--; }
        while (n && (isspace((unsigned char)b[n - 1]))) n--;
        if (n && b[n - 1] == ')') n--;
        b[n] = 0;
        /* drop blanks between entries */
        char *w = b;
        for (char *r = b; *r; r++) if (!isspace((unsigned char)*r)) *w++ = *r;
        *w = 0;
        if (*b) lc_symvec(st->c, b);
        free(v);
        return 1;
    }
    /* GSMATCH=ALWAYS|EQUAL|LEQUAL,major,minor */
    if (lcli_abbrev(line, kn, "GSMATCH") && kn >= 3) {
        char *v = lcli_dup(val, strlen(val));
        char *w = v;
        for (char *r = v; *r; r++) if (!isspace((unsigned char)*r)) *w++ = *r;
        *w = 0;
        st->c->gsmatch = v;
        return 1;
    }
    /* PSECT_ATTR=LIB$INITIALIZE,GBL: the psect LIB$INITIALIZE is global so every
     * module's contribution is gathered into the one vector the image startup
     * walks. This linker gathers it by name regardless, so the attribute is
     * already in force. Any other psect attribute is not honoured. */
    if (lcli_abbrev(line, kn, "PSECT_ATTR") && kn >= 5) {
        char up[128]; size_t i = 0;
        for (; val[i] && i < sizeof up - 1; i++) up[i] = (char)toupper((unsigned char)val[i]);
        up[i] = 0;
        return strcmp(up, "LIB$INITIALIZE,GBL") == 0;
    }
    /* CASE_SENSITIVE=YES|NO only changes how the options that follow compare names;
     * this linker compares names exactly. Both spellings are in force already. */
    if (lcli_abbrev(line, kn, "CASE_SENSITIVE") && kn >= 4) return 1;
    /* CLUSTER=name[,base-address[,pfc[,file-spec,...]]] (vms-5b3): the files are
     * input to the link, in a cluster the options declare; option-declared
     * clusters come ahead of the default cluster, so their files are linked
     * before the command's own. This linker places the image as one cluster
     * (no base address, no page fault cluster): a base or pfc is not honoured
     * and says so. The GCC port's ld names libgcc's vms-dwarf2eh.o this way. */
    if (lcli_abbrev(line, kn, "CLUSTER") && kn >= 3) {
        char *v = lcli_dup(val, strlen(val));
        int field = 0;
        for (char *f = v; f;) {
            char *cm = strchr(f, ',');
            if (cm) *cm = 0;
            while (*f && isspace((unsigned char)*f)) f++;
            size_t fl = strlen(f);
            while (fl && isspace((unsigned char)f[fl - 1])) f[--fl] = 0;
            if ((field == 1 || field == 2) && *f)
                fprintf(stderr, "%%LINK-W-IGNORED, CLUSTER=%s: %s '%s' is not acted on by this linker\n",
                        val, field == 1 ? "base address" : "page fault cluster", f);
            else if (field >= 3 && *f) {
                st->clusters = lc_grow(st->clusters, &st->cap_clusters, st->nclusters,
                                       sizeof *st->clusters);
                st->clusters[st->nclusters++] = lcli_default_type(f, ".obj");
            }
            field++;
            f = cm ? cm + 1 : NULL;
        }
        return 1;
    }
    return 0;
}

static void lcli_read_options(struct lcli_state *st, const char *path)
{
    if (st->depth >= 8) lcli_fatal("OPTNEST", "options files nested too deeply");
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "%%LINK-F-OPENIN, error opening %s as input\n", path);
        exit(EXIT_FAILURE);
    }
    st->depth++;
    char buf[1024], line[8192];
    line[0] = 0;
    while (fgets(buf, sizeof buf, f)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || isspace((unsigned char)buf[n - 1]))) buf[--n] = 0;
        char *bang = strchr(buf, '!');          /* comment to end of line */
        if (bang) { *bang = 0; n = strlen(buf); while (n && isspace((unsigned char)buf[n - 1])) buf[--n] = 0; }
        const char *p = buf;
        while (*p && isspace((unsigned char)*p)) p++;
        int cont = n && buf[n - 1] == '-';       /* hyphen continues the line */
        if (cont) buf[--n] = 0;
        if (strlen(line) + strlen(p) + 1 >= sizeof line) lcli_fatal("OPTLONG", "options line too long");
        strcat(line, p);
        if (cont) continue;
        char *s = line;
        while (*s && isspace((unsigned char)*s)) s++;
        if (*s) {
            /* a file spec has no `=` before its first `/`; a keyword is NAME[=value] */
            const char *slash = strchr(s, '/');
            const char *eq = strchr(s, '=');
            int is_file = !eq || (slash && slash < eq);
            if (strchr(s, ':') && eq && strchr(s, ':') < eq && !(slash && slash < eq)) is_file = 1;
            if (!is_file) {
                if (!lcli_option_keyword(st, s))
                    fprintf(stderr, "%%LINK-W-IGNORED, options file %s: '%s' is not acted on by this linker\n", path, s);
            } else {
                lcli_option_files(st, path, s);
            }
        }
        line[0] = 0;
    }
    fclose(f);
    st->depth--;
}

/* The file name part (after the last `]`, `:` or `/`). */
static const char *lcli_leaf(const char *spec)
{
    const char *b = spec;
    for (const char *p = spec; *p; p++)
        if (*p == ']' || *p == ':' || *p == '/') b = p + 1;
    return b;
}

static int lcli_same_leaf(const char *a, const char *b)
{
    a = lcli_leaf(a); b = lcli_leaf(b);
    return strlen(a) == strlen(b) && strncasecmp(a, b, strlen(a)) == 0;
}

/* An implicit library/shareable is added when it exists and the command did not
 * already name a file of that name. */
static int lcli_implicit_ok(const struct link_cmd *c, const char *spec, int shareable)
{
    for (int i = 0; i < c->nuse; i++) if (lcli_same_leaf(c->uses[i], spec)) return 0;
    for (int i = 0; i < c->nlibs; i++) if (lcli_same_leaf(c->libs[i], spec)) return 0;
    char rp[512];
    /* a shareable is looked for where binding it would open it */
    FILE *f = fopen(shareable ? resolve_producer_path(spec, rp, sizeof rp) : spec, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* A qualifier the LINK table defines but this linker does not honour: refused
 * when it is given, either way. */
static void lcli_refuse_unhonoured(void)
{
    static const char *const q[] = {
        "SYSTEM", "SYMBOL_TABLE", "DSF", "SYSEXE", "BPAGE", "CONTIGUOUS",
        "DEMAND_ZERO", "DNI", "GST", "HEADER", "NATIVE_ONLY", "P0IMAGE", "PROTECT",
        "REPLACE", "SECTION_BINDING", "THREADS_ENABLE", "ALPHA", "VAX",
        "FULL", "CROSS_REFERENCE", NULL };
    for (int i = 0; q[i]; i++) {
        uint32_t s = lcli_present(q[i]);
        if (s == CLI$_PRESENT || s == CLI$_NEGATED) {
            fprintf(stderr, "%%LINK-F-NOTSUPP, /%s%s is not supported by this linker\n",
                    s == CLI$_NEGATED ? "NO" : "", q[i]);
            exit(EXIT_FAILURE);
        }
    }
}

int main(int argc, char **argv)
{
    /* The command line, from the CLI that ran this image. */
    char *cmd;
#ifdef OVMX_LINK_CLI_ARGV
    /* OpenVMX/Alpha (rd vms-3cb1 follow-up): the image is a DEC C program whose
     * C RTL hands main() the CLI's command line as words; they are rejoined into
     * the command and parsed by CLI$DCL_PARSE like any other. No word is read as
     * an option. (x86_64/aarch64 ask the CLI directly, LIB$GET_FOREIGN below.) */
    size_t n = 0;
    for (int i = 1; i < argc; i++) n += strlen(argv[i]) + 1;
    cmd = malloc(n + 6);
    if (!cmd) die("oom");
    strcpy(cmd, "LINK");
    for (int i = 1; i < argc; i++) { strcat(cmd, " "); strcat(cmd, argv[i]); }
#else
    (void)argc; (void)argv;           /* the command comes from the CLI, not argv */
    struct dsc$descriptor_s tail;
    tail.dsc$w_length = 0;
    tail.dsc$b_dtype = DSC$K_DTYPE_T;
    tail.dsc$b_class = DSC$K_CLASS_D;
    tail.dsc$a_pointer = NULL;
    uint16_t tlen = 0;
    uint32_t gst = lib$get_foreign(&tail, NULL, &tlen);
    if (!(gst & 1)) lcli_fatal("NOFILES", "no input file specified");
    size_t n = tail.dsc$w_length;
    cmd = malloc(n + 6);
    if (!cmd) die("oom");
    memcpy(cmd, "LINK ", 5);
    if (n) memcpy(cmd + 5, tail.dsc$a_pointer, n);
    cmd[5 + n] = 0;
#endif
    uint32_t st;

    struct dsc$descriptor_s cldsrc = lcli_desc(link_cld_source);
    struct cli_command_table *table = NULL;
    if (!(cli$compile_cld(&cldsrc, &table) & 1) || !table)
        lcli_fatal("CLDERR", "the LINK command table did not compile");
    struct dsc$descriptor_s cmdd = lcli_desc(cmd);
    st = cli$dcl_parse(&cmdd, table, NULL, NULL);
    if (st == CLI$_IVQUAL) {
        fprintf(stderr, "%%CLI-W-IVQUAL, unrecognized qualifier - check validity, spelling, "
                "and placement\n");
        exit(EXIT_FAILURE);
    }
    if (!(st & 1)) lcli_fatal("SYNTAX", "the LINK command does not parse");

    struct link_cmd c;
    memset(&c, 0, sizeof c);
    struct lcli_state ls;
    memset(&ls, 0, sizeof ls);
    ls.c = &c;

    /* ---- command qualifiers (asked before any file value is fetched, so a
     * POSITIONAL qualifier answers for the command) ---- */
    lcli_refuse_unhonoured();
    g_noinform = (lcli_present("INFORMATIONALS") == CLI$_NEGATED);
    char *image = NULL;
    uint32_t sh = lcli_present("SHAREABLE");
    uint32_t ex = lcli_present("EXECUTABLE");
    if (ex == CLI$_NEGATED)
        lcli_fatal("NOTSUPP", "/NOEXECUTABLE is not supported: this linker writes an image");
    if (sh == CLI$_PRESENT) {
        if (ex == CLI$_PRESENT)
            lcli_fatal("CONFQUAL", "/EXECUTABLE and /SHAREABLE conflict");
        c.shareable = 1;
        image = lcli_value("SHAREABLE", NULL);
    } else {
        c.executable = 1;
        if (ex == CLI$_PRESENT) image = lcli_value("EXECUTABLE", NULL);
    }
    char *map = NULL;
    int want_map = (lcli_present("MAP") == CLI$_PRESENT);
    if (want_map) map = lcli_value("MAP", NULL);
    c.map_brief = (lcli_present("BRIEF") == CLI$_PRESENT);
    int nosyslib = (lcli_present("SYSLIB") == CLI$_NEGATED);
    int nosysshr = (lcli_present("SYSSHR") == CLI$_NEGATED);
    if (lcli_present("DEBUG") == CLI$_PRESENT || lcli_present("TRACEBACK") == CLI$_PRESENT ||
        lcli_present("DEBUG") == CLI$_NEGATED || lcli_present("TRACEBACK") == CLI$_NEGATED)
        fprintf(stderr, "%%LINK-W-IGNORED, /DEBUG, /TRACEBACK: the image carries the debug "
                "sections its objects contain; no debugger symbol table is generated\n");
    if (lcli_present("USERLIBRARY") == CLI$_PRESENT)
        fprintf(stderr, "%%LINK-W-IGNORED, /USERLIBRARY: there are no user library logicals "
                "to search; name each library\n");

    /* ---- the input files, each with the file qualifiers placed on it ---- */
    int more = 1;
    while (more) {
        char *f = lcli_value("FILE", &more);
        if (!f) break;
        if (lcli_present("OPTIONS") == CLI$_LOCPRES) {
            lcli_read_options(&ls, lcli_default_type(f, ".opt"));
            continue;
        }
        if (lcli_present("SHAREABLE") == CLI$_LOCPRES) {
            lc_use(&c, lcli_default_type(f, ".exe"));
            continue;
        }
        int lib = (lcli_present("LIBRARY") == CLI$_LOCPRES);
        int inc = (lcli_present("INCLUDE") == CLI$_LOCPRES);
        if (lcli_present("SELECTIVE_SEARCH") == CLI$_LOCPRES)
            fprintf(stderr, "%%LINK-W-IGNORED, /SELECTIVE_SEARCH on %s is not acted on by "
                    "this linker\n", f);
        if (lib || inc) {
            char *l = lcli_default_type(f, ".olb");
            int mm = 1;
            while (inc && mm) {
                char *m = lcli_value("INCLUDE", &mm);
                if (!m) break;
                lc_include(&c, l, m);
            }
            if (lib) lc_library(&c, l);
            continue;
        }
        lcli_add_object(&ls, f);
    }

    /* Option-declared clusters precede the default cluster: their files go first. */
    if (ls.nclusters) {
        const char **v = malloc((size_t)(c.nin + ls.nclusters + 1) * sizeof *v);
        if (!v) die("oom");
        memcpy(v, ls.clusters, (size_t)ls.nclusters * sizeof *v);
        memcpy(v + ls.nclusters, c.ins, (size_t)c.nin * sizeof *v);
        for (int l = 0; l < c.nlibs; l++) c.lib_pos[l] += ls.nclusters;
        free(c.ins);
        c.ins = v;
        c.nin += ls.nclusters;
        c.cap_ins = c.nin + 1;
        if (!ls.first_input) ls.first_input = ls.clusters[0];
    }
    /* The default search of OpenVMS LINK: /SYSLIB searches SYS$LIBRARY:STARLET and
     * /SYSSHR the system's shareable images (IMAGELIB), after everything the command
     * names. Here: the C RTL and its companions that are installed, bound only where
     * the link references them (an unreferenced producer adds nothing to the image),
     * and the object library STARLET searched last. Whatever the command already
     * named (by file name) is not added twice. */
    if (!nosysshr) {
        static const char *const shr[] = { "SYS$SHARE:DECC$SHR.EXE", "SYS$SHARE:LIBOTS_SHR.EXE",
                                           "SYS$SHARE:LIBVMSRMS$SHR.EXE", "SYS$SHARE:LIBVMS$SHR.EXE", NULL };
        for (int i = 0; shr[i]; i++)
            if (lcli_implicit_ok(&c, shr[i], 1)) lc_use(&c, shr[i]);
    }
    if (!nosyslib && lcli_implicit_ok(&c, "SYS$LIBRARY:STARLET.A", 0))
        lc_library(&c, "SYS$LIBRARY:STARLET.A");
    /* The GCC port's startup module defines __main; the VMS object format
     * carries no transfer-address record for this producer, so name it. */
    c.transfer = "__main";

    if (!ls.first_input) {
        /* modules a library supplies by /INCLUDE are input too; the image name
         * then defaults from the library's */
        if (c.nin) ls.first_input = c.ins[0];
        else if (c.nincl) ls.first_input = c.incl[0].lib;
        else lcli_fatal("NOFILES", "no input file specified");
    }
    c.out = image && *image ? lcli_default_type(image, ".exe")
                            : lcli_derived_name(ls.first_input, ".exe");
    if (want_map)
        c.map = map && *map ? lcli_default_type(map, ".map")
                            : lcli_derived_name(ls.first_input, ".map");
    return link_run(&c);
}
