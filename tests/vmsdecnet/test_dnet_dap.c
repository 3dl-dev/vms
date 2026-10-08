/*
 * test_dnet_dap.c - unit + fuzz proof for the DAP codec (dnet_dap) and the FAL
 * access-control credential decoder (dnet_fal_access_decode), rd vms-8c2.
 *
 * Anti-LARP bar (the item's veracity rubric):
 *   1. REAL-VMS ANCHORS (rd vms-a8a): every real OpenVMS DAP buffer from the
 *      oracle + the lab decodes field by field to the values VMS put there,
 *      and the encoder emits exactly the bytes a real VMS FAL accepted.
 *   2. ORACLE ANCHOR: the FAL access decoder pulls username="SYSTEM" +
 *      password (retained) out of the EXACT connect-data bytes the real VAX
 *      COPY put on the wire (docs/oracle/vax-copy-fal-dap.md §1), and the
 *      builder re-emits those bytes BYTE-IDENTICAL.
 *   3. BOUNDED / NEVER CRASH A PEER: the DAP decoder and the FAL access decoder
 *      are fuzzed against truncations, over-long counts and random noise; none
 *      may over-read (built with ASan/UBSan in CI) and each must reject cleanly.
 *
 * Built -Werror; the CMake target adds -fsanitize=address,undefined so an
 * over-read here is a HARD test failure, not a silent pass.
 */
#include "dnet_dap.h"
#include "dnet_cterm.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { \
    if (c) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s\n", msg); } \
} while (0)

/* ---- helpers -------------------------------------------------------------- */

static size_t unhex(const char *h, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (h[0] && h[1] && n < cap) {
        unsigned v; sscanf(h, "%2x", &v); out[n++] = (uint8_t)v; h += 2;
    }
    return n;
}

/* Decode every blocked message of one Session Control buffer; returns count,
 * fills ops[] and the decoded messages into msgs[]. -1 on a decode failure. */
static int split(const uint8_t *b, size_t n, struct dnet_dap_msg *msgs, int max)
{
    size_t pos = 0; int k = 0;
    while (pos < n && k < max) {
        size_t used = 0;
        if (dnet_dap_decode(b + pos, n - pos, &msgs[k], &used) != DNET_DAP_OK || used == 0)
            return -1;
        pos += used; k++;
    }
    return pos == n ? k : -1;
}

/* ---- 1. ORACLE ANCHORS: real OpenVMS VAX V7.3 DAP, decoded field by field --
 * Every buffer below is a real VMS FAL / VMS COPY Session Control buffer:
 * docs/oracle/vax-copy-fal-dap.hex.txt (VAX<->VAX COPY, rd vms-cd3) and the
 * rd vms-a8a lab captures (OVMX probe <-> real VAX FAL,
 * tests/lab/captures/decnet-fal-dap-20261004/). */

static void test_oracle_config(void)
{
    uint8_t b[64]; struct dnet_dap_msg m; size_t used;
    size_t n = unhex("01003c1007030702000500f7fbd9ffaeac8694e77f", b, sizeof b);
    CHECK(dnet_dap_decode(b, n, &m, &used) == DNET_DAP_OK && used == n, "real VMS CONFIGURATION decodes");
    CHECK(m.op == DNET_DAP_CONFIG && m.u.config.bufsiz == 0x103c &&
          m.u.config.ostype == DNET_DAP_OS_VAXVMS && m.u.config.filesys == DNET_DAP_FS_RMS32 &&
          m.u.config.vernum == 7 && m.u.config.econum == 2 && m.u.config.softver == 5,
          "CONFIG: BUFSIZ 4156, OSTYPE VAX/VMS, FILESYS RMS-32, DAP 7.2 sw 5");
    CHECK(m.u.config.syscap_len == 10, "CONFIG: SYSCAP is a 10-byte EX field");
    CHECK(dnet_dap_syscap_has(&m, DNET_DAP_CAP_SEQ_ORG) && dnet_dap_syscap_has(&m, DNET_DAP_CAP_SEQ_XFER) &&
          dnet_dap_syscap_has(&m, DNET_DAP_CAP_NAME_MSG) && dnet_dap_syscap_has(&m, DNET_DAP_CAP_LEN256) &&
          !dnet_dap_syscap_has(&m, 3) && !dnet_dap_syscap_has(&m, 9),
          "CONFIG: SYSCAP bits (seq org, seq xfer, 2-byte len, NAME; not direct/hash)");
}

static void test_oracle_server_open_reply(void)
{
    /* vms-cd3 seg2 from the accessed VAX: ATTRIBUTES + ALLOCATION + NAME +
     * ACK, blocked, ACK followed by 2 bytes a DAP 7 VMS appends (ignored). */
    uint8_t b[256]; struct dnet_dap_msg m[8];
    size_t n = unhex("020215efb081010100020200000109000080801088cb4d000b020def0300000000000109000000000f"
                     "0225012353595324535953524f4f543a5b5359534d47525d4f564d584441505f522e5458543b310600215f",
                     b, sizeof b);
    int k = split(b, n, m, 8);
    CHECK(k == 4 && m[0].op == DNET_DAP_ATTRIBUTES && m[1].op == DNET_DAP_ALLOC &&
          m[2].op == DNET_DAP_NAME && m[3].op == DNET_DAP_ACKNOWLEDGE,
          "VMS create reply splits into ATTRIBUTES, ALLOCATION, NAME, ACK");
    CHECK(m[0].u.attr.datatype == DNET_DAP_DT_ASCII && m[0].u.attr.org == DNET_DAP_ORG_SEQ &&
          m[0].u.attr.rfm == DNET_DAP_RFM_VAR && m[0].u.attr.rat == DNET_DAP_RAT_CR &&
          m[0].u.attr.alq == 9 && m[0].u.attr.fop == (1u << 18) && (m[0].u.attr.menu >> 21 & 1),
          "ATTRIBUTES: ASCII SEQ VAR CR, ALQ 9 (I-5), FOP SQO, menu bit 21 tolerated");
    CHECK(m[2].u.name.nametype == 1 &&
          strcmp(m[2].u.name.namespec, "SYS$SYSROOT:[SYSMGR]OVMXDAP_R.TXT;1") == 0,
          "NAME (type 15) carries the resolved full spec");
}

static void test_oracle_server_display_reply(void)
{
    /* vms-cd3 seg3: ATTRIBUTES (LRL/HBK/EBK/FFB/SBN present) + ALLOC +
     * PROTECTION (owner [000001,000004]) + NAME + ACK. */
    uint8_t b[256]; struct dnet_dap_msg m[8];
    size_t n = unhex("02021eefb0fd010100020200000109000080801088cb4d3a0001090101000000000b020def03000000"
                     "00000109000000000e02151f0f5b3030303030312c3030303030345d00000a0f0f02250123535953"
                     "24535953524f4f543a5b5359534d47525d4f564d584441505f522e5458543b31060059fa", b, sizeof b);
    int k = split(b, n, m, 8);
    CHECK(k == 5 && m[2].op == DNET_DAP_PROTECTION && m[4].op == DNET_DAP_ACKNOWLEDGE,
          "VMS display reply splits into ATTRIBUTES, ALLOC, PROTECTION, NAME, ACK");
    CHECK(m[0].u.attr.lrl == 58 && m[0].u.attr.hbk == 9 && m[0].u.attr.ebk == 1 &&
          m[0].u.attr.ffb == 0 && m[0].u.attr.sbn == 0,
          "ATTRIBUTES: LRL 58, HBK 9, EBK 1, FFB 0, SBN 0 (the real file's geometry)");
}

static void test_oracle_client_access(void)
{
    /* vms-cd3 seg2 from the ACCESSING VAX (VMS COPY): ATTRIBUTES + ALLOC +
     * PROTECTION + ACCESS(CREATE), ACCESS unblocked to segment end (with the
     * DAP 7 trailing bytes after PASSWORD -- bounded, ignored). */
    uint8_t b[256]; struct dnet_dap_msg m[8];
    size_t n = unhex("020210efa00401000202000001018080103a000b020def0300000000000101000000000e0201000300"
                     "02010e4f564d584441505f522e5458543b5340850200e900874e", b, sizeof b);
    int k = split(b, n, m, 8);
    CHECK(k == 4 && m[3].op == DNET_DAP_ACCESS, "VMS COPY setup splits into ATTRIBUTES, ALLOC, PROTECTION, ACCESS");
    CHECK(m[0].u.attr.lrl == 58 && m[0].u.attr.alq == 1, "accessor ATTRIBUTES: LRL 58, ALQ 1");
    CHECK(m[3].u.access.accfunc == DNET_DAP_ACC_CREATE && m[3].u.access.accopt == 1 &&
          strcmp(m[3].u.access.filespec, "OVMXDAP_R.TXT;") == 0 &&
          m[3].u.access.fac == 0x53 && m[3].u.access.shr == 0x40 &&
          m[3].u.access.display == 0x105,
          "ACCESS: CREATE, filespec, FAC PUT|GET|TRN|BRO, SHR NIL, DISPLAY main+alloc+NAME");
}

static void test_oracle_block_data(void)
{
    /* vms-cd3 seg5: CONTROL(PUT, RAC block-file, KEY VBN 1) + DATA with
     * LENGTH+LEN256 (operand 112: RECNUM VBN 1 + 110 bytes of the VAR file's
     * on-disk blocks) + ACCESS COMPLETE(END-OF-STREAM). */
    uint8_t b[256]; struct dnet_dap_msg m[8];
    size_t n = unhex("0402060443050101080806700001013a0048656c6c6f2066726f6d2056415831206e6f646520312e"
                     "31202d204441502f46414c206f7261636c652063617074757265206c696e65206f6e6530005365"
                     "636f6e64206c696e6520666f722061206d756c74692d7265636f726420444150206461746120"
                     "7472616e736665720700045714", b, sizeof b);
    int k = split(b, n, m, 8);
    CHECK(k == 3 && m[0].op == DNET_DAP_CONTROL && m[1].op == DNET_DAP_DATA &&
          m[2].op == DNET_DAP_ACCESS_COMPLETE, "VMS block put splits into CONTROL, DATA, ACCOMP");
    CHECK(m[0].u.control.ctlfunc == DNET_DAP_CTL_PUT && m[0].u.control.rac == DNET_DAP_RAC_BLKFILE &&
          m[0].u.control.keylen == 1 && m[0].u.control.key[0] == 1,
          "CONTROL: PUT, RAC block-mode file transfer, KEY = VBN 1");
    CHECK(m[1].u.data.recnum == 1 && m[1].u.data.reclen == 110 &&
          memcmp(m[1].u.data.rec + 2, "Hello from VAX1", 15) == 0,
          "DATA: LEN256 operand 112 -> RECNUM 1 + 110 data bytes, verbatim");
    CHECK(m[2].u.complete.cmpfunc == DNET_DAP_CMP_EOS, "ACCESS COMPLETE: END-OF-STREAM");
}

static void test_lab_get_and_status(void)
{
    /* rd vms-a8a lab: the real VAX FAL answering OVMX's DAP 5.6 GET. Note: no
     * trailing bytes -- the DAP 7 extras are absent when the peer speaks 5.6. */
    uint8_t b[256]; struct dnet_dap_msg m[8];
    size_t n = unhex("02021aefb07d010002020000010900000088cb4d0e00010901011a00000f021e011c53595324535953"
                     "524f4f543a5b5359534d47525d54312e5458543b310600", b, sizeof b);
    int k = split(b, n, m, 8);
    CHECK(k == 3 && m[0].op == DNET_DAP_ATTRIBUTES && m[1].op == DNET_DAP_NAME &&
          m[2].op == DNET_DAP_ACKNOWLEDGE &&
          strcmp(m[1].u.name.namespec, "SYS$SYSROOT:[SYSMGR]T1.TXT;1") == 0,
          "lab: VMS FAL open reply to OVMX = ATTRIBUTES, NAME, ACK");
    n = unhex("08060f000048656c6c6f206c696e65206f6e6508060900006c696e652074776f09002750", b, sizeof b);
    k = split(b, n, m, 8);
    CHECK(k == 3 && m[0].op == DNET_DAP_DATA && m[0].u.data.reclen == 14 &&
          memcmp(m[0].u.data.rec, "Hello line one", 14) == 0 &&
          m[1].u.data.reclen == 8 && memcmp(m[1].u.data.rec, "line two", 8) == 0 &&
          m[2].op == DNET_DAP_STATUS && m[2].u.status.stscode == DNET_DAP_STS_EOF,
          "lab: two blocked DATA records (LEN256 form) + STATUS EOF (MAC 5 / MIC 047)");
    n = unhex("090032400000021009", b, sizeof b);
    k = split(b, n, m, 8);
    CHECK(k == 1 && DNET_DAP_MAC(m[0].u.status.stscode) == DNET_DAP_MAC_OPEN &&
          DNET_DAP_MIC(m[0].u.status.stscode) == DNET_DAP_MIC_FNF &&
          m[0].u.status.have_stv && m[0].u.status.stv == 0x0910,
          "lab: file-not-found STATUS = MAC 4 / MIC 062, STV SS$_NOSUCHFILE");
}

/* ---- 2. ENCODER = the bytes a real VMS FAL accepted (rd vms-a8a lab) ------ */

static int enc_eq(const struct dnet_dap_msg *m, const char *hex)
{
    uint8_t got[DNET_DAP_MAX_MSG], want[DNET_DAP_MAX_MSG]; size_t gl = 0;
    size_t wl = unhex(hex, want, sizeof want);
    if (dnet_dap_encode(m, 0, got, sizeof got, &gl) != DNET_DAP_OK) return 0;
    return gl == wl && memcmp(got, want, wl) == 0;
}

static void test_encoder_lab_bytes(void)
{
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m); m.op = DNET_DAP_CONTROL; m.u.control.ctlfunc = DNET_DAP_CTL_CONNECT;
    CHECK(enc_eq(&m, "04000200"), "CONTROL(CONNECT) == the accepted 04 00 02 00");
    memset(&m, 0, sizeof m); m.op = DNET_DAP_CONTROL; m.u.control.ctlfunc = DNET_DAP_CTL_GET;
    m.u.control.menu = DNET_DAP_CTLM_RAC; m.u.control.rac = DNET_DAP_RAC_SEQFILE;
    CHECK(enc_eq(&m, "0400010103"), "CONTROL(GET, RAC seq-file) == the accepted 04 00 01 01 03");
    m.u.control.ctlfunc = DNET_DAP_CTL_PUT;
    CHECK(enc_eq(&m, "0400040103"), "CONTROL(PUT, RAC seq-file) == the accepted 04 00 04 01 03");
    memset(&m, 0, sizeof m); m.op = DNET_DAP_ACCESS_COMPLETE; m.u.complete.cmpfunc = DNET_DAP_CMP_CLOSE;
    CHECK(enc_eq(&m, "070001"), "ACCESS COMPLETE(CLOSE) == the accepted 07 00 01");
    memset(&m, 0, sizeof m); m.op = DNET_DAP_DATA; m.u.data.reclen = 13; memcpy(m.u.data.rec, "second record", 13);
    CHECK(enc_eq(&m, "0800007365636f6e64207265636f7264"), "DATA(record) == the accepted 08 00 00 <rec>");
    memset(&m, 0, sizeof m); m.op = DNET_DAP_ACCESS; m.u.access.accfunc = DNET_DAP_ACC_OPEN;
    strcpy(m.u.access.filespec, "SYS$SYSROOT:[SYSMGR]T1.TXT");
    m.u.access.have_fac = m.u.access.have_shr = m.u.access.have_display = 1;
    m.u.access.fac = DNET_DAP_FB_GET; m.u.access.shr = DNET_DAP_FB_GET;
    m.u.access.display = DNET_DAP_DSP_MAIN | DNET_DAP_DSP_NAME;
    CHECK(enc_eq(&m, "030001001a53595324535953524f4f543a5b5359534d47525d54312e54585402028102"),
          "ACCESS(OPEN, FAC/SHR GET, DISPLAY main+NAME) == the accepted bytes");
    /* OVMX CONFIGURATION: DAP 5.6, VAX/VMS + RMS-32, and a SYSCAP naming only
     * what OVMX serves -- decodes back to exactly those capabilities. */
    dnet_dap_ovmx_config(&m, 1459);
    uint8_t b[64]; size_t l = 0, used = 0; struct dnet_dap_msg d;
    CHECK(dnet_dap_encode(&m, 0, b, sizeof b, &l) == DNET_DAP_OK &&
          dnet_dap_decode(b, l, &d, &used) == DNET_DAP_OK && used == l &&
          d.u.config.vernum == 7 && d.u.config.econum == 2 &&
          dnet_dap_syscap_has(&d, DNET_DAP_CAP_SEQ_XFER) && dnet_dap_syscap_has(&d, DNET_DAP_CAP_SEQ_ORG) &&
          !dnet_dap_syscap_has(&d, 7) && !dnet_dap_syscap_has(&d, 21),
          "OVMX CONFIG: DAP 7.2 (rd vms-b2f), advertises seq org + seq file transfer, NOT VBN/block or checksum");
    CHECK(dnet_dap_syscap_has(&d, DNET_DAP_CAP_SUMMARY) && dnet_dap_syscap_has(&d, DNET_DAP_CAP_DATETIME) &&
          dnet_dap_syscap_has(&d, DNET_DAP_CAP_PROTECTION) && dnet_dap_syscap_has(&d, DNET_DAP_CAP_RENAME) &&
          dnet_dap_syscap_has(&d, DNET_DAP_CAP_WILDCARD) && !dnet_dap_syscap_has(&d, 22) &&
          !dnet_dap_syscap_has(&d, 23) && !dnet_dap_syscap_has(&d, 28),
          "OVMX CONFIG (vms-277a): advertises SUMMARY, DATE AND TIME, PROTECTION, RENAME, WILDCARD; NOT KEYDEF, ALLOC, ACL");
    /* LENGTH form for blocking, incl. LEN256 above 255. */
    memset(&m, 0, sizeof m); m.op = DNET_DAP_DATA; m.u.data.reclen = 300;
    memset(m.u.data.rec, 'x', 300);
    CHECK(dnet_dap_encode(&m, 1, b, 3, &l) == DNET_DAP_ENOSPACE, "encode refuses a too-small buffer");
    uint8_t big[DNET_DAP_MAX_MSG];
    CHECK(dnet_dap_encode(&m, 1, big, sizeof big, &l) == DNET_DAP_OK && big[1] == 0x06 &&
          big[2] == (301 & 0xff) && big[3] == (301 >> 8) &&
          dnet_dap_decode(big, l, &d, &used) == DNET_DAP_OK && d.u.data.reclen == 300 && used == l,
          "LENGTH+LEN256 blocked DATA (operand 301) round-trips");
}

/* ---- 3. fuzz the DAP decoder: no crash, clean reject ---------------------- */

static void test_dap_fuzz(void)
{
    unsigned bad = 0;
    struct dnet_dap_msg m; size_t consumed;
    srand(1234);
    for (int i = 0; i < 200000; i++) {
        uint8_t f[96];
        size_t n = (size_t)(rand() % (int)sizeof f);
        for (size_t k = 0; k < n; k++) f[k] = (uint8_t)rand();
        if (n >= 2 && (i & 1)) f[0] = (uint8_t)(1 + rand() % 16);   /* bias to known types */
        int rc = dnet_dap_decode(f, n, &m, &consumed);
        if (rc == DNET_DAP_OK && (consumed > n || consumed == 0)) bad++;
    }
    CHECK(bad == 0, "DAP decoder: 200k random inputs, never over-consume, no crash");

    /* Mutate every real oracle buffer: byte flips + every truncation. */
    static const char *seeds[] = {
        "020215efb081010100020200000109000080801088cb4d000b020def0300000000000109000000000f"
        "0225012353595324535953524f4f543a5b5359534d47525d4f564d584441505f522e5458543b310600215f",
        "0402060443050101080806700001013a0048656c6c6f2066726f6d2056415831206e6f646520312e",
        "030001001a53595324535953524f4f543a5b5359534d47525d54312e54585402028102",
        "090032400000021009", "01003c1007030702000500f7fbd9ffaeac8694e77f",
    };
    unsigned crashes_would_be = 0;
    for (size_t si = 0; si < sizeof seeds / sizeof seeds[0]; si++) {
        uint8_t base[256]; size_t n = unhex(seeds[si], base, sizeof base);
        for (size_t p = 0; p <= n; p++) {           /* every truncation */
            size_t pos = 0;
            while (pos < p) {
                if (dnet_dap_decode(base + pos, p - pos, &m, &consumed) != DNET_DAP_OK) break;
                if (consumed == 0 || consumed > p - pos) { crashes_would_be++; break; }
                pos += consumed;
            }
        }
        for (int it = 0; it < 20000; it++) {        /* random byte mutations */
            uint8_t f[256]; memcpy(f, base, n);
            int flips = 1 + rand() % 4;
            for (int q = 0; q < flips; q++) f[rand() % n] = (uint8_t)rand();
            size_t pos = 0;
            while (pos < n) {
                if (dnet_dap_decode(f + pos, n - pos, &m, &consumed) != DNET_DAP_OK) break;
                if (consumed == 0 || consumed > n - pos) { crashes_would_be++; break; }
                pos += consumed;
            }
        }
    }
    CHECK(crashes_would_be == 0, "DAP decoder: oracle buffers truncated + 100k mutations walk bounded");

    /* A LENGTH that overruns the buffer, LEN256 without LENGTH, a segmented
     * message, BITCNT on a non-DATA message: each refused, never guessed. */
    uint8_t x1[] = { 0x02, 0x02, 0x40, 0x01 };
    uint8_t x2[] = { 0x02, 0x04, 0x01, 0x01 };
    uint8_t x3[] = { 0x08, 0x40, 0x00 };
    uint8_t x4[] = { 0x06, 0x08, 0x01 };
    CHECK(dnet_dap_decode(x1, sizeof x1, &m, &consumed) == DNET_DAP_ETRUNC &&
          dnet_dap_decode(x2, sizeof x2, &m, &consumed) == DNET_DAP_EINVAL &&
          dnet_dap_decode(x3, sizeof x3, &m, &consumed) == DNET_DAP_EUNSUP &&
          dnet_dap_decode(x4, sizeof x4, &m, &consumed) == DNET_DAP_EINVAL,
          "overrun LENGTH / LEN256-without-LENGTH / segmented / misplaced BITCNT are refused");
    /* An EX field that never terminates within its declared maximum. */
    uint8_t x5[] = { 0x02, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01 };
    CHECK(dnet_dap_decode(x5, sizeof x5, &m, &consumed) == DNET_DAP_EBADLEN,
          "an EX-6 ATTMENU running past 6 bytes is refused");
}

/* ---- 4. FAL access decoder against the EXACT oracle connect bytes -------- */

static void test_fal_access_oracle(void)
{
    /*
     * docs/oracle/vax-copy-fal-dap.md §1 -- the Session Control connect DATA of
     * the real COPY's Connect Initiate (object 17 = FAL), byte for byte:
     *   00 11                DSTNAME = format 0, object 0x11 = 17 (FAL)
     *   02 00 1a 02 20 20 06 "SYSTEM"   SRCNAME = format 2, grp 0x021a usr 0x2020
     *   27                   MENUVER
     *   06 "SYSTEM"          access-control USERID  (the username FAL checks)
     *   06 "cdef12"          access-control PASSWORD (retained; placeholder here)
     *   00                   access-control ACCOUNT (empty)
     * The password value in the oracle is redacted; the STRUCTURE (tag/len/
     * position) is what is fixed, so any 6-byte password exercises the decode.
     */
    static const uint8_t conn[] = {
        0x00, 0x11,
        0x02, 0x00, 0x1a, 0x02, 0x20, 0x20, 0x06, 'S','Y','S','T','E','M',
        0x27,
        0x06, 'S','Y','S','T','E','M',
        0x06, 'c','d','e','f','1','2',
        0x00,                /* access-control ACCOUNT (empty) */
        0x00                 /* USRDATA (empty) -- the oracle frame's last byte */
    };
    char user[65], pass[65], acct[65];
    int rc = dnet_fal_access_decode(conn, sizeof conn,
                                    user, sizeof user, pass, sizeof pass,
                                    acct, sizeof acct);
    CHECK(rc == 0, "oracle FAL connect decodes");
    CHECK(strcmp(user, "SYSTEM") == 0, "FAL access userid == SYSTEM (oracle)");
    CHECK(strcmp(pass, "cdef12") == 0, "FAL access password RETAINED (the FAL difference)");
    CHECK(acct[0] == '\0', "FAL access account empty (oracle)");

    /* The builder re-emits those connect bytes byte-identical (proves the client
     * puts the creds on the wire exactly where the real VAX did). */
    uint8_t built[64]; size_t blen = 0;
    int brc = dnet_cterm_sc_connect_build(DNET_OBJ_FAL, "SYSTEM", 0x021a, 0x2020,
                                          "SYSTEM", "cdef12", "",
                                          built, sizeof built, &blen);
    CHECK(brc == 0, "FAL connect builds");
    CHECK(blen == sizeof conn && memcmp(built, conn, sizeof conn) == 0,
          "built FAL connect is BYTE-IDENTICAL to the oracle connect data");
}

/* ---- 5. fuzz the FAL access decoder: bounded, never retains on failure --- */

static void test_fal_access_fuzz(void)
{
    unsigned leaked = 0;
    srand(4321);
    for (int i = 0; i < 200000; i++) {
        uint8_t f[48];
        size_t n = (size_t)(rand() % (int)sizeof f);
        for (size_t k = 0; k < n; k++) f[k] = (uint8_t)rand();
        char user[65], pass[65], acct[65];
        int rc = dnet_fal_access_decode(f, n, user, sizeof user,
                                        pass, sizeof pass, acct, sizeof acct);
        /* On ANY failure every buffer must be empty (no half-parsed credential
         * survives to reach the authenticator). NUL-termination is guaranteed. */
        if (rc != 0 && (user[0] || pass[0] || acct[0])) leaked++;
    }
    CHECK(leaked == 0, "FAL access decoder: 200k random inputs, no credential leaks on failure, no crash");

    /* Truncation of the oracle connect: every prefix either decodes cleanly with
     * empty tail fields or rejects -- never over-reads (ASan enforces). */
    static const uint8_t conn[] = {
        0x00, 0x11, 0x02, 0x00, 0x1a, 0x02, 0x20, 0x20, 0x06,
        'S','Y','S','T','E','M', 0x27, 0x06, 'S','Y','S','T','E','M',
        0x06, 'p','w','1','2','3','4', 0x00
    };
    for (size_t p = 0; p <= sizeof conn; p++) {
        char user[65], pass[65], acct[65];
        (void)dnet_fal_access_decode(conn, p, user, sizeof user,
                                     pass, sizeof pass, acct, sizeof acct);
    }
    CHECK(1, "FAL access decoder: every truncated prefix handled without over-read");
}

int main(void)
{
    printf("test_dnet_dap: DAP codec + FAL access decoder (rd vms-8c2, vms-a8a)\n");
    test_oracle_config();
    test_oracle_server_open_reply();
    test_oracle_server_display_reply();
    test_oracle_client_access();
    test_oracle_block_data();
    test_lab_get_and_status();
    test_encoder_lab_bytes();
    test_dap_fuzz();
    test_fal_access_oracle();
    test_fal_access_fuzz();
    printf("test_dnet_dap: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
