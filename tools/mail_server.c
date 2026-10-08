/*
 * mail_server.c - SYS$SYSTEM:MAIL_SERVER.EXE, the DECnet MAIL-11 network
 * server process (rd vms-47fd). NETACP creates it for one inbound object-27
 * connect (dnet_mail_proc.h); it speaks MAIL-11 over its link mailboxes
 * (dnet_mail11.h), validates each recipient against SYSUAF, stores each
 * accepted message in the recipient's mail file through RMS -- the SAME file
 * OVMX MAIL reads (vms_mail_notify.h) -- and acknowledges only what it stored.
 * Like VMS's MAIL_SERVER.EXE it is not a command a user runs: started any other
 * way it finds no link and exits.
 */
#include <stdio.h>
#include <string.h>

#include "dnet_fal_proc.h"
#include "dnet_mail_proc.h"
#include "dnet_mail11.h"
#include "ssdef.h"
#include "sysuaf.h"
#include "vms_mail_notify.h"

static int check_rcpt(void *ctx, const char *user)
{
    (void)ctx;
    sysuaf_record_t rec;
    int ok = (sysuaf_lookup(user, &rec) == 0);
    memset(&rec, 0, sizeof rec);
    return ok;
}

static uint32_t deliver(void *ctx, const char *user, const struct dnet_m11_msg *m,
                        char *errtext, size_t errcap)
{
    (void)ctx;
    return mail_store_deliver(user, m->from, m->to, m->cc, m->subj,
                              m->lines, m->nlines, errtext, errcap);
}

int main(void)
{
    static struct dnet_netsrv_link l;
    static struct dnet_m11_server s;
    static uint8_t seg[DNET_FALP_MAXMSG];
    struct dnet_falp_linkblk lb;

    uint32_t st = dnet_netsrv_attach(DNET_MAILP_PRCNAM_FMT, &l, &lb);
    if (!(st & 1)) return 1;

    const struct dnet_m11_ops ops = { check_rcpt, deliver, NULL };
    dnet_m11_init(&s, lb.local_node, lb.remote_node, &ops);

    uint32_t status = SS$_NORMAL;
    for (;;) {
        size_t n = 0;
        if (dnet_netsrv_recv(&l, seg, sizeof seg, &n) != 0)
            break;                    /* the link ended or went idle */
        int rc = dnet_m11_rx(&s, seg, n);
        size_t rn = 0;
        while (dnet_m11_tx_pop(&s, seg, sizeof seg, &rn))
            if (dnet_netsrv_send(&l, seg, rn) != 0) { rc = DNET_M11_EPROTO; break; }
        if (rc != DNET_M11_OK) { status = SS$_BADPARAM; break; }
        /* Once the final statuses are out the client disconnects; keep the link
         * until it does (NETACP's END record) rather than racing its DI. */
    }
    if (status == SS$_NORMAL && !dnet_m11_done(&s))
        status = SS$_ABORT;           /* ended before the message was complete */
    dnet_netsrv_exit(&l, status);
    return (status & 1) ? 0 : 1;
}
