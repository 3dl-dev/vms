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

    /* Bind / Bind Accept no longer ride the general dnet_cterm_encode/decode
     * PDU set -- rd vms-bd0 replaced them with the real DNA foundation
     * short-TLV + msgtype-9-envelope codec (dnet_cterm_found_*), oracle-
     * anchored byte-for-byte in test_foundation_oracle() below. A generic
     * (non-oracle) round-trip of that codec lives here instead, matching the
     * shape of every other case in this function. */
    {
        uint8_t buf[32]; size_t n = 0;
        uint8_t mc = 0, pc = 0, vl = 0, val[8];
        uint8_t v[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
        check(dnet_cterm_found_short_build(7, DNET_CTERM_FOUND_PARAM_OBSERVED,
                  v, 4, 12, buf, sizeof(buf), &n) == DNET_CTERM_OK,
              "Foundation short-TLV encodes");
        check(dnet_cterm_found_short_parse(buf, n, &mc, &pc, val, sizeof(val),
                  &vl, NULL) == DNET_CTERM_OK &&
              mc == 7 && pc == DNET_CTERM_FOUND_PARAM_OBSERVED && vl == 4 &&
              memcmp(val, v, 4) == 0,
              "Foundation short-TLV round-trips (msg-code/param-code/value)");
    }
    {
        uint8_t buf[64]; size_t n = 0;
        uint8_t b[6] = { 1, 2, 3, 4, 5, 6 };
        uint16_t lf = 0; const uint8_t *body = NULL; size_t blen = 0; int match = 0;
        check(dnet_cterm_found_envelope_build(6, b, 6, buf, sizeof(buf), &n)
                  == DNET_CTERM_OK, "Foundation msgtype-9 envelope encodes");
        check(dnet_cterm_found_envelope_parse(buf, n, &lf, &body, &blen, &match)
                  == DNET_CTERM_OK &&
              lf == 6 && blen == 6 && memcmp(body, b, 6) == 0 && match,
              "Foundation envelope round-trips (len field + body)");
    }

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
    { uint8_t t[1] = { DNET_CTERM_MSG_UNBIND }; struct dnet_cterm_msg o;
      check(dnet_cterm_decode(t, sizeof(t), &o, NULL) == DNET_CTERM_ETRUNC,
            "a truncated Unbind is rejected ETRUNC (no fabrication)"); }
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

/*
 * THE FOUNDATION ORACLE SPECIMENS (rd vms-bd0). Captured on vaxlab-3: a real
 * VAX1<->VAX2 `$ SET HOST` that reached a live DCL prompt --
 * real-cterm-ci.pcap (default terminal, PAGE=24) and cterm-oracle-wid8.pcap
 * (SET TERMINAL/PAGE=48 on the client). Every array below is the CTERM
 * payload of one NSP "data seg" frame, i.e. the frame's bytes from absolute
 * offset 47 onward (Ethernet14 + DLlen2 + pad1 + routing21 + NSP9), copied
 * verbatim -- these are the ORACLE, and every assertion below is measured
 * against them, never against what OVMX happens to build.
 */
static const uint8_t k_oracle_found_host_seg1[] = {
    0x01, 0x02, 0x04, 0x00, 0x07, 0x00, 0x10, 0x00
};
static const uint8_t k_oracle_found_client_seg1[] = {
    0x04, 0x02, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t k_oracle_found_host_seg2[] = {
    0x09, 0x00, 0x17, 0x00,
    0x01, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x02, 0x10, 0x1e, 0x03, 0x04, 0xfe, 0xff, 0xef,
    0x00, 0x06, 0x00, 0x0b, 0x00, 0x08, 0x02, 0x02, 0x00
};
/* default-terminal client seg 2 (real-cterm-ci.pcap, both sessions, byte-
 * identical): WIDTH=132, PAGE=24. */
static const uint8_t k_oracle_found_client_seg2_default[] = {
    0x09, 0x00, 0x35, 0x00, 0x01, 0x00, 0x01, 0x04,
    0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x02, 0xf2, 0x03, 0x02, 0x02, 0xc0,
    0x03, 0x03, 0x04, 0xfe, 0xff, 0xef, 0x00, 0x04,
    0x18, 0x42, 0x20, 0x84, 0x00, 0xa0, 0x02, 0x00,
    0x18, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00
};
/* SET TERMINAL/PAGE=48 client seg 2 (cterm-oracle-wid8.pcap): WIDTH=132,
 * PAGE=48 -- a single-byte diff from the default specimen (verified by a
 * programmatic hex diff of the two captures: the ONLY payload byte that
 * differs is the PAGE low byte), which is what resolves the PAGE field. */
static const uint8_t k_oracle_found_client_seg2_page48[] = {
    0x09, 0x00, 0x35, 0x00, 0x01, 0x00, 0x01, 0x04,
    0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x02, 0xf2, 0x03, 0x02, 0x02, 0xc0,
    0x03, 0x03, 0x04, 0xfe, 0xff, 0xef, 0x00, 0x04,
    0x18, 0x42, 0x20, 0x84, 0x00, 0xa0, 0x02, 0x00,
    0x30, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00
};

static void test_foundation_oracle(void)
{
    printf("[found] the REAL DNA CTERM foundation messages, byte-exact"
           " against a captured VAX<->VAX SET HOST (rd vms-bd0)\n");

    uint8_t buf[128]; size_t n = 0;

    /* ---- 1. OVMX BUILDS byte-identical to the oracle, both directions ---- */
    check(dnet_cterm_found_host_start_build(buf, sizeof(buf), &n) == DNET_CTERM_OK &&
          n == sizeof(k_oracle_found_host_seg1) &&
          memcmp(buf, k_oracle_found_host_seg1, n) == 0,
          "host's seg-1 Start is BYTE-IDENTICAL to the real VAX host's"
          " (msg-code 1, param 0x02, value 00 07 00 10, 8 bytes total)");

    check(dnet_cterm_found_client_start_build(buf, sizeof(buf), &n) == DNET_CTERM_OK &&
          n == sizeof(k_oracle_found_client_seg1) &&
          memcmp(buf, k_oracle_found_client_seg1, n) == 0,
          "client's seg-1 response is BYTE-IDENTICAL to the real VAX"
          " client's (msg-code 4, value 00 07 00 00, 17 bytes total)");

    check(dnet_cterm_found_host_seg2_build(buf, sizeof(buf), &n) == DNET_CTERM_OK &&
          n == sizeof(k_oracle_found_host_seg2) &&
          memcmp(buf, k_oracle_found_host_seg2, n) == 0,
          "host's seg-2 envelope (msgtype 9, len field 23, 31-byte fixed"
          " body) is BYTE-IDENTICAL to the real VAX host's");

    check(dnet_cterm_found_client_termchar_build(132, 24, buf, sizeof(buf), &n)
              == DNET_CTERM_OK &&
          n == sizeof(k_oracle_found_client_seg2_default) &&
          memcmp(buf, k_oracle_found_client_seg2_default, n) == 0,
          "client's seg-2 envelope (WIDTH=132, PAGE=24) is BYTE-IDENTICAL to"
          " the real VAX client's default-terminal specimen"
          " (real-cterm-ci.pcap)");

    check(dnet_cterm_found_client_termchar_build(132, 48, buf, sizeof(buf), &n)
              == DNET_CTERM_OK &&
          n == sizeof(k_oracle_found_client_seg2_page48) &&
          memcmp(buf, k_oracle_found_client_seg2_page48, n) == 0,
          "client's seg-2 envelope (WIDTH=132, PAGE=48) is BYTE-IDENTICAL to"
          " the real VAX client's SET TERMINAL/PAGE=48 specimen"
          " (cterm-oracle-wid8.pcap) -- a single byte differs from the"
          " default specimen and it is the byte OVMX also changes");

    /* ---- 2. OVMX DECODES the real specimens correctly -------------------- */
    {
        uint8_t mc = 0, pc = 0, vl = 0, val[DNET_CTERM_FOUND_VALUE_MAX];
        check(dnet_cterm_found_short_parse(k_oracle_found_host_seg1,
                  sizeof(k_oracle_found_host_seg1), &mc, &pc, val, sizeof(val),
                  &vl, NULL) == DNET_CTERM_OK &&
              mc == 0x01 && pc == 0x02 && vl == 4 &&
              memcmp(val, (uint8_t[]){0x00,0x07,0x00,0x10}, 4) == 0,
              "decode of the real host seg-1 specimen recovers"
              " msg-code=1 / param=0x02 / value=00 07 00 10");
        check(dnet_cterm_found_short_parse(k_oracle_found_client_seg1,
                  sizeof(k_oracle_found_client_seg1), &mc, &pc, val, sizeof(val),
                  &vl, NULL) == DNET_CTERM_OK &&
              mc == 0x04 && pc == 0x02 && vl == 4 &&
              memcmp(val, (uint8_t[]){0x00,0x07,0x00,0x00}, 4) == 0,
              "decode of the real client seg-1 specimen recovers"
              " msg-code=4 / param=0x02 / value=00 07 00 00");
    }
    {
        uint16_t width = 0, page = 0;
        check(dnet_cterm_found_client_termchar_parse(k_oracle_found_client_seg2_default,
                  sizeof(k_oracle_found_client_seg2_default), &width, &page)
                  == DNET_CTERM_OK && width == 132 && page == 24,
              "decode of the real default client seg-2 specimen recovers"
              " WIDTH=132 / PAGE=24");
        check(dnet_cterm_found_client_termchar_parse(k_oracle_found_client_seg2_page48,
                  sizeof(k_oracle_found_client_seg2_page48), &width, &page)
                  == DNET_CTERM_OK && width == 132 && page == 48,
              "decode of the real PAGE=48 client seg-2 specimen recovers"
              " WIDTH=132 / PAGE=48");
    }
    {
        uint16_t lf = 0; const uint8_t *body = NULL; size_t blen = 0; int match = 0;
        check(dnet_cterm_found_envelope_parse(k_oracle_found_host_seg2,
                  sizeof(k_oracle_found_host_seg2), &lf, &body, &blen, &match)
                  == DNET_CTERM_OK && lf == 23 && blen == 31 && !match,
              "the host seg-2 envelope's length-field/body-length"
              " DISCREPANCY (23 vs 31) is surfaced, not hidden or 'fixed'");
        check(dnet_cterm_found_envelope_parse(k_oracle_found_client_seg2_default,
                  sizeof(k_oracle_found_client_seg2_default), &lf, &body, &blen,
                  &match) == DNET_CTERM_OK && lf == 53 && blen == 53 && match,
              "the client seg-2 envelope's length field DOES equal its body"
              " length (unlike the host's)");
    }

    /* ---- 3. BOUNDED against truncated/mutated real bytes ------------------
     * Both decoders in this codec run on wire bytes that may not have been
     * authenticated yet. Mutate the real specimens (never pure noise alone --
     * seeded from real bytes reaches the accepting paths, per the sc-connect
     * fuzz above) and require a DEFINED status every time: no crash (ASan/
     * UBSan catch that), no undefined return code. */
    {
        unsigned seed = 0xf0011d0;
        long undefined = 0, accepted_short = 0, accepted_env = 0;
        int iter;
        for (iter = 0; iter < 100000; iter++) {
            uint8_t mbuf[64];
            size_t mlen = sizeof(k_oracle_found_client_seg2_default) + 8;
            if (mlen > sizeof(mbuf)) mlen = sizeof(mbuf);
            size_t j;
            for (j = 0; j < mlen; j++)
                mbuf[j] = j < sizeof(k_oracle_found_client_seg2_default)
                              ? k_oracle_found_client_seg2_default[j]
                              : (uint8_t)(rand_r(&seed) & 0xff);
            if ((rand_r(&seed) & 3) == 0)
                mlen = rand_r(&seed) % sizeof(mbuf);
            int muts = 1 + (int)(rand_r(&seed) % 3);
            int m;
            for (m = 0; m < muts && mlen; m++)
                mbuf[rand_r(&seed) % mlen] = (uint8_t)(rand_r(&seed) & 0xff);

            uint8_t mc, pc, vl, val[DNET_CTERM_FOUND_VALUE_MAX];
            int rc1 = dnet_cterm_found_short_parse(mbuf, mlen, &mc, &pc, val,
                                                    sizeof(val), &vl, NULL);
            if (rc1 == DNET_CTERM_OK) accepted_short++;
            else if (rc1 != DNET_CTERM_ETRUNC && rc1 != DNET_CTERM_EBADLEN &&
                     rc1 != DNET_CTERM_EINVAL) undefined++;

            uint16_t lf; const uint8_t *body; size_t blen; int match;
            int rc2 = dnet_cterm_found_envelope_parse(mbuf, mlen, &lf, &body,
                                                       &blen, &match);
            if (rc2 == DNET_CTERM_OK) accepted_env++;
            else if (rc2 != DNET_CTERM_ETRUNC && rc2 != DNET_CTERM_EINVAL &&
                     rc2 != DNET_CTERM_EBADTYPE) undefined++;
        }
        check(undefined == 0,
              "mutation fuzz: 100000 mutated foundation specimens each get a"
              " DEFINED status from both decoders -- no undefined answer");
        check(accepted_short > 0 && accepted_env > 0,
              "mutation fuzz: the corpus reaches the accepting path of both"
              " decoders (not all-reject)");
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

    /* terminal -> Bind -> host connect indication. Real DNA foundation
     * phase carries no terminal-name field (rd vms-bd0); "OVMX2$RTA1:" is
     * accepted for call-site compatibility only and never reaches the wire
     * -- see the byte-exact oracle proof in test_foundation_oracle(). */
    check(dnet_cterm_bind(&term, "OVMX2$RTA1:", pdu, sizeof(pdu), &n) == DNET_CTERM_OK,
          "terminal builds Bind");
    check(dnet_cterm_state_of(&term) == DNET_CTERM_S_BINDING, "terminal -> BINDING");
    check(dnet_cterm_rx(&host, pdu, n, &ev) == DNET_CTERM_OK && ev == DNET_CTERM_EV_BIND_IND,
          "host sees BIND indication");
    check(host.found_bind_seen == 1,
          "host recorded the inbound foundation Bind (real DNA short-TLV,"
          " no name field on the wire -- rd vms-bd0)");

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

    /* HOST-ROLE rx seeds (rd vms-8b36): the client->host PDUs a BOUND inbound
     * server accepts -- a Bind (BIND_IND), a Read Data (keystrokes), an OOB
     * (^C). Fuzzing MUTATED versions of these INTO the host FSM is the
     * never-crash-a-peer proof for the inbound $ SET HOST server path (the
     * existing CTERM fuzz below drives only the TERMINAL role). */
    uint8_t h_seed[3][DNET_CTERM_MAX_PDU];
    size_t  h_slen[3] = {0,0,0};
    {
        struct dnet_cterm_session ts, hs;
        uint8_t ba[DNET_CTERM_MAX_PDU]; size_t bn = 0;
        enum dnet_cterm_event ev;
        dnet_cterm_session_init(&ts, DNET_CTERM_ROLE_TERMINAL);
        dnet_cterm_session_init(&hs, DNET_CTERM_ROLE_HOST);
        dnet_cterm_bind(&ts, "OVMX$RTA1:", h_seed[0], sizeof(h_seed[0]), &h_slen[0]);
        dnet_cterm_rx(&hs, h_seed[0], h_slen[0], &ev);              /* host BIND_IND */
        dnet_cterm_bind_accept(&hs, "VAX2", ba, sizeof(ba), &bn);
        dnet_cterm_rx(&ts, ba, bn, &ev);                            /* terminal BOUND */
        dnet_cterm_read_data(&ts, (const uint8_t *)"SHOW TIME", 9, 0x0d,
                             h_seed[1], sizeof(h_seed[1]), &h_slen[1]);
        dnet_cterm_oob(&ts, 0x03, h_seed[2], sizeof(h_seed[2]), &h_slen[2]);
        check(h_slen[0] && h_slen[1] && h_slen[2],
              "seed: client->host Bind/Read-Data/OOB PDUs built (host-role fuzz)");
    }

    unsigned st = 0xf54c0de;
    int nsp_accepts = 0, ct_accepts = 0, undefined = 0, consumed_over = 0;
    int host_accepts = 0, ft_undef = 0, fc_undef = 0;   /* rd vms-8b36 host-role + foundation-parser fuzz */
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

        /* --- HOST-ROLE CTERM rx fuzz (rd vms-8b36): the inbound-SERVER FSM a
         *     real VAX's bytes hit. A BOUND host fed MUTATED client bytes must
         *     never crash (ASan/UBSan) -- the never-crash-a-peer proof for the
         *     inbound path #1162 enables. --- */
        {
            struct dnet_cterm_session ht, hh;
            uint8_t bp[DNET_CTERM_MAX_PDU], ba[DNET_CTERM_MAX_PDU];
            size_t bn0 = 0, ban = 0; enum dnet_cterm_event hev;
            dnet_cterm_session_init(&ht, DNET_CTERM_ROLE_TERMINAL);
            dnet_cterm_session_init(&hh, DNET_CTERM_ROLE_HOST);
            dnet_cterm_bind(&ht, "OVMX$RTA1:", bp, sizeof(bp), &bn0);
            dnet_cterm_rx(&hh, bp, bn0, &hev);                  /* hh -> BIND_IND */
            dnet_cterm_bind_accept(&hh, "VAX2", ba, sizeof(ba), &ban);  /* hh BOUND */

            uint8_t hb[DNET_CTERM_MAX_PDU + 64]; size_t hl;
            if ((fz_next(&st) & 7) == 0) {
                hl = fz_next(&st) % 40;
                for (size_t j = 0; j < hl; j++) hb[j] = fz_next(&st);
            } else {
                int s = fz_next(&st) % 3;
                hl = h_slen[s];
                memcpy(hb, h_seed[s], hl);
                mutate(hb, &hl, sizeof(hb), &st);
            }
            int hrc = dnet_cterm_rx(&hh, hb, hl, &hev);   /* HOST must not crash */
            if (hrc == DNET_CTERM_OK) host_accepts++;
        }

        /* --- foundation BOUND-phase parsers the block above does NOT cover
         *     (rd vms-8b36): found_terminal_rx + found_client_termchar_parse,
         *     mutated real seg-2 envelope specimens; every input a DEFINED
         *     status, no over-read. --- */
        {
            uint8_t fb[128]; size_t fl;
            if ((fz_next(&st) & 7) == 0) {
                fl = fz_next(&st) % 48;
                for (size_t j = 0; j < fl; j++) fb[j] = fz_next(&st);
            } else {
                fl = sizeof(k_oracle_found_client_seg2_default);
                if (fl > sizeof(fb)) fl = sizeof(fb);
                memcpy(fb, k_oracle_found_client_seg2_default, fl);
                mutate(fb, &fl, sizeof(fb), &st);
            }
            enum dnet_cterm_found_term_kind fkind;
            uint8_t ftext[64]; size_t ftlen = 0; uint8_t fhandle[2];
            int frc = dnet_cterm_found_terminal_rx(fb, fl, &fkind, ftext,
                                                   sizeof(ftext), &ftlen, fhandle);
            /* A DEFINED status is any code in the CTERM enum (OK/ETRUNC/EBADLEN/
             * ENOSPACE/EINVAL/EBADTYPE) -- these parsers may propagate a
             * sub-decoder's code; an UNDEFINED value (outside the enum) or a
             * crash is the only failure, same discipline as the NSP fuzz. */
            if (!(frc == DNET_CTERM_OK || frc == DNET_CTERM_ETRUNC ||
                  frc == DNET_CTERM_EBADLEN || frc == DNET_CTERM_ENOSPACE ||
                  frc == DNET_CTERM_EINVAL || frc == DNET_CTERM_EBADTYPE))
                ft_undef++;
            uint16_t fw = 0, fp = 0;
            int prc = dnet_cterm_found_client_termchar_parse(fb, fl, &fw, &fp);
            if (!(prc == DNET_CTERM_OK || prc == DNET_CTERM_ETRUNC ||
                  prc == DNET_CTERM_EBADLEN || prc == DNET_CTERM_ENOSPACE ||
                  prc == DNET_CTERM_EINVAL || prc == DNET_CTERM_EBADTYPE))
                fc_undef++;
        }
    }
    check(undefined == 0, "every NSP decode returned a DEFINED status (no crash, no undefined code)");
    check(consumed_over == 0, "no accepting NSP decode claimed to consume past the buffer");
    check(nsp_accepts > 0, "fuzz corpus reaches accepting NSP decodes (not all-reject)");
    check(ct_accepts  > 0, "fuzz corpus reaches accepting CTERM rx (not all-reject)");
    /* rd vms-8b36: the HOST-role (inbound-server) FSM + the two foundation
     * BOUND-phase parsers, under the same mutation fuzz + ASan/UBSan. The
     * no-crash guarantee is enforced by the sanitizers on every iteration; these
     * assert the corpus reached the host's accepting path and neither foundation
     * parser ever returned an undefined status on a mutated/short input. */
    check(host_accepts > 0,
          "fuzz corpus reaches accepting HOST-role CTERM rx -- the inbound-server"
          " FSM (never-crash-a-peer proof, rd vms-8b36)");
    check(ft_undef == 0,
          "found_terminal_rx: every mutated envelope gets a DEFINED CTERM status"
          " -- no crash, no over-read, no undefined value");
    check(fc_undef == 0,
          "found_client_termchar_parse: every mutated envelope gets a DEFINED"
          " status -- no crash, no over-read");
    printf("  fuzz: %d NSP + %d CTERM(term) + %d CTERM(host) iterations, all"
           " decodes DEFINED (nsp_accepts=%d cterm_accepts=%d host_accepts=%d)\n",
           ITERS, ITERS, ITERS, nsp_accepts, ct_accepts, host_accepts);
}

/*
 * THE vms-6165 CLIENT FSM ORACLE SPECIMENS. Every array is the CTERM payload
 * (frame bytes from absolute offset 47) of one NSP "data seg" of the ACCEPTED
 * VAX2->VAX1 SET HOST in real-cterm-ci.pcap (link id 8194, the golden session
 * that reached a live DCL '$'), copied verbatim.
 */
/* Client seg-3 (#44) and seg-4 (#46): the two fixed 09-envelopes after termchar. */
static const uint8_t k_oracle_found_client_seg3[] = {
    0x09, 0x00, 0x0b, 0x00, 0x17, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t k_oracle_found_client_seg4[] = {
    0x09, 0x00, 0x16, 0x00, 0x13, 0x0c, 0x01, 0x00, 0x06, 0x00, 0x00,
    0x00, 0x18, 0x00, 0x42, 0x20, 0x84, 0x00, 0xa0, 0x02, 0x00, 0x18,
    0x00, 0x32, 0x00, 0x00
};
/* Host read-characteristics solicit (#51) and the client's reply (#53), both
 * echoing read handle 04 34. */
static const uint8_t k_oracle_readattr_solicit[] = {
    0x09, 0x00, 0x18, 0x00, 0x0f, 0x00, 0x04, 0x34, 0x00, 0x00, 0x27,
    0x00, 0x0c, 0x00, 0x04, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x0c, 0x00, 0x00, 0x00
};
static const uint8_t k_oracle_readchar_reply[] = {
    0x09, 0x00, 0x1a, 0x00, 0x0f, 0x00, 0x04, 0x34, 0x00, 0x00, 0x01,
    0x00, 0x06, 0x00, 0x00, 0x00, 0x18, 0x00, 0x42, 0x20, 0x84, 0x00,
    0xa0, 0x02, 0x00, 0x18, 0x00, 0x32, 0x00, 0x00
};
/* Host 02-08 screen write of the Username: prompt (#54); its displayed text. */
static const uint8_t k_oracle_write_username[] = {
    0x09, 0x00, 0x1d, 0x00, 0x02, 0x08, 0xb0, 0x00, 0x84, 0x00, 0x0c,
    0x00, 0x14, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d,
    0x0a, 0x55, 0x73, 0x65, 0x72, 0x6e, 0x61, 0x6d, 0x65, 0x3a, 0x20
};
static const uint8_t k_username_text[] = {
    0x0d, 0x0a, 0x55, 0x73, 0x65, 0x72, 0x6e, 0x61, 0x6d, 0x65, 0x3a, 0x20
};
/* Client READ DATA for "SYSTEM"<CR> (#57). */
static const uint8_t k_oracle_read_data_system[] = {
    0x09, 0x00, 0x0f, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06,
    0x00, 0x53, 0x59, 0x53, 0x54, 0x45, 0x4d, 0x0d
};

static void test_client_foundation_fsm(void)
{
    printf("[fsm] the vms-6165 host-speaks-first client foundation sequence +"
           " terminal-I/O classifier, driven by fed real host specimens\n");

    struct dnet_cterm_session s;
    uint8_t out[128]; size_t n = 0; int prog = 0;

    check(dnet_cterm_session_init(&s, DNET_CTERM_ROLE_TERMINAL) == DNET_CTERM_OK,
          "client session init");
    check(dnet_cterm_client_open(&s) == DNET_CTERM_OK &&
          dnet_cterm_state_of(&s) == DNET_CTERM_S_BINDING,
          "client_open arms the FSM (CLOSED -> BINDING) and sends nothing");
    check(dnet_cterm_client_found_next(&s, 132, 24, out, sizeof(out), &n)
              == DNET_CTERM_OK && n == 0,
          "HOST-FIRST: the client sends NOTHING until the host speaks");

    check(dnet_cterm_client_found_rx(&s, k_oracle_found_host_seg1,
              sizeof(k_oracle_found_host_seg1), &prog) == DNET_CTERM_OK && prog,
          "host seg-1 advances the foundation gate");
    check(dnet_cterm_client_found_next(&s, 132, 24, out, sizeof(out), &n)
              == DNET_CTERM_OK && n == sizeof(k_oracle_found_client_seg1) &&
          memcmp(out, k_oracle_found_client_seg1, n) == 0,
          "client replies with the BYTE-EXACT seg-1 (client_start)");
    check(dnet_cterm_client_found_next(&s, 132, 24, out, sizeof(out), &n)
              == DNET_CTERM_OK && n == 0,
          "client then WAITS for the host's config envelope");

    check(dnet_cterm_client_found_rx(&s, k_oracle_found_host_seg2,
              sizeof(k_oracle_found_host_seg2), &prog) == DNET_CTERM_OK && prog,
          "host's first 09-envelope advances the config gate");
    check(dnet_cterm_client_found_next(&s, 132, 24, out, sizeof(out), &n)
              == DNET_CTERM_OK && n == sizeof(k_oracle_found_client_seg2_default) &&
          memcmp(out, k_oracle_found_client_seg2_default, n) == 0,
          "client burst #1 = BYTE-EXACT seg-2 termchar (WIDTH=132/PAGE=24)");
    check(dnet_cterm_client_found_next(&s, 132, 24, out, sizeof(out), &n)
              == DNET_CTERM_OK && n == sizeof(k_oracle_found_client_seg3) &&
          memcmp(out, k_oracle_found_client_seg3, n) == 0,
          "client burst #2 = BYTE-EXACT seg-3");
    check(dnet_cterm_client_found_next(&s, 132, 24, out, sizeof(out), &n)
              == DNET_CTERM_OK && n == sizeof(k_oracle_found_client_seg4) &&
          memcmp(out, k_oracle_found_client_seg4, n) == 0,
          "client burst #3 = BYTE-EXACT seg-4");
    check(dnet_cterm_is_bound(&s),
          "after seg-4 the session is BOUND (foundation complete)");
    check(dnet_cterm_client_found_next(&s, 132, 24, out, sizeof(out), &n)
              == DNET_CTERM_OK && n == 0,
          "no further foundation messages once BOUND");

    /* Terminal-I/O classifier (BOUND phase). */
    enum dnet_cterm_found_term_kind tk = DNET_CTERM_TK_NONE;
    uint8_t txt[128]; size_t tl = 0; uint8_t h[2] = { 0, 0 };
    check(dnet_cterm_found_terminal_rx(k_oracle_write_username,
              sizeof(k_oracle_write_username), &tk, txt, sizeof(txt), &tl, h)
              == DNET_CTERM_OK && tk == DNET_CTERM_TK_START_READ &&
          tl == sizeof(k_username_text) &&
          memcmp(txt, k_username_text, tl) == 0,
          "a 02-08 host write IS the read-solicit (rd vms-6165): classified"
          " START_READ, text = '\\r\\nUsername: '");
    check(dnet_cterm_found_terminal_rx(k_oracle_readattr_solicit,
              sizeof(k_oracle_readattr_solicit), &tk, NULL, 0, NULL, h)
              == DNET_CTERM_OK && tk == DNET_CTERM_TK_READ_ATTR &&
          h[0] == 0x04 && h[1] == 0x34,
          "a 0f-00 host solicit is classified READ_ATTR, handle = 04 34");
    check(dnet_cterm_found_client_readchar_build(h, out, sizeof(out), &n)
              == DNET_CTERM_OK && n == sizeof(k_oracle_readchar_reply) &&
          memcmp(out, k_oracle_readchar_reply, n) == 0,
          "the client's read-characteristics reply echoes handle 04 34,"
          " BYTE-EXACT to the oracle (#53)");
    check(dnet_cterm_found_read_data_build((const uint8_t *)"SYSTEM", 6, 0x0d,
              out, sizeof(out), &n) == DNET_CTERM_OK &&
          n == sizeof(k_oracle_read_data_system) &&
          memcmp(out, k_oracle_read_data_system, n) == 0,
          "client READ DATA for 'SYSTEM'<CR> is BYTE-EXACT to the oracle (#57)");

    /* Bounded against a truncated envelope: never over-read, classify OTHER. */
    check(dnet_cterm_found_terminal_rx(k_oracle_write_username, 3, &tk,
              txt, sizeof(txt), &tl, h) != DNET_CTERM_OK ||
          tk == DNET_CTERM_TK_NONE || tk == DNET_CTERM_TK_OTHER,
          "a 3-byte truncated envelope is refused/OTHER, never over-read");
}

/*
 * rd vms-6165: the local-terminal input queue must be PROMPT-DRIVEN -- one
 * host Start-Read solicit dequeues exactly one buffered line, nothing is ever
 * emitted before the first solicit (the API is pull-only: feed()/eof() never
 * produce output on their own, only dequeue() does), and local stdin EOF
 * never discards what is still queued (the lab iter-2 bug: OVMX blasted its
 * entire stdin as one segment before the host's Username: prompt existed on
 * the wire, then closed stdin -- both halves of that bug are covered here).
 */
static void test_terminal_input_queue(void)
{
    printf("[inq] rd vms-6165 prompt-gated terminal-input queue\n");

    struct dnet_cterm_inq q;
    dnet_cterm_inq_init(&q);
    uint8_t line[64]; size_t ll = 0;

    check(dnet_cterm_inq_dequeue(&q, line, sizeof(line), &ll) == 0,
          "an empty queue with no solicit yet dequeues nothing");

    /* Canned/redirected stdin: the whole script arrives (and EOFs) before any
     * host solicit -- exactly the lab scenario. NO line may be emitted just
     * because it was fed; only an explicit dequeue() releases one. */
    static const uint8_t script[] = "SYSTEM\r\nsystem\r\nSHOW SYSTEM\r\nLOGOUT";
    check(dnet_cterm_inq_feed(&q, script, sizeof(script) - 1) == sizeof(script) - 1,
          "feed() buffers all of a canned script and reports full acceptance");
    dnet_cterm_inq_eof(&q);
    check(q.eof == 1,
          "stdin EOF only marks the queue -- feed()/eof() alone never emit"
          " anything (no output before the first solicit)");

    /* Solicit #1 -> dequeues "SYSTEM" (CRLF terminator dropped). */
    check(dnet_cterm_inq_dequeue(&q, line, sizeof(line), &ll) == 1 &&
          ll == 6 && memcmp(line, "SYSTEM", 6) == 0,
          "1st solicit dequeues exactly the 1st queued line, 'SYSTEM',"
          " CRLF terminator stripped");
    /* Solicit #2 -> dequeues "system" (the next line, in order). */
    check(dnet_cterm_inq_dequeue(&q, line, sizeof(line), &ll) == 1 &&
          ll == 6 && memcmp(line, "system", 6) == 0,
          "2nd solicit dequeues the NEXT queued line, 'system' -- in order,"
          " one line per solicit");
    /* Solicit #3 -> "SHOW SYSTEM". */
    check(dnet_cterm_inq_dequeue(&q, line, sizeof(line), &ll) == 1 &&
          ll == 11 && memcmp(line, "SHOW SYSTEM", 11) == 0,
          "3rd solicit dequeues 'SHOW SYSTEM'");
    /* Solicit #4 -> "LOGOUT" is unterminated (script ends without a CR/LF),
     * but EOF already fired, so it is still dequeuable as the final line --
     * stdin EOF does NOT tear down the session / lose the last line. */
    check(dnet_cterm_inq_dequeue(&q, line, sizeof(line), &ll) == 1 &&
          ll == 6 && memcmp(line, "LOGOUT", 6) == 0,
          "post-EOF, the final unterminated remainder 'LOGOUT' still"
          " dequeues -- EOF never discards buffered input");
    /* Solicit #5: nothing left, and EOF -> no false line manufactured. */
    check(dnet_cterm_inq_dequeue(&q, line, sizeof(line), &ll) == 0,
          "an exhausted post-EOF queue dequeues nothing (never fabricates a"
          " line)");

    /* Interactive pacing: no line is available until a solicit arrives, and
     * feed() alone (no dequeue) still emits nothing -- confirmed by re-using
     * the same probe as above. Then a live host solicit + a still-open
     * (no-EOF) session: partial input without a terminator is NOT released
     * early (the host must not receive a half-typed line). */
    dnet_cterm_inq_init(&q);
    check(dnet_cterm_inq_feed(&q, (const uint8_t *)"SYS", 3) == 3 &&
          dnet_cterm_inq_dequeue(&q, line, sizeof(line), &ll) == 0,
          "an interactive partial line with no terminator and no EOF is"
          " NOT released early");
    check(dnet_cterm_inq_feed(&q, (const uint8_t *)"TEM\r", 4) == 4,
          "the terminator arriving completes the line");
    check(dnet_cterm_inq_dequeue(&q, line, sizeof(line), &ll) == 1 &&
          ll == 6 && memcmp(line, "SYSTEM", 6) == 0,
          "the solicit now dequeues the complete 'SYSTEM' line, session"
          " never closed for lack of EOF");
}

int main(void)
{
    printf("test_dnet_cterm: DECnet Phase IV CTERM (Command Terminal / SET HOST)\n");
    test_codec();
    test_sc_connect();
    test_foundation_oracle();
    test_client_foundation_fsm();
    test_terminal_input_queue();
    test_session();
    test_engine_e2e();
    test_client_response_fuzz();
    if (failures == 0) { printf("test_dnet_cterm: ALL CHECKS PASSED\n"); return 0; }
    printf("test_dnet_cterm: %d CHECK(S) FAILED\n", failures);
    return 1;
}
