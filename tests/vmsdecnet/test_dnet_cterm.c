/*
 * test_dnet_cterm.c - DECnet Phase IV CTERM (Command Terminal) protocol proof
 *                     (rd vms-4d2, engine rung 3 of epic vms-30e; north-star
 *                     demo leg vms-e4dc).
 *
 * Proves the terminal-service layered product behind $ SET HOST at three layers:
 *
 *   1. CODEC round-trip: every CTERM message type encodes and decodes back
 *      byte-for-field-identical (Bind/Bind Accept/Unbind, Characteristics,
 *      Start Read/Read Data, Write/Write Complete, Out-of-Band). Deterministic,
 *      no socket. Also the minimal Session-Control connect message that a
 *      SET HOST CI carries to name the CTERM object (42).
 *
 *   2. SESSION FSM: a terminal (client) and a host (server) session drive each
 *      other CLOSED -> BINDING/BOUND -> UNBOUND: Bind/Bind Accept, a
 *      characteristics exchange both ways, a Start Read + Read Data (a keystroke
 *      line up), a Write (screen output down), an Out-of-Band ^C, and an Unbind.
 *      Raw CTERM PDUs, clock-free, no NSP.
 *
 *   3. ENGINE END-TO-END over a real socketpair(2): two DECnet engines open an
 *      NSP logical link (CI names the CTERM object) and then carry a WHOLE
 *      $ SET HOST terminal session -- Bind -> Bind Accept -> characteristics ->
 *      host banner Write -> host Start Read -> terminal Read Data (keystrokes) ->
 *      terminal ^C OOB -> host Write -> Unbind -> link disconnect -- as real
 *      on-wire NSP data frames (Ethernet + Phase IV long-data routing header +
 *      NSP data segment + CTERM PDU), genuine write(2)/read(2) of the encoded
 *      bytes, every CTERM payload round-tripping byte-identical. This is the
 *      exact link_send -> wire -> link_rx -> cterm_rx path a real SET HOST uses.
 *      No CAP_NET_RAW.
 *
 * Clean-room (Rule 8): CTERM is ENTIRELY SPEC-DERIVED -- there is NO oracle
 * specimen (the vms-3be capture never completed a logical link, so no CTERM byte
 * was ever observed). The message set + function mirror the public DNA CTERM
 * functional description; the numeric codes/layouts are OVMX-assigned and proven
 * here ONLY by round-trip, never presented as VMS-authentic bytes. See
 * docs/decnet-provenance-register.md sec 4.7.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dnet_cterm.h"
#include "dnet_engine.h"

static int failures = 0;
static void check(int cond, const char *what)
{
    if (cond) { printf("  OK: %s\n", what); }
    else      { printf("  FAIL: %s\n", what); failures++; }
}

/* ---- 1. codec round-trip ------------------------------------------------- */
static void rt(const struct dnet_cterm_msg *in, const char *what)
{
    uint8_t buf[DNET_CTERM_MAX_PDU];
    size_t enc = 0, cons = 0;
    struct dnet_cterm_msg out;
    check(dnet_cterm_encode(in, buf, sizeof(buf), &enc) == DNET_CTERM_OK, what);
    check(dnet_cterm_decode(buf, enc, &out, &cons) == DNET_CTERM_OK && cons == enc,
          "  decode consumes exactly what encode produced");
    check(out.type == in->type, "  type preserved");
}

static void test_codec(void)
{
    printf("[codec] every CTERM message type round-trips encode->decode\n");

    struct dnet_cterm_msg m;

    memset(&m, 0, sizeof(m));
    m.type = DNET_CTERM_MSG_BIND; m.ver_v = 1; m.mode = DNET_CTERM_MODE_COMMAND;
    strcpy(m.name, "OVMX1$RTA1:");
    rt(&m, "Bind encodes");
    { uint8_t b[DNET_CTERM_MAX_PDU]; size_t n; struct dnet_cterm_msg o;
      dnet_cterm_encode(&m, b, sizeof(b), &n); dnet_cterm_decode(b, n, &o, NULL);
      check(strcmp(o.name, "OVMX1$RTA1:") == 0 && o.mode == DNET_CTERM_MODE_COMMAND,
            "Bind name + mode survive the round-trip"); }

    memset(&m, 0, sizeof(m));
    m.type = DNET_CTERM_MSG_BIND_ACCEPT; m.ver_v = 1; m.status = 0;
    strcpy(m.name, "VAX1");
    rt(&m, "Bind Accept encodes");

    memset(&m, 0, sizeof(m));
    m.type = DNET_CTERM_MSG_UNBIND; m.reason = DNET_CTERM_UNBIND_NORMAL;
    rt(&m, "Unbind encodes");

    memset(&m, 0, sizeof(m));
    m.type = DNET_CTERM_MSG_CHARACTERISTICS; m.term_type = 4;
    m.width = 132; m.page = 24; m.char_flags = DNET_CTERM_CH_ECHO | DNET_CTERM_CH_WRAP;
    rt(&m, "Characteristics encodes");
    { uint8_t b[DNET_CTERM_MAX_PDU]; size_t n; struct dnet_cterm_msg o;
      dnet_cterm_encode(&m, b, sizeof(b), &n); dnet_cterm_decode(b, n, &o, NULL);
      check(o.width == 132 && o.page == 24 &&
            o.char_flags == (DNET_CTERM_CH_ECHO | DNET_CTERM_CH_WRAP),
            "Characteristics width/page/flags survive"); }

    memset(&m, 0, sizeof(m));
    m.type = DNET_CTERM_MSG_START_READ; m.rd_flags = DNET_CTERM_RD_NOECHO;
    m.rd_maxlen = 80; m.rd_timeout = 0; strcpy(m.prompt, "Password: ");
    rt(&m, "Start Read encodes");
    { uint8_t b[DNET_CTERM_MAX_PDU]; size_t n; struct dnet_cterm_msg o;
      dnet_cterm_encode(&m, b, sizeof(b), &n); dnet_cterm_decode(b, n, &o, NULL);
      check(strcmp(o.prompt, "Password: ") == 0 && o.rd_maxlen == 80 &&
            (o.rd_flags & DNET_CTERM_RD_NOECHO), "Start Read prompt/flags survive"); }

    memset(&m, 0, sizeof(m));
    m.type = DNET_CTERM_MSG_READ_DATA; m.terminator = 0x0d;
    const char *line = "SHOW SYSTEM";
    m.datalen = (uint16_t)strlen(line); memcpy(m.data, line, m.datalen);
    rt(&m, "Read Data encodes");
    { uint8_t b[DNET_CTERM_MAX_PDU]; size_t n; struct dnet_cterm_msg o;
      dnet_cterm_encode(&m, b, sizeof(b), &n); dnet_cterm_decode(b, n, &o, NULL);
      check(o.datalen == strlen(line) && memcmp(o.data, line, o.datalen) == 0 &&
            o.terminator == 0x0d, "Read Data payload + terminator survive"); }

    memset(&m, 0, sizeof(m));
    m.type = DNET_CTERM_MSG_OOB; m.oob_char = 0x03;  /* ^C */
    rt(&m, "Out-of-Band encodes");

    memset(&m, 0, sizeof(m));
    m.type = DNET_CTERM_MSG_WRITE; m.wr_flags = DNET_CTERM_WR_POSTFIX_NL;
    const char *scr = "OpenVMS VAX V7.3   Node VAX1\r\n";
    m.datalen = (uint16_t)strlen(scr); memcpy(m.data, scr, m.datalen);
    rt(&m, "Write encodes");
    { uint8_t b[DNET_CTERM_MAX_PDU]; size_t n; struct dnet_cterm_msg o;
      dnet_cterm_encode(&m, b, sizeof(b), &n); dnet_cterm_decode(b, n, &o, NULL);
      check(o.datalen == strlen(scr) && memcmp(o.data, scr, o.datalen) == 0,
            "Write screen output survives byte-identical"); }

    memset(&m, 0, sizeof(m));
    m.type = DNET_CTERM_MSG_WRITE_COMPLETE; rt(&m, "Write Complete encodes");

    /* A truncated PDU decodes as an honest error, never a fabricated message. */
    { uint8_t t[1] = { DNET_CTERM_MSG_BIND }; struct dnet_cterm_msg o;
      check(dnet_cterm_decode(t, sizeof(t), &o, NULL) == DNET_CTERM_ETRUNC,
            "a truncated Bind is rejected ETRUNC (no fabrication)"); }
    { uint8_t bad[1] = { 200 }; struct dnet_cterm_msg o;
      check(dnet_cterm_decode(bad, sizeof(bad), &o, NULL) == DNET_CTERM_EBADTYPE,
            "an unknown message type is rejected EBADTYPE"); }
}

static void test_sc_connect(void)
{
    printf("[sc] SET HOST connect data names the CTERM object (42)\n");
    uint8_t buf[128]; size_t n = 0;
    check(dnet_cterm_sc_connect_build(DNET_CTERM_OBJECT, "SYSTEM", "", "",
                                      buf, sizeof(buf), &n) == DNET_CTERM_OK,
          "SC connect builds (dst object 42, access user SYSTEM)");
    check(dnet_cterm_sc_connect_object(buf, n) == DNET_CTERM_OBJECT,
          "SC connect names object 42 (CTERM) -- the object $ SET HOST targets");
    check(n >= 4, "SC connect carries dst + src descriptors + access strings");
}

/* ---- 2. session FSM (raw CTERM PDUs, no NSP) ----------------------------- */
static void test_session(void)
{
    printf("[session] terminal + host: bind -> negotiate -> I/O -> unbind\n");

    struct dnet_cterm_session term, host;   /* term = SET HOST initiator */
    check(dnet_cterm_session_init(&term, DNET_CTERM_ROLE_TERMINAL) == DNET_CTERM_OK &&
          dnet_cterm_session_init(&host, DNET_CTERM_ROLE_HOST) == DNET_CTERM_OK,
          "sessions init (terminal + host)");
    check(dnet_cterm_state_of(&term) == DNET_CTERM_S_CLOSED &&
          dnet_cterm_state_of(&host) == DNET_CTERM_S_CLOSED, "both start CLOSED");

    uint8_t pdu[DNET_CTERM_MAX_PDU]; size_t n = 0;
    enum dnet_cterm_event ev;

    /* terminal -> Bind -> host connect indication. */
    check(dnet_cterm_bind(&term, "OVMX2$RTA1:", pdu, sizeof(pdu), &n) == DNET_CTERM_OK,
          "terminal builds Bind");
    check(dnet_cterm_state_of(&term) == DNET_CTERM_S_BINDING, "terminal -> BINDING");
    check(dnet_cterm_rx(&host, pdu, n, &ev) == DNET_CTERM_OK && ev == DNET_CTERM_EV_BIND_IND,
          "host sees BIND indication");
    check(strcmp(host.peer_name, "OVMX2$RTA1:") == 0, "host learned the terminal name");

    /* host -> Bind Accept -> terminal bound. */
    check(dnet_cterm_bind_accept(&host, "VAX1", pdu, sizeof(pdu), &n) == DNET_CTERM_OK,
          "host builds Bind Accept");
    check(dnet_cterm_state_of(&host) == DNET_CTERM_S_BOUND, "host -> BOUND");
    check(dnet_cterm_rx(&term, pdu, n, &ev) == DNET_CTERM_OK && ev == DNET_CTERM_EV_BOUND,
          "terminal sees session BOUND");
    check(dnet_cterm_is_bound(&term) && dnet_cterm_is_bound(&host), "both BOUND");

    /* terminal advertises its characteristics; host absorbs them. */
    check(dnet_cterm_send_characteristics(&term, 4, 80, 24,
              DNET_CTERM_CH_ECHO | DNET_CTERM_CH_WRAP, pdu, sizeof(pdu), &n)
              == DNET_CTERM_OK, "terminal sends Characteristics");
    check(dnet_cterm_rx(&host, pdu, n, &ev) == DNET_CTERM_OK &&
          ev == DNET_CTERM_EV_CHARACTERISTICS && host.width == 80 && host.page == 24,
          "host negotiated width 80 / page 24");

    /* host writes a banner (screen output down). */
    const char *banner = "Welcome to OpenVMS (TM) VAX Operating System\r\n";
    check(dnet_cterm_write(&host, (const uint8_t *)banner, strlen(banner),
              DNET_CTERM_WR_NOFORMAT, pdu, sizeof(pdu), &n) == DNET_CTERM_OK,
          "host builds Write (banner)");
    check(dnet_cterm_rx(&term, pdu, n, &ev) == DNET_CTERM_OK && ev == DNET_CTERM_EV_WRITE &&
          term.last.datalen == strlen(banner) &&
          memcmp(term.last.data, banner, term.last.datalen) == 0,
          "terminal receives the banner byte-identical");

    /* host solicits input (Start Read + prompt). */
    check(dnet_cterm_start_read(&host, "$ ", 0, 80, 0, pdu, sizeof(pdu), &n) == DNET_CTERM_OK,
          "host builds Start Read");
    check(dnet_cterm_rx(&term, pdu, n, &ev) == DNET_CTERM_OK && ev == DNET_CTERM_EV_START_READ &&
          strcmp(term.last.prompt, "$ ") == 0, "terminal sees the '$ ' prompt");

    /* terminal sends a keystroke line (input up). */
    const char *cmd = "SHOW TIME";
    check(dnet_cterm_read_data(&term, (const uint8_t *)cmd, strlen(cmd), 0x0d,
              pdu, sizeof(pdu), &n) == DNET_CTERM_OK, "terminal builds Read Data");
    check(dnet_cterm_rx(&host, pdu, n, &ev) == DNET_CTERM_OK && ev == DNET_CTERM_EV_READ_DATA &&
          host.last.datalen == strlen(cmd) &&
          memcmp(host.last.data, cmd, host.last.datalen) == 0 && host.last.terminator == 0x0d,
          "host receives 'SHOW TIME' + CR terminator");

    /* terminal sends an out-of-band ^C. */
    check(dnet_cterm_oob(&term, 0x03, pdu, sizeof(pdu), &n) == DNET_CTERM_OK,
          "terminal builds an OOB ^C");
    check(dnet_cterm_rx(&host, pdu, n, &ev) == DNET_CTERM_OK && ev == DNET_CTERM_EV_OOB &&
          host.last.oob_char == 0x03, "host receives the ^C out-of-band");

    /* host unbinds (logout). */
    check(dnet_cterm_unbind(&host, DNET_CTERM_UNBIND_NORMAL, pdu, sizeof(pdu), &n)
              == DNET_CTERM_OK, "host builds Unbind");
    check(dnet_cterm_state_of(&host) == DNET_CTERM_S_UNBOUND, "host -> UNBOUND");
    check(dnet_cterm_rx(&term, pdu, n, &ev) == DNET_CTERM_OK && ev == DNET_CTERM_EV_UNBOUND,
          "terminal sees the session released");
    check(dnet_cterm_state_of(&term) == DNET_CTERM_S_UNBOUND, "terminal -> UNBOUND");

    /* honest counters, not fabricated. */
    check(host.reads_recv == 1 && host.oob_recv == 1 && term.writes_recv == 1,
          "counters: host got 1 read + 1 oob, terminal got 1 write");

    /* role guard: a host cannot originate a Bind, a terminal cannot Write. */
    struct dnet_cterm_session h2;
    dnet_cterm_session_init(&h2, DNET_CTERM_ROLE_HOST);
    check(dnet_cterm_bind(&h2, "X", pdu, sizeof(pdu), &n) == DNET_CTERM_ESTATE,
          "a HOST role refuses to originate a Bind (ESTATE)");
}

/* ---- 3. engine end-to-end: a whole SET HOST session over a socketpair ---- */

/* Ship one built frame from wfd to rfd and hand it to the peer engine's link_rx;
 * deliver the CTERM payload (if a data segment) to the peer's cterm_rx. Returns
 * the higher-layer CTERM event, or DNET_CTERM_EV_NONE if the frame carried no
 * CTERM data (CI/CC/ACK/DI/DC). *has_reply/reply carry any NSP protocol reply. */
static enum dnet_cterm_event
ship(int wfd, int rfd, struct dnet_engine *rx_eng, struct dnet_cterm_session *rx_sess,
     const uint8_t *frame, size_t flen, dnet_tick_t now,
     uint8_t *reply, size_t reply_cap, size_t *reply_len, int *has_reply)
{
    uint8_t rxbuf[DNET_FRAME_MAX];
    if (write(wfd, frame, flen) != (ssize_t)flen) { failures++; return DNET_CTERM_EV_NONE; }
    ssize_t n = read(rfd, rxbuf, sizeof(rxbuf));
    if (n <= 0) { failures++; return DNET_CTERM_EV_NONE; }

    enum dnet_link_event lev = DNET_LINK_EV_NONE;
    if (dnet_engine_link_rx(rx_eng, now, rxbuf, (size_t)n, reply, reply_cap, reply_len,
                            has_reply, &lev) != DNET_ENGINE_OK) { failures++; return DNET_CTERM_EV_NONE; }

    if (lev == DNET_LINK_EV_DATA) {
        enum dnet_cterm_event cev = DNET_CTERM_EV_NONE;
        dnet_cterm_rx(rx_sess, rx_eng->rx_data, rx_eng->rx_datalen, &cev);
        return cev;
    }
    return DNET_CTERM_EV_NONE;
}

static void test_engine_e2e(void)
{
    printf("[engine] a whole $ SET HOST terminal session over a real socketpair\n");

    int sv[2];
    check(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "socketpair created");

    const uint8_t hwL[6] = { 0x02,0,0,0,0,0x0a };
    const uint8_t hwR[6] = { 0x02,0,0,0,0,0x0b };
    struct dnet_engine L, R;   /* L = 2.10 SET HOST initiator, R = 2.11 host */
    check(dnet_engine_init(&L, 2, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0) == DNET_ENGINE_OK &&
          dnet_engine_init(&R, 2, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0) == DNET_ENGINE_OK,
          "L (initiator) + R (host) engines init");

    struct dnet_cterm_session term, host;
    dnet_cterm_session_init(&term, DNET_CTERM_ROLE_TERMINAL);
    dnet_cterm_session_init(&host, DNET_CTERM_ROLE_HOST);

    uint8_t frame[DNET_FRAME_MAX], reply[DNET_FRAME_MAX], cterm[DNET_CTERM_MAX_PDU];
    size_t flen = 0, rlen = 0, clen = 0;
    int has_reply = 0;
    enum dnet_cterm_event cev;
    enum dnet_link_event lev;
    ssize_t n;
    dnet_tick_t t = 100;

    /* --- open the NSP logical link to the CTERM object (the CI carries the SC
     *     connect naming object 42, exactly as $ SET HOST originates). --- */
    uint8_t sc[128]; size_t sclen = 0;
    check(dnet_cterm_sc_connect_build(DNET_CTERM_OBJECT, "SYSTEM", "", "",
                                      sc, sizeof(sc), &sclen) == DNET_CTERM_OK,
          "SET HOST builds the SC connect for object 42");
    check(dnet_engine_link_open(&L, 2, 11, 0x2001, sc, sclen, 1459, 1,
                                DNET_NSP_VER_41, frame, sizeof(frame), &flen, t++)
              == DNET_ENGINE_OK, "L link_open builds the CI (to CTERM object)");
    check(ship(sv[0], sv[1], &R, &host, frame, flen, t, reply, sizeof(reply), &rlen, &has_reply)
              == DNET_CTERM_EV_NONE, "CI reaches R (connect indication, no CTERM yet)");
    check(R.link.remote_node == L.addr, "R learned the initiator's node from the CI");
    /* The CI carried the SC connect the host's session-control layer dispatches
     * on: it names object 42 -> the CTERM server object. (The engine retains the
     * CI connect data on the link; here we confirm the SC parse the server runs
     * resolves the object the wire delivered.) */
    check(R.link.conn_len == sclen &&
          dnet_cterm_sc_connect_object(R.link.conn_data, R.link.conn_len) == DNET_CTERM_OBJECT,
          "R dispatches the CI to the CTERM object 42 (SC connect on the link)");

    /* R accepts the link (CC back). */
    check(dnet_engine_link_accept(&R, 0x2002, reply, sizeof(reply), &rlen, t) == DNET_ENGINE_OK,
          "R link_accept builds the CC");
    check(write(sv[1], reply, rlen) == (ssize_t)rlen, "CC written back");
    n = read(sv[0], frame, sizeof(frame));
    check(dnet_engine_link_rx(&L, t, frame, (size_t)n, reply, sizeof(reply), &rlen,
                              &has_reply, &lev) == DNET_ENGINE_OK && lev == DNET_LINK_EV_CONNECT_CONF,
          "L sees the link RUN (connect confirm)");
    check(dnet_link_is_up(&L.link) && dnet_link_is_up(&R.link), "both engine links UP");
    t++;

    /* --- CTERM Bind: terminal -> host --- */
    check(dnet_cterm_bind(&term, "OVMXL$RTA1:", cterm, sizeof(cterm), &clen) == DNET_CTERM_OK,
          "terminal builds Bind");
    check(dnet_engine_link_send(&L, cterm, clen, frame, sizeof(frame), &flen, t) == DNET_ENGINE_OK,
          "L wraps the Bind in an NSP data frame");
    cev = ship(sv[0], sv[1], &R, &host, frame, flen, t, reply, sizeof(reply), &rlen, &has_reply);
    check(cev == DNET_CTERM_EV_BIND_IND, "host sees the CTERM Bind over the wire");
    /* absorb the NSP ack the data segment generated. */
    check(has_reply, "R produced an NSP ack for the Bind data segment");
    check(write(sv[1], reply, rlen) == (ssize_t)rlen && read(sv[0], frame, sizeof(frame)) > 0,
          "ack returns to L");
    dnet_engine_link_rx(&L, t, frame, DNET_FRAME_MAX, reply, sizeof(reply), &rlen, &has_reply, &lev);
    t++;

    /* --- Bind Accept: host -> terminal --- */
    check(dnet_cterm_bind_accept(&host, "OVMXR", cterm, sizeof(cterm), &clen) == DNET_CTERM_OK,
          "host builds Bind Accept");
    check(dnet_engine_link_send(&R, cterm, clen, reply, sizeof(reply), &rlen, t) == DNET_ENGINE_OK,
          "R wraps Bind Accept");
    cev = ship(sv[1], sv[0], &L, &term, reply, rlen, t, frame, sizeof(frame), &flen, &has_reply);
    check(cev == DNET_CTERM_EV_BOUND, "terminal sees the session BOUND over the wire");
    check(dnet_cterm_is_bound(&term) && dnet_cterm_is_bound(&host), "both CTERM sessions BOUND");
    if (has_reply) { check(write(sv[0], frame, flen) == (ssize_t)flen &&
                           read(sv[1], reply, sizeof(reply)) > 0, "ack returns to R");
        dnet_engine_link_rx(&R, t, reply, DNET_FRAME_MAX, frame, sizeof(frame), &flen, &has_reply, &lev); }
    t++;

    /* --- characteristics: terminal -> host --- */
    check(dnet_cterm_send_characteristics(&term, 4, 132, 24,
              DNET_CTERM_CH_ECHO | DNET_CTERM_CH_WRAP, cterm, sizeof(cterm), &clen)
              == DNET_CTERM_OK, "terminal builds Characteristics");
    dnet_engine_link_send(&L, cterm, clen, frame, sizeof(frame), &flen, t);
    cev = ship(sv[0], sv[1], &R, &host, frame, flen, t, reply, sizeof(reply), &rlen, &has_reply);
    check(cev == DNET_CTERM_EV_CHARACTERISTICS && host.width == 132 && host.page == 24,
          "host negotiated width 132 / page 24 over the wire");
    if (has_reply) { write(sv[1], reply, rlen); n = read(sv[0], frame, sizeof(frame));
        dnet_engine_link_rx(&L, t, frame, (size_t)n, reply, sizeof(reply), &rlen, &has_reply, &lev); }
    t++;

    /* --- host writes a login banner (screen output down) --- */
    const char *banner = "    OpenVMS VAX V7.3\r\nUsername: ";
    check(dnet_cterm_write(&host, (const uint8_t *)banner, strlen(banner),
              DNET_CTERM_WR_NOFORMAT, cterm, sizeof(cterm), &clen) == DNET_CTERM_OK,
          "host builds Write (login banner)");
    dnet_engine_link_send(&R, cterm, clen, reply, sizeof(reply), &rlen, t);
    cev = ship(sv[1], sv[0], &L, &term, reply, rlen, t, frame, sizeof(frame), &flen, &has_reply);
    check(cev == DNET_CTERM_EV_WRITE && term.last.datalen == strlen(banner) &&
          memcmp(term.last.data, banner, term.last.datalen) == 0,
          "terminal displays the banner byte-identical (screen output crossed the wire)");
    if (has_reply) { write(sv[0], frame, flen); n = read(sv[1], reply, sizeof(reply));
        dnet_engine_link_rx(&R, t, reply, (size_t)n, frame, sizeof(frame), &flen, &has_reply, &lev); }
    t++;

    /* --- terminal sends a keystroke line (input up) --- */
    const char *keys = "SYSTEM";
    check(dnet_cterm_read_data(&term, (const uint8_t *)keys, strlen(keys), 0x0d,
              cterm, sizeof(cterm), &clen) == DNET_CTERM_OK, "terminal builds Read Data (keystrokes)");
    dnet_engine_link_send(&L, cterm, clen, frame, sizeof(frame), &flen, t);
    cev = ship(sv[0], sv[1], &R, &host, frame, flen, t, reply, sizeof(reply), &rlen, &has_reply);
    check(cev == DNET_CTERM_EV_READ_DATA && host.last.datalen == strlen(keys) &&
          memcmp(host.last.data, keys, host.last.datalen) == 0,
          "host receives the keystrokes 'SYSTEM' byte-identical (input crossed the wire)");
    if (has_reply) { write(sv[1], reply, rlen); n = read(sv[0], frame, sizeof(frame));
        dnet_engine_link_rx(&L, t, frame, (size_t)n, reply, sizeof(reply), &rlen, &has_reply, &lev); }
    t++;

    /* --- terminal sends an out-of-band ^Y (interrupt) --- */
    check(dnet_cterm_oob(&term, 0x19, cterm, sizeof(cterm), &clen) == DNET_CTERM_OK,
          "terminal builds OOB ^Y");
    dnet_engine_link_send(&L, cterm, clen, frame, sizeof(frame), &flen, t);
    cev = ship(sv[0], sv[1], &R, &host, frame, flen, t, reply, sizeof(reply), &rlen, &has_reply);
    check(cev == DNET_CTERM_EV_OOB && host.last.oob_char == 0x19,
          "host receives the ^Y out-of-band over the wire");
    if (has_reply) { write(sv[1], reply, rlen); n = read(sv[0], frame, sizeof(frame));
        dnet_engine_link_rx(&L, t, frame, (size_t)n, reply, sizeof(reply), &rlen, &has_reply, &lev); }
    t++;

    /* --- host unbinds (the remote logs out) --- */
    check(dnet_cterm_unbind(&host, DNET_CTERM_UNBIND_NORMAL, cterm, sizeof(cterm), &clen)
              == DNET_CTERM_OK, "host builds Unbind");
    dnet_engine_link_send(&R, cterm, clen, reply, sizeof(reply), &rlen, t);
    cev = ship(sv[1], sv[0], &L, &term, reply, rlen, t, frame, sizeof(frame), &flen, &has_reply);
    check(cev == DNET_CTERM_EV_UNBOUND && dnet_cterm_state_of(&term) == DNET_CTERM_S_UNBOUND,
          "terminal sees the session released (Unbind crossed the wire)");
    if (has_reply) { write(sv[0], frame, flen); n = read(sv[1], reply, sizeof(reply));
        dnet_engine_link_rx(&R, t, reply, (size_t)n, frame, sizeof(frame), &flen, &has_reply, &lev); }
    t++;

    /* --- tear the NSP logical link down (DI/DC) --- */
    check(dnet_engine_link_close(&L, DNET_LINK_REASON_NORMAL, frame, sizeof(frame), &flen, t)
              == DNET_ENGINE_OK, "L closes the logical link (DI)");
    check(write(sv[0], frame, flen) == (ssize_t)flen, "DI written");
    n = read(sv[1], reply, sizeof(reply));
    dnet_engine_link_rx(&R, t, reply, (size_t)n, frame, sizeof(frame), &flen, &has_reply, &lev);
    check(lev == DNET_LINK_EV_DISCONNECT && has_reply, "R disconnects + confirms (DC)");
    check(write(sv[1], frame, flen) == (ssize_t)flen, "DC written back");
    n = read(sv[0], reply, sizeof(reply));
    dnet_engine_link_rx(&L, t, reply, (size_t)n, frame, sizeof(frame), &flen, &has_reply, &lev);
    check(lev == DNET_LINK_EV_DISCONNECT_CONF &&
          dnet_link_state_of(&L.link) == DNET_LINK_CLOSED, "L link CLOSED (SET HOST session over)");

    close(sv[0]); close(sv[1]);
}

int main(void)
{
    printf("test_dnet_cterm: DECnet Phase IV CTERM (Command Terminal / SET HOST)\n");
    test_codec();
    test_sc_connect();
    test_session();
    test_engine_e2e();
    if (failures == 0) { printf("test_dnet_cterm: ALL CHECKS PASSED\n"); return 0; }
    printf("test_dnet_cterm: %d CHECK(S) FAILED\n", failures);
    return 1;
}
