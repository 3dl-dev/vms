/*
 * dnet_fal.c - the DECnet FILE ACCESS LISTENER (object 17) server + the COPY
 * node:: client (rd vms-8c2). See dnet_fal.h for the security argument and the
 * layer boundary. Two facts govern every line here:
 *   - AUTH IS REAL: the connect-carried username+password go through
 *     sysuaf_lookup + sysuaf_authenticate (Purdy) + the disabled-account gate,
 *     the SAME path LOGINOUT/SSHD use. A bad password is refused; a fake would
 *     pass it. (oracle docs/oracle/vax-copy-fal-dap.md §1.)
 *   - FILE I/O IS REAL: records move through rms_textfile_* -- RMS over the
 *     ODS-2 executive ACP -- never a raw POSIX file (Rule 9 / INV-6). No
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
 * RECORD SCOPE (INV-6): records move as text lines through rms_textfile_*; a
 * received record carrying an embedded NUL cannot be stored faithfully that
 * way, so the transfer is REFUSED (never silently truncated). Binary/indexed/
 * relative files and block mode are not advertised and not served.
 */
#include "dnet_fal.h"
#include "dnet_cterm.h"     /* dnet_fal_access_decode (bounded cred decoder) */

#include <stdio.h>
#include <string.h>

#include "sysuaf.h"         /* the ONE faithful authenticator (Purdy)        */
#include "rms_textfile.h"   /* RMS over the ACP -- real file I/O             */
#include "ssdef.h"

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

/* STATUS. A real VMS FAL also carries the system condition in STV (lab:
 * FNF -> STV 0x0910 = real SS$_NOSUCHFILE); OVMX sends NO STV until its own
 * ssdef values match real VMS (rd vms-ef2: OVMX SS$_NOSUCHFILE is 2320, which
 * real VMS reads as NOSUCHOBJECT) -- an honest omission, not a wrong code. */
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
 * sequential, variable-length records, implied CR carriage control -- what
 * rms_textfile_* reads and writes. */
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

/* A record is storable as a text line iff it carries no NUL. */
static int rec_to_line(const struct dnet_dap_msg *m, char *line, size_t cap)
{
    size_t n = m->u.data.reclen;
    if (n + 1 > cap) return -1;
    if (n && memchr(m->u.data.rec, 0, n)) return -1;
    if (n) memcpy(line, m->u.data.rec, n);
    line[n] = '\0';
    return 0;
}

/* Serve an opened file (GET). Returns SS$_NORMAL after ACCESS COMPLETE. */
static uint32_t server_open_phase(struct dnet_dap_transport *t, const char *spec)
{
    struct dnet_dap_msg m;
    for (;;) {
        if (fal_recv(t, &m) < 0) return SS$_ABORT;
        if (m.op == DNET_DAP_CONTROL && m.u.control.ctlfunc == DNET_DAP_CTL_CONNECT) {
            if (send_simple(t, DNET_DAP_ACKNOWLEDGE) < 0) return SS$_ABORT;
            continue;
        }
        if (m.op == DNET_DAP_CONTROL && m.u.control.ctlfunc == DNET_DAP_CTL_GET) {
            if (!(m.u.control.menu & DNET_DAP_CTLM_RAC) ||
                m.u.control.rac != DNET_DAP_RAC_SEQFILE) {
                /* Only sequential FILE TRANSFER is advertised and served. */
                if (send_status(t, (DNET_DAP_MAC_UNSUPP << 12) | 0, 0) < 0) return SS$_ABORT;
                continue;
            }
            rms_textfile_t *tf = rms_textfile_open(spec);
            if (!tf) {
                if (send_status(t, (DNET_DAP_MAC_XFER << 12) | DNET_DAP_MIC_FNF, 0) < 0) return SS$_ABORT;
                continue;
            }
            char line[DNET_DAP_MAX_REC + 1];
            int too_long = 0;
            while (rms_textfile_getline(tf, line, sizeof line, &too_long)) {
                struct dnet_dap_msg d;
                memset(&d, 0, sizeof d);
                d.op = DNET_DAP_DATA;
                size_t n = strlen(line);
                if (n > DNET_DAP_MAX_REC) n = DNET_DAP_MAX_REC;
                d.u.data.reclen = (uint16_t)n;
                if (n) memcpy(d.u.data.rec, line, n);
                if (fal_send(t, &d) < 0) { rms_textfile_close(tf); return SS$_ABORT; }
            }
            rms_textfile_close(tf);
            if (send_status(t, DNET_DAP_STS_EOF, 0) < 0) return SS$_ABORT;
            continue;
        }
        if (m.op == DNET_DAP_ACCESS_COMPLETE) {
            if (send_complete(t, DNET_DAP_CMP_RESPONSE) < 0) return SS$_ABORT;
            if (m.u.complete.cmpfunc == DNET_DAP_CMP_EOS) continue;   /* stream only */
            return SS$_NORMAL;
        }
        (void)send_status(t, (DNET_DAP_MAC_SYNC << 12) | (m.type & 0xfff), 0);
        return SS$_ABORT;
    }
}

/* Store a created file (PUT). Records are written as they arrive. */
static uint32_t server_create_phase(struct dnet_dap_transport *t, const char *spec)
{
    struct dnet_dap_msg m;
    int putting = 0, wrote_any = 0;
    for (;;) {
        if (fal_recv(t, &m) < 0) return SS$_ABORT;
        if (m.op == DNET_DAP_CONTROL && m.u.control.ctlfunc == DNET_DAP_CTL_CONNECT) {
            if (send_simple(t, DNET_DAP_ACKNOWLEDGE) < 0) return SS$_ABORT;
            continue;
        }
        if (m.op == DNET_DAP_CONTROL && m.u.control.ctlfunc == DNET_DAP_CTL_PUT) {
            if (!(m.u.control.menu & DNET_DAP_CTLM_RAC) ||
                m.u.control.rac != DNET_DAP_RAC_SEQFILE) {
                if (send_status(t, (DNET_DAP_MAC_UNSUPP << 12) | 0, 0) < 0) return SS$_ABORT;
                continue;
            }
            putting = 1;
            continue;
        }
        if (m.op == DNET_DAP_DATA) {
            char line[DNET_DAP_MAX_REC + 1];
            if (!putting || rec_to_line(&m, line, sizeof line) != 0) {
                (void)send_status(t, (DNET_DAP_MAC_XFER << 12) | 0, 0);
                return SS$_ABORT;
            }
            int st = wrote_any ? rms_textfile_append_line(spec, line)
                               : rms_textfile_write_line(spec, line);
            if (st != 0) {
                (void)send_status(t, (DNET_DAP_MAC_XFER << 12) | DNET_DAP_MIC_CRE, 0);
                return SS$_ABORT;
            }
            wrote_any = 1;
            continue;
        }
        if (m.op == DNET_DAP_ACCESS_COMPLETE) {
            if (!wrote_any && m.u.complete.cmpfunc != DNET_DAP_CMP_PURGE &&
                rms_textfile_write_line(spec, "") != 0) {
                (void)send_status(t, (DNET_DAP_MAC_TERM << 12) | DNET_DAP_MIC_CRE, 0);
                return SS$_ABORT;
            }
            if (send_complete(t, DNET_DAP_CMP_RESPONSE) < 0) return SS$_ABORT;
            if (m.u.complete.cmpfunc == DNET_DAP_CMP_EOS) { putting = 0; continue; }
            return SS$_NORMAL;
        }
        (void)send_status(t, (DNET_DAP_MAC_SYNC << 12) | (m.type & 0xfff), 0);
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

uint32_t dnet_fal_server_run(struct dnet_dap_transport *t)
{
    if (!t || !t->send || !t->recv) return SS$_ABORT;
    t->rxlen = t->rxoff = 0;

    /* CONFIGURATION: the accessed process waits for the accessor's first
     * (spec 5.1), then answers with its own. */
    struct dnet_dap_msg m;
    if (fal_recv(t, &m) < 0 || m.op != DNET_DAP_CONFIG) return SS$_ABORT;
    if (send_config(t) < 0) return SS$_ABORT;

    /* Setup: ATTRIBUTES [ext] [NAME] ACCESS. ONE access per link at this rung
     * (a completed transfer or an honest refusal ends the session): the spec
     * also allows further accesses on the same link (wildcard copies), which
     * is a filed follow-on, not served. */
    uint32_t result;
    int have_attr = 0;
    uint8_t peer_org = DNET_DAP_ORG_SEQ;
    for (;;) {
        if (fal_recv(t, &m) < 0) return SS$_ABORT;
        switch (m.op) {
        case DNET_DAP_ATTRIBUTES:
            have_attr = 1;
            peer_org = (m.u.attr.menu & (1u << DNET_DAP_ATT_ORG)) ? m.u.attr.org
                                                                  : DNET_DAP_ORG_SEQ;
            continue;
        case DNET_DAP_KEYDEF: case DNET_DAP_ALLOC: case DNET_DAP_SUMMARY:
        case DNET_DAP_DATETIME: case DNET_DAP_PROTECTION: case DNET_DAP_ACL:
        case DNET_DAP_NAME:
            continue;
        case DNET_DAP_ACCESS:
            break;
        default:
            (void)send_status(t, (DNET_DAP_MAC_SYNC << 12) | (m.type & 0xfff), 0);
            return SS$_ABORT;
        }

        /* ACCESS: the spec requires ATTRIBUTES first (a real VMS FAL answers a
         * bare ACCESS with STATUS sync/ACCESS -- observed, rd vms-a8a). */
        if (!have_attr) {
            if (send_status(t, (DNET_DAP_MAC_SYNC << 12) | DNET_DAP_ACCESS, 0) < 0)
                return SS$_ABORT;
            continue;
        }
        have_attr = 0;
        const char *spec = m.u.access.filespec;
        struct dnet_dap_msg a;
        if (m.u.access.accfunc == DNET_DAP_ACC_OPEN) {
            rms_textfile_t *tf = rms_textfile_open(spec);
            if (!tf) {
                if (send_status(t, (DNET_DAP_MAC_OPEN << 12) | DNET_DAP_MIC_FNF, 0) < 0) return SS$_ABORT;
                return SS$_NOSUCHFILE;
            }
            rms_textfile_close(tf);
            text_attributes(&a);
            if (fal_send(t, &a) < 0 || send_simple(t, DNET_DAP_ACKNOWLEDGE) < 0)
                return SS$_ABORT;
            result = server_open_phase(t, spec);
        } else if (m.u.access.accfunc == DNET_DAP_ACC_CREATE) {
            if (peer_org != DNET_DAP_ORG_SEQ) {
                /* Only sequential organisation is advertised (INV-6). */
                (void)send_status(t, (DNET_DAP_MAC_UNSUPP << 12) | 0, 0);
                return SS$_ABORT;
            }
            text_attributes(&a);
            if (fal_send(t, &a) < 0 || send_simple(t, DNET_DAP_ACKNOWLEDGE) < 0)
                return SS$_ABORT;
            result = server_create_phase(t, spec);
        } else {
            (void)send_status(t, (DNET_DAP_MAC_UNSUPP << 12) | 0, 0);
            return SS$_ABORT;
        }
        return result;
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

    rms_textfile_t *tf = rms_textfile_open(local_spec);
    if (!tf) return SS$_NOSUCHFILE;

    if (config_exchange_client(t) < 0) { rms_textfile_close(tf); return SS$_ABORT; }
    uint32_t st = client_setup(t, DNET_DAP_ACC_CREATE, remote_spec, DNET_DAP_FB_PUT);
    if (st != SS$_NORMAL) { rms_textfile_close(tf); return st; }
    if (send_control_xfer(t, DNET_DAP_CTL_PUT) < 0) { rms_textfile_close(tf); return SS$_ABORT; }

    char line[DNET_DAP_MAX_REC + 1];
    int too_long = 0;
    while (rms_textfile_getline(tf, line, sizeof line, &too_long)) {
        struct dnet_dap_msg d;
        memset(&d, 0, sizeof d);
        d.op = DNET_DAP_DATA;
        size_t n = strlen(line);
        if (n > DNET_DAP_MAX_REC) n = DNET_DAP_MAX_REC;
        d.u.data.reclen = (uint16_t)n;
        if (n) memcpy(d.u.data.rec, line, n);
        if (fal_send(t, &d) < 0) { rms_textfile_close(tf); return SS$_ABORT; }
    }
    rms_textfile_close(tf);
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

    struct dnet_dap_msg m;
    int wrote_any = 0;
    for (;;) {
        if (fal_recv(t, &m) < 0) return SS$_ABORT;
        if (m.op == DNET_DAP_DATA) {
            char line[DNET_DAP_MAX_REC + 1];
            if (rec_to_line(&m, line, sizeof line) != 0) {
                /* Binary record: not storable as text -- refuse, never truncate. */
                (void)send_complete(t, DNET_DAP_CMP_CLOSE);
                return SS$_BADPARAM;
            }
            int w = wrote_any ? rms_textfile_append_line(local_spec, line)
                              : rms_textfile_write_line(local_spec, line);
            if (w != 0) return SS$_ABORT;
            wrote_any = 1;
            continue;
        }
        if (m.op == DNET_DAP_STATUS) {
            uint16_t sts = m.u.status.stscode;
            if (DNET_DAP_MAC(sts) == DNET_DAP_MAC_XFER && DNET_DAP_MIC(sts) == DNET_DAP_MIC_EOF)
                break;
            st = status_to_cond(&m);
            (void)client_close(t);
            return st;
        }
        return SS$_ABORT;
    }
    if (!wrote_any && rms_textfile_write_line(local_spec, "") != 0) return SS$_ABORT;
    return client_close(t);
}
