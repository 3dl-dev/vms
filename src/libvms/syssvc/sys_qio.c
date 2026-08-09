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
    uint32_t base_func = func & 0xFF;
    uint32_t st;
    uint32_t actlen = 0;

    switch (base_func) {
        case IO$_READVBLK:
        case IO$_READLBLK:
        case IO$_READPBLK:
            if (!p1) { st = SS$_BADPARAM; break; }
            st = vms_kif_mbx_read(exec_chan, p1, p2, &actlen);
            break;

        case IO$_WRITEVBLK:
        case IO$_WRITELBLK:
        case IO$_WRITEPBLK:
            if (!p1) { st = SS$_BADPARAM; break; }
            st = vms_kif_mbx_write(exec_chan, p1, p2);
            if (st & 1) actlen = p2;
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
