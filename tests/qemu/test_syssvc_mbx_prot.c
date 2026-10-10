/*
 * test_syssvc_mbx_prot.c - mailbox PROTECTION, the writer's PID, and the
 * non-waiting write, against the real executive (rd vms-c6d1).
 *
 * WHY. NETACP takes requests -- including the access-control passwords of a
 * COPY node"user pw":: -- through an executive mailbox. Before this item a
 * mailbox had no protection: any local process could $ASSIGN it by name and
 * destructively READ another process's request, and a client could stall the
 * server's reply write by never draining its own mailbox. VMS answers both with
 * features every mailbox has (System Services Reference, $CREMBX; I/O User's
 * Reference, Mailbox Driver), and this suite proves OVMX's executive now has them:
 *
 *   - $CREMBX's promsk is a SOGW protection mask owned by the creator's UIC, and
 *     the executive's ONE protection decision (src/kernel-core/vms_prot.h, the
 *     same one the Files-11 ACP applies to a file) gates every read and write,
 *     with the privileges enabled at that I/O. $ASSIGN itself is NOT checked:
 *     real VAX V7.3 and Alpha V8.4 assign a channel to a mailbox the caller may
 *     neither read nor write (docs/oracle/semantics/mbxprot/, MBXP.ALL.ASSIGN).
 *       * write right only (the request-mailbox shape S:RWLP,O:RWLP,G,W:W)
 *                                          -> the write, but the READ is NOPRIV
 *       * read right only                  -> the read, but the WRITE is NOPRIV
 *     BYPASS and SYSPRV lift it as they do for a file; READALL does NOT open a
 *     mailbox (MBXP.READALL.READ on both real systems).
 *   - every message carries the writer's VMS PID, stamped by the executive (the
 *     reader's IOSB second longword) -- not something the writer can assert.
 *   - IO$M_NORSWAIT: a write to a mailbox with no room completes at once with
 *     SS$_MBFULL instead of waiting for a reader.
 *
 * SHAPE. The parent (root: UIC group 0, a SYSTEM-category accessor) creates the
 * mailboxes. A re-exec'd child -- a separate process with its own PCB -- drops
 * itself to the unprivileged UIC [100,100] holding only SETPRV (VMS_IOCTL_SETIDENT,
 * as test_syssvc_privilege_enforce does), so it is in the WORLD category of every
 * mailbox, and reports each verdict over a pipe. Single privilege bits are then
 * switched on and off one at a time, so every verdict is decided by one bit.
 *
 * NEGATIVE CONTROLS (tests/qemu/facility_defects.sh): mbx-prot-read-unchecked,
 * mbx-prot-write-unchecked, mbx-readall-grants-read, crembx-promsk-dropped,
 * mbx-sender-pid-not-stamped, mbx-norswait-ignored; and the privilege overrides
 * acp-bypass-ignored / acp-sysprv-ignored, which mutate the shared vms_prot.h
 * and so redden this suite AND the file one.
 *
 * No /dev/vms -> honest SKIP (77), never a fabricated pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>

#include "starlet.h"
#include "descrip.h"
#include "iodef.h"
#include "iosbdef.h"
#include "ssdef.h"
#include "prvdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77
#define PEER_TIMEOUT_MS 30000

/* Protection masks (a SET bit DENIES; nibbles System, Owner, Group, World; per
 * nibble bit0 R, bit1 W, bit2 L, bit3 P). The child [100,100] is WORLD to all. */
#define PROT_REQUEST 0xDF00u   /* S:RWLP,O:RWLP,G:,W:W  -- the NETACP request shape */
#define PROT_READER  0xEF00u   /* S:RWLP,O:RWLP,G:,W:R  -- read-only for the world */
#define PROT_PRIVATE 0xFF00u   /* S:RWLP,O:RWLP,G:,W:   -- nothing for the world */

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

/* The mailboxes the parent made, handed to the child. */
struct mbx_names {
    char request[32];   /* PROT_REQUEST */
    char reader[32];    /* PROT_READER  */
    char priv[32];      /* PROT_PRIVATE, made through vms_kif */
    char sysmbx[32];    /* PROT_PRIVATE, made through sys$crembx(promsk) */
};

/* The child's verdicts. */
struct verdicts {
    uint32_t pid;                   /* the child's own VMS PID ($GETJPI) */
    uint32_t setident;
    uint32_t priv_assign;           /* PROT_PRIVATE: a channel (assign unchecked) */
    uint32_t priv_read;             /*   read: SS$_NOPRIV */
    uint32_t priv_write;            /*   write: SS$_NOPRIV */
    uint32_t sys_write;             /* the sys$crembx(promsk) one, via sys$qiow */
    uint32_t req_assign;            /* PROT_REQUEST: a channel */
    uint32_t req_write[6];          /* six requests: each SS$_NORMAL */
    uint32_t req_read;              /* unprivileged read: SS$_NOPRIV */
    uint32_t rdr_assign;            /* PROT_READER: a channel */
    uint32_t rdr_read;              /* empty + IO$M_NOW: SS$_ENDOFFILE (allowed) */
    uint32_t rdr_write;             /* SS$_NOPRIV */
    uint32_t readall_read;          /* READALL alone: still SS$_NOPRIV on a mailbox */
    uint32_t bypass_read;           /* BYPASS alone: the request read is granted */
    uint32_t bypass_write;          /* BYPASS alone: the private write is granted */
    uint32_t bypass_off_read;       /* BYPASS off again: SS$_NOPRIV */
    uint32_t sysprv_read;           /* SYSPRV alone: SYSTEM category -> granted */
    uint32_t sysprv_write;          /* SYSPRV alone: the private write is granted */
    uint32_t sysprv_off_write;      /* SYSPRV off again: SS$_NOPRIV */
};

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static int read_bounded(int fd, void *buf, size_t len, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    size_t got = 0;
    while (got < len) {
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) return 0;
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n <= 0) return 0;
        got += (size_t)n;
    }
    return 1;
}

static void priv(uint64_t bit, int on)
{
    uint64_t prev = 0;
    (void)vms_kif_setprv(bit, on, 0, &prev);
}

static uint32_t read_now(uint32_t ch)
{
    char buf[64];
    uint32_t n = 0;
    return vms_kif_mbx_read(ch, buf, sizeof(buf), &n, 1);
}

/* The unprivileged child. */
static int child_main(int rfd, int wfd)
{
    struct mbx_names nm;
    struct verdicts v;
    struct vms_procinfo pi;
    uint32_t req = 0, rdr = 0, prv = 0;
    int i;

    memset(&v, 0, sizeof(v));
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL))
        return 1;
    if (!read_bounded(rfd, &nm, sizeof(nm), PEER_TIMEOUT_MS))
        return 1;

    memset(&pi, 0, sizeof(pi));
    if ($VMS_STATUS_SUCCESS(vms_kif_getjpi_self(&pi)))
        v.pid = pi.vms_pid;

    /* World category of every mailbox below, no privilege but SETPRV. */
    v.setident = vms_kif_setident("MBXPROT", (100u << 16) | 100u, PRV$M_SETPRV);

    v.priv_assign = vms_kif_mbx_assign(nm.priv, &prv);
    v.priv_read = read_now(prv);
    v.priv_write = vms_kif_mbx_write(prv, "P", 1);
    {
        struct dsc$descriptor_s d = mkdsc(nm.sysmbx);
        struct _iosb iosb;
        uint16_t c = 0;
        uint32_t st = sys$assign(&d, &c, 0, NULL);
        if ($VMS_STATUS_SUCCESS(st)) {
            memset(&iosb, 0, sizeof(iosb));
            st = sys$qiow(0, c, IO$_WRITEVBLK | IO$M_NOW, &iosb, NULL, 0, (void *)"S", 1,
                          0, 0, 0, 0);
            if ($VMS_STATUS_SUCCESS(st))
                st = iosb.iosb$w_status;
            (void)sys$dassgn(c);
        }
        v.sys_write = st;
    }

    v.req_assign = vms_kif_mbx_assign(nm.request, &req);
    for (i = 0; i < 6; i++) {
        char msg[16];
        snprintf(msg, sizeof(msg), "REQ%d", i + 1);
        v.req_write[i] = vms_kif_mbx_write(req, msg, (uint32_t)strlen(msg));
    }
    v.req_read = read_now(req);

    v.rdr_assign = vms_kif_mbx_assign(nm.reader, &rdr);
    v.rdr_read = read_now(rdr);
    v.rdr_write = vms_kif_mbx_write(rdr, "X", 1);

    priv(PRV$M_READALL, 1);
    v.readall_read = read_now(req);
    priv(PRV$M_READALL, 0);

    priv(PRV$M_BYPASS, 1);
    v.bypass_read = read_now(req);
    v.bypass_write = vms_kif_mbx_write(prv, "B", 1);
    priv(PRV$M_BYPASS, 0);
    v.bypass_off_read = read_now(req);

    priv(PRV$M_SYSPRV, 1);
    v.sysprv_read = read_now(req);
    v.sysprv_write = vms_kif_mbx_write(prv, "Y", 1);
    priv(PRV$M_SYSPRV, 0);
    v.sysprv_off_write = vms_kif_mbx_write(prv, "Z", 1);

    if (write(wfd, &v, sizeof(v)) != (ssize_t)sizeof(v))
        return 1;
    /* Hold the channels until the parent has read what is left. */
    {
        char go;
        (void)!read(rfd, &go, 1);
    }
    if (req) (void)vms_kif_dassgn((uint16_t)req);
    if (rdr) (void)vms_kif_dassgn((uint16_t)rdr);
    if (prv) (void)vms_kif_dassgn((uint16_t)prv);
    return 0;
}

static void on_alarm(int sig)
{
    (void)sig;
    printf("  FAIL: an IO$M_NORSWAIT write to a full mailbox completes at once (SS$_MBFULL), never waits for a reader\n");
    printf("=== test_syssvc_mbx_prot: %d passed, %d failed ===\n", pass, fail + 1);
    fflush(stdout);
    _exit(1);
}

static uint32_t mk(uint32_t promsk, uint32_t *chan, char *dev, size_t devsz)
{
    uint32_t unit = 0;
    return vms_kif_mbx_create_prot(0, 64, 1024, promsk, chan, &unit, dev, (uint32_t)devsz);
}

int main(int argc, char **argv)
{
    struct mbx_names nm;
    struct verdicts v;
    uint32_t req_c = 0, rdr_c = 0, prv_c = 0;
    uint16_t sys_c = 0;
    int p2c[2], c2p[2];
    pid_t pid;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    if (argc >= 4 && strcmp(argv[1], "--child") == 0)
        return child_main(atoi(argv[2]), atoi(argv[3]));

    printf("=== test_syssvc_mbx_prot: mailbox protection, sender PID, IO$M_NORSWAIT (vms-c6d1) ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_mbx_prot: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    memset(&nm, 0, sizeof(nm));
    check($VMS_STATUS_SUCCESS(mk(PROT_REQUEST, &req_c, nm.request, sizeof(nm.request))),
          "$CREMBX a request mailbox S:RWLP,O:RWLP,G,W:W");
    check($VMS_STATUS_SUCCESS(mk(PROT_READER, &rdr_c, nm.reader, sizeof(nm.reader))),
          "$CREMBX a mailbox S:RWLP,O:RWLP,G,W:R");
    check($VMS_STATUS_SUCCESS(mk(PROT_PRIVATE, &prv_c, nm.priv, sizeof(nm.priv))),
          "$CREMBX a mailbox S:RWLP,O:RWLP,G,W: (no world access)");
    {
        /* The same mask through the PUBLIC service: sys$crembx must carry promsk to
         * the executive (it used to discard it). Found again by its LNM$SYSTEM name. */
        struct dsc$descriptor_s ln = mkdsc("OVMX$C6D1_PROT_MBX");
        uint32_t st = sys$crembx(0, &sys_c, 64, 1024, PROT_PRIVATE, 0, &ln);
        check($VMS_STATUS_SUCCESS(st), "sys$crembx with promsk S:RWLP,O:RWLP,G,W: and a logical name");
        snprintf(nm.sysmbx, sizeof(nm.sysmbx), "OVMX$C6D1_PROT_MBX");
    }

    if (pipe(p2c) < 0 || pipe(c2p) < 0) { printf("  FAIL: pipe()\n"); return 1; }
    pid = fork();
    if (pid < 0) { printf("  FAIL: fork()\n"); return 1; }
    if (pid == 0) {
        char r[16], w[16];
        close(p2c[1]); close(c2p[0]);
        snprintf(r, sizeof(r), "%d", p2c[0]);
        snprintf(w, sizeof(w), "%d", c2p[1]);
        execl(argv[0], argv[0], "--child", r, w, (char *)NULL);
        _exit(1);
    }
    close(p2c[0]); close(c2p[1]);
    (void)!write(p2c[1], &nm, sizeof(nm));

    memset(&v, 0, sizeof(v));
    if (!read_bounded(c2p[0], &v, sizeof(v), PEER_TIMEOUT_MS)) {
        printf("  FAIL: the unprivileged child never reported\n");
        fail++;
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        goto summary;
    }

    check($VMS_STATUS_SUCCESS(v.setident), "child: SETIDENT to [100,100] holding only SETPRV");

    check(v.priv_assign == SS$_NORMAL,
          "an unprivileged $ASSIGN of a no-world-access mailbox yields a channel (VMS checks each I/O, not the assign)");
    /* negctl-knockon: mbx-prot-read-unchecked */
    check(v.priv_read == SS$_NOPRIV,
          "an unprivileged READ of a no-world-access mailbox is SS$_NOPRIV");
    /* negctl-knockon: mbx-prot-write-unchecked */
    check(v.priv_write == SS$_NOPRIV,
          "an unprivileged WRITE to a no-world-access mailbox is SS$_NOPRIV");
    /* negctl: crembx-promsk-dropped */
    /* negctl-knockon: mbx-prot-write-unchecked */
    check(v.sys_write == SS$_NOPRIV,
          "the promsk given to sys$crembx is enforced: an unprivileged $QIOW write through sys$assign is SS$_NOPRIV");

    check(v.req_assign == SS$_NORMAL,
          "unprivileged $ASSIGN of the write-only (W:W) request mailbox yields a channel");
    check(v.req_write[0] == SS$_NORMAL && v.req_write[1] == SS$_NORMAL &&
          v.req_write[2] == SS$_NORMAL && v.req_write[3] == SS$_NORMAL &&
          v.req_write[4] == SS$_NORMAL && v.req_write[5] == SS$_NORMAL,
          "an unprivileged client may WRITE requests into the W:W mailbox");
    /* negctl: mbx-prot-read-unchecked */
    check(v.req_read == SS$_NOPRIV,
          "an unprivileged client may NOT READ the W:W request mailbox (SS$_NOPRIV) -- no other client's request is readable");

    check(v.rdr_assign == SS$_NORMAL,
          "unprivileged $ASSIGN of the read-only (W:R) mailbox yields a channel");
    check(v.rdr_read == SS$_ENDOFFILE,
          "an unprivileged IO$M_NOW READ of the empty W:R mailbox is allowed (SS$_ENDOFFILE)");
    /* negctl: mbx-prot-write-unchecked */
    check(v.rdr_write == SS$_NOPRIV,
          "an unprivileged WRITE to the W:R mailbox is SS$_NOPRIV");

    /* negctl: mbx-readall-grants-read */
    /* negctl-knockon: mbx-prot-read-unchecked */
    check(v.readall_read == SS$_NOPRIV,
          "READALL alone does NOT open a read-denied mailbox (SS$_NOPRIV, as on real VAX V7.3 and Alpha V8.4)");
    /* negctl-knockon: acp-bypass-ignored */
    check(v.bypass_read == SS$_NORMAL,
          "BYPASS alone grants the read of the W:W request mailbox");
    /* negctl-knockon: acp-bypass-ignored */
    check(v.bypass_write == SS$_NORMAL,
          "BYPASS alone grants a write to the no-world-access mailbox");
    /* negctl-knockon: mbx-prot-read-unchecked */
    check(v.bypass_off_read == SS$_NOPRIV, "BYPASS off again: the read is refused");
    /* negctl-knockon: acp-sysprv-ignored */
    check(v.sysprv_read == SS$_NORMAL,
          "SYSPRV alone grants the read of the W:W request mailbox (SYSTEM protection category)");
    /* negctl-knockon: acp-sysprv-ignored */
    check(v.sysprv_write == SS$_NORMAL,
          "SYSPRV alone grants a write to the no-world-access mailbox (SYSTEM protection category)");
    /* negctl-knockon: mbx-prot-write-unchecked */
    check(v.sysprv_off_write == SS$_NOPRIV, "SYSPRV off again: the write is refused");

    /* The privileged reader (the parent, SYSTEM category) reads what the child
     * wrote, and the executive -- not the child -- says who wrote it. */
    {
        char buf[64];
        uint32_t n = 0, sender = 0;
        uint32_t st = vms_kif_mbx_read_ex(req_c, buf, sizeof(buf), &n, 1, &sender);
        check(st == SS$_NORMAL && n == 4 && memcmp(buf, "REQ", 3) == 0,
              "the SYSTEM-category creator reads a request the unprivileged client wrote");
        /* negctl: mbx-sender-pid-not-stamped */
        check(v.pid != 0 && sender == v.pid,
              "the request carries the WRITER's VMS PID, stamped by the executive (IOSB second longword)");
    }

    /* $CHECK_ACCESS-shaped (rd vms-046): the executive answers for the CHILD --
     * still alive, [100,100], no privilege enabled -- exactly what its own $QIOs
     * just got (docs/oracle/semantics/mbxown/: on VAX V7.3 a non-system user may
     * WRITE but not READ the W:W request mailbox). A server asks this before it
     * writes into a mailbox a requester named. */
    {
        uint32_t oc = 0;
        char open_dev[32] = "";
        (void)mk(0, &oc, open_dev, sizeof(open_dev));
        /* negctl: mbx-chkacc-read-unchecked */
        check(vms_kif_mbx_chkacc(nm.request, v.pid, VMS_MBX_ACC_READ) == SS$_NOPRIV,
              "CHKACC: the unprivileged child may NOT read the W:W request mailbox"
              " (SS$_NOPRIV, as its own read was)");
        check(vms_kif_mbx_chkacc(nm.request, v.pid, VMS_MBX_ACC_WRITE) == SS$_NORMAL,
              "CHKACC: the child may write the W:W request mailbox");
        check(vms_kif_mbx_chkacc(nm.reader, v.pid, VMS_MBX_ACC_READ) == SS$_NORMAL &&
              vms_kif_mbx_chkacc(nm.reader, v.pid, VMS_MBX_ACC_WRITE) == SS$_NOPRIV,
              "CHKACC: on the W:R mailbox the child may read and may not write");
        check(vms_kif_mbx_chkacc(nm.priv, v.pid, VMS_MBX_ACC_READ | VMS_MBX_ACC_WRITE)
                  == SS$_NOPRIV,
              "CHKACC: the child may not use the no-world-access mailbox");
        check(open_dev[0] && vms_kif_mbx_chkacc(open_dev, v.pid,
                                                VMS_MBX_ACC_READ | VMS_MBX_ACC_WRITE)
                  == SS$_NORMAL,
              "CHKACC: the child may read and write an open (promsk 0) mailbox");
        check(vms_kif_mbx_chkacc(nm.request, 0x7FFFFFF0u, VMS_MBX_ACC_READ) == SS$_NONEXPR,
              "CHKACC: no such process is SS$_NONEXPR");
        check(vms_kif_mbx_chkacc("MBA99999:", v.pid, VMS_MBX_ACC_READ) == SS$_NOSUCHDEV,
              "CHKACC: no such mailbox is SS$_NOSUCHDEV");
        check(vms_kif_mbx_chkacc(nm.request, v.pid, 0) == SS$_BADPARAM,
              "CHKACC: no access asked is SS$_BADPARAM");
        if (oc) (void)vms_kif_dassgn((uint16_t)oc);
    }
    (void)!write(p2c[1], "x", 1);
    waitpid(pid, NULL, 0);

    /* IO$M_NORSWAIT: a full mailbox answers SS$_MBFULL at once. A 10 s alarm turns a
     * write that waits into a named FAIL instead of a hung guest. */
    {
        uint32_t fc = 0, unit = 0, n = 0;
        char dev[32], buf[64];
        static const char forty[] = "0123456789012345678901234567890123456789";
        uint32_t st = vms_kif_mbx_create_prot(0, 40, 40, 0, &fc, &unit, dev, sizeof(dev));
        check($VMS_STATUS_SUCCESS(st), "$CREMBX a 40-byte mailbox with 40 bytes of buffer quota");
        check(vms_kif_mbx_write(fc, forty, 40) == SS$_NORMAL, "the first 40-byte message fills it");
        signal(SIGALRM, on_alarm);
        alarm(10);
        st = vms_kif_mbx_write_ex(fc, forty, 40, 1);
        alarm(0);
        /* negctl: mbx-norswait-ignored */
        check(st == SS$_MBFULL,
              "an IO$M_NORSWAIT write to a full mailbox completes at once (SS$_MBFULL), never waits for a reader");
        {
            struct _iosb iosb;
            uint16_t c = 0;
            struct dsc$descriptor_s d = mkdsc(dev);
            memset(&iosb, 0, sizeof(iosb));
            alarm(10);
            if ($VMS_STATUS_SUCCESS(sys$assign(&d, &c, 0, NULL))) {
                st = sys$qiow(0, c, IO$_WRITEVBLK | IO$M_NOW | IO$M_NORSWAIT, &iosb, NULL, 0,
                              (void *)forty, 40, 0, 0, 0, 0);
                (void)sys$dassgn(c);
            } else {
                st = 0;
            }
            alarm(0);
            check(st == SS$_MBFULL || iosb.iosb$w_status == SS$_MBFULL,
                  "$QIOW IO$_WRITEVBLK|IO$M_NOW|IO$M_NORSWAIT carries the modifier: SS$_MBFULL");
        }
        check(vms_kif_mbx_read(fc, buf, sizeof(buf), &n, 1) == SS$_NORMAL && n == 40,
              "reading the queued message makes room");
        check(vms_kif_mbx_write_ex(fc, forty, 40, 1) == SS$_NORMAL,
              "IO$M_NORSWAIT never refuses a write that fits");
        (void)vms_kif_dassgn((uint16_t)fc);
    }

summary:
    if (req_c) (void)vms_kif_dassgn((uint16_t)req_c);
    if (rdr_c) (void)vms_kif_dassgn((uint16_t)rdr_c);
    if (prv_c) (void)vms_kif_dassgn((uint16_t)prv_c);
    if (sys_c) (void)sys$dassgn(sys_c);
    printf("=== test_syssvc_mbx_prot: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
