/*
 * mail_store.c - the OVMX MAIL store's WRITE side (rd vms-47fd): append a
 * message, append a read/deleted mark. Compiled into MAIL.EXE (SEND, READ,
 * DELETE) and MAIL_SERVER.EXE (DECnet MAIL-11 delivery) -- the two writers of
 * the one mail file vms_mail_notify.h describes.
 *
 * RMS ONLY. $OPEN (or, for a first message, $CREATE) the user's mail file,
 * $CONNECT positioned at end of file (RAB$M_EOF), one $PUT per record, $CLOSE.
 * Every service rides the executive ACP; any failure is returned as its real
 * RMS status and nothing is written anywhere else (INV-6). A message is
 * acknowledged to its sender only after its committing E record was $PUT and
 * the file $CLOSEd: the reader ignores a message without one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vms_mail_notify.h"
#include "rms/rms.h"
#include "rms/xab.h"
#include "sysuaf.h"
#include "vmsfs/device.h"

static const char *const g_mon[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                       "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

struct mstore {
    struct FAB    fab;
    struct RAB    rab;
    struct XABPRO pro;
    char          spec[600];
};

/* Open `user`'s mail file for append, creating it on first use. */
static uint32_t ms_open(struct mstore *m, const char *user)
{
    char raw[600];
    memset(m, 0, sizeof *m);
    if (mail_store_spec(user, raw, sizeof raw) != 0)
        return RMS$_FNF;
    /* Device-field logicals resolved the way rms_textfile does, so the ACP
     * $ASSIGNs a mounted unit. */
    if (vmsfs_resolve_filespec_device(raw, m->spec, sizeof m->spec) != SS$_NORMAL ||
        m->spec[0] == '\0')
        snprintf(m->spec, sizeof m->spec, "%s", raw);

    m->fab = cc$rms_fab;
    m->fab.fab$l_fna = m->spec;
    m->fab.fab$b_fns = (uint8_t)strlen(m->spec);
    m->fab.fab$b_org = FAB$C_SEQ;
    m->fab.fab$b_rfm = FAB$C_STMLF;
    m->fab.fab$b_fac = FAB$M_PUT | FAB$M_GET;

    uint32_t st = sys$open(&m->fab, 0, 0);
    if (!(st & 1)) {
        if (st != RMS$_FNF)
            return st;                 /* there but unreachable: never a 2nd file */
        /* The mail file belongs to its USER, whoever creates it: VMS
         * MAIL_SERVER creates a recipient's MAIL.MAI owned by the recipient,
         * so the recipient's own MAIL can later mark and delete. XABPRO's
         * owner UIC carries it; the ACP checks the creator may (SYSPRV). */
        sysuaf_record_t urec;
        if (sysuaf_lookup(user, &urec) == 0) {
            m->pro = cc$rms_xabpro;
            m->pro.xab$l_uic = ((uint32_t)urec.uic_group << 16) |
                               (urec.uic_member & 0xFFFFu);
            m->fab.fab$l_xab = (struct XABKEY *)&m->pro;  /* the FAB types its XAB chain head as XABKEY */
        }
        memset(&urec, 0, sizeof urec);     /* the record carries the hash */
        st = sys$create(&m->fab, 0, 0);
        m->fab.fab$l_xab = NULL;
        if (!(st & 1))
            return st;
    }
    m->rab = cc$rms_rab;
    m->rab.rab$l_fab = &m->fab;
    m->rab.rab$l_rop = RAB$M_EOF;
    st = sys$connect(&m->rab, 0, 0);
    if (!(st & 1)) {
        (void)sys$close(&m->fab, 0, 0);
        return st;
    }
    return RMS$_NORMAL;
}

static uint32_t ms_put(struct mstore *m, char tag, const char *id, const char *payload)
{
    char rec[MAIL_FIELD_MAX + MAIL_ID_LEN + 8];
    int n = payload ? snprintf(rec, sizeof rec, "%c|%s|%s", tag, id, payload)
                    : snprintf(rec, sizeof rec, "%c|%s", tag, id);
    if (n < 0 || (size_t)n >= sizeof rec)
        return RMS$_RTB;
    m->rab.rab$l_rbf = rec;
    m->rab.rab$w_rsz = (uint16_t)n;
    return sys$put(&m->rab, 0, 0);
}

static uint32_t ms_close(struct mstore *m)
{
    (void)sys$disconnect(&m->rab, 0, 0);
    return sys$close(&m->fab, 0, 0);
}

/* A message id nobody else holds: wall clock, nanoseconds, pid. */
static void new_id(char out[MAIL_ID_LEN + 1])
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t v = ((uint64_t)ts.tv_sec << 30) ^ (uint64_t)ts.tv_nsec ^
                 ((uint64_t)getpid() << 48);
    static unsigned seq;
    v += ++seq;
    snprintf(out, MAIL_ID_LEN + 1, "%016llX", (unsigned long long)v);
}

/* " 8-OCT-2026 06:37:49.01" without the leading pad: VMS absolute time. */
static void vms_now(char *buf, size_t sz)
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    snprintf(buf, sz, "%d-%s-%04d %02d:%02d:%02d.%02d", tm.tm_mday, g_mon[tm.tm_mon],
             1900 + tm.tm_year, tm.tm_hour, tm.tm_min, tm.tm_sec,
             (int)(ts.tv_nsec / 10000000));
}

/* A stored field must be one record: bounded, no record delimiter inside. */
static int field_ok(const char *s)
{
    if (!s) return 1;
    size_t n = strlen(s);
    if (n > MAIL_FIELD_MAX) return 0;
    return strpbrk(s, "\r\n") == NULL;
}

uint32_t mail_store_deliver(const char *recipient, const char *from,
                            const char *to, const char *cc, const char *subj,
                            const char *const *lines, unsigned nlines,
                            char *errtext, size_t errcap)
{
    if (errtext && errcap) errtext[0] = '\0';
    int ok = field_ok(to) && field_ok(cc) && field_ok(subj) && from &&
             strlen(from) < MAIL_FROM_MAX && !strpbrk(from, "\r\n");
    for (unsigned i = 0; ok && i < nlines; i++)
        ok = field_ok(lines[i]);
    if (!ok) {
        if (errtext) snprintf(errtext, errcap, "%%RMS-F-RTB, record too large for user's buffer");
        return RMS$_RTB;
    }

    struct mstore m;
    uint32_t st = ms_open(&m, recipient);
    if (!(st & 1)) {
        if (errtext)
            snprintf(errtext, errcap, "%%MAIL-E-OPENOUT, error opening mail file for %s",
                     recipient);
        return st;
    }
    char id[MAIL_ID_LEN + 1], date[MAIL_DATE_MAX];
    new_id(id);
    vms_now(date, sizeof date);
    st = ms_put(&m, 'H', id, date);
    if (st & 1) st = ms_put(&m, 'F', id, from);
    if (st & 1) st = ms_put(&m, 'T', id, to ? to : "");
    if (st & 1) st = ms_put(&m, 'C', id, cc ? cc : "");
    if (st & 1) st = ms_put(&m, 'S', id, subj ? subj : "");
    for (unsigned i = 0; (st & 1) && i < nlines; i++)
        st = ms_put(&m, 'B', id, lines[i]);
    if (st & 1) st = ms_put(&m, 'E', id, NULL);     /* the commit */
    uint32_t cst = ms_close(&m);
    if ((st & 1) && !(cst & 1)) st = cst;          /* not on disk until $CLOSE */
    if (!(st & 1) && errtext)
        snprintf(errtext, errcap, "%%MAIL-E-WRITEERR, error writing mail file for %s",
                 recipient);
    return (st & 1) ? SS$_NORMAL : st;
}

int mail_store_mark(const char *username, const char *id, char what)
{
    if (!id || strlen(id) != MAIL_ID_LEN || (what != 'R' && what != 'D'))
        return -1;
    struct mstore m;
    if (!(ms_open(&m, username) & 1))
        return -1;
    uint32_t st = ms_put(&m, what, id, NULL);
    uint32_t cst = ms_close(&m);
    return ((st & 1) && (cst & 1)) ? 0 : -1;
}
