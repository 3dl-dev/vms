/*
 * sys_qio.c - $QIO / $QIOW System Services
 *
 * VMS Queued I/O implemented on top of Linux io_uring for true async I/O.
 * $QIO submits via io_uring and returns immediately (truly asynchronous).
 * $QIOW submits via io_uring and waits for completion.
 *
 * Falls back to synchronous read()/write() if io_uring initialization fails.
 *
 * The I/O Status Block (IOSB) is filled with the completion status
 * and byte count after each operation, just as on real VMS.
 */

/*
 * OVMX service register (rd vms-d89) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * These two cited vms-1c57 until vms-fab; it is closed, and vms-6aa carries the
 * remainder -- shared with $ASSIGN/$DASSGN next door, because the channel and
 * the I/O queue are one facility.
 *
 * OVMX-PARTIAL: sys$qio (vms-6aa) -- exec: a request on a channel the executive
 *     issued (the console terminal) is routed to the executive's device, so the
 *     I/O goes where the device table says it goes.
 * OVMX-LOCAL: sys$qio -- the channel-to-fd mapping comes from this process's PCB,
 *     and the io_uring ring (and its read()/write() fallback) is this process's.
 *     There is no executive I/O queue, so no other process can see the request.
 * OVMX-PARTIAL: sys$qiow (vms-6aa) -- exec: the same executive-issued terminal
 *     channel path as $QIO.
 * OVMX-LOCAL: sys$qiow -- the same process-local PCB channel table and io_uring
 *     ring, plus a completion wait taken inside this process.
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include "ovmx_async.h"
#include "starlet.h"
#include "efndef.h"
#include "vms/pcb.h"
#include "ovmx_pcb_ctx.h"
#include "vms_kif.h"
#include "dcdef.h"
#include "ovmx_host_absent.h" /* POSIX timers/termios/... the OpenVMS C RTL lacks */

/* Import from sys_assign.c */
extern int vms$$chan_to_fd(uint16_t chan);
extern uint32_t vms$$chan_exec_chan(uint16_t chan);
extern int vms$$chan_is_mailbox(uint16_t chan);
extern int vms$$chan_is_bg(uint16_t chan);
extern int vms$$chan_is_net(uint16_t chan);

/* Import from sys_uring.c */
extern int vms_uring_init(void);
extern int vms_uring_submit_rw(int fd, void *buf, uint32_t len, uint64_t offset,
                                int is_read, void *iosb, uint32_t efn,
                                void (*astadr)(uint32_t), uint32_t astprm);
extern int vms_uring_wait_completion(void);
extern int vms_uring_process_completions(void);

/*
 * Try to ensure io_uring is initialized; returns 1 if available, 0 if not.
 */
static int uring_available(void) {
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return 0;
    if (pcb->uring_fd >= 0) return 1;
    return (vms_uring_init() == 0) ? 1 : 0;
}

/*
 * Synchronous fallback I/O - used when io_uring is not available.
 * Performs read/write and fills the IOSB directly.
 */

/* the null device NLA0: is /dev/null underneath. Compare device numbers with
 * the substrate's own /dev/null rather than hard-coding Linux's 1:3 (NetBSD
 * numbers it differently). */
static int qio_is_null_device(int fd)
{
    struct stat sb, nb;
    return fstat(fd, &sb) == 0 && S_ISCHR(sb.st_mode) &&
           stat("/dev/null", &nb) == 0 && S_ISCHR(nb.st_mode) &&
           sb.st_rdev == nb.st_rdev;
}

/*
 * IO$_WRITEOF on the null device completes at once with a count of 0, as on
 * real VAX V7.3 / Alpha V8.4 (docs/oracle/semantics/io/ IO.NL.WRITEOF; rd
 * vms-262a). Returns 1 when it handled the request.
 */
static int qio_null_writeof(uint16_t chan, uint32_t func, void *iosb_ptr,
                            uint32_t efn, void (*astadr)(uint32_t), uint32_t astprm)
{
    if ((func & IO$M_FCODE) != IO$_WRITEOF)
        return 0;
    int fd = vms$$chan_to_fd(chan);
    if (fd < 0 || !qio_is_null_device(fd))
        return 0;
    struct _iosb *iosb = (struct _iosb *)iosb_ptr;
    if (iosb) {
        iosb->iosb$w_status = (uint16_t)SS$_NORMAL;
        iosb->iosb$w_bcnt = 0;
        iosb->iosb$l_dev_depend = 0;
    }
    if ((efn & 0xFFu) < 128) sys$setef(efn);
    if (astadr) astadr(astprm);
    return 1;
}

static uint32_t qio_sync(int fd, uint32_t base_func, void *iosb_ptr,
                          void *p1, uint32_t p2, uint32_t efn,
                          void (*astadr)(uint32_t), uint32_t astprm) {
    struct _iosb *iosb = (struct _iosb *)iosb_ptr;
    ssize_t result;

    switch (base_func) {
        case IO$_READVBLK:
        case IO$_READLBLK:
        case IO$_READPBLK:
            if (!p1) {
                if (iosb) {
                    iosb->iosb$w_status = (uint16_t)SS$_BADPARAM;
                    iosb->iosb$w_bcnt = 0;
                    iosb->iosb$l_dev_depend = 0;
                }
                return SS$_BADPARAM;
            }
            result = read(fd, p1, p2);
            if (result < 0) {
                if (iosb) {
                    iosb->iosb$w_status = (uint16_t)SS$_ABORT;
                    iosb->iosb$w_bcnt = 0;
                    iosb->iosb$l_dev_depend = 0;
                }
                return SS$_ABORT;
            }
            if (result == 0) {
                if (iosb) {
                    iosb->iosb$w_status = (uint16_t)SS$_ENDOFFILE;
                    iosb->iosb$w_bcnt = 0;
                    iosb->iosb$l_dev_depend = 0;
                }
                return SS$_ENDOFFILE;
            }
            if (iosb) {
                iosb->iosb$w_status = (uint16_t)SS$_NORMAL;
                /* VMS IOSB byte count is 16-bit; transfers >65535 bytes
                 * are clamped.  The dev_depend field carries the full
                 * 32-bit count when callers need it. */
                iosb->iosb$w_bcnt = (result > 65535) ? 65535 : (uint16_t)result;
                iosb->iosb$l_dev_depend = (uint32_t)result;
            }
            break;

        case IO$_WRITEVBLK:
        case IO$_WRITELBLK:
        case IO$_WRITEPBLK:
            if (!p1) {
                if (iosb) {
                    iosb->iosb$w_status = (uint16_t)SS$_BADPARAM;
                    iosb->iosb$w_bcnt = 0;
                    iosb->iosb$l_dev_depend = 0;
                }
                return SS$_BADPARAM;
            }
            result = write(fd, p1, p2);
            /* The null device NLA0: discards a write and reports a count of 0
             * (real VAX V7.3 / Alpha V8.4, docs/oracle/semantics/io/
             * IO.NL.WRITE; rd vms-262a). */
            if (result > 0 && qio_is_null_device(fd))
                result = 0;
            if (result < 0) {
                if (iosb) {
                    iosb->iosb$w_status = (uint16_t)SS$_ABORT;
                    iosb->iosb$w_bcnt = 0;
                    iosb->iosb$l_dev_depend = 0;
                }
                return SS$_ABORT;
            }
            if (iosb) {
                iosb->iosb$w_status = (uint16_t)SS$_NORMAL;
                /* Clamp to 16-bit; full count in dev_depend */
                iosb->iosb$w_bcnt = (result > 65535) ? 65535 : (uint16_t)result;
                iosb->iosb$l_dev_depend = (uint32_t)result;
            }
            break;

        case IO$_NOP:
            if (iosb) {
                iosb->iosb$w_status = (uint16_t)SS$_NORMAL;
                iosb->iosb$w_bcnt = 0;
                iosb->iosb$l_dev_depend = 0;
            }
            break;

        default:
            if (iosb) {
                iosb->iosb$w_status = (uint16_t)SS$_ILLIOFUNC;
                iosb->iosb$w_bcnt = 0;
                iosb->iosb$l_dev_depend = 0;
            }
            return SS$_ILLIOFUNC;
    }

    /* Set event flag on completion */
    if ((efn & 0xFFu) < 128) {
        sys$setef(efn);
    }

    /* Call AST completion routine if provided */
    if (astadr) {
        astadr(astprm);
    }

    return SS$_NORMAL;
}


/*
 * ASYNCHRONOUS MAILBOX READ ($QIO, not $QIOW) -- vms-003.
 *
 * A mailbox read with no writer does not complete; on VMS $QIO queues it and
 * returns at once, the IOSB/event flag/AST complete when a message arrives, and
 * $CANCEL aborts it (SS$_ABORT in the IOSB). qio_mailbox_op below is correct for
 * $QIOW (the caller waits) but used to block $QIO as well, so a program that
 * queued a read and then did other work -- Eight-Cubed sys_cancel -- never got
 * its next statement.
 *
 * The executive offers a non-blocking dequeue (VMS_MBX_READ_NOW -> SS$_ENDOFFILE
 * when empty) but no pending-read queue, so the request is held here and driven
 * by a short interval timer that polls it (every ~5 ms) from the owning process's
 * own signal context -- the same context the $SETIMR timers deliver ASTs from. A
 * message found completes the request: IOSB, event flag, AST (dispatched through
 * the executive's AST queue). $CANCEL completes it with SS$_ABORT. A proper
 * executive-resident pending-read queue (vms_mbx.c) would remove the polling; that
 * is the follow-on. This is process-local: no other process sees the request.
 */
extern void vms$$deliver_pending_asts(void);

#if defined(__linux__)
#define ASYNC_RD_MAX 16
static struct async_rd {
    int      in_use;
    uint16_t chan;
    uint32_t exec_chan;
    void    *buf;
    uint32_t bufsz;
    struct _iosb *iosb;
    uint32_t efn;
    void   (*astadr)(uint32_t);
    uint32_t astprm;
} async_rd[ASYNC_RD_MAX];
static timer_t async_timer;
static int async_timer_created;
static int async_timer_armed;
static int async_handler_set;

static void async_rd_complete(struct async_rd *r, uint32_t st, uint32_t actlen)
{
    struct _iosb *iosb = r->iosb;
    void (*ast)(uint32_t) = r->astadr;
    uint32_t prm = r->astprm, efn = r->efn;
    r->in_use = 0;
    /* A message longer than the request: the caller gets the bytes it asked
     * for, the IOSB counts THOSE, and the status is SS$_BUFFEROVF (the rest of
     * the message is gone) -- rd vms-542, observed on real VAX V7.3 / Alpha
     * V8.4 (docs/oracle/semantics/io/, IO.MBX.READ.SHORT). */
    if ((st & 1) && actlen > r->bufsz) {
        actlen = r->bufsz;
        st = SS$_BUFFEROVF;
    }
    if (iosb) {
        iosb->iosb$w_status = (uint16_t)st;
        iosb->iosb$w_bcnt = (actlen > 65535) ? 65535 : (uint16_t)actlen;
        iosb->iosb$l_dev_depend = actlen;
    }
    if ((efn & 0xFFu) < 128)
        (void)sys$setef(efn);
    if (ast) {
        if (sys$dclast(ast, prm, 3) & 1)
            vms$$deliver_pending_asts();
    }
}

static void async_timer_arm(int on)
{
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    if (on) {
        its.it_value.tv_nsec = 5 * 1000 * 1000;
        its.it_interval.tv_nsec = 5 * 1000 * 1000;
    }
    if (async_timer_created)
        timer_settime(async_timer, 0, &its, NULL);
    async_timer_armed = on;
}

static void async_rd_poll(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si; (void)uc;
    int pending = 0;
    for (int i = 0; i < ASYNC_RD_MAX; i++) {
        struct async_rd *r = &async_rd[i];
        if (!r->in_use)
            continue;
        uint32_t actlen = 0, sender = 0;
        uint32_t st = vms_kif_mbx_read_ex(r->exec_chan, r->buf, r->bufsz, &actlen, 1,
                                          &sender);
        /* ENDOFFILE with no sender = nothing queued yet (IO$M_NOW on an empty
         * mailbox): still pending. With a sender it is a dequeued IO$_WRITEOF
         * message, which completes the read (rd vms-262a). */
        if (st == SS$_ENDOFFILE && sender == 0) {
            pending = 1;
            continue;
        }
        async_rd_complete(r, st, (st & 1) ? actlen : 0);
    }
    if (!pending)
        async_timer_arm(0);
}

/* Queue an asynchronous blocking mailbox read; returns SS$_NORMAL once queued. */
static uint32_t qio_mailbox_read_async(uint16_t chan, void *iosb_ptr, void *p1,
                                       uint32_t p2, uint32_t efn,
                                       void (*astadr)(uint32_t), uint32_t astprm)
{
    int slot = -1;
    for (int i = 0; i < ASYNC_RD_MAX; i++)
        if (!async_rd[i].in_use) { slot = i; break; }
    if (slot < 0)
        return SS$_EXQUOTA;

    if (!async_handler_set) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sa.sa_sigaction = async_rd_poll;
        sigaction(SIGRTMIN + 1, &sa, NULL);
        async_handler_set = 1;
    }
    if (!async_timer_created) {
        struct sigevent sev;
        memset(&sev, 0, sizeof sev);
        sev.sigev_notify = SIGEV_SIGNAL;
        sev.sigev_signo = SIGRTMIN + 1;
        if (timer_create(CLOCK_MONOTONIC, &sev, &async_timer) < 0)
            return SS$_INSFMEM;
        async_timer_created = 1;
    }

    sigset_t blk, old;
    sigemptyset(&blk);
    sigaddset(&blk, SIGRTMIN + 1);
    sigprocmask(SIG_BLOCK, &blk, &old);

    struct async_rd *r = &async_rd[slot];
    r->chan = chan;
    r->exec_chan = vms$$chan_exec_chan(chan);
    r->buf = p1;
    r->bufsz = p2;
    r->iosb = (struct _iosb *)iosb_ptr;
    r->efn = efn;
    r->astadr = astadr;
    r->astprm = astprm;
    if (r->iosb)
        memset(r->iosb, 0, sizeof *r->iosb);       /* pending: status 0 */
    if ((efn & 0xFFu) < 128)
        (void)sys$clref(efn);
    r->in_use = 1;
    {
        /* A message already queued completes the request at once; only an empty
         * mailbox leaves it pending. */
        uint32_t actlen = 0, sender = 0;
        uint32_t st = vms_kif_mbx_read_ex(r->exec_chan, p1, p2, &actlen, 1, &sender);
        if (st != SS$_ENDOFFILE || sender != 0) {
            async_rd_complete(r, st, (st & 1) ? actlen : 0);
            sigprocmask(SIG_SETMASK, &old, NULL);
            return SS$_NORMAL;
        }
    }
    if (!async_timer_armed)
        async_timer_arm(1);

    sigprocmask(SIG_SETMASK, &old, NULL);
    return SS$_NORMAL;
}

/* $CANCEL support: complete every pending asynchronous mailbox read on `chan`
 * with SS$_ABORT. Called by sys$cancel. */
void vms$$qio_cancel_chan(uint16_t chan)
{
    sigset_t blk, old;
    sigemptyset(&blk);
    sigaddset(&blk, SIGRTMIN + 1);
    sigprocmask(SIG_BLOCK, &blk, &old);
    int pending = 0;
    for (int i = 0; i < ASYNC_RD_MAX; i++) {
        struct async_rd *r = &async_rd[i];
        if (!r->in_use)
            continue;
        if (r->chan == chan)
            async_rd_complete(r, SS$_ABORT, 0);
        else
            pending = 1;
    }
    if (!pending && async_timer_armed)
        async_timer_arm(0);
    sigprocmask(SIG_SETMASK, &old, NULL);
}

#elif OVMX_HOST_ABSENT
/*
 * The OpenVMS-calling-standard build (the OVMX/Alpha shareables a native VMS
 * image calls; rd vms-3b3f): the same pending-read queue, polled every 5 ms
 * from the $SETIMR interval timer's SIGALRM (sys_time.c, ovmx$$vt_poll)
 * because this build has no POSIX timers or realtime signals.
 */
int ovmx$$vt_poll(void (*fn)(void));
uint64_t ovmx$$vt_block(void);
void ovmx$$vt_restore(uint64_t old);

#define ASYNC_RD_MAX 16
static struct async_rd {
    int      in_use;
    uint16_t chan;
    uint32_t exec_chan;
    void    *buf;
    uint32_t bufsz;
    struct _iosb *iosb;
    uint32_t efn;
    void   (*astadr)(uint32_t);
    uint32_t astprm;
} async_rd[ASYNC_RD_MAX];

static void async_rd_complete(struct async_rd *r, uint32_t st, uint32_t actlen)
{
    struct _iosb *iosb = r->iosb;
    void (*ast)(uint32_t) = r->astadr;
    uint32_t prm = r->astprm, efn = r->efn;
    r->in_use = 0;
    if ((st & 1) && actlen > r->bufsz) {          /* IO.MBX.READ.SHORT, vms-542 */
        actlen = r->bufsz;
        st = SS$_BUFFEROVF;
    }
    if (iosb) {
        iosb->iosb$w_status = (uint16_t)st;
        iosb->iosb$w_bcnt = (actlen > 65535) ? 65535 : (uint16_t)actlen;
        iosb->iosb$l_dev_depend = actlen;
    }
    if ((efn & 0xFFu) < 128)
        (void)sys$setef(efn);
    if (ast) {
        if (sys$dclast(ast, prm, 3) & 1)
            vms$$deliver_pending_asts();
    }
}

static void async_rd_poll(void)
{
    int pending = 0;
    for (int i = 0; i < ASYNC_RD_MAX; i++) {
        struct async_rd *r = &async_rd[i];
        if (!r->in_use)
            continue;
        uint32_t actlen = 0, sender = 0;
        uint32_t st = vms_kif_mbx_read_ex(r->exec_chan, r->buf, r->bufsz, &actlen, 1,
                                          &sender);
        if (st == SS$_ENDOFFILE && sender == 0) {
            pending = 1;
            continue;
        }
        async_rd_complete(r, st, (st & 1) ? actlen : 0);
    }
    if (!pending)
        (void)ovmx$$vt_poll(NULL);
}

static uint32_t qio_mailbox_read_async(uint16_t chan, void *iosb_ptr, void *p1,
                                       uint32_t p2, uint32_t efn,
                                       void (*astadr)(uint32_t), uint32_t astprm)
{
    uint64_t old = ovmx$$vt_block();
    int slot = -1;
    for (int i = 0; i < ASYNC_RD_MAX; i++)
        if (!async_rd[i].in_use) { slot = i; break; }
    if (slot < 0) {
        ovmx$$vt_restore(old);
        return SS$_EXQUOTA;
    }
    struct async_rd *r = &async_rd[slot];
    r->chan = chan;
    r->exec_chan = vms$$chan_exec_chan(chan);
    r->buf = p1;
    r->bufsz = p2;
    r->iosb = (struct _iosb *)iosb_ptr;
    r->efn = efn;
    r->astadr = astadr;
    r->astprm = astprm;
    if (r->iosb)
        memset(r->iosb, 0, sizeof *r->iosb);       /* pending: status 0 */
    if ((efn & 0xFFu) < 128)
        (void)sys$clref(efn);
    r->in_use = 1;
    uint32_t actlen = 0, sender = 0;
    uint32_t st = vms_kif_mbx_read_ex(r->exec_chan, p1, p2, &actlen, 1, &sender);
    if (st != SS$_ENDOFFILE || sender != 0)
        async_rd_complete(r, st, (st & 1) ? actlen : 0);
    else if (!ovmx$$vt_poll(async_rd_poll)) {
        r->in_use = 0;
        ovmx$$vt_restore(old);
        return SS$_INSFMEM;
    }
    ovmx$$vt_restore(old);
    return SS$_NORMAL;
}

void vms$$qio_cancel_chan(uint16_t chan)
{
    uint64_t old = ovmx$$vt_block();
    int pending = 0;
    for (int i = 0; i < ASYNC_RD_MAX; i++) {
        struct async_rd *r = &async_rd[i];
        if (!r->in_use)
            continue;
        if (r->chan == chan)
            async_rd_complete(r, SS$_ABORT, 0);
        else
            pending = 1;
    }
    if (!pending)
        (void)ovmx$$vt_poll(NULL);
    ovmx$$vt_restore(old);
}
#else  /* no POSIX interval timers/rt signals here -- the read stays synchronous */
void vms$$qio_cancel_chan(uint16_t chan) { (void)chan; }
#endif /* __linux__ */

/*
 * qio_mailbox_op - IO$_READVBLK/WRITEVBLK for a MAILBOX channel (vms-d44).
 *
 * A mailbox channel's fd is always -1 (see sys_assign.c): the mailbox --
 * its message queue, its buffer-quota accounting -- is entirely the
 * executive's (src/kernel/vms_mbx.c), so there is no local file descriptor
 * for io_uring or read()/write() to operate on. This bypasses BOTH: one
 * $QIO/$QIOW call in, one vms_kif_mbx_read/write() ioctl round trip out,
 * synchronously -- exactly like qio_sync()'s non-uring fallback, except
 * there is no uring path to try first for a device that isn't a fd at all.
 *
 * $QIO's blocking mailbox read is what real VMS documents (the read
 * completes when a message arrives), so a synchronous ioctl that blocks in
 * the executive is the faithful shape here, not a deficiency to route
 * around: vms_kif_mbx_read() already retries on EINTR the same way
 * sys$waitfr's kif_wait_call() does (see vms_kif.c).
 */
/* Set by an op whose refusal came before VMS would touch the IOSB (a mailbox
 * protection check); consumed and cleared by qio_service_status. */
static __thread int qio_refused_before_iosb;

static uint32_t qio_mailbox_op(uint16_t chan, uint32_t func, void *iosb_ptr,
                                void *p1, uint32_t p2, uint32_t efn,
                                void (*astadr)(uint32_t), uint32_t astprm) {
    struct _iosb *iosb = (struct _iosb *)iosb_ptr;
    uint32_t exec_chan = vms$$chan_exec_chan(chan);
    /*
     * The VMS I/O function code is the low SIX bits (IO$M_FCODE == 0x3F); bits
     * 6-15 are function MODIFIERS (IO$M_FMODIFIERS). Masking with 0xFF instead
     * left modifier bit 6 (IO$M_NOW == 0x40) IN the base function, so a
     * $QIO IO$_READVBLK|IO$M_NOW (0x71) matched no case and returned
     * SS$_ILLIOFUNC -- the non-blocking mailbox read MMK's echo_ast issues
     * through sp_receive, which made send_cmd_and_wait never drain the result
     * and deadlock in $HIBER (vms-95c). (IO$M_WRTATTN == 0x100 is bit 8, above
     * both masks, so the SETMODE arm was unaffected -- which is why only the
     * IO$M_NOW read broke.) Extract the function code with IO$M_FCODE; the
     * modifiers are tested separately below (IO$M_NOW, IO$M_WRTATTN).
     */
    uint32_t base_func = func & IO$M_FCODE;
    uint32_t st;
    uint32_t actlen = 0;
    uint32_t sender = 0;            /* a read's writer PID (IOSB longword 2) */

    switch (base_func) {
        case IO$_READVBLK:
        case IO$_READLBLK:
        case IO$_READPBLK:
            if (!p1) { st = SS$_BADPARAM; break; }
            /*
             * IO$M_NOW (vms-5df): a $QIO ...READVBLK|IO$M_NOW completes at
             * once instead of waiting for a writer. Per the VSI OpenVMS I/O
             * User's Reference Manual (Mailbox Driver), if the mailbox is
             * empty the read completes with SS$_ENDOFFILE; the executive's
             * non-blocking dequeue (VMS_MBX_READ_NOW) does exactly that. The
             * default (modifier absent) keeps the documented blocking read
             * that MMK's send_cmd_and_wait and the wrtattn tests rely on.
             */
            st = vms_kif_mbx_read_ex(exec_chan, p1, p2, &actlen,
                                     (func & IO$M_NOW) != 0, &sender);
            /* the vms_kif layer reports the WHOLE message length; a request
             * shorter than the message gets its bytes, that count, and
             * SS$_BUFFEROVF (rd vms-542) */
            if ((st & 1) && actlen > p2) {
                actlen = p2;
                st = SS$_BUFFEROVF;
            }
            break;

        case IO$_WRITEOF:
            /* an end-of-file message: the reader that dequeues it completes
             * with SS$_ENDOFFILE (rd vms-262a) */
            st = vms_kif_mbx_write_eof(exec_chan);
            break;

        case IO$_WRITEVBLK:
        case IO$_WRITELBLK:
        case IO$_WRITEPBLK:
            if (!p1) { st = SS$_BADPARAM; break; }
            /* IO$M_NORSWAIT (rd vms-c6d1): no room in the mailbox completes the
             * write with SS$_MBFULL instead of waiting for a reader. */
            st = vms_kif_mbx_write_ex(exec_chan, p1, p2,
                                      (func & IO$M_NORSWAIT) != 0);
            if (st & 1) actlen = p2;
            break;

        case IO$_SETMODE:
        case IO$_SETCHAR:
            /*
             * IO$_SETMODE|IO$M_WRTATTN -- arm a WRITE-ATTENTION AST on this
             * mailbox channel (vms-9003). Per the VSI I/O User's Reference
             * (mailbox driver) the AST routine is P1 and its parameter is P2;
             * it is delivered at the caller's access mode (PSL_C_USER for a
             * normal image's $QIO). The executive registers it and fires it
             * cross-process on the next write (src/kernel-core/vms_mbx.c) --
             * the notification MMK's send_cmd_and_wait waits on. IO$M_READATTN
             * (the read-attention counterpart) is not implemented; a SETMODE
             * without a recognized attention modifier is refused honestly.
             */
            if (func & IO$M_WRTATTN) {
                st = vms_kif_mbx_set_wrtattn(exec_chan, (uint8_t)PSL_C_USER,
                                             (uint64_t)(uintptr_t)p1,
                                             (uint64_t)p2);
            } else {
                st = SS$_ILLIOFUNC;
            }
            break;

        case IO$_NOP:
            st = SS$_NORMAL;
            break;

        default:
            st = SS$_ILLIOFUNC;
            break;
    }

    /*
     * A protection refusal (SS$_NOPRIV from the executive's check of the mailbox
     * mask, rd vms-c6d1) happens before VMS touches the IOSB: on real VAX V7.3
     * and Alpha V8.4 the IOSB is left exactly as it was
     * (docs/oracle/semantics/mbxprot/, e.g. MBXP.NOR.READ). Say so to
     * qio_service_status, which would otherwise zero it.
     */
    if (st == SS$_NOPRIV) {
        qio_refused_before_iosb = 1;
        return st;
    }

    if (iosb) {
        iosb->iosb$w_status = (uint16_t)st;
        iosb->iosb$w_bcnt = (actlen > 65535) ? 65535 : (uint16_t)actlen;
        /* a mailbox READ's second IOSB longword is the sender's PID (real
         * VAX V7.3 / Alpha V8.4, docs/oracle/semantics/io/ IO.MBX.READ;
         * rd vms-4a69) */
        iosb->iosb$l_dev_depend = sender ? sender : actlen;
    }

    if (st & 1) {
        if ((efn & 0xFFu) < 128) sys$setef(efn);
        if (astadr) astadr(astprm);
    }

    return st;
}

/*
 * qio_bg_op - $QIO functions for an INET pseudo-device (BGn:) channel (vms-527).
 *
 * The exact analogue of qio_mailbox_op above, socket-for-queue: a BG channel's
 * fd is always -1 (see sys_assign.c) because the socket is entirely the
 * executive's (src/kernel/vms_bg.c), so there is no local descriptor for
 * io_uring or read()/write() to operate on. Each $QIO function is one thin
 * vms_kif_bg_* ioctl into vms.ko, which drives the HOST kernel's in-kernel
 * socket API -- OVMX never reimplements TCP. The function map is the SRI-QIO /
 * INETDRIVER interface (docs/design-tcpip-services-ovmx.md §4 L2):
 *
 *   IO$_SETMODE / IO$_SETCHAR  -> create the socket
 *   IO$_ACCESS                 -> connect to a peer (P1 = sockaddr, P2 = len)
 *   IO$_WRITEVBLK              -> send   (P1 = buffer, P2 = length)
 *   IO$_READVBLK               -> recv   (P1 = buffer, P2 = buffer size)
 *   IO$_DEACCESS               -> shutdown the connection
 *
 * A blocking send/recv that blocks in the executive is the faithful shape here
 * (the QIOW completes when the host kernel completes the socket op), the same
 * reasoning qio_mailbox_op gives for a blocking mailbox read.
 */
struct bg_sockaddr_in {
    uint16_t family;    /* AF_INET */
    uint16_t port;      /* network byte order */
    uint32_t addr;      /* network byte order (IPv4) */
};

static uint32_t qio_bg_op(uint16_t chan, uint32_t func, void *iosb_ptr,
                          void *p1, uint32_t p2, uint32_t p3, uint32_t efn,
                          void (*astadr)(uint32_t), uint32_t astprm) {
    struct _iosb *iosb = (struct _iosb *)iosb_ptr;
    uint32_t exec_chan = vms$$chan_exec_chan(chan);
    uint32_t base_func = func & IO$M_FCODE;
    uint32_t st;
    uint32_t actlen = 0;

    switch (base_func) {
        case IO$_SETMODE:
        case IO$_SETCHAR:
            /* Overloaded (vms-698): P1 != NULL -> bind (P1 = sockaddr; the
             * effective local address, incl. an ephemeral port, is written back);
             * else P3 != 0 -> listen (P3 = backlog); else -> create the socket.
             * The create path is further selected by P2 (the socket-kind
             * selector, IO$K_SOCK_*, iodef.h, vms-80b): IO$K_SOCK_ICMP creates a
             * raw ICMP socket for PING, anything else (0 = IO$K_SOCK_STREAM, what
             * every existing caller passes) the default AF_INET/SOCK_STREAM TCP
             * client socket. */
            if (p1) {
                struct bg_sockaddr_in *sa = (struct bg_sockaddr_in *)p1;
                uint16_t eport = 0;
                uint32_t eaddr = 0;
                st = vms_kif_bg_bind(exec_chan, sa->family, sa->port, sa->addr,
                                     &eport, &eaddr);
                if (st & 1) { sa->port = eport; sa->addr = eaddr; }
            } else if (p3 != 0) {
                st = vms_kif_bg_listen(exec_chan, (int)p3);
            } else if (p2 == IO$K_SOCK_ICMP) {
                st = vms_kif_bg_setmode_icmp(exec_chan);
            } else {
                st = vms_kif_bg_setmode(exec_chan);
            }
            break;

        case IO$_ACCESS:
            if (func & IO$M_ACCEPT) {
                /* accept (vms-698): P3 = a second BG channel ($ASSIGNed empty) to
                 * receive the connection; P1 (optional) = peer sockaddr out. */
                uint32_t acc_chan;
                uint16_t fam = 0, port = 0;
                uint32_t a4 = 0;
                if (p3 == 0 || !vms$$chan_is_bg((uint16_t)p3)) {
                    st = SS$_BADPARAM;
                    break;
                }
                acc_chan = vms$$chan_exec_chan((uint16_t)p3);
                st = vms_kif_bg_accept(exec_chan, acc_chan, &fam, &port, &a4);
                if ((st & 1) && p1 && p2 >= sizeof(struct bg_sockaddr_in)) {
                    struct bg_sockaddr_in *sa = (struct bg_sockaddr_in *)p1;
                    sa->family = fam; sa->port = port; sa->addr = a4;
                }
            } else {
                /* Connect. P1 = sockaddr (family/port/addr), P2 = its length. */
                if (!p1 || p2 < sizeof(struct bg_sockaddr_in)) {
                    st = SS$_BADPARAM;
                    break;
                }
                {
                    const struct bg_sockaddr_in *sa =
                        (const struct bg_sockaddr_in *)p1;
                    st = vms_kif_bg_connect(exec_chan, sa->family, sa->port,
                                            sa->addr);
                }
            }
            break;

        case IO$_WRITEVBLK:
        case IO$_WRITELBLK:
        case IO$_WRITEPBLK:
            if (!p1) { st = SS$_BADPARAM; break; }
            st = vms_kif_bg_send(exec_chan, p1, p2, &actlen);
            break;

        case IO$_READVBLK:
        case IO$_READLBLK:
        case IO$_READPBLK:
            if (!p1) { st = SS$_BADPARAM; break; }
            st = vms_kif_bg_recv(exec_chan, p1, p2, &actlen);
            break;

        case IO$_DEACCESS:
            st = vms_kif_bg_deaccess(exec_chan);
            break;

        case IO$_NOP:
            st = SS$_NORMAL;
            break;

        default:
            st = SS$_ILLIOFUNC;
            break;
    }

    if (iosb) {
        iosb->iosb$w_status = (uint16_t)st;
        iosb->iosb$w_bcnt = (actlen > 65535) ? 65535 : (uint16_t)actlen;
        iosb->iosb$l_dev_depend = actlen;
    }

    if (st & 1) {
        if ((efn & 0xFFu) < 128) sys$setef(efn);
        if (astadr) astadr(astprm);
    }

    return st;
}

/*
 * ===================== the DECnet _NET: $QIO path (rd vms-dda) =====================
 *
 * $ASSIGN _NET: resolves the executive DECnet device face (a1-0); a $QIO on that
 * channel is a LOGICAL-LINK operation, and on VMS every logical link -- inbound
 * and outbound -- belongs to NETACP. So this path does not run any protocol: it
 * BROKERS each operation to the running NETACP over the T1 transport (docs/
 * design-decnet-net-qio-mailbox-seam.md):
 *
 *   NETACP owns ONE request mailbox, published as the LNM$SYSTEM logical
 *   DNET$NETACP_REQ. Each _NET: channel gets its OWN temporary reply mailbox (a
 *   mailbox read is destructive, so replies must never share a queue), and
 *   every request carries that mailbox's unit plus a correlation id; the reply
 *   is accepted only if its id matches (dnet_broker_call).
 *
 * The record codec and the client marshalling (dnet_broker_xfer) are the SAME
 * source NETACP and its host-floor selftest use, compiled here as a PRIVATE
 * (static) copy -- one record format, and no new universal in LIBVMS$SHR.
 *
 * Function codes (p1/p2 per docs/design-decnet-net-qio-broker-a1-contract.md):
 *   IO$_ACCESS    p1 = NCB text  NODE"user password account"::"object", p2 = len
 *   IO$_WRITEVBLK p1 = buffer, p2 = length          (one NSP message)
 *   IO$_READVBLK  p1 = buffer, p2 = size; waits for a message. With IO$M_NOW it
 *                 completes at once, SS$_ENDOFFILE when none is buffered.
 *   IO$_DEACCESS  disconnect.
 *   IO$_ACPCONTROL a READ-ONLY query of NETACP's volatile database (NCP SHOW,
 *                 DCL SHOW NETWORK -- rd vms-30e): p1 = buffer, p2 = its size,
 *                 p3 = the length of the query the caller placed at p1 (a
 *                 dnet_netshow request). NETACP's snapshot record overwrites the
 *                 buffer; the IOSB byte count is its length. Needs no logical
 *                 link on the channel. (VMS passes the NFB in p1 and the result
 *                 buffer in p4; OVMX's $QIO carries p3..p6 as 32-bit values, so
 *                 the query and the answer share the one p1 buffer -- an OVMX
 *                 calling form, labelled.)
 * FAIL-HONEST (Rule 9 / INV-6): no NETACP serving -> SS$_DEVOFFLINE; a NETACP
 * that stops answering -> SS$_DEVOFFLINE; never a fabricated transfer.
 */
#include "../../vmsdecnet/nsp/include/dnet_nsp.h"   /* DNET_NSP_MAX_DATA */
#define DNET_BROKER_API static __attribute__((unused))
#include "../../vmsdecnet/broker/include/dnet_broker.h"
#include "../../vmsdecnet/broker/dnet_broker.c"
#include <time.h>

_Static_assert(DNET_BROKER_ST_NORMAL == SS$_NORMAL &&
               DNET_BROKER_ST_ENDOFFILE == SS$_ENDOFFILE &&
               DNET_BROKER_ST_DEVOFFLINE == SS$_DEVOFFLINE &&
               DNET_BROKER_ST_FILNOTACC == SS$_FILNOTACC &&
               DNET_BROKER_ST_BADPARAM == SS$_BADPARAM &&
               DNET_BROKER_ST_ABORT == SS$_ABORT &&
               DNET_BROKER_ST_EXQUOTA == SS$_EXQUOTA &&
               DNET_BROKER_ST_ILLIOFUNC == SS$_ILLIOFUNC &&
               DNET_BROKER_ST_TIMEOUT == SS$_TIMEOUT &&
               DNET_BROKER_ST_INVLOGIN == SS$_INVLOGIN &&
               DNET_BROKER_ST_NOSUCHDEV == SS$_NOSUCHDEV &&
               DNET_BROKER_ST_BUFFEROVF == SS$_BUFFEROVF,
               "broker statuses are the VMS SS$_ values");

#define NET_REQ_LOGNAM  "DNET$NETACP_REQ"
#define NET_POLL_NS     2000000L      /* 2 ms between reply-mailbox polls      */
#define NET_REPLY_POLLS 5000u         /* ~10 s for NETACP to answer a request   */
#define NET_OPEN_POLLS  60000u        /* ~120 s for a connect (CI give-up ~45 s) */

struct net_chan_state {
    int      bound;                   /* reply mailbox + request channel held  */
    uint32_t exec_chan;               /* the _NET: channel this state belongs to */
    uint32_t req_chan;                /* channel to NETACP's request mailbox   */
    uint32_t rep_chan;                /* this channel's own reply mailbox      */
    struct dnet_broker_chan bc;
};
static struct net_chan_state g_netchan[PCB_MAX_CHANNELS];
static pthread_mutex_t g_netchan_lock = PTHREAD_MUTEX_INITIALIZER;

static int net_io_put(void *ctx, const uint8_t *rec, size_t len)
{
    struct net_chan_state *ns = ctx;
    /* Empty this channel's reply mailbox before asking (rd vms-c6d1): NETACP
     * answers with IO$M_NORSWAIT, so a mailbox still holding answers nobody is
     * waiting for any more (a completion that arrived after its waiter gave
     * up) would cost the NEXT request its answer (SS$_MBFULL at NETACP). None
     * of them can be for the request about to go: its correlation id is new. */
    for (int k = 0; k < 64; k++) {
        uint8_t junk[DNET_BROKER_RSP_MAX + 16];
        uint32_t got = 0;
        if (!(vms_kif_mbx_read(ns->rep_chan, junk, sizeof junk, &got, 1) & 1))
            break;
    }
    return (vms_kif_mbx_write(ns->req_chan, rec, (uint32_t)len) & 1) ? 0 : -1;
}
static int net_io_get(void *ctx, uint8_t *buf, size_t cap, size_t *len)
{
    struct net_chan_state *ns = ctx;
    uint32_t got = 0;
    uint32_t st = vms_kif_mbx_read(ns->rep_chan, buf, (uint32_t)cap, &got, 1 /*IO$M_NOW*/);
    if (st == SS$_ENDOFFILE)
        return 0;
    if (!(st & 1))
        return -1;
    *len = got > cap ? cap : got;
    return 1;
}
static void net_io_idle(void *ctx)
{
    (void)ctx;
    struct timespec ts = { 0, NET_POLL_NS };
    nanosleep(&ts, NULL);
}

/* Bind channel `chan` to the running NETACP: find its request mailbox through
 * the DNET$NETACP_REQ logical and create this channel's reply mailbox. Returns
 * SS$_NORMAL, or SS$_DEVOFFLINE when no NETACP is serving (no logical, or its
 * mailbox is gone). Caller holds g_netchan_lock. */
static uint32_t net_bind(uint16_t chan, struct net_chan_state *ns)
{
    uint32_t exec_chan = vms$$chan_exec_chan(chan);
    if (ns->bound && ns->exec_chan == exec_chan)
        return SS$_NORMAL;
    memset(ns, 0, sizeof *ns);

    char dev[64];
    uint16_t dlen = 0;
    if (vms_kif_lnm_translate(VMS_LNM_TBL_SYSTEM, NET_REQ_LOGNAM, 0, dev,
                              sizeof dev - 1, &dlen, NULL, NULL) != 1 || dlen == 0)
        return SS$_DEVOFFLINE;
    dev[dlen < sizeof dev ? dlen : sizeof dev - 1] = '\0';
    if (!(vms_kif_mbx_assign(dev, &ns->req_chan) & 1))
        return SS$_DEVOFFLINE;        /* stale logical: that NETACP is gone */

    uint32_t unit = 0;
    char rdev[64];
    /* The reply mailbox is this process's alone: S:RWLP,O:RWLP,G:,W: (rd
     * vms-c6d1) -- NETACP (a SYSTEM-category UIC) may write the answer, no other
     * user may read it, since a reply can carry the link's data. */
    uint32_t st = vms_kif_mbx_create_prot(0, DNET_BROKER_RSP_MAX + 16,
                                          (DNET_BROKER_RSP_MAX + 16) * 8, 0xFF00u,
                                          &ns->rep_chan, &unit, rdev, sizeof rdev);
    if (!(st & 1)) {
        (void)vms_kif_dassgn(ns->req_chan);
        memset(ns, 0, sizeof *ns);
        return st;
    }
    struct vms_procinfo self;
    if (vms_kif_getjpi_self(&self) & 1)
        ns->bc.owner_pid = self.vms_pid;
    ns->bc.reply_unit = unit;
    ns->exec_chan = exec_chan;
    ns->bound = 1;
    return SS$_NORMAL;
}

static void net_unbind(struct net_chan_state *ns)
{
    if (!ns->bound)
        return;
    (void)vms_kif_mbx_delmbx(ns->rep_chan);
    (void)vms_kif_dassgn(ns->rep_chan);
    (void)vms_kif_dassgn(ns->req_chan);
    memset(ns, 0, sizeof *ns);
}

/*
 * vms$$net_chan_release - $DASSGN of a _NET: channel (called from sys$dassgn
 * before the executive channel is released): disconnect a link still open on
 * it, as $DASSGN does on VMS, and give back the reply mailbox.
 */
void vms$$net_chan_release(uint16_t chan)
{
    uint32_t slot = pcb_chan_to_slot(chan);     /* channel N*16 is slot N */
    if (chan == 0 || slot >= PCB_MAX_CHANNELS)
        return;
    pthread_mutex_lock(&g_netchan_lock);
    struct net_chan_state *ns = &g_netchan[slot];
    if (ns->bound && ns->bc.handle != 0) {
        struct dnet_broker_io io = { ns, net_io_put, net_io_get, net_io_idle,
                                     NET_REPLY_POLLS, NET_OPEN_POLLS };
        size_t x = 0;
        (void)dnet_broker_xfer(&ns->bc, &io, DNET_BROKER_OP_CLOSE, NULL, 0, NULL, 0, &x);
    }
    net_unbind(ns);
    pthread_mutex_unlock(&g_netchan_lock);
}

static uint32_t qio_net_op(uint16_t chan, uint32_t func, void *iosb_ptr,
                           void *p1, uint32_t p2, uint32_t p3, uint32_t efn,
                           void (*astadr)(uint32_t), uint32_t astprm) {
    struct _iosb *iosb = (struct _iosb *)iosb_ptr;
    uint32_t base_func = func & IO$M_FCODE;
    uint32_t st;
    size_t xfer = 0;
    uint16_t op = 0;

    switch (base_func) {
        case IO$_ACCESS:    op = DNET_BROKER_OP_OPEN;  break;  /* connect      */
        case IO$_WRITEVBLK: op = DNET_BROKER_OP_SEND;  break;  /* send message */
        case IO$_READVBLK:  op = DNET_BROKER_OP_RECV;  break;  /* receive      */
        case IO$_DEACCESS:  op = DNET_BROKER_OP_CLOSE; break;  /* disconnect   */
        case IO$_ACPCONTROL: op = DNET_BROKER_OP_SHOW; break;  /* NCP SHOW     */
        default: break;
    }

    if (base_func == IO$_NOP) {
        st = SS$_NORMAL;
    } else if (op == 0) {
        st = SS$_ILLIOFUNC;
    } else if (chan == 0 || pcb_chan_to_slot(chan) >= PCB_MAX_CHANNELS) {
        st = SS$_IVCHAN;
    } else {
        /* One request at a time per channel; channels proceed independently
         * (the lock is dropped while a request waits on its reply). The
         * state is per channel SLOT: channel numbers are N*16 (rd vms-0a2). */
        pthread_mutex_lock(&g_netchan_lock);
        struct net_chan_state *ns = &g_netchan[pcb_chan_to_slot(chan)];
        uint32_t bst = net_bind(chan, ns);
        pthread_mutex_unlock(&g_netchan_lock);
        if (!(bst & 1)) {
            /* No NETACP is serving logical links on this node: the device face
             * resolved, its I/O path is inactive. Never a fake (INV-6). */
            st = SS$_DEVOFFLINE;
        } else {
            struct dnet_broker_io io = { ns, net_io_put, net_io_get, net_io_idle,
                                         NET_REPLY_POLLS, NET_OPEN_POLLS };
            if (op == DNET_BROKER_OP_RECV && (func & IO$M_NOW))
                op |= DNET_BROKER_OPF_NOW;
            if (op == DNET_BROKER_OP_SHOW) {
                /* The query sits at p1 (p3 bytes); the answer replaces it. */
                st = (!p1 || p3 == 0 || p3 > p2) ? SS$_BADPARAM
                   : dnet_broker_control(&ns->bc, &io, p1, p3, p1, p2, &xfer);
            } else {
                st = dnet_broker_xfer(&ns->bc, &io, op,
                                      (op == DNET_BROKER_OP_OPEN || op == DNET_BROKER_OP_SEND) ? p1 : NULL,
                                      (op == DNET_BROKER_OP_OPEN || op == DNET_BROKER_OP_SEND) ? p2 : 0,
                                      (base_func == IO$_READVBLK) ? p1 : NULL,
                                      (base_func == IO$_READVBLK) ? p2 : 0, &xfer);
            }
        }
    }

    if (iosb) {
        iosb->iosb$w_status = (uint16_t)st;
        iosb->iosb$w_bcnt = (xfer > 65535) ? 65535 : (uint16_t)xfer;
        iosb->iosb$l_dev_depend = (uint32_t)xfer;
    }
    if (st & 1) {
        if ((efn & 0xFFu) < 128) sys$setef(efn);
        if (astadr) astadr(astprm);
    }
    return st;
}

/*
 * qio_validate_and_classify - Shared validation for sys$qio and sys$qiow.
 *
 * Resolves the channel to an fd, validates the function code and buffer,
 * and classifies the operation as read or write.
 *
 * Returns SS$_NORMAL on success (fd and is_read are set), or an error status.
 */
static uint32_t qio_validate_and_classify(uint16_t chan, uint32_t func,
                                           void *iosb_ptr, void *p1,
                                           uint32_t efn,
                                           void (*astadr)(uint32_t),
                                           uint32_t astprm,
                                           int *out_fd, int *out_is_read) {
    int fd = vms$$chan_to_fd(chan);
    if (fd < 0) return pcb_chan_unheld_status(chan);

    /*
     * vms-1c57: THE CHANNEL IS THE IDENTITY. If this channel was bound to a
     * device through $ASSIGN (vms$$chan_exec_chan nonzero -- currently only
     * true for the terminal, src/kernel/vms_devtab.c's OPA0:), $QIO must
     * operate on THAT channel: reconfirm with the executive, on every call,
     * that it still recognizes the channel before doing any local I/O. This
     * is a READ (VMS_IOCTL_GETDVI by channel) -- $QIO does not write the
     * device table as a side effect, which would make the table's contents
     * agree with reality while leaving the I/O path free to diverge from it
     * again the moment nobody was checking. A channel /dev/vms no longer
     * has -- because the executive is unreachable, or (not currently
     * possible from this API, but not assumed impossible either) because
     * something else tore it down -- must not let bytes move on the strength
     * of a local slot number alone.
     */
    uint32_t exec_chan = vms$$chan_exec_chan(chan);
    if (exec_chan != 0) {
        struct vms_devinfo info;
        uint32_t dvi_st = vms_kif_getdvi_chan(exec_chan, &info);
        if (!(dvi_st & 1)) {
            struct _iosb *iosb = (struct _iosb *)iosb_ptr;
            if (iosb) {
                iosb->iosb$w_status = (uint16_t)dvi_st;
                iosb->iosb$w_bcnt = 0;
                iosb->iosb$l_dev_depend = 0;
            }
            return dvi_st;
        }
    }

    uint32_t base_func = func & 0xFF;

    /* Handle IO$_NOP synchronously */
    if (base_func == IO$_NOP) {
        struct _iosb *iosb = (struct _iosb *)iosb_ptr;
        if (iosb) {
            iosb->iosb$w_status = (uint16_t)SS$_NORMAL;
            iosb->iosb$w_bcnt = 0;
            iosb->iosb$l_dev_depend = 0;
        }
        if ((efn & 0xFFu) < 128) sys$setef(efn);
        if (astadr) astadr(astprm);
        *out_fd = fd;
        return 0xFFFFFFFF;  /* Sentinel: NOP handled, caller should return SS$_NORMAL */
    }

    /* Determine read/write */
    int is_read;
    switch (base_func) {
        case IO$_READVBLK:
        case IO$_READLBLK:
        case IO$_READPBLK:
            is_read = 1;
            break;
        case IO$_WRITEVBLK:
        case IO$_WRITELBLK:
        case IO$_WRITEPBLK:
            is_read = 0;
            break;
        default: {
            struct _iosb *iosb = (struct _iosb *)iosb_ptr;
            if (iosb) {
                iosb->iosb$w_status = (uint16_t)SS$_ILLIOFUNC;
                iosb->iosb$w_bcnt = 0;
                iosb->iosb$l_dev_depend = 0;
            }
            return SS$_ILLIOFUNC;
        }
    }

    /* Validate buffer */
    if (!p1) {
        struct _iosb *iosb = (struct _iosb *)iosb_ptr;
        if (iosb) {
            iosb->iosb$w_status = (uint16_t)SS$_BADPARAM;
            iosb->iosb$w_bcnt = 0;
            iosb->iosb$l_dev_depend = 0;
        }
        return SS$_BADPARAM;
    }

    *out_fd = fd;
    *out_is_read = is_read;
    return SS$_NORMAL;
}

/*
 * qio_terminal_setmode - the P2-selector IO$_SETMODE (IO$K_TT_PASSALL /
 * IO$K_TT_NORMAL) on a channel that is NOT an executive terminal (vms-f54;
 * rd vms-f8c). A $ SET HOST CTERM client (src/vmsdecnet/engine/decnetd.c) asks
 * for pass-all on its terminal; on a terminal unit that request is the
 * executive terminal driver's (qio_terminal_op below, VMS_IOCTL_TT_SETMODE).
 * What reaches HERE is a channel with no terminal behind it -- a pipe or a
 * redirect, e.g. the automated end-to-end test feeding the client a pipe --
 * where pass-all has no meaning: a graceful no-op that reports SS$_NORMAL.
 * Nothing touches the substrate's termios: the line discipline belongs to the
 * executive (rd vms-f8c), not to userspace.
 */
static uint32_t qio_terminal_setmode(int fd, uint32_t p2, void *iosb_ptr,
                                     uint32_t efn, void (*astadr)(uint32_t),
                                     uint32_t astprm) {
    struct _iosb *iosb = (struct _iosb *)iosb_ptr;
    uint32_t st = SS$_NORMAL;

    (void)fd; (void)p2;
    if (iosb) {
        iosb->iosb$w_status = (uint16_t)st;
        iosb->iosb$w_bcnt = 0;
        iosb->iosb$l_dev_depend = 0;
    }
    if ((efn & 0xFFu) < 128) sys$setef(efn);
    if (astadr) astadr(astprm);
    return st;
}

/*
 * sys$qio - Queue I/O Request (asynchronous).
 *
 * Submits the I/O via io_uring and returns immediately. The IOSB is
 * filled, event flag set, and AST called when the I/O completes.
 * Falls back to synchronous I/O if io_uring is not available.
 */

/*
 * qio_efn_request - the event-flag half of queuing an I/O request: clear the
 * flag; fail only when the executive says the number is not a flag of ours.
 */
static uint32_t qio_efn_request(uint32_t efn)
{
    if (efn == EFN$C_ENF)
        return SS$_NORMAL;
    uint32_t c = sys$clref(efn);
    if (c == SS$_ILLEFC || c == SS$_UNASEFC)
        return c;
    return SS$_NORMAL;
}

/*
 * qio_completes_at_once - a device whose I/O never waits (a disk file, the null
 * device): an asynchronous $QIO on it is completed in the request -- IOSB, event
 * flag, AST -- as the driver would complete it, instead of being parked on
 * io_uring whose completions nothing reaps for $QIO (rd vms-084).
 */
static int qio_completes_at_once(int fd)
{
    struct stat sb;
    if (fstat(fd, &sb) != 0)
        return 0;
    if (S_ISREG(sb.st_mode) || S_ISBLK(sb.st_mode))
        return 1;
    /* the null device completes at once (it never blocks) */
    return qio_is_null_device(fd);
}


/*
 * TERMINAL $QIO (rd vms-d900). A channel to a terminal (a DC$_TERM row in the
 * executive's device table: OPA0:, an RTAn:) answers the terminal driver's
 * functions -- observed on OpenVMS through TT: (docs/oracle/semantics/tt/):
 *   IO$_SENSEMODE / IO$_SENSECHAR  the characteristics buffer: class (DC$_TERM),
 *       type, page width (word), characteristics (3 bytes), page length, and
 *       the extended characteristics longword when the buffer has room; the
 *       IOSB's second word is the line speed (TT$C_BAUD_9600 = 15, the
 *       console line).
 *   IO$_SETMODE / IO$_SETCHAR with a characteristics buffer  set the width,
 *       page length and characteristics in the executive's row.
 *   IO$_READVBLK / IO$_READLBLK / IO$_READPROMPT with IO$M_TIMED  a zero (or
 *       elapsed) timeout with nothing typed ends SS$_TIMEOUT in the IOSB; a
 *       zero-length read is SS$_NORMAL at once; IO$M_PURGE discards typeahead;
 *       READPROMPT writes its prompt (P5/P6) first. The IOSB carries the offset
 *       to the terminator, the terminator and its size.
 *   IO$_ACCESS  SS$_DEVOFFLINE, as the terminal driver refuses it.
 * Every value read or set is the executive device row's (vms_kif_getdvi_chan
 * / vms_kif_ttsetmode) -- never a process-local model (INV-6).
 */
#define TT_SPEED_9600 15
static int qio_chan_is_terminal(uint16_t chan, uint32_t *exec_chan_out,
                                struct vms_devinfo *info)
{
    uint32_t ec = vms$$chan_exec_chan(chan);
    if (ec == 0) return 0;
    if (!(vms_kif_getdvi_chan(ec, info) & 1)) return 0;
    if (info->devclass != DC$_TERM) return 0;
    *exec_chan_out = ec;
    return 1;
}

static void tt_iosb(void *iosb_ptr, uint32_t st, uint16_t w1, uint16_t w2, uint16_t w3)
{
    if (!iosb_ptr) return;
    uint16_t *w = (uint16_t *)iosb_ptr;
    w[0] = (uint16_t)st; w[1] = w1; w[2] = w2; w[3] = w3;
}

static uint32_t qio_terminal_op(uint16_t chan, int fd, uint32_t ec,
                                const struct vms_devinfo *info, uint32_t func,
                                void *iosb_ptr, void *p1, uint32_t p2, uint32_t p3,
                                uintptr_t p4, uintptr_t p5, uint32_t p6,
                                uint32_t efn, void (*astadr)(uint32_t), uint32_t astprm,
                                int *handled)
{
    (void)chan; (void)fd;
    uint32_t base = func & IO$M_FCODE;
    *handled = 1;
    switch (base) {
    case IO$_SENSEMODE:
    case IO$_SENSECHAR: {
        if (p1 && p2 > 0) {
            uint8_t b[12];
            /* IO$_SENSECHAR answers the PERMANENT width and page, SENSEMODE
             * the current ones (observed TT.SENSECHAR 80/24 vs TT.SENSEMODE
             * 511/0 on Alpha V8.4; rd vms-d900). */
            uint32_t w = base == IO$_SENSECHAR ? info->perm_width : info->width;
            uint32_t pg = base == IO$_SENSECHAR ? info->perm_page : info->page;
            memset(b, 0, sizeof b);
            b[0] = (uint8_t)info->devclass;
            b[1] = (uint8_t)info->devtype;
            b[2] = (uint8_t)(w & 0xFF);
            b[3] = (uint8_t)((w >> 8) & 0xFF);
            b[4] = (uint8_t)(info->devchar & 0xFF);
            b[5] = (uint8_t)((info->devchar >> 8) & 0xFF);
            b[6] = (uint8_t)((info->devchar >> 16) & 0xFF);
            b[7] = (uint8_t)(pg & 0xFF);
            uint32_t ext = (uint32_t)(info->devchar >> 32);
            memcpy(b + 8, &ext, 4);
            memcpy(p1, b, p2 < sizeof b ? p2 : sizeof b);
        }
        tt_iosb(iosb_ptr, SS$_NORMAL, TT_SPEED_9600, 0, 0);
        break;
    }
    case IO$_SETMODE:
    case IO$_SETCHAR: {
        if (!p1 || p2 < 8) {
            /* the P2-selector form (vms-f54): IO$K_TT_PASSALL hands every
             * byte through unedited, unechoed (a SET HOST CTERM session);
             * IO$K_TT_NORMAL restores the interactive driver (rd vms-f8c). */
            uint32_t st = vms_kif_tt_setmode(ec, p2 == IO$K_TT_PASSALL ? VMS_TT_MODE_PASSALL : 0);
            if (!(st & 1)) { tt_iosb(iosb_ptr, st, 0, 0, 0); return st; }
            tt_iosb(iosb_ptr, SS$_NORMAL, 0, 0, 0);
            break;
        }
        const uint8_t *b = (const uint8_t *)p1;
        uint32_t width = (uint32_t)b[2] | ((uint32_t)b[3] << 8);
        uint32_t page = b[7];
        uint64_t chars = (uint64_t)b[4] | ((uint64_t)b[5] << 8) | ((uint64_t)b[6] << 16);
        if (p2 >= 12) {
            uint32_t ext; memcpy(&ext, b + 8, 4);
            chars |= (uint64_t)ext << 32;
        }
        /* the buffer carries characteristic bits 0-23 and (with room) the
         * extended longword; bits 24-31 are not in it and are left alone */
        uint64_t covered = 0x0000000000FFFFFFULL | (p2 >= 12 ? 0xFFFFFFFF00000000ULL : 0);
        /* IO$_SETCHAR sets the permanent characteristics as well as the
         * current ones; IO$_SETMODE only the current (rd vms-d900). */
        uint32_t st = vms_kif_ttsetmode(ec, VMS_TTSET_CHAR | VMS_TTSET_WIDTH | VMS_TTSET_PAGE |
                                            (base == IO$_SETCHAR ? VMS_TTSET_PERM : 0),
                                        chars & covered, (~chars) & covered, width, page);
        if (!(st & 1)) { tt_iosb(iosb_ptr, st, 0, 0, 0); return st; }
        tt_iosb(iosb_ptr, SS$_NORMAL, TT_SPEED_9600, 0, 0);
        break;
    }
    case IO$_READVBLK:
    case IO$_READLBLK:
    case IO$_READPROMPT: {
        /* THE EXECUTIVE TERMINAL DRIVER (rd vms-f8c): the read is the class
         * driver's -- type-ahead consumed and echoed as it is read, after the
         * prompt; the terminator mask; IO$M_TIMED / PURGE / NOECHO / NOFILTR /
         * TRMNOECHO / CVTLOW. Nothing here reads the substrate descriptor. A
         * terminal with no port attached answers SS$_DEVOFFLINE (Rule 9). */
        struct vms_tt_read_args ra;
        memset(&ra, 0, sizeof ra);
        ra.chan = ec;
        if (func & IO$M_NOECHO)    ra.flags |= VMS_TT_RD_NOECHO;
        if (func & IO$M_TIMED)     ra.flags |= VMS_TT_RD_TIMED;
        if (func & IO$M_PURGE)     ra.flags |= VMS_TT_RD_PURGE;
        if (func & IO$M_NOFILTR)   ra.flags |= VMS_TT_RD_NOFILTR;
        if (func & IO$M_TRMNOECHO) ra.flags |= VMS_TT_RD_TRMNOECHO;
        if (func & IO$M_CVTLOW)    ra.flags |= VMS_TT_RD_CVTLOW;
        ra.buf = (uint64_t)(uintptr_t)p1;
        ra.bufsz = p1 ? p2 : 0;
        ra.timeout = p3;
        /* P4: the terminator descriptor. Short form: a quadword whose first
         * longword is 0 and whose second is the mask for characters 0-31.
         * Long form: a word mask size in bytes and the mask's address. 0 =
         * the standard terminator set (I/O User's Reference, "Terminators"). */
        if (p4) {
            const uint32_t *td = (const uint32_t *)(uintptr_t)p4;
            if (td[0] == 0) {
                ra.termmask[0] = td[1];
            } else {
                uint32_t msz = td[0] & 0xFFFFu;
                const uint8_t *m = (const uint8_t *)(uintptr_t)td[1];
                if (msz > sizeof ra.termmask) msz = sizeof ra.termmask;
                if (m) memcpy(ra.termmask, m, msz);
            }
            ra.flags |= VMS_TT_RD_TERMMASK;
        }
        if (base == IO$_READPROMPT && p5 && p6) {
            ra.prompt = (uint64_t)(uintptr_t)p5;
            ra.promptsz = p6;
        }
        uint32_t st = vms_kif_tt_read(&ra);
        if (st == SS$_DEVOFFLINE || st == SS$_NOSUCHDEV || st == SS$_IVCHAN) {
            tt_iosb(iosb_ptr, st, 0, 0, 0);
            return st;
        }
        tt_iosb(iosb_ptr, st, (uint16_t)ra.count, (uint16_t)ra.term, (uint16_t)ra.termsz);
        break;
    }
    case IO$_WRITEVBLK:
    case IO$_WRITELBLK: {
        /* Output through the class driver, byte for byte (rd vms-f8c). */
        uint32_t st = (p1 && p2) ? vms_kif_tt_write(ec, p1, p2) : SS$_NORMAL;
        if (!(st & 1)) { tt_iosb(iosb_ptr, st, 0, 0, 0); return st; }
        tt_iosb(iosb_ptr, SS$_NORMAL, (uint16_t)p2, 0, 0);
        break;
    }
    case IO$_ACCESS:
        tt_iosb(iosb_ptr, 0, 0, 0, 0);
        return SS$_DEVOFFLINE;
    default:
        *handled = 0;
        return SS$_NORMAL;
    }
    if ((efn & 0xFFu) < 128) sys$setef(efn);
    if (astadr) astadr(astprm);
    return SS$_NORMAL;
}

static uint32_t qio_body(uint32_t efn, uint16_t chan, uint32_t func,
                  void *iosb_ptr, void (*astadr)(uint32_t), uint32_t astprm,
                  void *p1, uint32_t p2, uint32_t p3,
                  uintptr_t p4, uintptr_t p5, uint32_t p6) {
    (void)p4; (void)p5; (void)p6;

    /* The event flag is cleared when the request is queued, and an efn that is
     * not one of this process's flags fails the request before anything else
     * (SS$_ILLEFC / SS$_UNASEFC) -- real VAX V7.3 and Alpha V8.4, semantic
     * oracle docs/oracle/semantics/io/ IO.QIO.* (rd vms-084). The executive
     * answers through $CLREF; with no executive at all (a host test) there are
     * no flags to validate and the request goes on. */
    {
        uint32_t cst = qio_efn_request(efn);
        if (cst != SS$_NORMAL)
            return cst;
    }

    if (vms$$chan_is_mailbox(chan)) {
        uint32_t bf = func & IO$M_FCODE;
#if defined(__linux__) || OVMX_HOST_ABSENT
        if ((bf == IO$_READVBLK || bf == IO$_READLBLK || bf == IO$_READPBLK) &&
            !(func & IO$M_NOW) && p1)
            return qio_mailbox_read_async(chan, iosb_ptr, p1, p2, efn,
                                          astadr, astprm);
#else
        (void)bf;
#endif
        return qio_mailbox_op(chan, func, iosb_ptr, p1, p2, efn, astadr, astprm);
    }

    if (qio_null_writeof(chan, func, iosb_ptr, efn, astadr, astprm))
        return SS$_NORMAL;

    if (vms$$chan_is_bg(chan))
        return qio_bg_op(chan, func, iosb_ptr, p1, p2, p3, efn, astadr, astprm);

    if (vms$$chan_is_net(chan))
        return qio_net_op(chan, func, iosb_ptr, p1, p2, p3, efn, astadr, astprm);

    {
        uint32_t tec = 0;
        struct vms_devinfo tinfo;
        if (qio_chan_is_terminal(chan, &tec, &tinfo)) {
            /* the terminal's I/O is the executive driver's: no descriptor
             * is needed (rd vms-f8c) */
            int tfd = vms$$chan_to_fd(chan);
            int handled = 0;
            uint32_t tst = qio_terminal_op(chan, tfd, tec, &tinfo, func, iosb_ptr,
                                           p1, p2, p3, p4, p5, p6, efn, astadr,
                                           astprm, &handled);
            if (handled) return tst;
        }
    }

    /* IO$_SETMODE line discipline on a terminal channel (vms-f54): the terminal
     * driver's home, dispatched before the read/write classifier (which rejects
     * SETMODE as SS$_ILLIOFUNC). Off a real tty it is a graceful no-op. */
    if ((func & IO$M_FCODE) == IO$_SETMODE) {
        int fd = vms$$chan_to_fd(chan);
        if (fd < 0) return pcb_chan_unheld_status(chan);
        return qio_terminal_setmode(fd, p2, iosb_ptr, efn, astadr, astprm);
    }

    int fd, is_read;
    uint32_t status = qio_validate_and_classify(chan, func, iosb_ptr, p1,
                                                 efn, astadr, astprm,
                                                 &fd, &is_read);
    if (status == 0xFFFFFFFF) return SS$_NORMAL;  /* NOP handled */
    if (status != SS$_NORMAL) return status;

    uint32_t base_func = func & 0xFF;

    /* Try io_uring async submit (devices that can wait: terminals, pipes) */
    if (!qio_completes_at_once(fd) && uring_available()) {
        uint64_t offset = (p3 != 0) ? (uint64_t)p3 : (uint64_t)-1;
        int rc = vms_uring_submit_rw(fd, p1, p2, offset, is_read,
                                      iosb_ptr, efn, astadr, astprm);
        if (rc == 0)
            return SS$_NORMAL;
    }

    /* Synchronous fallback */
    return qio_sync(fd, base_func, iosb_ptr, p1, p2, efn, astadr, astprm);
}

/*
 * sys$qiow - Queue I/O Request and Wait for completion.
 *
 * Submits via io_uring and blocks until the I/O completes.
 * Falls back to synchronous I/O if io_uring is not available.
 */
static uint32_t qiow_body(uint32_t efn, uint16_t chan, uint32_t func,
                   void *iosb_ptr, void (*astadr)(uint32_t), uint32_t astprm,
                   void *p1, uint32_t p2, uint32_t p3,
                   uintptr_t p4, uintptr_t p5, uint32_t p6) {
    (void)p4; (void)p5; (void)p6;

    {   /* as for $QIO: clear the flag, refuse a number that is not a flag */
        uint32_t cst = qio_efn_request(efn);
        if (cst != SS$_NORMAL)
            return cst;
    }

    if (vms$$chan_is_mailbox(chan))
        return qio_mailbox_op(chan, func, iosb_ptr, p1, p2, efn, astadr, astprm);

    if (qio_null_writeof(chan, func, iosb_ptr, efn, astadr, astprm))
        return SS$_NORMAL;

    if (vms$$chan_is_bg(chan))
        return qio_bg_op(chan, func, iosb_ptr, p1, p2, p3, efn, astadr, astprm);

    if (vms$$chan_is_net(chan))
        return qio_net_op(chan, func, iosb_ptr, p1, p2, p3, efn, astadr, astprm);

    {
        uint32_t tec = 0;
        struct vms_devinfo tinfo;
        if (qio_chan_is_terminal(chan, &tec, &tinfo)) {
            /* the terminal's I/O is the executive driver's: no descriptor
             * is needed (rd vms-f8c) */
            int tfd = vms$$chan_to_fd(chan);
            int handled = 0;
            uint32_t tst = qio_terminal_op(chan, tfd, tec, &tinfo, func, iosb_ptr,
                                           p1, p2, p3, p4, p5, p6, efn, astadr,
                                           astprm, &handled);
            if (handled) return tst;
        }
    }

    /* IO$_SETMODE line discipline on a terminal channel (vms-f54): see sys$qio. */
    if ((func & IO$M_FCODE) == IO$_SETMODE) {
        int fd = vms$$chan_to_fd(chan);
        if (fd < 0) return pcb_chan_unheld_status(chan);
        return qio_terminal_setmode(fd, p2, iosb_ptr, efn, astadr, astprm);
    }

    int fd, is_read;
    uint32_t status = qio_validate_and_classify(chan, func, iosb_ptr, p1,
                                                 efn, astadr, astprm,
                                                 &fd, &is_read);
    if (status == 0xFFFFFFFF) return SS$_NORMAL;  /* NOP handled */
    if (status != SS$_NORMAL) return status;

    uint32_t base_func = func & 0xFF;

    /* Try io_uring: submit + wait (devices that can wait; a disk file or the
     * null device is done in place, as $QIO does it) */
    if (!qio_completes_at_once(fd) && uring_available()) {
        uint64_t offset = (p3 != 0) ? (uint64_t)p3 : (uint64_t)-1;
        int rc = vms_uring_submit_rw(fd, p1, p2, offset, is_read,
                                      iosb_ptr, efn, astadr, astprm);
        if (rc == 0) {
            vms_uring_wait_completion();
            if (iosb_ptr) {
                struct _iosb *iosb = (struct _iosb *)iosb_ptr;
                if (iosb->iosb$w_status == (uint16_t)SS$_ENDOFFILE)
                    return SS$_ENDOFFILE;
                if (iosb->iosb$w_status == (uint16_t)SS$_ABORT)
                    return SS$_ABORT;
            }
            return SS$_NORMAL;
        }
    }

    /* Synchronous fallback */
    return qio_sync(fd, base_func, iosb_ptr, p1, p2, efn, astadr, astprm);
}

/*
 * THE SERVICE STATUS IS NOT THE I/O STATUS (rd vms-d01). $QIO and $QIOW return
 * the status of QUEUING the request; how the I/O itself ended is in the IOSB
 * (and, for $QIO, signalled by the event flag and AST). Observed on OpenVMS
 * VAX V7.3 and Alpha V8.4 (docs/oracle/semantics/io/): a $QIOW read of NLA0:
 * or of an empty mailbox with IO$M_NOW is SS$_NORMAL with SS$_ENDOFFILE in the
 * IOSB; a short mailbox read is SS$_NORMAL with SS$_BUFFEROVF in the IOSB.
 * Only a request REFUSED before it was queued -- an illegal function, a write
 * too big for the mailbox, a bad channel or buffer, an exceeded quota --
 * returns its error as the service status, and then the IOSB is not an I/O
 * result: Alpha V8.4 zeroes it (VAX leaves it), and OVMX follows the Alpha --
 * except for a bad channel, refused before the IOSB is touched on both.
 */
static int qio_refused(uint32_t st)
{
    switch (st) {
    case SS$_ILLIOFUNC: case SS$_MBTOOSML: case SS$_IVCHAN: case SS$_IVIDENT:
    case SS$_BADPARAM:
    case SS$_ACCVIO: case SS$_EXQUOTA: case SS$_ILLEFC: case SS$_UNASEFC:
    case SS$_NOPRIV: case SS$_INSFMEM: case SS$_IVBUFLEN: case SS$_NOSUCHDEV:
    case SS$_DEVOFFLINE: case SS$_UNSUPPORTED: case SS$_MBFULL:
        return 1;
    default:
        return 0;
    }
}

static uint32_t qio_service_status(uint32_t st, void *iosb_ptr)
{
    int before_iosb = qio_refused_before_iosb;

    qio_refused_before_iosb = 0;
    if (st == SS$_NORMAL)
        return st;
    if (before_iosb)
        return st;              /* refused before the IOSB: left as it was */
    if (qio_refused(st)) {
        /* VMS clears the IOSB once the channel is validated, before the
         * driver's checks: a bad channel leaves it as it was (IO.CHAN0,
         * IO.AFTER_DASSGN), a refusal after that leaves it zeroed. */
        if (iosb_ptr && st != SS$_IVCHAN && st != SS$_IVIDENT)
            memset(iosb_ptr, 0, sizeof(struct _iosb));
        return st;
    }
    /* Completed (with whatever I/O status): the IOSB carries it. */
    if (iosb_ptr) {
        struct _iosb *b = (struct _iosb *)iosb_ptr;
        if (b->iosb$w_status == 0)
            b->iosb$w_status = (uint16_t)st;
    }
    return SS$_NORMAL;
}

uint32_t sys$qio(uint32_t efn, uint16_t chan, uint32_t func,
                 void *iosb_ptr, void (*astadr)(uint32_t), uint32_t astprm,
                 void *p1, uint32_t p2, uint32_t p3,
                 uintptr_t p4, uintptr_t p5, uint32_t p6)
{
    uint32_t cst = qio_efn_request(efn);   /* refused before anything: IOSB as is */
    if (cst != SS$_NORMAL)
        return cst;
    return qio_service_status(qio_body(efn, chan, func, iosb_ptr, astadr, astprm,
                                       p1, p2, p3, p4, p5, p6), iosb_ptr);
}

uint32_t sys$qiow(uint32_t efn, uint16_t chan, uint32_t func,
                  void *iosb_ptr, void (*astadr)(uint32_t), uint32_t astprm,
                  void *p1, uint32_t p2, uint32_t p3,
                  uintptr_t p4, uintptr_t p5, uint32_t p6)
{
    uint32_t cst = qio_efn_request(efn);
    if (cst != SS$_NORMAL)
        return cst;
    return qio_service_status(qiow_body(efn, chan, func, iosb_ptr, astadr, astprm,
                                        p1, p2, p3, p4, p5, p6), iosb_ptr);
}
