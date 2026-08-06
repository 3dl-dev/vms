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
    check(n == SCS_MSCP_GUS_END_LEN, "a GUS end message is 48 bytes (Table A-7)");
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

    /* --- READ WITH NO TRANSFER HOOK: design decision (4). This is the INV-6
     * boundary and the single most important assertion in this file. --- */
    make_command(body, sizeof(body), &v, 0x200u, 0, SCS_MSCP_OP_READ, 0);
    body[12] = (uint8_t)(SCS_MSCP_BLOCK_SIZE & 0xff); /* P.BCNT = 512 */
    body[13] = (uint8_t)(SCS_MSCP_BLOCK_SIZE >> 8);
    body[28] = 3; /* P.LBN = 3 */
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
    body[12] = 0x00;
    body[13] = 0x04; /* P.BCNT = 1024 == two blocks */
    body[28] = 7;    /* P.LBN = 7 */
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
    body[12] = 0x00;
    body[13] = 0x02; /* 512 */
    n = scs_mscp_srv_handle(&srv, 2u, &v, body, sizeof(body), end, sizeof(end));
    check(scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
              == SCS_MSCP_ST_HOST_BUF_ERR,
          "a FAILED block transfer is Host Buffer Access Error (sec 6.14)");

    /* Invalid byte counts and LBNs are refused with the offset-coded sub-code
     * scheme of Table B-2, not serviced. */
    rec.fail = 0;
    make_command(body, sizeof(body), &v, 0x203u, 0, SCS_MSCP_OP_READ, 0);
    body[12] = 0x01; /* 513 -- not a whole number of blocks */
    body[13] = 0x02;
    n = scs_mscp_srv_handle(&srv, 2u, &v, body, sizeof(body), end, sizeof(end));
    check(scs_mscp_status_major(u16(end, SCS_MSCP_P_STS))
              == SCS_MSCP_ST_INVALID_CMD,
          "a byte count that is not a whole number of blocks is Invalid "
          "Command (sec 5.3)");

    make_command(body, sizeof(body), &v, 0x204u, 0, SCS_MSCP_OP_READ, 0);
    body[12] = 0x00;
    body[13] = 0x02;
    body[28] = (uint8_t)(NBLK + 10); /* past the end */
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
    check(n > 0, "a WRITE is answered, not dropped");
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
              "a GUS end frame is 14 + 58 + 48 == 120 bytes");
        check(u16(out, 14 + 0) == (uint16_t)(58 + SCS_MSCP_GUS_END_LEN - 2),
              "...and its SCA content length word is 104, NOT the template's "
              "84 -- the word tracks the body, which is the whole claim");
    }
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

    printf("test_scs_mscp_srv: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
