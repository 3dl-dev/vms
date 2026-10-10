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
 * every file FAL opens. OVMX now does the same:
 *
 *   NETACP: connect-time auth (dnet_fal_connect_auth_rec, THE ONE authenticator)
 *     -> $CREMBX two link mailboxes (to-server, from-server)
 *     -> $CREPRC SYS$SYSTEM:FAL.EXE, UIC = the user's UIC, privileges = the
 *        user's DEFAULT privileges (uaf$q_def_priv), process name
 *        FAL_<to>_<from> naming the two mailbox units
 *     -> first record: the link block (user name + SYSUAF default device and
 *        directory -- the SYS$NET/NCB analogue a VMS network job is given)
 *     -> then pumps NSP data segments <-> mailbox records.
 *   FAL.EXE: reads its own process name ($GETJPI), assigns the two mailboxes,
 *     sets its default directory to the user's SYS$LOGIN, and runs
 *     dnet_fal_server_run over them. Every rms_textfile_* open it performs is
 *     checked by the executive ACP against ITS OWN row: the user's UIC and
 *     privileges (vmsfs_acp.c acp_check_access) -- not SYSTEM's.
 *
 * Process name + link block as the channel is an OVMX design choice (LABELLED):
 * real VMS hands a network job its link through the SYS$NET logical name and
 * the _NET: device; OVMX's _NET: $QIO broker (rd vms-dda) is not yet the
 * server-side seam, so the link rides two executive mailboxes (the T1 transport
 * ratified for vms-22c). The security property -- the access runs with the
 * authenticated identity, never NETACP's -- is the VMS one.
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
#define DNET_FALP_PRCNAM_FMT   "FAL_%u_%u"

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

/* ---- NETACP side ---------------------------------------------------------- */

struct dnet_fal_proc {
    int      active;
    uint32_t pid;              /* the FAL server process ($CREPRC)            */
    uint32_t ch_to, ch_from;   /* NETACP's channels to the two mailboxes       */
    uint32_t unit_to, unit_from;
    uint32_t uic;              /* the persona the server runs with            */
    const char *fail_stage;    /* on a refused start: which service refused   */
};

/*
 * dnet_fal_proc_start - create the server for an AUTHENTICATED access: the two
 * mailboxes, the FAL.EXE process with the user's UIC + default privileges, and
 * the link block. `uic`/`def_privs`/`username`/`default_dir` come from the
 * SYSUAF record dnet_fal_connect_auth_rec returned. Returns SS$_NORMAL, or the
 * failing service's status with nothing left behind (no mailbox, no process):
 * the caller refuses the connect -- there is no in-NETACP fallback.
 */
uint32_t dnet_fal_proc_start(struct dnet_fal_proc *p, uint32_t uic,
                             uint64_t def_privs, const char *username,
                             const char *default_dir);

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
 * THE SAME LINK FOR ANY NETWORK SERVER IMAGE (rd vms-47fd). FAL.EXE (object 17)
 * and MAIL_SERVER.EXE (object 27) are both NETACP-created server processes on
 * this mailbox link; only the image, the process-name prefix and the persona
 * differ, so the plumbing is one implementation.
 *
 * dnet_netsrv_proc_start - $CREMBX the two mailboxes, queue the link block,
 * $CREPRC `image_spec` (a SYS$SYSTEM: spec) as process `prcnam_fmt` (two %u:
 * the to/from mailbox units) with `uic` + `privs`. Status as dnet_fal_proc_start.
 */
uint32_t dnet_netsrv_proc_start(struct dnet_fal_proc *p, const char *image_spec,
                                const char *prcnam_fmt, uint32_t uic, uint64_t privs,
                                const struct dnet_falp_linkblk *lb);

/* Is a runnable `image_spec` on this system disk? 1 / 0. */
int dnet_netsrv_image_present(const char *image_spec);

/* ---- server-image side (FAL.EXE, MAIL_SERVER.EXE) ------------------------- */

struct dnet_netsrv_link { uint32_t ch_in, ch_out; int ended; };

/* Find this process's link from its own name (`prcnam_scanfmt`, e.g.
 * "FAL_%u_%u"), assign both mailboxes, read the link block. SS$_NORMAL or a
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

/* ---- FAL.EXE side --------------------------------------------------------- */

/*
 * dnet_fal_proc_serve - the whole FAL.EXE body: find the link from this
 * process's own name, read the link block, set the default directory, run
 * dnet_fal_server_run over the mailboxes, report EXIT. Returns the session
 * status (SS$_NORMAL on a completed transfer).
 */
uint32_t dnet_fal_proc_serve(void);

#ifdef __cplusplus
}
#endif

#endif /* DNET_FAL_PROC_H */
