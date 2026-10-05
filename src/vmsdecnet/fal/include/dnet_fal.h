/*
 * dnet_fal.h - the DECnet FILE ACCESS LISTENER (FAL, DECnet Session Control
 * object 17) server, and the COPY node:: client that drives it (rd vms-8c2,
 * epic vms-30e; north-star demo leg vms-e4dc). This is the file-transfer
 * layered product behind `$ COPY node"user pw"::file localfile`.
 *
 * ================== WHAT THIS FIXES, IN ONE PARAGRAPH ==================
 * An inbound FAL connect is a SECURITY surface: the connecting peer supplies a
 * username AND password IN THE CONNECT (oracle docs/oracle/vax-copy-fal-dap.md
 * §1 -- the OPPOSITE of CTERM, whose access-control fields are empty), and if
 * FAL served the file WITHOUT checking them it would be the file-access analogue
 * of the no-auth-CTERM hole the operator was furious about. So the FAL server
 * here AUTHENTICATES the connect-time credentials through THE ONE faithful
 * authenticator -- the same binary-SYSUAF / Purdy path LOGINOUT and SSHD use
 * (sysuaf_lookup + sysuaf_authenticate + sysuaf_interactive_login_permitted) --
 * and REFUSES the connect (the caller sends an NSP disconnect) on a bad
 * password or a disabled account. A fake check would let a bad password
 * through; only the real Purdy verify against the account's stored quadword
 * refuses it. Then, and only then, it serves/stores the file through RMS over
 * the ODS-2 executive ACP (rms_textfile_*), real file I/O -- no userspace
 * fallback that fakes a transfer (Rule 9).
 * =======================================================================
 *
 * LAYER BOUNDARY (Rule 1 / executive boundary). This module owns the DAP
 * PRESENTATION logic + the AUTH + the RMS file I/O. It does NOT own the wire:
 * the caller (decnetd, over the live datalink; the selftest, over a socketpair)
 * owns the NSP logical link and moves each DAP message as an NSP data segment,
 * handed here through a `struct dnet_dap_transport`. That is the same discipline
 * the CTERM host uses (the daemon owns the datalink; the session module owns the
 * protocol). NO fork/exec/openpty/dup2 and NO raw-termios/raw-fd file mechanics
 * live here -- file I/O is rms_textfile_* (RMS over the ACP), scanned by the
 * standing gate.
 */
#ifndef DNET_FAL_H
#define DNET_FAL_H

#include <stddef.h>
#include <stdint.h>

#include "dnet_dap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DECnet Session Control object number for FAL (oracle §1: object 0x11 = 17). */
#define DNET_FAL_OBJECT   17

/* Caps for the connect-carried access-control credentials. A username or
 * password longer than this is refused by the bounded decoder, not clipped. */
#define DNET_FAL_USER_MAX   64
#define DNET_FAL_PASS_MAX   64

/*
 * The NSP SEGMENT transport the caller provides -- the seam between the DAP
 * presentation layer (this module) and the NSP session layer (the caller's
 * logical link). send() ships ONE Session Control buffer (one NSP data
 * segment carrying one DAP message -- OVMX never blocks on send); recv()
 * delivers the next received segment, which a real VMS peer may fill with
 * SEVERAL blocked DAP messages (spec sec. 3.2: e.g. ATTRIBUTES+NAME+ACK in one
 * segment, observed live). This module splits them with the bounded decoder,
 * holding the remainder in rx[]. Both callbacks return 0 on success and a
 * negative value on a wire failure or a closed link. The rx* members are
 * private to this module; zero-initialise the struct.
 */
#define DNET_FAL_SEG_MAX 2048
struct dnet_dap_transport {
    int  (*send)(void *ctx, const uint8_t *seg, size_t len);
    int  (*recv)(void *ctx, uint8_t *buf, size_t cap, size_t *len);
    void  *ctx;
    uint8_t rx[DNET_FAL_SEG_MAX];
    size_t  rxlen, rxoff;
};

/*
 * dnet_fal_authenticate - THE ONE faithful authenticator, exposed for the
 * server and its acceptance test. Looks the username up in the binary SYSUAF
 * (over the ACP), Purdy-verifies `password`, and rejects a disabled account.
 *
 * Returns SS$_NORMAL (1) iff the credentials authenticate a login-permitted
 * account; SS$_INVLOGIN on no-such-user or a wrong password (the two are NOT
 * distinguished to the peer -- a real login does not leak which); SS$_NOPRIV on
 * a real account that is DISUSER'd. Fail-honest: with no /dev/vms / no mounted
 * SYSUAF, sysuaf_lookup returns not-found and this returns SS$_INVLOGIN -- it
 * never fabricates a pass (INV-6, Rule 9). The caller wipes the password buffer
 * after this returns.
 */
uint32_t dnet_fal_authenticate(const char *username, const char *password);

/*
 * dnet_fal_connect_auth - the CONNECT-TIME gate a FAL dispatcher runs the
 * moment an object-17 Connect Initiate arrives, BEFORE it accepts the link.
 * Decodes the access-control username+password from the UNTRUSTED connect bytes
 * (dnet_fal_access_decode, fully bounded), authenticates them
 * (dnet_fal_authenticate), and WIPES the password buffer. Returns SS$_NORMAL if
 * the caller may accept the link and serve the session; a refusal status
 * (SS$_INVLOGIN / SS$_NOPRIV) otherwise -- on which the caller sends an NSP
 * Disconnect Initiate (reason OBJREJ) and NEVER accepts. This is the FAL
 * analogue of the CTERM no-auth gate: no file is served on an unauthenticated
 * connect. `authed_user` (may be NULL, cap >= 1) receives the username on
 * success, for the accounting/log surface.
 */
uint32_t dnet_fal_connect_auth(const uint8_t *conn_data, size_t conn_len,
                               char *authed_user, size_t authed_user_cap);

/*
 * The identity a successful connect-time authentication establishes -- what
 * the FAL server process is created with (rd vms-d85): the account's UIC, its
 * DEFAULT privileges and its SYS$LOGIN, straight from the SYSUAF record the
 * password was verified against.
 */
struct dnet_fal_identity {
    char     username[33];
    uint32_t uic;                 /* (group << 16) | member */
    uint64_t def_privs;           /* uaf$q_def_priv          */
    char     default_dir[256];    /* SYSUAF default device:[directory] */
};

/*
 * dnet_fal_connect_auth_id - dnet_fal_connect_auth, plus the authenticated
 * account's identity in *id on SS$_NORMAL (zeroed on any refusal).
 */
uint32_t dnet_fal_connect_auth_id(const uint8_t *conn_data, size_t conn_len,
                                  struct dnet_fal_identity *id);

/*
 * FILE NAME RESOLUTION for the FAL server (directory list + NAME): begin a
 * search of `spec` (wildcards allowed), return each RESULTANT spec
 * ("DEV:[DIR]NAME.TYP;V"), end. The shipped implementation is RMS $PARSE +
 * $SEARCH over the ACP (dnet_fal_search.c); 0 = ok / a match, -1 = no (more)
 * files or failure. Never composes a name RMS did not return.
 */
int  dnet_fal_search_begin(const char *spec, void **ctx);
int  dnet_fal_search_next(void *ctx, char *rsa, size_t cap);
void dnet_fal_search_end(void *ctx);

/* Record output for a FAL CREATE: $CREATE the file (sequential, the given
 * RMS record format + attributes) returning its RESULTANT spec, $PUT records
 * verbatim, $CLOSE. RMS over the ACP (dnet_fal_search.c); 0 = ok, -1 = fail. */
int  dnet_fal_wopen(const char *spec, uint8_t rfm, uint8_t rat, void **h,
                    char *rsa, size_t cap);
int  dnet_fal_wput(void *h, const uint8_t *rec, size_t len);
int  dnet_fal_wclose(void *h);

/* Record input for a FAL OPEN (and the COPY client's local source): $OPEN the
 * file and $GET its records through RMS -- which knows the file's record
 * format (VAR, STM*, FIX), so a record is exactly what TYPE shows, NULs and
 * all. *rfm / *rat (may be NULL) receive the file's real record format and
 * attributes for the ATTRIBUTES reply. rget: 1 = a record (len in *len),
 * 0 = end of file, -1 = error. 0 = ok / -1 = fail elsewhere. */
int  dnet_fal_ropen(const char *spec, void **h, uint8_t *rfm, uint8_t *rat);
int  dnet_fal_rget(void *h, uint8_t *rec, size_t cap, size_t *len);
int  dnet_fal_rclose(void *h);

/*
 * dnet_fal_server_run - serve one AUTHENTICATED, ACCEPTED FAL session to
 * completion. The caller has already run dnet_fal_connect_auth (got SS$_NORMAL)
 * and accepted the link (sent the Connect Confirm), so this runs only the DAP
 * session over `t`: CONFIGURATION exchange, then an ACCESS (open for a GET /
 * create for a PUT), ATTRIBUTES/NAME, CONTROL, DATA records (read from / written
 * to the local file via RMS over the ACP), and STATUS / ACCESS-COMPLETE.
 *
 * Returns SS$_NORMAL on a completed transfer, SS$_NOSUCHFILE if the requested
 * file cannot be opened for a GET, or SS$_ABORT on a transport/protocol failure
 * or an unserved (non-sequential) file. No credential is handled here -- the
 * connect-time gate already ran; this half only moves the file.
 */
uint32_t dnet_fal_server_run(struct dnet_dap_transport *t);

/*
 * dnet_fal_server_set_default - the default device:[directory] a peer's
 * filespec is completed against: the accessed user's login directory, as a VMS
 * network job resolves a remote "FILE.TXT" in SYS$LOGIN. A spec with no device
 * and no directory gets both; one with a [directory] but no device gets the
 * device; one naming a device or logical ("SYS$LOGIN:X") is left as is. NULL
 * or "" clears it. (FAL.EXE sets it from the link block, rd vms-d85.)
 */
void dnet_fal_server_set_default(const char *default_dir);

/* Exposed for the host test: apply the default above to `spec`. 0, or -1 when
 * the result would not fit `cap` (the spec is then refused, never truncated). */
int dnet_fal_apply_default(const char *spec, char *out, size_t cap);

/*
 * dnet_fal_client_put - `$ COPY local remote::` : send the local sequential file
 * to the remote FAL as a new file. Drives the DAP client (CONFIGURATION,
 * ACCESS CREATE, CONTROL PUT, DATA per record, ACCESS-COMPLETE) over `t`;
 * reads the local file via RMS over the ACP. `remote_spec` is the destination
 * file spec (no node prefix). Returns SS$_NORMAL on success, SS$_NOSUCHFILE if
 * the local source cannot be opened, SS$_ABORT on a transport/remote failure.
 */
uint32_t dnet_fal_client_put(const char *local_spec, const char *remote_spec,
                             struct dnet_dap_transport *t);

/*
 * dnet_fal_client_get - `$ COPY remote:: local` : fetch the remote sequential
 * file and store it locally. Drives the DAP client (CONFIGURATION, ACCESS OPEN,
 * CONTROL GET, receive DATA records, ACCESS-COMPLETE) over `t`; writes the local
 * file via RMS over the ACP. Returns SS$_NORMAL on success, SS$_NOSUCHFILE if
 * the remote reports the source missing, SS$_ABORT on a transport failure.
 */
uint32_t dnet_fal_client_get(const char *remote_spec, const char *local_spec,
                             struct dnet_dap_transport *t);

#ifdef __cplusplus
}
#endif

#endif /* DNET_FAL_H */
