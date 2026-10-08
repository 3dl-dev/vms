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

/*
 * The RMS completion of the last $PARSE/$SEARCH on a search context (RMS$_FNF,
 * RMS$_NMF, RMS$_PRV, ...) and, in esa (may be NULL), the EXPANDED spec RMS
 * built -- what a real VMS FAL names back when a lookup finds nothing
 * (NAME volume/directory/file, then STATUS). The begin call keeps its context
 * even when $PARSE fails so this can report why; it returns -1 only when no
 * context could be made at all.
 */
uint32_t dnet_fal_search_status(void *ctx, uint32_t *stv, char *esa, size_t cap);

/*
 * The file attributes a DIRECTORY LIST / DISPLAY reports, read from the real
 * ODS-2 file header through the executive ACP (rms_file_attr). Only what the
 * header holds; the DAP layer omits everything else (INV-6).
 */
struct dnet_fal_fattr {
    uint8_t  org, rfm, rat;      /* FAT: org nibble (DAP ORG), FAB$C_ rfm, rattrib */
    uint16_t mrs, lrl, deq;      /* FAT maxrec, rsize (longest record), defext */
    uint32_t alq, ebk;           /* highest allocated VBN, end-of-file VBN       */
    uint16_t ffb;                /* first free byte of the EOF block             */
    uint16_t fileprot;           /* ODS-2 protection: S/O/G/W deny nibbles       */
    uint16_t uic_group, uic_member;
    uint16_t revision;           /* header revision count                        */
    uint8_t  credate[8], revdate[8], expdate[8];   /* VMS 64-bit times, 0 = none */
};
/* 0 = filled; -1 = failed with the RMS status in *sts (may be NULL). */
int  dnet_fal_fileattr(const char *spec, struct dnet_fal_fattr *out, uint32_t *sts);

/*
 * $ERASE / $RENAME through RMS over the ACP, as THIS process (the FAL server
 * process carries the authenticated user's UIC + privileges, so the executive
 * ACP decides). 0 = done; -1 = refused, with the RMS completion in *sts and the
 * system status (FAB$L_STV) in *stv. Never a server-side protection rule.
 */
int  dnet_fal_erase(const char *spec, uint32_t *sts, uint32_t *stv);
int  dnet_fal_rename(const char *oldspec, const char *newspec, uint32_t *sts, uint32_t *stv);

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
/* dnet_fal_ropen, plus the RMS completion of a refused $OPEN in *sts (may be
 * NULL): FNF vs PRV decide the STATUS the server sends. */
int  dnet_fal_ropen_st(const char *spec, void **h, uint8_t *rfm, uint8_t *rat,
                       uint32_t *sts);
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
 * Also served (rd vms-277a): DIRECTORY LIST with the ATTRIBUTES / SUMMARY /
 * DATE AND TIME / PROTECTION messages DISPLAY asks for, ACCESS ERASE and
 * ACCESS RENAME + NAME -- each answered in one blocked segment in a real VMS
 * FAL's shape, with its STATUS bytes for a refusal.
 *
 * Returns SS$_NORMAL on a completed transfer, SS$_NOSUCHFILE if the requested
 * file cannot be opened for a GET, or SS$_ABORT on a transport/protocol failure
 * or an unserved (non-sequential) file. No credential is handled here -- the
 * connect-time gate already ran; this half only moves the file.
 */
uint32_t dnet_fal_server_run(struct dnet_dap_transport *t);


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

/*
 * dnet_fal_client_erase / dnet_fal_client_rename - `$ DELETE node::spec` and
 * `$ RENAME node::old node::new` at the DAP level (spec 5.2.6 / 5.2.8): the
 * CONFIGURATION exchange, then ACCESS(ERASE) or ACCESS(RENAME) + NAME(new),
 * DISPLAY = NAME as a VMS client asks. Returns SS$_NORMAL on ACCESS COMPLETE
 * (RESPONSE); otherwise SS$_ABORT, with the remote STATUS's STSCODE and STV
 * (0 when absent) in *stscode / *stv (may be NULL) so a caller can report the
 * exact refusal the remote FAL sent.
 */
uint32_t dnet_fal_client_erase(const char *remote_spec, struct dnet_dap_transport *t,
                               uint16_t *stscode, uint64_t *stv);
uint32_t dnet_fal_client_rename(const char *old_spec, const char *new_spec,
                                struct dnet_dap_transport *t,
                                uint16_t *stscode, uint64_t *stv);

#ifdef __cplusplus
}
#endif

#endif /* DNET_FAL_H */
