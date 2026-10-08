/*
 * test_dnet_mail11.c - host replay of the real VAX V7.3 MAIL-11 capture through
 * the OVMX MAIL-11 receiver (rd vms-47fd).
 *
 * ORACLE-DRIVEN. argv[1] is tests/lab/captures/decnet-mail11-20261008/
 * mail11-wire.txt. The test parses it itself: for every session the real VAX2
 * CONFIRMED, it checks dnet_m11_connect_accept() turns the VAX1 Connect
 * Initiate's Session Control data into exactly the Connect Confirm data VAX2
 * sent, then feeds every VAX1 -> VAX2 data segment to the receiver and asserts
 * the receiver's replies are VAX2's segments, byte for byte (session 3: the
 * accepted message; session 4: NOSUCHUSER). Then the bounds: truncation,
 * oversize, too many lines/recipients, a store that fails (never acknowledged),
 * malformed connects, and a mutation fuzz over the oracle stream.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dnet_mail11.h"

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) { g_pass++; } else { g_fail++; \
    printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NOTE(...) do { printf("  "); printf(__VA_ARGS__); printf("\n"); } while (0)

/* ---- the capture ---------------------------------------------------------- */

#define MAXSEG 32
struct seg { uint8_t b[600]; size_t n; };
struct session {
    uint8_t rci[128]; size_t rcin;
    uint8_t cc[128];  size_t ccn;
    int confirmed;
    struct seg cli[MAXSEG]; unsigned ncli;     /* 1.1 -> 1.2 data */
    struct seg srv[MAXSEG]; unsigned nsrv;     /* 1.2 -> 1.1 data */
};
static struct session g_sess[8];
static unsigned g_nsess;

static size_t parse_hex(const char *p, uint8_t *out, size_t cap)
{
    size_t n = 0;
    unsigned v;
    int used;
    while (n < cap && sscanf(p, " %2x%n", &v, &used) == 1) {
        out[n++] = (uint8_t)v;
        p += used;
    }
    return n;
}

static int load_capture(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[4096];
    struct session *cur = NULL;
    while (fgets(line, sizeof line, fp)) {
        char dir[16], kind[16];
        double t;
        int off = 0;
        if (sscanf(line, " %lf %15s %15[A-Z]%n", &t, dir, kind, &off) != 3) continue;
        int to_srv = strcmp(dir, "1.1->1.2") == 0;
        const char *rest = line + off;
        if (!strcmp(kind, "RCI") && to_srv) {
            if (g_nsess >= 8) break;
            cur = &g_sess[g_nsess++];
            memset(cur, 0, sizeof *cur);
            const char *h = strchr(rest, ':');
            cur->rcin = h ? parse_hex(h + 1, cur->rci, sizeof cur->rci) : 0;
        } else if (!strcmp(kind, "CC") && cur && !to_srv) {
            const char *h = strchr(rest, ':');
            cur->ccn = h ? parse_hex(h + 1, cur->cc, sizeof cur->cc) : 0;
            cur->confirmed = 1;
        } else if (!strcmp(kind, "DATA") && cur) {
            const char *h = strchr(rest, ':');
            struct seg *s = to_srv ? (cur->ncli < MAXSEG ? &cur->cli[cur->ncli++] : NULL)
                                   : (cur->nsrv < MAXSEG ? &cur->srv[cur->nsrv++] : NULL);
            if (s && h) s->n = parse_hex(h + 1, s->b, sizeof s->b);
        }
    }
    fclose(fp);
    return 0;
}

/* ---- a recording store ---------------------------------------------------- */

struct store {
    int      calls;
    int      bad_state;       /* deliver called outside the end-of-message step */
    uint32_t result;          /* what deliver returns                         */
    char     user[16];
    char     from[96], to[600], cc[600], subj[600];
    unsigned nlines;
    char     line[4][600];
};

static struct dnet_m11_server g_s;      /* big: static */

static int check_rcpt(void *ctx, const char *user)
{
    (void)ctx;
    return strcmp(user, "SYSTEM") == 0 || strcmp(user, "GUEST") == 0;
}

static uint32_t deliver(void *ctx, const char *user, const struct dnet_m11_msg *m,
                        char *err, size_t errcap)
{
    struct store *st = ctx;
    st->calls++;
    if (g_s.state != DNET_M11_S_BODY) st->bad_state++;
    snprintf(st->user, sizeof st->user, "%s", user);
    snprintf(st->from, sizeof st->from, "%s", m->from);
    snprintf(st->to, sizeof st->to, "%s", m->to);
    snprintf(st->cc, sizeof st->cc, "%s", m->cc);
    snprintf(st->subj, sizeof st->subj, "%s", m->subj);
    st->nlines = m->nlines;
    for (unsigned i = 0; i < m->nlines && i < 4; i++)
        snprintf(st->line[i], sizeof st->line[i], "%s", m->lines[i]);
    if (st->result != DNET_M11_STS_SUCCESS)
        snprintf(err, errcap, "%%MAIL-E-WRITEERR, error writing mail file for %s", user);
    return st->result;
}

static void arm(struct store *st)
{
    memset(st, 0, sizeof *st);
    st->result = DNET_M11_STS_SUCCESS;
    struct dnet_m11_ops ops = { check_rcpt, deliver, st };
    dnet_m11_init(&g_s, "VAX2", "VAX1", &ops);
}

/* Feed records; gather every reply into out[] (returns count, -1 on EPROTO). */
static int feed(const struct seg *recs, unsigned n, struct seg *out, unsigned outmax,
                unsigned *nout)
{
    *nout = 0;
    for (unsigned i = 0; i < n; i++) {
        int rc = dnet_m11_rx(&g_s, recs[i].b, recs[i].n);
        size_t l = 0;
        uint8_t buf[DNET_M11_MAX_REPLY];
        while (dnet_m11_tx_pop(&g_s, buf, sizeof buf, &l))
            if (*nout < outmax) { memcpy(out[*nout].b, buf, l); out[(*nout)++].n = l; }
        if (rc != DNET_M11_OK) return -1;
    }
    return 0;
}

static struct seg mk(const void *p, size_t n)
{
    struct seg s; memset(&s, 0, sizeof s);
    if (n > sizeof s.b) n = sizeof s.b;
    memcpy(s.b, p, n); s.n = n;
    return s;
}
static struct seg mks(const char *p) { return mk(p, strlen(p)); }
static const uint8_t ZERO = 0;

/* The oracle's accepted message, as client records. */
static unsigned oracle_msg(struct seg *r)
{
    unsigned n = 0;
    r[n++] = mks("SYSTEM      ");
    r[n++] = mks("SYSTEM");
    r[n++] = mk(&ZERO, 1);
    r[n++] = mks("VAX2::SYSTEM");
    r[n++] = mk("", 0);
    r[n++] = mks("DECnet MAIL-11 oracle");
    r[n++] = mks("Line one of a MAIL-11 message from VAX1.");
    r[n++] = mks("Line two.");
    r[n++] = mk(&ZERO, 1);
    return n;
}

static int is_status(const struct seg *s, uint32_t st)
{
    uint8_t b[4] = { (uint8_t)st, (uint8_t)(st >> 8), (uint8_t)(st >> 16), (uint8_t)(st >> 24) };
    return s->n == 4 && memcmp(s->b, b, 4) == 0;
}

static int is_refusal(const struct seg *o, uint32_t st, const char *text)
{
    return is_status(&o[0], st) && o[1].n == strlen(text) && memcmp(o[1].b, text, o[1].n) == 0 &&
           o[2].n == 1 && o[2].b[0] == 0;
}

int main(int argc, char **argv)
{
    static struct seg out[256];
    static struct seg recs[DNET_M11_MAX_LINES + 32];
    unsigned nout = 0;
    struct store st;

    if (argc < 2 || load_capture(argv[1]) != 0) {
        printf("FAIL: cannot read the MAIL-11 oracle capture (argv[1])\n");
        return 1;
    }

    /* ---- 1. the oracle, replayed ---------------------------------------- */
    unsigned confirmed = 0;
    for (unsigned k = 0; k < g_nsess; k++) {
        struct session *ss = &g_sess[k];
        if (!ss->confirmed) {
            NOTE("session %u: refused by the VAX at connect (no MAIL account) -- refusal shape only", k + 1);
            continue;
        }
        confirmed++;
        uint8_t acc[64]; size_t accn = 0;
        CHECK(ss->rcin > 9 && ss->ccn > 9 &&
              dnet_m11_connect_accept(ss->rci + 9, ss->rcin - 9, acc, sizeof acc, &accn) == 0 &&
              accn == ss->ccn - 9 && memcmp(acc, ss->cc + 9, accn) == 0,
              "session %u: Connect Confirm data == the VAX's (10 + 16 accept bytes)", k + 1);
        arm(&st);
        int rc = feed(ss->cli, ss->ncli, out, 256, &nout);
        CHECK(rc == 0, "session %u: the receiver accepted the VAX client stream", k + 1);
        int same = nout == ss->nsrv;
        for (unsigned i = 0; same && i < nout; i++)
            same = out[i].n == ss->srv[i].n && memcmp(out[i].b, ss->srv[i].b, out[i].n) == 0;
        CHECK(same, "session %u: %u reply segments, byte-identical to the VAX's %u", k + 1, nout,
              ss->nsrv);
        if (ss->nsrv == 2) {   /* the accepted message */
            CHECK(st.calls == 1 && !strcmp(st.user, "SYSTEM") && !strcmp(st.from, "VAX1::SYSTEM") &&
                  !strcmp(st.to, "VAX2::SYSTEM") && st.cc[0] == '\0' &&
                  !strcmp(st.subj, "DECnet MAIL-11 oracle") && st.nlines == 2 &&
                  !strcmp(st.line[0], "Line one of a MAIL-11 message from VAX1.") &&
                  !strcmp(st.line[1], "Line two."),
                  "session %u: stored From VAX1::SYSTEM, To, empty CC, Subj and both body lines", k + 1);
            CHECK(dnet_m11_done(&g_s), "session %u: exchange complete", k + 1);
        } else {
            CHECK(st.calls == 0, "session %u: a refused recipient stores nothing", k + 1);
        }
    }
    CHECK(confirmed == 2, "the capture holds the two confirmed sessions (accepted + NOSUCHUSER)");

    /* ---- 2. INV-6: a failed store is never acknowledged ------------------- */
    unsigned n = oracle_msg(recs);
    arm(&st);
    st.result = 0x000182CA;   /* an RMS failure status from the store */
    feed(recs, n, out, 256, &nout);
    CHECK(nout == 4 && is_status(&out[0], DNET_M11_STS_SUCCESS) &&
          is_refusal(&out[1], 0x000182CA, "%MAIL-E-WRITEERR, error writing mail file for SYSTEM"),
          "a store failure answers the store's status + text + 00, never 01 00 00 00");
    arm(&st);
    st.result = 0x00010001;   /* odd but not SS$_NORMAL: still not 'stored' */
    feed(recs, n, out, 256, &nout);
    CHECK(nout == 4 && is_status(&out[1], DNET_M11_STS_EXQUOTA),
          "only SS$_NORMAL from the store is acknowledged");

    /* ---- 3. truncation: the link ends before the end-of-message 00 -------- */
    unsigned trunc_bad = 0;
    for (unsigned cut = 0; cut < n; cut++) {
        arm(&st);
        feed(recs, cut, out, 256, &nout);
        /* at most the recipient's own 01 00 00 00 -- never a final status */
        if (st.calls != 0 || dnet_m11_done(&g_s) || nout > 1) trunc_bad++;
    }
    CHECK(trunc_bad == 0, "every truncation of the oracle stream stores and acknowledges "
          "nothing (%u violations)", trunc_bad);

    /* ---- 4. oversize / over-count refusals -------------------------------- */
    static char big[DNET_M11_MAX_REC + 2];
    memset(big, 'x', sizeof big - 1);
    n = 0;
    recs[n++] = mks("SYSTEM"); recs[n++] = mks("SYSTEM"); recs[n++] = mk(&ZERO, 1);
    recs[n++] = mks("VAX2::SYSTEM"); recs[n++] = mk("", 0); recs[n++] = mks("big line");
    recs[n++] = mk(big, DNET_M11_MAX_REC + 1); recs[n++] = mks("after"); recs[n++] = mk(&ZERO, 1);
    arm(&st);
    feed(recs, n, out, 256, &nout);
    CHECK(st.calls == 0 && nout == 4 &&
          is_refusal(&out[1], DNET_M11_STS_EXQUOTA, "%SYSTEM-F-EXQUOTA, exceeded quota"),
          "an over-long body line refuses the message: 1C 00 00 00 + %%SYSTEM-F-EXQUOTA + 00, nothing stored");

    n = 0;
    recs[n++] = mks("SYSTEM"); recs[n++] = mks("SYSTEM"); recs[n++] = mk(&ZERO, 1);
    recs[n++] = mks("T"); recs[n++] = mk("", 0); recs[n++] = mks("many lines");
    for (unsigned i = 0; i < DNET_M11_MAX_LINES + 1; i++) recs[n++] = mks("l");
    recs[n++] = mk(&ZERO, 1);
    arm(&st);
    feed(recs, n, out, 256, &nout);
    CHECK(st.calls == 0 && nout == 4 && is_status(&out[1], DNET_M11_STS_EXQUOTA),
          "more than DNET_M11_MAX_LINES body lines refuses the message");

    n = 0;
    recs[n++] = mks("SYSTEM"); recs[n++] = mks("SYSTEM"); recs[n++] = mk(&ZERO, 1);
    recs[n++] = mks("T"); recs[n++] = mk("", 0); recs[n++] = mks("S");
    recs[n++] = mks("a\nb"); recs[n++] = mk(&ZERO, 1);
    arm(&st);
    feed(recs, n, out, 256, &nout);
    CHECK(st.calls == 0 && nout == 4 &&
          is_refusal(&out[1], DNET_M11_STS_BADPARAM, "%SYSTEM-F-BADPARAM, bad parameter value"),
          "a body line carrying a record delimiter refuses the message (no silent rewrite)");

    n = 0;
    recs[n++] = mks("SYSTEM");
    for (unsigned i = 0; i < DNET_M11_MAX_RCPT + 1; i++) recs[n++] = mks("GUEST");
    arm(&st);
    feed(recs, n, out, 256, &nout);
    CHECK(nout == DNET_M11_MAX_RCPT + 3 && is_status(&out[DNET_M11_MAX_RCPT - 1], 1) &&
          is_refusal(&out[DNET_M11_MAX_RCPT], DNET_M11_STS_EXQUOTA, "%SYSTEM-F-EXQUOTA, exceeded quota"),
          "recipient %u is refused EXQUOTA; the first %u are accepted", DNET_M11_MAX_RCPT + 1,
          DNET_M11_MAX_RCPT);

    n = 0;
    recs[n++] = mks("SYSTEM");
    recs[n++] = mks("FOURTEENCHARSX");
    recs[n++] = mks("BAD:NAME");
    arm(&st);
    feed(recs, n, out, 256, &nout);
    CHECK(nout == 6 &&
          is_refusal(&out[0], DNET_M11_STS_NOSUCHUSR, "%MAIL-E-NOSUCHUSR, no such user FOURTEENCHARSX at node VAX2") &&
          is_refusal(&out[3], DNET_M11_STS_NOSUCHUSR, "%MAIL-E-NOSUCHUSR, no such user BAD:NAME at node VAX2"),
          "an over-long or non-username recipient is NOSUCHUSR, never looked up");

    static char longsender[DNET_M11_MAX_SENDER + 2];
    memset(longsender, 'S', sizeof longsender - 1);
    struct seg one = mks(longsender);
    arm(&st);
    CHECK(feed(&one, 1, out, 256, &nout) == -1 && nout == 0 && g_s.state == DNET_M11_S_ABORT,
          "an over-long sender record ends the session (protocol error), no reply");
    one = mk("", 0);
    arm(&st);
    CHECK(feed(&one, 1, out, 256, &nout) == -1, "an empty sender record ends the session");
    n = oracle_msg(recs);
    recs[n++] = mks("trailing");
    arm(&st);
    CHECK(feed(recs, n, out, 256, &nout) == -1 && st.calls == 1,
          "a record after the exchange completed is a protocol error (the one stored copy stands)");

    /* ---- 5. connect data ---------------------------------------------------- */
    const struct session *acc = NULL;
    for (unsigned k = 0; k < g_nsess; k++) if (g_sess[k].confirmed) { acc = &g_sess[k]; break; }
    if (acc) {
        const uint8_t *c = acc->rci + 9; size_t cl = acc->rcin - 9;
        uint8_t m[128], o[64]; size_t on = 0;
        int all_short_refused = 1;
        for (size_t l = 0; l < cl; l++)
            if (dnet_m11_connect_accept(c, l, o, sizeof o, &on) == 0) all_short_refused = 0;
        CHECK(all_short_refused, "every truncated connect message is refused");
        memcpy(m, c, cl); m[1] = 17;
        CHECK(dnet_m11_connect_accept(m, cl, o, sizeof o, &on) != 0, "object 17 is not MAIL-11");
        memcpy(m, c, cl); m[cl - 16] = 0x02;
        CHECK(dnet_m11_connect_accept(m, cl, o, sizeof o, &on) != 0,
              "a different MAIL-11 protocol version is refused, not guessed at");
        memcpy(m, c, cl); m[15] = 0x25;   /* MENUVER without USRDATA */
        CHECK(dnet_m11_connect_accept(m, cl - 17, o, sizeof o, &on) != 0,
              "a connect without MAIL-11 user data is refused");
    }

    /* ---- 6. fuzz: mutate the oracle stream --------------------------------- */
    unsigned long seed = 0x4d41494cUL;
    unsigned fuzz_bad = 0;
    for (int it = 0; it < 20000; it++) {
        n = oracle_msg(recs);
        unsigned muts = 1 + (unsigned)(seed % 4);
        for (unsigned k = 0; k < muts; k++) {
            seed = seed * 6364136223846793005UL + 1442695040888963407UL;
            unsigned r = (unsigned)(seed >> 33) % n;
            switch ((seed >> 20) % 5) {
            case 0: if (recs[r].n) recs[r].b[(seed >> 8) % recs[r].n] ^= (uint8_t)(seed >> 40); break;
            case 1: recs[r].n = (size_t)((seed >> 12) % (recs[r].n + 1)); break;
            case 2:
                recs[r].n = (size_t)((seed >> 12) % sizeof recs[r].b);
                for (size_t j = 0; j < recs[r].n; j++)
                    recs[r].b[j] = (uint8_t)(seed >> (j % 50));
                break;
            case 3: n = r + 1; break;
            case 4: if (n < 60) recs[n++] = mk(&ZERO, 1); break;
            }
        }
        arm(&st);
        feed(recs, n, out, 256, &nout);
        /* Invariants: the store is reached only at the end-of-message step,
         * once per accepted recipient; every 01 00 00 00 is a recipient
         * acceptance or a stored copy; the bounds hold. */
        unsigned acks = 0;
        for (unsigned i = 0; i < nout; i++) acks += is_status(&out[i], 1);
        if (st.bad_state || (unsigned)st.calls > g_s.nrcpt ||
            acks > g_s.nrcpt + g_s.delivered || g_s.delivered > (unsigned)st.calls ||
            g_s.txq_n > DNET_M11_MAX_TXQ || g_s.bodylen > DNET_M11_MAX_BODY ||
            g_s.nlines > DNET_M11_MAX_LINES)
            fuzz_bad++;
    }
    CHECK(fuzz_bad == 0, "20000 mutated oracle streams: never a store before DONE, never an "
          "unstored ack, bounds held (%u violations)", fuzz_bad);

    printf("test_dnet_mail11: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
