/*
 * test_scs_mscp_srv.c - vms-291 (MSCP epic Phase D): the MSCP disk SERVER
 * responder.
 *
 * WHAT MAKES THIS TEST WORTH ANYTHING. Phase B could prove its command builders
 * wire-neutral because the corpus held real captured joiner COMMANDS. The same
 * proof is available for END MESSAGES and had not been used: a real VAX's MSCP
 * server answers are in our lab captures. Censused over all 489 pcaps, filtered
 * to SCS MTYPE 10 and read at body[8]:
 *
 *   SCA content 86   body[8] = 0x84 (OP.SCC|OP.END)    954 frames
 *   SCA content 110  body[8] = 0x83 (OP.GUS|OP.END)  18855 frames
 *
 * and the SCC ENDs pair EXACTLY with the 954 SCC commands in the same corpus,
 * which is what identifies the class rather than merely being consistent with
 * it. THE GOLDEN BODY below is one of those frames, byte for byte, from
 * af2-firsttimer-established-20260728.pcap. Requiring the builder to reproduce
 * it is the same standard Phase B held itself to.
 *
 * Clean-room (CLAUDE.md Rule 8): the layouts come from AA-L619A-TK Table A-7
 * (public doc) and the bytes from our own lab captures. Nothing disassembled.
 */
#include "scs_mscp_srv.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks = 0;
static int failures = 0;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL %s\n", what);
    }
}

static uint16_t u16(const uint8_t *b, size_t o)
{
    return (uint16_t)((uint16_t)b[o] | ((uint16_t)b[o + 1] << 8));
}

static uint32_t u32(const uint8_t *b, size_t o)
{
    return (uint32_t)u16(b, o) | ((uint32_t)u16(b, o + 2) << 16);
}

static void wle16(uint8_t *b, size_t o, uint16_t v)
{
    b[o] = (uint8_t)(v & 0xffu);
    b[o + 1] = (uint8_t)((v >> 8) & 0xffu);
}

static void wle32(uint8_t *b, size_t o, uint32_t v)
{
    wle16(b, o, (uint16_t)(v & 0xffffu));
    wle16(b, o + 2, (uint16_t)((v >> 16) & 0xffffu));
}

/* ===================== THE GOLDEN SCC END MESSAGE ========================= */

/*
 * SCA content[58:86] of a real VAX SET CONTROLLER CHARACTERISTICS end message,
 * af2-firsttimer-established-20260728.pcap. Decoded against Table A-7:
 *
 *   [0:4]   P.CRF  0x81a30002  the command reference number it answers --
 *                              and note it is SCS_MSCP_CMD_REF(SCC_CLASS=2,
 *                              MSGID=0x81a3), i.e. exactly the constants
 *                              scs_mscp.h already carries for our own client.
 *   [4:6]   P.UNIT 0x0002      sec 6.16 makes this field RESERVED on SCC (the
 *                              command addresses the controller, not a unit);
 *                              the server echoes 2 and we reproduce that.
 *   [8]     endcode 0x84       OP.SCC | OP.END
 *   [9]     P.FLGS 0x00
 *   [10:12] P.STS  0x0000      Success/Normal -- sec 6.16 gives SCC no other
 *   [12:14] P.VRSN 0x0000      MSCP version
 *   [14:16] P.CNTF 0xa004      CONSTANT over 954/954 -- NOT a Table A-4 flag
 *                              word and NOT an echo of the host's request
 *   [16:18] P.CTMO 20          controller timeout, seconds
 *   [18:20] 0x0547             Table A-7 says RESERVED; a real server disagrees
 *   [20:28] P.CNTI 0x0104000000000401  controller identifier
 */
static const uint8_t golden_scc_end[] = {
    0x02, 0x00, 0xa3, 0x81, 0x02, 0x00, 0x00, 0x00,
    0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xa0,
    0x14, 0x00, 0x47, 0x05, 0x01, 0x04, 0x00, 0x00,
    0x00, 0x00, 0x04, 0x01,
};

#define GOLDEN_SCC_CMD_REF 0x81a30002u
#define GOLDEN_SCC_UNIT    0x0002u
#define GOLDEN_CTLR_ID     ((uint64_t)0x0104000000000401ULL)
#define GOLDEN_CTLR_TIMEOUT 20u
#define GOLDEN_CNTF        0xa004u
#define GOLDEN_VERSION_WORD 0x0547u

/* A SET CONTROLLER CHARACTERISTICS *command* body, as our own client builds it
 * (scs_mscp_scc_defaults): version 0, the af2 flags, timeout 0. */
static void make_scc_command(uint8_t body[SCS_MSCP_BODY_LEN],
                             struct scs_mscp_view *v, uint32_t cmd_ref,
                             uint16_t unit, uint16_t version)
{
    struct scs_mscp_cmd c;
    scs_mscp_scc_defaults(&c, cmd_ref);
    c.unit = unit;
    c.scc_version = version;
    scs_mscp_build_body(&c, body, SCS_MSCP_BODY_LEN);

    memset(v, 0, sizeof(*v));
    v->cmd_ref = cmd_ref;
    v->unit = unit;
    v->opcode = SCS_MSCP_OP_SET_CTLR_CHAR;
    v->base_opcode = SCS_MSCP_OP_SET_CTLR_CHAR;
    v->modifiers = 0;
}

/* A generic command view + body for the non-SCC opcodes. */
static void make_command(uint8_t *body, size_t body_len,
                         struct scs_mscp_view *v, uint32_t cmd_ref,
                         uint16_t unit, uint8_t opcode, uint16_t modifiers)
{
    memset(body, 0, body_len);
    body[SCS_MSCP_P_CRF + 0] = (uint8_t)(cmd_ref & 0xff);
    body[SCS_MSCP_P_CRF + 1] = (uint8_t)((cmd_ref >> 8) & 0xff);
    body[SCS_MSCP_P_CRF + 2] = (uint8_t)((cmd_ref >> 16) & 0xff);
    body[SCS_MSCP_P_CRF + 3] = (uint8_t)((cmd_ref >> 24) & 0xff);
    body[SCS_MSCP_P_UNIT + 0] = (uint8_t)(unit & 0xff);
    body[SCS_MSCP_P_UNIT + 1] = (uint8_t)((unit >> 8) & 0xff);
    body[SCS_MSCP_P_OPCD] = opcode;
    body[SCS_MSCP_P_MOD + 0] = (uint8_t)(modifiers & 0xff);
    body[SCS_MSCP_P_MOD + 1] = (uint8_t)((modifiers >> 8) & 0xff);

    memset(v, 0, sizeof(*v));
    v->cmd_ref = cmd_ref;
    v->unit = unit;
    v->opcode = opcode;
    v->base_opcode = (uint8_t)(opcode & SCS_MSCP_OPCODE_MASK);
    v->is_end = (opcode & SCS_MSCP_END_BIT) ? 1 : 0;
    v->modifiers = modifiers;
}

/* Bring a server to Controller-Online for conid, the way a real class driver
 * does: by completing a SET CONTROLLER CHARACTERISTICS. */
static void bring_controller_online(struct scs_mscp_srv *srv, uint32_t conid)
{
    uint8_t body[SCS_MSCP_BODY_LEN];
    uint8_t end[SCS_MSCP_SRV_END_MAX];
    struct scs_mscp_view v;
    make_scc_command(body, &v, GOLDEN_SCC_CMD_REF, GOLDEN_SCC_UNIT, 0);
    scs_mscp_srv_handle(srv, conid, &v, body, sizeof(body), end, sizeof(end));
}

/* ============================== the tests ================================ */

/*
 * THE BYTE-EXACT PROOF. Build the SCC end message from fields and require it to
 * equal a real VAX's, byte for byte.
 */
static void test_scc_end_byte_exact(void)
{
    struct scs_mscp_srv srv;
    uint8_t body[SCS_MSCP_BODY_LEN];
    uint8_t end[SCS_MSCP_SRV_END_MAX];
    struct scs_mscp_view v;

    scs_mscp_srv_init(&srv, GOLDEN_CTLR_ID, GOLDEN_CTLR_TIMEOUT);
    scs_mscp_srv_set_ctlr_profile(&srv, GOLDEN_CNTF, GOLDEN_VERSION_WORD);
    make_scc_command(body, &v, GOLDEN_SCC_CMD_REF, GOLDEN_SCC_UNIT, 0);

    long n = scs_mscp_srv_handle(&srv, 0x1234u, &v, body, sizeof(body), end,
                                 sizeof(end));
    check(n == (long)sizeof(golden_scc_end),
          "the SCC end message is SCS_MSCP_SCC_END_LEN (28) bytes, the length "
          "of the captured 86-content class body");
    if (n == (long)sizeof(golden_scc_end)) {
        int same = memcmp(end, golden_scc_end, sizeof(golden_scc_end)) == 0;
        check(same, "the field-built SCC end message is BYTE-IDENTICAL to the "
                    "real VAX server's answer in af2-firsttimer-established");
        if (!same) {
            printf("    built:  ");
            for (size_t i = 0; i < sizeof(golden_scc_end); i++)
                printf("%02x ", end[i]);
            printf("\n    golden: ");
            for (size_t i = 0; i < sizeof(golden_scc_end); i++)
                printf("%02x ", golden_scc_end[i]);
            printf("\n");
        }
    }

    /* The SCC is what takes the class driver Controller-Online (sec 3.4). */
    struct scs_mscp_srv_host *h = scs_mscp_srv_host_for(&srv, 0x1234u);
    check(h != NULL && h->ctlr_online,
          "completing SET CONTROLLER CHARACTERISTICS takes the class driver "
          "Controller-Online (sec 3.4)");
    /* sec 5.8: the host's requested flags are per-class-driver state, kept even
     * though the end message does not echo them. */
    check(h != NULL && h->ctlr_flags == SCS_MSCP_SCC_CTLR_FLAGS,
          "the host's REQUESTED controller flags are recorded on the host "
          "record even though the end message reports 0xa004 instead");
}

/*
 * sec 6.16: a non-zero MSCP version must be answered Invalid Command. Table A-1
 * note: that end message carries JUST OP.END, not the opcode with OP.END added.
 */
static void test_scc_rejects_nonzero_version(void)
{
    struct scs_mscp_srv srv;
    uint8_t body[SCS_MSCP_BODY_LEN];
    uint8_t end[SCS_MSCP_SRV_END_MAX];
    struct scs_mscp_view v;

    scs_mscp_srv_init(&srv, GOLDEN_CTLR_ID, GOLDEN_CTLR_TIMEOUT);
    make_scc_command(body, &v, 0x11112222u, 0, 1 /* illegal */);

    long n = scs_mscp_srv_handle(&srv, 1u, &v, body, sizeof(body), end,
                                 sizeof(end));
    check(n > 0, "a bad SCC still produces an end message, never silence");
    check(n > 0 && end[SCS_MSCP_P_OPCD] == SCS_MSCP_END_BIT,
          "the Invalid Command end message carries JUST OP.END (0x80), not "
          "OP.SCC|OP.END -- Table A-1's note");
    check(n > 0 && scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
                       == SCS_MSCP_ST_INVALID_CMD,
          "a non-zero MSCP version is Invalid Command (sec 6.16)");
    check(scs_mscp_srv_host_for(&srv, 1u)->ctlr_online == 0,
          "a REFUSED SET CONTROLLER CHARACTERISTICS does NOT take the class "
          "driver Controller-Online");
}

/*
 * sec 3.4: everything else requires Controller-Online first. This is also what
 * stops a stray frame on a half-open connection from mounting a volume.
 */
static void test_commands_require_controller_online(void)
{
    struct scs_mscp_srv srv;
    uint8_t body[SCS_MSCP_BODY_LEN];
    uint8_t end[SCS_MSCP_SRV_END_MAX];
    struct scs_mscp_view v;
    const uint8_t ops[] = {SCS_MSCP_OP_GET_UNIT_STATUS, SCS_MSCP_OP_ONLINE,
                           SCS_MSCP_OP_READ, SCS_MSCP_OP_WRITE};

    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        scs_mscp_srv_init(&srv, GOLDEN_CTLR_ID, GOLDEN_CTLR_TIMEOUT);
        make_command(body, sizeof(body), &v, 0xabcd0001u, 0, ops[i], 0);
        long n = scs_mscp_srv_handle(&srv, 9u, &v, body, sizeof(body), end,
                                     sizeof(end));
        check(n > 0 && scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
                           == SCS_MSCP_ST_INVALID_CMD,
              "a command before Controller-Online is Invalid Command (sec 3.4)");
    }
}

/*
 * The GET UNIT STATUS walk: MD.NXU enumeration, the Table A-7 field placement,
 * and the Unit-Offline terminator OVMX's own client walk looks for.
 */
static void test_gus_walk_and_fields(void)
{
    struct scs_mscp_srv srv;
    uint8_t body[SCS_MSCP_BODY_LEN];
    uint8_t end[SCS_MSCP_SRV_END_MAX];
    struct scs_mscp_view v;
    int fd = open("/dev/zero", O_RDONLY);
    check(fd >= 0, "test fixture: /dev/zero opens");

    scs_mscp_srv_init(&srv, GOLDEN_CTLR_ID, GOLDEN_CTLR_TIMEOUT);
    bring_controller_online(&srv, 7u);
    check(scs_mscp_srv_attach_fd(&srv, 5, fd, 1000, 0xdeadbeefULL, 0x2452,
                                 0x11223344) == 0,
          "a unit attaches");

    /* MD.NXU from 0 must find unit 5 (sec 6.12: "the next known unit >= the
     * specified unit number"), not miss it for want of an exact match. */
    make_command(body, sizeof(body), &v, 0x1u, 0, SCS_MSCP_OP_GET_UNIT_STATUS,
                 SCS_MSCP_MOD_NEXT_UNIT);
    long n = scs_mscp_srv_handle(&srv, 7u, &v, body, sizeof(body), end,
                                 sizeof(end));
    check(n == SCS_MSCP_GUS_END_LEN && SCS_MSCP_GUS_END_LEN == 52,
          "a GUS end message is 52 bytes -- what a REAL server emits (110 SCA "
          "content), NOT Table A-7's 48, which is a length no VAX has emitted");
    check(u16(end, SCS_MSCP_E_GUS_TAIL) == SCS_MSCP_E_GUS_TAIL_OBSERVED,
          "body[48:50] carries the observed 0x006e -- COPIED, not composed: a "
          "real server always writes it, and length-echo vs plain-constant is "
          "still undecidable on a single-length population");
    check(u16(end, SCS_MSCP_E_GUS_TAIL + 2) == 0,
          "body[50:52] is left ZERO -- a real server leaves it as stale "
          "garbage, so there is nothing to copy and we do not invent one");
    check(end[SCS_MSCP_P_OPCD] == (SCS_MSCP_OP_GET_UNIT_STATUS
                                   | SCS_MSCP_END_BIT),
          "the GUS endcode is 0x83");
    check(u16(end, SCS_MSCP_P_UNIT) == 5,
          "MD.NXU from unit 0 reports the unit it FOUND (5), not the one asked "
          "for -- that is what makes the walk advance");
    check(scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
              == SCS_MSCP_ST_AVAILABLE,
          "a served but not-yet-ONLINEd unit is Unit-Available (sec 6.12)");
    /* Table A-7 field placement. */
    check(u16(end, SCS_MSCP_E_UNFL) == SCS_MSCP_UF_WRITE_PROT_SW,
          "the unit advertises UF.WPS -- design decision (2): read-only is "
          "declared on the wire, not merely enforced");
    check(u32(end, SCS_MSCP_E_UNTI) == 0xdeadbeefu,
          "P.UNTI (unit identifier) lands at Table A-7 offset 20");
    check(u32(end, SCS_MSCP_E_MEDI) == 0x2452u,
          "P.MEDI (media type identifier) lands at Table A-7 offset 28");
    check(u16(end, SCS_MSCP_E_SHUN) == 5,
          "P.SHUN (shadow unit) == the unit number (sec 6.12)");

    /* The walk continues past 5 and must terminate. */
    make_command(body, sizeof(body), &v, 0x2u, 6, SCS_MSCP_OP_GET_UNIT_STATUS,
                 SCS_MSCP_MOD_NEXT_UNIT);
    n = scs_mscp_srv_handle(&srv, 7u, &v, body, sizeof(body), end, sizeof(end));
    check(n == SCS_MSCP_GUS_END_LEN
              && scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
                     == SCS_MSCP_ST_OFFLINE,
          "the walk terminates with Unit-Offline -- the terminator OVMX's OWN "
          "client walk (scs_mscp.c) looks for");
    check(u32(end, SCS_MSCP_E_UNTI) == 0,
          "the terminator's unit identifier is 0, which sec 6.12 makes mean "
          "'virtually no characteristics are valid'");

    /* Without MD.NXU it is an exact-match lookup and unit 6 is simply absent. */
    make_command(body, sizeof(body), &v, 0x3u, 5, SCS_MSCP_OP_GET_UNIT_STATUS,
                 0);
    n = scs_mscp_srv_handle(&srv, 7u, &v, body, sizeof(body), end, sizeof(end));
    check(n > 0 && u16(end, SCS_MSCP_P_UNIT) == 5,
          "without MD.NXU an exact match still answers");
    close(fd);
}

/* ONLINE: the mount-verify step that makes a unit transferable, and the
 * "Already Online" SUCCESS sub-code that must not be reported as an error. */
static void test_online(void)
{
    struct scs_mscp_srv srv;
    uint8_t body[SCS_MSCP_BODY_LEN];
    uint8_t end[SCS_MSCP_SRV_END_MAX];
    struct scs_mscp_view v;
    int fd = open("/dev/zero", O_RDONLY);

    scs_mscp_srv_init(&srv, GOLDEN_CTLR_ID, GOLDEN_CTLR_TIMEOUT);
    bring_controller_online(&srv, 3u);
    scs_mscp_srv_attach_fd(&srv, 1, fd, 4096, 0xcafe1234ULL, 0x2452, 0xaabbccdd);

    make_command(body, sizeof(body), &v, 0x100u, 1, SCS_MSCP_OP_ONLINE, 0);
    long n = scs_mscp_srv_handle(&srv, 3u, &v, body, sizeof(body), end,
                                 sizeof(end));
    check(n == SCS_MSCP_ONLINE_END_LEN,
          "an ONLINE end message is 44 bytes (Table A-7)");
    check(end[SCS_MSCP_P_OPCD] == (SCS_MSCP_OP_ONLINE | SCS_MSCP_END_BIT),
          "the ONLINE endcode is 0x89");
    check(scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
              == SCS_MSCP_ST_SUCCESS,
          "ONLINE on a served unit succeeds");
    check(u32(end, SCS_MSCP_E_UNSZ) == 4096u,
          "P.UNSZ carries the unit size in logical blocks -- the field a "
          "mounting VAX sizes the volume from (Table A-7 offset 36)");
    check(u32(end, SCS_MSCP_E_VSER) == 0xaabbccddu,
          "P.VSER (volume serial number) lands at Table A-7 offset 40");
    check(n == SCS_MSCP_ONLINE_END_LEN && SCS_MSCP_ONLINE_END_LEN == 44,
          "an ONLINE end message is 44 bytes -- confirmed against a REAL "
          "server's 102-content ONLINE end in the vms-291 serving capture");

    /* MEASURED: P.UNFL bit 15 is HOST-ORIGINATED -- the class driver's ONLINE
     * COMMAND carries 0x8000 and the server echoes it. A server that ignored
     * the host's word would answer with flags the host never asked for. */
    {
        struct scs_mscp_view hv;
        uint8_t hb[SCS_MSCP_BODY_LEN], he[SCS_MSCP_SRV_END_MAX];
        make_command(hb, sizeof(hb), &hv, 0x103u, 1, SCS_MSCP_OP_ONLINE, 0);
        hb[SCS_MSCP_E_UNFL] = 0x00;
        hb[SCS_MSCP_E_UNFL + 1] = 0x80; /* the host asks for bit 15 */
        scs_mscp_srv_handle(&srv, 3u, &hv, hb, sizeof(hb), he, sizeof(he));
        check((u16(he, SCS_MSCP_E_UNFL) & 0x8000u) != 0,
              "the ONLINE end message ECHOES the host's requested unit flags "
              "(bit 15 is host-originated, measured on the wire)");
        check((u16(he, SCS_MSCP_E_UNFL) & SCS_MSCP_UF_WRITE_PROT_SW) != 0,
              "...and the unit's OWN UF.WPS survives the echo -- a host must "
              "not be able to clear the write protection by asking nicely");
    }

    /* A second ONLINE is Success/Already Online, NOT an error -- getting this
     * wrong would make a re-MOUNT fail for no reason. */
    make_command(body, sizeof(body), &v, 0x101u, 1, SCS_MSCP_OP_ONLINE, 0);
    n = scs_mscp_srv_handle(&srv, 3u, &v, body, sizeof(body), end, sizeof(end));
    check(scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
              == SCS_MSCP_ST_SUCCESS,
          "a repeated ONLINE is SUCCESS, not an error (sec 6.13)");
    check(scs_mscp_status_subcode(u16(end, SCS_MSCP_P_STS))
              == SCS_MSCP_SUB_ALREADY_ONLINE,
          "...with the 'Already Online' sub-code (Table B-2)");

    /* An unknown unit is Unit-Offline. */
    make_command(body, sizeof(body), &v, 0x102u, 99, SCS_MSCP_OP_ONLINE, 0);
    n = scs_mscp_srv_handle(&srv, 3u, &v, body, sizeof(body), end, sizeof(end));
    check(n > 0 && scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
                       == SCS_MSCP_ST_OFFLINE,
          "ONLINE on a unit we do not serve is Unit-Offline");
    close(fd);
}

/* ===================== the backing store, for real ======================= */

struct xfer_record {
    unsigned long calls;
    uint32_t      last_lbn;
    uint8_t       last[SCS_MSCP_BLOCK_SIZE];
    int           fail;
};

static long recording_xfer(void *ctx, const uint8_t buffer_desc[12],
                           uint32_t lbn, const uint8_t *data, size_t len)
{
    struct xfer_record *r = (struct xfer_record *)ctx;
    (void)buffer_desc;
    r->calls++;
    r->last_lbn = lbn;
    if (len <= sizeof(r->last)) {
        memcpy(r->last, data, len);
    }
    if (r->fail) {
        return -1;
    }
    return (long)len;
}

/* Build a temporary raw block image with recognisable per-block content. */
static int make_image(unsigned nblocks, char *path, size_t path_len)
{
    snprintf(path, path_len, "/tmp/vms291-img-%d.raw", (int)getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return -1;
    }
    uint8_t blk[SCS_MSCP_BLOCK_SIZE];
    for (unsigned i = 0; i < nblocks; i++) {
        memset(blk, (int)(i & 0xff), sizeof(blk));
        /* A marker so a wrong LBN cannot accidentally look right. */
        blk[0] = 'B';
        blk[1] = (uint8_t)(i & 0xff);
        blk[2] = (uint8_t)((i >> 8) & 0xff);
        if (write(fd, blk, sizeof(blk)) != (ssize_t)sizeof(blk)) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static void test_backing_store_and_read(void)
{
    struct scs_mscp_srv srv;
    uint8_t body[SCS_MSCP_BODY_LEN];
    uint8_t end[SCS_MSCP_SRV_END_MAX];
    struct scs_mscp_view v;
    char path[128];
    const unsigned NBLK = 64;
    int fd = make_image(NBLK, path, sizeof(path));
    check(fd >= 0, "test fixture: the raw block image is created");
    if (fd < 0) {
        return;
    }

    scs_mscp_srv_init(&srv, GOLDEN_CTLR_ID, GOLDEN_CTLR_TIMEOUT);
    bring_controller_online(&srv, 2u);
    scs_mscp_srv_attach_fd(&srv, 0, fd, NBLK, 0x5555ULL, 0x2452, 0x1);

    /* --- the backing store itself --- */
    uint8_t buf[SCS_MSCP_BLOCK_SIZE * 2];
    struct scs_mscp_srv_unit *u = scs_mscp_srv_find_unit(&srv, 0);
    check(u != NULL, "the attached unit is findable");
    check(scs_mscp_srv_read_blocks(u, 3, 1, buf, sizeof(buf))
              == SCS_MSCP_BLOCK_SIZE,
          "reading one block returns 512 bytes");
    check(buf[0] == 'B' && buf[1] == 3,
          "block 3 really is block 3 -- the LBN is honoured, not ignored");
    check(scs_mscp_srv_read_blocks(u, NBLK - 1, 2, buf, sizeof(buf)) == -1,
          "a read running past the end of the volume is REFUSED (sec 5.3)");
    /* THE DECLARED UNIT SIZE MUST BOUND THE READ, NOT THE FILE SIZE. Without
     * this arm the bound check is untestable: on a volume that ends where the
     * file ends, deleting the check still fails via the short-read path, so a
     * mutation survives (measured -- guardrail 23). Serving a unit SMALLER than
     * its backing file is the case that separates them, and it is a real one:
     * an image is routinely larger than the volume it carries. */
    {
        struct scs_mscp_srv srv_small;
        scs_mscp_srv_init(&srv_small, GOLDEN_CTLR_ID, GOLDEN_CTLR_TIMEOUT);
        /* NBLK blocks on disk, but we declare only 8 of them served. */
        check(scs_mscp_srv_attach_fd(&srv_small, 0, fd, 8, 0x77ULL, 0, 0) == 0,
              "a unit smaller than its backing file attaches");
        struct scs_mscp_srv_unit *su = scs_mscp_srv_find_unit(&srv_small, 0);
        check(scs_mscp_srv_read_blocks(su, 7, 1, buf, sizeof(buf))
                  == SCS_MSCP_BLOCK_SIZE,
              "the last declared block reads");
        check(scs_mscp_srv_read_blocks(su, 8, 1, buf, sizeof(buf)) == -1,
              "a block PAST THE DECLARED UNIT SIZE is refused even though the "
              "backing file has data there -- the served volume bounds the "
              "read, not the file");
    }
    check(scs_mscp_srv_read_blocks(u, 0, 2, buf, SCS_MSCP_BLOCK_SIZE) == -1,
          "a read into a buffer too small for the blocks asked for is refused");

    /* --- READ BEFORE ONLINE. sec 6.14 lists Unit-Available among READ's
     * statuses: a unit we serve but which no ONLINE has claimed cannot
     * transfer. This arm exists because test_scs_mscp_srv_mutants.py MEASURED
     * that without it the `!u->online` guard can be deleted outright and every
     * other assertion in this file stays green -- the guard was untested, and
     * an untested guard is the one a refactor removes. --- */
    {
        struct scs_mscp_view av;
        uint8_t ab[SCS_MSCP_BODY_LEN], ae[SCS_MSCP_SRV_END_MAX];
        make_command(ab, sizeof(ab), &av, 0x1feu, 0, SCS_MSCP_OP_READ, 0);
        ab[SCS_MSCP_P_BCNT] = (uint8_t)(SCS_MSCP_BLOCK_SIZE & 0xff);
        ab[SCS_MSCP_P_BCNT + 1] = (uint8_t)(SCS_MSCP_BLOCK_SIZE >> 8);
        long an = scs_mscp_srv_handle(&srv, 2u, &av, ab, sizeof(ab), ae,
                                      sizeof(ae));
        check(an > 0 && scs_mscp_status_major(u16(ae, SCS_MSCP_P_STS))
                            == SCS_MSCP_ST_AVAILABLE,
              "a READ on a served unit that no ONLINE has claimed is "
              "Unit-Available, NOT a transfer (sec 6.14)");
        check(an > 0 && u32(ae, SCS_MSCP_E_BCNT) == 0,
              "...and reports zero bytes transferred");
        check(srv.blocks_read == 0,
              "...and never reached the backing store");
    }

    /* --- READ WITH NO TRANSFER HOOK: design decision (4). This is the INV-6
     * boundary and the single most important assertion in this file. --- */
    make_command(body, sizeof(body), &v, 0x200u, 0, SCS_MSCP_OP_READ, 0);
    body[SCS_MSCP_P_BCNT] = (uint8_t)(SCS_MSCP_BLOCK_SIZE & 0xff);
    body[SCS_MSCP_P_BCNT + 1] = (uint8_t)(SCS_MSCP_BLOCK_SIZE >> 8);
    body[SCS_MSCP_P_LBN] = 3;
    /* The unit must be ONLINE before a transfer is legal. */
    {
        struct scs_mscp_view ov;
        uint8_t ob[SCS_MSCP_BODY_LEN], oe[SCS_MSCP_SRV_END_MAX];
        make_command(ob, sizeof(ob), &ov, 0x1ffu, 0, SCS_MSCP_OP_ONLINE, 0);
        scs_mscp_srv_handle(&srv, 2u, &ov, ob, sizeof(ob), oe, sizeof(oe));
    }
    long n = scs_mscp_srv_handle(&srv, 2u, &v, body, sizeof(body), end,
                                 sizeof(end));
    check(n > 0, "a READ with no transfer hook still answers");
    check(scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
              != SCS_MSCP_ST_SUCCESS,
          "A READ THAT CANNOT MOVE ITS DATA MUST NOT REPORT SUCCESS -- this is "
          "the INV-6 boundary; a fake success here is the whole bug class the "
          "project forbids");
    check(scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
              == SCS_MSCP_ST_CTLR_ERR,
          "...it reports Controller Error (block data transfer is ungrounded, "
          "rd vms-941)");
    check(u32(end, SCS_MSCP_E_BCNT) == 0,
          "...and reports ZERO bytes transferred (sec 5.5: the byte count is "
          "what actually crossed)");
    check(srv.xfer_refusals == 1, "the refusal is counted");

    /* --- READ WITH A HOOK: the data really moves, and it is the right data. */
    struct xfer_record rec;
    memset(&rec, 0, sizeof(rec));
    scs_mscp_srv_set_xfer(&srv, recording_xfer, &rec);
    make_command(body, sizeof(body), &v, 0x201u, 0, SCS_MSCP_OP_READ, 0);
    body[SCS_MSCP_P_BCNT] = 0x00;
    body[SCS_MSCP_P_BCNT + 1] = 0x04; /* 1024 == two blocks */
    body[SCS_MSCP_P_LBN] = 7;
    n = scs_mscp_srv_handle(&srv, 2u, &v, body, sizeof(body), end, sizeof(end));
    check(scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
              == SCS_MSCP_ST_SUCCESS,
          "a READ with a working transfer hook succeeds");
    check(u32(end, SCS_MSCP_E_BCNT) == 1024u,
          "...and reports the 1024 bytes that actually crossed");
    check(rec.calls == 2, "two blocks were handed to the transfer service");
    check(rec.last_lbn == 8 && rec.last[1] == 8,
          "the SECOND block delivered was LBN 8, with block 8's content -- so "
          "the blocks are read in order and from the right offsets");
    check(srv.blocks_read == 2, "the block counter moved");

    /* A failing transfer is reported as Host Buffer Access Error (sec 6.14),
     * not swallowed. */
    rec.fail = 1;
    make_command(body, sizeof(body), &v, 0x202u, 0, SCS_MSCP_OP_READ, 0);
    body[SCS_MSCP_P_BCNT] = 0x00;
    body[SCS_MSCP_P_BCNT + 1] = 0x02; /* 512 */
    n = scs_mscp_srv_handle(&srv, 2u, &v, body, sizeof(body), end, sizeof(end));
    check(scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
              == SCS_MSCP_ST_HOST_BUF_ERR,
          "a FAILED block transfer is Host Buffer Access Error (sec 6.14)");

    /* Invalid byte counts and LBNs are refused with the offset-coded sub-code
     * scheme of Table B-2, not serviced. */
    rec.fail = 0;
    make_command(body, sizeof(body), &v, 0x203u, 0, SCS_MSCP_OP_READ, 0);
    body[SCS_MSCP_P_BCNT] = 0x01; /* 513 -- not a whole number of blocks */
    body[SCS_MSCP_P_BCNT + 1] = 0x02;
    n = scs_mscp_srv_handle(&srv, 2u, &v, body, sizeof(body), end, sizeof(end));
    check(scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
              == SCS_MSCP_ST_INVALID_CMD,
          "a byte count that is not a whole number of blocks is Invalid "
          "Command (sec 5.3)");

    make_command(body, sizeof(body), &v, 0x204u, 0, SCS_MSCP_OP_READ, 0);
    body[SCS_MSCP_P_BCNT] = 0x00;
    body[SCS_MSCP_P_BCNT + 1] = 0x02;
    body[SCS_MSCP_P_LBN] = (uint8_t)(NBLK + 10); /* past the end */
    n = scs_mscp_srv_handle(&srv, 2u, &v, body, sizeof(body), end, sizeof(end));
    check(scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
              == SCS_MSCP_ST_INVALID_CMD,
          "an LBN past the end of the volume is Invalid Command (sec 5.3)");

    close(fd);
    unlink(path);
}

/* WRITE in v1: refused with the published status, never silently dropped and
 * never falsely acknowledged. Design decision (2). */
static void test_write_is_refused_honestly(void)
{
    struct scs_mscp_srv srv;
    uint8_t body[SCS_MSCP_BODY_LEN];
    uint8_t end[SCS_MSCP_SRV_END_MAX];
    struct scs_mscp_view v;
    int fd = open("/dev/zero", O_RDONLY);

    scs_mscp_srv_init(&srv, GOLDEN_CTLR_ID, GOLDEN_CTLR_TIMEOUT);
    bring_controller_online(&srv, 4u);
    scs_mscp_srv_attach_fd(&srv, 0, fd, 100, 0x99ULL, 0x2452, 0x1);

    make_command(body, sizeof(body), &v, 0x300u, 0, SCS_MSCP_OP_WRITE, 0);
    long n = scs_mscp_srv_handle(&srv, 4u, &v, body, sizeof(body), end,
                                 sizeof(end));
    check(n == SCS_MSCP_WRITE_END_LEN && SCS_MSCP_WRITE_END_LEN == 36,
          "a WRITE end message is 36 bytes -- MEASURED on a real server, which "
          "declares 36 for WRITE and 32 for READ; Table A-7's generic end is "
          "32, so the two are NOT the same length and assuming so is wrong");
    check(end[SCS_MSCP_P_OPCD] == (SCS_MSCP_OP_WRITE | SCS_MSCP_END_BIT),
          "the WRITE endcode is 0xa2");
    check(scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
              == SCS_MSCP_ST_WRITE_PROT,
          "a WRITE is refused Write Protected -- the v1 read-only decision, "
          "stated in the protocol's own words");
    check(u16(end, SCS_MSCP_P_STS)
              == SCS_MSCP_STATUS(SCS_MSCP_ST_WRITE_PROT,
                                 SCS_MSCP_SUB_WP_SOFTWARE),
          "...with the Software Write Protect sub-code, 0x1006 (Table B-2), "
          "matching the UF.WPS the unit advertised");
    check(srv.writes_refused == 1, "the refusal is counted");
    close(fd);
}

/* Refusals that are NOT end messages: things that must not produce a frame. */
static void test_hard_refusals(void)
{
    struct scs_mscp_srv srv;
    uint8_t body[SCS_MSCP_BODY_LEN];
    uint8_t end[SCS_MSCP_SRV_END_MAX];
    struct scs_mscp_view v;
    scs_mscp_srv_init(&srv, GOLDEN_CTLR_ID, GOLDEN_CTLR_TIMEOUT);

    make_command(body, sizeof(body), &v, 0x1u, 0,
                 SCS_MSCP_OP_GET_UNIT_STATUS | SCS_MSCP_END_BIT, 0);
    check(scs_mscp_srv_handle(&srv, 1u, &v, body, sizeof(body), end,
                              sizeof(end)) == -1,
          "an END MESSAGE arriving at the responder is refused outright -- "
          "answering it would put a second end message on the wire for a "
          "command nobody sent");

    make_command(body, sizeof(body), &v, 0 /* illegal */, 0,
                 SCS_MSCP_OP_SET_CTLR_CHAR, 0);
    check(scs_mscp_srv_handle(&srv, 1u, &v, body, sizeof(body), end,
                              sizeof(end)) == -1,
          "a zero command reference number is refused (sec 5.1: unique, "
          "NON-ZERO) -- there is nothing safe to echo");

    make_command(body, sizeof(body), &v, 0x5u, 0, SCS_MSCP_OP_SET_CTLR_CHAR, 0);
    check(scs_mscp_srv_handle(&srv, 1u, &v, body, sizeof(body), end, 4) == -1,
          "an output buffer too small for even a header is refused");
    check(scs_mscp_srv_handle(&srv, 1u, NULL, body, sizeof(body), end,
                              sizeof(end)) == -1,
          "NULL arguments are refused");

    /* An opcode we do not implement gets the Invalid Command end message. */
    make_command(body, sizeof(body), &v, 0x6u, 0, 0x14 /* REPLACE */, 0);
    long n = scs_mscp_srv_handle(&srv, 1u, &v, body, sizeof(body), end,
                                 sizeof(end));
    check(n > 0 && end[SCS_MSCP_P_OPCD] == SCS_MSCP_END_BIT,
          "an unimplemented opcode gets the Invalid Command end message -- "
          "OVMX says so in the protocol's own words rather than going silent");
}

/* Attach-time refusals that keep the server's own invariants honest. */
static void test_attach_refusals(void)
{
    struct scs_mscp_srv srv;
    int fd = open("/dev/zero", O_RDONLY);
    scs_mscp_srv_init(&srv, GOLDEN_CTLR_ID, GOLDEN_CTLR_TIMEOUT);

    check(scs_mscp_srv_attach_fd(&srv, 0, fd, 100, 0 /* illegal */, 0, 0) == -1,
          "a ZERO unit identifier is refused -- sec 6.12 makes it mean 'no "
          "characteristics are valid', which a unit we serve must not say");
    check(scs_mscp_srv_attach_fd(&srv, 0, -1, 100, 1, 0, 0) == -1,
          "a bad descriptor is refused");
    check(scs_mscp_srv_attach_fd(&srv, 0, fd, 0, 1, 0, 0) == -1,
          "a zero-length volume is refused");
    check(scs_mscp_srv_attach_fd(&srv, 0, fd, 100, 1, 0, 0) == 0,
          "a good attach succeeds");
    check(scs_mscp_srv_attach_fd(&srv, 0, fd, 100, 1, 0, 0) == -1,
          "attaching the SAME unit number twice is refused rather than "
          "silently replacing the first");
    check(scs_mscp_srv_find_unit(&srv, 0)->read_only == 1,
          "every attached unit is read-only in v1");
    close(fd);
}

/* The end message has to ride a frame, with both length words DERIVED. */
static void test_end_frame(void)
{
    struct scs_mscp_params p;
    uint8_t out[256];
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, "\xaa\xbb\xcc\xdd\xee\xff", 6);
    memcpy(p.src_mac, "\x11\x22\x33\x44\x55\x66", 6);
    memcpy(p.src_logical, "\xaa\x00\x04\x00\x01\x04", 6);
    memcpy(p.peer_logical, "\xaa\x00\x04\x00\x1a\x04", 6);
    p.remote_conid = 0x11112222u;
    p.local_conid = 0x33334444u;
    p.recv_ack = 0x0019;
    p.send_seq = 0x0019;

    long n = scs_mscp_srv_build_end_frame(&p, golden_scc_end,
                                          sizeof(golden_scc_end), out,
                                          sizeof(out));
    check(n == 14 + 58 + (long)sizeof(golden_scc_end),
          "the SCC end frame is 14 Ethernet + 58 SCA + 28 body == 100 bytes, "
          "i.e. the captured 86-content class");
    check(u16(out, 14 + 0) == (uint16_t)(58 + sizeof(golden_scc_end) - 2),
          "the SCA content length word is DERIVED from what we emit, never "
          "inherited -- an over-declared length is dropped as a runt in "
          "silence, because nothing in this protocol NAKs");

    check(u16(out, 14 + 46) == 10,
          "the SCS MTYPE is 10, the p. 4-13 application message, matching the "
          "captured end-message class");
    check(memcmp(out + 14 + 58, golden_scc_end, sizeof(golden_scc_end)) == 0,
          "the body rides intact");
    check(memcmp(out + 14 + 2, p.peer_logical, 6) == 0
              && memcmp(out + 14 + 10, p.src_logical, 6) == 0,
          "the identity fields are substituted, not replayed");
    check(scs_mscp_srv_build_end_frame(&p, golden_scc_end,
                                       sizeof(golden_scc_end), out, 10) == -1,
          "a short output buffer is refused");
    check(scs_mscp_srv_build_end_frame(&p, golden_scc_end,
                                       SCS_MSCP_SRV_END_MAX + 1, out,
                                       sizeof(out)) == -1,
          "an over-long body is refused");

    /* THE LENGTH-WORD ARM ABOVE CANNOT, ON ITS OWN, SEE THE BUG IT DESCRIBES.
     * The template's own [0:2] is 84, and 58 + 28 - 2 is also 84, so for an
     * SCC-sized body a builder that inherited the template word would pass --
     * measured, that exact mutation SURVIVED the first version of this file
     * (guardrail 23). A SECOND body length is what makes the word observable.
     * Kept last because it overwrites `out`. */
    {
        uint8_t gus_body[SCS_MSCP_GUS_END_LEN];
        memset(gus_body, 0, sizeof(gus_body));
        gus_body[SCS_MSCP_P_OPCD] =
            SCS_MSCP_OP_GET_UNIT_STATUS | SCS_MSCP_END_BIT;
        long gn = scs_mscp_srv_build_end_frame(&p, gus_body, sizeof(gus_body),
                                               out, sizeof(out));
        check(gn == 14 + 58 + SCS_MSCP_GUS_END_LEN,
              "a GUS end frame is 14 + 58 + 52 == 124 bytes");
        check(u16(out, 14 + 0) == (uint16_t)(58 + SCS_MSCP_GUS_END_LEN - 2),
              "...and its SCA content length word tracks the body rather than "
              "the template's 84, which is the whole claim");
    }
}

/* ===================== SCA block data transfer (vms-4e31) ================= */
/*
 * vms-941 un-deferred. Grounding: docs/design-mscp-direction.md, "Phase D
 * part 1's lab capture" -- a real VAX serving a disk to a real VAX, captured
 * on lab-2 (vaxlab-9, 2026-08-06). These tests build synthetic frames from
 * that documented shape; none of them needs lab access.
 */

/* The 28-byte header, field by field, including the two UNGROUNDED ones,
 * which must round-trip UNCHANGED -- carrying a value through is not the same
 * claim as decoding it. */
static void test_blk_hdr_round_trip(void)
{
    struct scs_mscp_blk_hdr h, out;
    uint8_t raw[SCS_MSCP_BLK_HDR_LEN];

    memset(&h, 0, sizeof(h));
    h.dest_conid = 0x01020304u;
    h.f4 = 0xbeefu; /* UNGROUNDED bit pattern -- no meaning asserted */
    h.f6 = 0xcafeu;
    h.bytes_remaining = 0x11223344u;
    h.src_buf_name = 0x55667788u;
    h.dest_offset = 0x99aabbccu;
    h.dest_buf_name = 0xddeeff00u;
    h.src_offset = 0x13579bdfu;

    scs_mscp_srv_blk_build_hdr(raw, &h);
    check(scs_mscp_srv_blk_parse_hdr(raw, &out) == 0, "the header parses");
    check(out.dest_conid == h.dest_conid && out.f4 == h.f4 && out.f6 == h.f6
              && out.bytes_remaining == h.bytes_remaining
              && out.src_buf_name == h.src_buf_name
              && out.dest_offset == h.dest_offset
              && out.dest_buf_name == h.dest_buf_name
              && out.src_offset == h.src_offset,
          "every field round-trips exactly, including the two UNGROUNDED "
          "ones -- carried through, never reinterpreted");

    scs_mscp_srv_blk_build_hdr(raw, NULL);
    {
        int allzero = 1;
        size_t i;
        for (i = 0; i < sizeof(raw); i++) {
            if (raw[i] != 0) {
                allzero = 0;
            }
        }
        check(allzero, "a NULL header builds all-zero bytes, not a crash");
    }
    check(scs_mscp_srv_blk_parse_hdr(NULL, &out) == -1
              && scs_mscp_srv_blk_parse_hdr(raw, NULL) == -1,
          "NULL arguments are refused");
}

/*
 * TRAP 2: WRITE's two-frame request/response has BYTE-IDENTICAL 28-byte
 * headers -- only data presence tells them apart. Proves both halves of that
 * claim: the headers really are identical, and the parser really does
 * distinguish them anyway.
 */
static void test_trap2_write_request_vs_response(void)
{
    struct scs_mscp_params p;
    struct scs_mscp_blk_hdr h;
    uint8_t req_frame[256], resp_frame[256];
    uint8_t payload[64];
    long req_n, resp_n;
    struct scs_mscp_srv_blk_view req_v, resp_v;
    size_t i;

    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, "\x01\x02\x03\x04\x05\x06", 6);
    memcpy(p.src_mac, "\x07\x08\x09\x0a\x0b\x0c", 6);
    memcpy(p.src_logical, "\xaa\x00\x04\x00\x01\x04", 6);
    memcpy(p.peer_logical, "\xaa\x00\x04\x00\x1a\x04", 6);
    p.remote_conid = 0x99998888u;
    p.local_conid = 0x77776666u;
    p.recv_ack = 5;
    p.send_seq = 5;

    memset(&h, 0, sizeof(h));
    /* Upper 16 bits (content[44:46]) is 0x8765 -- nowhere near the envelope
     * format word 0x0004, so the conformance-fail claim below is real, not
     * coincidental. */
    h.dest_conid = 0x87654321u;
    h.f4 = 13; /* UNGROUNDED, observed shape -- see the header comment */
    h.f6 = 2;
    h.bytes_remaining = sizeof(payload);
    h.src_buf_name = 0x5555u;
    h.dest_offset = 0;
    h.dest_buf_name = 0x6666u;
    h.src_offset = 0;
    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 3 + 1);
    }

    req_n = scs_mscp_srv_build_block_frame(&p, &h, payload, sizeof(payload),
                                           req_frame, sizeof(req_frame));
    resp_n = scs_mscp_srv_build_block_frame(&p, &h, NULL, 0, resp_frame,
                                            sizeof(resp_frame));
    check(req_n > 0 && resp_n > 0, "both frames build");
    check(req_n == resp_n + (long)sizeof(payload),
          "the request frame is exactly the response frame plus the payload");

    check(memcmp(req_frame + 14 + SCS_MSCP_BLK_HDR_OFF,
                 resp_frame + 14 + SCS_MSCP_BLK_HDR_OFF,
                 SCS_MSCP_BLK_HDR_LEN) == 0,
          "THE TRAP, made concrete: WRITE's request and response headers are "
          "byte-identical -- nothing in the header distinguishes them");

    check(scs_mscp_srv_blk_parse_frame(req_frame, (size_t)req_n, &req_v) == 0
              && req_v.data_len == sizeof(payload) && req_v.data != NULL
              && memcmp(req_v.data, payload, sizeof(payload)) == 0,
          "THE FIX: the data-bearing frame parses as carrying its payload");
    check(scs_mscp_srv_blk_parse_frame(resp_frame, (size_t)resp_n, &resp_v)
                  == 0
              && resp_v.data_len == 0 && resp_v.data == NULL,
          "...and the header-only frame parses as carrying NONE -- correctly "
          "told apart from the request despite the identical header. A "
          "parser keyed on the header (e.g. comparing hdr fields) would see "
          "these as the SAME message, which is exactly the bug this data-len "
          "discriminator exists to avoid");

    check(u16(req_frame, 14 + 44) != 0x0004u,
          "content[44:46] is NOT the SCS envelope format word 0x0004 -- a "
          "block-transfer frame deliberately fails that conformance test");
}

/*
 * TRAP 1: READ's final partial chunk piggybacks into the SAME Ethernet frame
 * as the MSCP end message, past what the SCS envelope's declared inner
 * length covers. Builds the combined shape, then proves the receive-side fix
 * (bound by the frame's REAL length) against the receive-side bug (bound by
 * the declared end-message length) side by side.
 */
static void test_trap1_read_end_piggyback(void)
{
    struct scs_mscp_params p;
    uint8_t end_body[SCS_MSCP_READ_END_LEN];
    uint8_t out[512];
    struct scs_mscp_blk_hdr tail_hdr;
    uint8_t tail_data[200];
    long end_only_n, combined_n;
    struct scs_mscp_srv_blk_view v, v_naive;
    size_t i;
    int rc, rc_naive;

    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, "\xaa\xbb\xcc\xdd\xee\xff", 6);
    memcpy(p.src_mac, "\x11\x22\x33\x44\x55\x66", 6);
    memcpy(p.src_logical, "\xaa\x00\x04\x00\x01\x04", 6);
    memcpy(p.peer_logical, "\xaa\x00\x04\x00\x1a\x04", 6);
    p.remote_conid = 0x11112222u;
    p.local_conid = 0x33334444u;
    p.recv_ack = 0x0019;
    p.send_seq = 0x0019;

    memset(end_body, 0, sizeof(end_body));
    end_body[SCS_MSCP_P_OPCD] = SCS_MSCP_OP_READ | SCS_MSCP_END_BIT;
    wle16(end_body, SCS_MSCP_P_STS,
         SCS_MSCP_STATUS(SCS_MSCP_ST_SUCCESS, SCS_MSCP_SUB_NORMAL));
    wle32(end_body, SCS_MSCP_E_BCNT, 712u);

    memset(&tail_hdr, 0, sizeof(tail_hdr));
    tail_hdr.dest_conid = p.local_conid;
    tail_hdr.f4 = 9;
    tail_hdr.f6 = 3;
    tail_hdr.bytes_remaining = (uint32_t)sizeof(tail_data);
    tail_hdr.src_buf_name = 0xaaaau;
    tail_hdr.dest_offset = 512;
    tail_hdr.dest_buf_name = 0x2222u;
    tail_hdr.src_offset = 0;
    for (i = 0; i < sizeof(tail_data); i++) {
        tail_data[i] = (uint8_t)(i * 7 + 3);
    }

    /* The plain end frame -- what a naive caller mistakes for the whole
     * frame. */
    end_only_n = scs_mscp_srv_build_end_frame(&p, end_body, sizeof(end_body),
                                              out, sizeof(out));
    check(end_only_n > 0, "the plain READ end frame builds");

    /* THE COMBINED FRAME -- what a real server actually sends. */
    combined_n = scs_mscp_srv_build_read_end_with_piggyback(
        &p, end_body, sizeof(end_body), &tail_hdr, tail_data,
        sizeof(tail_data), out, sizeof(out));
    check(combined_n == end_only_n + (long)SCS_MSCP_BLK_HDR_LEN
                             + (long)sizeof(tail_data),
          "the piggybacked frame is the end message PLUS the 28-byte block "
          "header PLUS the tail data -- one Ethernet frame, two messages");
    check(end_only_n < combined_n,
          "the declared end-message length UNDER-COUNTS the real frame -- "
          "TRAP 1, made observable");

    /* THE FIX: pass the frame's REAL length. */
    rc = scs_mscp_srv_parse_read_end_trailer(out, (size_t)combined_n,
                                             (size_t)end_only_n, &v);
    check(rc == 0, "the trailer parses");
    check(v.data_len == sizeof(tail_data) && v.data != NULL,
          "...and recovers every byte of the piggybacked chunk");
    check(rc == 0 && v.data != NULL
              && memcmp(v.data, tail_data, sizeof(tail_data)) == 0,
          "...byte for byte");
    check(v.hdr.dest_conid == tail_hdr.dest_conid
              && v.hdr.dest_offset == tail_hdr.dest_offset
              && v.hdr.bytes_remaining == tail_hdr.bytes_remaining,
          "...and the block header's fields round-trip");

    /* THE BUG, reproduced: a receive path that (mis)used the declared
     * end-message length as the frame's real size sees no trailer at all --
     * this IS that path, modelled by passing end_only_n as frame_len. A
     * regression that goes back to trusting a declared length rather than
     * the frame's real size makes this assertion fail. */
    rc_naive = scs_mscp_srv_parse_read_end_trailer(
        out, (size_t)end_only_n, (size_t)end_only_n, &v_naive);
    check(rc_naive == 0 && v_naive.data_len == 0,
          "...and TRUSTING the declared length as the frame's real size "
          "finds ZERO trailer bytes -- the exact silent data loss TRAP 1 "
          "warns about");
}

/*
 * struct scs_mscp_srv_blk_sink installed as the transfer hook: a READ
 * through scs_mscp_srv_handle() really moves real backing-store bytes
 * through real SCA block-transfer frames, end to end -- this is what "wire
 * the real path in behind the kill switch" means.
 */
static void test_blk_sink_read_end_to_end(void)
{
    struct scs_mscp_srv srv;
    uint8_t body[SCS_MSCP_BODY_LEN];
    uint8_t end[SCS_MSCP_SRV_END_MAX];
    struct scs_mscp_view v;
    struct scs_mscp_params p;
    struct scs_mscp_srv_blk_sink sink;
    uint8_t sink_out[4096];
    char path[128];
    const unsigned NBLK = 16;
    const size_t FRAME_LEN = 14u + SCS_MSCP_BLK_HDR_OFF + SCS_MSCP_BLK_HDR_LEN
                             + SCS_MSCP_BLOCK_SIZE;
    struct scs_mscp_srv_unit *u;
    struct scs_mscp_srv_blk_view v0, v1;
    uint8_t expect0[SCS_MSCP_BLOCK_SIZE], expect1[SCS_MSCP_BLOCK_SIZE];
    long n;
    int fd = make_image(NBLK, path, sizeof(path));

    check(fd >= 0, "test fixture: the raw block image is created");
    if (fd < 0) {
        return;
    }

    scs_mscp_srv_init(&srv, GOLDEN_CTLR_ID, GOLDEN_CTLR_TIMEOUT);
    bring_controller_online(&srv, 11u);
    scs_mscp_srv_attach_fd(&srv, 0, fd, NBLK, 0x8888ULL, 0x2452, 0x1);
    {
        struct scs_mscp_view ov;
        uint8_t ob[SCS_MSCP_BODY_LEN], oe[SCS_MSCP_SRV_END_MAX];
        make_command(ob, sizeof(ob), &ov, 0x9f0u, 0, SCS_MSCP_OP_ONLINE, 0);
        scs_mscp_srv_handle(&srv, 11u, &ov, ob, sizeof(ob), oe, sizeof(oe));
    }

    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, "\xaa\xbb\xcc\xdd\xee\xff", 6);
    memcpy(p.src_mac, "\x11\x22\x33\x44\x55\x66", 6);
    memcpy(p.src_logical, "\xaa\x00\x04\x00\x01\x04", 6);
    memcpy(p.peer_logical, "\xaa\x00\x04\x00\x1a\x04", 6);
    p.remote_conid = 0x11112222u;
    p.local_conid = 0x33334444u;
    p.recv_ack = 1;
    p.send_seq = 1;

    scs_mscp_srv_blk_sink_init(&sink, &p, 1024u, sink_out, sizeof(sink_out));
    sink.conn_const = 9;
    sink.xfer_const = 4;
    sink.src_buf_name = 0xabcdu;
    scs_mscp_srv_set_xfer(&srv, scs_mscp_srv_blk_sink_xfer, &sink);

    make_command(body, sizeof(body), &v, 0x9f1u, 0, SCS_MSCP_OP_READ, 0);
    wle32(body, SCS_MSCP_P_BCNT, 1024u);
    wle32(body, SCS_MSCP_P_LBN, 2u);
    /* the host buffer descriptor: {offset, SCS buffer NAME, SCS connection ID} */
    wle32(body, SCS_MSCP_P_BUFF + 0, 0x1000u);
    wle32(body, SCS_MSCP_P_BUFF + 4, 0x2222u);
    wle32(body, SCS_MSCP_P_BUFF + 8, 0x87653333u);

    n = scs_mscp_srv_handle(&srv, 11u, &v, body, sizeof(body), end,
                            sizeof(end));
    check(n > 0, "a READ with the sink installed answers");
    check(scs_mscp_status_major(u16(end, SCS_MSCP_P_STS)) == SCS_MSCP_ST_SUCCESS,
          "...and SUCCEEDS -- the always-refuse kill switch, given something "
          "real to gate, now answers a real mount's READ");
    check(u32(end, SCS_MSCP_E_BCNT) == 1024u, "...and reports all 1024 bytes");
    check(sink.frames_built == 2, "two 512-byte block-transfer frames were built");
    check(sink.used == FRAME_LEN * 2,
          "...and the sink buffer holds exactly two frames, nothing more");

    memset(&v0, 0, sizeof(v0));
    memset(&v1, 0, sizeof(v1));
    check(scs_mscp_srv_blk_parse_frame(sink_out, FRAME_LEN, &v0) == 0,
          "the first frame parses");
    check(scs_mscp_srv_blk_parse_frame(sink_out + FRAME_LEN, FRAME_LEN, &v1)
              == 0,
          "the second frame parses");

    u = scs_mscp_srv_find_unit(&srv, 0);
    scs_mscp_srv_read_blocks(u, 2, 1, expect0, sizeof(expect0));
    scs_mscp_srv_read_blocks(u, 3, 1, expect1, sizeof(expect1));
    check(v0.data_len == SCS_MSCP_BLOCK_SIZE && v0.data != NULL
              && memcmp(v0.data, expect0, SCS_MSCP_BLOCK_SIZE) == 0,
          "the first frame carries block 2's REAL content from the backing "
          "store -- not a fake success");
    check(v1.data_len == SCS_MSCP_BLOCK_SIZE && v1.data != NULL
              && memcmp(v1.data, expect1, SCS_MSCP_BLOCK_SIZE) == 0,
          "...and the second frame carries block 3's");

    check(v0.hdr.dest_conid == 0x87653333u && v1.hdr.dest_conid == 0x87653333u,
          "every frame addresses the connection ID from the host's buffer "
          "descriptor");
    check(v0.hdr.dest_buf_name == 0x2222u && v1.hdr.dest_buf_name == 0x2222u,
          "...and the destination buffer NAME -- the correlation key");
    check(v0.hdr.dest_offset == 0x1000u,
          "the first frame's destination offset is the buffer descriptor's "
          "base offset");
    check(v1.hdr.dest_offset == 0x1000u + SCS_MSCP_BLOCK_SIZE,
          "...and the second frame's offset advances by exactly one block");
    check(v0.hdr.bytes_remaining == 1024u,
          "the first frame's down-counting field includes ITS OWN data -- "
          "1024 remaining before either frame's bytes have crossed");
    check(v1.hdr.bytes_remaining == SCS_MSCP_BLOCK_SIZE,
          "the LAST frame's bytes-remaining equals exactly its own data "
          "length -- the down-count landing on zero at transfer's end");
    check(v0.hdr.f4 == 9 && v0.hdr.f6 == 4 && v1.hdr.f4 == 9 && v1.hdr.f6 == 4,
          "the two UNGROUNDED fields are carried through unchanged on every "
          "frame, never reinterpreted");
    check(u16(sink_out, 14 + 44) != 0x0004u,
          "and the frame fails SCS envelope conformance, as designed");

    close(fd);
    unlink(path);
}

/*
 * ONE WRITE, END TO END: the receiving half of block data transfer (what a
 * real WRITE's REQDAT pull would need) really moves bytes into the backing
 * store. This does NOT change scs_mscp_srv_handle()'s WRITE opcode policy --
 * v1 stays Write Protected (design decision (2), test_write_is_refused_honestly
 * above) -- it proves the transfer primitive a read-write v2 would build on.
 */
static void test_write_block_transfer_end_to_end(void)
{
    struct scs_mscp_params p;
    struct scs_mscp_blk_hdr h;
    uint8_t req_frame[700], resp_frame[700];
    uint8_t payload[SCS_MSCP_BLOCK_SIZE];
    uint8_t readback[SCS_MSCP_BLOCK_SIZE];
    long req_n, resp_n;
    struct scs_mscp_srv_blk_view req_v;
    struct scs_mscp_srv srv;
    struct scs_mscp_srv_unit *u;
    char path[128];
    const unsigned NBLK = 8;
    size_t i;
    int fd = make_image(NBLK, path, sizeof(path));

    check(fd >= 0, "test fixture: the raw block image is created");
    if (fd < 0) {
        return;
    }

    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, "\x01\x02\x03\x04\x05\x06", 6);
    memcpy(p.src_mac, "\x07\x08\x09\x0a\x0b\x0c", 6);
    memcpy(p.src_logical, "\xaa\x00\x04\x00\x01\x04", 6);
    memcpy(p.peer_logical, "\xaa\x00\x04\x00\x1a\x04", 6);
    p.remote_conid = 0x44445555u;
    p.local_conid = 0x66667777u;
    p.recv_ack = 2;
    p.send_seq = 2;

    memset(&h, 0, sizeof(h));
    h.dest_conid = 0xabcdef01u;
    h.f4 = 13; /* UNGROUNDED, observed shape -- see the header comment */
    h.f6 = 1;
    h.bytes_remaining = (uint32_t)sizeof(payload);
    h.src_buf_name = 0x9999u;
    h.dest_offset = 0;
    h.dest_buf_name = 0x8888u;
    h.src_offset = 0;
    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)('W' + (i % 7));
    }

    /* TRAP 2 shape again: request carries the data, response is header-only. */
    req_n = scs_mscp_srv_build_block_frame(&p, &h, payload, sizeof(payload),
                                           req_frame, sizeof(req_frame));
    resp_n = scs_mscp_srv_build_block_frame(&p, &h, NULL, 0, resp_frame,
                                            sizeof(resp_frame));
    check(req_n > 0 && resp_n > 0, "both frames of the WRITE exchange build");
    check(scs_mscp_srv_blk_parse_frame(req_frame, (size_t)req_n, &req_v) == 0
              && req_v.data_len == sizeof(payload),
          "the request half is recovered with its full payload");

    scs_mscp_srv_init(&srv, GOLDEN_CTLR_ID, GOLDEN_CTLR_TIMEOUT);
    check(scs_mscp_srv_attach_fd(&srv, 0, fd, NBLK, 0x7777ULL, 0x2452, 0x1)
              == 0,
          "the unit attaches");
    u = scs_mscp_srv_find_unit(&srv, 0);
    check(scs_mscp_srv_write_blocks(u, 4, req_v.data, 1)
              == (long)SCS_MSCP_BLOCK_SIZE,
          "the block-transfer request's data writes to the backing store");
    check(scs_mscp_srv_read_blocks(u, 4, 1, readback, sizeof(readback))
              == (long)SCS_MSCP_BLOCK_SIZE
              && memcmp(readback, payload, sizeof(payload)) == 0,
          "...and reading it back gets exactly what the request carried, "
          "byte for byte");
    check(scs_mscp_srv_write_blocks(u, NBLK - 1, req_v.data, 2) == -1,
          "a write running past the end of the volume is refused (sec 5.3), "
          "same bound as a read");

    close(fd);
    unlink(path);
}

int main(void)
{
    test_scc_end_byte_exact();
    test_scc_rejects_nonzero_version();
    test_commands_require_controller_online();
    test_gus_walk_and_fields();
    test_online();
    test_backing_store_and_read();
    test_write_is_refused_honestly();
    test_hard_refusals();
    test_attach_refusals();
    test_end_frame();
    test_blk_hdr_round_trip();
    test_trap2_write_request_vs_response();
    test_trap1_read_end_piggyback();
    test_blk_sink_read_end_to_end();
    test_write_block_transfer_end_to_end();

    printf("test_scs_mscp_srv: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
