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
#include <string.h>

#include "sysuaf.h"         /* the ONE faithful authenticator (Purdy)        */
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

/* Split a resultant "DEV:[DIR]NAME.TYP;V" after its directory (or device). */
static size_t name_split(const char *rsa)
{
    const char *rb = strrchr(rsa, ']');
    if (!rb) rb = strrchr(rsa, '>');
    if (!rb) rb = strrchr(rsa, ':');
    return rb ? (size_t)(rb - rsa + 1) : 0;
}

/* DIRECTORY LIST (spec 5.2.11): for every file the spec (wildcards allowed)
 * resolves to, NAME(directory) when the directory changes, NAME(file), and the
 * main ATTRIBUTES if DISPLAY asks; then ACCESS COMPLETE(RESPONSE). Names are
 * the RESULTANT specs RMS $SEARCH returns (dnet_fal_search_*), never composed
 * here. A real VMS COPY lists its input this way before opening it. */
static uint32_t server_dirlist(struct dnet_dap_transport *t, const struct dnet_dap_msg *acc)
{
    void *ctx = NULL;
    char rsa[DNET_DAP_MAX_SPEC + 1], lastdir[DNET_DAP_MAX_SPEC + 1] = "";
    int n = 0;
    if (dnet_fal_search_begin(acc->u.access.filespec, &ctx) == 0) {
        while (dnet_fal_search_next(ctx, rsa, sizeof rsa) == 0) {
            size_t cut = name_split(rsa);
            char dir[DNET_DAP_MAX_SPEC + 1];
            snprintf(dir, sizeof dir, "%.*s", (int)cut, rsa);
            if (strcmp(dir, lastdir) != 0) {
                if (send_name(t, DNET_DAP_NT_DIRECTORY, dir) < 0) { dnet_fal_search_end(ctx); return SS$_ABORT; }
                snprintf(lastdir, sizeof lastdir, "%s", dir);
            }
            if (send_name(t, DNET_DAP_NT_FILENAME, rsa + cut) < 0) { dnet_fal_search_end(ctx); return SS$_ABORT; }
            if (acc->u.access.have_display && (acc->u.access.display & DNET_DAP_DSP_MAIN)) {
                struct dnet_dap_msg a;
                text_attributes(&a);
                if (fal_send(t, &a) < 0) { dnet_fal_search_end(ctx); return SS$_ABORT; }
            }
            n++;
        }
        dnet_fal_search_end(ctx);
    }
    if (n == 0)
        return (send_status(t, (DNET_DAP_MAC_OPEN << 12) | DNET_DAP_MIC_FNF, 0) < 0)
                   ? SS$_ABORT : SS$_NOSUCHFILE;
    return (send_complete(t, DNET_DAP_CMP_RESPONSE) < 0) ? SS$_ABORT : SS$_NORMAL;
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

uint32_t dnet_fal_server_run(struct dnet_dap_transport *t)
{
    if (!t || !t->send || !t->recv) return SS$_ABORT;
    t->rxlen = t->rxoff = 0;

    /* CONFIGURATION: the accessed process waits for the accessor's first
     * (spec 5.1), then answers with its own. */
    struct dnet_dap_msg m;
    if (fal_recv(t, &m) < 0 || m.op != DNET_DAP_CONFIG) return SS$_ABORT;
    if (send_config(t) < 0) return SS$_ABORT;

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


        if (m.u.access.accfunc == DNET_DAP_ACC_DIRLIST) {
            have_attr = 0;
            result = server_dirlist(t, &m);
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
            if (dnet_fal_ropen(spec, &rf, &frfm, &frat) != 0) {
                if (send_status(t, (DNET_DAP_MAC_OPEN << 12) | DNET_DAP_MIC_FNF, 0) < 0)
                    return SS$_ABORT;
                result = SS$_NOSUCHFILE;
                continue;
            }
            (void)dnet_fal_rclose(rf);
            char rsa[DNET_DAP_MAX_SPEC + 1];
            text_attributes(&a);
            /* The file's REAL record format and attributes, as RMS opened it. */
            if (frfm >= DNET_DAP_RFM_FIX && frfm <= 6) a.u.attr.rfm = frfm;
            a.u.attr.rat = (uint8_t)(frat & 0x0f);
            if (fal_send(t, &a) < 0) return SS$_ABORT;
            if (want_name && resolve_one(spec, rsa, sizeof rsa) == 0 &&
                send_name(t, DNET_DAP_NT_FILESPEC, rsa) < 0)
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
