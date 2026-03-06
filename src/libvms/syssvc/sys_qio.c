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

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include "starlet.h"
#include "vms/pcb.h"

/* Import from sys_assign.c */
extern int vms$$chan_to_fd(uint16_t chan);

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

    int fd = vms$$chan_to_fd(chan);
    if (fd < 0) return SS$_IVCHAN;

    uint32_t base_func = func & 0xFF;  /* Strip function modifiers */

    /* Handle IO$_NOP synchronously -- no I/O needed */
    if (base_func == IO$_NOP) {
        struct _iosb *iosb = (struct _iosb *)iosb_ptr;
        if (iosb) {
            iosb->iosb$w_status = (uint16_t)SS$_NORMAL;
            iosb->iosb$w_bcnt = 0;
            iosb->iosb$l_dev_depend = 0;
        }
        if (efn != 0) sys$setef(efn);
        if (astadr) astadr(astprm);
        return SS$_NORMAL;
    }

    /* Determine if this is a read or write operation */
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

    /* Try io_uring async submit */
    if (uring_available()) {
        /*
         * Use p3 as the byte offset for block I/O.
         * Pass (uint64_t)-1 to let the kernel use the file position
         * when p3 is 0 (stream I/O).
         */
        uint64_t offset = (p3 != 0) ? (uint64_t)p3 : (uint64_t)-1;
        int rc = vms_uring_submit_rw(fd, p1, p2, offset, is_read,
                                      iosb_ptr, efn, astadr, astprm);
        if (rc == 0)
            return SS$_NORMAL;
        /* Fall through to synchronous if submit failed */
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

    int fd = vms$$chan_to_fd(chan);
    if (fd < 0) return SS$_IVCHAN;

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
        return SS$_NORMAL;
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

    /* Try io_uring: submit + wait */
    if (uring_available()) {
        uint64_t offset = (p3 != 0) ? (uint64_t)p3 : (uint64_t)-1;
        int rc = vms_uring_submit_rw(fd, p1, p2, offset, is_read,
                                      iosb_ptr, efn, astadr, astprm);
        if (rc == 0) {
            /* Block until completion */
            vms_uring_wait_completion();

            /* Return status from IOSB if available */
            if (iosb_ptr) {
                struct _iosb *iosb = (struct _iosb *)iosb_ptr;
                if (iosb->iosb$w_status == (uint16_t)SS$_ENDOFFILE)
                    return SS$_ENDOFFILE;
                if (iosb->iosb$w_status == (uint16_t)SS$_ABORT)
                    return SS$_ABORT;
            }
            return SS$_NORMAL;
        }
        /* Fall through to synchronous if submit failed */
    }

    /* Synchronous fallback */
    return qio_sync(fd, base_func, iosb_ptr, p1, p2, efn, astadr, astprm);
}
