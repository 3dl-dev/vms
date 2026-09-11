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
 * Clean-room (Rule 8), AND THE LINE BETWEEN THE TWO HALVES OF THIS FILE:
 *
 *   - The CTERM PDUs (Bind, Characteristics, Read/Write, OOB) are ENTIRELY
 *     SPEC-DERIVED. There is no oracle specimen for them: the vms-3be capture
 *     never completed a logical link, and the vms-558 capture that DID
 *     complete one carries the CTERM payloads inside NSP data segments that
 *     have not been decoded field-by-field. The message set + function mirror
 *     the public DNA CTERM functional description; the numeric codes/layouts
 *     are OVMX-assigned and proven here ONLY by round-trip, never presented as
 *     VMS-authentic bytes. See docs/decnet-provenance-register.md sec 4.7.
 *
 *   - The SESSION CONTROL CONNECT MESSAGE is now ORACLE-GROUNDED (rd vms-558 /
 *     vms-f40). docs/oracle/vax-sethost-cterm.pcap frame 5 carries the real
 *     VAX's twenty bytes, and test_sc_connect() below asserts against THOSE
 *     BYTES -- including the format-0 destination descriptor OVMX's first cut
 *     got wrong, and the empty access-control fields that settle the
 *     credential question. That half is measured, not assigned.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <stdlib.h>         /* rand/srand for the client-response mutation fuzz */

#include "dnet_cterm.h"
#include "dnet_engine.h"
#include "dnet_nsp.h"       /* dnet_nsp_encode/decode: the client's link decode */
#include "ovmx_identity.h"  /* INV-1/INV-0: a self-announcing banner is the OVMX
                             * product identity, never a bare "OpenVMS" literal */

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
    const char *scr = OVMX_PRODUCT_BANNER "   Node OVMXR\r\n";
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

/*
 * THE ORACLE SPECIMEN (rd vms-558 / vms-f40). The twenty bytes of Session
 * Control connect data a real OpenVMS VAX V7.3 put in the Connect Initiate of
 * `$ SET HOST VAX2` -- docs/oracle/vax-sethost-cterm.pcap frame 5, offsets
 * 0x2f..0x42 of the captured Ethernet frame (the retransmission, frame 46, is
 * byte-identical). This array is the ORACLE, copied from the capture, and every
 * assertion about the layout below is measured against it rather than against
 * what OVMX happens to emit.
 */
static const uint8_t k_oracle_sc_connect[] = {
    0x00, 0x2a,                                     /* DSTNAME: fmt 0, object 42 */
    0x02, 0x00, 0x1a, 0x02, 0x20, 0x20,             /* SRCNAME: fmt 2, objtype 0,
                                                     * grpcode 0x021a, usrcode 0x2020 */
    0x06, 'S', 'Y', 'S', 'T', 'E', 'M',             /* ... counted "SYSTEM"      */
    0x27,                                           /* MENUVER                   */
    0x00, 0x00, 0x00, 0x00                          /* RQSTRID/PASSWRD/ACCOUNT
                                                     * (+USRDATA) ALL EMPTY      */
};

static void test_sc_connect(void)
{
    printf("[sc] the SET HOST connect: oracle layout + a bounded decoder\n");
    uint8_t buf[128]; size_t n = 0;
    struct dnet_cterm_sc_connect sc;

    /* ---- 1. THE ORACLE SPECIMEN DECODES, AND SAYS WHAT THE ORACLE SAYS ---- */
    check(dnet_cterm_sc_connect_parse(k_oracle_sc_connect,
                                      sizeof(k_oracle_sc_connect), &sc)
              == DNET_CTERM_OK,
          "the REAL VAX's connect data (oracle pcap frame 5) decodes");
    check(sc.dst_format == DNET_SC_FMT_OBJECT && sc.dst_object == DNET_CTERM_OBJECT,
          "the destination is a FORMAT-0 descriptor naming object 42 (CTERM) --"
          " format 0, not the format 1 OVMX's first cut emitted");
    check(sc.src_format == DNET_SC_FMT_CODED && sc.src_object == 0 &&
          sc.src_grpcode == 0x021a && sc.src_usrcode == 0x2020 &&
          strcmp(sc.src_user, "SYSTEM") == 0,
          "the SOURCE descriptor is format 2 and carries the source user"
          " \"SYSTEM\" plus the specimen's group/user codes");
    check(sc.menuver == 0x27, "the MENUVER byte is the specimen's 0x27");

    /* ---- 2. THE SECURITY FACT: NO CREDENTIAL IS ON THE WIRE --------------- */
    check(sc.rqstrid[0] == '\0' && sc.account[0] == '\0' && sc.password_len == 0,
          "the ACCESS-CONTROL fields (RQSTRID/PASSWRD/ACCOUNT) are ALL EMPTY --"
          " a real SET HOST carries NO password, so an auto-login from the"
          " carried username would authenticate on zero credential material");

    /* The decoder has NOWHERE to put a password even when one is sent: it
     * records the length and drops the bytes (dnet_cterm.h). That is what makes
     * "OVMX cannot auto-login from the wire" structural rather than a comment. */
    check(dnet_cterm_sc_connect_build(DNET_CTERM_OBJECT, "SYSTEM", 0x021a, 0x2020,
                                      "RQ", "SECRETPW", "ACCT",
                                      buf, sizeof(buf), &n) == DNET_CTERM_OK &&
          dnet_cterm_sc_connect_parse(buf, n, &sc) == DNET_CTERM_OK &&
          sc.password_len == 8 && sc.password_present == 1 &&
          memmem(&sc, sizeof(sc), "SECRETPW", 8) == NULL,
          "a connect that DOES carry a password is decoded as a LENGTH only --"
          " the plaintext appears nowhere in the decoded struct");

    /* ---- 3. WHAT OVMX EMITS MATCHES THE ORACLE BYTE FOR BYTE -------------- */
    check(dnet_cterm_sc_connect_build(DNET_CTERM_OBJECT, "SYSTEM", 0x021a, 0x2020,
                                      "", "", "", buf, sizeof(buf), &n)
              == DNET_CTERM_OK,
          "OVMX builds a SET HOST connect for object 42");
    check(n == sizeof(k_oracle_sc_connect) &&
          memcmp(buf, k_oracle_sc_connect, n) == 0,
          "OVMX's connect data is BYTE-IDENTICAL to the real VAX's IN FULL --"
          " MENUVER 0x27 + RQSTRID/PASSWRD/ACCOUNT/USRDATA all empty"
          " (27 00 00 00 00). Emitting only the first THREE fields (omitting the"
          " trailing USRDATA) left the block 1 byte short of every accepted real"
          " form; real OpenVMS session control read past end-of-data and SILENTLY"
          " discarded the Connect Initiate -- no reject, no counter, no LOGINOUT"
          " (vms-a70 direction A root cause)");
    check(dnet_cterm_sc_connect_object(buf, n) == DNET_CTERM_OBJECT,
          "the object-number dispatch reads 42 back out of OVMX's own connect");

    /* ---- 4. BOUNDED AGAINST ATTACKER-CONTROLLED BYTES --------------------- */
    /* These bytes arrive from an UNAUTHENTICATED peer. Every one of these must
     * be a clean refusal, never an over-read: "OVMX never crashes a peer" cuts
     * both ways, and this decoder is the first thing an inbound SET HOST
     * touches. Run under ASan/valgrind in CI, an over-read here is a hard red. */
    { struct dnet_cterm_sc_connect s2;
      check(dnet_cterm_sc_connect_parse(NULL, 4, &s2) == DNET_CTERM_EINVAL &&
            dnet_cterm_sc_connect_parse(k_oracle_sc_connect, 4, NULL) == DNET_CTERM_EINVAL,
            "NULL arguments are refused EINVAL"); }
    { struct dnet_cterm_sc_connect s2;
      /* EVERY prefix of the specimen: each must either decode (a short but
       * well-formed connect) or be refused -- and never read past its end. */
      int over = 0;
      for (size_t len = 0; len < sizeof(k_oracle_sc_connect); len++) {
          uint8_t tmp[sizeof(k_oracle_sc_connect)];
          memcpy(tmp, k_oracle_sc_connect, len);
          int rc = dnet_cterm_sc_connect_parse(tmp, len, &s2);
          if (rc != DNET_CTERM_OK && rc != DNET_CTERM_ETRUNC &&
              rc != DNET_CTERM_EBADLEN && rc != DNET_CTERM_EINVAL)
              over = 1;
      }
      check(!over, "every TRUNCATED prefix of the specimen is answered with a"
                   " defined status (OK/ETRUNC/EBADLEN/EINVAL), never a crash"); }
    { struct dnet_cterm_sc_connect s2;
      /* A counted string whose length byte claims more than the message holds. */
      uint8_t lying[] = { 0x00, 0x2a, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 'A' };
      check(dnet_cterm_sc_connect_parse(lying, sizeof(lying), &s2) == DNET_CTERM_ETRUNC,
            "a counted string claiming 255 bytes in a 10-byte message is"
            " refused ETRUNC (never read past the buffer)"); }
    { struct dnet_cterm_sc_connect s2;
      /* A counted string that fits the message but not the field: REFUSED, not
       * clipped. A clipped identity that happens to resolve is the bug class
       * this rule exists to prevent. */
      uint8_t big[8 + 2 + DNET_SC_MAX_STR + 8];
      size_t o = 0;
      big[o++] = 0x00; big[o++] = 0x2a;
      big[o++] = 0x02; big[o++] = 0x00; big[o++] = 0; big[o++] = 0; big[o++] = 0; big[o++] = 0;
      big[o++] = (uint8_t)(DNET_SC_MAX_STR + 1);
      for (int i = 0; i <= DNET_SC_MAX_STR; i++) big[o++] = 'A';
      check(dnet_cterm_sc_connect_parse(big, o, &s2) == DNET_CTERM_EBADLEN,
            "an over-long source-user string is REFUSED EBADLEN, not clipped"); }
    { struct dnet_cterm_sc_connect s2;
      uint8_t badfmt[] = { 0x07, 0x2a, 0x00, 0x00 };
      check(dnet_cterm_sc_connect_parse(badfmt, sizeof(badfmt), &s2) == DNET_CTERM_EINVAL,
            "an unknown descriptor FORMAT is refused EINVAL"); }
    { struct dnet_cterm_sc_connect s2;
      uint8_t zeroobj[] = { 0x00, 0x00, 0x00, 0x00 };
      check(dnet_cterm_sc_connect_parse(zeroobj, sizeof(zeroobj), &s2) == DNET_CTERM_EINVAL,
            "a format-0 descriptor naming object 0 is refused (it names nothing)"); }
    { /* A connect for a DIFFERENT object must not read as CTERM. */
      uint8_t other[] = { 0x00, 0x11, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
      check(dnet_cterm_sc_connect_object(other, sizeof(other)) == 17,
            "a connect to object 17 (FAL) reads back as 17, not as CTERM"); }

    /* ---- 5. Remote Port Info is the oracle's accounting string ------------ */
    { char rpi[64];
      struct dnet_cterm_sc_connect s2;
      check(dnet_cterm_sc_connect_parse(k_oracle_sc_connect,
                                        sizeof(k_oracle_sc_connect), &s2) == DNET_CTERM_OK &&
            dnet_cterm_remote_port_info(&s2, 1025, rpi, sizeof(rpi)) == DNET_CTERM_OK &&
            strcmp(rpi, "1025::SYSTEM") == 0,
            "Remote Port Info renders as \"1025::SYSTEM\" -- exactly the string"
            " the oracle's SHOW TERMINAL printed for this connect"); }
    { char rpi[64];
      struct dnet_cterm_sc_connect s2;
      memset(&s2, 0, sizeof(s2));
      /* A peer that puts control bytes in its source name must not be able to
       * inject them into a console line or an accounting record. */
      memcpy(s2.src_user, "AB\033[2JC\007D", 9);
      check(dnet_cterm_remote_port_info(&s2, 1, rpi, sizeof(rpi)) == DNET_CTERM_OK &&
            strcmp(rpi, "1::AB[2JCD") == 0,
            "control characters in a peer-supplied source name are STRIPPED from"
            " the accounting surface (no escape-sequence injection)"); }

    /* ---- 6. MUTATION FUZZ, SEEDED FROM THE SPECIMEN ----------------------- */
    /*
     * The hand-written cases above cover the malformed shapes an author thought
     * of. This covers the ones nobody did, and it is seeded deliberately:
     * PURELY RANDOM bytes essentially never form a valid connect (measured
     * while writing this: ONE acceptance in 3,000,000 draws), so a random fuzz
     * never reaches the deep paths at all. Mutating the REAL VAX's twenty bytes
     * does -- roughly 45% of the draws below are accepted, so the accepting
     * paths, not just the rejecting ones, are what gets exercised.
     *
     * Deterministic (fixed seed) so a failure is reproducible, and sized to
     * stay a fraction of a second in CI. The value of running it under CI's
     * sanitizer build is that an over-read here is a RED, not a silent wrong
     * answer: these bytes arrive from a peer that has not authenticated, and
     * "OVMX never crashes a peer" cuts both ways. Locally, 3,000,000 draws of
     * this shape under -fsanitize=address,undefined reported nothing.
     */
    {
        unsigned seed = 987654321u;
        long accepted = 0, anomalies = 0;
        int iter;

        for (iter = 0; iter < 200000; iter++) {
            uint8_t mbuf[sizeof(k_oracle_sc_connect) + 8];
            struct dnet_cterm_sc_connect s2;
            size_t mlen = sizeof(k_oracle_sc_connect);
            int muts, m, rc;

            if ((rand_r(&seed) & 3) == 0)
                mlen = (size_t)(rand_r(&seed) % sizeof(mbuf));
            for (size_t j = 0; j < mlen; j++)
                mbuf[j] = j < sizeof(k_oracle_sc_connect)
                              ? k_oracle_sc_connect[j]
                              : (uint8_t)(rand_r(&seed) & 0xff);
            muts = 1 + (rand_r(&seed) % 3);
            for (m = 0; m < muts && mlen; m++)
                mbuf[rand_r(&seed) % mlen] = (uint8_t)(rand_r(&seed) & 0xff);

            rc = dnet_cterm_sc_connect_parse(mbuf, mlen, &s2);
            if (rc == DNET_CTERM_OK) {
                char rpi[24];   /* deliberately SHORT: exercise ENOSPACE too */
                accepted++;
                (void)dnet_cterm_remote_port_info(&s2, 1025, rpi, sizeof(rpi));
            } else if (rc != DNET_CTERM_ETRUNC && rc != DNET_CTERM_EBADLEN &&
                       rc != DNET_CTERM_EINVAL) {
                anomalies++;
            }
            (void)dnet_cterm_sc_connect_object(mbuf, mlen);
        }
        check(anomalies == 0,
              "mutation fuzz: 200000 mutated connects each get a DEFINED status"
              " (OK/ETRUNC/EBADLEN/EINVAL) -- no undefined answer");
        check(accepted > 50000,
              "mutation fuzz: the corpus REACHES the accepting paths (a fuzz that"
              " only ever gets rejected proves nothing about them)");
    }

    /* ---- 5. THE A2/A8 ISOLATION SEAM (vms-9ab, design vms-515 §3.4) -------
     * dnet_conn_descriptor_from_wire() is the LOW-PRIVILEGE boundary: it turns
     * untrusted connect bytes into a validated typed descriptor, and it is the
     * ONLY thing between a hostile frame and NETACP's privileged control path.
     * Prove its contract holds under the same mutation fuzz: a malformed frame
     * NEVER yields a validated descriptor, a rejected frame ALWAYS leaves the
     * descriptor all-zero (validated == 0 -- the state the privileged path
     * refuses), and an accepted descriptor is fully bounded and credential-free. */
    {
        /* Positive: the real VAX specimen distils to a validated object-42
         * descriptor whose proxy identity is the carried user and nothing more. */
        struct dnet_conn_descriptor d;
        check(dnet_conn_descriptor_from_wire(k_oracle_sc_connect,
                                             sizeof(k_oracle_sc_connect) - 1,
                                             1025, &d) == DNET_CTERM_OK &&
              d.validated == 1 && d.dst_is_object == 1 &&
              d.dst_object == DNET_CTERM_OBJECT &&
              strcmp(d.proxy_user, "SYSTEM") == 0 && d.peer_addr == 1025,
              "from_wire: the oracle connect yields a VALIDATED object-42"
              " descriptor carrying only the proxy user (SYSTEM) + engine addr");

        /* The descriptor TYPE has no field a credential could live in: even a
         * connect that DOES carry a password produces a descriptor with the
         * plaintext nowhere in it (structural, not a measured coincidence). */
        uint8_t withpw[128]; size_t pn = 0;
        check(dnet_cterm_sc_connect_build(DNET_CTERM_OBJECT, "SYSTEM", 0x021a,
                                          0x2020, "RQ", "SECRETPW", "ACCT",
                                          withpw, sizeof(withpw), &pn) == DNET_CTERM_OK &&
              dnet_conn_descriptor_from_wire(withpw, pn, 7, &d) == DNET_CTERM_OK &&
              memmem(&d, sizeof(d), "SECRETPW", 8) == NULL,
              "from_wire: a password-bearing connect produces a descriptor with"
              " the plaintext NOWHERE in it -- the seam cannot carry a credential");

        /* An unvalidated (all-zero) descriptor is the state a REJECTED frame
         * leaves, and it is exactly what the privileged path refuses. */
        struct dnet_conn_descriptor zero;
        memset(&zero, 0, sizeof(zero));
        check(zero.validated == 0,
              "an all-zero descriptor is unvalidated -- the privileged control"
              " path (dnet_cterm_host_open_desc) refuses it before any device"
              " or process exists");

        unsigned seed = 0x5eed9ab, leaks = 0, dirty_reject = 0, unbounded = 0;
        unsigned validated_ct = 0, i;
        for (i = 0; i < 200000; i++) {
            uint8_t mbuf[80];
            size_t mlen, j;
            int muts, m, rc;
            mlen = (size_t)(rand_r(&seed) % sizeof(mbuf));
            for (j = 0; j < mlen; j++)
                mbuf[j] = j < sizeof(k_oracle_sc_connect)
                              ? k_oracle_sc_connect[j]
                              : (uint8_t)(rand_r(&seed) & 0xff);
            muts = 1 + (rand_r(&seed) % 3);
            for (m = 0; m < muts && mlen; m++)
                mbuf[rand_r(&seed) % mlen] = (uint8_t)(rand_r(&seed) & 0xff);

            memset(&d, 0xAB, sizeof(d));   /* poison: a failure must fully clear */
            rc = dnet_conn_descriptor_from_wire(mbuf, mlen, 1025, &d);
            if (rc == DNET_CTERM_OK) {
                validated_ct++;
                /* An ACCEPTED descriptor must be validated, NUL-terminated
                 * within bound, printable-only, and CONSISTENT with the parser's
                 * own object decode -- never a validated object-42 the parser
                 * would not also call object 42. */
                if (!d.validated)
                    leaks++;
                if (d.proxy_user[DNET_SC_MAX_STR] != '\0' ||
                    d.proxy_task[DNET_SC_MAX_STR] != '\0')
                    unbounded++;
                for (const char *p = d.proxy_user; *p; p++)
                    if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e)
                        unbounded++;
                if (d.dst_is_object) {
                    int obj = dnet_cterm_sc_connect_object(mbuf, mlen);
                    if (obj < 0 || (uint8_t)obj != d.dst_object)
                        leaks++;
                }
            } else {
                /* A REJECTED frame must leave the descriptor all-zero: no
                 * poison bytes survive, and above all validated == 0. */
                struct dnet_conn_descriptor z;
                memset(&z, 0, sizeof(z));
                if (d.validated != 0 || memcmp(&d, &z, sizeof(d)) != 0)
                    dirty_reject++;
            }
        }
        check(leaks == 0,
              "from_wire fuzz: NO malformed frame ever produced a validated"
              " descriptor inconsistent with the parser -- the privileged path"
              " cannot be steered to a fabricated object by hostile bytes");
        check(dirty_reject == 0,
              "from_wire fuzz: EVERY rejected frame left the descriptor all-zero"
              " (validated == 0) -- a refused parse hands the privileged path"
              " nothing it will act on");
        check(unbounded == 0,
              "from_wire fuzz: every accepted descriptor's strings stay bounded"
              " and printable -- no over-run, no control-char injection");
        check(validated_ct > 20000,
              "from_wire fuzz: the corpus REACHES the validated path (else the"
              " no-leak result would be vacuous)");
    }
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
    const char *banner = "Welcome to " OVMX_PRODUCT_BANNER "\r\n";
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
    check(dnet_cterm_sc_connect_build(DNET_CTERM_OBJECT, "SYSTEM", 0x021a, 0x2020,
                                      "", "", "", sc, sizeof(sc), &sclen) == DNET_CTERM_OK,
          "SET HOST builds the SC connect for object 42 (oracle shape)");
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
    const char *banner = "    " OVMX_PRODUCT_BANNER "\r\nUsername: ";
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

/* ---- 5. CLIENT response-parse fuzz (rd vms-f54) -------------------------- *
 *
 * The $ SET HOST CLIENT decodes whatever a REMOTE node sends back: NSP transport
 * PDUs (Connect Confirm / data / Disconnect Initiate) via dnet_nsp_decode, and
 * the CTERM PDUs riding those data segments (Bind Accept / Write / Unbind) via
 * dnet_cterm_rx. A hostile or buggy remote must not be able to crash the client
 * with a malformed response -- "never crash a peer, and never be crashed BY
 * one". This mutation-fuzzes both decoders against object-42-shaped seeds and
 * pure noise: every draw must yield a DEFINED status (the decoder RETURNS; an
 * out-of-bounds read would trip ASan on the sanitizer legs), and the corpus must
 * reach the accepting paths (a fuzz that only ever rejects proves nothing). */
static uint8_t fz_next(unsigned *st) { *st = *st * 1103515245u + 12345u; return (uint8_t)(*st >> 16); }

static void mutate(uint8_t *buf, size_t *len, size_t cap, unsigned *st)
{
    int muts = 1 + (fz_next(st) & 3);
    for (int i = 0; i < muts && *len; i++) {
        int op = fz_next(st) % 3;
        if (op == 0) {                        /* flip a byte */
            buf[fz_next(st) % *len] ^= fz_next(st);
        } else if (op == 1 && *len > 1) {     /* truncate */
            *len = 1 + (fz_next(st) % *len);
        } else if (*len < cap) {              /* extend with noise */
            buf[*len] = fz_next(st);
            (*len)++;
        }
    }
}

static void test_client_response_fuzz(void)
{
    printf("[fuzz] the SET HOST client's response decoders survive a hostile remote\n");

    /* NSP seeds the client actually receives: Connect Confirm, a data segment,
     * a Disconnect Initiate -- all for our logical-link addresses. */
    uint8_t nsp_seed[3][DNET_NSP_MAX_DATA + 64];
    size_t  nsp_slen[3] = {0,0,0};
    struct dnet_nsp_msg m;
    memset(&m, 0, sizeof(m));
    m.type = DNET_NSP_T_CC; m.msgflg = DNET_NSP_MSGFLG_CC;
    m.dstaddr = 0x2002; m.srcaddr = 0x2001; m.services = 1; m.info = DNET_NSP_VER_41; m.segsize = 1459;
    check(dnet_nsp_encode(&m, nsp_seed[0], sizeof(nsp_seed[0]), &nsp_slen[0]) == DNET_NSP_OK,
          "seed: Connect Confirm encodes");
    memset(&m, 0, sizeof(m));
    m.type = DNET_NSP_T_DATA; m.msgflg = DNET_NSP_MSGFLG_DATA;
    m.dstaddr = 0x2001; m.srcaddr = 0x2002; m.segnum = DNET_NSP_DATA_BOM | DNET_NSP_DATA_EOM;
    m.datalen = 8; memcpy(m.data, "Username", 8);
    check(dnet_nsp_encode(&m, nsp_seed[1], sizeof(nsp_seed[1]), &nsp_slen[1]) == DNET_NSP_OK,
          "seed: data segment encodes");
    memset(&m, 0, sizeof(m));
    m.type = DNET_NSP_T_DI; m.msgflg = DNET_NSP_MSGFLG_DI;
    m.dstaddr = 0x2001; m.srcaddr = 0x2002; m.reason = 0;
    check(dnet_nsp_encode(&m, nsp_seed[2], sizeof(nsp_seed[2]), &nsp_slen[2]) == DNET_NSP_OK,
          "seed: Disconnect Initiate encodes");

    /* CTERM seeds the client actually receives from the host, captured from a
     * real handshake: Bind Accept, a Write (screen output), an Unbind. */
    uint8_t ct_seed[3][DNET_CTERM_MAX_PDU];
    size_t  ct_slen[3] = {0,0,0};
    {
        struct dnet_cterm_session t0, h0;
        uint8_t p[DNET_CTERM_MAX_PDU]; size_t n = 0; enum dnet_cterm_event ev;
        dnet_cterm_session_init(&t0, DNET_CTERM_ROLE_TERMINAL);
        dnet_cterm_session_init(&h0, DNET_CTERM_ROLE_HOST);
        dnet_cterm_bind(&t0, "OVMX$RTA1:", p, sizeof(p), &n);
        dnet_cterm_rx(&h0, p, n, &ev);
        dnet_cterm_bind_accept(&h0, "VAX2", ct_seed[0], sizeof(ct_seed[0]), &ct_slen[0]);
        dnet_cterm_rx(&t0, ct_seed[0], ct_slen[0], &ev);           /* t0 now BOUND */
        dnet_cterm_write(&h0, (const uint8_t *)"Username: ", 10,
                         DNET_CTERM_WR_NOFORMAT, ct_seed[1], sizeof(ct_seed[1]), &ct_slen[1]);
        dnet_cterm_unbind(&h0, DNET_CTERM_UNBIND_NORMAL, ct_seed[2], sizeof(ct_seed[2]), &ct_slen[2]);
        check(ct_slen[0] && ct_slen[1] && ct_slen[2], "seed: CTERM Bind-Accept/Write/Unbind built");
    }

    unsigned st = 0xf54c0de;
    int nsp_accepts = 0, ct_accepts = 0, undefined = 0, consumed_over = 0;
    const int ITERS = 60000;
    for (int i = 0; i < ITERS; i++) {
        /* --- NSP decode fuzz (dnet_nsp_decode) --- */
        uint8_t nb[DNET_NSP_MAX_DATA + 128];
        size_t nl;
        if ((fz_next(&st) & 7) == 0) {                 /* 1/8: pure noise */
            nl = fz_next(&st) % 48;
            for (size_t j = 0; j < nl; j++) nb[j] = fz_next(&st);
        } else {                                       /* else: mutate a seed */
            int s = fz_next(&st) % 3;
            nl = nsp_slen[s];
            memcpy(nb, nsp_seed[s], nl);
            mutate(nb, &nl, sizeof(nb), &st);
        }
        struct dnet_nsp_msg om;
        size_t cons = 0;
        int rc = dnet_nsp_decode(nb, nl, &om, &cons);
        /* Any of the documented codes is acceptable; the ONLY failures are a
         * crash (caught by ASan), an undefined return, or an accept that claims
         * to have consumed more than the buffer held. */
        if (!(rc == DNET_NSP_OK || rc == DNET_NSP_ETRUNC || rc == DNET_NSP_EBADLEN ||
              rc == DNET_NSP_EINVAL || rc == DNET_NSP_EBADTYPE || rc == DNET_NSP_ENOSPACE))
            undefined++;
        if (rc == DNET_NSP_OK) { nsp_accepts++; if (cons > nl) consumed_over++; }

        /* --- CTERM rx fuzz (dnet_cterm_rx) into a fresh BOUND terminal --- */
        struct dnet_cterm_session t, h;
        uint8_t p[DNET_CTERM_MAX_PDU]; size_t n = 0; enum dnet_cterm_event ev;
        dnet_cterm_session_init(&t, DNET_CTERM_ROLE_TERMINAL);
        dnet_cterm_session_init(&h, DNET_CTERM_ROLE_HOST);
        dnet_cterm_bind(&t, "OVMX$RTA1:", p, sizeof(p), &n);
        dnet_cterm_rx(&h, p, n, &ev);
        dnet_cterm_bind_accept(&h, "VAX2", p, sizeof(p), &n);
        dnet_cterm_rx(&t, p, n, &ev);                  /* t BOUND */

        uint8_t cb[DNET_CTERM_MAX_PDU + 64];
        size_t cl;
        if ((fz_next(&st) & 7) == 0) {
            cl = fz_next(&st) % 40;
            for (size_t j = 0; j < cl; j++) cb[j] = fz_next(&st);
        } else {
            int s = fz_next(&st) % 3;
            cl = ct_slen[s];
            memcpy(cb, ct_seed[s], cl);
            mutate(cb, &cl, sizeof(cb), &st);
        }
        int crc = dnet_cterm_rx(&t, cb, cl, &ev);      /* must not crash */
        if (crc == DNET_CTERM_OK) ct_accepts++;
    }
    check(undefined == 0, "every NSP decode returned a DEFINED status (no crash, no undefined code)");
    check(consumed_over == 0, "no accepting NSP decode claimed to consume past the buffer");
    check(nsp_accepts > 0, "fuzz corpus reaches accepting NSP decodes (not all-reject)");
    check(ct_accepts  > 0, "fuzz corpus reaches accepting CTERM rx (not all-reject)");
    printf("  fuzz: %d NSP + %d CTERM iterations, all decodes returned a defined"
           " status (nsp_accepts=%d cterm_accepts=%d)\n",
           ITERS, ITERS, nsp_accepts, ct_accepts);
}

int main(void)
{
    printf("test_dnet_cterm: DECnet Phase IV CTERM (Command Terminal / SET HOST)\n");
    test_codec();
    test_sc_connect();
    test_session();
    test_engine_e2e();
    test_client_response_fuzz();
    if (failures == 0) { printf("test_dnet_cterm: ALL CHECKS PASSED\n"); return 0; }
    printf("test_dnet_cterm: %d CHECK(S) FAILED\n", failures);
    return 1;
}
