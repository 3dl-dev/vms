/*
 * dnet_fal_proc.h - the FAL NETWORK SERVER PROCESS (rd vms-d85, R4 G3; epic
 * vms-30e). The VMS shape of serving an inbound file access: NETACP does not
 * touch the file itself; it creates a server process that RUNS AS THE
 * AUTHENTICATED USER and the DAP session moves through it.
 *
 * WHY (the gap this closes). The vms-8c2 FAL server ran inside NETACP with the
 * daemon's own identity: after a correct password, an authenticated GUEST could
 * GET/PUT any path SYSTEM can reach (docs/security/decnet-networking-r4-sweep.md
 * G3). Real VMS never serves a network file access in NETACP: the access lands
 * in a network job (NETSERVER -> FAL.EXE) created with the user's UIC and
 * default privileges, so the ordinary Files-11 protection check applies to
 * every file FAL opens. OVMX does the same.
 *
 * FAL (object 17) RECEIVES ITS LINK THE VMS WAY, THROUGH SYS$NET (rd vms-0a7
 * S3):
 *   NETACP: connect-time auth (dnet_fal_connect_auth_id, THE ONE authenticator)
 *     -> $CREPRC SYS$SYSTEM:FAL.EXE, PRC$M_NETWRK, UIC = the user's UIC,
 *        privileges = the user's DEFAULT privileges, input = the inbound-accept
 *        NCB naming the waiting link (dnet_ncb.h) -- $CREPRC gives a network
 *        process that NCB as its SYS$NET (and SYS$INPUT) in LNM$PROCESS
 *     -> holds the link (no Connect Confirm yet) for the process it created.
 *   FAL.EXE: $TRNLNM SYS$NET, $ASSIGN _NET:, $QIOW IO$_ACCESS with that NCB
 *     -- the ACCEPT; NETACP sends the Connect Confirm then -- and the DAP
 *     session moves over $QIOW IO$_READVBLK / IO$_WRITEVBLK on the channel,
 *     IO$_DEACCESS + $DASSGN at the end. The link is then NETACP's brokered
 *     slot like any local process's (NSP flow control, receive window).
 *   The accept's completion carries the network job's information (user name,
 *   SYSUAF default device:[directory]) -- what VMS's LOGINOUT establishes for a
 *   network job; OVMX's FAL.EXE is not run under LOGINOUT, so NETACP, which
 *   read the SYSUAF record to authenticate, hands it over (LABELLED OVMX
 *   design: IO$_ACCESS p3, sys_qio.c). FAL.EXE sets its default directory and
 *   SYS$LOGIN from it.
 *
 * MAIL_SERVER.EXE (object 27, rd vms-47fd) still rides the older OVMX mailbox
 * link below (two executive mailboxes named by its process name, LABELLED; the
 * move to SYS$NET is a filed follow-up). The security property for both -- the
 * access runs with the authenticated identity, never NETACP's -- is the VMS one.
 *
 * RECORD FRAMING on both mailboxes: byte 0 = record type, the rest = payload.
 */
#ifndef DNET_FAL_PROC_H
#define DNET_FAL_PROC_H

#include <stddef.h>
#include <stdint.h>

#include "dnet_fal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The image NETACP activates for an inbound object-17 connect: NETACP's object
 * table (the FAL row of NCP SHOW KNOWN OBJECTS, rd vms-2d0) names this same file. */
#define DNET_FAL_IMAGE_SPEC    "SYS$SYSTEM:FAL.EXE"

#define DNET_FALP_REC_LINKBLK  'L'   /* NETACP -> FAL: first record, link block  */
#define DNET_FALP_REC_DATA     'D'   /* either way: one NSP data segment payload  */
#define DNET_FALP_REC_END      'E'   /* NETACP -> FAL: the link is gone           */
#define DNET_FALP_REC_EXIT     'X'   /* FAL -> NETACP: session status (4 bytes LE)*/

#define DNET_FALP_MAXMSG       (DNET_FAL_SEG_MAX + 1)

/* Idle bound on a FAL session (either direction silent this long ends it):
 * a stalled peer cannot hold a server process forever (R4 G2 class). */
#define DNET_FALP_IDLE_SEC     120

/* The link block NETACP hands the server (bounded, NUL-terminated fields). */
struct dnet_falp_linkblk {
    char username[33];
    char default_dir[256];     /* SYSUAF default device:[directory] */
    char remote_node[16];      /* the peer, by node name (else its address) */
    char local_node[16];       /* this node's executor name                 */
};

/* ---- NETACP side of the MAILBOX link (MAIL_SERVER.EXE, object 27) ---------- */

struct dnet_fal_proc {
    int      active;
    uint32_t pid;              /* the FAL server process ($CREPRC)            */
    uint32_t ch_to, ch_from;   /* NETACP's channels to the two mailboxes       */
    uint32_t unit_to, unit_from;
    uint32_t uic;              /* the persona the server runs with            */
    const char *fail_stage;    /* on a refused start: which service refused   */
};

/* Is a runnable SYS$SYSTEM:FAL.EXE on this system disk? 1 / 0. */
int dnet_fal_proc_image_present(void);

/* Hand one received NSP data segment to the server. 0 / -1. */
int dnet_fal_proc_put(struct dnet_fal_proc *p, const uint8_t *seg, size_t len);

/* Poll (never blocks) for the server's next record. Returns 1 with a DATA
 * segment in buf (length *len), 2 when the server reported EXIT (*status set), 0 when
 * nothing is queued, -1 on a mailbox failure. */
int dnet_fal_proc_poll(struct dnet_fal_proc *p, uint8_t *buf, size_t cap,
                       size_t *len, uint32_t *status);

/* Is the server process still in the executive's process table? */
int dnet_fal_proc_alive(const struct dnet_fal_proc *p);

/* Tell the server the link is gone (END record) and release the mailboxes. */
void dnet_fal_proc_close(struct dnet_fal_proc *p);

/*
 * THE MAILBOX LINK FOR A NETWORK SERVER IMAGE (rd vms-47fd). MAIL_SERVER.EXE
 * (object 27) is a NETACP-created server process on this mailbox link (FAL.EXE
 * was too, until it moved to SYS$NET, rd vms-0a7 S3).
 *
 * dnet_netsrv_proc_start - $CREMBX the two mailboxes, queue the link block,
 * $CREPRC `image_spec` (a SYS$SYSTEM: spec) as process `prcnam_fmt` (two %u:
 * the to/from mailbox units) with `uic` + `privs`. SS$_NORMAL, or the failing
 * service's status with nothing left behind (no mailbox, no process).
 */
uint32_t dnet_netsrv_proc_start(struct dnet_fal_proc *p, const char *image_spec,
                                const char *prcnam_fmt, uint32_t uic, uint64_t privs,
                                const struct dnet_falp_linkblk *lb);

/* Is a runnable `image_spec` on this system disk? 1 / 0. */
int dnet_netsrv_image_present(const char *image_spec);

/* ---- server-image side of the mailbox link (MAIL_SERVER.EXE) -------------- */

struct dnet_netsrv_link { uint32_t ch_in, ch_out; int ended; };

/* Find this process's link from its own name (`prcnam_scanfmt`, e.g.
 * "MAIL_%u_%u"), assign both mailboxes, read the link block. SS$_NORMAL or a
 * VMS status (no link: the image was not started by NETACP). */
uint32_t dnet_netsrv_attach(const char *prcnam_scanfmt, struct dnet_netsrv_link *l,
                            struct dnet_falp_linkblk *lb);

/* Ship one NSP data segment payload to the peer. 0 / -1. */
int dnet_netsrv_send(struct dnet_netsrv_link *l, const uint8_t *seg, size_t len);

/* Next received segment payload (may be empty), waiting at most the idle bound
 * DNET_FALP_IDLE_SEC. 0, or -1: the link ended, went idle, or failed. */
int dnet_netsrv_recv(struct dnet_netsrv_link *l, uint8_t *buf, size_t cap, size_t *len);

/* Report the session status to NETACP (the EXIT record). */
void dnet_netsrv_exit(struct dnet_netsrv_link *l, uint32_t status);

/* ---- FAL over SYS$NET (rd vms-0a7 S3) ---------------------------------------- */

struct dnet_broker_chan;
struct dnet_broker_io;

/*
 * dnet_fal_net_start - NETACP's half: $CREPRC SYS$SYSTEM:FAL.EXE as process
 * `prcnam`, PRC$M_DETACH | PRC$M_NETWRK, with `uic` + `privs` (the
 * authenticated user's UIC and DEFAULT privileges) and input = `ncb`, the
 * inbound-accept NCB, which becomes the process's SYS$NET. *pid = the new
 * process. SS$_NORMAL, or the failing service's status with *fail_stage set
 * (nothing is left behind; the caller refuses the connect).
 */
uint32_t dnet_fal_net_start(const char *ncb, const char *prcnam, uint32_t uic,
                            uint64_t privs, uint32_t *pid, const char **fail_stage);

/* The link a network server accepted from its SYS$NET. Live: `chan` is the
 * $ASSIGN _NET: channel and every operation is a $QIOW on it. The host
 * selftest sets bc/io instead (the in-process broker queue standing in for
 * the executive mailboxes, as DECNETD's netcli does). */
struct dnet_netsrv_net {
    uint16_t chan;
    struct dnet_broker_chan *bc;
    const struct dnet_broker_io *io;
    int ended;
};

/* IO$_ACCESS with the accept NCB: accept the link; *lb = the network job's
 * information NETACP returned. SS$_NORMAL or the completion status. */
uint32_t dnet_netsrv_net_accept(struct dnet_netsrv_net *n, const char *ncb, size_t ncblen,
                                struct dnet_falp_linkblk *lb);
/* IO$_WRITEVBLK one DAP segment. 0 / -1. */
int dnet_netsrv_net_send(struct dnet_netsrv_net *n, const uint8_t *seg, size_t len);
/* IO$_READVBLK the next segment (the waiting form). 0 / -1 (link gone). */
int dnet_netsrv_net_recv(struct dnet_netsrv_net *n, uint8_t *buf, size_t cap, size_t *len);
/* IO$_DEACCESS (a normal disconnect: queued data still goes first). */
void dnet_netsrv_net_close(struct dnet_netsrv_net *n);

/* The FAL session on an accepted link: default directory + SYS$LOGIN from
 * *lb, dnet_fal_server_run over the link, IO$_DEACCESS. The session status. */
uint32_t dnet_fal_net_run(struct dnet_netsrv_net *n, const struct dnet_falp_linkblk *lb);

/*
 * dnet_fal_net_serve - the whole FAL.EXE body: $TRNLNM SYS$NET (LNM$PROCESS),
 * $ASSIGN _NET:, accept, dnet_fal_net_run, $DASSGN. SS$_NOLOGNAM when this
 * process has no SYS$NET (it was not created by NETACP for a link).
 */
uint32_t dnet_fal_net_serve(void);

#ifdef __cplusplus
}
#endif

#endif /* DNET_FAL_PROC_H */
