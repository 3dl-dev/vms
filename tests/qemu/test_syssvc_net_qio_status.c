/*
 * test_syssvc_net_qio_status (rd vms-dda / vms-d01) -- a $QIOW on a _NET:
 * channel completes with NETACP's answer in the IOSB, and a DECnet client
 * image reads it from there.
 *
 * THE BUG THIS GUARDS. Since vms-d01, $QIO/$QIOW return the status of QUEUING
 * a request and put how the I/O ENDED in the IOSB, as on VMS. DECNETD's _NET:
 * client kept reading the service status: a connect NETACP refused
 * (SS$_INVLOGIN) looked like an open link, every later read of that
 * nonexistent link (SS$_FILNOTACC in the IOSB) looked like an empty message,
 * and a bad-password COPY 0"SYSTEM WRONGPW":: polled for ever -- the booted
 * refused-OPEN hang (test_decnet_startnet_boot_e2e.sh, boot 2). Every client
 * now issues its _NET: $QIOWs through dnet_net_qiow (dnet_netqio.h).
 *
 * WHAT RUNS. The real executive: $ASSIGN _NET: (the DECnet device face on the
 * rail's NIC), libvms qio_net_op, and real executive mailboxes. This process
 * also answers as NETACP's request side -- it publishes DNET$NETACP_REQ for a
 * mailbox it reads, and answers each request on the client's reply mailbox
 * with a status chosen by the test (a refused connect, an accepted one, an
 * empty read). That is the far end of the seam only; the client half under
 * test is the code DECNETD runs. The real NETACP's own refusal is proven by the
 * booted DECnet e2e.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "iodef.h"
#include "iosbdef.h"
#include "lnmdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "vms/logical.h"

#include "../../src/vmsdecnet/nsp/include/dnet_nsp.h"
#define DNET_BROKER_API static __attribute__((unused))
#include "../../src/vmsdecnet/broker/include/dnet_broker.h"
#include "../../src/vmsdecnet/broker/dnet_broker.c"
#include "../../src/vmsdecnet/broker/include/dnet_netqio.h"

#define EXIT_SKIP 77
#define REQ_LOGNAM "DNET$NETACP_REQ"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

/* ---- the far end: NETACP's request mailbox, answered as the test says ---- */
static uint32_t g_req_chan;
static volatile int g_stop;
static volatile uint32_t g_open_status = SS$_INVLOGIN;   /* answer to an OPEN   */
static volatile unsigned g_served;

static void *responder(void *v)
{
    (void)v;
    (void)vms_kif_open();                  /* /dev/vms is opened per thread */
    while (!g_stop) {
        uint8_t buf[DNET_BROKER_REQ_MAX + 16];
        uint32_t got = 0;
        if (!(vms_kif_mbx_read(g_req_chan, buf, sizeof buf, &got, 1) & 1)) {
            struct timespec ts = { 0, 2 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }
        struct dnet_broker_req req;
        if (dnet_broker_req_decode(buf, got > sizeof buf ? sizeof buf : got, &req) !=
            DNET_BROKER_OK)
            continue;
        struct dnet_broker_rsp rsp;
        memset(&rsp, 0, sizeof rsp);
        rsp.corr_id = req.corr_id;
        switch (req.op & DNET_BROKER_OP_MASK) {
        case DNET_BROKER_OP_OPEN:
            rsp.status = g_open_status;
            if (rsp.status & 1) {          /* an accepted connect carries its handle */
                rsp.data[0] = 0x5A; rsp.data[1] = 0xA5; rsp.data[2] = 0x01; rsp.data[3] = 0x00;
                rsp.datalen = 4;
            }
            break;
        case DNET_BROKER_OP_RECV:  rsp.status = SS$_ENDOFFILE; break;  /* nothing buffered */
        default:                   rsp.status = SS$_NORMAL;    break;
        }
        uint8_t rec[DNET_BROKER_RSP_MAX];
        size_t n = 0;
        char dev[32];
        uint32_t rc = 0;
        snprintf(dev, sizeof dev, "MBA%u:", (unsigned)req.reply_unit);
        if (dnet_broker_rsp_encode(&rsp, rec, sizeof rec, &n) == DNET_BROKER_OK &&
            (vms_kif_mbx_assign(dev, &rc) & 1)) {
            (void)vms_kif_mbx_write(rc, rec, (uint32_t)n);
            (void)vms_kif_dassgn(rc);
            g_served++;
        }
    }
    vms_kif_close();
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_net_qio_status (a _NET: $QIOW ends with NETACP's answer"
           " in the IOSB, rd vms-dda/vms-d01) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_net_qio_status: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    char dev[64];
    uint16_t dl = 0;
    if (vms_kif_lnm_translate(VMS_LNM_TBL_SYSTEM, REQ_LOGNAM, 0, dev, sizeof dev - 1,
                              &dl, NULL, NULL) == 1 && dl > 0) {
        printf("  FAIL: a NETACP already serves %s here -- this suite answers as NETACP"
               " itself and will not take over a live one\n", REQ_LOGNAM);
        return 1;
    }

    uint16_t chan = 0;
    struct dsc$descriptor_s net = mkdsc("_NET:");
    uint32_t ast = sys$assign(&net, &chan, 0, NULL);
    CHECK(ast == SS$_NORMAL && chan != 0,
          "$ASSIGN _NET: grants a channel (the DECnet device face on this node's NIC)");
    if (ast != SS$_NORMAL) {
        printf("=== test_syssvc_net_qio_status: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    uint32_t unit = 0;
    dev[0] = '\0';
    uint32_t st = vms_kif_mbx_create(0, DNET_BROKER_REQ_MAX + 16,
                                     (DNET_BROKER_REQ_MAX + 16) * 8, &g_req_chan, &unit,
                                     dev, sizeof dev);
    const char *vals[1] = { dev };
    int published = (st & 1) &&
        (vms_kif_lnm_define(VMS_LNM_TBL_SYSTEM, REQ_LOGNAM, vals, 1, 0, LNM$C_USER) & 1);
    CHECK(published, "the test's NETACP request mailbox is published as DNET$NETACP_REQ");
    pthread_t th;
    int running = published && pthread_create(&th, NULL, responder, NULL) == 0;

    if (running) {
        /* A connect NETACP refuses (bad password). */
        static char ncb[] = "0\"SYSTEM WRONGPW\"::\"17=\"";
        struct _iosb iosb;
        memset(&iosb, 0, sizeof iosb);
        g_open_status = SS$_INVLOGIN;
        uint32_t svc = sys$qiow(0, chan, IO$_ACCESS, &iosb, NULL, 0, ncb,
                                (uint32_t)strlen(ncb), 0, 0, 0, 0);
        CHECK(svc == SS$_NORMAL && iosb.iosb$w_status == SS$_INVLOGIN,
              "$QIOW IO$_ACCESS refused by NETACP: the service status is SS$_NORMAL (queued),"
              " the IOSB says SS$_INVLOGIN (as VMS, vms-d01) -- so the service status alone"
              " cannot tell a refusal from a link");
        size_t x = 0;
        uint32_t cst = dnet_net_qiow(chan, IO$_ACCESS, ncb, (uint32_t)strlen(ncb), &x);
        /* negctl: net-client-takes-qio-service-status */
        CHECK(cst == SS$_INVLOGIN,
              "the DECnet client's IO$_ACCESS (dnet_net_qiow) ends SS$_INVLOGIN for a refused"
              " connect -- never an open link");
        uint8_t rb[64];
        cst = dnet_net_qiow(chan, IO$_READVBLK | IO$M_NOW, rb, sizeof rb, &x);
        /* negctl: net-client-takes-qio-service-status */
        CHECK(cst == SS$_FILNOTACC,
              "a read on the channel after the refused connect ends SS$_FILNOTACC (no link)"
              " -- not an empty message to poll for ever (the booted hang)");

        /* A connect NETACP accepts, then a read with nothing buffered. */
        g_open_status = SS$_NORMAL;
        static char ncb2[] = "0\"SYSTEM MANAGER\"::\"17=\"";
        cst = dnet_net_qiow(chan, IO$_ACCESS, ncb2, (uint32_t)strlen(ncb2), &x);
        CHECK(cst == SS$_NORMAL, "an accepted connect ends SS$_NORMAL (the link is open)");
        cst = dnet_net_qiow(chan, IO$_READVBLK | IO$M_NOW, rb, sizeof rb, &x);
        CHECK(cst == SS$_ENDOFFILE && x == 0,
              "IO$_READVBLK|IO$M_NOW with no message buffered ends SS$_ENDOFFILE, 0 bytes");
        cst = dnet_net_qiow(chan, IO$_DEACCESS, NULL, 0, &x);
        CHECK(cst == SS$_NORMAL, "IO$_DEACCESS ends SS$_NORMAL");
        CHECK(g_served >= 5, "every request reached the test's NETACP through the executive"
                             " mailboxes (none was answered locally)");
    }

    CHECK(sys$dassgn(chan) == SS$_NORMAL, "$DASSGN releases the _NET: channel");
    if (running) {
        g_stop = 1;
        pthread_join(th, NULL);
    }
    if (published)
        (void)vms_kif_lnm_delete(VMS_LNM_TBL_SYSTEM, REQ_LOGNAM, LNM$C_USER);
    if (st & 1) {
        (void)vms_kif_mbx_delmbx(g_req_chan);
        (void)vms_kif_dassgn(g_req_chan);
    }
    CHECK(vms_kif_lnm_translate(VMS_LNM_TBL_SYSTEM, REQ_LOGNAM, 0, dev, sizeof dev - 1,
                                &dl, NULL, NULL) != 1,
          "DNET$NETACP_REQ is gone again (the next suite sees no NETACP)");

    printf("=== test_syssvc_net_qio_status: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
