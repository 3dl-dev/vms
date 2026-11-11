/*
 * test_rtpad_session.c -- RTPAD.EXE's CTERM terminal side against real VAX
 * host sessions (rd vms-0a7 S2, vms-18c9).
 *
 *   1. REPLAY, LIVE OVMX<->VAX: the host segments a real OpenVMS VAX V7.3 sent
 *      an OVMX SET HOST (tests/lab/captures/decnet-sethost-dcl-20260911/, the
 *      ORACLE_L table) drive rtpad_session; the lines typed are the ones that
 *      session typed. Every segment RTPAD sends must be byte-identical to the
 *      client segment the VAX accepted at that point, the Password: read must
 *      be a no-echo read, and the screen must show the VAX's output.
 *   2. REPLAY, VAX<->VAX: the real VAX2 host's segments to a real VAX1 client
 *      (docs/oracle/vax-sethost-cterm.pcap, ORACLE_O) drive rtpad_session; its
 *      foundation replies must equal the real VAX client's bytes.
 *   3. A file SYS$INPUT: lines are released one per host solicit, nothing
 *      before the host asks; %REM-S-END is written as a record naming the
 *      local node; a link that refuses a send ends the session with its status.
 *   4. FUZZ (peer-receive code): 300k mutated host segments into every session
 *      state; nothing may overrun (ASan/UBSan where available), and every
 *      screen write stays inside its buffer.
 *
 * Clean-room (Rule 8): the bytes are the captured wire; nothing is invented.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtpad_session.h"
#include "ssdef.h"
#include "rmsdef.h"

struct cth_oracle_seg { int host; const uint8_t *b; size_t n; };
#include "cterm_host_oracle.inc"

#define NSEG(a) (sizeof(a) / sizeof((a)[0]))

static int failures, passes;
static void check(int ok, const char *what)
{
    if (ok) { passes++; printf("  ok   %s\n", what); }
    else    { failures++; printf("  FAIL %s\n", what); }
}

/* ---- a recording fake of the link and the terminal -------------------- */
#define MAXSENT 128
struct fake {
    uint8_t sent[MAXSENT][DNET_CTERM_MAX_PDU];
    size_t sentlen[MAXSENT];
    int nsent;
    uint32_t send_status;             /* what link_send answers */
    uint8_t screen[65536];
    size_t screenlen;
    char record[256];
    int nrecords;
    /* typed lines */
    const uint8_t *lines[32];
    size_t linelen[32];
    uint8_t lineterm[32];
    int nlines, nextline;
    int echo_of[32], term_echo_of[32];
    unsigned timeout_of[32];
    int time_out[32];                 /* this line's read runs out of time */
    int prompted;                     /* reads that carried a prompt */
    uint8_t init[64];                 /* the last read's initial input */
    size_t initlen;
    uint32_t oob_mask;                /* the last set_oob */
    int oob_sets;
    int reads;
    /* out-of-band during a read: the character the driver gives (its AST
     * runs inside the read, as the executive delivers it), which with
     * IO$M_TT_ABORT ends the read SS$_ABORT */
    uint8_t oob_in_read[32];
    struct rtpad_session *sess;
    uint8_t lastprompt[160];
    size_t lastplen;
    int questions;                    /* reads prompting "Are you repeating" */
    char qtext[160];
};

static uint32_t f_send(void *ctx, const uint8_t *pdu, size_t n)
{
    struct fake *f = ctx;
    if (f->nsent < MAXSENT && n <= DNET_CTERM_MAX_PDU) {
        memcpy(f->sent[f->nsent], pdu, n);
        f->sentlen[f->nsent] = n;
    }
    f->nsent++;
    return f->send_status;
}
static void f_show(void *ctx, const uint8_t *b, size_t n)
{
    struct fake *f = ctx;
    if (f->screenlen + n <= sizeof f->screen) {
        memcpy(f->screen + f->screenlen, b, n);
        f->screenlen += n;
    }
}
static void f_record(void *ctx, const char *msg)
{
    struct fake *f = ctx;
    snprintf(f->record, sizeof f->record, "%s", msg);
    f->nrecords++;
}
static uint32_t f_read_line(void *ctx, const uint8_t *prompt, size_t plen,
                            const uint8_t *init, size_t ilen,
                            uint8_t *buf, size_t cap, int echo, int term_echo,
                            unsigned timeout_s, size_t *n, uint8_t *term)
{
    struct fake *f = ctx;
    f_show(ctx, prompt, plen);        /* the driver writes a read's prompt */
    f->prompted += plen ? 1 : 0;
    if (plen < sizeof f->lastprompt) {
        memcpy(f->lastprompt, prompt, plen);
        f->lastplen = plen;
        if (plen > 17 && memcmp(prompt, "Are you repeating", 17) == 0) {
            f->questions++;
            snprintf(f->qtext, sizeof f->qtext, "%.*s", (int)plen, (const char *)prompt);
        }
    }
    if (ilen && ilen < sizeof f->init) {
        memcpy(f->init, init, ilen);
        f->initlen = ilen;
    }
    if (f->nextline >= f->nlines)
        return SS$_ENDOFFILE;
    int i = f->nextline++;
    if (f->oob_in_read[i] && f->sess) {
        rtpad_session_oob(f->sess, f->oob_in_read[i]);
        f->reads++;
        return SS$_ABORT;             /* IO$M_TT_ABORT ended the read */
    }
    f->echo_of[i] = echo;
    f->term_echo_of[i] = term_echo;
    f->timeout_of[i] = timeout_s;
    f->reads++;
    size_t k = f->linelen[i] < cap ? f->linelen[i] : cap;
    memcpy(buf, f->lines[i], k);
    *n = k;
    if (f->time_out[i])
        return SS$_TIMEOUT;           /* the driver: IO$M_TIMED expired */
    *term = f->lineterm[i];
    return SS$_NORMAL;
}
static uint32_t f_read_record(void *ctx, uint8_t *buf, size_t cap, size_t *n)
{
    struct fake *f = ctx;
    if (f->nextline >= f->nlines)
        return RMS$_EOF;
    int i = f->nextline++;
    f->reads++;
    size_t k = f->linelen[i] < cap ? f->linelen[i] : cap;
    memcpy(buf, f->lines[i], k);
    *n = k;
    return SS$_NORMAL;
}

static void f_set_oob(void *ctx, uint32_t mask)
{
    struct fake *f = ctx;
    f->oob_mask = mask;
    f->oob_sets++;
}

static void fake_io(struct fake *f, struct rtpad_io *io)
{
    memset(io, 0, sizeof *io);
    io->ctx = f;
    io->link_send = f_send;
    io->show = f_show;
    io->record = f_record;
    io->read_line = f_read_line;
    io->read_record = f_read_record;
    io->set_oob = f_set_oob;
}

/* A client Read Data (09 00 LEN 00 | 03 00 00 00 00 00 <len:2> data term):
 * its typed line and terminator. 1 = it is one. */
static int read_data_of(const uint8_t *b, size_t n, const uint8_t **line,
                        size_t *len, uint8_t *term)
{
    if (n < 13 || b[0] != 0x09 || b[4] != 0x03)
        return 0;
    size_t dl = (size_t)b[10] | ((size_t)b[11] << 8);
    if (12 + dl + 1 > n)
        return 0;
    *line = b + 12;
    *len = dl;
    *term = b[12 + dl];
    return 1;
}

static int screen_has(const struct fake *f, const char *s)
{
    size_t sl = strlen(s);
    if (sl > f->screenlen) return 0;
    for (size_t i = 0; i + sl <= f->screenlen; i++)
        if (memcmp(f->screen + i, s, sl) == 0) return 1;
    return 0;
}

/* Replay a capture's host segments through rtpad_session, typing the lines
 * the capture's client typed. Returns how many client segments matched (in
 * order) before the first difference; *nclient = how many the capture has. */
static int replay(const struct cth_oracle_seg *seg, size_t start, size_t end,
                  struct fake *f, struct rtpad_session *s, int *nclient, int *first_bad)
{
    struct rtpad_io io;
    fake_io(f, &io);
    /* the lines the capture's client typed */
    for (size_t i = start; i < end; i++) {
        const uint8_t *l; size_t ll; uint8_t t;
        if (!seg[i].host && read_data_of(seg[i].b, seg[i].n, &l, &ll, &t) &&
            f->nlines < 32) {
            f->lines[f->nlines] = l;
            f->linelen[f->nlines] = ll;
            f->lineterm[f->nlines] = t;
            f->nlines++;
        }
    }
    static struct rtpad_io keep;
    keep = io;
    if (rtpad_session_init(s, &keep, 1) != 0)
        return -1;
    rtpad_session_linkup(s);
    for (size_t i = start; i < end && !s->done; i++) {
        if (!seg[i].host)
            continue;
        rtpad_session_rx(s, seg[i].b, seg[i].n);
        while (rtpad_session_wants_input(s) && f->nextline < f->nlines)
            rtpad_session_input(s);
    }
    /* compare what was sent with the capture's client segments */
    int k = 0, match = 0;
    *first_bad = -1;
    for (size_t i = start; i < end; i++) {
        if (seg[i].host)
            continue;
        if (*first_bad < 0 && k < f->nsent && k < MAXSENT &&
            f->sentlen[k] == seg[i].n && memcmp(f->sent[k], seg[i].b, seg[i].n) == 0)
            match++;
        else if (*first_bad < 0)
            *first_bad = k;
        k++;
    }
    *nclient = k;
    return match;
}

static void test_replay_live(void)
{
    printf("[replay] a real VAX V7.3 host's segments to an OVMX SET HOST (ORACLE_L)\n");
    static struct fake f;
    static struct rtpad_session s;
    memset(&f, 0, sizeof f);
    f.send_status = SS$_NORMAL;
    int ncl = 0, bad = -1;
    int m = replay(k_oracle_l, 0, NSEG(k_oracle_l), &f, &s, &ncl, &bad);
    if (bad >= 0)
        printf("    first differing client segment: #%d (sent %d, capture %d)\n",
               bad, f.nsent, ncl);
    check(m == ncl && f.nsent == ncl,
          "every segment RTPAD sends is the client segment the real VAX accepted");
    check(f.nlines >= 3 && f.reads == f.nlines,
          "each typed line answered exactly one host Start Read");
    check(screen_has(&f, "Username:") && screen_has(&f, "Password:"),
          "the VAX's Username: and Password: prompts reached the screen");
    check(f.prompted >= 2,
          "the host prompts went WITH their terminal reads (IO$_READPROMPT), not as separate writes");
    int pw = -1;
    for (int i = 0; i < f.nlines; i++)
        if (i == 1) pw = i;
    check(pw == 1 && f.echo_of[0] == 1 && f.echo_of[1] == 0,
          "Username: is read with echo, Password: without (the host's N flag)");
    check(s.bound_ever == 1, "the CTERM session bound");
    rtpad_session_finish(&s, "OVMXA");
    check(f.nrecords == 1 &&
          strcmp(f.record, "%REM-S-END, control returned to node OVMXA::") == 0,
          "control returns with %REM-S-END naming the local node, as a record");
}

static void test_replay_vax_client(void)
{
    printf("[replay] the real VAX2 host's segments to a real VAX1 client (ORACLE_O)\n");
    /* Session 2 (login, DCL, logout) starts at the second host Bind Request. */
    size_t start = 0;
    int binds = 0;
    for (size_t i = 0; i < NSEG(k_oracle_o); i++)
        if (k_oracle_o[i].host && k_oracle_o[i].n >= 1 && k_oracle_o[i].b[0] == 0x01 &&
            ++binds == 2)
            start = i;
    check(start > 0, "the capture holds a second (login) session");
    static struct fake f;
    static struct rtpad_session s;
    memset(&f, 0, sizeof f);
    f.send_status = SS$_NORMAL;
    int ncl = 0, bad = -1;
    (void)replay(k_oracle_o, start, NSEG(k_oracle_o), &f, &s, &ncl, &bad);
    /* The foundation: everything the client sent before its first Read Data.
     * Identical except where a message carries the ORIGINATING TERMINAL's
     * characteristics (TT$/TT2$ and fill bytes): that session's VAX1 client was
     * a VT100, and RTPAD sends the captured characteristics block. The differing
     * bytes must all lie inside that block (the 12 bytes after "18 00 42" /
     * "04 18 42"), never in a message header, length or handle. */
    int nfound = 0, same = 1, k = 0;
    for (size_t i = start; i < NSEG(k_oracle_o); i++) {
        if (k_oracle_o[i].host)
            continue;
        const uint8_t *l; size_t ll; uint8_t t;
        if (read_data_of(k_oracle_o[i].b, k_oracle_o[i].n, &l, &ll, &t))
            break;
        const uint8_t *v = k_oracle_o[i].b;
        size_t vn = k_oracle_o[i].n;
        if (k >= f.nsent || f.sentlen[k] != vn) {
            same = 0;
        } else if (memcmp(f.sent[k], v, vn) != 0) {
            size_t blk = vn;
            for (size_t j = 0; j + 2 < vn; j++) {
                if (v[j] == 0x18 && v[j + 1] == 0x42) { blk = j + 2; break; }
                if (v[j] == 0x18 && v[j + 2] == 0x42) { blk = j + 3; break; }
            }
            for (size_t j = 0; j < vn; j++)
                if (f.sent[k][j] != v[j] && (j < blk || j >= blk + 12)) {
                    same = 0;
                    printf("    client segment %d (capture #%zu) differs outside the"
                           " terminal block at byte %zu\n", k, i, j);
                    break;
                }
        }
        nfound++;
        k++;
    }
    printf("    foundation segments compared: %d\n", nfound);
    check(nfound >= 3 && same,
          "RTPAD's foundation replies are the real VAX client's, but for the"
          " originating terminal's characteristics");
    check(s.bound_ever == 1 && screen_has(&f, "Username:"),
          "the session binds and the VAX host's login prompt is shown");
}

static void test_file_input(void)
{
    printf("[file] a file SYS$INPUT: one line per solicit, nothing before\n");
    static struct fake f;
    static struct rtpad_session s;
    struct rtpad_io io;
    memset(&f, 0, sizeof f);
    f.send_status = SS$_NORMAL;
    static const uint8_t l1[] = "SYSTEM", l2[] = "secret";
    f.lines[0] = l1; f.linelen[0] = 6; f.lines[1] = l2; f.linelen[1] = 6;
    f.nlines = 2;
    fake_io(&f, &io);
    check(rtpad_session_init(&s, &io, 0) == 0, "session init");
    rtpad_session_linkup(&s);
    check(!rtpad_session_wants_input(&s), "no input wanted before the host asks");
    int asked = 0;
    for (size_t i = 0; i < NSEG(k_oracle_l) && !s.done; i++) {
        if (!k_oracle_l[i].host)
            continue;
        rtpad_session_rx(&s, k_oracle_l[i].b, k_oracle_l[i].n);
        if (rtpad_session_wants_input(&s)) {
            asked++;
            int before = f.reads;
            rtpad_session_input(&s);
            if (f.reads != before + 1 && f.nextline <= f.nlines && asked <= 2) {
                check(0, "one record read per solicit");
                return;
            }
        }
    }
    check(asked >= 3 && f.reads == 2,
          "each solicit took one record; the end of the file stops input");
    check(s.input_eof == 1, "the end of the file is seen, the link is kept");
}

/* ---- a timed read (rd vms-0a7 S2 live bench, VAX V7.3 host) --------------
 * After "User authorization failure" a VAX host's LOGINOUT issues a no-prompt
 * retry read, TIMED (FLAGS bit 13, TIMEOUT 20 s): from the bench wire
 * (tests/lab/captures/vms-0a7-s2-20261103), the host->terminal segment that
 * follows the failure Write. The VAX's own SET HOST ends the session 20 s
 * later; RTPAD must time the read and tell the host (Read Data, code 5). */
static const uint8_t k_vax_retry_read[] = {
    0x09, 0x00, 0x11, 0x00,
    0x02, 0x08, 0xb8, 0x00, 0x84, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00 };
/* the same read with bit 13 clear: TIMEOUT is not meaningful */
static const uint8_t k_untimed_read[] = {
    0x09, 0x00, 0x11, 0x00,
    0x02, 0x08, 0x98, 0x00, 0x84, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00 };

static void test_timed_read(void)
{
    printf("[timed] the host's Start Read TIMEOUT reaches the terminal read; expiry -> Read Data code 5\n");
    unsigned secs = 99;
    check(dnet_cterm_start_read_timeout(k_vax_retry_read, sizeof k_vax_retry_read, &secs) == 0 &&
          secs == 20, "the VAX retry read is timed: 20 s");
    check(dnet_cterm_start_read_timeout(k_untimed_read, sizeof k_untimed_read, &secs) == 0 &&
          secs == 0, "bit 13 clear: untimed, whatever TIMEOUT holds");
    check(dnet_cterm_start_read_timeout(k_vax_retry_read, 9, &secs) != 0 && secs == 0,
          "a truncated Start Read: refused, untimed");

    static struct fake f;
    static struct rtpad_session s;
    struct rtpad_io io;
    memset(&f, 0, sizeof f);
    f.send_status = SS$_NORMAL;
    static const uint8_t typed[] = "SYS";
    f.lines[0] = typed; f.linelen[0] = 3; f.time_out[0] = 1;
    f.nlines = 1;
    fake_io(&f, &io);
    rtpad_session_init(&s, &io, 1);
    rtpad_session_linkup(&s);
    for (size_t i = 0; i < NSEG(k_oracle_l) && !dnet_cterm_is_bound(&s.term); i++)
        if (k_oracle_l[i].host)
            rtpad_session_rx(&s, k_oracle_l[i].b, k_oracle_l[i].n);
    check(dnet_cterm_is_bound(&s.term), "bound on the oracle's opening");
    int before = f.nsent;
    rtpad_session_rx(&s, k_vax_retry_read, sizeof k_vax_retry_read);
    check(rtpad_session_wants_input(&s), "the retry read solicits input");
    rtpad_session_input(&s);
    check(f.timeout_of[0] == 20, "the terminal read was timed with the host's 20 s");
    static const uint8_t want[] = { 0x09, 0x00, 0x0b, 0x00, 0x03, 0x05, 0x00, 0x00, 0x00,
                                    0x00, 0x03, 0x00, 'S', 'Y', 'S' };
    check(f.nsent == before + 1 && f.sentlen[before] == sizeof want &&
          memcmp(f.sent[before], want, sizeof want) == 0,
          "expiry sent Read Data: code 5, termination position 3, the typed SYS, no terminator");
    check(!rtpad_session_wants_input(&s) && !s.done, "the read is answered; the session goes on");
}

/* ---- out of band, characteristics, recall, the terminal's own image ----
 * The VAX V7.3 host's Characteristics, from the bench wire (tests/lab/
 * captures/vms-0a7-s2-20261103): at login DCL makes CTRL/Y out of band
 * (attributes 0x29 under mask 0x3b) and CTRL/T (0x03 under 0x3f); its recall
 * read carries the recalled command after END-OF-PROMPT. */
static const uint8_t k_vax_char_ctrly[] = { 0x09, 0x00, 0x07, 0x00,
    0x0b, 0x00, 0x02, 0x02, 0x19, 0x3b, 0x29 };
static const uint8_t k_vax_char_ctrlt[] = { 0x09, 0x00, 0x07, 0x00,
    0x0b, 0x00, 0x02, 0x02, 0x14, 0x3f, 0x03 };
static const uint8_t k_vax_recall_read[] = { 0x09, 0x00, 0x1f, 0x00,
    0x02, 0x08, 0x90, 0x0a, 0x00, 0x01, 0x0e, 0x00, 0x00, 0x00, 0x05, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, '\r', '\n', 0x00, '$', ' ',
    'S', 'H', 'O', 'W', ' ', 'T', 'I', 'M', 'E' };

/* DCL's command read (untimed, no initial input), from the same wire */
static const uint8_t k_vax_dcl_read[] = { 0x09, 0x00, 0x16, 0x00,
    0x02, 0x08, 0x90, 0x0a, 0x00, 0x01, 0x05, 0x00, 0x00, 0x00, 0x05, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, '\r', '\n', 0x00, '$', ' ' };

static void bind_on_oracle(struct fake *f, struct rtpad_session *s, struct rtpad_io *io)
{
    memset(f, 0, sizeof *f);
    f->send_status = SS$_NORMAL;
    fake_io(f, io);
    rtpad_session_init(s, io, 1);
    rtpad_session_linkup(s);
    for (size_t i = 0; i < NSEG(k_oracle_l) && !dnet_cterm_is_bound(&s->term); i++)
        if (k_oracle_l[i].host)
            rtpad_session_rx(s, k_oracle_l[i].b, k_oracle_l[i].n);
}

static void test_oob(void)
{
    printf("[oob] CTRL/Y and CTRL/C are the remote session's; the host's out-of-band set\n");
    static struct fake f;
    static struct rtpad_session s;
    static struct rtpad_io io;
    bind_on_oracle(&f, &s, &io);
    check(f.oob_sets >= 1 && f.oob_mask == RTPAD_OOB_ALWAYS,
          "from the start CTRL/Y and CTRL/C are taken out of band from the local driver");
    /* VAX V7.3 client to a host that never made CTRL/Y or CTRL/C out of band
     * (S2 bench wire, VAX SET HOST OVMXA): CTRL/Y -> 04 00 19, CTRL/C -> the
     * same 04 00 19, the interrupt shown locally */
    static const uint8_t want_y0[] = { 0x09, 0x00, 0x03, 0x00, 0x04, 0x00, 0x19 };
    int before = f.nsent;
    rtpad_session_oob(&s, 0x19);
    check(f.nsent == before + 1 && f.sentlen[before] == sizeof want_y0 &&
          memcmp(f.sent[before], want_y0, sizeof want_y0) == 0 && f.nrecords == 0,
          "CTRL/Y before the host says anything: Out-of-Band 04 00 19 (the driver shows the interrupt)");
    rtpad_session_oob(&s, 0x03);
    check(f.nsent == before + 2 && memcmp(f.sent[before + 1], want_y0, sizeof want_y0) == 0,
          "CTRL/C goes as that same CTRL/Y (as a VAX client) while the host has not made CTRL/C out of band");
    rtpad_session_rx(&s, k_vax_char_ctrly, sizeof k_vax_char_ctrly);
    rtpad_session_rx(&s, k_vax_char_ctrlt, sizeof k_vax_char_ctrlt);
    check(s.chattr[0x19] == 0x29 && s.chattr[0x14] == 0x03,
          "the host's CHARACTER-ATTRIBUTES recorded (CTRL/Y 0x29, CTRL/T 0x03)");
    check(f.oob_mask == (RTPAD_OOB_ALWAYS | (1u << 0x14)),
          "CTRL/T joins the local out-of-band mask");
    before = f.nsent;
    rtpad_session_oob(&s, 0x19);
    static const uint8_t want_y[] = { 0x09, 0x00, 0x03, 0x00, 0x04, 0x01, 0x19 };
    check(f.nsent == before + 1 && f.sentlen[before] == sizeof want_y &&
          memcmp(f.sent[before], want_y, sizeof want_y) == 0,
          "CTRL/Y goes as Out-of-Band 04 01 19 (discard, per its attributes)");
    {
        /* a host read outstanding and no CTRL/Y just before: the host's read
         * ends, Read Data completion code 3 with no data, right after the
         * Out-of-Band (VAX client's wire, S2 bench 155.081 / 160.232) */
        static const uint8_t want_rd3[] = { 0x09, 0x00, 0x08, 0x00,
            0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        s.y_seen = 0;
        rtpad_session_rx(&s, k_vax_dcl_read, sizeof k_vax_dcl_read);
        before = f.nsent;
        rtpad_session_oob(&s, 0x19);
        check(f.nsent == before + 2 && f.sentlen[before + 1] == sizeof want_rd3 &&
              memcmp(f.sent[before + 1], want_rd3, sizeof want_rd3) == 0 &&
              !s.read_pending && !s.ask_pending,
              "CTRL/Y at a read: Out-of-Band, then Read Data code 3 (03 03 00 00 00 00 00 00); the host solicits again");
    }
    before = f.nsent;
    f.nrecords = 0;
    rtpad_session_oob(&s, 0x14);
    static const uint8_t want_t[] = { 0x09, 0x00, 0x03, 0x00, 0x04, 0x00, 0x14 };
    check(f.nsent == before + 1 && memcmp(f.sent[before], want_t, sizeof want_t) == 0 &&
          f.nrecords == 0,
          "CTRL/T (immediate hello) goes as Out-of-Band 04 00 14, nothing echoed");
    rtpad_session_oob(&s, 0x40);
    check(f.nsent == before + 1, "a character past 31 is not out of band: nothing sent");
}

static void test_recall_and_sense(void)
{
    printf("[recall] a Start Read's text after END-OF-PROMPT starts the line; [sense] THIS terminal\n");
    static struct fake f;
    static struct rtpad_session s;
    static struct rtpad_io io;
    memset(&f, 0, sizeof f);
    f.send_status = SS$_NORMAL;
    static const uint8_t ret[] = "";
    f.lines[0] = ret; f.linelen[0] = 0; f.lineterm[0] = 0x0d; f.nlines = 1;
    fake_io(&f, &io);
    rtpad_session_init(&s, &io, 1);
    /* a VT100, 80 x 24 (IO$_SENSEMODE: class 66, type 96, width 80,
     * characteristics a0 02 00 | page 24, extended 00 32 00 00) */
    static const uint8_t vt100[12] = { 0x42, 0x60, 0x50, 0x00, 0xa0, 0x02, 0x00, 0x18,
                                       0x00, 0x32, 0x00, 0x00 };
    rtpad_session_set_sense(&s, vt100);
    rtpad_session_linkup(&s);
    for (size_t i = 0; i < NSEG(k_oracle_l) && !dnet_cterm_is_bound(&s.term); i++)
        if (k_oracle_l[i].host)
            rtpad_session_rx(&s, k_oracle_l[i].b, k_oracle_l[i].n);
    int seg2 = 0, seg4 = 0;
    for (int k = 0; k < f.nsent && k < MAXSENT; k++) {
        for (size_t o = 0; o + 12 <= f.sentlen[k]; o++)
            if (memcmp(f.sent[k] + o, vt100, 12) == 0) {
                if (f.sent[k][4] == 0x01) seg2 = 1;
                if (f.sent[k][4] == 0x13) seg4 = 1;
            }
    }
    check(seg2 && seg4, "the client foundation reports the VT100 80x24 the terminal driver gave");
    rtpad_session_rx(&s, k_vax_recall_read, sizeof k_vax_recall_read);
    check(rtpad_session_wants_input(&s), "the recall read solicits input");
    rtpad_session_input(&s);
    check(f.initlen == 9 && memcmp(f.init, "SHOW TIME", 9) == 0,
          "the recalled SHOW TIME is the read's initial input, not part of its prompt");
    check(screen_has(&f, "$ ") && !screen_has(&f, "$ SHOW"), "the prompt is just the prompt");
    /* DCL's next read: untimed; it starts at once, its prompt WITH it (the
     * driver renders "\r\n\0$ "), the link served meanwhile (rd vms-fc0) */
    size_t mark = f.screenlen;
    rtpad_session_rx(&s, k_vax_dcl_read, sizeof k_vax_dcl_read);
    check(f.screenlen == mark && rtpad_session_wants_input(&s) && rtpad_session_read_now(&s) &&
          s.rd_plen == 5 && memcmp(s.rd_prompt, "\r\n\0$ ", 5) == 0,
          "an untimed read starts at once with its prompt (nothing shown before the read)");
}

/* CTRL/Y as a VAX V7.3 client counts it (S2 bench and its VAX-only probes,
 * tests/lab/captures/vms-0a7-s2-20261103): with a host read outstanding, a
 * CTRL/Y after another with fewer than two lines sent since asks "Are you
 * repeating ^Y to abort the remote session on node X?"; the answer is never
 * the host's; Y ends the session. */
static void test_ctrly_repeat(void)
{
    printf("[ctrly] the repeat rule, the question, its answers\n");
    static struct fake f;
    static struct rtpad_session s;
    static struct rtpad_io io;
    bind_on_oracle(&f, &s, &io);
    f.sess = &s;
    rtpad_session_set_node(&s, "VAXC::");
    static const uint8_t l_show[] = "SHOW TIME", l_xyz[] = "XYZ", l_y[] = "Y", l_none[] = "";
    /* the script of local reads, in order */
    int k = 0;
#define LINE(t, oob) do { f.lines[k] = (t); f.linelen[k] = strlen((const char *)(t)); \
        f.lineterm[k] = 0x0d; f.oob_in_read[k] = (oob); k++; } while (0)
    LINE(l_show, 0);        /* 0: SHOW TIME */
    LINE(l_show, 0);        /* 1: SHOW TIME */
    LINE(l_none, 0x19);     /* 2: CTRL/Y at the prompt: first -> code 3 */
    LINE(l_none, 0x19);     /* 3: CTRL/Y in the read the host issued after: repeat */
    LINE(l_xyz, 0);         /* 4: the answer XYZ */
    LINE(l_none, 0x19);     /* 5: CTRL/Y at once again: repeat (asking does not reset) */
    LINE(l_none, 0);        /* 6: the answer RETURN */
    LINE(l_show, 0);        /* 7: SHOW TIME */
    LINE(l_show, 0);        /* 8: SHOW TIME (two lines since) */
    LINE(l_none, 0x19);     /* 9: CTRL/Y: no question, code 3 */
    LINE(l_none, 0x19);     /* 10: CTRL/Y: repeat */
    LINE(l_y, 0);           /* 11: the answer Y */
    f.nlines = k;
#undef LINE
    int sent_lines = 0, code3 = 0;
    for (int turn = 0; turn < 40 && !s.done; turn++) {
        if (!rtpad_session_wants_input(&s))
            rtpad_session_rx(&s, k_vax_dcl_read, sizeof k_vax_dcl_read);   /* the host solicits */
        int before = f.nsent;
        rtpad_session_input(&s);
        for (int m = before; m < f.nsent && m < MAXSENT; m++) {
            const uint8_t *ln; size_t ll; uint8_t tm;
            if (f.sentlen[m] >= 6 && f.sent[m][4] == 0x03 && f.sent[m][5] == 0x03)
                code3++;
            else if (read_data_of(f.sent[m], f.sentlen[m], &ln, &ll, &tm) == 1) {
                sent_lines++;
                check(!(ll == 3 && memcmp(ln, "XYZ", 3) == 0), "the answer XYZ is never sent to the host");
            }
        }
        if (f.nextline >= f.nlines && !s.ask_pending)
            break;
    }
    check(code3 == 2, "two first CTRL/Ys end the host's read with code 3 (a repeated one does not)");
    check(f.questions == 3, "three repeated CTRL/Ys ask the question");
    check(strcmp(f.qtext, "Are you repeating ^Y to abort the remote session on node VAXC? ") == 0,
          "the question names the node as SET HOST did, as the VAX shows it");
    check(sent_lines == 4, "only the four SHOW TIME lines go to the host");
    check(s.done && s.user_abort && s.status == SS$_NORMAL,
          "Y ends the session, status SS$_NORMAL ($STATUS %X00000001 on the VAX)");
    f.nrecords = 0;
    rtpad_session_finish(&s, "OVMXA");
    check(f.nrecords == 1 && strcmp(f.record, "%REM-S-END, control returned to node OVMXA::") == 0,
          "... and control returns with %REM-S-END");
}

static void test_send_refused(void)
{
    printf("[link] a link that refuses a send ends the session with its status\n");
    static struct fake f;
    static struct rtpad_session s;
    struct rtpad_io io;
    memset(&f, 0, sizeof f);
    f.send_status = SS$_ABORT;
    fake_io(&f, &io);
    rtpad_session_init(&s, &io, 1);
    rtpad_session_linkup(&s);
    for (size_t i = 0; i < NSEG(k_oracle_l) && !s.done; i++)
        if (k_oracle_l[i].host)
            rtpad_session_rx(&s, k_oracle_l[i].b, k_oracle_l[i].n);
    check(s.done && s.status == SS$_ABORT, "done, status the refusal (SS$_ABORT)");
    rtpad_session_finish(&s, "N");
    check(f.nrecords == 0, "no %REM-S-END for a session never established");
}

/* ---- fuzz: the peer-receive path ----------------------------------------- */
static uint32_t rng = 0x5eed1234u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void test_fuzz(void)
{
    printf("[fuzz] 300k mutated host segments into every session state\n");
    static struct fake f;
    static struct rtpad_session s;
    struct rtpad_io io;
    static const uint8_t ln[] = "X";
    long iters = 0;
    for (int round = 0; round < 300; round++) {
        memset(&f, 0, sizeof f);
        f.send_status = SS$_NORMAL;
        for (int i = 0; i < 32; i++) { f.lines[i] = ln; f.linelen[i] = 1; f.lineterm[i] = 0x0d; }
        f.nlines = 32;
        fake_io(&f, &io);
        rtpad_session_init(&s, &io, round & 1);
        rtpad_session_linkup(&s);
        /* walk the real session to a random depth, then mutate */
        size_t depth = rnd() % NSEG(k_oracle_l);
        for (size_t i = 0; i < depth; i++)
            if (k_oracle_l[i].host)
                rtpad_session_rx(&s, k_oracle_l[i].b, k_oracle_l[i].n);
        for (int m = 0; m < 1000; m++, iters++) {
            uint8_t buf[1200];
            const struct cth_oracle_seg *src = &k_oracle_l[rnd() % NSEG(k_oracle_l)];
            size_t n = src->n < sizeof buf ? src->n : sizeof buf;
            memcpy(buf, src->b, n);
            int kind = (int)(rnd() % 5);
            if (kind == 0) {                       /* flip bytes */
                for (int j = 0; j < 4 && n; j++) buf[rnd() % n] ^= (uint8_t)rnd();
            } else if (kind == 1) {                /* truncate */
                n = n ? rnd() % n : 0;
            } else if (kind == 2) {                /* lie in a LENGTH word */
                if (n > 3) { buf[2] = (uint8_t)rnd(); buf[3] = (uint8_t)rnd(); }
            } else if (kind == 3) {                /* random bytes */
                n = rnd() % sizeof buf;
                for (size_t j = 0; j < n; j++) buf[j] = (uint8_t)rnd();
            } else {                               /* grow with junk */
                size_t add = rnd() % 200;
                for (size_t j = 0; j < add && n < sizeof buf; j++) buf[n++] = (uint8_t)rnd();
            }
            rtpad_session_rx(&s, buf, n);
            if (rtpad_session_wants_input(&s))
                rtpad_session_input(&s);
            if (s.done) {
                rtpad_session_init(&s, &io, round & 1);
                rtpad_session_linkup(&s);
                f.nextline = 0;
            }
        }
    }
    check(iters == 300000 && f.screenlen <= sizeof f.screen,
          "300000 mutated segments handled, nothing overran");
}

int main(void)
{
    test_replay_live();
    test_replay_vax_client();
    test_file_input();
    test_timed_read();
    test_oob();
    test_recall_and_sense();
    test_ctrly_repeat();
    test_send_refused();
    test_fuzz();
    printf("RTPAD-SESSION: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
