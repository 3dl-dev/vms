/*
 * rtpad.c -- SYS$SYSTEM:RTPAD.EXE, the remote terminal pad behind $ SET HOST
 * (rd vms-0a7 S2, vms-18c9).
 *
 * On VMS the DCL command tables send SET HOST node to RTPAD.EXE, which reads
 * its parameters with CLI$GET_VALUE / CLI$PRESENT, opens a logical link to
 * the CTERM object (42) on the node through _NET:, and carries the user's
 * terminal over it until the remote session logs out, then says
 * "%REM-S-END, control returned to node <local>::".
 *
 * Everything this image does goes through VMS services:
 *   - the command:  CLI$GET_VALUE("NODE"), CLI$PRESENT/CLI$GET_VALUE
 *                   ("USERNAME") on the command DCL parsed (DCL$TABLES);
 *   - the network:  $ASSIGN _NET:, $QIOW IO$_ACCESS (the NCB
 *                   node["user"]::"42="), IO$_WRITEVBLK, IO$_READVBLK!IO$M_NOW,
 *                   IO$_DEACCESS, and IO$_ACPCONTROL to ask the running NETACP
 *                   for this node's name;
 *   - the terminal: $GETDVIW SYS$INPUT (is it a terminal?), $ASSIGN TT:, and
 *                   $QIOW IO$_READVBLK (with the host's echo), IO$_WRITEVBLK,
 *                   IO$_SENSEMODE!IO$M_TYPEAHDCNT;
 *   - waiting:      $SETIMR + $WFLOR on a local event flag.
 * There is no argv, no stdio, no isatty, no poll, no thread and no host fd.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rtpad_session.h"
#include "dnet_cterm.h"
#include "dnet_netqio.h"
#include "dnet_netshow.h"
#include "descrip.h"
#include "starlet.h"
#include "iodef.h"
#include "iosbdef.h"
#include "ssdef.h"
#include "rmsdef.h"
#include "stsdef.h"
#include "dvidef.h"
#include "dcdef.h"
#include "iledef.h"
#include "clitable.h"
#include "libclidef.h"
#include "trmdef.h"

/* The process context the system services keep their channel table in (an
 * image the executive activates may not have one yet: the same bootstrap DCL
 * and the network images do). */
struct vms_pcb;
extern struct vms_pcb *vms_pcb_get(void);
extern struct vms_pcb *vms_pcb_init(uint64_t initial_privs);

#define RTPAD_EFN      21u      /* the wait's timer flag (a local flag) */
#define RTPAD_IDLE_MS  20u      /* between looks at the link and the terminal */
#define RTPAD_DRAIN    64       /* link reads per turn before the terminal */

struct rtpad {
    uint16_t net;               /* _NET: channel of the CTERM link */
    uint16_t ch_in, ch_out;     /* SYS$INPUT, SYS$OUTPUT */
    uint16_t ch_tt;             /* TT:, when SYS$INPUT is the terminal */
    struct dnet_cterm_inq inq;  /* a file SYS$INPUT, split into lines */
};

static void mkdesc(struct dsc$descriptor_s *d, const char *s, size_t n)
{
    d->dsc$w_length = (uint16_t)n;
    d->dsc$b_dtype = DSC$K_DTYPE_T;
    d->dsc$b_class = DSC$K_CLASS_S;
    d->dsc$a_pointer = (char *)s;
}

static void mkdesc_z(struct dsc$descriptor_s *d, const char *s)
{
    mkdesc(d, s, strlen(s));
}

/* ---- the terminal --------------------------------------------------------- */

static void out_bytes(uint16_t chan, const void *b, size_t n, uint32_t cc)
{
    struct _iosb iosb;
    (void)sys$qiow(0, chan, IO$_WRITEVBLK, &iosb, NULL, 0, (void *)b, (uint32_t)n,
                   0, cc, 0, 0);
}

static void io_show(void *ctx, const uint8_t *b, size_t n)
{
    struct rtpad *r = ctx;
    out_bytes(r->ch_tt ? r->ch_tt : r->ch_out, b, n, 0);
}

/* One VMS record (carriage control " "): the driver puts it on a line of its
 * own, wherever the cursor is (rd vms-a353). */
static void io_record(void *ctx, const char *msg)
{
    struct rtpad *r = ctx;
    out_bytes(r->ch_tt ? r->ch_tt : r->ch_out, msg, strlen(msg), 0x20);
}

static uint32_t io_read_line(void *ctx, const uint8_t *prompt, size_t plen,
                             const uint8_t *init, size_t ilen,
                             uint8_t *buf, size_t cap, int echo,
                             int term_echo, unsigned timeout_s, size_t *n,
                             uint8_t *term)
{
    struct rtpad *r = ctx;
    struct _iosb iosb;
    uint32_t st;
    memset(&iosb, 0, sizeof iosb);
    if (ilen) {
        /* initial input (DCL's recalled command): the extended read, its
         * prompt and TRM$_INISTRNG in the item list */
        struct ovmx_trm_item it[4];
        size_t ni = 0;
        uint32_t mods = 0;
        if (!echo) mods |= TRM$M_TM_NOECHO;
        if (!term_echo) mods |= TRM$M_TM_TRMNOECHO;
        if (timeout_s) mods |= TRM$M_TM_TIMED;
        memset(it, 0, sizeof it);
        it[ni].code = TRM$_MODIFIERS; it[ni].val = mods; ni++;
        if (timeout_s) { it[ni].code = TRM$_TIMEOUT; it[ni].val = timeout_s; ni++; }
        if (plen) {
            it[ni].code = TRM$_PROMPT; it[ni].len = (unsigned short)plen;
            it[ni].val = (uintptr_t)prompt; ni++;
        }
        it[ni].code = TRM$_INISTRNG; it[ni].len = (unsigned short)ilen;
        it[ni].val = (uintptr_t)init; ni++;
        st = sys$qiow(0, r->ch_tt, IO$_READVBLK | IO$M_EXTEND, &iosb, NULL, 0, buf,
                      (uint32_t)cap, 0, 0, (uintptr_t)it, (uint32_t)(ni * sizeof it[0]));
    } else {
        /* the host's prompt goes with the read (P5/P6), as VMS's driver shows it */
        uint32_t func = plen ? IO$_READPROMPT : IO$_READVBLK;
        if (!echo)
            func |= IO$M_NOECHO;
        if (!term_echo)
            func |= IO$M_TRMNOECHO;
        if (timeout_s)
            func |= IO$M_TIMED;            /* P3 = the host's TIMEOUT, seconds */
        st = sys$qiow(0, r->ch_tt, func, &iosb, NULL, 0, buf, (uint32_t)cap,
                      timeout_s, 0, (uintptr_t)prompt, (uint32_t)plen);
    }
    if (st & 1)
        st = iosb.iosb$w_status;
    if (st == SS$_TIMEOUT) {
        *n = iosb.iosb$w_bcnt;         /* what was typed before the time ran out */
        return st;
    }
    if (!(st & 1))
        return st;
    *n = iosb.iosb$w_bcnt;
    /* IOSB words 2/3: the terminator and its size. A read that ended without
     * one (the buffer filled) goes as a full line, with CR. */
    *term = (iosb.iosb$l_dev_depend >> 16) ? (uint8_t)(iosb.iosb$l_dev_depend & 0xff)
                                           : 0x0d;
    return SS$_NORMAL;
}

/* CTRL/Y, CTRL/C and whatever the host makes out of band: IO$M_OUTBAND on
 * the terminal, the AST carrying the character (P2/P4: a quadword, its
 * second longword the mask). The AST hands it to the session. */
static struct rtpad_session *g_oob_session;

static void oob_ast(uint32_t c)
{
    if (g_oob_session)
        rtpad_session_oob(g_oob_session, (uint8_t)c);
}

static void io_set_oob(void *ctx, uint32_t mask)
{
    struct rtpad *r = ctx;
    static uint32_t mq[2];
    struct _iosb iosb;
    if (!r->ch_tt)
        return;
    mq[0] = 0;
    mq[1] = mask;
    (void)sys$qiow(0, r->ch_tt, IO$_SETMODE | IO$M_OUTBAND, &iosb, NULL, 0,
                   mask ? (void *)oob_ast : NULL, 0, 3, (uintptr_t)mq, 0, 0);
}

/* THIS terminal, as the driver reports it (IO$_SENSEMODE: class, type,
 * width, characteristics with the page length, extended characteristics). */
static int sense_terminal(struct rtpad *r, uint8_t sense[12])
{
    struct _iosb iosb;
    memset(sense, 0, 12);
    memset(&iosb, 0, sizeof iosb);
    uint32_t st = sys$qiow(0, r->ch_tt, IO$_SENSEMODE, &iosb, NULL, 0, sense, 12,
                           0, 0, 0, 0);
    if (st & 1)
        st = iosb.iosb$w_status;
    if (!(st & 1) || sense[0] != DC$_TERM)
        return 0;
    /* OVMX's SENSEMODE does not yet return the TT$/TT2$ characteristics in
     * bytes 4-6 and 8-11 (it returns device characteristics there: rd
     * vms-686), and a VMS host would act on them -- a real VAX showed the
     * session's RTA as Passall. Until it does, those bytes are the ones a
     * real VAX client reported (oracle docs/oracle/vax-sethost-cterm.*:
     * TTSYNC LOWER WRAP / 00 32 00 00); class, type, width and page are this
     * terminal's. */
    static const uint8_t oracle_tt[3] = { 0xa0, 0x02, 0x00 };
    static const uint8_t oracle_tt2[4] = { 0x00, 0x32, 0x00, 0x00 };
    memcpy(sense + 4, oracle_tt, sizeof oracle_tt);
    memcpy(sense + 8, oracle_tt2, sizeof oracle_tt2);
    return 1;
}

/* A file SYS$INPUT: its bytes, split into lines; one line per call. */
static uint32_t io_read_record(void *ctx, uint8_t *buf, size_t cap, size_t *n)
{
    struct rtpad *r = ctx;
    for (;;) {
        if (dnet_cterm_inq_dequeue(&r->inq, buf, cap, n))
            return SS$_NORMAL;
        if (r->inq.eof)
            return RMS$_EOF;
        uint8_t in[DNET_CTERM_MAX_DATA];
        struct _iosb iosb;
        memset(&iosb, 0, sizeof iosb);
        uint32_t st = sys$qiow(0, r->ch_in, IO$_READVBLK, &iosb, NULL, 0, in,
                               (uint32_t)sizeof in, 0, 0, 0, 0);
        uint32_t got = (st & 1) ? iosb.iosb$l_dev_depend : 0;
        if ((st & 1) && got > 0) {
            if (got > sizeof in)
                got = sizeof in;
            (void)dnet_cterm_inq_feed(&r->inq, in, got);
            /* a record read ends its line */
            if (in[got - 1] != '\n' && in[got - 1] != '\r')
                (void)dnet_cterm_inq_feed(&r->inq, (const uint8_t *)"\r", 1);
        } else {
            dnet_cterm_inq_eof(&r->inq);
        }
    }
}

/* Has the terminal driver a line started in its type-ahead buffer? */
static int term_ready(struct rtpad *r)
{
    uint8_t tc[8];
    struct _iosb iosb;
    memset(tc, 0, sizeof tc);
    memset(&iosb, 0, sizeof iosb);
    uint32_t st = sys$qiow(0, r->ch_tt, IO$_SENSEMODE | IO$M_TYPEAHDCNT, &iosb,
                           NULL, 0, tc, (uint32_t)sizeof tc, 0, 0, 0, 0);
    if (st & 1)
        st = iosb.iosb$w_status;
    if (!(st & 1))
        return 1;          /* the driver cannot say: let the read report it */
    return (tc[0] | (tc[1] << 8)) != 0;
}

/* Is SYS$INPUT the job's terminal? Asked of the executive's device row. */
static int sysinput_is_terminal(void)
{
    struct dsc$descriptor_s d;
    uint32_t cls = 0;
    uint16_t rl = 0;
    ILE3 il[2];
    struct _iosb iosb;
    mkdesc_z(&d, "SYS$INPUT");
    memset(il, 0, sizeof il);
    il[0].ile3$w_length = sizeof cls;
    il[0].ile3$w_code = DVI$_DEVCLASS;
    il[0].ile3$ps_bufaddr = &cls;
    il[0].ile3$ps_retlen_addr = &rl;
    memset(&iosb, 0, sizeof iosb);
    uint32_t st = sys$getdviw(0, 0, &d, il, &iosb, NULL, 0, 0);
    return (st & 1) && cls == DC$_TERM;
}

static uint32_t open_terminal(struct rtpad *r)
{
    struct dsc$descriptor_s din, dout, dtt;
    uint32_t st;
    mkdesc_z(&din, "SYS$INPUT:");
    mkdesc_z(&dout, "SYS$OUTPUT:");
    st = sys$assign(&dout, &r->ch_out, 0, NULL, 0);
    if (!(st & 1))
        return st;
    if (sysinput_is_terminal()) {
        /* Read through the TERMINAL UNIT, on which a read takes IO$M_NOECHO:
         * without it a remote Password: would be echoed, so a terminal whose
         * channel cannot be had refuses SET HOST (rd vms-a353). */
        mkdesc_z(&dtt, "TT:");
        return sys$assign(&dtt, &r->ch_tt, 0, NULL, 0);
    }
    st = sys$assign(&din, &r->ch_in, 0, NULL, 0);
    dnet_cterm_inq_init(&r->inq);
    return st;
}

/* ---- the network ----------------------------------------------------------- */

static uint32_t io_link_send(void *ctx, const uint8_t *pdu, size_t n)
{
    struct rtpad *r = ctx;
    return dnet_net_qiow(r->net, IO$_WRITEVBLK, (void *)pdu, (uint32_t)n, NULL);
}

static uint32_t assign_net(uint16_t *chan)
{
    struct dsc$descriptor_s d;
    mkdesc_z(&d, "_NET:");
    return sys$assign(&d, chan, 0, NULL, 0);
}

/* This node's name, as the RUNNING NETACP holds its executor (NCP SHOW
 * EXECUTOR's source: _NET: IO$_ACPCONTROL). "a.n" when it has no name. */
static void local_node(char *out, size_t cap)
{
    uint16_t ch = 0;
    uint8_t buf[1100];
    size_t qlen = 0;
    struct dnet_netshow_req q = { DNET_NETSHOW_VERSION, DNET_NETSHOW_ENT_EXECUTOR, 0 };
    static struct dnet_netshow_rsp rsp;
    out[0] = '\0';
    if (!(assign_net(&ch) & 1))
        return;
    if (dnet_netshow_req_encode(&q, buf, sizeof buf, &qlen) == DNET_NETSHOW_OK) {
        struct _iosb iosb;
        memset(&iosb, 0, sizeof iosb);
        uint32_t st = sys$qiow(0, ch, IO$_ACPCONTROL, &iosb, NULL, 0, buf,
                               (uint32_t)sizeof buf, (uint32_t)qlen, 0, 0, 0);
        if (st & 1)
            st = iosb.iosb$w_status;
        if ((st & 1) && dnet_netshow_rsp_decode(buf, iosb.iosb$l_dev_depend, &rsp)
                            == DNET_NETSHOW_OK) {
            if (rsp.exec.name[0]) {
                size_t k = strlen(rsp.exec.name);
                if (k >= cap) k = cap - 1;
                memcpy(out, rsp.exec.name, k);
                out[k] = '\0';
            } else {
                /* area.node, in decimal */
                unsigned a = rsp.exec.addr >> 10, n = rsp.exec.addr & 0x3ff;
                char t[12];
                size_t k = 0, j;
                char rev[12];
                size_t rk = 0;
                do { rev[rk++] = (char)('0' + a % 10); a /= 10; } while (a);
                while (rk) t[k++] = rev[--rk];
                t[k++] = '.';
                do { rev[rk++] = (char)('0' + n % 10); n /= 10; } while (n);
                while (rk) t[k++] = rev[--rk];
                for (j = 0; j < k && j + 1 < cap; j++) out[j] = t[j];
                out[j] = '\0';
            }
        }
    }
    (void)sys$dassgn(ch);
}

/* ---- the command ----------------------------------------------------------- */

/* One CLI$GET_VALUE into a NUL-terminated buffer; 0 when absent. */
static size_t cli_value(const char *label, char *out, size_t cap)
{
    struct dsc$descriptor_s l, v;
    uint16_t len = 0;
    mkdesc_z(&l, label);
    mkdesc(&v, out, cap - 1);
    uint32_t st = cli$get_value(&l, &v, &len);
    if (!(st & 1))
        len = 0;
    out[len] = '\0';
    return len;
}

/* ---- reporting ------------------------------------------------------------- */

/* The link could not be had: exit with the condition, as VMS RTPAD does --
 * DCL shows its message once and $STATUS is that value, with no
 * STS$M_INHIB_MSG (VAX V7.3: SET HOST NOSUCH -> "%SYSTEM-F-NOSUCHNODE, remote
 * node is unknown", $STATUS %X0000028C; tests/lab/captures/vms-0a7-s2-20261103). */
static void fail_exit(struct rtpad *r, uint32_t st)
{
    if (r->ch_tt) (void)sys$dassgn(r->ch_tt);
    if (r->ch_in) (void)sys$dassgn(r->ch_in);
    if (r->ch_out) (void)sys$dassgn(r->ch_out);
    sys$exit(st);
}

static void idle(void)
{
    /* a VMS delta time: negative, in 100 ns units */
    uint64_t delta = (uint64_t)(-(int64_t)RTPAD_IDLE_MS * 10000);
    if (sys$setimr(RTPAD_EFN, &delta, NULL, 0, 0) & 1)
        (void)sys$wflor(RTPAD_EFN, 1u << (RTPAD_EFN & 31));
    /* an out-of-band character typed while no read ran: its AST now */
    (void)sys$setast(1);
}

int main(void)
{
    static struct rtpad r;
    static struct rtpad_session s;
    char node[64], user[64], here[16];
    char ncb[160];
    size_t k = 0;

    memset(&r, 0, sizeof r);
    if (!vms_pcb_get())
        vms_pcb_init(0);
    uint32_t st = open_terminal(&r);
    if (!(st & 1))
        fail_exit(&r, st);

    /* The command DCL parsed against its tables (DCL$TABLES: SET HOST). */
    if (cli_value("NODE", node, sizeof node) == 0)
        fail_exit(&r, SS$_BADPARAM);
    user[0] = '\0';
    {
        struct dsc$descriptor_s l;
        mkdesc_z(&l, "USERNAME");
        if (cli$present(&l) & 1)
            (void)cli_value("USERNAME", user, sizeof user);
    }

    /* The NCB: node, the access-control user when one was given (the remote
     * LOGINOUT authenticates fresh either way), and object 42. */
    {
        size_t nl = strlen(node), ul = strlen(user);
        /* a node given as NODE:: names the same node */
        while (nl && node[nl - 1] == ':')
            nl--;
        if (nl == 0 || nl + ul + 16 > sizeof ncb)
            fail_exit(&r, SS$_BADPARAM);
        memcpy(ncb, node, nl); k = nl;
        if (ul) {
            ncb[k++] = '"';
            memcpy(ncb + k, user, ul); k += ul;
            ncb[k++] = '"';
        }
        memcpy(ncb + k, "::\"42=\"", 7); k += 7;
        ncb[k] = '\0';
    }

    local_node(here, sizeof here);

    st = assign_net(&r.net);
    if (!(st & 1))
        fail_exit(&r, st);
    st = dnet_net_qiow(r.net, IO$_ACCESS, ncb, (uint32_t)k, NULL);
    if (!(st & 1)) {
        (void)sys$dassgn(r.net);
        fail_exit(&r, st);
    }

    static const struct rtpad_io io_ops = {
        NULL, io_link_send, io_show, io_record, io_read_line, io_read_record,
        io_set_oob
    };
    static struct rtpad_io io;
    io = io_ops;
    io.ctx = &r;
    g_oob_session = &s;
    if (rtpad_session_init(&s, &io, r.ch_tt != 0) != 0)
        fail_exit(&r, SS$_ABORT);
    if (r.ch_tt) {
        uint8_t sense[12];
        if (sense_terminal(&r, sense))
            rtpad_session_set_sense(&s, sense);
    }
    rtpad_session_linkup(&s);

    while (!s.done) {
        for (int i = 0; i < RTPAD_DRAIN && !s.done; i++) {
            uint8_t buf[1100];
            size_t got = 0;
            st = dnet_net_qiow(r.net, IO$_READVBLK | IO$M_NOW, buf,
                               (uint32_t)sizeof buf, &got);
            if (st == SS$_ENDOFFILE)
                break;
            if (!(st & 1)) {
                s.done = 1;            /* the host unbound, or the link went */
                break;
            }
            rtpad_session_rx(&s, buf, got);
        }
        if (s.done)
            break;
        if (rtpad_session_wants_input(&s) &&
            (!r.ch_tt || rtpad_session_read_now(&s) || term_ready(&r)))
            rtpad_session_input(&s);
        else
            idle();
    }

    io_set_oob(&r, 0);                     /* the local terminal is DCL's again */
    g_oob_session = NULL;
    rtpad_session_finish(&s, here[0] ? here : "0");
    (void)dnet_net_qiow(r.net, IO$_DEACCESS, NULL, 0, NULL);
    (void)sys$dassgn(r.net);
    if (r.ch_tt) (void)sys$dassgn(r.ch_tt);
    if (r.ch_in) (void)sys$dassgn(r.ch_in);
    (void)sys$dassgn(r.ch_out);
    sys$exit((s.status & 1) ? SS$_NORMAL : (s.status | STS$M_INHIB_MSG));
    return 0;
}
