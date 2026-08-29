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
#include "starlet.h"
#include "vms/pcb.h"
#include "vms_kif.h"

/* Import from sys_assign.c */
extern int vms$$chan_to_fd(uint16_t chan);
extern uint32_t vms$$chan_exec_chan(uint16_t chan);
extern int vms$$chan_is_mailbox(uint16_t chan);
extern int vms$$chan_is_bg(uint16_t chan);

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
    if (efn != 0) {
        sys$setef(efn);
    }

    /* Call AST completion routine if provided */
    if (astadr) {
        astadr(astprm);
    }

    return SS$_NORMAL;
}

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
            st = vms_kif_mbx_read(exec_chan, p1, p2, &actlen,
                                  (func & IO$M_NOW) != 0);
            break;

        case IO$_WRITEVBLK:
        case IO$_WRITELBLK:
        case IO$_WRITEPBLK:
            if (!p1) { st = SS$_BADPARAM; break; }
            st = vms_kif_mbx_write(exec_chan, p1, p2);
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

    if (iosb) {
        iosb->iosb$w_status = (uint16_t)st;
        iosb->iosb$w_bcnt = (actlen > 65535) ? 65535 : (uint16_t)actlen;
        iosb->iosb$l_dev_depend = actlen;
    }

    if (st & 1) {
        if (efn != 0) sys$setef(efn);
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
        if (efn != 0) sys$setef(efn);
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
    if (fd < 0) return SS$_IVCHAN;

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
        if (efn != 0) sys$setef(efn);
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
 * sys$qio - Queue I/O Request (asynchronous).
 *
 * Submits the I/O via io_uring and returns immediately. The IOSB is
 * filled, event flag set, and AST called when the I/O completes.
 * Falls back to synchronous I/O if io_uring is not available.
 */
uint32_t sys$qio(uint32_t efn, uint16_t chan, uint32_t func,
                  void *iosb_ptr, void (*astadr)(uint32_t), uint32_t astprm,
                  void *p1, uint32_t p2, uint32_t p3,
                  uint32_t p4, uint32_t p5, uint32_t p6) {
    (void)p4; (void)p5; (void)p6;

    if (vms$$chan_is_mailbox(chan))
        return qio_mailbox_op(chan, func, iosb_ptr, p1, p2, efn, astadr, astprm);

    if (vms$$chan_is_bg(chan))
        return qio_bg_op(chan, func, iosb_ptr, p1, p2, p3, efn, astadr, astprm);

    int fd, is_read;
    uint32_t status = qio_validate_and_classify(chan, func, iosb_ptr, p1,
                                                 efn, astadr, astprm,
                                                 &fd, &is_read);
    if (status == 0xFFFFFFFF) return SS$_NORMAL;  /* NOP handled */
    if (status != SS$_NORMAL) return status;

    uint32_t base_func = func & 0xFF;

    /* Try io_uring async submit */
    if (uring_available()) {
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
uint32_t sys$qiow(uint32_t efn, uint16_t chan, uint32_t func,
                   void *iosb_ptr, void (*astadr)(uint32_t), uint32_t astprm,
                   void *p1, uint32_t p2, uint32_t p3,
                   uint32_t p4, uint32_t p5, uint32_t p6) {
    (void)p4; (void)p5; (void)p6;

    if (vms$$chan_is_mailbox(chan))
        return qio_mailbox_op(chan, func, iosb_ptr, p1, p2, efn, astadr, astprm);

    if (vms$$chan_is_bg(chan))
        return qio_bg_op(chan, func, iosb_ptr, p1, p2, p3, efn, astadr, astprm);

    int fd, is_read;
    uint32_t status = qio_validate_and_classify(chan, func, iosb_ptr, p1,
                                                 efn, astadr, astprm,
                                                 &fd, &is_read);
    if (status == 0xFFFFFFFF) return SS$_NORMAL;  /* NOP handled */
    if (status != SS$_NORMAL) return status;

    uint32_t base_func = func & 0xFF;

    /* Try io_uring: submit + wait */
    if (uring_available()) {
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
