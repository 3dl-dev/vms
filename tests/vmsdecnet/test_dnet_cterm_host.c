/*
 * test_dnet_cterm_host.c - the CTERM HOST role against real VMS wire bytes
 * (rd vms-a70 direction B).
 *
 *   1. CODEC EXACTNESS: where a real VAX host's message content is deterministic,
 *      OVMX's builder emits the same bytes (Bind Request, the Characteristics
 *      negotiation, the Initiate given the VAX's own parameter, message 23, the
 *      Username:/Password:/$ Start Reads given the VAX's own flags, the raw and
 *      record Writes, Unbind).
 *   2. EVERY REAL MESSAGE DECODES: each host message of both captures parses
 *      with the host-side parsers (Common Data framing, Start Read fields
 *      consistent), and each client message decodes the way the host FSM needs
 *      (Bind Accept, Initiate parameters, Read Data with its terminator).
 *   3. THE FSM REPLAYS A REAL SESSION: the real VAX1 client's segments from the
 *      VAX<->VAX oracle drive dnet_cth as the host; every segment the FSM emits
 *      is checked against what the real VAX2 host sent at that point.
 *   4. FUZZ: 200k mutated client segments into every FSM state and every
 *      decoder, under ASan/UBSan where the toolchain has it.
 *
 * Clean-room (Rule 8): the bytes are the captured wire; nothing is invented.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dnet_cterm.h"
#include "dnet_cterm_hostfsm.h"

struct cth_oracle_seg { int host; const uint8_t *b; size_t n; };
#include "cterm_host_oracle.inc"

#define NSEG(a) (sizeof(a) / sizeof((a)[0]))

static int failures, passes;
static void check(int cond, const char *what)
{
    if (cond) { passes++; return; }
    failures++;
    printf("  FAIL: %s\n", what);
}

static int same(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    return an == bn && memcmp(a, b, an) == 0;
}

/* The carried message `idx` of a Common Data segment, or NULL. */
static const uint8_t *carried(const uint8_t *seg, size_t n, int idx, size_t *ml)
{
    struct dnet_cth_cd_iter it;
    const uint8_t *m; size_t l;
    if (dnet_cth_cd_iter_init(&it, seg, n) != DNET_CTH_OK) return NULL;
    for (int i = 0; dnet_cth_cd_iter_next(&it, &m, &l) == 1; i++)
        if (i == idx) { *ml = l; return m; }
    return NULL;
}

/* ---- 1. codec exactness ---------------------------------------------------- */
static void test_codec_exact(void)
{
    uint8_t b[DNET_CTH_SEG_MAX], m[DNET_CTH_SEG_MAX];
    size_t n = 0, ml = 0;
    struct dnet_cth_cd cd;

    check(dnet_cth_bind_request_build(b, sizeof b, &n) == 0 && same(b, n, k_o15, sizeof k_o15),
          "Bind Request == the VAX host's first segment (01 02 04 00 07 00 10 00)");
    {
        uint8_t legacy[16]; size_t ll = 0;
        check(dnet_cterm_found_host_start_build(legacy, sizeof legacy, &ll) == 0 &&
              same(b, n, legacy, ll), "the spec-field Bind Request equals the client codec's oracle literal");
    }

    /* Common Data #1: Initiate (with the VAX's own max-message 0x1e10) +
     * Characteristics INPUT-COUNT-STATE=2: the VAX's 35-byte segment exactly. */
    static const uint8_t ics[2] = { 0x02, 0x00 };
    dnet_cth_cd_begin(&cd, b, sizeof b);
    dnet_cth_initiate_build(0x1e10, m, sizeof m, &ml);  dnet_cth_cd_add(&cd, m, ml);
    dnet_cth_char_build(0x0208, ics, 2, m, sizeof m, &ml); dnet_cth_cd_add(&cd, m, ml);
    check(same(b, cd.len, k_o17, sizeof k_o17),
          "Common Data {Initiate, Characteristics 0x0208} == the VAX host's 35-byte segment");
    {
        uint8_t legacy[64]; size_t ll = 0;
        check(dnet_cterm_found_host_seg2_build(legacy, sizeof legacy, &ll) == 0 &&
              same(b, cd.len, legacy, ll),
              "the 'length mismatch' seg2 literal IS two carried messages (Initiate + Characteristics)");
    }
    static const uint8_t cc[3] = { 0x03, 0x3b, 0x00 };
    dnet_cth_cd_begin(&cd, b, sizeof b);
    dnet_cth_char_build(0x0202, cc, 3, m, sizeof m, &ml); dnet_cth_cd_add(&cd, m, ml);
    check(same(b, cd.len, k_o18, sizeof k_o18),
          "Characteristics CHARACTER-ATTRIBUTES(^C, 3b, 00) == the VAX host's 11-byte segment");

    check(dnet_cth_unbind_build(DNET_CTH_UNBIND_USER, b, sizeof b, &n) == 0 &&
          same(b, n, k_o57, sizeof k_o57), "Unbind reason 3 == the VAX host's logout segment (02 03 00)");

    /* Start Reads: the VAX's flags/max/timeout in, the VAX's bytes out. */
    struct { const uint8_t *seg; size_t n; uint32_t flags; uint16_t max, tmo;
             const char *prompt; size_t plen; const char *what; } rd[] = {
        { k_o26, sizeof k_o26, 0x00b008, 0x007a, 20, "\r\nUsername: ", 12, "Start Read \"Username:\" (timed 20 s) == VAX bytes" },
        { k_o28, sizeof k_o28, 0x00b808, 0x0080, 20, "\r\nPassword: ", 12, "Start Read \"Password:\" (no-echo) == VAX bytes" },
        { k_o39, sizeof k_o39, 0x0a9008, 0x0100, 0,  "\r\n\0$ ", 5, "Start Read DCL \"$ \" == VAX bytes" },
        { k_l13, sizeof k_l13, 0x00b808, 0x0084, 20, "\r\nPassword: ", 12, "Start Read \"Password:\" (live capture) == VAX bytes" },
    };
    for (size_t i = 0; i < NSEG(rd); i++) {
        struct dnet_cth_read_req rq = { rd[i].flags, rd[i].max, rd[i].tmo };
        dnet_cth_start_read_build(&rq, (const uint8_t *)rd[i].prompt, rd[i].plen, m, sizeof m, &ml);
        dnet_cth_cd_begin(&cd, b, sizeof b); dnet_cth_cd_add(&cd, m, ml);
        check(same(b, cd.len, rd[i].seg, rd[i].n), rd[i].what);
    }

    /* Writes: the raw pass-through and the VMS record form. */
    static const uint8_t esc_gt[2] = { 0x1b, 0x3e };
    dnet_cth_write_build(DNET_CTH_WR_RAW, 0, 0, esc_gt, 2, m, sizeof m, &ml);
    dnet_cth_cd_begin(&cd, b, sizeof b); dnet_cth_cd_add(&cd, m, ml);
    check(same(b, cd.len, k_o38, sizeof k_o38), "raw Write (07 32 00 00 00 ESC >) == VAX bytes");
    {
        static const char t[] = "VAX/VMS V7.3      node VAX2";
        dnet_cth_cd_begin(&cd, b, sizeof b);
        dnet_cth_write_build(DNET_CTH_WR_RECORD, 1, 0x0d, NULL, 0, m, sizeof m, &ml); dnet_cth_cd_add(&cd, m, ml);
        dnet_cth_write_build(DNET_CTH_WR_RECORD, 1, 0x0d, (const uint8_t *)t, strlen(t), m, sizeof m, &ml); dnet_cth_cd_add(&cd, m, ml);
        dnet_cth_write_build(DNET_CTH_WR_RECORD, 1, 0x0d, NULL, 0, m, sizeof m, &ml); dnet_cth_cd_add(&cd, m, ml);
        check(same(b, cd.len, k_o23, sizeof k_o23),
              "three record Writes blocked in one Common Data == the VAX's announcement segment");
    }
}

/* ---- 2. every real message decodes ----------------------------------------- */
static void decode_all(const struct cth_oracle_seg *s, size_t ns, const char *name)
{
    int host_ok = 1, client_ok = 1, reads = 0, rdata = 0, inits = 0;
    char what[160];
    for (size_t i = 0; i < ns; i++) {
        const uint8_t *p = s[i].b; size_t n = s[i].n;
        if (p[0] != DNET_CTH_F_COMMON_DATA) continue;
        struct dnet_cth_cd_iter it;
        const uint8_t *m; size_t ml; int rc;
        if (dnet_cth_cd_iter_init(&it, p, n) != DNET_CTH_OK) { host_ok = client_ok = 0; continue; }
        size_t covered = 2;
        while ((rc = dnet_cth_cd_iter_next(&it, &m, &ml)) == 1) {
            covered += 2 + ml;
            if (s[i].host && m[0] == DNET_CTH_M_START_READ) {
                struct dnet_cth_start_read sr;
                if (dnet_cth_start_read_parse(m, ml, &sr) != DNET_CTH_OK ||
                    sr.termset_len != 0 || sr.end_of_data != sr.dlen ||
                    sr.end_of_prompt != sr.end_of_data)
                    host_ok = 0;
                reads++;
            }
            if (!s[i].host && m[0] == DNET_CTH_M_READ_DATA) {
                struct dnet_cth_read_data rd;
                if (dnet_cth_read_data_parse(m, ml, &rd) != DNET_CTH_OK) client_ok = 0;
                else if ((rd.flags & 0x0f) == DNET_CTH_RDC_TERMINATOR &&
                         !((size_t)rd.term_pos + 1 == rd.dlen && rd.data[rd.term_pos] == 0x0d))
                    client_ok = 0;
                rdata++;
            }
            if (!s[i].host && m[0] == DNET_CTH_M_INITIATE) {
                struct dnet_cth_peer_init pi;
                if (dnet_cth_initiate_parse(m, ml, &pi) != DNET_CTH_OK ||
                    pi.max_msg != 0x03f2 || pi.input_buf != 0x03c0 || !pi.have_bitmap)
                    client_ok = 0;
                inits++;
            }
        }
        if (rc < 0 || covered != n) { if (s[i].host) host_ok = 0; else client_ok = 0; }
    }
    snprintf(what, sizeof what, "%s: every host segment frames exactly as Common Data and every Start Read decodes consistently (%d reads)", name, reads);
    check(host_ok && reads > 0, what);
    snprintf(what, sizeof what, "%s: every client segment frames; Initiate = max 1010 / input 960 / bitmap; %d Read Data decode with CR terminator", name, rdata);
    check(client_ok && inits > 0 && rdata > 0, what);
}

/* ---- 3. the FSM replays a real session ------------------------------------- */
static struct dnet_cth H;

static int pop(uint8_t *b, size_t *n) { return dnet_cth_tx_pop(&H, b, DNET_CTH_SEG_MAX, n); }

static void test_fsm_replay(void)
{
    uint8_t b[DNET_CTH_SEG_MAX]; size_t n = 0, ml = 0;
    uint64_t t = 1000;
    const struct cth_oracle_seg *o = k_oracle_o;   /* session 2: o15 .. o57 */

    dnet_cth_init(&H, 50);
    check(dnet_cth_tx_pop(&H, b, sizeof b, &n) == 0, "nothing is sent before the link is up");
    check(dnet_cth_open(&H) == 0 && pop(b, &n) && same(b, n, o[15].b, o[15].n) && !pop(b, &n),
          "link up: the HOST speaks first, and only the Bind Request");

    check(dnet_cth_rx(&H, o[16].b, o[16].n, t) == 0 && H.state == DNET_CTH_S_INIT_SENT,
          "the real client's Bind Accept is accepted");
    check(pop(b, &n) && n == o[17].n && memcmp(b, o[17].b, 19) == 0 &&
          b[19] == 0xfc && b[20] == 0x03 && memcmp(b + 21, o[17].b + 21, n - 21) == 0,
          "then Initiate+Characteristics: the VAX's bytes, except max-message = OVMX's true 1020");
    check(pop(b, &n) && same(b, n, o[18].b, o[18].n), "then CHARACTER-ATTRIBUTES(^C not OOB): VAX bytes");
    check(!pop(b, &n), "and nothing more until the server's Initiate");

    check(dnet_cth_rx(&H, o[19].b, o[19].n, t) == 0 && dnet_cth_is_bound(&H) &&
          H.peer.max_msg == 1010 && H.seg_max == 1010, "the real client's Initiate binds; its max message (1010) caps our segments");
    check(pop(b, &n) && same(b, n, o[21].b, o[21].n), "the host answers with message 23, the VAX's exact bytes");
    check(dnet_cth_rx(&H, o[20].b, o[20].n, t) == 0 && dnet_cth_rx(&H, o[22].b, o[22].n, t) == 0 &&
          !pop(b, &n), "the client's VMS messages 23 and 19 are accepted and ignored");

    /* LOGINOUT says "\r\nUsername: " and waits: one Write for the line end,
     * one Start Read whose prompt is the tail. */
    static const char ann[] = "\r\nVAX/VMS V7.3      node VAX2\r\n\r\nUsername: ";
    dnet_cth_term_output(&H, (const uint8_t *)ann, strlen(ann), t);
    dnet_cth_tick(&H, t + 10, 1);
    check(!pop(b, &n), "output still arriving (within idle) is held");
    dnet_cth_tick(&H, t + 60, 1);
    const uint8_t *m;
    check(pop(b, &n) && (m = carried(b, n, 0, &ml)) && m[0] == DNET_CTH_M_WRITE &&
          ml == 5 + 33 && memcmp(m, "\x07\x32\x00\x00\x00", 5) == 0 &&
          memcmp(m + 5, "\r\nVAX/VMS V7.3      node VAX2\r\n\r\n", 33) == 0,
          "complete lines go out as one raw Write");
    struct dnet_cth_start_read sr;
    check(pop(b, &n) && (m = carried(b, n, 0, &ml)) && dnet_cth_start_read_parse(m, ml, &sr) == 0 &&
          sr.dlen == 10 && memcmp(sr.data, "Username: ", 10) == 0 && sr.end_of_prompt == 10 &&
          sr.flags == (DNET_CTH_RD_FORMAT | DNET_CTH_RD_TERM_ECHO | DNET_CTH_RD_TERMSET_UNIV) &&
          sr.max_length == 10 + DNET_CTH_READ_MAX && H.read_active,
          "the unterminated tail is the prompt of a Start Read (echo, terminator echo, universal terminators)");

    /* The real client's Read Data "SYSTEM\r" reaches the terminal. */
    check(dnet_cth_rx(&H, o[27].b, o[27].n, t + 100) == 0 && !H.read_active, "the real Read Data completes the read");
    uint8_t in[64];
    size_t k = dnet_cth_term_input(&H, in, sizeof in, 1);
    check(k == 7 && memcmp(in, "SYSTEM\r", 7) == 0, "typed line + its CR terminator are terminal input");
    /* The pty echoes it; the server already did, so the echo is dropped. */
    static const char after[] = "SYSTEM\r\n\r\nPassword: ";
    dnet_cth_term_output(&H, (const uint8_t *)after, strlen(after), t + 110);
    check(H.outlen == strlen("\r\nPassword: ") && memcmp(H.out, "\r\nPassword: ", H.outlen) == 0,
          "the substrate's echo of the typed line is dropped, nothing else");
    dnet_cth_tick(&H, t + 200, 0);
    check(pop(b, &n) && (m = carried(b, n, 0, &ml)) && m[0] == DNET_CTH_M_WRITE, "line end written");
    check(pop(b, &n) && (m = carried(b, n, 0, &ml)) && dnet_cth_start_read_parse(m, ml, &sr) == 0 &&
          (sr.flags & DNET_CTH_RD_NOECHO) && sr.dlen == 10 && memcmp(sr.data, "Password: ", 10) == 0,
          "a terminal with echo off gets a NO-ECHO read (the Password: case)");
    check(dnet_cth_rx(&H, o[29].b, o[29].n, t + 300) == 0 &&
          dnet_cth_term_input(&H, in, sizeof in, 0) == 7 && memcmp(in, "system\r", 7) == 0 &&
          H.echolen == 0, "no-echo input reaches the terminal with no echo expected");

    /* Nothing printed for a while: an empty-prompt read still solicits input. */
    dnet_cth_tick(&H, t + 300 + DNET_CTH_EMPTY_READ_MS - 1, 1);
    check(!pop(b, &n), "no read before the empty-read delay");
    dnet_cth_tick(&H, t + 300 + DNET_CTH_EMPTY_READ_MS, 1);
    check(pop(b, &n) && (m = carried(b, n, 0, &ml)) && dnet_cth_start_read_parse(m, ml, &sr) == 0 &&
          sr.dlen == 0, "an idle session with no prompt still has a read out");
    /* Output while a read is out goes straight through as a Write. */
    dnet_cth_term_output(&H, (const uint8_t *)"$ ", 2, t + 1000);
    dnet_cth_tick(&H, t + 1100, 1);
    check(pop(b, &n) && (m = carried(b, n, 0, &ml)) && m[0] == DNET_CTH_M_WRITE && ml == 7 &&
          !pop(b, &n), "output during an outstanding read is a Write, never a second Start Read");

    /* A big burst is chunked within the peer's max message. */
    static uint8_t big[3000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)('A' + i % 26);
    big[sizeof big - 1] = '\n';
    dnet_cth_term_output(&H, big, sizeof big, t + 2000);
    dnet_cth_tick(&H, t + 2100, 1);
    size_t total = 0; int segs = 0, ok = 1;
    while (pop(b, &n)) {
        segs++;
        if (n > 1010) ok = 0;
        if ((m = carried(b, n, 0, &ml)) && m[0] == DNET_CTH_M_WRITE) total += ml - 5;
    }
    check(ok && total == sizeof big && segs == 3, "3000 bytes go as 3 Writes, each segment <= the server's 1010");

    /* Logout: the last words, then Unbind 02 03 00. */
    dnet_cth_term_output(&H, (const uint8_t *)"  SYSTEM logged out\r\n", 21, t + 3000);
    check(dnet_cth_close(&H) == 0 && pop(b, &n) && (m = carried(b, n, 0, &ml)) && m[0] == DNET_CTH_M_WRITE &&
          pop(b, &n) && same(b, n, o[57].b, o[57].n) && !pop(b, &n) && dnet_cth_is_over(&H),
          "session exit: pending output is written, then the VAX's exact Unbind");
    check(dnet_cth_rx(&H, o[29].b, o[29].n, t) == DNET_CTH_ESTATE, "nothing is accepted after Unbind");

    /* OVMX's own SET HOST client reads this host's output and prompts. */
    {
        dnet_cth_init(&H, 50);
        dnet_cth_open(&H); dnet_cth_rx(&H, o[16].b, o[16].n, t); dnet_cth_rx(&H, o[19].b, o[19].n, t);
        while (pop(b, &n)) {}
        dnet_cth_term_output(&H, (const uint8_t *)"Hi\r\n$ ", 6, t);
        dnet_cth_tick(&H, t + 100, 1);
        enum dnet_cterm_found_term_kind kd; uint8_t tx[64]; size_t tl = 0;
        int w = pop(b, &n) && dnet_cterm_found_terminal_rx(b, n, &kd, tx, sizeof tx, &tl, NULL) == 0 &&
                kd == DNET_CTERM_TK_WRITE && tl == 4 && memcmp(tx, "Hi\r\n", 4) == 0;
        int r = pop(b, &n) && dnet_cterm_found_terminal_rx(b, n, &kd, tx, sizeof tx, &tl, NULL) == 0 &&
                kd == DNET_CTERM_TK_START_READ && tl == 2 && memcmp(tx, "$ ", 2) == 0;
        check(w && r, "OVMX's SET HOST client classifies this host's raw Write and its prompt-carrying Start Read");
    }

    /* A server-side Unbind ends the session. */
    dnet_cth_init(&H, 50);
    dnet_cth_open(&H); while (pop(b, &n)) {}
    static const uint8_t unb[3] = { 0x02, 0x04, 0x00 };
    check(dnet_cth_rx(&H, unb, 3, t) == 0 && dnet_cth_is_over(&H) && H.peer_unbound &&
          H.peer_unbind_reason == 4, "a server Unbind (terminal disconnected) ends the session");

    /* The idle-timeout session (session 1): Read Data completion 5, no data. */
    dnet_cth_init(&H, 50);
    dnet_cth_open(&H); dnet_cth_rx(&H, o[1].b, o[1].n, t); dnet_cth_rx(&H, o[4].b, o[4].n, t);
    while (pop(b, &n)) {}
    H.read_active = 1;
    check(dnet_cth_rx(&H, o[12].b, o[12].n, t) == 0 && !H.read_active && H.inlen == 0,
          "a timed-out read (completion 5, no data) completes with no input");

    /* The OVMX-client capture's client segments bind the same FSM. */
    dnet_cth_init(&H, 50);
    dnet_cth_open(&H);
    int bound = 0;
    for (size_t i = 0; i < NSEG(k_oracle_l); i++)
        if (!k_oracle_l[i].host && dnet_cth_rx(&H, k_oracle_l[i].b, k_oracle_l[i].n, t) == 0 &&
            dnet_cth_is_bound(&H)) bound = 1;
    check(bound && H.inlen == strlen("SYSTEM\rsystem\rWRITE SYS$OUTPUT F$GETSYI(\"NODENAME\")\rSHOW SYSTEM\rLOGOUT\r"),
          "the live capture's client stream binds and delivers all five typed lines");
}

/* ---- 4. fuzz --------------------------------------------------------------- */
static unsigned fz(unsigned *st) { *st = *st * 1103515245u + 12345u; return *st >> 16; }

static void test_fuzz(void)
{
    static uint8_t buf[DNET_CTH_SEG_MAX + 64];
    unsigned st = 0x0a70b;
    const struct cth_oracle_seg *pool[64]; size_t np = 0;
    for (size_t i = 0; i < NSEG(k_oracle_o) && np < 64; i++)
        if (!k_oracle_o[i].host) pool[np++] = &k_oracle_o[i];
    long iters = 0;
    int before = failures;
    for (int r = 0; r < 200000; r++) {
        const struct cth_oracle_seg *s = pool[fz(&st) % np];
        size_t n = s->n;
        memcpy(buf, s->b, n);
        int muts = 1 + (int)(fz(&st) % 4);
        for (int k = 0; k < muts; k++) {
            switch (fz(&st) % 5) {
            case 0: buf[fz(&st) % n] = (uint8_t)fz(&st); break;
            case 1: n = 1 + fz(&st) % n; break;                              /* truncate */
            case 2: if (n + 8 <= sizeof buf) { for (int j = 0; j < 8; j++) buf[n++] = (uint8_t)fz(&st); } break;
            case 3: if (n > 3) { buf[2] = (uint8_t)fz(&st); buf[3] = (uint8_t)fz(&st); } break; /* LENGTH */
            case 4: buf[0] = (uint8_t)(fz(&st) % 12); break;
            }
        }
        /* Put the FSM in a random state, then feed the hostile segment. */
        dnet_cth_init(&H, 1 + fz(&st) % 100);
        int stage = (int)(fz(&st) % 4);
        if (stage >= 1) dnet_cth_open(&H);
        if (stage >= 2) dnet_cth_rx(&H, k_o16, sizeof k_o16, 0);
        if (stage >= 3) dnet_cth_rx(&H, k_o19, sizeof k_o19, 0);
        if (fz(&st) & 1) H.read_active = 1;
        (void)dnet_cth_rx(&H, buf, n, 5);
        (void)dnet_cth_tick(&H, 1000, (int)(fz(&st) & 1));
        uint8_t tb[DNET_CTH_SEG_MAX]; size_t tl;
        while (dnet_cth_tx_pop(&H, tb, sizeof tb, &tl))
            if (tl > H.seg_max || tl > DNET_CTH_SEG_MAX) { failures++; printf("  FAIL: fuzz emitted an oversize segment\n"); }
        uint8_t ib[64];
        while (dnet_cth_term_input(&H, ib, sizeof ib, 1)) {}
        if (H.inlen > DNET_CTH_IN_MAX || H.outlen > DNET_CTH_OUT_MAX || H.echolen > DNET_CTH_ECHO_MAX ||
            H.txcount > DNET_CTH_TXQ) { failures++; printf("  FAIL: fuzz broke a bound\n"); }
        /* And the raw decoders on the same bytes. */
        struct dnet_cth_peer_init pi; struct dnet_cth_read_data rd; struct dnet_cth_start_read sr;
        (void)dnet_cth_initiate_parse(buf, n, &pi);
        (void)dnet_cth_read_data_parse(buf, n, &rd);
        (void)dnet_cth_start_read_parse(buf, n, &sr);
        if (n > 4) {
            (void)dnet_cth_initiate_parse(buf + 4, n - 4, &pi);
            (void)dnet_cth_read_data_parse(buf + 4, n - 4, &rd);
            (void)dnet_cth_start_read_parse(buf + 4, n - 4, &sr);
        }
        iters++;
    }
    char what[96];
    snprintf(what, sizeof what, "%ld mutated client segments: no over-read, no bound broken, no oversize emit", iters);
    check(failures == before, what);
}

int main(void)
{
    test_codec_exact();
    decode_all(k_oracle_o, NSEG(k_oracle_o), "VAX<->VAX oracle");
    decode_all(k_oracle_l, NSEG(k_oracle_l), "VAX host <-> OVMX client");
    test_fsm_replay();
    test_fuzz();
    if (failures) { printf("test_dnet_cterm_host: %d FAILED, %d passed\n", failures, passes); return 1; }
    printf("test_dnet_cterm_host: all %d checks passed\n", passes);
    return 0;
}
