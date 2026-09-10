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
 * The DAP message SEQUENCE (CONFIGURATION -> ACCESS -> ATTRIBUTES/NAME ->
 * CONTROL -> DATA -> STATUS/ACCESS-COMPLETE) is the oracle's; the per-field DAP
 * framing is the public spec via dnet_dap.c (clean-room, Rule 8).
 */
#include "dnet_fal.h"
#include "dnet_cterm.h"     /* dnet_fal_access_decode (bounded cred decoder) */

#include <string.h>

#include "sysuaf.h"         /* the ONE faithful authenticator (Purdy)        */
#include "rms_textfile.h"   /* RMS over the ACP -- real file I/O             */
#include "ssdef.h"

/* OVMX CONFIGURATION identity bytes (public DAP spec fields; OVMX presents as a
 * VMS/RMS node). Values are OVMX-chosen within the spec and only meaningful
 * between two OVMX nodes at this rung. */
#define FAL_OSTYPE_VMS    0x01
#define FAL_FILESYS_RMS   0x01
#define FAL_DAP_VERSION   0x07   /* DAP root version 7 (public spec)          */
#define FAL_BUFSIZ        1459   /* matches the oracle's negotiated segsize    */

/* ---- authentication ------------------------------------------------------ */

uint32_t dnet_fal_authenticate(const char *username, const char *password)
{
    if (!username || !password || username[0] == '\0')
        return SS$_INVLOGIN;

    sysuaf_record_t rec;
    /* No-such-user and a wrong password both surface as SS$_INVLOGIN so the
     * peer cannot probe which usernames exist -- exactly as a real login does
     * not distinguish them. Fail-honest: no SYSUAF / no /dev/vms -> lookup
     * fails -> refuse (never fabricate a pass). */
    if (sysuaf_lookup(username, &rec) != 0)
        return SS$_INVLOGIN;
    if (!sysuaf_authenticate(&rec, password))
        return SS$_INVLOGIN;
    /* A real account with the right password but DISUSER/DISACNT is refused --
     * the disabled-account gate, same as the interactive/SSH paths. */
    if (!sysuaf_interactive_login_permitted(&rec))
        return SS$_NOPRIV;
    return SS$_NORMAL;
}

/* ---- small DAP send/recv helpers over the caller's transport ------------- */

static int fal_send(struct dnet_dap_transport *t, const struct dnet_dap_msg *m)
{
    return t->send(t->ctx, m);
}
static int fal_recv(struct dnet_dap_transport *t, struct dnet_dap_msg *m)
{
    return t->recv(t->ctx, m);
}

static int send_simple(struct dnet_dap_transport *t, enum dnet_dap_op op, uint8_t func)
{
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = op;
    m.u.complete.func = func;
    return fal_send(t, &m);
}

static int send_status(struct dnet_dap_transport *t, uint16_t code)
{
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_STATUS;
    m.u.status.stscode = code;
    return fal_send(t, &m);
}

static int send_config(struct dnet_dap_transport *t)
{
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_CONFIG;
    m.u.config.bufsiz  = FAL_BUFSIZ;
    m.u.config.ostype  = FAL_OSTYPE_VMS;
    m.u.config.filesys = FAL_FILESYS_RMS;
    m.u.config.version = FAL_DAP_VERSION;
    return fal_send(t, &m);
}

/* Exchange CONFIGURATION with the peer (both ends send + receive one). The
 * caller side (server vs client) sends first or receives first symmetrically;
 * here every endpoint sends then receives, which the transport orders. */
static int config_exchange(struct dnet_dap_transport *t, int send_first)
{
    struct dnet_dap_msg m;
    if (send_first) {
        if (send_config(t) < 0) return -1;
        if (fal_recv(t, &m) < 0 || m.op != DNET_DAP_CONFIG) return -1;
    } else {
        if (fal_recv(t, &m) < 0 || m.op != DNET_DAP_CONFIG) return -1;
        if (send_config(t) < 0) return -1;
    }
    return 0;
}

/* ---- FAL SERVER ---------------------------------------------------------- */

/* Serve a GET: read the local file and stream its records as DATA, then EOF. */
static uint32_t server_serve_get(struct dnet_dap_transport *t, const char *spec)
{
    rms_textfile_t *tf = rms_textfile_open(spec);
    if (!tf) {
        /* Honest miss -- the source is not there / not reachable over the ACP.
         * Tell the client and end the access cleanly (never crash it). */
        (void)send_status(t, DNET_DAP_STS_ACCFAIL);
        return SS$_NOSUCHFILE;
    }

    /* ATTRIBUTES then NAME (the resolved full spec) -- the oracle's attributes
     * exchange, carrying the values it fixed as ground truth. */
    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_ATTRIBUTES;
    m.u.attr.org = DNET_DAP_ORG_SEQ;
    m.u.attr.rfm = DNET_DAP_RFM_VAR;
    m.u.attr.rat = 0;
    m.u.attr.mrs = DNET_DAP_MAX_REC;
    m.u.attr.alq = 0;
    if (fal_send(t, &m) < 0) { rms_textfile_close(tf); return SS$_ABORT; }

    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_NAME;
    m.u.name.nametype = 1;   /* full file spec */
    strncpy(m.u.name.namespec, spec, sizeof m.u.name.namespec - 1);
    if (fal_send(t, &m) < 0) { rms_textfile_close(tf); return SS$_ABORT; }

    if (send_simple(t, DNET_DAP_ACKNOWLEDGE, 0) < 0) { rms_textfile_close(tf); return SS$_ABORT; }

    /* CONTROL GET from the client. */
    if (fal_recv(t, &m) < 0 || m.op != DNET_DAP_CONTROL ||
        m.u.control.ctlfunc != DNET_DAP_CTL_GET) {
        rms_textfile_close(tf);
        return SS$_ABORT;
    }

    /* Stream every record verbatim as a DATA message. */
    char line[DNET_DAP_MAX_REC];
    int too_long = 0;
    while (rms_textfile_getline(tf, line, sizeof line, &too_long)) {
        size_t n = strlen(line);
        if (n > DNET_DAP_MAX_REC) n = DNET_DAP_MAX_REC;
        memset(&m, 0, sizeof m);
        m.op = DNET_DAP_DATA;
        m.u.data.reclen = (uint16_t)n;
        if (n) memcpy(m.u.data.rec, line, n);
        if (fal_send(t, &m) < 0) { rms_textfile_close(tf); return SS$_ABORT; }
    }
    rms_textfile_close(tf);

    /* ACCESS-COMPLETE with an EOF indication, then the client's completion. */
    if (send_simple(t, DNET_DAP_ACCESS_COMPLETE, 1) < 0) return SS$_ABORT;
    if (fal_recv(t, &m) < 0) return SS$_ABORT;   /* client's ACCESS-COMPLETE */
    return SS$_NORMAL;
}

/* Serve a PUT: receive the client's records and store them via RMS. */
static uint32_t server_serve_put(struct dnet_dap_transport *t, const char *spec)
{
    if (send_simple(t, DNET_DAP_ACKNOWLEDGE, 0) < 0) return SS$_ABORT;

    struct dnet_dap_msg m;
    int have_control = 0;
    int wrote_any = 0;

    for (;;) {
        if (fal_recv(t, &m) < 0) return SS$_ABORT;
        if (m.op == DNET_DAP_ATTRIBUTES) {
            /* Only sequential, variable/stream records are served this rung;
             * anything else is refused honestly (INV-6 -- indexed/relative are
             * filed follow-ons, never faked). */
            if (m.u.attr.org != DNET_DAP_ORG_SEQ) {
                (void)send_status(t, DNET_DAP_STS_ACCFAIL);
                return SS$_ABORT;
            }
            continue;
        }
        if (m.op == DNET_DAP_CONTROL) {
            if (m.u.control.ctlfunc != DNET_DAP_CTL_PUT) {
                (void)send_status(t, DNET_DAP_STS_ACCFAIL);
                return SS$_ABORT;
            }
            have_control = 1;
            if (send_simple(t, DNET_DAP_ACKNOWLEDGE, 0) < 0) return SS$_ABORT;
            continue;
        }
        if (m.op == DNET_DAP_DATA) {
            if (!have_control) { (void)send_status(t, DNET_DAP_STS_ACCFAIL); return SS$_ABORT; }
            char rec[DNET_DAP_MAX_REC + 1];
            uint16_t n = m.u.data.reclen;
            if (n > DNET_DAP_MAX_REC) n = DNET_DAP_MAX_REC;
            if (n) memcpy(rec, m.u.data.rec, n);
            rec[n] = '\0';
            /* First record creates/supersedes; the rest append -- the file
             * lands on the ODS-2 volume through the ACP, record by record. */
            int st = wrote_any ? rms_textfile_append_line(spec, rec)
                               : rms_textfile_write_line(spec, rec);
            if (st != 0) { (void)send_status(t, DNET_DAP_STS_ACCFAIL); return SS$_ABORT; }
            wrote_any = 1;
            continue;
        }
        if (m.op == DNET_DAP_ACCESS_COMPLETE) {
            /* An empty source file is legal: create it now if no record came. */
            if (!wrote_any) {
                if (rms_textfile_write_line(spec, "") != 0) {
                    (void)send_status(t, DNET_DAP_STS_ACCFAIL);
                    return SS$_ABORT;
                }
            }
            return (send_status(t, DNET_DAP_STS_SUCCESS) < 0) ? SS$_ABORT : SS$_NORMAL;
        }
        /* Any other (or unknown) message ends the access honestly. */
        (void)send_status(t, DNET_DAP_STS_ACCFAIL);
        return SS$_ABORT;
    }
}

uint32_t dnet_fal_connect_auth(const uint8_t *conn_data, size_t conn_len,
                               char *authed_user, size_t authed_user_cap)
{
    if (authed_user && authed_user_cap) authed_user[0] = '\0';

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
    uint32_t auth = (drc != 0) ? SS$_INVLOGIN : dnet_fal_authenticate(user, pass);

    /* The password never outlives the check. */
    memset(pass, 0, sizeof pass);
    memset(acct, 0, sizeof acct);

    if (auth != SS$_NORMAL) {
        memset(user, 0, sizeof user);
        return auth;   /* caller sends the NSP disconnect; no CC, no DAP, no file */
    }
    if (authed_user && authed_user_cap) {
        strncpy(authed_user, user, authed_user_cap - 1);
        authed_user[authed_user_cap - 1] = '\0';
    }
    memset(user, 0, sizeof user);
    return SS$_NORMAL;
}

uint32_t dnet_fal_server_run(struct dnet_dap_transport *t)
{
    if (!t || !t->send || !t->recv) return SS$_ABORT;

    /* CONFIGURATION exchange (server receives first). The connect-time gate
     * (dnet_fal_connect_auth) has already authenticated and the caller has
     * accepted the link, so no credential is handled here. */
    if (config_exchange(t, 0) < 0) return SS$_ABORT;

    /* ACCESS -> branch on the function. */
    struct dnet_dap_msg m;
    if (fal_recv(t, &m) < 0 || m.op != DNET_DAP_ACCESS)
        return SS$_ABORT;

    if (m.u.access.accfunc == DNET_DAP_ACC_OPEN)
        return server_serve_get(t, m.u.access.filespec);
    if (m.u.access.accfunc == DNET_DAP_ACC_CREATE)
        return server_serve_put(t, m.u.access.filespec);

    (void)send_status(t, DNET_DAP_STS_ACCFAIL);
    return SS$_ABORT;
}

/* ---- FAL CLIENT (the COPY node:: driver) --------------------------------- */

uint32_t dnet_fal_client_put(const char *local_spec, const char *remote_spec,
                             struct dnet_dap_transport *t)
{
    if (!t || !t->send || !t->recv || !local_spec || !remote_spec) return SS$_ABORT;

    rms_textfile_t *tf = rms_textfile_open(local_spec);
    if (!tf) return SS$_NOSUCHFILE;

    if (config_exchange(t, 1) < 0) { rms_textfile_close(tf); return SS$_ABORT; }

    struct dnet_dap_msg m;

    /* ACCESS CREATE the remote file. */
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_ACCESS;
    m.u.access.accfunc = DNET_DAP_ACC_CREATE;
    strncpy(m.u.access.filespec, remote_spec, sizeof m.u.access.filespec - 1);
    if (fal_send(t, &m) < 0) { rms_textfile_close(tf); return SS$_ABORT; }
    if (fal_recv(t, &m) < 0 || m.op != DNET_DAP_ACKNOWLEDGE) { rms_textfile_close(tf); return SS$_ABORT; }

    /* ATTRIBUTES (sequential, variable). */
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_ATTRIBUTES;
    m.u.attr.org = DNET_DAP_ORG_SEQ;
    m.u.attr.rfm = DNET_DAP_RFM_VAR;
    m.u.attr.mrs = DNET_DAP_MAX_REC;
    if (fal_send(t, &m) < 0) { rms_textfile_close(tf); return SS$_ABORT; }

    /* CONTROL PUT. */
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_CONTROL;
    m.u.control.ctlfunc = DNET_DAP_CTL_PUT;
    if (fal_send(t, &m) < 0) { rms_textfile_close(tf); return SS$_ABORT; }
    if (fal_recv(t, &m) < 0 || m.op != DNET_DAP_ACKNOWLEDGE) { rms_textfile_close(tf); return SS$_ABORT; }

    /* DATA per record. */
    char line[DNET_DAP_MAX_REC];
    int too_long = 0;
    while (rms_textfile_getline(tf, line, sizeof line, &too_long)) {
        size_t n = strlen(line);
        if (n > DNET_DAP_MAX_REC) n = DNET_DAP_MAX_REC;
        memset(&m, 0, sizeof m);
        m.op = DNET_DAP_DATA;
        m.u.data.reclen = (uint16_t)n;
        if (n) memcpy(m.u.data.rec, line, n);
        if (fal_send(t, &m) < 0) { rms_textfile_close(tf); return SS$_ABORT; }
    }
    rms_textfile_close(tf);

    /* ACCESS-COMPLETE, then the server's STATUS. */
    if (send_simple(t, DNET_DAP_ACCESS_COMPLETE, 0) < 0) return SS$_ABORT;
    if (fal_recv(t, &m) < 0 || m.op != DNET_DAP_STATUS) return SS$_ABORT;
    return (m.u.status.stscode == DNET_DAP_STS_SUCCESS) ? SS$_NORMAL : SS$_ABORT;
}

uint32_t dnet_fal_client_get(const char *remote_spec, const char *local_spec,
                             struct dnet_dap_transport *t)
{
    if (!t || !t->send || !t->recv || !local_spec || !remote_spec) return SS$_ABORT;

    if (config_exchange(t, 1) < 0) return SS$_ABORT;

    struct dnet_dap_msg m;

    /* ACCESS OPEN the remote file. */
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_ACCESS;
    m.u.access.accfunc = DNET_DAP_ACC_OPEN;
    strncpy(m.u.access.filespec, remote_spec, sizeof m.u.access.filespec - 1);
    if (fal_send(t, &m) < 0) return SS$_ABORT;

    /* Server replies ATTRIBUTES (+NAME +ACK) on success, or STATUS(accfail). */
    if (fal_recv(t, &m) < 0) return SS$_ABORT;
    if (m.op == DNET_DAP_STATUS) return SS$_NOSUCHFILE;
    if (m.op != DNET_DAP_ATTRIBUTES) return SS$_ABORT;
    /* NAME (resolved full spec), then ACK. */
    if (fal_recv(t, &m) < 0 || m.op != DNET_DAP_NAME) return SS$_ABORT;
    if (fal_recv(t, &m) < 0 || m.op != DNET_DAP_ACKNOWLEDGE) return SS$_ABORT;

    /* CONTROL GET. */
    memset(&m, 0, sizeof m);
    m.op = DNET_DAP_CONTROL;
    m.u.control.ctlfunc = DNET_DAP_CTL_GET;
    if (fal_send(t, &m) < 0) return SS$_ABORT;

    /* Receive DATA records; write each locally through RMS. */
    int wrote_any = 0;
    for (;;) {
        if (fal_recv(t, &m) < 0) return SS$_ABORT;
        if (m.op == DNET_DAP_DATA) {
            char rec[DNET_DAP_MAX_REC + 1];
            uint16_t n = m.u.data.reclen;
            if (n > DNET_DAP_MAX_REC) n = DNET_DAP_MAX_REC;
            if (n) memcpy(rec, m.u.data.rec, n);
            rec[n] = '\0';
            int st = wrote_any ? rms_textfile_append_line(local_spec, rec)
                               : rms_textfile_write_line(local_spec, rec);
            if (st != 0) return SS$_ABORT;
            wrote_any = 1;
            continue;
        }
        if (m.op == DNET_DAP_ACCESS_COMPLETE) {
            if (!wrote_any) {
                if (rms_textfile_write_line(local_spec, "") != 0) return SS$_ABORT;
            }
            /* Acknowledge completion back to the server. */
            if (send_simple(t, DNET_DAP_ACCESS_COMPLETE, 0) < 0) return SS$_ABORT;
            return SS$_NORMAL;
        }
        if (m.op == DNET_DAP_STATUS)
            return (m.u.status.stscode == DNET_DAP_STS_EOF) ? SS$_NORMAL : SS$_NOSUCHFILE;
        return SS$_ABORT;
    }
}
