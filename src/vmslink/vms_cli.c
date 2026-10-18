/*
 * vms_cli.c - the VMS command syntax of LINK (vms-ce4), included by link.c.
 *
 *   LINK[/EXECUTABLE[=spec]] [/NOINFORMATIONALS] file[/OPTIONS|/LIBRARY|/SHAREABLE][,file...] ...
 *
 * DCL hands LINK.EXE the verb's argument tail as typed. This front end turns it
 * into the argument vector the rest of link.c has always taken (-o, --use,
 * --library, --transfer, input files), so one linker serves both: the GNU-style
 * flags the build scripts use, and the command the alpha-dec-vms GCC port's ld
 * wrapper (gcc/config/vms/vms-ld.c) writes: `$ link /exe=X /noinform OPT.TMP/opt`.
 *
 * A command is in VMS syntax when it names no output with -o. Arguments starting
 * with `-` are passed through unchanged (--transfer, --base, --use, --library:
 * OVMX additions to the command).
 *
 * An options file holds, one per line (`!` starts a comment; a trailing `-`
 * continues the line): `file[/LIBRARY|/SHAREABLE]` -- an object, an object
 * library, a shareable image to bind -- or an option keyword. The keywords this
 * linker acts on are listed in vms_opt_keyword(); any other draws
 * %LINK-W-IGNORED, never silence.
 *
 * Qualifiers that would be accepted and then not honoured are refused:
 * /SHAREABLE (as a command qualifier), /MAP, /CROSS_REFERENCE and /FULL write
 * no file here, so they draw %LINK-F-NOTSUPP.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef OVMX_LINK_C
/* Compiled on its own (the product-source symbol scan compiles every file): the two
 * names link.c supplies. Included by link.c, which defines OVMX_LINK_C, none of this. */
void die(const char *msg);
static int g_noinform;
#endif

struct vms_args {
    const char **v;
    int          n, cap;
};

static void vms_push(struct vms_args *a, const char *s)
{
    if (a->n + 1 >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 32;
        a->v = realloc(a->v, (size_t)a->cap * sizeof *a->v);
        if (!a->v) die("oom building the linker command");
    }
    a->v[a->n++] = s;
    a->v[a->n] = NULL;
}

static char *vms_dup(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (!p) die("oom");
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* `q` abbreviates `full` if it is a non-empty prefix of it (VMS: unique prefix). */
static int vms_ci_abbrev(const char *q, size_t qn, const char *full)
{
    if (qn == 0 || qn > strlen(full)) return 0;
    for (size_t i = 0; i < qn; i++)
        if (toupper((unsigned char)q[i]) != toupper((unsigned char)full[i])) return 0;
    return 1;
}

static const char *vms_ext(const char *spec)
{
    /* the extension of the file-name part: after the last ']' or ':' */
    const char *base = spec;
    for (const char *p = spec; *p; p++)
        if (*p == ']' || *p == ':') base = p + 1;
    const char *dot = strrchr(base, '.');
    return dot;
}

/* add the VMS default type when the name has none (lower case: RMS ignores the
 * case; a case-sensitive host finds the files a build writes in lower case) */
static char *vms_default_type(const char *spec, const char *type)
{
    if (vms_ext(spec)) return vms_dup(spec, strlen(spec));
    size_t n = strlen(spec) + strlen(type);
    char *p = malloc(n + 1);
    if (!p) die("oom");
    strcpy(p, spec); strcat(p, type);
    return p;
}

enum { VK_OBJ, VK_OPT, VK_LIB, VK_SHR };

struct vms_state {
    struct vms_args out;          /* the translated vector (argv[0] first) */
    const char     *exe;          /* /EXECUTABLE=spec */
    const char     *first_input;  /* default executable name derives from it */
    int             have_transfer;
    int             depth;        /* options-file nesting */
};

static void vms_fatal_qual(const char *code, const char *what)
{
    fprintf(stderr, "%%LINK-F-%s, %s\n", code, what);
    exit(1);
}

/* Act on a file with its kind. */
static void vms_add_file(struct vms_state *st, char *spec, int kind, int from_opt);

/* Keywords of an options file the linker acts on. Returns 1 when handled. */
static int vms_opt_keyword(struct vms_state *st, const char *line)
{
    (void)st;
    const char *eq = strchr(line, '=');
    size_t kn = eq ? (size_t)(eq - line) : strlen(line);
    while (kn && isspace((unsigned char)line[kn - 1])) kn--;
    char key[64];
    if (kn == 0 || kn >= sizeof key) return 0;
    memcpy(key, line, kn); key[kn] = 0;
    const char *val = eq ? eq + 1 : "";
    while (*val && isspace((unsigned char)*val)) val++;
    /* PSECT_ATTR=LIB$INITIALIZE,GBL: the psect LIB$INITIALIZE is global so every
     * module's contribution is gathered into the one vector the image startup
     * walks. This linker gathers it by name regardless, so the attribute is
     * already in force. Any other psect attribute is not honoured. */
    if (vms_ci_abbrev(key, kn, "PSECT_ATTR") && kn >= 5) {
        char up[128]; size_t i = 0;
        for (; val[i] && i < sizeof up - 1; i++) up[i] = (char)toupper((unsigned char)val[i]);
        up[i] = 0;
        if (strcmp(up, "LIB$INITIALIZE,GBL") == 0) return 1;
        return 0;
    }
    /* CASE_SENSITIVE=YES|NO only changes how the options that follow compare names;
     * this linker compares names exactly. Both spellings are in force already. */
    if (vms_ci_abbrev(key, kn, "CASE_SENSITIVE") && kn >= 4) return 1;
    return 0;
}

static void vms_read_options(struct vms_state *st, const char *path)
{
    if (st->depth >= 8) vms_fatal_qual("OPTNEST", "options files nested too deeply");
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "%%LINK-F-OPENIN, error opening %s as input\n", path);
        exit(1);
    }
    st->depth++;
    char buf[1024], line[4096];
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
        if (strlen(line) + strlen(p) + 1 >= sizeof line) vms_fatal_qual("OPTLONG", "options line too long");
        strcat(line, p);
        if (cont) continue;
        char *s = line;
        while (*s && isspace((unsigned char)*s)) s++;
        if (*s) {
            /* a file spec has no `=` before its first `/`; a keyword is NAME[=value] */
            const char *slash = strchr(s, '/');
            const char *eq = strchr(s, '=');
            int is_file = !eq || (slash && slash < eq);
            if (strchr(s, ':') && eq && strchr(s, ':') < eq && !(slash && slash < eq)) is_file = 1; /* dev:[dir]file */
            if (!is_file) {
                if (!vms_opt_keyword(st, s))
                    fprintf(stderr, "%%LINK-W-IGNORED, options file %s: '%s' is not acted on by this linker\n", path, s);
            } else {
                /* file[/LIBRARY|/SHAREABLE|/INCLUDE...][, file ...] */
                char *seg = s;
                while (seg && *seg) {
                    char *comma = strchr(seg, ',');
                    if (comma) *comma = 0;
                    char *q = strchr(seg, '/');
                    int kind = VK_OBJ;
                    if (q) {
                        *q = 0;
                        for (char *t = q + 1; t && *t;) {
                            char *nx = strchr(t, '/');
                            size_t tn = nx ? (size_t)(nx - t) : strlen(t);
                            char *eqv = memchr(t, '=', tn);
                            size_t qn = eqv ? (size_t)(eqv - t) : tn;
                            if (vms_ci_abbrev(t, qn, "LIBRARY") && qn >= 3) kind = VK_LIB;
                            else if (vms_ci_abbrev(t, qn, "SHAREABLE") && qn >= 3) kind = VK_SHR;
                            else
                                fprintf(stderr, "%%LINK-W-IGNORED, options file %s: /%.*s on %s is not acted on by this linker\n",
                                        path, (int)qn, t, seg);
                            t = nx ? nx + 1 : NULL;
                        }
                    }
                    while (*seg && isspace((unsigned char)*seg)) seg++;
                    if (*seg) vms_add_file(st, vms_dup(seg, strlen(seg)), kind, 1);
                    seg = comma ? comma + 1 : NULL;
                }
            }
        }
        line[0] = 0;
    }
    fclose(f);
    st->depth--;
}

static void vms_add_file(struct vms_state *st, char *spec, int kind, int from_opt)
{
    (void)from_opt;
    switch (kind) {
    case VK_OPT:
        vms_read_options(st, vms_default_type(spec, ".opt"));
        break;
    case VK_LIB:
        vms_push(&st->out, "--library");
        vms_push(&st->out, vms_default_type(spec, ".olb"));
        break;
    case VK_SHR:
        vms_push(&st->out, "--use");
        vms_push(&st->out, vms_default_type(spec, ".exe"));
        break;
    default: {
        char *o = vms_default_type(spec, ".obj");
        if (!st->first_input) st->first_input = o;
        vms_push(&st->out, o);
        break; }
    }
}

/* One command-line qualifier (name[=value]) with no file attached. */
static void vms_command_qualifier(struct vms_state *st, const char *q, size_t qn, const char *val, int *noinform)
{
    int neg = 0;
    if (qn > 2 && toupper((unsigned char)q[0]) == 'N' && toupper((unsigned char)q[1]) == 'O') {
        neg = 1; q += 2; qn -= 2;
    }
    if (vms_ci_abbrev(q, qn, "EXECUTABLE") && qn >= 3) {
        if (neg) vms_fatal_qual("NOTSUPP", "/NOEXECUTABLE is not supported: this linker writes an image");
        if (val && *val) st->exe = val;
    } else if (vms_ci_abbrev(q, qn, "INFORMATIONALS") && qn >= 4) {
        *noinform = neg;
    } else if (vms_ci_abbrev(q, qn, "SHAREABLE") && qn >= 3) {
        if (!neg)
            vms_fatal_qual("NOTSUPP", "/SHAREABLE image creation is not supported by the VMS command syntax "
                           "(use --shareable --symbol-vector)");
    } else if (vms_ci_abbrev(q, qn, "MAP") && qn >= 3) {
        if (!neg) vms_fatal_qual("NOTSUPP", "/MAP: this linker writes no link map");
    } else if (vms_ci_abbrev(q, qn, "FULL") && qn >= 3) {
        if (!neg) vms_fatal_qual("NOTSUPP", "/FULL: this linker writes no link map");
    } else if (vms_ci_abbrev(q, qn, "CROSS_REFERENCE") && qn >= 3) {
        if (!neg) vms_fatal_qual("NOTSUPP", "/CROSS_REFERENCE: this linker writes no link map");
    } else if ((vms_ci_abbrev(q, qn, "DEBUG") && qn >= 3) || (vms_ci_abbrev(q, qn, "TRACEBACK") && qn >= 3)) {
        fprintf(stderr, "%%LINK-W-IGNORED, /%s%.*s: the image carries the debug sections its objects contain; "
                "no debugger symbol table is generated\n", neg ? "NO" : "", (int)qn, q);
    } else if ((vms_ci_abbrev(q, qn, "SYSLIB") && qn >= 4) || (vms_ci_abbrev(q, qn, "SYSSHR") && qn >= 4)
               || (vms_ci_abbrev(q, qn, "USERLIBRARY") && qn >= 4)) {
        if (!neg)
            fprintf(stderr, "%%LINK-W-IGNORED, /%.*s: no implicit system library search list is defined; "
                    "name each library\n", (int)qn, q);
    } else if (vms_ci_abbrev(q, qn, "OPTIONS") || vms_ci_abbrev(q, qn, "LIBRARY")
               || vms_ci_abbrev(q, qn, "INCLUDE") || vms_ci_abbrev(q, qn, "SELECTIVE_SEARCH")) {
        /* a file qualifier given with no file: the VMS grammar attaches it to the
         * next file, which is not how this front end reads a command line */
        vms_fatal_qual("IVQUAL", "a file qualifier must follow the file it applies to");
    } else {
        fprintf(stderr, "%%LINK-F-IVQUAL, unrecognized qualifier\n \\%.*s\\\n", (int)qn, q);
        exit(1);
    }
    (void)val;
}

/* A GNU-style command always names its output with -o; a command without one is
 * the VMS command (whose output name defaults from the first input). */
static int vms_syntax(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "-o") == 0) return 0;
    return 1;
}

/* Translate a VMS-syntax command into the vector main() has always parsed. */
static char **vms_translate(int argc, char **argv, int *new_argc)
{
    struct vms_state st;
    memset(&st, 0, sizeof st);
    vms_push(&st.out, argv[0]);
    int noinform = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-') {                       /* GNU-style pass-through */
            if (strcmp(a, "--transfer") == 0) st.have_transfer = 1;
            vms_push(&st.out, a);
            if (i + 1 < argc && (strcmp(a, "--transfer") == 0 || strcmp(a, "--base") == 0 ||
                                 strcmp(a, "--use") == 0 || strcmp(a, "--library") == 0 || strcmp(a, "-o") == 0))
                vms_push(&st.out, argv[++i]);
            continue;
        }
        /* split on top-level commas and `+` into file/qualifier segments */
        const char *p = a;
        while (*p) {
            const char *e = p; int depth = 0;
            while (*e && (depth || (*e != ',' && *e != '+'))) {
                if (*e == '(') depth++;
                else if (*e == ')' && depth) depth--;
                e++;
            }
            char *seg = vms_dup(p, (size_t)(e - p));
            char *slash = (seg[0] == '/') ? seg : strchr(seg, '/');
            int kind = VK_OBJ;
            char *file = seg;
            if (slash) {
                char *quals = slash;
                char sv = *slash; *slash = 0;
                (void)sv;
                for (char *t = quals + 1; t && *t;) {
                    char *nx = strchr(t, '/');
                    size_t tn = nx ? (size_t)(nx - t) : strlen(t);
                    char *eqv = memchr(t, '=', tn);
                    size_t qn = eqv ? (size_t)(eqv - t) : tn;
                    char *val = eqv ? vms_dup(eqv + 1, tn - qn - 1) : NULL;
                    if (val && val[0] == '(') {  /* (a,b): not meaningful for the qualifiers handled here */ }
                    if (file[0] && qn >= 3 && vms_ci_abbrev(t, qn, "OPTIONS")) kind = VK_OPT;
                    else if (file[0] && qn >= 3 && vms_ci_abbrev(t, qn, "LIBRARY")) kind = VK_LIB;
                    else if (file[0] && qn >= 3 && vms_ci_abbrev(t, qn, "SHAREABLE")) kind = VK_SHR;
                    else if (file[0] && qn >= 3 && (vms_ci_abbrev(t, qn, "INCLUDE") || vms_ci_abbrev(t, qn, "SELECTIVE_SEARCH")))
                        fprintf(stderr, "%%LINK-W-IGNORED, /%.*s on %s is not acted on by this linker\n", (int)qn, t, file);
                    else
                        vms_command_qualifier(&st, t, qn, val, &noinform);
                    t = nx ? nx + 1 : NULL;
                }
            }
            if (file[0]) vms_add_file(&st, file, kind, 0);
            p = *e ? e + 1 : e;
        }
    }
    if (!st.have_transfer) {
        /* The GCC port's startup module defines __main; the VMS object format
         * carries no transfer-address record for this producer, so name it. */
        vms_push(&st.out, "--transfer"); vms_push(&st.out, "__main");
    }
    if (!st.first_input) vms_fatal_qual("NOFILES", "no input file specified");
    char *outname;
    if (st.exe) outname = vms_default_type(st.exe, ".exe");
    else {
        /* default: the first input's name, type .EXE, in the current directory */
        const char *f = st.first_input;
        const char *base = f;
        for (const char *q = f; *q; q++) if (*q == ']' || *q == ':') base = q + 1;
        const char *dot = strrchr(base, '.');
        size_t n = dot ? (size_t)(dot - base) : strlen(base);
        outname = malloc(n + 5);
        if (!outname) die("oom");
        memcpy(outname, base, n); strcpy(outname + n, ".exe");
    }
    vms_push(&st.out, "--executable");
    vms_push(&st.out, "-o");
    vms_push(&st.out, outname);
    g_noinform = noinform;
    *new_argc = st.out.n;
    return (char **)st.out.v;
}
