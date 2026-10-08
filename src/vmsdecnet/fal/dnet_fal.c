/*
 * dnet_fal.c - the DECnet FILE ACCESS LISTENER (object 17) server + the COPY
 * node:: client (rd vms-8c2). See dnet_fal.h for the security argument and the
 * layer boundary. Two facts govern every line here:
 *   - AUTH IS REAL: the connect-carried username+password go through
 *     sysuaf_lookup + sysuaf_authenticate (Purdy) + the disabled-account gate,
 *     the SAME path LOGINOUT/SSHD use. A bad password is refused; a fake would
 *     pass it. (oracle docs/oracle/vax-copy-fal-dap.md §1.)
 *   - FILE I/O IS REAL: records move through RMS $OPEN/$GET and
 *     $CREATE/$PUT (dnet_fal_search.c) over the ODS-2 executive ACP -- never a
 *     raw POSIX file (Rule 9 / INV-6). No
 *     fork/exec/openpty/dup2, no raw-termios/raw-fd file mechanics.
 *
 * The DAP message SEQUENCE is the public DAP 5.6 spec's (sec. 5.1 setup,
 * 5.2.1 sequential file retrieval, 5.2.2 sequential file storage) and was
 * driven live against a real OpenVMS VAX V7.3 FAL (rd vms-a8a,
 * tests/lab/captures/decnet-fal-dap-20261004/):
 *   both:   CONFIGURATION <-> CONFIGURATION
 *   GET:    ATTRIBUTES, ACCESS(OPEN)      -> ATTRIBUTES [ext/NAME] ACK | STATUS
 *           CONTROL(CONNECT)              -> ACK
 *           CONTROL(GET, RAC=seq file)    -> DATA ... STATUS(EOF)
 *           ACCESS COMPLETE(CLOSE)        -> ACCESS COMPLETE(RESPONSE)
 *   PUT:    ATTRIBUTES, ACCESS(CREATE)    -> ATTRIBUTES [ext/NAME] ACK | STATUS
 *           CONTROL(CONNECT)              -> ACK
 *           CONTROL(PUT, RAC=seq file), DATA ..., ACCESS COMPLETE(CLOSE)
 *                                         -> ACCESS COMPLETE(RESPONSE) | STATUS
 * Field framing is dnet_dap.c (clean-room, Rule 8).
 *
 * RECORD SCOPE (INV-6): received records are $PUT verbatim to one RMS stream
 * (dnet_fal_wopen/wput/wclose, RMS over the ACP); records SENT are $GET from
 * the file's own RMS record format (dnet_fal_ropen/rget/rclose) -- a VAR file
 * an earlier COPY wrote reads back record for record. Binary/indexed/
 * relative files and block mode are not advertised and not served.
 */
#include "dnet_fal.h"
#include "dnet_cterm.h"     /* dnet_fal_access_decode (bounded cred decoder) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sysuaf.h"         /* the ONE faithful authenticator (Purdy)        */
#include "ssdef.h"
#include "rmsdef.h"

#define FAL_BUFSIZ  1459   /* the NSP segment size OVMX negotiates (oracle) */

/* ---- segment I/O + blocked-message splitting ------------------------------ */

static int fal_send(struct dnet_dap_transport *t, const struct dnet_dap_msg *m)
{
    uint8_t seg[DNET_DAP_MAX_MSG];
    size_t n = 0;
    if (dnet_dap_encode(m, 0, seg, sizeof seg, &n) != DNET_DAP_OK) return -1;
    return t->send(t->ctx, seg, n);
}

/* Next DAP message: from the held remainder of the last segment, else from a
 * freshly received one. A malformed message aborts the access (negative). */
static int fal_recv(struct dnet_dap_transport *t, struct dnet_dap_msg *m)
{
    while (t->rxoff >= t->rxlen) {
        size_t n = 0;
        if (t->recv(t->ctx, t->rx, sizeof t->rx, &n) < 0) return -1;
        t->rxlen = n; t->rxoff = 0;
    }
    size_t used = 0;
    if (dnet_dap_decode(t->rx + t->rxoff, t->rxlen - t->rxoff, m, &used) != DNET_DAP_OK
        || used == 0) {
        t->rxoff = t->rxlen = 0;
        return -1;
    }
    t->rxoff += used;
    return 0;
}

static int send_simple(struct dnet_dap_transport *t, enum dnet_dap_op op)
{
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = op;
    return fal_send(t, &m);
}

static int send_complete(struct dnet_dap_transport *t, uint8_t cmpfunc)
{
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_ACCESS_COMPLETE;
    m.u.complete.cmpfunc = cmpfunc;
    return fal_send(t, &m);
}

/* STATUS without an STV. The RMS refusals of an access carry the real VMS
 * STV a VAX FAL sends (rms_status_msg, rd vms-277a); the protocol statuses
 * sent here (sync, unsupported, EOF) carry none. */
static int send_status(struct dnet_dap_transport *t, uint16_t stscode, uint32_t stv)
{
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_STATUS;
    m.u.status.stscode = stscode;
    m.u.status.have_stv = (stv != 0);
    m.u.status.stv = stv;
    return fal_send(t, &m);
}

static int send_config(struct dnet_dap_transport *t)
{
    struct dnet_dap_msg m;
    dnet_dap_ovmx_config(&m, FAL_BUFSIZ);
    return fal_send(t, &m);
}

/* The attributes OVMX presents for a text file it serves or sends: ASCII,
 * sequential, variable-length records, implied CR carriage control. An OPEN
 * overrides rfm/rat with the file's real ones. */
static void text_attributes(struct dnet_dap_msg *m)
{
    memset(m, 0, sizeof *m);
    m->op = DNET_DAP_ATTRIBUTES;
    m->u.attr.menu = (1u << DNET_DAP_ATT_DATATYPE) | (1u << DNET_DAP_ATT_ORG) |
                     (1u << DNET_DAP_ATT_RFM) | (1u << DNET_DAP_ATT_RAT);
    m->u.attr.datatype = DNET_DAP_DT_ASCII;
    m->u.attr.org = DNET_DAP_ORG_SEQ;
    m->u.attr.rfm = DNET_DAP_RFM_VAR;
    m->u.attr.rat = DNET_DAP_RAT_CR;
}

/* Map a peer STATUS to the VMS condition the COPY caller reports. A real VMS
 * FAL carries the underlying system status in STV; FNF before open is
 * SS$_NOSUCHFILE either way. */
static uint32_t status_to_cond(const struct dnet_dap_msg *m)
{
    uint16_t sts = m->u.status.stscode;
    if (DNET_DAP_MAC(sts) == DNET_DAP_MAC_OPEN && DNET_DAP_MIC(sts) == DNET_DAP_MIC_FNF)
        return SS$_NOSUCHFILE;
    if (m->u.status.have_stv && m->u.status.stv != 0 && m->u.status.stv <= 0xffffffffu)
        return (uint32_t)m->u.status.stv;
    return SS$_ABORT;
}

/* Wait for the end of a setup/connect reply: skip ATTRIBUTES / extended
 * attributes / NAME, stop on ACK (0) or STATUS (*cond set, 1). */
static int await_ack(struct dnet_dap_transport *t, uint32_t *cond)
{
    struct dnet_dap_msg m;
    for (;;) {
        if (fal_recv(t, &m) < 0) { *cond = SS$_ABORT; return 1; }
        switch (m.op) {
        case DNET_DAP_ACKNOWLEDGE: return 0;
        case DNET_DAP_STATUS:      *cond = status_to_cond(&m); return 1;
        case DNET_DAP_ATTRIBUTES: case DNET_DAP_NAME: case DNET_DAP_KEYDEF:
        case DNET_DAP_ALLOC: case DNET_DAP_SUMMARY: case DNET_DAP_DATETIME:
        case DNET_DAP_PROTECTION: case DNET_DAP_ACL:
            continue;
        default:
            *cond = SS$_ABORT; return 1;
        }
    }
}

static int config_exchange_client(struct dnet_dap_transport *t)
{
    struct dnet_dap_msg m;
    if (send_config(t) < 0) return -1;
    if (fal_recv(t, &m) < 0 || m.op != DNET_DAP_CONFIG) return -1;
    return 0;
}

/* ---- authentication ------------------------------------------------------ */

/* Authenticate and, on success, hand back the verified SYSUAF record. */
static uint32_t fal_authenticate_rec(const char *username, const char *password,
                                     sysuaf_record_t *rec)
{
    if (!username || !password || username[0] == '\0')
        return SS$_INVLOGIN;
    /* No-such-user and a wrong password both surface as SS$_INVLOGIN so the
     * peer cannot probe which usernames exist -- exactly as a real login does
     * not distinguish them. Fail-honest: no SYSUAF / no /dev/vms -> lookup
     * fails -> refuse (never fabricate a pass). */
    if (sysuaf_lookup(username, rec) != 0)
        return SS$_INVLOGIN;
    if (!sysuaf_authenticate(rec, password))
        return SS$_INVLOGIN;
    /* A real account with the right password but DISUSER/DISACNT is refused --
     * the disabled-account gate, same as the interactive/SSH paths. */
    if (!sysuaf_interactive_login_permitted(rec))
        return SS$_NOPRIV;
    return SS$_NORMAL;
}

uint32_t dnet_fal_authenticate(const char *username, const char *password)
{
    sysuaf_record_t rec;
    uint32_t st = fal_authenticate_rec(username, password, &rec);
    memset(&rec, 0, sizeof rec);
    return st;
}


/* Serve an opened file (GET). Returns SS$_NORMAL after ACCESS COMPLETE. */
static int send_record_n(struct dnet_dap_transport *t, const uint8_t *rec, size_t n)
{
    struct dnet_dap_msg d;
    memset(&d, 0, sizeof d);
    d.op = DNET_DAP_DATA;
    if (n > DNET_DAP_MAX_REC) n = DNET_DAP_MAX_REC;
    d.u.data.reclen = (uint16_t)n;
    if (n) memcpy(d.u.data.rec, rec, n);
    return fal_send(t, &d);
}

/* Serve an opened file (GET). Two access modes, both spec 5.2: sequential
 * FILE TRANSFER (RAC 3 -- one CONTROL GET streams every record, then EOF) and
 * sequential RECORD access (RAC 0 -- one CONTROL GET per record, the mode a
 * real VMS COPY uses as the reader, rd vms-d85 lab). Returns SS$_NORMAL after
 * ACCESS COMPLETE. */
static uint32_t server_open_phase(struct dnet_dap_transport *t, const char *spec)
{
    struct dnet_dap_msg m;
    void *rf = NULL;                     /* the open RMS record stream      */
    int at_eof = 0;
    uint8_t rec[DNET_DAP_MAX_REC];
    size_t rlen = 0;
    uint32_t result = SS$_ABORT;
    for (;;) {
        if (fal_recv(t, &m) < 0) break;
        if (m.op == DNET_DAP_CONTROL && m.u.control.ctlfunc == DNET_DAP_CTL_CONNECT) {
            if (!rf && dnet_fal_ropen(spec, &rf, NULL, NULL) != 0) rf = NULL;
            at_eof = 0;
            if (!rf) {
                if (send_status(t, (DNET_DAP_MAC_XFER << 12) | DNET_DAP_MIC_FNF, 0) < 0) break;
                continue;
            }
            if (send_simple(t, DNET_DAP_ACKNOWLEDGE) < 0) break;
            continue;
        }
        if (m.op == DNET_DAP_CONTROL && m.u.control.ctlfunc == DNET_DAP_CTL_GET) {
            uint8_t rac = (m.u.control.menu & DNET_DAP_CTLM_RAC) ? m.u.control.rac
                                                                : DNET_DAP_RAC_SEQ;
            if (rac != DNET_DAP_RAC_SEQFILE && rac != DNET_DAP_RAC_SEQ) {
                /* Keyed / RFA / block access are not advertised or served. */
                if (send_status(t, (DNET_DAP_MAC_UNSUPP << 12) | 0, 0) < 0) break;
                continue;
            }
            if (!rf && dnet_fal_ropen(spec, &rf, NULL, NULL) != 0) rf = NULL;
            if (!rf) {
                if (send_status(t, (DNET_DAP_MAC_XFER << 12) | DNET_DAP_MIC_FNF, 0) < 0) break;
                continue;
            }
            int g = 0;
            if (rac == DNET_DAP_RAC_SEQFILE) {
                int ok = 1;
                while (!at_eof && (g = dnet_fal_rget(rf, rec, sizeof rec, &rlen)) == 1)
                    if (send_record_n(t, rec, rlen) < 0) { ok = 0; break; }
                if (!ok || g < 0) break;      /* a read error ends the access */
                at_eof = 1;
                if (send_status(t, DNET_DAP_STS_EOF, 0) < 0) break;
            } else if (!at_eof && (g = dnet_fal_rget(rf, rec, sizeof rec, &rlen)) == 1) {
                if (send_record_n(t, rec, rlen) < 0) break;
            } else {
                if (g < 0) break;
                at_eof = 1;
                if (send_status(t, DNET_DAP_STS_EOF, 0) < 0) break;
            }
            continue;
        }
        if (m.op == DNET_DAP_ACCESS_COMPLETE) {
            if (send_complete(t, DNET_DAP_CMP_RESPONSE) < 0) break;
            if (m.u.complete.cmpfunc == DNET_DAP_CMP_EOS) continue;   /* stream only */
            result = SS$_NORMAL;
            break;
        }
        (void)send_status(t, (DNET_DAP_MAC_SYNC << 12) | (m.type & 0xfff), 0);
        break;
    }
    if (rf) (void)dnet_fal_rclose(rf);
    return result;
}

/* Store a created file (PUT): every DATA record is $PUT, verbatim, to the
 * stream $CREATEd at ACCESS time; $CLOSE at ACCESS COMPLETE. */
static uint32_t server_create_phase(struct dnet_dap_transport *t, void *h)
{
    struct dnet_dap_msg m;
    int putting = 0;
    for (;;) {
        if (fal_recv(t, &m) < 0) { (void)dnet_fal_wclose(h); return SS$_ABORT; }
        if (m.op == DNET_DAP_CONTROL && m.u.control.ctlfunc == DNET_DAP_CTL_CONNECT) {
            if (send_simple(t, DNET_DAP_ACKNOWLEDGE) < 0) { (void)dnet_fal_wclose(h); return SS$_ABORT; }
            continue;
        }
        if (m.op == DNET_DAP_CONTROL && m.u.control.ctlfunc == DNET_DAP_CTL_PUT) {
            uint8_t rac = (m.u.control.menu & DNET_DAP_CTLM_RAC) ? m.u.control.rac
                                                                : DNET_DAP_RAC_SEQ;
            if (rac != DNET_DAP_RAC_SEQFILE && rac != DNET_DAP_RAC_SEQ) {
                if (send_status(t, (DNET_DAP_MAC_UNSUPP << 12) | 0, 0) < 0) { (void)dnet_fal_wclose(h); return SS$_ABORT; }
                continue;
            }
            putting = 1;
            continue;
        }
        if (m.op == DNET_DAP_DATA) {
            if (!putting || dnet_fal_wput(h, m.u.data.rec, m.u.data.reclen) != 0) {
                (void)send_status(t, (DNET_DAP_MAC_XFER << 12) | 0, 0);
                (void)dnet_fal_wclose(h);
                return SS$_ABORT;
            }
            continue;
        }
        if (m.op == DNET_DAP_ACCESS_COMPLETE) {
            if (m.u.complete.cmpfunc == DNET_DAP_CMP_EOS) {
                putting = 0;
                if (send_complete(t, DNET_DAP_CMP_RESPONSE) < 0) { (void)dnet_fal_wclose(h); return SS$_ABORT; }
                continue;
            }
            if (dnet_fal_wclose(h) != 0) {
                (void)send_status(t, (DNET_DAP_MAC_TERM << 12) | 0, 0);
                return SS$_ABORT;
            }
            return (send_complete(t, DNET_DAP_CMP_RESPONSE) < 0) ? SS$_ABORT : SS$_NORMAL;
        }
        (void)send_status(t, (DNET_DAP_MAC_SYNC << 12) | (m.type & 0xfff), 0);
        (void)dnet_fal_wclose(h);
        return SS$_ABORT;
    }
}

uint32_t dnet_fal_connect_auth(const uint8_t *conn_data, size_t conn_len,
                               char *authed_user, size_t authed_user_cap)
{
    struct dnet_fal_identity id;
    uint32_t st = dnet_fal_connect_auth_id(conn_data, conn_len, &id);
    if (authed_user && authed_user_cap) {
        authed_user[0] = '\0';
        if (st == SS$_NORMAL) {
            strncpy(authed_user, id.username, authed_user_cap - 1);
            authed_user[authed_user_cap - 1] = '\0';
        }
    }
    memset(&id, 0, sizeof id);
    return st;
}

uint32_t dnet_fal_connect_auth_id(const uint8_t *conn_data, size_t conn_len,
                                  struct dnet_fal_identity *id)
{
    if (id) memset(id, 0, sizeof *id);

    /* Decode the connect-carried credentials (bounded), authenticate, wipe. */
    char user[DNET_FAL_USER_MAX + 1];
    char pass[DNET_FAL_PASS_MAX + 1];
    char acct[DNET_FAL_USER_MAX + 1];
    int drc = dnet_fal_access_decode(conn_data, conn_len,
                                     user, sizeof user,
                                     pass, sizeof pass,
                                     acct, sizeof acct);
    /* A malformed connect never reaches the authenticator -- it is refused as
     * an invalid login, exactly as a wrong password is. */
    sysuaf_record_t rec;
    memset(&rec, 0, sizeof rec);
    uint32_t auth = (drc != 0) ? SS$_INVLOGIN : fal_authenticate_rec(user, pass, &rec);

    /* The password never outlives the check. */
    memset(pass, 0, sizeof pass);
    memset(acct, 0, sizeof acct);

    if (auth == SS$_NORMAL && id) {
        snprintf(id->username, sizeof id->username, "%s", user);
        id->uic = (rec.uic_group << 16) | (rec.uic_member & 0xffffu);
        const uint8_t *dp = rec.raw.uaf$q_def_priv;
        id->def_privs = (uint64_t)dp[0] | ((uint64_t)dp[1] << 8) |
                        ((uint64_t)dp[2] << 16) | ((uint64_t)dp[3] << 24) |
                        ((uint64_t)dp[4] << 32) | ((uint64_t)dp[5] << 40) |
                        ((uint64_t)dp[6] << 48) | ((uint64_t)dp[7] << 56);
        snprintf(id->default_dir, sizeof id->default_dir, "%s", rec.default_dir);
    }
    memset(user, 0, sizeof user);
    memset(&rec, 0, sizeof rec);   /* the record carries the password hash */
    return auth;   /* non-NORMAL: caller sends the NSP disconnect; no CC, no DAP */
}

/* Send NAME(type, spec). */
static int send_name(struct dnet_dap_transport *t, unsigned type, const char *spec)
{
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_NAME;
    m.u.name.nametype = type;
    snprintf(m.u.name.namespec, sizeof m.u.name.namespec, "%s", spec);
    return fal_send(t, &m);
}

/* ---- blocked replies (rd vms-277a) ----------------------------------------
 * A real VMS FAL answers a DIRECTORY LIST, an ERASE or a RENAME in ONE Session
 * Control buffer: every message but the last carries FLAGS.LENGTH, the last
 * runs to the end of the segment (spec 3.2; both peers advertise SYSCAP
 * "blocking up to response", tests/lab/captures/decnet-fal-verbs-20261008).
 * The batch holds the committed (LENGTH) messages plus the pending last one in
 * both forms, and never builds a segment longer than the smaller of the
 * peer's CONFIGURATION BUFSIZ and OVMX's own. */
struct fal_batch {
    struct dnet_dap_transport *t;
    size_t  cap;
    uint8_t seg[DNET_FAL_SEG_MAX];
    size_t  len;
    uint8_t last[DNET_DAP_MAX_MSG], lastb[DNET_DAP_MAX_MSG];
    size_t  lastlen, lastblen;
    int     have_last;
};

static int fb_flush(struct fal_batch *b)
{
    if (!b->have_last) return 0;
    memcpy(b->seg + b->len, b->last, b->lastlen);
    size_t n = b->len + b->lastlen;
    b->len = 0; b->have_last = 0;
    return b->t->send(b->t->ctx, b->seg, n);
}

static int fb_add(struct fal_batch *b, const struct dnet_dap_msg *m)
{
    uint8_t u[DNET_DAP_MAX_MSG], k[DNET_DAP_MAX_MSG];
    size_t un = 0, kn = 0;
    if (dnet_dap_encode(m, 0, u, sizeof u, &un) != DNET_DAP_OK ||
        dnet_dap_encode(m, 1, k, sizeof k, &kn) != DNET_DAP_OK)
        return -1;
    if (b->have_last) {
        if (b->len + b->lastblen + un > b->cap) {
            if (fb_flush(b) < 0) return -1;
        } else {
            memcpy(b->seg + b->len, b->lastb, b->lastblen);
            b->len += b->lastblen;
        }
    }
    memcpy(b->last, u, un);  b->lastlen = un;
    memcpy(b->lastb, k, kn); b->lastblen = kn;
    b->have_last = 1;
    return 0;
}

static int fb_name(struct fal_batch *b, unsigned type, const char *spec)
{
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_NAME;
    m.u.name.nametype = type;
    snprintf(m.u.name.namespec, sizeof m.u.name.namespec, "%s", spec);
    return fb_add(b, &m);
}

static int fb_simple(struct fal_batch *b, enum dnet_dap_op op)
{
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = op;
    return fb_add(b, &m);
}

static int fb_complete(struct fal_batch *b)
{
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_ACCESS_COMPLETE;
    m.u.complete.cmpfunc = DNET_DAP_CMP_RESPONSE;
    return fb_add(b, &m);
}

/* ---- RMS completion -> DAP STATUS (rd vms-277a) ----------------------------
 * The REFUSAL itself always comes from RMS over the executive ACP, as this
 * process (the authenticated user); only its ENCODING is chosen here, and it
 * is the encoding a real VMS V7.3 FAL put on the wire for the same outcome
 * (tests/lab/captures/decnet-fal-verbs-20261008, segment by segment):
 *   - not found (DIRECTORY LIST, OPEN, ERASE): MAC 4 / MIC 062 FNF,
 *     STV 0x0910 (real VMS SS$_NOSUCHFILE)              STATUS 09 00 32 40 00 00 02 10 09
 *   - OPEN refused by protection: the SAME FNF + 0x0910 -- the VAX FAL
 *     answered DNTEST's TYPE of a SYSTEM-only file exactly so (every OPEN
 *     refusal is answered FNF, the server's behaviour before vms-277a too)
 *   - ERASE refused by protection: MAC 4 / MIC 0125 PRV, STV 0x24 (real VMS
 *     SS$_NOPRIV)                                        STATUS 09 00 55 40 00 00 01 24
 *   - RENAME refused by protection: MAC 4 / MIC 0137 RMV, no STV
 *                                                        STATUS 09 00 5f 40
 * The STV values are the REAL VMS condition values seen on the wire, not
 * OVMX's own ssdef.h numbers (rd vms-ef2: several of those differ from VMS).
 * Other refusals map to the DAP 5.6 Table 4 MICCODE of the same RMS error,
 * without an STV (none was observed to copy). */
#define VMS_WIRE_SS_NOSUCHFILE 0x0910u
#define VMS_WIRE_SS_NOPRIV     0x0024u

enum fal_verb { FV_OPEN, FV_LIST, FV_ERASE, FV_RENAME };

static void rms_status_msg(enum fal_verb v, uint32_t sts, struct dnet_dap_msg *m)
{
    memset(m, 0, sizeof *m);
    m->op = DNET_DAP_STATUS;
    uint16_t mic = 0;
    uint32_t stv = 0;
    int fnf = (sts == RMS$_FNF || sts == RMS$_NMF);
    int prv = (sts == RMS$_PRV);
    /* OPEN: every refusal is FNF, as it was before rd vms-277a (and as the VAX
     * FAL answered a protection refusal); the cause stays in the ACP. */
    if (fnf || v == FV_OPEN)          { mic = DNET_DAP_MIC_FNF; stv = VMS_WIRE_SS_NOSUCHFILE; }
    else if (prv && v == FV_RENAME)   { mic = DNET_DAP_MIC_RMV; }
    else if (prv)                     { mic = DNET_DAP_MIC_PRV; stv = VMS_WIRE_SS_NOPRIV; }
    else if (sts == RMS$_DNF)         { mic = DNET_DAP_MIC_DNF; }
    else if (sts == RMS$_FEX)         { mic = DNET_DAP_MIC_FEX; }
    else if (sts == RMS$_ACC)         { mic = DNET_DAP_MIC_ACC; }
    m->u.status.stscode = (uint16_t)((DNET_DAP_MAC_OPEN << 12) | mic);
    m->u.status.have_stv = (stv != 0);
    m->u.status.stv = stv;
}

/* The access's own result (FAL.EXE's exit status) for an RMS refusal. */
static uint32_t rms_cond(uint32_t sts)
{
    if (sts == RMS$_FNF || sts == RMS$_NMF) return SS$_NOSUCHFILE;
    if (sts == RMS$_PRV) return SS$_NOPRIV;
    return SS$_BADPARAM;
}

/* Split a spec "DEV:[DIR]NAME.TYP;V" into its volume ("DEV:", may be empty),
 * directory ("[DIR]", may be empty) and file parts -- the three NAME messages
 * of a DIRECTORY LIST. Pure string surgery on a spec RMS returned. */
static void spec_split(const char *s, char *vol, char *dir, char *file, size_t cap)
{
    const char *lb = strpbrk(s, "[<");
    const char *rb = lb ? strpbrk(lb, "]>") : NULL;
    const char *colon = NULL;
    for (const char *p = s; *p && (!lb || p < lb); p++) if (*p == ':') colon = p;
    if (lb && !rb) lb = NULL;
    size_t vn = colon ? (size_t)(colon - s + 1) : 0;
    snprintf(vol, cap, "%.*s", (int)vn, s);
    if (lb) {
        snprintf(dir, cap, "%.*s", (int)(rb - lb + 1), lb);
        snprintf(file, cap, "%s", rb + 1);
    } else {
        dir[0] = '\0';
        snprintf(file, cap, "%s", s + vn);
    }
}

/* VMS 64-bit absolute time -> the DAP A-18 "dd-MON-yy hh:mm:ss" (spec 3.15).
 * 0 (no date recorded) or a delta time -> -1: the field is then omitted. */
static int vms_time_dap(const uint8_t q[8], char out[DNET_DAP_DATE_LEN + 1])
{
    static const char mon[12][4] = { "JAN","FEB","MAR","APR","MAY","JUN",
                                     "JUL","AUG","SEP","OCT","NOV","DEC" };
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | q[i];
    if (v == 0 || (v >> 63)) return -1;
    uint64_t secs = v / 10000000u;
    int64_t days = (int64_t)(secs / 86400u) - 40587;   /* MJD 0 = 17-NOV-1858 */
    unsigned sod = (unsigned)(secs % 86400u);
    /* days since 1970-01-01 -> civil date (proleptic Gregorian). */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    snprintf(out, DNET_DAP_DATE_LEN + 1, "%02u-%s-%02u %02u:%02u:%02u", d, mon[m - 1],
             (unsigned)(((y % 100) + 100) % 100), sod / 3600, (sod / 60) % 60, sod % 60);
    return 0;
}

/* The attribute messages DISPLAY asks for, from the file's real header:
 * MAIN ATTRIBUTES, SUMMARY, DATE AND TIME, PROTECTION -- in the spec's order
 * and the order the VAX FAL sent them. Only fields the header holds are sent:
 * no FOP / DEV / SBN / datatype (not read here) and nothing DAP 7 adds
 * (ATTMENU bit 21, binary DATE AND TIME, message type 18 "File ID" -- a DAP 7
 * peer's; OVMX advertises DAP 5.6). Returns -1 only on a transport failure. */
static int fb_attrs(struct fal_batch *b, uint64_t display, const struct dnet_fal_fattr *fa)
{
    struct dnet_dap_msg m;
    if (display & DNET_DAP_DSP_MAIN) {
        memset(&m, 0, sizeof m);
        m.op = DNET_DAP_ATTRIBUTES;
        m.u.attr.menu = (1u << DNET_DAP_ATT_ORG) | (1u << DNET_DAP_ATT_RFM) |
                        (1u << DNET_DAP_ATT_RAT) | (1u << DNET_DAP_ATT_MRS) |
                        (1u << DNET_DAP_ATT_ALQ) | (1u << DNET_DAP_ATT_DEQ) |
                        (1u << DNET_DAP_ATT_LRL) | (1u << DNET_DAP_ATT_HBK) |
                        (1u << DNET_DAP_ATT_EBK) | (1u << DNET_DAP_ATT_FFB);
        m.u.attr.org = fa->org;
        m.u.attr.rfm = fa->rfm;
        m.u.attr.rat = fa->rat & 0x0f;
        m.u.attr.mrs = fa->mrs;
        m.u.attr.alq = fa->alq;
        m.u.attr.deq = fa->deq;
        m.u.attr.lrl = fa->lrl;
        m.u.attr.hbk = fa->alq;
        m.u.attr.ebk = fa->ebk;
        m.u.attr.ffb = fa->ffb;
        if (fb_add(b, &m) < 0) return -1;
    }
    /* A sequential file has no keys, areas or record descriptors to
     * summarise: the VAX FAL sends SUMMARY with an empty operand, and so
     * does OVMX (nothing held, nothing claimed). */
    if ((display & DNET_DAP_DSP_SUMMARY) && fa->org == DNET_DAP_ORG_SEQ) {
        memset(&m, 0, sizeof m);
        m.op = DNET_DAP_SUMMARY;
        if (fb_add(b, &m) < 0) return -1;
    }
    if (display & DNET_DAP_DSP_DATETIME) {
        memset(&m, 0, sizeof m);
        m.op = DNET_DAP_DATETIME;
        if (vms_time_dap(fa->credate, m.u.datetime.cdt) == 0) m.u.datetime.menu |= DNET_DAP_DAT_CDT;
        if (vms_time_dap(fa->revdate, m.u.datetime.rdt) == 0) m.u.datetime.menu |= DNET_DAP_DAT_RDT;
        if (vms_time_dap(fa->expdate, m.u.datetime.edt) == 0) m.u.datetime.menu |= DNET_DAP_DAT_EDT;
        m.u.datetime.menu |= DNET_DAP_DAT_RVN;
        m.u.datetime.rvn = fa->revision;
        if (fb_add(b, &m) < 0) return -1;
    }
    if (display & DNET_DAP_DSP_PROT) {
        memset(&m, 0, sizeof m);
        m.op = DNET_DAP_PROTECTION;
        m.u.prot.menu = DNET_DAP_PRM_OWNER | DNET_DAP_PRM_SYS | DNET_DAP_PRM_OWN |
                        DNET_DAP_PRM_GRP | DNET_DAP_PRM_WLD;
        snprintf(m.u.prot.owner, sizeof m.u.prot.owner, "[%06o,%06o]",
                 (unsigned)fa->uic_group, (unsigned)fa->uic_member);
        m.u.prot.psys = fa->fileprot & 0xf;
        m.u.prot.pown = (fa->fileprot >> 4) & 0xf;
        m.u.prot.pgrp = (fa->fileprot >> 8) & 0xf;
        m.u.prot.pwld = (fa->fileprot >> 12) & 0xf;
        if (fb_add(b, &m) < 0) return -1;
    }
    return 0;
}

#define FAL_ATTR_DISPLAY (DNET_DAP_DSP_MAIN | DNET_DAP_DSP_SUMMARY | \
                          DNET_DAP_DSP_DATETIME | DNET_DAP_DSP_PROT)

/* DIRECTORY LIST (spec 5.2.11), in the VAX FAL's shape: for every file the spec
 * (wildcards allowed) resolves to, NAME(volume) / NAME(directory) when they
 * change, NAME(file), the attribute messages DISPLAY asks for, [NAME(resultant)
 * if DISPLAY bit 8], ACKNOWLEDGE; then ACCESS COMPLETE(RESPONSE) -- one blocked
 * segment. Names are the RESULTANT specs RMS $SEARCH returns, never composed.
 * Nothing found: the volume / directory / file NAMEs of the EXPANDED spec RMS
 * parsed, then STATUS (FNF + STV 0x0910), exactly as the VAX FAL answers. */
static uint32_t server_dirlist(struct fal_batch *b, const struct dnet_dap_msg *acc)
{
    void *ctx = NULL;
    char rsa[DNET_DAP_MAX_SPEC + 1];
    char vol[DNET_DAP_MAX_SPEC + 1], dir[DNET_DAP_MAX_SPEC + 1], file[DNET_DAP_MAX_SPEC + 1];
    char lastvol[DNET_DAP_MAX_SPEC + 1] = "", lastdir[DNET_DAP_MAX_SPEC + 1] = "";
    uint64_t display = acc->u.access.have_display ? acc->u.access.display : 0;
    int n = 0, first = 1;
    uint32_t sts = RMS$_FNF;
    if (dnet_fal_search_begin(acc->u.access.filespec, &ctx) != 0) ctx = NULL;
    while (ctx && dnet_fal_search_next(ctx, rsa, sizeof rsa) == 0) {
        spec_split(rsa, vol, dir, file, sizeof vol);
        if (vol[0] && (first || strcmp(vol, lastvol) != 0)) {
            if (fb_name(b, DNET_DAP_NT_VOLUME, vol) < 0) goto abort;
            snprintf(lastvol, sizeof lastvol, "%s", vol);
            lastdir[0] = '\0';
        }
        if (dir[0] && strcmp(dir, lastdir) != 0) {
            if (fb_name(b, DNET_DAP_NT_DIRECTORY, dir) < 0) goto abort;
            snprintf(lastdir, sizeof lastdir, "%s", dir);
        }
        first = 0;
        if (fb_name(b, DNET_DAP_NT_FILENAME, file) < 0) goto abort;
        if (display & FAL_ATTR_DISPLAY) {
            struct dnet_fal_fattr fa;
            /* A file whose header this user may not read gets its NAME and no
             * attributes: nothing is sent that was not read (INV-6). */
            if (dnet_fal_fileattr(rsa, &fa, NULL) == 0 && fb_attrs(b, display, &fa) < 0)
                goto abort;
        }
        if ((display & DNET_DAP_DSP_NAME) && fb_name(b, DNET_DAP_NT_FILESPEC, rsa) < 0)
            goto abort;
        if (fb_simple(b, DNET_DAP_ACKNOWLEDGE) < 0) goto abort;
        n++;
    }
    if (n == 0) {
        char esa[DNET_DAP_MAX_SPEC + 1] = "";
        if (ctx) sts = dnet_fal_search_status(ctx, NULL, esa, sizeof esa);
        if (esa[0]) {
            spec_split(esa, vol, dir, file, sizeof vol);
            if (vol[0] && fb_name(b, DNET_DAP_NT_VOLUME, vol) < 0) goto abort;
            if (dir[0] && fb_name(b, DNET_DAP_NT_DIRECTORY, dir) < 0) goto abort;
            if (fb_name(b, DNET_DAP_NT_FILENAME, file) < 0) goto abort;
        }
        if (ctx) dnet_fal_search_end(ctx);
        struct dnet_dap_msg s;
        rms_status_msg(FV_LIST, sts, &s);
        if (fb_add(b, &s) < 0 || fb_flush(b) < 0) return SS$_ABORT;
        return rms_cond(sts);
    }
    sts = dnet_fal_search_status(ctx, NULL, NULL, 0);
    dnet_fal_search_end(ctx);
    if (sts != RMS$_NMF && sts != RMS$_FNF && !(sts & 1)) {
        /* The listing broke off on a real error after some files. */
        struct dnet_dap_msg s;
        rms_status_msg(FV_LIST, sts, &s);
        if (fb_add(b, &s) < 0 || fb_flush(b) < 0) return SS$_ABORT;
        return rms_cond(sts);
    }
    if (fb_complete(b) < 0 || fb_flush(b) < 0) return SS$_ABORT;
    return SS$_NORMAL;
abort:
    if (ctx) dnet_fal_search_end(ctx);
    return SS$_ABORT;
}

/* The resultant spec of the file an OPEN names (first $SEARCH match). */
static int resolve_one(const char *spec, char *rsa, size_t cap)
{
    void *ctx = NULL;
    int rc = -1;
    if (dnet_fal_search_begin(spec, &ctx) == 0) {
        rc = dnet_fal_search_next(ctx, rsa, cap);
        dnet_fal_search_end(ctx);
    }
    return rc;
}

static int spec_is_wild(const char *s)
{
    return strpbrk(s, "*%") != NULL || strstr(s, "...") != NULL;
}

/* Send a STATUS for an RMS refusal as the whole (one-segment) reply. */
static uint32_t fb_refuse(struct fal_batch *b, enum fal_verb v, uint32_t sts)
{
    struct dnet_dap_msg s;
    rms_status_msg(v, sts, &s);
    if (fb_add(b, &s) < 0 || fb_flush(b) < 0) return SS$_ABORT;
    return rms_cond(sts);
}

/* ERASE (spec 5.2.6): ACCESS(ERASE) -> [NAME(resultant) ACK] ACCESS COMPLETE
 * (RESPONSE), or STATUS. $ERASE runs as this process, so the executive ACP
 * decides whether the user may delete the file. A wildcard spec erases every
 * file $SEARCH resolves, each by its resultant; the first refusal ends the
 * access with its STATUS. */
static uint32_t server_erase(struct fal_batch *b, const struct dnet_dap_msg *acc)
{
    const char *spec = acc->u.access.filespec;
    int want_name = acc->u.access.have_display && (acc->u.access.display & DNET_DAP_DSP_NAME);
    uint32_t sts = 0, stv = 0;
    char rsa[DNET_DAP_MAX_SPEC + 1] = "";
    if (!spec_is_wild(spec)) {
        /* The resultant is for the NAME reply; the ERASE is RMS's verdict,
         * whether or not this user may $SEARCH the directory. */
        if (resolve_one(spec, rsa, sizeof rsa) != 0) rsa[0] = '\0';
        if (dnet_fal_erase(rsa[0] ? rsa : spec, &sts, &stv) != 0)
            return fb_refuse(b, FV_ERASE, sts);
        if (want_name && rsa[0] &&
            (fb_name(b, DNET_DAP_NT_FILESPEC, rsa) < 0 || fb_simple(b, DNET_DAP_ACKNOWLEDGE) < 0))
            return SS$_ABORT;
        return (fb_complete(b) < 0 || fb_flush(b) < 0) ? SS$_ABORT : SS$_NORMAL;
    }
    /* Wildcard: resolve every match first, then erase -- never delete under
     * a live directory search. */
    void *ctx = NULL;
    size_t nm = 0, capn = 0;
    char (*names)[DNET_DAP_MAX_SPEC + 1] = NULL;
    uint32_t ssts = RMS$_FNF;
    if (dnet_fal_search_begin(spec, &ctx) == 0) {
        while (dnet_fal_search_next(ctx, rsa, sizeof rsa) == 0) {
            if (nm == capn) {
                size_t nc = capn ? capn * 2 : 16;
                if (nc > 4096) break;
                void *p = realloc(names, nc * sizeof *names);
                if (!p) break;
                names = p; capn = nc;
            }
            memcpy(names[nm++], rsa, sizeof rsa);
        }
        ssts = dnet_fal_search_status(ctx, NULL, NULL, 0);
        dnet_fal_search_end(ctx);
    }
    uint32_t result;
    if (nm == 0) {
        result = fb_refuse(b, FV_ERASE, ssts);
    } else {
        result = SS$_NORMAL;
        for (size_t i = 0; i < nm; i++) {
            if (dnet_fal_erase(names[i], &sts, &stv) != 0) {
                result = fb_refuse(b, FV_ERASE, sts);
                break;
            }
            if (want_name && (fb_name(b, DNET_DAP_NT_FILESPEC, names[i]) < 0 ||
                              fb_simple(b, DNET_DAP_ACKNOWLEDGE) < 0)) {
                result = SS$_ABORT; break;
            }
        }
        if (result == SS$_NORMAL && (fb_complete(b) < 0 || fb_flush(b) < 0))
            result = SS$_ABORT;
    }
    free(names);
    return result;
}

/* RENAME (spec 5.2.8): ACCESS(RENAME, old) + NAME(new) -> [NAME(old resultant)
 * ACK NAME(new resultant) ACK] ACCESS COMPLETE(RESPONSE), or STATUS -- the
 * VAX FAL's reply, one segment. $RENAME (the executive ACP's atomic MOVE) runs
 * as this process. A wildcard rename (spec 5.2.20.3, go/no-go) is not served. */
static uint32_t server_rename(struct fal_batch *b, struct dnet_dap_transport *t,
                              const struct dnet_dap_msg *acc)
{
    struct dnet_dap_msg nm;
    if (fal_recv(t, &nm) < 0) return SS$_ABORT;
    if (nm.op != DNET_DAP_NAME) {
        struct dnet_dap_msg s;
        memset(&s, 0, sizeof s);
        s.op = DNET_DAP_STATUS;
        s.u.status.stscode = (uint16_t)((DNET_DAP_MAC_SYNC << 12) | (nm.type & 0xfff));
        (void)fb_add(b, &s);
        (void)fb_flush(b);
        return SS$_ABORT;
    }
    const char *oldspec = acc->u.access.filespec;
    const char *newspec = nm.u.name.namespec;
    int want_name = acc->u.access.have_display && (acc->u.access.display & DNET_DAP_DSP_NAME);
    if (spec_is_wild(oldspec) || spec_is_wild(newspec)) {
        struct dnet_dap_msg s;
        memset(&s, 0, sizeof s);
        s.op = DNET_DAP_STATUS;
        s.u.status.stscode = (uint16_t)(DNET_DAP_MAC_UNSUPP << 12);
        if (fb_add(b, &s) < 0 || fb_flush(b) < 0) return SS$_ABORT;
        return SS$_BADPARAM;
    }
    char orsa[DNET_DAP_MAX_SPEC + 1] = "", nrsa[DNET_DAP_MAX_SPEC + 1] = "";
    char target[DNET_DAP_MAX_SPEC + 1];
    uint32_t sts = 0, stv = 0;
    if (resolve_one(oldspec, orsa, sizeof orsa) != 0) orsa[0] = '\0';
    snprintf(target, sizeof target, "%s", newspec);
    if (orsa[0]) {
        /* The new name's device and directory default from the old file's
         * (VMS $RENAME), and a new spec naming the SAME device:[directory]
         * the accessor named the old file by -- a search list such as
         * SYS$SYSROOT:[SYSMGR] -- means the directory the old file was FOUND
         * in: the VAX FAL renamed SYS$SYSROOT:[SYSMGR]RENME.TXT;1 to
         * SYS$SYSROOT:[SYSMGR]RENAMED.TXT;1 in place. Both are taken from the
         * RMS resultant, never composed from a guess. */
        char ov[DNET_DAP_MAX_SPEC + 1], od[DNET_DAP_MAX_SPEC + 1], of[DNET_DAP_MAX_SPEC + 1];
        char nv[DNET_DAP_MAX_SPEC + 1], nd[DNET_DAP_MAX_SPEC + 1], nf[DNET_DAP_MAX_SPEC + 1];
        char rv[DNET_DAP_MAX_SPEC + 1], rd[DNET_DAP_MAX_SPEC + 1], rf[DNET_DAP_MAX_SPEC + 1];
        spec_split(oldspec, ov, od, of, sizeof ov);
        spec_split(newspec, nv, nd, nf, sizeof nv);
        spec_split(orsa, rv, rd, rf, sizeof rv);
        int same = (!nv[0] && !nd[0]) || (!strcmp(nv, ov) && !strcmp(nd, od));
        if (same && strlen(rv) + strlen(rd) + strlen(nf) <= DNET_DAP_MAX_SPEC)
            snprintf(target, sizeof target, "%s%s%s", rv, rd, nf);
    }
    if (dnet_fal_rename(orsa[0] ? orsa : oldspec, target, &sts, &stv) != 0)
        return fb_refuse(b, FV_RENAME, sts);
    if (want_name) {
        if (orsa[0] && (fb_name(b, DNET_DAP_NT_FILESPEC, orsa) < 0 ||
                        fb_simple(b, DNET_DAP_ACKNOWLEDGE) < 0))
            return SS$_ABORT;
        /* The new resultant is what RMS now finds under the new name. */
        if (resolve_one(target, nrsa, sizeof nrsa) == 0 &&
            (fb_name(b, DNET_DAP_NT_FILESPEC, nrsa) < 0 ||
             fb_simple(b, DNET_DAP_ACKNOWLEDGE) < 0))
            return SS$_ABORT;
    }
    return (fb_complete(b) < 0 || fb_flush(b) < 0) ? SS$_ABORT : SS$_NORMAL;
}

uint32_t dnet_fal_server_run(struct dnet_dap_transport *t)
{
    if (!t || !t->send || !t->recv) return SS$_ABORT;
    t->rxlen = t->rxoff = 0;

    /* CONFIGURATION: the accessed process waits for the accessor's first
     * (spec 5.1), then answers with its own. */
    struct dnet_dap_msg m;
    if (fal_recv(t, &m) < 0 || m.op != DNET_DAP_CONFIG) return SS$_ABORT;
    if (send_config(t) < 0) return SS$_ABORT;

    static struct fal_batch batch;          /* one FAL session per process   */
    struct fal_batch *b = &batch;
    memset(b, 0, sizeof *b);
    b->t = t;
    b->cap = FAL_BUFSIZ;
    if (m.u.config.bufsiz && m.u.config.bufsiz < b->cap) b->cap = m.u.config.bufsiz;

    /* Accesses follow one another on the link (spec 5.1: a VMS COPY lists its
     * input with a DIRECTORY LIST access, then OPENs it, on one link). Serve
     * until the peer disconnects; the result is the last access's status. */
    uint32_t result = SS$_ABORT;
    int have_attr = 0;
    uint8_t peer_org = DNET_DAP_ORG_SEQ, peer_rfm = DNET_DAP_RFM_VAR, peer_rat = DNET_DAP_RAT_CR;
    for (;;) {
        if (fal_recv(t, &m) < 0) return result;          /* link closed: done */
        switch (m.op) {
        case DNET_DAP_ATTRIBUTES:
            have_attr = 1;
            peer_org = (m.u.attr.menu & (1u << DNET_DAP_ATT_ORG)) ? m.u.attr.org
                                                                  : DNET_DAP_ORG_SEQ;
            /* Record format / attributes for a CREATE: the accessor's, within
             * what OVMX RMS writes (FIX/VAR/STM*); otherwise VAR + CR. */
            peer_rfm = ((m.u.attr.menu & (1u << DNET_DAP_ATT_RFM)) &&
                        m.u.attr.rfm >= DNET_DAP_RFM_FIX && m.u.attr.rfm <= 6)
                           ? m.u.attr.rfm : DNET_DAP_RFM_VAR;
            peer_rat = (m.u.attr.menu & (1u << DNET_DAP_ATT_RAT))
                           ? (uint8_t)(m.u.attr.rat & 0x0f) : DNET_DAP_RAT_CR;
            continue;
        case DNET_DAP_KEYDEF: case DNET_DAP_ALLOC: case DNET_DAP_SUMMARY:
        case DNET_DAP_DATETIME: case DNET_DAP_PROTECTION: case DNET_DAP_ACL:
        case DNET_DAP_NAME:
            continue;
        case DNET_DAP_ACCESS_COMPLETE:                   /* closing an access we */
            if (send_complete(t, DNET_DAP_CMP_RESPONSE) < 0) return SS$_ABORT;
            continue;                                    /* already finished     */
        case DNET_DAP_ACCESS:
            break;
        default:
            (void)send_status(t, (DNET_DAP_MAC_SYNC << 12) | (m.type & 0xfff), 0);
            return SS$_ABORT;
        }

        /* DIRECTORY LIST, ERASE and RENAME need no ATTRIBUTES first (spec
         * 5.1.2 note 1) and are answered in one blocked segment. */
        if (m.u.access.accfunc == DNET_DAP_ACC_DIRLIST ||
            m.u.access.accfunc == DNET_DAP_ACC_ERASE ||
            m.u.access.accfunc == DNET_DAP_ACC_RENAME) {
            have_attr = 0;
            if (m.u.access.accfunc == DNET_DAP_ACC_DIRLIST)     result = server_dirlist(b, &m);
            else if (m.u.access.accfunc == DNET_DAP_ACC_ERASE)  result = server_erase(b, &m);
            else                                                result = server_rename(b, t, &m);
            if (result == SS$_ABORT) return result;
            continue;
        }

        /* OPEN / CREATE: the spec requires ATTRIBUTES first (a real VMS FAL
         * answers a bare ACCESS with STATUS sync/ACCESS -- observed, rd vms-a8a). */
        if (!have_attr) {
            if (send_status(t, (DNET_DAP_MAC_SYNC << 12) | DNET_DAP_ACCESS, 0) < 0)
                return SS$_ABORT;
            continue;
        }
        have_attr = 0;
        const char *spec = m.u.access.filespec;
        int want_name = m.u.access.have_display && (m.u.access.display & DNET_DAP_DSP_NAME);
        struct dnet_dap_msg a;
        if (m.u.access.accfunc == DNET_DAP_ACC_OPEN) {
            void *rf = NULL;
            uint8_t frfm = DNET_DAP_RFM_VAR, frat = DNET_DAP_RAT_CR;
            uint32_t osts = RMS$_FNF;
            if (dnet_fal_ropen_st(spec, &rf, &frfm, &frat, &osts) != 0) {
                /* The executive ACP refused or found nothing: the VAX FAL's
                 * STATUS for both is FNF + STV 0x0910 (rms_status_msg). */
                struct dnet_dap_msg s;
                rms_status_msg(FV_OPEN, osts, &s);
                if (fal_send(t, &s) < 0) return SS$_ABORT;
                result = SS$_NOSUCHFILE;
                continue;
            }
            (void)dnet_fal_rclose(rf);
            char rsa[DNET_DAP_MAX_SPEC + 1];
            int have_rsa = (resolve_one(spec, rsa, sizeof rsa) == 0);
            text_attributes(&a);
            /* The file's REAL record format and attributes, as RMS opened it. */
            if (frfm >= DNET_DAP_RFM_FIX && frfm <= 6) a.u.attr.rfm = frfm;
            a.u.attr.rat = (uint8_t)(frat & 0x0f);
            if (fal_send(t, &a) < 0) return SS$_ABORT;
            /* Extended attributes the accessor asked for (a VMS TYPE asks for
             * DATE AND TIME), from the opened file's header. */
            uint64_t disp = m.u.access.have_display ? m.u.access.display : 0;
            if (disp & (DNET_DAP_DSP_SUMMARY | DNET_DAP_DSP_DATETIME | DNET_DAP_DSP_PROT)) {
                struct dnet_fal_fattr fa;
                if (dnet_fal_fileattr(have_rsa ? rsa : spec, &fa, NULL) == 0) {
                    if (fb_attrs(b, disp & ~(uint64_t)DNET_DAP_DSP_MAIN, &fa) < 0 ||
                        fb_flush(b) < 0)
                        return SS$_ABORT;
                }
            }
            if (want_name && have_rsa && send_name(t, DNET_DAP_NT_FILESPEC, rsa) < 0)
                return SS$_ABORT;
            if (send_simple(t, DNET_DAP_ACKNOWLEDGE) < 0) return SS$_ABORT;
            result = server_open_phase(t, spec);
        } else if (m.u.access.accfunc == DNET_DAP_ACC_CREATE) {
            if (peer_org != DNET_DAP_ORG_SEQ) {
                /* Only sequential organisation is advertised (INV-6). */
                if (send_status(t, (DNET_DAP_MAC_UNSUPP << 12) | 0, 0) < 0) return SS$_ABORT;
                result = SS$_ABORT;
                continue;
            }
            /* $CREATE now, with the accessor's record format, so a requested
             * NAME is the resultant spec of the file that really exists. */
            void *h = NULL;
            char rsa[DNET_DAP_MAX_SPEC + 1] = "";
            if (dnet_fal_wopen(spec, peer_rfm, peer_rat, &h, rsa, sizeof rsa) != 0) {
                if (send_status(t, (DNET_DAP_MAC_OPEN << 12) | DNET_DAP_MIC_CRE, 0) < 0)
                    return SS$_ABORT;
                result = SS$_ABORT;
                continue;
            }
            /* $CREATE returns the resultant in the NAM (rd vms-98e). A VMS
             * COPY that asked for NAME rejects an ACK without it (RMS-F-BUG_DAP,
             * DAP code 0001A006 = MAC 10 sync / MIC ACK, observed against a
             * booted OVMX 2026-10-05), so no resultant is an honest refusal. */
            if (want_name && !rsa[0]) {
                (void)dnet_fal_wclose(h);
                if (send_status(t, (DNET_DAP_MAC_OPEN << 12) | DNET_DAP_MIC_CRE, 0) < 0)
                    return SS$_ABORT;
                result = SS$_ABORT;
                continue;
            }
            text_attributes(&a);
            a.u.attr.rfm = peer_rfm;
            a.u.attr.rat = peer_rat;
            if (fal_send(t, &a) < 0) { (void)dnet_fal_wclose(h); return SS$_ABORT; }
            if (want_name && rsa[0] && send_name(t, DNET_DAP_NT_FILESPEC, rsa) < 0) {
                (void)dnet_fal_wclose(h); return SS$_ABORT;
            }
            if (send_simple(t, DNET_DAP_ACKNOWLEDGE) < 0) { (void)dnet_fal_wclose(h); return SS$_ABORT; }
            result = server_create_phase(t, h);
        } else {
            if (send_status(t, (DNET_DAP_MAC_UNSUPP << 12) | 0, 0) < 0) return SS$_ABORT;
            result = SS$_ABORT;
            continue;
        }
        if (result == SS$_ABORT) return result;
    }
}

/* ---- FAL CLIENT (the COPY node:: driver) ---------------------------------- */

/* Setup an access: ATTRIBUTES + ACCESS, then the remote's reply up to ACK,
 * then CONTROL(CONNECT) -> ACK. Returns SS$_NORMAL or the remote condition. */
static uint32_t client_setup(struct dnet_dap_transport *t, uint8_t accfunc,
                             const char *remote_spec, uint64_t fac)
{
    struct dnet_dap_msg m;
    uint32_t cond = SS$_ABORT;
    text_attributes(&m);
    if (fal_send(t, &m) < 0) return SS$_ABORT;

    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_ACCESS;
    m.u.access.accfunc = accfunc;
    if (strlen(remote_spec) > DNET_DAP_MAX_SPEC) return SS$_BADPARAM;
    strncpy(m.u.access.filespec, remote_spec, sizeof m.u.access.filespec - 1);
    m.u.access.have_fac = 1;     m.u.access.fac = fac;
    m.u.access.have_shr = 1;     m.u.access.shr = (fac == DNET_DAP_FB_GET) ? DNET_DAP_FB_GET : 0;
    m.u.access.have_display = 1; m.u.access.display = DNET_DAP_DSP_MAIN;
    if (fal_send(t, &m) < 0) return SS$_ABORT;
    if (await_ack(t, &cond)) return cond;

    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_CONTROL;
    m.u.control.ctlfunc = DNET_DAP_CTL_CONNECT;
    if (fal_send(t, &m) < 0) return SS$_ABORT;
    if (await_ack(t, &cond)) return cond;
    return SS$_NORMAL;
}

static int send_control_xfer(struct dnet_dap_transport *t, uint8_t func)
{
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_CONTROL;
    m.u.control.ctlfunc = func;
    m.u.control.menu = DNET_DAP_CTLM_RAC;
    m.u.control.rac = DNET_DAP_RAC_SEQFILE;
    return fal_send(t, &m);
}

/* CLOSE and wait for the RESPONSE (or a STATUS carrying the failure). */
static uint32_t client_close(struct dnet_dap_transport *t)
{
    struct dnet_dap_msg m;
    if (send_complete(t, DNET_DAP_CMP_CLOSE) < 0) return SS$_ABORT;
    for (;;) {
        if (fal_recv(t, &m) < 0) return SS$_ABORT;
        if (m.op == DNET_DAP_ACCESS_COMPLETE && m.u.complete.cmpfunc == DNET_DAP_CMP_RESPONSE)
            return SS$_NORMAL;
        if (m.op == DNET_DAP_STATUS) return status_to_cond(&m);
        return SS$_ABORT;
    }
}

uint32_t dnet_fal_client_put(const char *local_spec, const char *remote_spec,
                             struct dnet_dap_transport *t)
{
    if (!t || !t->send || !t->recv || !local_spec || !remote_spec) return SS$_ABORT;
    t->rxlen = t->rxoff = 0;

    void *rf = NULL;
    if (dnet_fal_ropen(local_spec, &rf, NULL, NULL) != 0) return SS$_NOSUCHFILE;

    if (config_exchange_client(t) < 0) { (void)dnet_fal_rclose(rf); return SS$_ABORT; }
    uint32_t st = client_setup(t, DNET_DAP_ACC_CREATE, remote_spec, DNET_DAP_FB_PUT);
    if (st != SS$_NORMAL) { (void)dnet_fal_rclose(rf); return st; }
    if (send_control_xfer(t, DNET_DAP_CTL_PUT) < 0) { (void)dnet_fal_rclose(rf); return SS$_ABORT; }

    uint8_t rec[DNET_DAP_MAX_REC];
    size_t rlen = 0;
    int g;
    while ((g = dnet_fal_rget(rf, rec, sizeof rec, &rlen)) == 1) {
        if (send_record_n(t, rec, rlen) < 0) { (void)dnet_fal_rclose(rf); return SS$_ABORT; }
    }
    (void)dnet_fal_rclose(rf);
    if (g < 0) return SS$_ABORT;      /* a local read error is not a clean EOF */
    return client_close(t);
}

uint32_t dnet_fal_client_get(const char *remote_spec, const char *local_spec,
                             struct dnet_dap_transport *t)
{
    if (!t || !t->send || !t->recv || !local_spec || !remote_spec) return SS$_ABORT;
    t->rxlen = t->rxoff = 0;

    if (config_exchange_client(t) < 0) return SS$_ABORT;
    uint32_t st = client_setup(t, DNET_DAP_ACC_OPEN, remote_spec, DNET_DAP_FB_GET);
    if (st != SS$_NORMAL) return st;
    if (send_control_xfer(t, DNET_DAP_CTL_GET) < 0) return SS$_ABORT;

    /* The local copy: one RMS stream, records verbatim (embedded NULs kept). */
    void *h = NULL;
    if (dnet_fal_wopen(local_spec, DNET_DAP_RFM_VAR, DNET_DAP_RAT_CR, &h, NULL, 0) != 0) {
        (void)client_close(t);
        return SS$_ABORT;
    }
    struct dnet_dap_msg m;
    for (;;) {
        if (fal_recv(t, &m) < 0) { (void)dnet_fal_wclose(h); return SS$_ABORT; }
        if (m.op == DNET_DAP_DATA) {
            if (dnet_fal_wput(h, m.u.data.rec, m.u.data.reclen) != 0) {
                (void)dnet_fal_wclose(h);
                (void)client_close(t);
                return SS$_ABORT;
            }
            continue;
        }
        if (m.op == DNET_DAP_STATUS) {
            uint16_t sts = m.u.status.stscode;
            if (DNET_DAP_MAC(sts) == DNET_DAP_MAC_XFER && DNET_DAP_MIC(sts) == DNET_DAP_MIC_EOF)
                break;
            st = status_to_cond(&m);
            (void)dnet_fal_wclose(h);
            (void)client_close(t);
            return st;
        }
        (void)dnet_fal_wclose(h);
        return SS$_ABORT;
    }
    if (dnet_fal_wclose(h) != 0) { (void)client_close(t); return SS$_ABORT; }
    return client_close(t);
}

/* ---- DELETE / RENAME client (rd vms-277a) ----------------------------------
 * ACCESS(ERASE | RENAME) with DISPLAY = NAME, the RENAME's NAME(new) after it,
 * then the remote's reply: NAME / ACK until ACCESS COMPLETE(RESPONSE), or a
 * STATUS whose exact STSCODE / STV the caller gets back. */
static uint32_t client_fileop(struct dnet_dap_transport *t, uint8_t accfunc,
                              const char *spec, const char *newspec,
                              uint16_t *stscode, uint64_t *stv)
{
    if (stscode) *stscode = 0;
    if (stv) *stv = 0;
    if (!t || !t->send || !t->recv || !spec) return SS$_ABORT;
    if (strlen(spec) > DNET_DAP_MAX_SPEC || (newspec && strlen(newspec) > DNET_DAP_MAX_SPEC))
        return SS$_BADPARAM;
    t->rxlen = t->rxoff = 0;
    if (config_exchange_client(t) < 0) return SS$_ABORT;

    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_ACCESS;
    m.u.access.accfunc = accfunc;
    strncpy(m.u.access.filespec, spec, sizeof m.u.access.filespec - 1);
    m.u.access.have_display = 1;
    m.u.access.display = DNET_DAP_DSP_NAME;
    if (fal_send(t, &m) < 0) return SS$_ABORT;
    if (newspec && send_name(t, DNET_DAP_NT_FILESPEC, newspec) < 0) return SS$_ABORT;
    for (;;) {
        if (fal_recv(t, &m) < 0) return SS$_ABORT;
        if (m.op == DNET_DAP_NAME || m.op == DNET_DAP_ACKNOWLEDGE) continue;
        if (m.op == DNET_DAP_ACCESS_COMPLETE && m.u.complete.cmpfunc == DNET_DAP_CMP_RESPONSE)
            return SS$_NORMAL;
        if (m.op == DNET_DAP_STATUS) {
            if (stscode) *stscode = m.u.status.stscode;
            if (stv && m.u.status.have_stv) *stv = m.u.status.stv;
        }
        return SS$_ABORT;
    }
}

uint32_t dnet_fal_client_erase(const char *remote_spec, struct dnet_dap_transport *t,
                               uint16_t *stscode, uint64_t *stv)
{
    return client_fileop(t, DNET_DAP_ACC_ERASE, remote_spec, NULL, stscode, stv);
}

uint32_t dnet_fal_client_rename(const char *old_spec, const char *new_spec,
                                struct dnet_dap_transport *t,
                                uint16_t *stscode, uint64_t *stv)
{
    if (!new_spec) return SS$_BADPARAM;
    return client_fileop(t, DNET_DAP_ACC_RENAME, old_spec, new_spec, stscode, stv);
}
