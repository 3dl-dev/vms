/*
 * scs_mscp_srv.c - MSCP DISK SERVER responder (vms-291, MSCP epic Phase D).
 * See scs_mscp_srv.h for provenance, the golden end-message grounding, and the
 * four design decisions this file implements.
 *
 * WHAT IS GROUNDED AND WHAT IS BOOK-ONLY -- read before changing a byte:
 *
 *   SCC END (0x84)  GROUNDED. 954 captured frames, 86-content, MTYPE 10.
 *                   test_scs_mscp_srv.c rebuilds a real one byte-for-byte.
 *   GUS END (0x83)  GROUNDED. 18855 captured frames, 110-content, MTYPE 10.
 *                   Byte-exact test likewise.
 *   ONLINE END      BOOK-ONLY (Table A-7). No capture in our corpus contains
 *   (0x89)          one -- the captured joiner never mounts. Laid out from the
 *                   published offsets and labelled as unproven.
 *   READ END        BOOK-ONLY, and its DATA PATH IS NOT IMPLEMENTED AT ALL.
 *   (0xa1)          See scs_mscp_srv_read() -- it refuses rather than faking.
 */
#define _POSIX_C_SOURCE 200809L

#include "scs_mscp_srv.h"

#include <string.h>
#include <unistd.h>

#include "scs_env.h"

/* ============================ little helpers ============================= */

static void put_le16(uint8_t *d, uint16_t v)
{
    d[0] = (uint8_t)(v & 0xffu);
    d[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put_le32(uint8_t *d, uint32_t v)
{
    put_le16(d, (uint16_t)(v & 0xffffu));
    put_le16(d + 2, (uint16_t)((v >> 16) & 0xffffu));
}

static void put_le64(uint8_t *d, uint64_t v)
{
    put_le32(d, (uint32_t)(v & 0xffffffffu));
    put_le32(d + 4, (uint32_t)((v >> 32) & 0xffffffffu));
}

static uint32_t get_le32(const uint8_t *s)
{
    return (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16)
           | ((uint32_t)s[3] << 24);
}

/* ========================= server / unit lifecycle ======================== */

int scs_mscp_srv_init(struct scs_mscp_srv *srv, uint64_t ctlr_id,
                      uint16_t ctlr_timeout)
{
    if (srv == NULL) {
        return -1;
    }
    memset(srv, 0, sizeof(*srv));
    for (unsigned i = 0; i < SCS_MSCP_SRV_MAX_UNITS; i++) {
        srv->units[i].fd = -1;
    }
    srv->ctlr_id = ctlr_id;
    srv->ctlr_timeout = ctlr_timeout;
    return 0;
}

int scs_mscp_srv_set_ctlr_profile(struct scs_mscp_srv *srv,
                                  uint16_t ctlr_flags_reported,
                                  uint16_t ctlr_version_word)
{
    if (srv == NULL) {
        return -1;
    }
    srv->ctlr_flags_reported = ctlr_flags_reported;
    srv->ctlr_version_word = ctlr_version_word;
    return 0;
}

int scs_mscp_srv_attach_fd(struct scs_mscp_srv *srv, uint16_t unit, int fd,
                           uint32_t unit_size, uint64_t unit_id,
                           uint32_t media_id, uint32_t volume_ser)
{
    if (srv == NULL || fd < 0 || unit_size == 0) {
        return -1;
    }
    /* sec 6.12: a zero unit identifier means "virtually no characteristics are
     * valid". A unit we are actually serving must not say that about itself. */
    if (unit_id == 0) {
        return -1;
    }
    if (scs_mscp_srv_find_unit(srv, unit) != NULL) {
        return -1; /* already served -- refuse rather than silently replace */
    }
    for (unsigned i = 0; i < SCS_MSCP_SRV_MAX_UNITS; i++) {
        struct scs_mscp_srv_unit *u = &srv->units[i];
        if (u->in_use) {
            continue;
        }
        memset(u, 0, sizeof(*u));
        u->in_use = 1;
        u->unit = unit;
        u->fd = fd;
        u->unit_size = unit_size;
        u->unit_id = unit_id;
        u->media_id = media_id;
        u->volume_ser = volume_ser;
        u->online = 0;
        /* Design decision (2): v1 serves read-only and SAYS SO. UF.WPS is not
         * advisory here -- scs_mscp_srv_handle() refuses WRITE with the
         * matching Table B-2 status, so the flag and the behaviour agree. */
        u->read_only = 1;
        u->unit_flags = SCS_MSCP_UF_WRITE_PROT_SW;
        return 0;
    }
    return -1;
}

int scs_mscp_srv_set_xfer(struct scs_mscp_srv *srv, scs_mscp_srv_xfer_fn fn,
                          void *ctx)
{
    if (srv == NULL) {
        return -1;
    }
    srv->xfer = fn;
    srv->xfer_ctx = ctx;
    return 0;
}

struct scs_mscp_srv_unit *scs_mscp_srv_find_unit(struct scs_mscp_srv *srv,
                                                 uint16_t unit)
{
    if (srv == NULL) {
        return NULL;
    }
    for (unsigned i = 0; i < SCS_MSCP_SRV_MAX_UNITS; i++) {
        if (srv->units[i].in_use && srv->units[i].unit == unit) {
            return &srv->units[i];
        }
    }
    return NULL;
}

struct scs_mscp_srv_unit *scs_mscp_srv_next_unit(struct scs_mscp_srv *srv,
                                                 uint16_t unit)
{
    struct scs_mscp_srv_unit *best = NULL;
    if (srv == NULL) {
        return NULL;
    }
    /* MD.NXU (sec 6.12): "the next known unit >= the specified unit number",
     * ascending. The table is unordered, so pick the minimum satisfying it. */
    for (unsigned i = 0; i < SCS_MSCP_SRV_MAX_UNITS; i++) {
        struct scs_mscp_srv_unit *u = &srv->units[i];
        if (!u->in_use || u->unit < unit) {
            continue;
        }
        if (best == NULL || u->unit < best->unit) {
            best = u;
        }
    }
    return best;
}

struct scs_mscp_srv_host *scs_mscp_srv_host_for(struct scs_mscp_srv *srv,
                                                uint32_t conid)
{
    if (srv == NULL) {
        return NULL;
    }
    for (unsigned i = 0; i < SCS_MSCP_SRV_MAX_HOSTS; i++) {
        if (srv->hosts[i].in_use && srv->hosts[i].conid == conid) {
            return &srv->hosts[i];
        }
    }
    for (unsigned i = 0; i < SCS_MSCP_SRV_MAX_HOSTS; i++) {
        if (!srv->hosts[i].in_use) {
            memset(&srv->hosts[i], 0, sizeof(srv->hosts[i]));
            srv->hosts[i].in_use = 1;
            srv->hosts[i].conid = conid;
            /* sec 6.16: 60 seconds is the per-host default until a SET
             * CONTROLLER CHARACTERISTICS says otherwise. */
            srv->hosts[i].host_timeout = 60;
            return &srv->hosts[i];
        }
    }
    return NULL;
}

/* ============================= backing store ============================= */

long scs_mscp_srv_read_blocks(struct scs_mscp_srv_unit *u, uint32_t lbn,
                              uint32_t nblocks, uint8_t *out, size_t out_len)
{
    if (u == NULL || out == NULL || !u->in_use || u->fd < 0 || nblocks == 0) {
        return -1;
    }
    /* sec 5.3: the transfer must not run past the end of the volume. Checked
     * without overflowing: lbn + nblocks is computed in 64 bits. */
    if ((uint64_t)lbn + (uint64_t)nblocks > (uint64_t)u->unit_size) {
        return -1;
    }
    size_t want = (size_t)nblocks * (size_t)SCS_MSCP_BLOCK_SIZE;
    if (out_len < want) {
        return -1;
    }
    off_t off = (off_t)lbn * (off_t)SCS_MSCP_BLOCK_SIZE;
    size_t done = 0;
    while (done < want) {
        ssize_t n = pread(u->fd, out + done, want - done, off + (off_t)done);
        if (n <= 0) {
            /* A short read is a real failure, not a partial success: the class
             * driver asked for whole blocks. */
            return -1;
        }
        done += (size_t)n;
    }
    return (long)done;
}

/* ========================= end-message construction ======================= */

/*
 * Lay the sec 5.5 generic end-message header. `base_opcode` is the COMMAND
 * being answered; OP.END is added here so no caller can forget it and emit a
 * command-shaped answer.
 */
static void end_header(uint8_t *end, size_t end_len, uint32_t cmd_ref,
                       uint16_t unit, uint8_t base_opcode, uint8_t flags,
                       uint16_t status)
{
    /* sec 5.2: an MSCP server "must supply 0 in reserved fields of messages it
     * sends". Zero first, then write only defined fields. */
    memset(end, 0, end_len);
    put_le32(end + SCS_MSCP_P_CRF, cmd_ref);
    put_le16(end + SCS_MSCP_P_UNIT, unit);
    end[SCS_MSCP_P_OPCD] = (uint8_t)(base_opcode | SCS_MSCP_END_BIT);
    end[SCS_MSCP_P_FLGS] = flags;
    put_le16(end + SCS_MSCP_P_STS, status);
}

/*
 * The Invalid Command end message. Table A-1's note is explicit and easy to get
 * wrong: it "contains just OP.END" -- the endcode is 0x80, NOT the offending
 * opcode with OP.END added, because the controller is saying it did not
 * recognise the command at all.
 */
static long build_invalid_command(uint8_t *end, size_t end_len,
                                  uint32_t cmd_ref, uint16_t unit,
                                  uint16_t status)
{
    if (end_len < SCS_MSCP_HDR_LEN) {
        return -1;
    }
    end_header(end, SCS_MSCP_HDR_LEN, cmd_ref, unit, 0u, 0u, status);
    return (long)SCS_MSCP_HDR_LEN;
}

/* -------- SET CONTROLLER CHARACTERISTICS end message (Table A-7) --------- */

static long build_scc_end(struct scs_mscp_srv *srv,
                          struct scs_mscp_srv_host *host,
                          const struct scs_mscp_view *cmd, const uint8_t *body,
                          size_t body_len, uint8_t *end, size_t end_len)
{
    if (end_len < SCS_MSCP_SCC_END_LEN) {
        return -1;
    }
    /* sec 6.16: "the host must supply 0" in P.VRSN; a non-zero MSCP version is
     * answered Invalid Command. This is a real refusal with a real code, and
     * the test drives it. */
    if (body_len >= SCS_MSCP_P_VRSN + 2) {
        uint16_t version = (uint16_t)((uint16_t)body[SCS_MSCP_P_VRSN]
                                      | ((uint16_t)body[SCS_MSCP_P_VRSN + 1] << 8));
        if (version != 0) {
            return build_invalid_command(
                end, end_len, cmd->cmd_ref, cmd->unit,
                SCS_MSCP_STATUS(SCS_MSCP_ST_INVALID_CMD, SCS_MSCP_P_VRSN));
        }
    }

    /* Adopt the host-settable controller state (sec 5.8: per-class-driver, all
     * clear at Controller-Online until a SCC sets them). */
    if (host != NULL && body_len >= SCS_MSCP_P_HTMO + 2) {
        host->ctlr_flags = (uint16_t)((uint16_t)body[SCS_MSCP_P_CNTF]
                                      | ((uint16_t)body[SCS_MSCP_P_CNTF + 1] << 8));
        host->host_timeout = (uint16_t)((uint16_t)body[SCS_MSCP_P_HTMO]
                                        | ((uint16_t)body[SCS_MSCP_P_HTMO + 1] << 8));
        /* sec 3.4: completing the first SET CONTROLLER CHARACTERISTICS is what
         * takes this class driver past the initial credit regime. It is the
         * gate every other command is tested against below. */
        host->ctlr_online = 1;
    }

    /* sec 6.16: SET CONTROLLER CHARACTERISTICS has exactly one status --
     * Success (Normal). There is no failure path here other than the version
     * check above. */
    end_header(end, SCS_MSCP_SCC_END_LEN, cmd->cmd_ref, cmd->unit,
               SCS_MSCP_OP_SET_CTLR_CHAR, 0u,
               SCS_MSCP_STATUS(SCS_MSCP_ST_SUCCESS, SCS_MSCP_SUB_NORMAL));
    put_le16(end + SCS_MSCP_E_VRSN, 0u); /* the server echoes version 0 */
    /* NOT an echo of host->ctlr_flags. Measured over 954 of 954 captured SCC
     * end messages, the VMS server returns a CONSTANT 0xa004 here regardless of
     * what the class driver asked for -- it is not a Table A-4 flag word at
     * all. Echoing the host's request would have looked reasonable and been
     * wrong; see the commentary in scs_mscp_srv.h. The host's request is still
     * recorded on the host record, because sec 5.8 makes it per-class-driver
     * state the server must honour for attention/error-log messages. */
    put_le16(end + SCS_MSCP_E_CNTF, srv->ctlr_flags_reported);
    put_le16(end + SCS_MSCP_E_CTMO, srv->ctlr_timeout);
    put_le16(end + SCS_MSCP_E_RSVD18, srv->ctlr_version_word);
    put_le64(end + SCS_MSCP_E_CNTI, srv->ctlr_id);
    return (long)SCS_MSCP_SCC_END_LEN;
}

/* ------------- GET UNIT STATUS end message (Table A-7, sec 6.12) --------- */

static void fill_unit_characteristics(const struct scs_mscp_srv_unit *u,
                                      uint8_t *end)
{
    put_le16(end + SCS_MSCP_E_MLUN, u->multi_unit);
    put_le16(end + SCS_MSCP_E_UNFL, u->unit_flags);
    put_le64(end + SCS_MSCP_E_UNTI, u->unit_id);
    put_le32(end + SCS_MSCP_E_MEDI, u->media_id);
}

static long build_gus_end(struct scs_mscp_srv *srv,
                          const struct scs_mscp_view *cmd, uint8_t *end,
                          size_t end_len)
{
    if (end_len < SCS_MSCP_GUS_END_LEN) {
        return -1;
    }

    /* MD.NXU (Table A-2) turns the lookup into the enumeration a class driver
     * uses to discover what we serve. Without it, exact match only. */
    struct scs_mscp_srv_unit *u =
        (cmd->modifiers & SCS_MSCP_MOD_NEXT_UNIT)
            ? scs_mscp_srv_next_unit(srv, cmd->unit)
            : scs_mscp_srv_find_unit(srv, cmd->unit);

    if (u == NULL) {
        /* sec 6.12: Unit-Offline, sub-code 0 -- "unit unknown or online to
         * another controller". This is the terminator that ends an MD.NXU walk,
         * and it is exactly what OVMX's own CLIENT walk looks for. The unit
         * word echoes what was asked for; every characteristic stays zero,
         * which sec 6.12 makes meaningful: a zero unit identifier says "no
         * characteristics here are valid". */
        end_header(end, SCS_MSCP_GUS_END_LEN, cmd->cmd_ref, cmd->unit,
                   SCS_MSCP_OP_GET_UNIT_STATUS, 0u,
                   SCS_MSCP_STATUS(SCS_MSCP_ST_OFFLINE,
                                   SCS_MSCP_SUB_OFL_UNKNOWN));
        put_le16(end + SCS_MSCP_E_GUS_TAIL, SCS_MSCP_E_GUS_TAIL_OBSERVED);
        return (long)SCS_MSCP_GUS_END_LEN;
    }

    /* sec 6.12: Success implies Unit-Online to this class driver. A unit we
     * serve but which no ONLINE has claimed yet is Unit-Available -- which is
     * precisely the status the captured real-VAX GUS walk returns for its own
     * unmounted units, and what lets a class driver see a unit it may then
     * bring online. */
    uint16_t status = u->online
                          ? SCS_MSCP_STATUS(SCS_MSCP_ST_SUCCESS,
                                            SCS_MSCP_SUB_NORMAL)
                          : SCS_MSCP_STATUS(SCS_MSCP_ST_AVAILABLE,
                                            SCS_MSCP_SUB_NORMAL);

    end_header(end, SCS_MSCP_GUS_END_LEN, cmd->cmd_ref, u->unit,
               SCS_MSCP_OP_GET_UNIT_STATUS, 0u, status);
    fill_unit_characteristics(u, end);
    put_le16(end + SCS_MSCP_E_SHUN, u->unit); /* sec 6.12: shadow unit == unit */
    put_le16(end + SCS_MSCP_E_TRCK, u->track_size);
    put_le16(end + SCS_MSCP_E_GRP, u->group_size);
    put_le16(end + SCS_MSCP_E_CYL, u->cyl_size);
    put_le16(end + SCS_MSCP_E_RCTS, u->rct_size);
    end[SCS_MSCP_E_RBNS] = u->rbns;
    end[SCS_MSCP_E_RCTC] = u->rct_copies;
    /* The four bytes past Table A-7. [48:50] is copied from the observation --
     * a real server always writes it; [50:52] is left zero because a real
     * server demonstrably leaves it as stale garbage. See the commentary on
     * SCS_MSCP_E_GUS_TAIL_OBSERVED. */
    put_le16(end + SCS_MSCP_E_GUS_TAIL, SCS_MSCP_E_GUS_TAIL_OBSERVED);
    return (long)SCS_MSCP_GUS_END_LEN;
}

/* ------------------- ONLINE end message (Table A-7, sec 6.13) ------------ */

static long build_online_end(struct scs_mscp_srv *srv,
                             const struct scs_mscp_view *cmd,
                             const uint8_t *body, size_t body_len, uint8_t *end,
                             size_t end_len)
{
    if (end_len < SCS_MSCP_ONLINE_END_LEN) {
        return -1;
    }
    struct scs_mscp_srv_unit *u = scs_mscp_srv_find_unit(srv, cmd->unit);
    if (u == NULL) {
        /* sec 6.13 lists Unit-Offline among ONLINE's status codes. */
        end_header(end, SCS_MSCP_ONLINE_END_LEN, cmd->cmd_ref, cmd->unit,
                   SCS_MSCP_OP_ONLINE, 0u,
                   SCS_MSCP_STATUS(SCS_MSCP_ST_OFFLINE,
                                   SCS_MSCP_SUB_OFL_UNKNOWN));
        return (long)SCS_MSCP_ONLINE_END_LEN;
    }

    /* sec 6.13: "Already Online" is a SUCCESS sub-code, not an error -- the
     * unit was already Unit-Online to this class driver and its state is
     * unaltered. Getting this wrong (reporting an error) would make a
     * re-MOUNT fail for no reason. */
    uint16_t status = u->online
                          ? SCS_MSCP_STATUS(SCS_MSCP_ST_SUCCESS,
                                            SCS_MSCP_SUB_ALREADY_ONLINE)
                          : SCS_MSCP_STATUS(SCS_MSCP_ST_SUCCESS,
                                            SCS_MSCP_SUB_NORMAL);
    u->online = 1;

    end_header(end, SCS_MSCP_ONLINE_END_LEN, cmd->cmd_ref, u->unit,
               SCS_MSCP_OP_ONLINE, 0u, status);
    fill_unit_characteristics(u, end);
    /* MEASURED (vms-291 serving capture): P.UNFL bit 15 is HOST-ORIGINATED.
     * The class driver's ONLINE COMMAND carries 0x8000 at Table A-6 offset 14
     * and the server ECHOES it into the end message -- so the bit is not
     * something a controller invents, and a server that ignored the host's
     * word would answer with flags the host never asked for. sec 6.13 says as
     * much in prose ("sets host-settable characteristics"); the capture is what
     * makes it actionable. The unit's own non-host-settable flags (UF.WPS,
     * from design decision (2)) are OR-ed in and cannot be cleared by a host. */
    if (body != NULL && body_len >= SCS_MSCP_E_UNFL + 2) {
        uint16_t host_flags =
            (uint16_t)((uint16_t)body[SCS_MSCP_E_UNFL]
                       | ((uint16_t)body[SCS_MSCP_E_UNFL + 1] << 8));
        put_le16(end + SCS_MSCP_E_UNFL, (uint16_t)(host_flags | u->unit_flags));
    }
    /* P.UNSZ and P.VSER are what distinguish the ONLINE end message from the
     * GET UNIT STATUS one -- and P.UNSZ is the field a mounting VAX uses to
     * size the volume. */
    put_le32(end + SCS_MSCP_E_UNSZ, u->unit_size);
    put_le32(end + SCS_MSCP_E_VSER, u->volume_ser);
    return (long)SCS_MSCP_ONLINE_END_LEN;
}

/* --------------------- READ / WRITE (sec 5.3, 6.14, 6.18) --------------- */

static long build_transfer_end(uint8_t *end, size_t end_len, uint32_t cmd_ref,
                               uint16_t unit, uint8_t base_opcode,
                               uint16_t status, uint32_t byte_count)
{
    /* sec 5.5: a transfer end message carries the byte count SUCCESSFULLY
     * TRANSFERRED UP TO THE FIRST ERROR -- so a failure reports 0 here, and
     * reporting the requested count on a failure would be a lie the class
     * driver would believe. */
    /* READ and WRITE end messages are NOT the same length on a real server:
     * READ declares 32 (Table A-7's generic end) and WRITE declares 36. The
     * extra four bytes on WRITE are undecoded and stay zero. */
    size_t need = (base_opcode == SCS_MSCP_OP_WRITE) ? SCS_MSCP_WRITE_END_LEN
                                                     : SCS_MSCP_READ_END_LEN;
    if (end_len < need) {
        return -1;
    }
    end_header(end, need, cmd_ref, unit, base_opcode, 0u, status);
    put_le32(end + SCS_MSCP_E_BCNT, byte_count);
    return (long)need;
}

static long handle_read(struct scs_mscp_srv *srv,
                        const struct scs_mscp_view *cmd, const uint8_t *body,
                        size_t body_len, uint8_t *end, size_t end_len)
{
    /* The sec 5.3 transfer command layout: byte count at P.BCNT, the 12-byte
     * host buffer descriptor at P.BUFF, the LBN at P.LBN -- all named in
     * scs_mscp.h, which is the one place in the tree these offsets are
     * written. */
    if (body_len < SCS_MSCP_P_LBN + 4) {
        return build_invalid_command(
            end, end_len, cmd->cmd_ref, cmd->unit,
            SCS_MSCP_STATUS(SCS_MSCP_ST_INVALID_CMD, 0u));
    }
    uint32_t byte_count = get_le32(body + SCS_MSCP_P_BCNT);
    uint32_t lbn = get_le32(body + SCS_MSCP_P_LBN);

    struct scs_mscp_srv_unit *u = scs_mscp_srv_find_unit(srv, cmd->unit);
    if (u == NULL) {
        return build_transfer_end(end, end_len, cmd->cmd_ref, cmd->unit,
                                  SCS_MSCP_OP_READ,
                                  SCS_MSCP_STATUS(SCS_MSCP_ST_OFFLINE,
                                                  SCS_MSCP_SUB_OFL_UNKNOWN),
                                  0u);
    }
    /* sec 6.14 lists Unit-Available among READ's statuses: a unit that has not
     * been brought Unit-Online cannot transfer. */
    if (!u->online) {
        return build_transfer_end(end, end_len, cmd->cmd_ref, u->unit,
                                  SCS_MSCP_OP_READ,
                                  SCS_MSCP_STATUS(SCS_MSCP_ST_AVAILABLE, 0u),
                                  0u);
    }
    /* sec 5.3: the byte count must be a whole number of blocks and must fit
     * inside the volume. Table B-2's Invalid Command sub-code scheme is
     * "offset*256 + code" -- the offset of the field in error. */
    if (byte_count == 0 || (byte_count % SCS_MSCP_BLOCK_SIZE) != 0) {
        return build_transfer_end(
            end, end_len, cmd->cmd_ref, u->unit, SCS_MSCP_OP_READ,
            SCS_MSCP_STATUS(SCS_MSCP_ST_INVALID_CMD, SCS_MSCP_P_BCNT * 256u), 0u);
    }
    uint32_t nblocks = byte_count / SCS_MSCP_BLOCK_SIZE;
    if ((uint64_t)lbn + (uint64_t)nblocks > (uint64_t)u->unit_size) {
        return build_transfer_end(
            end, end_len, cmd->cmd_ref, u->unit, SCS_MSCP_OP_READ,
            SCS_MSCP_STATUS(SCS_MSCP_ST_INVALID_CMD, SCS_MSCP_P_LBN * 256u), 0u);
    }

    /* DESIGN DECISION (4). The blocks are read for real -- and then they have
     * to CROSS, by SCA block data transfer into the named host buffer, which
     * OVMX has no wire-grounded implementation of (rd vms-941). With no hook
     * installed the honest answer is a Controller Error, NOT a Success end
     * message for data that never moved. This branch is asserted by a unit
     * test precisely so it cannot quietly become a fake success. */
    if (srv->xfer == NULL) {
        srv->xfer_refusals++;
        return build_transfer_end(
            end, end_len, cmd->cmd_ref, u->unit, SCS_MSCP_OP_READ,
            SCS_MSCP_STATUS(SCS_MSCP_ST_CTLR_ERR,
                            SCS_MSCP_SUB_CNT_INCONSISTENT),
            0u);
    }

    /* One block at a time: the buffer stays bounded no matter what byte count
     * the class driver asks for, and a mid-transfer failure reports exactly the
     * bytes that did cross (sec 5.5). */
    uint8_t block[SCS_MSCP_BLOCK_SIZE];
    uint32_t moved = 0;
    for (uint32_t i = 0; i < nblocks; i++) {
        if (scs_mscp_srv_read_blocks(u, lbn + i, 1, block, sizeof(block)) < 0) {
            return build_transfer_end(
                end, end_len, cmd->cmd_ref, u->unit, SCS_MSCP_OP_READ,
                SCS_MSCP_STATUS(SCS_MSCP_ST_DRIVE_ERR, 0u), moved);
        }
        srv->blocks_read++;
        long n = srv->xfer(srv->xfer_ctx, body + SCS_MSCP_P_BUFF, lbn + i, block,
                           sizeof(block));
        if (n < 0 || (size_t)n != sizeof(block)) {
            /* The transfer service failed. sec 6.14 gives Host Buffer Access
             * Error for exactly this. Report the bytes that did cross. */
            return build_transfer_end(
                end, end_len, cmd->cmd_ref, u->unit, SCS_MSCP_OP_READ,
                SCS_MSCP_STATUS(SCS_MSCP_ST_HOST_BUF_ERR, 0u), moved);
        }
        moved += (uint32_t)n;
    }
    return build_transfer_end(end, end_len, cmd->cmd_ref, u->unit,
                              SCS_MSCP_OP_READ,
                              SCS_MSCP_STATUS(SCS_MSCP_ST_SUCCESS,
                                              SCS_MSCP_SUB_NORMAL),
                              moved);
}

static long handle_write(struct scs_mscp_srv *srv,
                         const struct scs_mscp_view *cmd, uint8_t *end,
                         size_t end_len)
{
    struct scs_mscp_srv_unit *u = scs_mscp_srv_find_unit(srv, cmd->unit);
    if (u == NULL) {
        return build_transfer_end(end, end_len, cmd->cmd_ref, cmd->unit,
                                  SCS_MSCP_OP_WRITE,
                                  SCS_MSCP_STATUS(SCS_MSCP_ST_OFFLINE,
                                                  SCS_MSCP_SUB_OFL_UNKNOWN),
                                  0u);
    }
    /* DESIGN DECISION (2). v1 is read-only, the unit ADVERTISED UF.WPS in every
     * ONLINE and GET UNIT STATUS end message, and a WRITE that arrives anyway
     * gets the published status for that situation -- Write Protected,
     * sub-code "Unit is Software Write Protected" (Table B-2, 0x1006). Never
     * dropped, never falsely acknowledged. */
    srv->writes_refused++;
    return build_transfer_end(end, end_len, cmd->cmd_ref, u->unit,
                              SCS_MSCP_OP_WRITE,
                              SCS_MSCP_STATUS(SCS_MSCP_ST_WRITE_PROT,
                                              SCS_MSCP_SUB_WP_SOFTWARE),
                              0u);
}

/* ================================ dispatch =============================== */

long scs_mscp_srv_handle(struct scs_mscp_srv *srv, uint32_t conid,
                         const struct scs_mscp_view *cmd, const uint8_t *body,
                         size_t body_len, uint8_t *end, size_t end_len)
{
    if (srv == NULL || cmd == NULL || body == NULL || end == NULL) {
        return -1;
    }
    if (body_len < SCS_MSCP_HDR_LEN || end_len < SCS_MSCP_HDR_LEN) {
        return -1;
    }
    /* This module answers COMMANDS. An end message arriving here is either a
     * loop or a misrouted client response, and answering it would put a second
     * end message on the wire for a command nobody sent. */
    if (cmd->is_end) {
        return -1;
    }
    /* sec 5.1: the command reference number is "unique, NON-ZERO". A zero one
     * is indistinguishable from the value a server puts in an unsolicited
     * message, so there is nothing safe to echo. */
    if (cmd->cmd_ref == 0) {
        return -1;
    }

    srv->cmds_received++;
    struct scs_mscp_srv_host *host = scs_mscp_srv_host_for(srv, conid);

    long n;
    switch (cmd->base_opcode) {
    case SCS_MSCP_OP_SET_CTLR_CHAR:
        n = build_scc_end(srv, host, cmd, body, body_len, end, end_len);
        break;

    case SCS_MSCP_OP_GET_UNIT_STATUS:
    case SCS_MSCP_OP_ONLINE:
    case SCS_MSCP_OP_READ:
    case SCS_MSCP_OP_WRITE:
        /* sec 3.4 makes "Controller-Online" the precondition for a class driver
         * doing anything: the first SET CONTROLLER CHARACTERISTICS is what
         * establishes the relationship. A command before it is answered
         * Invalid Command rather than serviced -- which is also what stops a
         * stray frame on a half-open connection from mounting a volume. */
        if (host == NULL || !host->ctlr_online) {
            n = build_invalid_command(
                end, end_len, cmd->cmd_ref, cmd->unit,
                SCS_MSCP_STATUS(SCS_MSCP_ST_INVALID_CMD, 0u));
            break;
        }
        if (cmd->base_opcode == SCS_MSCP_OP_GET_UNIT_STATUS) {
            n = build_gus_end(srv, cmd, end, end_len);
        } else if (cmd->base_opcode == SCS_MSCP_OP_ONLINE) {
            n = build_online_end(srv, cmd, body, body_len, end, end_len);
        } else if (cmd->base_opcode == SCS_MSCP_OP_READ) {
            n = handle_read(srv, cmd, body, body_len, end, end_len);
        } else {
            n = handle_write(srv, cmd, end, end_len);
        }
        break;

    default:
        /* Table A-1 note: the Invalid Command end message carries JUST OP.END.
         * Rule 8 keeps this deliberately broad -- OVMX does not implement the
         * other published opcodes and says so in the protocol's own words
         * rather than staying silent. */
        n = build_invalid_command(end, end_len, cmd->cmd_ref, cmd->unit,
                                  SCS_MSCP_STATUS(SCS_MSCP_ST_INVALID_CMD, 0u));
        break;
    }

    if (n > 0) {
        srv->ends_sent++;
    }
    return n;
}

/* ========================== the frame it rides in ======================== */

/*
 * THE SCA/PPD HEADER of a served end message. This is the SAME labeled replay
 * scs_mscp.c documents for the command direction -- the 0x4b-class PPD/NISCA
 * fields nobody has decoded ([8:10], [16:18], [24:26], [36:42]) -- with the
 * identity, sequencing and envelope fields substituted at build time and BOTH
 * length words derived. Kept separate from scs_mscp.c's copy because the
 * captured server frames carry their own [0:2] and inner length (they are 86-
 * and 110-content, not 94), and because sharing a mutable template between the
 * client and server directions would let a change to one silently move the
 * other.
 */
static const uint8_t srv_sca_hdr[SCS_MSCP_BODY_OFF] = {
    /* [0:2]   SCA content length - 2                (DERIVED at build time)  */
    0x54, 0x00,
    /* [2:8]   destination logical address           (substituted)            */
    0xaa, 0x00, 0x04, 0x00, 0x1a, 0x04,
    /* [8:10]  REPLAY -- undecoded 0x4b-class PPD field                       */
    0x01, 0x00,
    /* [10:16] source logical address                (substituted)            */
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,
    /* [16:18] REPLAY -- PPD marker 0x4b / format 0x13 (NOT the SCS MTYPE)    */
    0x4b, 0x13,
    /* [18:24] recv_ack / send_seq / incarnation      (substituted)           */
    0x19, 0x00, 0x19, 0x00, 0x01, 0x00,
    /* [24:26] REPLAY -- undecoded                                            */
    0x12, 0x00,
    /* [26:28] recv_ack mirror                        (substituted)           */
    0x19, 0x00,
    /* [28:30] REPLAY -- undecoded                                            */
    0x00, 0x00,
    /* [30:32] send_seq mirror                        (substituted)           */
    0x19, 0x00,
    /* [32:34] REPLAY -- undecoded                                            */
    0x00, 0x00,
    /* [34:36] recv_ack mirror                        (substituted)           */
    0x19, 0x00,
    /* [36:42] REPLAY -- undecoded 0x4b-class PPD fields                      */
    0x00, 0x00, 0x01, 0x00, 0x00, 0x02,
    /* [42:58] the SCS ENVELOPE -- overwritten by scs_env_build_frame()       */
    0x2a, 0x00, 0x04, 0x00, 0x0a, 0x00, 0x01, 0x00,
    0x08, 0x00, 0xd2, 0x8f, 0x0a, 0x00, 0x54, 0x35,
};

long scs_mscp_srv_build_end_frame(const struct scs_mscp_params *p,
                                  const uint8_t *body, size_t body_len,
                                  uint8_t *out, size_t out_len)
{
    if (p == NULL || body == NULL || out == NULL) {
        return -1;
    }
    if (body_len == 0 || body_len > SCS_MSCP_SRV_END_MAX) {
        return -1;
    }
    size_t sca_len = (size_t)SCS_MSCP_BODY_OFF + body_len;
    size_t frame_len = (size_t)SCS_ENV_ETH_HDR_LEN + sca_len;
    if (out_len < frame_len) {
        return -1;
    }

    memset(out, 0, frame_len);

    /* Ethernet header. */
    memcpy(out + 0, p->dst_mac, 6);
    memcpy(out + 6, p->src_mac, 6);
    out[12] = 0x60;
    out[13] = 0x07;

    memcpy(out + SCS_ENV_ETH_HDR_LEN, srv_sca_hdr, SCS_MSCP_BODY_OFF);

    /* [0:2] DERIVED, never inherited: the SCA content length counts the bytes
     * after itself. An over-declared length is dropped as a runt in silence. */
    put_le16(out + SCS_ENV_ETH_HDR_LEN + 0, (uint16_t)(sca_len - 2));

    /* Identity substitutions (SCA-content offsets + 14). */
    memcpy(out + SCS_ENV_ETH_HDR_LEN + 2, p->peer_logical, 6);
    memcpy(out + SCS_ENV_ETH_HDR_LEN + 10, p->src_logical, 6);

    /* SCS sequenced-message counters: recv_ack at [18:20] repeated at
     * [26:28]/[34:36]; send_seq at [20:22] mirrored at [30:32]. */
    put_le16(out + SCS_ENV_ETH_HDR_LEN + 18, p->recv_ack);
    put_le16(out + SCS_ENV_ETH_HDR_LEN + 20, p->send_seq);
    if (p->incarnation != 0) {
        put_le16(out + SCS_ENV_ETH_HDR_LEN + 22, p->incarnation);
    }
    put_le16(out + SCS_ENV_ETH_HDR_LEN + 26, p->recv_ack);
    put_le16(out + SCS_ENV_ETH_HDR_LEN + 30, p->send_seq);
    put_le16(out + SCS_ENV_ETH_HDR_LEN + 34, p->recv_ack);

    /* The SCS envelope, from the one shared build path (vms-ec7). The inner
     * length is derived there from the frame length, which is why an end
     * message of a different size needs no new envelope code. */
    {
        struct scs_env_fields env;
        env.mtype = SCS_ENV_MTYPE_APP_MESSAGE;
        env.credit = SCS_MSCP_ENV_CREDIT;
        env.dest_conid = p->remote_conid;
        env.src_conid = p->local_conid;
        if (scs_env_build_frame(out, frame_len, &env) != 0) {
            return -1;
        }
    }

    memcpy(out + SCS_ENV_ETH_HDR_LEN + SCS_MSCP_BODY_OFF, body, body_len);
    return (long)frame_len;
}
