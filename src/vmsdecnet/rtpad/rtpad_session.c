/*
 * rtpad_session.c -- the CTERM terminal side of $ SET HOST (RTPAD.EXE).
 * rd vms-0a7 S2 / vms-18c9; moved out of DECNETD.EXE's former --set-host
 * client role (rd vms-f54, vms-6165, vms-a353).
 *
 * WHO ECHOES (rd vms-a353). In CTERM the TERMINAL echoes what is typed, as the
 * host's Start Read says (AA-DY88A-TK 4.16.3: N = no echo, T = echo the
 * terminator); the host never sends the typed text back -- in the oracle no
 * host->client frame carries the typed username or password, and a real VAX
 * client shows "Username: SYSTEM" but a blank "Password: " (docs/oracle/
 * vax-sethost-cterm.*). So each Start Read becomes ONE terminal read, issued
 * by the terminal driver with the Start Read's echo; the driver -- not this
 * program -- echoes and edits the line.
 *
 * INPUT IS PROMPT-GATED (rd vms-6165): one host solicit, one line; nothing is
 * sent before the host asks.
 *
 * This file is pure protocol: every I/O goes through struct rtpad_io (RTPAD.EXE
 * implements it with $QIO), so it holds no libc I/O at all.
 */
#include <string.h>

#include "rtpad_session.h"
#include "dnet_cterm_hostfsm.h"     /* dnet_cth_cd_iter: Common Data splitting */
#include "ssdef.h"
#include "rmsdef.h"

int rtpad_session_init(struct rtpad_session *s, const struct rtpad_io *io,
                       int is_terminal)
{
    memset(s, 0, sizeof *s);
    s->io = io;
    s->is_terminal = is_terminal ? 1 : 0;
    s->status = SS$_NORMAL;
    if (dnet_cterm_session_init(&s->term, DNET_CTERM_ROLE_TERMINAL) != 0)
        return -1;
    /* CTRL/Y and CTRL/C are the remote session's from the start */
    if (s->is_terminal && io->set_oob) {
        s->oob_mask = RTPAD_OOB_ALWAYS;
        io->set_oob(io->ctx, s->oob_mask);
    }
    return 0;
}

void rtpad_session_set_sense(struct rtpad_session *s, const uint8_t sense[12])
{
    if (!s || !sense)
        return;
    memcpy(s->sense, sense, sizeof s->sense);
    s->sense_set = 1;
    dnet_cterm_client_set_sense(&s->term, sense);
}

/* The host's Characteristics (CTERM message 11): every CHARACTER-ATTRIBUTES
 * (selector 0x0202: character, mask, attributes) updates the table; a change
 * in which characters are out of band re-arms the terminal's OUTBAND mask.
 * Parsing stops at the first selector whose value length it does not know. */
static void on_characteristics(struct rtpad_session *s, const uint8_t *m, size_t ml)
{
    size_t i = 2;                              /* MSGTYPE, FLAGS */
    while (i + 2 <= ml) {
        unsigned sel = (unsigned)m[i] | ((unsigned)m[i + 1] << 8);
        if (sel != 0x0202 || i + 5 > ml)
            break;
        uint8_t c = m[i + 2], mask = m[i + 3], attr = m[i + 4];
        if (c < 32)
            s->chattr[c] = (uint8_t)((s->chattr[c] & ~mask) | (attr & mask));
        i += 5;
    }
    uint32_t want = RTPAD_OOB_ALWAYS;
    for (unsigned c = 0; c < 32; c++)
        if (s->chattr[c] & 3)
            want |= 1u << c;
    if (s->is_terminal && s->io->set_oob && want != s->oob_mask) {
        s->oob_mask = want;
        s->io->set_oob(s->io->ctx, want);
    }
}

void rtpad_session_oob(struct rtpad_session *s, uint8_t c)
{
    if (!s || s->done || c >= 32 || !dnet_cterm_is_bound(&s->term)) {
        if (s) s->oob_dropped++;
        return;
    }
    /* What a VAX V7.3 client sends (S2 live bench, VAX SET HOST OVMXA, a host
     * that never made CTRL/Y or CTRL/C out of band): CTRL/Y as Out-of-Band
     * 04 00 19 whatever the host's attributes, and CTRL/C as that same CTRL/Y
     * -- unless the host made CTRL/C out of band itself -- with the interrupt
     * shown on its own screen. */
    int interrupt = 0;
    if (c == 0x03 && (s->chattr[0x03] & 3) == 0)
        c = 0x19;
    if (c == 0x19)
        interrupt = 1;
    unsigned kind = s->chattr[c] & 3;
    if (kind == 0 && !interrupt) {
        /* not out of band at the host: it never reaches the local DCL */
        s->oob_dropped++;
        return;
    }
    /* Out-of-Band (CTERM 4): MSGTYPE, FLAGS (bit 0: output was discarded,
     * when the character's attributes say discard), CHARACTER */
    uint8_t body[3] = { 0x04, (uint8_t)((s->chattr[c] & 0x08) ? 1 : 0), c };
    size_t olen = 0;
    if (dnet_cterm_found_envelope_build(sizeof body, body, sizeof body, s->opdu,
                                        sizeof s->opdu, &olen) != DNET_CTERM_OK)
        return;
    if (!(s->io->link_send(s->io->ctx, s->opdu, olen) & 1))
        return;
    s->oob_sent++;
    (void)kind;
    if (!interrupt)
        return;
    /* The interrupt is shown by the local driver (RTPAD arms the OUTBAND mask
     * with IO$M_TT_ABORT: the driver shows " Interrupt " / *INTERRUPT* for
     * the terminal it is and ends the read in progress).
     *
     * Then, as a VAX V7.3 client (S2 live bench and its VAX-only probes,
     * tests/lab/captures/vms-0a7-s2-20261103: every observation fits): with
     * a host read outstanding, a CTRL/Y that follows another with fewer than
     * two lines sent to the host since is "repeating" -- the client asks
     * "Are you repeating ^Y to abort the remote session on node X?" and the
     * host's read stays outstanding; any other CTRL/Y ends the host's read
     * (Read Data, completion code 3, no data: 03 03 00 00 00 00 00 00 on the
     * wire after the Out-of-Band) and the host solicits again. */
    int repeat = s->y_seen && s->read_pending && s->lines_since_y < 2;
    s->y_seen = 1;
    s->lines_since_y = 0;
    if (repeat) {
        s->ask_pending = 1;
        return;
    }
    if (s->read_pending) {
        size_t clen = 0;
        if (dnet_cterm_found_read_data_build_code(NULL, 0, 3, s->opdu, sizeof s->opdu,
                                                  &clen) != 0)
            return;
        if (!(s->io->link_send(s->io->ctx, s->opdu, clen) & 1))
            return;
        s->term.reads_sent++;
        s->read_pending = 0;
        s->rd_plen = s->rd_init = 0;
    }
}

/* Host output to the screen. */
static void show(struct rtpad_session *s, const uint8_t *b, size_t n)
{
    if (n)
        s->io->show(s->io->ctx, b, n);
}

static void fail(struct rtpad_session *s, uint32_t st)
{
    s->status = st;
    s->done = 1;
}

static int send_pdu(struct rtpad_session *s, size_t n)
{
    uint32_t st = s->io->link_send(s->io->ctx, s->cpdu, n);
    if (!(st & 1)) {
        fail(s, st);
        return -1;
    }
    return 0;
}

void rtpad_session_linkup(struct rtpad_session *s)
{
    if (dnet_cterm_client_open(&s->term) != 0)
        fail(s, SS$_ABORT);
}

/* Send one input line as the answer to the outstanding Start Read. */
static void send_line(struct rtpad_session *s, const uint8_t *line, size_t n,
                      uint8_t term)
{
    size_t clen = 0;
    if (dnet_cterm_found_read_data_build(line, n, term, s->cpdu, sizeof s->cpdu,
                                         &clen) != 0) {
        fail(s, SS$_ABORT);
        return;
    }
    if (send_pdu(s, clen) != 0)
        return;
    s->term.reads_sent++;
    s->read_pending = 0;
    s->lines_since_y++;
}

/* One BOUND-phase CTERM message (re-wrapped in its own 09 envelope). */
static void on_bound_msg(struct rtpad_session *s, const uint8_t *data, size_t len)
{
    size_t clen = 0;
    enum dnet_cterm_found_term_kind tk = DNET_CTERM_TK_NONE;
    uint8_t txt[DNET_CTERM_MAX_DATA];
    size_t txtlen = 0;
    uint8_t rhandle[4] = { 0, 0, 0, 0 };
    if (dnet_cterm_found_terminal_rx(data, len, &tk, txt, sizeof txt, &txtlen,
                                     rhandle) != DNET_CTERM_OK)
        return;
    if (tk == DNET_CTERM_TK_WRITE) {
        s->term.writes_recv++;
        if (txtlen)
            show(s, txt, txtlen);
    } else if (tk == DNET_CTERM_TK_START_READ) {
        /* A Start Read is PROMPT-AND-READ: show the prompt it carries, then
         * one line answers it. Its echo is the HOST's choice (N / T flags); a
         * flags field that does not parse reads as no-echo (rd vms-a353). */
        s->term.writes_recv++;
        s->rd_echo = 0;
        s->rd_term_echo = 0;
        (void)dnet_cterm_start_read_echo(data, len, &s->rd_echo, &s->rd_term_echo);
        /* A timed read (a VAX host times LOGINOUT's: 20 s) ends when the time
         * runs out, and the host is told so -- without it a remote LOGINOUT
         * waits on its retry read for ever (rd vms-0a7 S2 live bench). */
        (void)dnet_cterm_start_read_timeout(data, len, &s->rd_timeout);
        /* A terminal takes the prompt WITH its read (IO$_READPROMPT), as
         * VMS's driver shows a read's prompt; a file input just shows it. */
        s->rd_plen = s->rd_init = 0;
        if (s->is_terminal && txtlen <= sizeof s->rd_prompt) {
            /* END-OF-PROMPT splits the prompt from initial input (DCL's
             * recall: "$ " then the recalled command, AA-DY88A-TK 4.16.3) */
            struct dnet_cth_start_read sr;
            size_t eop = txtlen;
            if (len > 4 && dnet_cth_start_read_parse(data + 4, len - 4, &sr) == DNET_CTH_OK &&
                sr.end_of_prompt <= txtlen && sr.end_of_data <= txtlen &&
                sr.end_of_prompt <= sr.end_of_data)
                eop = sr.end_of_prompt;
            memcpy(s->rd_prompt, txt, txtlen);
            s->rd_plen = eop;
            s->rd_init = txtlen - eop;
        } else if (txtlen) {
            show(s, txt, txtlen);
        }
        s->read_pending = 1;
    } else if (tk == DNET_CTERM_TK_READ_ATTR) {
        /* The host asks for the terminal's characteristics: answer with the
         * read-characteristics reply, echoing its handle -- THIS terminal's
         * IO$_SENSEMODE image once RTPAD has it. */
        if (dnet_cterm_found_client_readchar_build_sense(rhandle,
                                                         s->sense_set ? s->sense : NULL,
                                                         s->cpdu, sizeof s->cpdu,
                                                         &clen) == 0)
            (void)send_pdu(s, clen);
    } else if (len > 4 && data[4] == 0x0b) {
        on_characteristics(s, data + 4, len - 4);
    }
    /* Anything else: acknowledged by NSP, nothing to show or answer. */
}

void rtpad_session_rx(struct rtpad_session *s, const uint8_t *data, size_t len)
{
    size_t clen = 0;
    if (s->done || !data || len == 0)
        return;
    if (dnet_cterm_state_of(&s->term) == DNET_CTERM_S_BINDING) {
        /* FOUNDATION (rd vms-6165): consume the host's foundation message, then
         * send every client reply now due. WIDTH/PAGE are the captured 132/24
         * so every byte sent is the oracle's. */
        int prog = 0;
        if (dnet_cterm_client_found_rx(&s->term, data, len, &prog) != DNET_CTERM_OK)
            return;
        for (;;) {
            int frc = dnet_cterm_client_found_next(&s->term,
                                                   s->sense_set ? (uint16_t)(s->sense[2] | (s->sense[3] << 8)) : 132,
                                                   s->sense_set ? s->sense[7] : 24, s->cpdu,
                                                   sizeof s->cpdu, &clen);
            if (frc != DNET_CTERM_OK || clen == 0)
                break;
            if (send_pdu(s, clen) != 0)
                return;
        }
        if (dnet_cterm_is_bound(&s->term))
            s->bound_ever = 1;
        return;
    }
    if (!dnet_cterm_is_bound(&s->term))
        return;

    /* BOUND: terminal I/O inside 09 Common Data messages. ONE segment carries
     * SEVERAL CTERM messages, each LENGTH-prefixed (AA-DY89A-TK); a real VAX
     * packs a whole screen of Writes into one, so each is handled on its own. */
    struct dnet_cth_cd_iter it;
    if (dnet_cth_cd_iter_init(&it, data, len) != DNET_CTH_OK)
        return;
    const uint8_t *sub;
    size_t sublen;
    while (!s->done && dnet_cth_cd_iter_next(&it, &sub, &sublen) == 1) {
        if (sublen == 0)
            continue;
        if (sub[0] == 0x07) {                     /* Write: render it properly */
            uint8_t shown[DNET_CTERM_MAX_DATA + 512];
            size_t sn = 0;
            s->term.writes_recv++;
            if (dnet_cterm_write_render(sub, sublen, &s->skip_lf, shown,
                                        sizeof shown, &sn) == 0 && sn)
                show(s, shown, sn);
            continue;
        }
        uint8_t one[4 + DNET_CTERM_MAX_DATA + 64];
        if (sublen + 4 > sizeof one)
            continue;
        one[0] = 0x09; one[1] = 0x00;
        one[2] = (uint8_t)(sublen & 0xff); one[3] = (uint8_t)(sublen >> 8);
        memcpy(one + 4, sub, sublen);
        on_bound_msg(s, one, sublen + 4);
    }
}

int rtpad_session_read_now(const struct rtpad_session *s)
{
    /* Every solicited read starts at once, its prompt with it: RTPAD serves
     * the link from an AST while the read is outstanding, so what is typed is
     * echoed and edited as it comes (a paste longer than the type-ahead
     * buffer is read whole) and host output is shown as it arrives (rd
     * vms-fc0). */
    return s->read_pending || s->ask_pending;
}

int rtpad_session_wants_input(const struct rtpad_session *s)
{
    return !s->done && !s->input_eof && dnet_cterm_is_bound(&s->term) &&
           (s->read_pending || s->ask_pending);
}

void rtpad_session_set_node(struct rtpad_session *s, const char *node)
{
    size_t k = 0;
    while (node && node[k] && node[k] != ':' && k + 1 < sizeof s->node) {
        s->node[k] = node[k];
        k++;
    }
    s->node[k] = '\0';
}

/* "Are you repeating ^Y to abort the remote session on node X?" -- a local
 * read, its answer never the host's (VAX V7.3 client: an answer "XYZ" runs
 * nothing remote). Y ends the session: Unbind, "%REM-S-END ..." and $STATUS
 * %X00000001; anything else leaves the host's read outstanding, posted again
 * with its prompt (S2 probes). */
static void ask_abort(struct rtpad_session *s)
{
    static const char q1[] = "Are you repeating ^Y to abort the remote session on node ";
    uint8_t prompt[sizeof q1 + sizeof s->node + 4];
    uint8_t ans[64];
    size_t pn = 0, nl = strlen(s->node), n = 0;
    uint8_t term = 0x0d;

    s->ask_pending = 0;
    memcpy(prompt, q1, sizeof q1 - 1); pn = sizeof q1 - 1;
    memcpy(prompt + pn, s->node, nl); pn += nl;
    prompt[pn++] = '?';
    prompt[pn++] = ' ';
    uint32_t st = s->io->read_line(s->io->ctx, prompt, pn, NULL, 0, ans, sizeof ans,
                                   1, 1, 0, &n, &term);
    if ((st & 1) && n > 0 && (ans[0] == 'Y' || ans[0] == 'y')) {
        s->user_abort = 1;
        s->status = SS$_NORMAL;
        s->done = 1;
    }
}

void rtpad_session_input(struct rtpad_session *s)
{
    uint8_t line[DNET_CTERM_MAX_DATA];
    size_t n = 0;
    uint32_t st;

    if (!rtpad_session_wants_input(s))
        return;
    if (s->is_terminal) {
        uint8_t term = 0x0d;
        if (s->ask_pending) {
            ask_abort(s);
            return;
        }
        st = s->io->read_line(s->io->ctx, s->rd_prompt, s->rd_plen,
                              s->rd_prompt + s->rd_plen, s->rd_init,
                              line, sizeof line, s->rd_echo,
                              s->rd_term_echo, s->rd_timeout, &n, &term);
        if (st == SS$_TIMEOUT && s->rd_timeout) {
            /* Read Data, completion code 5: what was typed, no terminator */
            size_t clen = 0;
            if (n > sizeof line)
                n = sizeof line;
            if (dnet_cterm_found_read_data_build_code(line, n, 5, s->cpdu,
                                                      sizeof s->cpdu, &clen) != 0) {
                fail(s, SS$_ABORT);
                return;
            }
            if (send_pdu(s, clen) != 0)
                return;
            s->term.reads_sent++;
            s->read_pending = 0;
            s->rd_plen = s->rd_init = 0;
            return;
        }
        if (!(st & 1)) {
            /* A read the driver cancelled (CTRL/Y) leaves the solicit
             * outstanding for the next line. Anything else -- the terminal
             * hung up -- stops reading it but keeps the link: the session
             * ends on the host's Unbind or a link drop (rd vms-6165). */
            if (st != SS$_ABORT && st != SS$_CANCEL)
                s->input_eof = 1;
            else if (s->ask_pending)
                ask_abort(s);          /* a repeated CTRL/Y ended the read */
            return;
        }
        if (n > sizeof line)
            n = sizeof line;
        s->rd_plen = s->rd_init = 0;
        send_line(s, line, n, term);
        return;
    }
    /* A file: the next record is the next line; nothing is echoed. */
    st = s->io->read_record(s->io->ctx, line, sizeof line, &n);
    if (!(st & 1)) {
        s->input_eof = 1;
        return;
    }
    if (n > sizeof line)
        n = sizeof line;
    send_line(s, line, n, 0x0d);
}

void rtpad_session_finish(struct rtpad_session *s, const char *local_node)
{
    if (dnet_cterm_is_bound(&s->term)) {
        size_t clen = 0;
        if (dnet_cterm_unbind(&s->term, DNET_CTERM_UNBIND_NORMAL, s->cpdu,
                              sizeof s->cpdu, &clen) == 0)
            (void)s->io->link_send(s->io->ctx, s->cpdu, clen);
    }
    /* CONTROL RETURNS with the VMS message (docs/oracle/vax-sethost-cterm.
     * console.txt), naming the LOCAL node, once a session was established. It
     * goes as a record, so it starts on a line of its own (rd vms-a353). */
    if (s->bound_ever) {
        char msg[96];
        static const char pre[] = "%REM-S-END, control returned to node ";
        size_t k = 0, nl = local_node ? strlen(local_node) : 0;
        if (nl > sizeof msg - sizeof pre - 3)
            nl = sizeof msg - sizeof pre - 3;
        memcpy(msg, pre, sizeof pre - 1);
        k = sizeof pre - 1;
        if (nl) {
            memcpy(msg + k, local_node, nl);
            k += nl;
        }
        msg[k++] = ':';
        msg[k++] = ':';
        msg[k] = '\0';
        s->io->record(s->io->ctx, msg);
    }
}
