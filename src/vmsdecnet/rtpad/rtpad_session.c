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
    return 0;
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
            s->io->show(s->io->ctx, txt, txtlen);
    } else if (tk == DNET_CTERM_TK_START_READ) {
        /* A Start Read is PROMPT-AND-READ: show the prompt it carries, then
         * one line answers it. Its echo is the HOST's choice (N / T flags); a
         * flags field that does not parse reads as no-echo (rd vms-a353). */
        s->term.writes_recv++;
        s->rd_echo = 0;
        s->rd_term_echo = 0;
        (void)dnet_cterm_start_read_echo(data, len, &s->rd_echo, &s->rd_term_echo);
        if (txtlen)
            s->io->show(s->io->ctx, txt, txtlen);
        s->read_pending = 1;
    } else if (tk == DNET_CTERM_TK_READ_ATTR) {
        /* The host asks for the terminal's characteristics: answer with the
         * read-characteristics reply, echoing its handle. */
        if (dnet_cterm_found_client_readchar_build(rhandle, s->cpdu, sizeof s->cpdu,
                                                   &clen) == 0)
            (void)send_pdu(s, clen);
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
            int frc = dnet_cterm_client_found_next(&s->term, 132, 24, s->cpdu,
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
                s->io->show(s->io->ctx, shown, sn);
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

int rtpad_session_wants_input(const struct rtpad_session *s)
{
    return !s->done && !s->input_eof && dnet_cterm_is_bound(&s->term) &&
           s->read_pending;
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
        st = s->io->read_line(s->io->ctx, line, sizeof line, s->rd_echo,
                              s->rd_term_echo, &n, &term);
        if (!(st & 1)) {
            /* A read the driver cancelled (CTRL/Y) leaves the solicit
             * outstanding for the next line. Anything else -- the terminal
             * hung up -- stops reading it but keeps the link: the session
             * ends on the host's Unbind or a link drop (rd vms-6165). */
            if (st != SS$_ABORT)
                s->input_eof = 1;
            return;
        }
        if (n > sizeof line)
            n = sizeof line;
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
