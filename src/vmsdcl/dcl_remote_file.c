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
/* What DCL leaves in $STATUS (without STS$M_INHIB_MSG, which the caller adds)
 * after a failure the live VAX V7.3 reported. DIRECTORY leaves the RMS status
 * of the failed search -- not the STV under it -- so a wrong password is
 * RMS$_FND (%X1001C02A) and an empty listing RMS$_FNF at warning severity
 * (%X10018290). TYPE leaves its own message: %TYPE-W-OPENIN is %X10951098 and
 * %TYPE-W-SEARCHFAIL %X10951238, whatever RMS error lies under them.
 * (tests/lab/captures/vms-0a7-s1-20261102/events.log, VAXC L2c/L2d/L5a/L5b.) */
#define TYPE_W_OPENIN     0x00951098u
#define TYPE_W_SEARCHFAIL 0x00951238u
#define DIR_COLS 4
#define DIR_CELL 20

/* ---- DIRECTORY/FULL texts, shared with the local DIRECTORY/FULL ---------- */

/* Per-category access bits within a VMS protection nibble (set = DENIED). */
#define FMT_PROT_R 0x01
#define FMT_PROT_W 0x02
#define FMT_PROT_E 0x04
#define FMT_PROT_D 0x08

/* "Record format:" text for a FAT record format (vms-a44), as OpenVMS VAX V7.3
 * prints it (vax73-dirfull-recfmt.txt):
 *   FIX  "Fixed length 20 byte records"
 *   VAR  "Variable length, maximum 0 bytes, longest 32 bytes"
 *   VFC  "VFC, 2 byte header, maximum 0 bytes, longest 32 bytes"
 *   STM / STMLF / STMCR  "Stream" / "Stream_LF" / "Stream_CR", maximum, longest
 *   UDF  "Undefined, maximum 0 bytes, longest 0 bytes" */
void dcl_fmt_recfmt(uint8_t rfm, uint16_t mrs, uint16_t lrl, uint8_t fsz,
                              char *out, size_t cap)
{
    const char *nm;
    switch (rfm) {
    case FAB$C_FIX:
        snprintf(out, cap, "Fixed length %u byte records", (unsigned)mrs);
        return;
    case FAB$C_VFC:
        snprintf(out, cap, "VFC, %u byte header, maximum %u bytes, longest %u bytes",
                 (unsigned)fsz, (unsigned)mrs, (unsigned)lrl);
        return;
    case FAB$C_VAR:   nm = "Variable length"; break;
    case FAB$C_STM:   nm = "Stream";          break;
    case FAB$C_STMLF: nm = "Stream_LF";       break;
    case FAB$C_STMCR: nm = "Stream_CR";       break;
    default:          nm = "Undefined";       break;
    }
    snprintf(out, cap, "%s, maximum %u bytes, longest %u bytes", nm,
             (unsigned)mrs, (unsigned)lrl);
}

/* "Record attributes:" text (vms-a44): the carriage-control attribute (Fortran /
 * Carriage return / Print file carriage control), then ", Non-spanned" for BLK;
 * "None" when neither is set. */
void dcl_fmt_recattr(uint8_t rat, char *out, size_t cap)
{
    const char *cc = (rat & FAB$M_FTN) ? "Fortran carriage control"
                   : (rat & FAB$M_CR)  ? "Carriage return carriage control"
                   : (rat & FAB$M_PRN) ? "Print file carriage control" : NULL;
    if (cc && (rat & FAB$M_BLK))      snprintf(out, cap, "%s, Non-spanned", cc);
    else if (cc)                      snprintf(out, cap, "%s", cc);
    else if (rat & FAB$M_BLK)         snprintf(out, cap, "Non-spanned");
    else                              snprintf(out, cap, "None");
}

/* A VMS protection word in the long form DIRECTORY/FULL prints:
 * "System:RWED, Owner:RWED, Group:RE, World:". A clear bit allows the access. */
void dcl_fmt_prot_full(uint16_t prot, char *buf, size_t bufsize)
{
    static const char *cats[]  = { "System", "Owner", "Group", "World" };
    static const int   shift[] = { 0, 4, 8, 12 };
    size_t off = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t bits = (uint8_t)((prot >> shift[i]) & 0x0F);
        char acc[5]; int a = 0;
        if (!(bits & FMT_PROT_R)) acc[a++] = 'R';
        if (!(bits & FMT_PROT_W)) acc[a++] = 'W';
        if (!(bits & FMT_PROT_E)) acc[a++] = 'E';
        if (!(bits & FMT_PROT_D)) acc[a++] = 'D';
        acc[a] = '\0';
        int n = snprintf(buf + off, bufsize - off, "%s%s:%s",
                         (i > 0) ? ", " : "", cats[i], acc);
        if (n < 0 || (size_t)n >= bufsize - off) break;
        off += (size_t)n;
    }
}

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
            ret = f.end_sts;          /* the RMS status, as the VAX leaves it (%X1001C02A) */
        }
    } else if (f.n == 0) {
        printf("%%DIRECT-W-NOFILES, no files found\n");
        ret = RMS$_FNF & ~7u;
    }
    fflush(stdout);
    found_free(&f);
    return ret;
}


/* ---- DIRECTORY/FULL ----------------------------------------------------------- */

/* The text of a condition alone ("insufficient privilege or object protection
 * violation"), as the VAX prints it after a name it could not describe. */
static void msg_text(uint32_t cond, char *out, size_t cap)
{
    uint16_t tl = 0;
    struct dsc$descriptor_s d;
    d.dsc$w_length = (uint16_t)(cap - 1);
    d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_S;
    d.dsc$a_pointer = out;
    (void)sys$getmsg(cond, &tl, &d, 1, NULL);
    out[tl < cap ? tl : cap - 1] = '\0';
}

/* A VMS local-time quadword (100-ns ticks since 17-NOV-1858) as
 * "dd-MMM-yyyy hh:mm:ss.cc". The time is already local, so no zone is applied. */
static void full_date(const uint8_t q[8], char *out, size_t cap)
{
    static const char *const mon[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                       "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    uint64_t t = 0;
    for (int i = 7; i >= 0; i--) t = (t << 8) | q[i];
    uint64_t secs = t / 10000000ull;
    unsigned cs = (unsigned)((t % 10000000ull) / 100000ull);
    int64_t days = (int64_t)(secs / 86400ull) - 40587;       /* day 0 of 1858 -> since 1970 */
    unsigned sod = (unsigned)(secs % 86400ull);
    /* days since 1970-01-01 -> civil date (proleptic Gregorian) */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    snprintf(out, cap, "%2u-%s-%04lld %02u:%02u:%02u.%02u", d, mon[m - 1], (long long)y,
             sod / 3600, sod / 60 % 60, sod % 60, cs);
}

/* The name.type;ver of a resultant: past the node, volume and directory. */
static const char *name_part(const char *r) { return r + dir_part(r); }

/* One served file, in the layout a VAX V7.3 prints for a remote file
 * (tests/lab/captures/decnet-fal-verbs-20261008/vax-to-vax-sys-login/
 * falverbs-v2v-vax1-console.txt). What DAP cannot carry is what the VAX prints
 * for it over the network: no File ID, the shelving and caching states
 * "(remote)", a global buffer count and version limit of 0. */
static void full_entry(const struct rms_remote_fent *e, long *used, long *alloc)
{
    const struct rms_fileattr *a = &e->fa;
    const char *nm = name_part(e->name);
    int len = (int)strlen(nm);
    printf("%s%*sFile ID:  None\n", nm, len < 30 ? 30 - len : 1, "");
    long u = 0, al = 0;
    if (e->have_attr) {
        u = a->efblk ? (a->ffbyte ? (long)a->efblk : (long)a->efblk - 1) : 0;
        al = (long)a->hiblk;
        char sz[40];
        snprintf(sz, sizeof sz, "%ld/%ld", u, al);
        printf("Size:            %-13s", sz);
        if (e->have_owner) printf("Owner:    %s", e->owner);
        putchar('\n');
    } else if (e->have_owner) {
        printf("Owner:    %s\n", e->owner);
    }
    char dt[40];
    if (e->have_cre) { full_date(a->credate, dt, sizeof dt); printf("Created:   %s\n", dt); }
    else             printf("Created:   <None specified>\n");
    if (e->have_rev) {
        full_date(a->revdate, dt, sizeof dt);
        printf("Revised:   %s (%u)\n", dt, (unsigned)a->revision);
    } else {
        printf("Revised:   <None specified>\n");
    }
    printf("Expires:   <None specified>\n");
    printf("Backup:    <No backup recorded>\n");
    printf("Effective: <None specified>\n");
    printf("Recording: <None specified>\n");
    if (e->have_attr) {
        const char *org = a->org == FAB$C_REL ? "Relative" : a->org == FAB$C_IDX ? "Indexed"
                        : a->org == FAB$C_SEQ ? "Sequential" : "Hashed";
        printf("File organization:  %s\n", org);
    }
    printf("Shelved state:      (remote)\n");
    printf("Caching attribute:  (remote)\n");
    if (e->have_attr) {
        char fmt[96], attr[96];
        printf("File attributes:    Allocation: %ld, Extend: %u, Global buffer count: 0, "
               "Version limit: 0\n", al, (unsigned)a->defext);
        dcl_fmt_recfmt(a->rfm, a->mrs, a->lrl, a->vfcsize, fmt, sizeof fmt);
        dcl_fmt_recattr(a->rat, attr, sizeof attr);
        printf("Record format:      %s\n", fmt);
        printf("Record attributes:  %s\n", attr);
    }
    printf("RMS attributes:     None\n");
    printf("Journaling enabled: None\n");
    if (e->have_prot) {
        char pb[80];
        dcl_fmt_prot_full(a->fileprot, pb, sizeof pb);
        printf("File protection:    %s\n", pb);
    }
    printf("Access Cntrl List:  None\n");
    printf("Client attributes:  None\n");
    *used += u;
    *alloc += al;
}

uint32_t dcl_remote_directory_full(const char *spec)
{
    char wire[NAM$C_MAXESS + 1];
    if (!spec || default_spec(spec, 1, wire, sizeof wire) != 0) {
        printf("%%DIRECT-E-OPENIN, error opening %s as input\n", spec ? spec : "");
        put_status(RMS$_SYN, 0);
        return RMS$_SYN;
    }
    struct rms_remote_full f;
    uint32_t st = rms_remote_dirlist_full(wire, &f);
    if (!(st & 1)) {
        printf("%%DIRECT-E-OPENIN, error opening %s as input\n", spec);
        put_status(st, 0);
        fflush(stdout);
        return st;
    }
    size_t groups = 0, total = 0;
    long gused = 0, galloc = 0;
    for (size_t i = 0; i < f.n; ) {
        size_t pl = dir_part(f.e[i].name), j = i + 1;
        while (j < f.n && dir_part(f.e[j].name) == pl && !strncmp(f.e[j].name, f.e[i].name, pl)) j++;
        printf("\nDirectory %.*s\n\n", (int)pl, f.e[i].name);
        long used = 0, alloc = 0;
        int sep = 0;                       /* a blank line is owed after a full entry */
        for (size_t k = i; k < j; k++) {
            if (sep) putchar('\n');
            sep = 0;
            if (f.e[k].sts) {
                char tx[300];
                /* the VAX words the refusal with the system status under it (PRV over
                 * SS$_NOPRIV: "insufficient privilege or object protection
                 * violation"), else with the RMS one */
                msg_text(f.e[k].stv ? f.e[k].stv : f.e[k].sts, tx, sizeof tx);
                const char *nm = name_part(f.e[k].name);
                printf("%-19s %s\n", nm, tx);
            } else {
                full_entry(&f.e[k], &used, &alloc);
                sep = 1;
            }
        }
        size_t n = j - i;
        printf("\nTotal of %zu file%s, %ld/%ld block%s.\n", n, n == 1 ? "" : "s", used, alloc,
               used == 1 ? "" : "s");
        total += n; gused += used; galloc += alloc;
        groups++;
        i = j;
    }
    if (groups > 1)
        printf("\nGrand total of %zu directories, %zu file%s, %ld/%ld block%s.\n", groups, total,
               total == 1 ? "" : "s", gused, galloc, gused == 1 ? "" : "s");

    uint32_t ret = RMS$_NORMAL;
    if (f.end_sts != RMS$_NMF) {
        if (f.n == 0 && f.end_sts == RMS$_FNF) {
            printf("%%DIRECT-W-NOFILES, no files found\n");
            ret = RMS$_FNF & ~7u;
        } else {
            printf("%%DIRECT-E-OPENIN, error opening %s as input\n", f.end_what);
            put_status(f.end_sts, f.end_stv);
            ret = f.end_sts;
        }
    } else if (f.n == 0) {
        printf("%%DIRECT-W-NOFILES, no files found\n");
        ret = RMS$_FNF & ~7u;
    }
    fflush(stdout);
    rms_remote_full_free(&f);
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
        return TYPE_W_SEARCHFAIL;
    }
    struct found f;
    (void)search_all(wire, &f);
    if (f.n == 0) {
        uint32_t sts = f.end_sts == RMS$_NMF ? RMS$_FNF : f.end_sts;
        printf("%%TYPE-W-SEARCHFAIL, error searching for %s\n", f.esa);
        put_status(sts, f.end_stv);
        fflush(stdout);
        found_free(&f);
        return TYPE_W_SEARCHFAIL;
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
            ret = TYPE_W_OPENIN;
            if (st == RMS_FND) break;          /* the link itself failed: stop */
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
            /* A VFC record's fixed control (RHB 0: not asked for) is not
             * part of the record $GET returns; TYPE shows the data. */
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
        if (fa.rfm == FAB$C_VFC && fa.vfcsize) in.fab$b_fsz = fa.vfcsize;
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

    /* A VFC record's fixed control area travels in the record header
     * buffer: $GET fills it, $PUT writes it (RAB$L_RHB), so the output
     * holds the input's control bytes and its records, as on VMS. */
    static char rhb[256];
    struct RAB ir = cc$rms_rab, orb = cc$rms_rab;
    ir.rab$l_fab = &in;
    ir.rab$l_ubf = rec;
    ir.rab$w_usz = (uint16_t)(sizeof rec - 1);
    ir.rab$l_rhb = rhb;
    orb.rab$l_fab = &o;
    orb.rab$l_rhb = rhb;
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
    if ((ret & 1) && log) {
        /* VMS ends the line with the output file's size: "(1 block)" /
         * "(N blocks)", the blocks in use from the file's own attributes
         * (EOF block, first free byte) -- what the ACP, or the remote FAL, says
         * of the file just written. Where they cannot be had, no count is
         * printed rather than a made-up one. */
        struct rms_fileattr fa;
        char cnt[40] = "";
        if (rms_file_attr(rshow[0] ? rshow : dst, &fa) == RMS$_NORMAL) {
            long used = fa.efblk ? (fa.ffbyte ? (long)fa.efblk : (long)fa.efblk - 1) : 0;
            snprintf(cnt, sizeof cnt, " (%ld block%s)", used, used == 1 ? "" : "s");
        }
        printf("%%COPY-S-COPIED, %s copied to %s%s\n", src, dshow, cnt);
    }
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
