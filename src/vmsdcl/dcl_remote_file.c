/*
 * dcl_remote_file.c - `$ DIRECTORY node::spec` and `$ TYPE node::spec` as
 * RMS clients (rd vms-0a7 slice S1; the layout and messages are rd vms-6a4's,
 * matched to a real VAX V7.3).
 *
 * RMS does the remote access: $PARSE and $SEARCH list the remote directory
 * through its FAL, $OPEN / $GET / $CLOSE read a remote file. Nothing here
 * knows DECnet. What is here is how VMS lays the answers out:
 *
 *  - DIRECTORY heads each directory with the node, volume and directory the
 *    remote gave (`Directory VAX2"SYSTEM password"::SYS$SYSDEVICE:[SYSMGR]`),
 *    lists the names in four 20-column fields, and totals each directory
 *    (`Total of 3 files.`), with a grand total over several.
 *  - TYPE of a wildcard heads each file with its full name between blank
 *    lines; a file it cannot open is %TYPE-W-OPENIN and the rest are typed.
 *  - A failure prints the RMS status, and -- for the RMS errors that carry
 *    the ACP's own status (DNF, ACC, FND) -- that status on the next line.
 *
 * Oracles: tests/lab/captures/decnet-fal-verbs-20261008/ (VAX<->VAX consoles)
 * and tests/lab/captures/vms-6a4-dir-type-copy-20261102/ (OVMX against a real
 * VAX); tests/vmsrms/test_rms_remote.c replays the VAX FAL's own replies and
 * compares this output with the VAX console byte for byte.
 */
#include "dcl/dcl_remote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rms/rms.h"
#include "ssdef.h"
#include "rmsdef.h"
#include "starlet.h"
#include "descrip.h"

#define RMS_FND 0x0001C02Au      /* RMS$_FND, catalog 0x0001C02A (not in rmsdef.h) */
#define DIR_COLS 4
#define DIR_CELL 20

/* ---- messages ---------------------------------------------------------- */

/* One continuation line, "-FAC-S-IDENT, text", from the message catalog. */
static void put_reason(uint32_t cond)
{
    char txt[300];
    uint16_t tl = 0;
    struct dsc$descriptor_s d;
    d.dsc$w_length = (uint16_t)(sizeof txt - 1);
    d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_S;
    d.dsc$a_pointer = txt;
    (void)sys$getmsg(cond, &tl, &d, 15, NULL);   /* NOMSG text when unknown */
    txt[tl < sizeof txt ? tl : sizeof txt - 1] = '\0';
    if (txt[0] == '%') txt[0] = '-';
    printf("%s\n", txt);
}

/* The RMS status, then -- for the RMS errors that carry the ACP status in
 * the STV -- that status. Observed against a real VAX FAL (rd vms-6a4): DNF
 * prints -RMS-E-DNF then -SYSTEM-W-NOSUCHFILE; FNF and PRV print the RMS
 * line alone; FND over a refused link prints the connect's failure. */
static void put_status(uint32_t sts, uint32_t stv)
{
    put_reason(sts);
    if ((sts == RMS$_DNF || sts == RMS$_ACC || sts == RMS_FND) && stv)
        put_reason(stv);
}

/* ---- spec defaults ------------------------------------------------------ */

/* DIRECTORY lists name.*;* (and *.*;* for a bare directory); TYPE reads the
 * highest version (an empty ";"). Only the part after the node's "::". */
static int default_spec(const char *spec, int dir, char *out, size_t cap)
{
    const char *f = spec, *p;
    int q = 0;
    for (p = spec; *p; p++) {
        if (*p == '"') q = !q;
        else if (!q && p[0] == ':' && p[1] == ':') f = p + 2;
    }
    const char *name = f;
    for (p = f; *p; p++)
        if (*p == ']' || *p == '>' || *p == ':') name = p + 1;
    int has_ver = strchr(name, ';') != NULL, has_dot = strchr(name, '.') != NULL;
    int n;
    if (dir)
        n = !name[0] ? snprintf(out, cap, "%s*.*;*", spec)
                     : snprintf(out, cap, "%s%s%s", spec, has_dot ? "" : ".*", has_ver ? "" : ";*");
    else
        n = snprintf(out, cap, "%s%s", spec, has_ver ? "" : ";");
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return -1; }
    return 0;
}

static int is_wild(const char *s)
{
    return strpbrk(s, "*%") != NULL || strstr(s, "...") != NULL;
}

/* ---- the $SEARCH ---------------------------------------------------------- */

struct found {
    char **rsa; size_t n, cap;
    uint32_t end_sts, end_stv;          /* RMS$_NMF, or the search's failure */
    char esa[NAM$C_MAXESS + 1];         /* the expanded string at the end    */
};

static void found_free(struct found *f)
{
    for (size_t i = 0; i < f->n; i++) free(f->rsa[i]);
    free(f->rsa);
    memset(f, 0, sizeof *f);
}

static uint32_t search_all(const char *spec, struct found *f)
{
    memset(f, 0, sizeof *f);
    struct FAB fab = cc$rms_fab;
    struct NAM nam = cc$rms_nam;
    char esa[NAM$C_MAXESS + 1], rsa[NAM$C_MAXRSS + 1];
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$l_nam = &nam;
    nam.nam$l_esa = esa; nam.nam$b_ess = NAM$C_MAXESS;
    nam.nam$l_rsa = rsa; nam.nam$b_rss = NAM$C_MAXRSS;
    snprintf(f->esa, sizeof f->esa, "%s", spec);
    uint32_t st = sys$parse(&fab, 0, 0);
    if (!(st & 1)) { f->end_sts = st; f->end_stv = fab.fab$l_stv; return st; }
    for (;;) {
        st = sys$search(&fab, 0, 0);
        if (!(st & 1)) break;
        if (f->n == f->cap) {
            size_t nc = f->cap ? f->cap * 2 : 32;
            char **g = realloc(f->rsa, nc * sizeof *g);
            if (!g) { st = RMS$_DME; break; }
            f->rsa = g; f->cap = nc;
        }
        char *r = malloc((size_t)nam.nam$b_rsl + 1);
        if (!r) { st = RMS$_DME; break; }
        memcpy(r, rsa, nam.nam$b_rsl);
        r[nam.nam$b_rsl] = '\0';
        f->rsa[f->n++] = r;
    }
    f->end_sts = st;
    f->end_stv = fab.fab$l_stv;
    if (nam.nam$b_esl) {
        memcpy(f->esa, esa, nam.nam$b_esl);
        f->esa[nam.nam$b_esl] = '\0';
    }
    rms_search_end(&nam);
    return st;
}

/* Length of the node + device + directory part of a resultant name. */
static size_t dir_part(const char *r)
{
    size_t at = 0;
    int q = 0;
    for (size_t i = 0; r[i]; i++) {
        if (r[i] == '"') { q = !q; continue; }
        if (!q && (r[i] == ']' || r[i] == '>' || r[i] == ':')) at = i + 1;
    }
    return at;
}

/* ---- DIRECTORY --------------------------------------------------------------- */

/*
 * One directory's names, in the row layout a live OpenVMS VAX V7.3 prints
 * (rd vms-c18; a 362-name listing with names of 10 to 28 characters). Names
 * start at columns 0, 20, 40 and 60.
 *  - A name of up to 19 characters fills a 19-character field. In columns
 *    0-40 a blank follows it ("BRKP.TXT;1" ends a 3-name row at 60); in
 *    column 60 the row ends at 79 ("NETSERVER.LOG;7    ").
 *  - A longer name is printed whole, even past column 80. The next name
 *    starts at the first column stop after it with at least one blank
 *    between; the blanks are written only when a name follows on the same
 *    row, so a row a long name ends has none.
 */
static size_t dir_print_group(const struct found *f, size_t from, size_t to)
{
    size_t pl = dir_part(f->rsa[from]);
    printf("\nDirectory %.*s\n\n", (int)pl, f->rsa[from]);
    int col = 0, pend = 0;
    for (size_t i = from; i < to; i++) {
        const char *nm = f->rsa[i] + pl;
        int len = (int)strlen(nm);
        if (col > 0 && pend > 0) printf("%*s", pend, "");
        pend = 0;
        printf("%s", nm);
        if (len <= DIR_CELL - 1) {
            printf("%*s", DIR_CELL - 1 - len, "");
            if (col == (DIR_COLS - 1) * DIR_CELL) {
                putchar('\n');
                col = 0;
            } else {
                putchar(' ');
                col += DIR_CELL;
            }
        } else {
            int end = col + len;
            int next = (end / DIR_CELL + 1) * DIR_CELL;
            if (next >= DIR_COLS * DIR_CELL) {
                putchar('\n');
                col = 0;
            } else {
                pend = next - end;
                col = next;
            }
        }
    }
    if (col > 0) putchar('\n');
    size_t n = to - from;
    printf("\nTotal of %zu file%s.\n", n, n == 1 ? "" : "s");
    return n;
}

uint32_t dcl_remote_directory(const char *spec)
{
    char wire[NAM$C_MAXESS + 1];
    if (!spec || default_spec(spec, 1, wire, sizeof wire) != 0) {
        {
            printf("%%DIRECT-E-OPENIN, error opening %s as input\n", spec ? spec : "");
            put_status(RMS$_SYN, 0);
        }
        return RMS$_SYN;
    }
    struct found f;
    (void)search_all(wire, &f);

    size_t groups = 0, total = 0;
    for (size_t i = 0; i < f.n; ) {
        size_t pl = dir_part(f.rsa[i]), j = i + 1;
        while (j < f.n && dir_part(f.rsa[j]) == pl && !strncmp(f.rsa[j], f.rsa[i], pl)) j++;
        total += dir_print_group(&f, i, j);
        groups++;
        i = j;
    }
    if (groups > 1)
        printf("\nGrand total of %zu directories, %zu file%s.\n",
                groups, total, total == 1 ? "" : "s");

    uint32_t ret = RMS$_NORMAL;
    if (f.end_sts != RMS$_NMF) {
        if (f.n == 0 && f.end_sts == RMS$_FNF) {
            printf("%%DIRECT-W-NOFILES, no files found\n");
            ret = RMS$_FNF & ~7u;     /* at warning severity: $STATUS %X10018290 on the VAX */
        } else {
            /* A refused link or search: the live VAX prints %DIRECT-E-OPENIN
             * on what the remote had expanded, then why. */
            printf("%%DIRECT-E-OPENIN, error opening %s as input\n", f.esa);
            put_status(f.end_sts, f.end_stv);
            ret = (f.end_sts == RMS_FND && f.end_stv) ? f.end_stv : f.end_sts;
        }
    } else if (f.n == 0) {
        printf("%%DIRECT-W-NOFILES, no files found\n");
        ret = RMS$_FNF & ~7u;
    }
    fflush(stdout);
    found_free(&f);
    return ret;
}

/* ---- TYPE --------------------------------------------------------------------- */

uint32_t dcl_remote_type(const char *spec)
{
    char wire[NAM$C_MAXESS + 1];
    if (!spec || default_spec(spec, 0, wire, sizeof wire) != 0) {
        {
            printf("%%TYPE-W-SEARCHFAIL, error searching for %s\n", spec ? spec : "");
            put_status(RMS$_SYN, 0);
        }
        return RMS$_SYN;
    }
    struct found f;
    (void)search_all(wire, &f);
    if (f.n == 0) {
        uint32_t sts = f.end_sts == RMS$_NMF ? RMS$_FNF : f.end_sts;
        printf("%%TYPE-W-SEARCHFAIL, error searching for %s\n", f.esa);
        put_status(sts, f.end_stv);
        uint32_t ret = (sts == RMS_FND && f.end_stv) ? f.end_stv : sts;
        fflush(stdout);
        found_free(&f);
        return ret;
    }

    int wild = is_wild(spec) || f.n > 1;
    uint32_t ret = RMS$_NORMAL;
    static char rec[65536];
    for (size_t i = 0; i < f.n; i++) {
        const char *what = f.rsa[i];
        struct FAB fab = cc$rms_fab;
        fab.fab$l_fna = (char *)what;
        fab.fab$b_fns = (uint8_t)strlen(what);
        fab.fab$b_fac = FAB$M_GET;
        fab.fab$b_shr = FAB$M_SHRGET;
        uint32_t st = sys$open(&fab, 0, 0);
        if (!(st & 1)) {
            printf("%%TYPE-W-OPENIN, error opening %s as input\n", what);
            put_status(st, fab.fab$l_stv);
            if (st == RMS_FND) {               /* the link itself failed: stop */
                ret = fab.fab$l_stv ? fab.fab$l_stv : st;
                break;
            }
            ret = st;
            continue;
        }
        struct RAB rab = cc$rms_rab;
        rab.rab$l_fab = &fab;
        rab.rab$l_ubf = rec;
        rab.rab$w_usz = (uint16_t)(sizeof rec - 1);
        (void)sys$connect(&rab, 0, 0);
        /* The wildcard header, as the VAX prints it: a line holding one
         * blank, the file's full name, another one-blank line. */
        if (wild) printf(" \n%s\n \n", what);
        int broke = 0;
        for (;;) {
            st = sys$get(&rab, 0, 0);
            if (st == RMS$_EOF) break;
            if (!(st & 1)) {
                printf("%%TYPE-W-READERR, error reading %s\n", what);
                put_status(st, rab.rab$l_stv);
                ret = st;
                broke = (rab.rab$l_stv == SS$_ABORT);   /* the link broke: stop */
                break;
            }
            if (rab.rab$w_rsz) printf("%.*s", (int)rab.rab$w_rsz, rab.rab$l_rbf);
            putchar('\n');
        }
        (void)sys$close(&fab, 0, 0);
        if (broke) break;
    }
    fflush(stdout);
    found_free(&f);
    return ret;
}

/* ---- COPY --------------------------------------------------------------------- */

/* The spec as a message shows it: a password in the access string is the
 * word "password", as RMS's expanded and resultant strings give it. */
static void mask_spec(const char *in, char *out, size_t cap)
{
    const char *q1 = strchr(in, '"');
    const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    if (!q1 || !q2 || !strstr(q2, "::")) { snprintf(out, cap, "%s", in); return; }
    const char *p = q1 + 1, *u0, *u1;
    while (p < q2 && *p == ' ') p++;
    u0 = p;
    while (p < q2 && *p != ' ') p++;
    u1 = p;
    while (p < q2 && *p == ' ') p++;
    int has_pw = p < q2;
    snprintf(out, cap, "%.*s\"%.*s%s\"%s", (int)(q1 - in), in, (int)(u1 - u0), u0,
             has_pw ? " password" : "", q2 + 1);
}

/* The output name for one input: a directory (or bare node) output takes the
 * input's name.type; a "*" field in the output takes the input's field. */
static void out_for(const char *dst, const char *src_rsa, char *out, size_t cap)
{
    const char *snt = src_rsa + dir_part(src_rsa);
    char sname[256], stype[256] = "";
    snprintf(sname, sizeof sname, "%s", snt);
    char *semi = strchr(sname, ';');
    if (semi) *semi = '\0';
    char *dot = strchr(sname, '.');
    if (dot) { snprintf(stype, sizeof stype, "%s", dot + 1); *dot = '\0'; }

    size_t dp = dir_part(dst);
    const char *dnt = dst + dp;
    if (!dnt[0]) {
        snprintf(out, cap, "%s%s%s%s", dst, sname, stype[0] ? "." : "", stype);
        return;
    }
    if (!strchr(dnt, '*')) { snprintf(out, cap, "%s", dst); return; }
    char dname[256], dtype[256] = "", dver[64] = "";
    snprintf(dname, sizeof dname, "%s", dnt);
    char *dsemi = strchr(dname, ';');
    if (dsemi) { snprintf(dver, sizeof dver, "%s", dsemi); *dsemi = '\0'; }
    char *ddot = strchr(dname, '.');
    if (ddot) { snprintf(dtype, sizeof dtype, "%s", ddot + 1); *ddot = '\0'; }
    const char *n = strcmp(dname, "*") ? dname : sname;
    const char *t = ddot ? (strcmp(dtype, "*") ? dtype : stype) : stype;
    snprintf(out, cap, "%.*s%s%s%s%s", (int)dp, dst, n, t[0] ? "." : "", t, dver);
}

/* One file, record by record: $OPEN + $GET on the input, $CREATE + $PUT on
 * the output, which takes the input's record structure. */
static uint32_t copy_one(const char *src, const char *dst, int log)
{
    static char rec[65536];
    char dshow[1100], rshow[NAM$C_MAXRSS + 1];
    mask_spec(dst, dshow, sizeof dshow);

    struct FAB in = cc$rms_fab;
    in.fab$l_fna = (char *)src;
    in.fab$b_fns = (uint8_t)strlen(src);
    in.fab$b_fac = FAB$M_GET;
    in.fab$b_shr = FAB$M_SHRGET;
    struct rms_fileattr fa;
    int local_attr = 0;
    if (!strstr(src, "::") && rms_file_attr(src, &fa) == RMS$_NORMAL &&
        fa.rfm >= FAB$C_FIX && fa.rfm <= FAB$C_STMCR) {
        /* RMS over the ACP takes the record format from the caller (as
         * dcl_rms_read_open does): the header's, read first. */
        in.fab$b_rfm = fa.rfm;
        if (fa.mrs) in.fab$w_mrs = fa.mrs;
        local_attr = 1;
    }
    uint32_t st = sys$open(&in, 0, 0);
    if (!(st & 1)) {
        printf("%%COPY-E-OPENIN, error opening %s as input\n", src);
        put_status(st, in.fab$l_stv);
        return st;
    }

    struct FAB o = cc$rms_fab;
    struct NAM onam = cc$rms_nam;
    onam.nam$l_rsa = rshow;
    onam.nam$b_rss = NAM$C_MAXRSS;
    o.fab$l_nam = &onam;
    o.fab$l_fna = (char *)dst;
    o.fab$b_fns = (uint8_t)strlen(dst);
    o.fab$b_fac = FAB$M_PUT;
    o.fab$b_org = FAB$C_SEQ;
    o.fab$b_rfm = local_attr ? fa.rfm : in.fab$b_rfm;
    o.fab$b_rat = local_attr ? (uint8_t)fa.rat : in.fab$b_rat;
    o.fab$w_mrs = local_attr ? fa.mrs : in.fab$w_mrs;
    o.fab$b_fsz = local_attr ? fa.vfcsize : in.fab$b_fsz;
    st = sys$create(&o, 0, 0);
    if (!(st & 1)) {
        printf("%%COPY-E-OPENOUT, error opening %s as output\n", dshow);
        put_status(st, o.fab$l_stv);
        printf("%%COPY-W-NOTCOPIED, %s not copied\n", src);
        (void)sys$close(&in, 0, 0);
        return st;
    }
    if (onam.nam$b_rsl) {
        rshow[onam.nam$b_rsl] = '\0';
        if (!strstr(rshow, "::")) snprintf(dshow, sizeof dshow, "%s", rshow);
    }

    struct RAB ir = cc$rms_rab, orb = cc$rms_rab;
    ir.rab$l_fab = &in;
    ir.rab$l_ubf = rec;
    ir.rab$w_usz = (uint16_t)(sizeof rec - 1);
    orb.rab$l_fab = &o;
    (void)sys$connect(&ir, 0, 0);
    (void)sys$connect(&orb, 0, 0);
    uint32_t ret = RMS$_NORMAL;
    for (;;) {
        st = sys$get(&ir, 0, 0);
        if (st == RMS$_EOF) break;
        if (!(st & 1)) {
            printf("%%COPY-E-READERR, error reading %s\n", src);
            put_status(st, ir.rab$l_stv);
            ret = st;
            break;
        }
        orb.rab$l_rbf = ir.rab$l_rbf;
        orb.rab$w_rsz = ir.rab$w_rsz;
        st = sys$put(&orb, 0, 0);
        if (!(st & 1)) {
            printf("%%COPY-E-WRITEERR, error writing %s\n", dshow);
            put_status(st, orb.rab$l_stv);
            ret = st;
            break;
        }
    }
    st = sys$close(&o, 0, 0);
    if ((ret & 1) && !(st & 1)) {
        printf("%%COPY-E-CLOSEOUT, error closing %s as output\n", dshow);
        put_status(st, o.fab$l_stv);
        ret = st;
    }
    (void)sys$close(&in, 0, 0);
    if ((ret & 1) && log)
        printf("%%COPY-S-COPIED, %s copied to %s\n", src, dshow);
    return ret;
}

uint32_t dcl_remote_copy(const char *sx, const char *dx, int log)
{
    if (!sx || !dx) return RMS$_SYN;
    struct found f;
    (void)search_all(sx, &f);
    if (f.n == 0) {
        uint32_t sts = f.end_sts == RMS$_NMF ? RMS$_FNF : f.end_sts;
        printf("%%COPY-E-OPENIN, error opening %s as input\n", f.esa);
        put_status(sts, f.end_stv);
        uint32_t ret = (sts == RMS_FND && f.end_stv) ? f.end_stv : sts;
        fflush(stdout);
        found_free(&f);
        return ret;
    }
    uint32_t ret = RMS$_NORMAL;
    size_t copied = 0;
    for (size_t i = 0; i < f.n; i++) {
        char o[1200];
        out_for(dx, f.rsa[i], o, sizeof o);
        uint32_t st = copy_one(f.rsa[i], o, log);
        if (st & 1) copied++;
        else ret = st;
    }
    if (copied > 1)
        printf("%%COPY-S-NEWFILES, %zu files created\n", copied);
    fflush(stdout);
    found_free(&f);
    return ret;
}
