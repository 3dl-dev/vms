/*
 * vms_kif.c - Kernel Interface userspace implementation
 *
 * Wraps /dev/vms ioctl calls behind VMS-style function APIs.
 * Uses the libvmssys syscall wrappers (no glibc).
 */

#include "vms_kif.h"
#include "vms_syscall.h"
#include "vms_string.h"
#include "vms_errno.h"

/* File descriptor for /dev/vms — thread-local so each thread opens/closes
 * independently.  Callers must invoke vms_kif_open() before using any KIF
 * function and vms_kif_close() when done, once per thread. */
static __thread int vms_dev_fd = -1;

/*
 * The PROCESS this thread's executive registration belongs to, or 0 for
 * "not bound". Not a bare boolean, and that is the whole point (vms-9fc):
 *
 *   fork() copies thread-local storage wholesale. A child therefore
 *   inherits BOTH the parent's descriptor number and the parent's
 *   "already bound" mark, while the executive keys its process table by
 *   process -- so the child is NOT registered no matter what its TLS
 *   says. A boolean flag makes the child skip registration forever and
 *   every ioctl it issues is rejected. Recording WHICH process the mark
 *   belongs to is what makes the stale inherited mark detectable.
 *
 * It records a PROCESS id (getpid, the thread-group id) and not a thread
 * id (gettid), because that is what the registration is a property of:
 * on VMS a process has one PCB and its kernel threads share it, and the
 * executive keys struct vms_proc by current->tgid to match. Recording a
 * thread id here would be storing thread identity for a process-scoped
 * fact -- the same category error that, on the kernel side, minted one
 * VMS process per Linux thread (vms-9fc round 2).
 *
 * The comparison covers all three ways a thread can arrive here
 * unbound: a forked child (stale inherited mark, pid mismatch), a newly
 * created thread (fresh TLS, mark 0), and a freshly activated image
 * after execve() (TLS wiped, mark 0).
 */
static __thread vms_pid_t vms_bound_pid = 0;

/* ================================================================
 * Connection management
 * ================================================================ */

/*
 * vms_kif_kerr_to_ss - translate a failed ioctl's negative errno into the
 * VMS status the caller is entitled to see.
 *
 * This used to be a hard-coded SS$_BADPARAM for EVERY failure, which told
 * every caller that ITS parameters were bad -- a false statement about the
 * caller, and one that hid the real defect this file existed to expose.
 *
 * Both sides of /dev/vms are ours, so the errno set is CLOSED, not
 * open-ended: the kernel module sources return exactly -EFAULT, -ENOMEM, -ENOTTY,
 * -ESRCH and -EAGAIN. Each mapped status is ORACLE-PINNED on the reference
 * lab node VAX1, OpenVMS VAX V7.3, by two independent documented-tool
 * observations (vms-9fc, 2026-07-30):
 *
 *   LIBRARY/EXTRACT=$SSDEF/OUTPUT=... SYS$LIBRARY:STARLET.MLB + SEARCH:
 *       $EQU  SS$_ACCVIO     12
 *       $EQU  SS$_INSFMEM   292
 *       $EQU  SS$_ILLIOFUNC 244
 *       $EQU  SS$_BUGCHECK  676
 *   F$MESSAGE round-trip:
 *       12  -> %SYSTEM-F-ACCVIO,   access violation, ...
 *       292 -> %SYSTEM-F-INSFMEM,  insufficient dynamic memory
 *       244 -> %SYSTEM-F-ILLIOFUNC, illegal I/O function code
 *       676 -> %SYSTEM-F-BUGCHECK, internal consistency failure
 *
 * -EAGAIN never reaches here: it is only ever returned by DELIVERAST, and
 * vms_kif_deliverast consumes it in band as "no AST pending".
 *
 * -ESRCH ("this task has no entry in the executive's process table") is
 * deliberately NOT given a status of its own. VMS is never in that state:
 * a process always has a PCB. Under CLAUDE.md Rule 10 the answer to a
 * condition VMS never faces is to make it unreachable rather than to hand
 * it a plausible-looking status, and kif_bind() below is what makes it
 * unreachable. If it is ever observed anyway, the executive has lost a PCB
 * it is required to hold, which is exactly SS$_BUGCHECK -- "internal
 * consistency failure" -- and not something the caller did wrong.
 */
uint32_t vms_kif_kerr_to_ss(int err)
{
    switch (-err) {
    case VMS_EFAULT:  return SS$_ACCVIO;
    case VMS_ENOMEM:  return SS$_INSFMEM;
    case VMS_ENOTTY:  return SS$_ILLIOFUNC;
    default:          return SS$_BUGCHECK;
    }
}

int vms_kif_open(void)
{
    if (vms_dev_fd >= 0)
        return vms_dev_fd;

    vms_dev_fd = vms_sys_openat(-100 /* AT_FDCWD */, "/dev/vms", 2 /* O_RDWR */, 0);
    return vms_dev_fd;
}

void vms_kif_close(void)
{
    if (vms_dev_fd >= 0) {
        vms_sys_close(vms_dev_fd);
        vms_dev_fd = -1;
    }
    vms_bound_pid = 0;
}

uint32_t vms_kif_register(uint32_t *vms_pid)
{
    struct vms_register_args args;
    int rc;

    /* No "executive absent" check. The executive is INTEGRAL: PID 1 refuses
     * to bring the system up unless /dev/vms is open, and holds it open for
     * the life of the system (src/ovmx_init/ovmx_init.c, executive_attach),
     * so no caller can observe its absence. This guard used to report
     * SS$_BADPARAM for that impossible case, which was doubly wrong -- it
     * described an unreachable state, and it did so with a status that means
     * something else entirely. See CLAUDE.md Rule 9. */

    /* NO privilege argument and NO process-ID argument (vms-2b8). The
     * executive derives the authorized mask and the UIC from this task's
     * real credentials and ASSIGNS the VMS process ID itself; there is
     * nothing for the caller to ask for, and so nothing to forge. The
     * assigned ID comes back out -- like $CREPRC's pidadr, the executive
     * tells the caller what it decided. */
    vms_memset(&args, 0, sizeof(args));

    rc = vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_REGISTER, (unsigned long)&args);
    if (rc < 0)
        return vms_kif_kerr_to_ss(rc);

    /* Remember which process this registration belongs to, so an explicit
     * open+register (the sequence vms_kif.h documents, and the one the
     * kernel tests drive) satisfies kif_bind() without a second round trip. */
    if (args.status & 1)
        vms_bound_pid = vms_sys_getpid();

    if (vms_pid)
        *vms_pid = args.vms_pid;

    return args.status;
}

/*
 * kif_bind - complete the documented open -> register sequence for the
 * calling task, once, before any ioctl that needs a registered process.
 *
 * THIS IS THE DEFECT vms-9fc EXISTS TO FIX. vms_kif.h has always
 * documented the protocol ("1. open /dev/vms  2. call vms_kif_register()"),
 * vms_kif_register() has always existed -- and NOTHING in the product ever
 * called it. src/kernel/vms_module.c routes every ioctl except REGISTER
 * through vms_proc_find_or_err(), which returns -ESRCH for an unregistered
 * task, so EVERY /dev/vms call made by the product failed. That included
 * sys$ENQ/$DEQ, the one facility the executive-retrofit design named as
 * "already wired" and told every other implementer to copy.
 *
 * The fix belongs here and only here: binding once at the kernel-interface
 * layer is what makes every facility built on it inherit a registered
 * process, instead of each caller remembering a step that four separate
 * callers had already forgotten.
 *
 * Nothing is reported if the bind cannot be completed. That is deliberate
 * and must not be "fixed" by returning a status: the only way to fail here
 * is for the executive to be unreachable, and OVMX does not run without the
 * executive (CLAUDE.md Rules 9 and 10 -- PID 1 refuses to boot). A failed
 * ioctl on the unopened descriptor still yields an honest status through
 * vms_kif_kerr_to_ss(); what it must never yield is a fake success.
 */
static void kif_bind(void)
{
    vms_pid_t pid = vms_sys_getpid();

    if (vms_bound_pid == pid)
        return;

    /*
     * Any descriptor found here belongs to another PROCESS -- this
     * thread's TLS was inherited across fork(). It is a dup of that
     * process's channel, so drop it and take our own rather than sharing
     * a lifetime with a process the executive accounts for separately.
     * (A sibling thread of the SAME process never gets here with a
     * descriptor: TLS starts fresh at -1, and the register below is
     * adopted onto the process's existing PCB.)
     */
    if (vms_dev_fd >= 0) {
        vms_sys_close(vms_dev_fd);
        vms_dev_fd = -1;
    }

    /*
     * Opened unconditionally, and the result is deliberately NOT tested.
     * The only way it can fail is for the executive to be absent, and
     * OVMX is never in that state -- PID 1 refuses to bring the system up
     * without /dev/vms and holds the descriptor for the life of the
     * system. Branching on it would be handling a condition VMS never
     * faces (Rule 10), which is why tests/integration/test_runtime_target.sh
     * rejects the branch. If a descriptor somehow is not available the
     * REGISTER below fails and vms_bound_pid stays 0, so nothing is ever
     * told it is bound when it is not.
     */
    (void)vms_kif_open();

    /*
     * NOTHING IS PASSED. A process does not get to declare its own
     * privileges or its own process ID: on VMS both come from the
     * executive (the authorization record and $SETPRV for one, the
     * process database for the other), never from the image asking. This
     * call used to hand over getpid() and an init_privs quadword, and the
     * arguments are gone rather than zeroed (vms-2b8), so no caller can
     * reintroduce the self-declared-identity facade the VMS_PRIVILEGES
     * environment variable already is.
     */
    (void)vms_kif_register(NULL);
}

/*
 * kif_call - bind, then issue one ioctl.
 *
 * Returns 0 when the ioctl was delivered (the caller then reads the
 * operation's own status out of args), or a VMS status describing why the
 * ioctl itself could not be delivered. 0 is not a VMS status, so it is
 * unambiguous as a sentinel.
 */
static uint32_t kif_call(unsigned long req, void *args)
{
    int rc;

    kif_bind();

    rc = vms_sys_ioctl(vms_dev_fd, req, (unsigned long)args);
    return (rc < 0) ? vms_kif_kerr_to_ss(rc) : 0;
}

#define KIF_CALL(req, argsp) do {                   \
        uint32_t _kif_st = kif_call((req), (argsp)); \
        if (_kif_st)                                \
            return _kif_st;                         \
    } while (0)

/*
 * kif_wait_call - kif_call for the three BLOCKING event-flag services
 * ($WAITFR, $WFLOR, $WFLAND). It differs from kif_call in exactly one way:
 * a signal does not end the wait.
 *
 * WHY THIS LOOP EXISTS, AND WHY IT IS NOT "SWALLOWING AN ERROR" (vms-2a8).
 *
 * $WAITFR has no "your wait was interrupted" condition value. The service
 * returns when the flag is set, or it does not return; the only other
 * outcomes are the two argument errors (SS$_ILLEFC, SS$_UNASEFC) which are
 * decided before any waiting happens. On VMS the thing that interrupts a
 * wait is an AST, and the documented behaviour is that the AST executes and
 * then THE WAIT RESUMES -- $WAITFR does not report the interruption to its
 * caller, because from the caller's point of view nothing happened.
 *
 * Linux signals are the host mechanism that lands in the same place: the
 * executive's wait_event_interruptible() abandons the ioctl with
 * -ERESTARTSYS so the handler can run. If the handler was installed without
 * SA_RESTART, the kernel hands userspace -EINTR instead of restarting, and
 * a wait service would then have to answer for a wait that did not finish.
 * Under CLAUDE.md Rule 10 the answer to a condition VMS never faces is to
 * make it UNREACHABLE, not to invent a plausible status for it -- so the
 * wait is RE-ENTERED here and no caller can ever observe it.
 *
 * The executive writes NO status on that path (src/kernel/vms_eflag.c), so
 * this cannot mask a real result: the only thing being retried is a wait
 * whose predicate is still false. Every other errno still becomes a status
 * through kif_call's mapping.
 */
static uint32_t kif_wait_call(unsigned long req, void *args)
{
    int rc;

    kif_bind();

    do {
        rc = vms_sys_ioctl(vms_dev_fd, req, (unsigned long)args);
    } while (rc == -VMS_EINTR);

    return (rc < 0) ? vms_kif_kerr_to_ss(rc) : 0;
}

#define KIF_WAIT_CALL(req, argsp) do {                   \
        uint32_t _kif_st = kif_wait_call((req), (argsp)); \
        if (_kif_st)                                     \
            return _kif_st;                              \
    } while (0)

/* ================================================================
 * Access Mode (3a)
 * ================================================================ */

uint32_t vms_kif_setmode(uint8_t mode)
{
    struct vms_mode_args args;

    vms_memset(&args, 0, sizeof(args));
    args.mode = mode;

    KIF_CALL(VMS_IOCTL_SETMODE, &args);

    return args.status;
}

uint32_t vms_kif_getmode(uint8_t *mode, uint64_t *cur_privs, uint64_t *perm_privs)
{
    struct vms_getmode_args args;

    vms_memset(&args, 0, sizeof(args));

    KIF_CALL(VMS_IOCTL_GETMODE, &args);

    if (mode) *mode = args.mode;
    if (cur_privs) *cur_privs = args.cur_privs;
    if (perm_privs) *perm_privs = args.perm_privs;

    return 0x00000001; /* SS$_NORMAL */
}

uint32_t vms_kif_setprv(uint64_t mask, int enable, int permanent, uint64_t *prev)
{
    struct vms_priv_args args;

    vms_memset(&args, 0, sizeof(args));
    args.mask = mask;
    args.enable = enable ? 1 : 0;
    args.permanent = permanent ? 1 : 0;

    KIF_CALL(VMS_IOCTL_SETPRV, &args);

    if (prev) *prev = args.prev;
    return args.status;
}

uint32_t vms_kif_chkpriv(uint64_t mask)
{
    struct vms_priv_args args;

    vms_memset(&args, 0, sizeof(args));
    args.mask = mask;

    KIF_CALL(VMS_IOCTL_CHKPRIV, &args);

    return args.status;
}

/* ================================================================
 * AST Delivery (3b)
 * ================================================================ */

uint32_t vms_kif_dclast(uint64_t astadr, uint64_t astprm, uint8_t acmode)
{
    struct vms_ast_args args;

    vms_memset(&args, 0, sizeof(args));
    args.astadr = astadr;
    args.astprm = astprm;
    args.acmode = acmode;

    KIF_CALL(VMS_IOCTL_DCLAST, &args);

    return args.status;
}

uint32_t vms_kif_setast(int enable)
{
    struct vms_setast_args args;

    vms_memset(&args, 0, sizeof(args));
    args.enable = enable ? 1 : 0;

    KIF_CALL(VMS_IOCTL_SETAST, &args);

    return args.status;
}

int vms_kif_deliverast(uint64_t *astadr, uint64_t *astprm, uint8_t *acmode)
{
    struct vms_ast_args args;

    vms_memset(&args, 0, sizeof(args));

    /* VMS_IOCTL_DELIVERAST is _IOR: pass a pointer to the buffer the kernel
     * fills with the next pending AST. Returns 0 with the buffer populated
     * when an AST is delivered, or <0 (kernel -EAGAIN) when none is pending.
     *
     * This is the one entry point that does not go through KIF_CALL: it
     * reports "no AST pending" in band as -1 rather than as a VMS status,
     * so there is no status for kif_call() to return. It still binds. */
    kif_bind();

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_DELIVERAST, (unsigned long)&args) < 0)
        return -1;

    if (astadr) *astadr = args.astadr;
    if (astprm) *astprm = args.astprm;
    if (acmode) *acmode = args.acmode;
    return 0;
}

/* ================================================================
 * Event Flags (3c)
 * ================================================================ */

uint32_t vms_kif_setef(uint32_t efn)
{
    struct vms_ef_args args;

    vms_memset(&args, 0, sizeof(args));
    args.efn = efn;

    KIF_CALL(VMS_IOCTL_SETEF, &args);

    return args.status;
}

uint32_t vms_kif_clref(uint32_t efn)
{
    struct vms_ef_args args;

    vms_memset(&args, 0, sizeof(args));
    args.efn = efn;

    KIF_CALL(VMS_IOCTL_CLREF, &args);

    return args.status;
}

uint32_t vms_kif_waitfr(uint32_t efn)
{
    struct vms_ef_args args;

    vms_memset(&args, 0, sizeof(args));
    args.efn = efn;

    KIF_WAIT_CALL(VMS_IOCTL_WAITFR, &args);

    return args.status;
}

uint32_t vms_kif_wflor(uint32_t efn, uint32_t mask)
{
    struct vms_ef_wait_args args;

    vms_memset(&args, 0, sizeof(args));
    args.efn = efn;
    args.mask = mask;

    KIF_WAIT_CALL(VMS_IOCTL_WFLOR, &args);

    return args.status;
}

uint32_t vms_kif_wfland(uint32_t efn, uint32_t mask)
{
    struct vms_ef_wait_args args;

    vms_memset(&args, 0, sizeof(args));
    args.efn = efn;
    args.mask = mask;

    KIF_WAIT_CALL(VMS_IOCTL_WFLAND, &args);

    return args.status;
}

uint32_t vms_kif_readef(uint32_t efn, uint32_t *state)
{
    struct vms_ef_read_args args;

    vms_memset(&args, 0, sizeof(args));
    args.efn = efn;

    KIF_CALL(VMS_IOCTL_READEF, &args);

    if (state) *state = args.state;
    return args.status;
}

uint32_t vms_kif_ascefc(uint32_t efn, const char *name, uint32_t prot, uint32_t perm)
{
    struct vms_ef_common_args args;

    vms_memset(&args, 0, sizeof(args));
    args.efn = efn;
    if (name)
        vms_strncpy(args.name, name, 31);
    args.name[31] = '\0';
    args.prot = prot;
    args.perm = perm;

    KIF_CALL(VMS_IOCTL_ASCEFC, &args);

    return args.status;
}

uint32_t vms_kif_dacefc(uint32_t efn)
{
    struct vms_ef_args args;

    vms_memset(&args, 0, sizeof(args));
    args.efn = efn;

    KIF_CALL(VMS_IOCTL_DACEFC, &args);

    return args.status;
}

uint32_t vms_kif_dlcefc(const char *name)
{
    struct vms_ef_common_args args;

    vms_memset(&args, 0, sizeof(args));
    if (name)
        vms_strncpy(args.name, name, 31);
    args.name[31] = '\0';

    KIF_CALL(VMS_IOCTL_DLCEFC, &args);

    return args.status;
}

/* ================================================================
 * Lock Manager (3d)
 * ================================================================ */

uint32_t vms_kif_enq(uint32_t efn, uint32_t lkmode, uint32_t flags,
                      const char *resnam, uint32_t parid,
                      uint64_t astadr, uint64_t astprm,
                      uint64_t blkastadr,
                      uint32_t *lkid, uint8_t *valblk)
{
    struct vms_enq_args args;

    vms_memset(&args, 0, sizeof(args));
    args.efn = efn;
    args.lkmode = lkmode;
    args.flags = flags;
    args.parid = parid;
    if (resnam)
        vms_strncpy(args.resnam, resnam, 31);
    args.resnam[31] = '\0';
    args.astadr = astadr;
    args.astprm = astprm;
    args.blkastadr = blkastadr;

    if (valblk && (flags & LCK_M_VALBLK))
        vms_memcpy(args.valblk, valblk, LCK_VALBLK_SIZE);

    KIF_CALL(VMS_IOCTL_ENQ, &args);

    if (lkid) *lkid = args.lkid;
    if (valblk && (flags & LCK_M_VALBLK))
        vms_memcpy(valblk, args.valblk, LCK_VALBLK_SIZE);

    return args.status;
}

uint32_t vms_kif_deq(uint32_t lkid, uint8_t *valblk, uint32_t flags)
{
    struct vms_deq_args args;

    vms_memset(&args, 0, sizeof(args));
    args.lkid = lkid;
    args.flags = flags;

    if (valblk && (flags & LCK_M_VALBLK))
        vms_memcpy(args.valblk, valblk, LCK_VALBLK_SIZE);

    KIF_CALL(VMS_IOCTL_DEQ, &args);

    return args.status;
}

uint32_t vms_kif_convert(uint32_t lkid, uint32_t lkmode, uint32_t flags,
                          uint64_t blkastadr, uint8_t *valblk)
{
    struct vms_enq_args args;

    vms_memset(&args, 0, sizeof(args));
    args.lkid = lkid;
    args.lkmode = lkmode;
    args.flags = flags | LCK_M_CONVERT;
    args.blkastadr = blkastadr;

    if (valblk && (flags & LCK_M_VALBLK))
        vms_memcpy(args.valblk, valblk, LCK_VALBLK_SIZE);

    KIF_CALL(VMS_IOCTL_CONVERT, &args);

    if (valblk && (flags & LCK_M_VALBLK))
        vms_memcpy(valblk, args.valblk, LCK_VALBLK_SIZE);

    return args.status;
}

uint32_t vms_kif_getlki(uint32_t lkid, uint32_t *granted_mode,
                          uint32_t *requested_mode, char *resnam,
                          uint8_t *valblk)
{
    struct vms_getlki_args args;

    vms_memset(&args, 0, sizeof(args));
    args.lkid = lkid;

    KIF_CALL(VMS_IOCTL_GETLKI, &args);

    if (granted_mode) *granted_mode = args.granted_mode;
    if (requested_mode) *requested_mode = args.requested_mode;
    if (resnam) vms_strncpy(resnam, args.resnam, 32);
    if (valblk) vms_memcpy(valblk, args.valblk, LCK_VALBLK_SIZE);

    return args.status;
}

/* ================================================================
 * Device table (executive-resident I/O database)
 *
 * These are readers of, and a channel-scoped writer to, shared state
 * the executive owns. What they report about a device is what every
 * other process on the node sees -- not a per-process idea of what a
 * device looks like.
 * ================================================================ */

uint32_t vms_kif_assign(const char *devnam, uint32_t *chan)
{
    struct vms_assign_args args;

    if (!devnam || !chan)
        return 0x00000014; /* SS$_BADPARAM */

    vms_memset(&args, 0, sizeof(args));
    vms_strncpy(args.devnam, devnam, VMS_DEVNAM_SIZE - 1);
    args.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    KIF_CALL(VMS_IOCTL_ASSIGN, &args);

    /* VMS writes the channel only on success (odd status); a failed
     * $ASSIGN must not disturb the caller's channel variable. */
    if (args.status & 1)
        *chan = args.chan;

    return args.status;
}

/* Shared body for $ALLOC and $DALLOC: both name a device and return a
 * status, and neither writes anything back to the caller. */
static uint32_t vms_kif_alloc_op(unsigned long req, const char *devnam)
{
    struct vms_alloc_args args;

    if (!devnam)
        return 0x00000014; /* SS$_BADPARAM */

    vms_memset(&args, 0, sizeof(args));
    vms_strncpy(args.devnam, devnam, VMS_DEVNAM_SIZE - 1);
    args.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    KIF_CALL(req, &args);

    return args.status;
}

uint32_t vms_kif_alloc(const char *devnam)
{
    return vms_kif_alloc_op(VMS_IOCTL_ALLOC, devnam);
}

uint32_t vms_kif_dalloc(const char *devnam)
{
    return vms_kif_alloc_op(VMS_IOCTL_DALLOC, devnam);
}

uint32_t vms_kif_dassgn(uint32_t chan)
{
    struct vms_dassgn_args args;

    vms_memset(&args, 0, sizeof(args));
    args.chan = chan;

    KIF_CALL(VMS_IOCTL_DASSGN, &args);

    return args.status;
}

uint32_t vms_kif_getdvi_devnam(const char *devnam, struct vms_devinfo *info)
{
    struct vms_getdvi_args args;

    if (!devnam)
        return 0x00000014; /* SS$_BADPARAM */

    vms_memset(&args, 0, sizeof(args));
    args.select = VMS_DVI_SEL_DEVNAM;
    vms_strncpy(args.info.devnam, devnam, VMS_DEVNAM_SIZE - 1);
    args.info.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    KIF_CALL(VMS_IOCTL_GETDVI, &args);

    if (info)
        vms_memcpy(info, &args.info, sizeof(*info));

    return args.status;
}

uint32_t vms_kif_getdvi_chan(uint32_t chan, struct vms_devinfo *info)
{
    struct vms_getdvi_args args;

    vms_memset(&args, 0, sizeof(args));
    args.select = VMS_DVI_SEL_CHAN;
    args.chan = chan;

    KIF_CALL(VMS_IOCTL_GETDVI, &args);

    if (info)
        vms_memcpy(info, &args.info, sizeof(*info));

    return args.status;
}

uint32_t vms_kif_devscan(uint32_t *index, struct vms_devinfo *info)
{
    struct vms_devscan_args args;

    if (!index)
        return 0x00000014; /* SS$_BADPARAM */

    vms_memset(&args, 0, sizeof(args));
    args.index = *index;

    KIF_CALL(VMS_IOCTL_DEVSCAN, &args);

    *index = args.index;
    if (info)
        vms_memcpy(info, &args.info, sizeof(*info));

    return args.status;
}

uint32_t vms_kif_ttsetmode(uint32_t chan, uint32_t flags,
                           uint64_t setchar, uint64_t clrchar,
                           uint32_t width, uint32_t page)
{
    struct vms_setmode_args args;

    vms_memset(&args, 0, sizeof(args));
    args.chan = chan;
    args.flags = flags;
    args.setchar = setchar;
    args.clrchar = clrchar;
    args.width = width;
    args.page = page;

    KIF_CALL(VMS_IOCTL_TTSETMODE, &args);

    return args.status;
}

uint32_t vms_kif_setterm(uint32_t chan)
{
    struct vms_setterm_args args;

    vms_memset(&args, 0, sizeof(args));
    args.chan = chan;

    KIF_CALL(VMS_IOCTL_SETTERM, &args);

    return args.status;
}

/* ================================================================
 * Process table (executive-resident PCB directory)
 * ================================================================ */

uint32_t vms_kif_setprn(const char *prcnam)
{
    struct vms_setprn_args args;

    if (!prcnam)
        return 0x00000014; /* SS$_BADPARAM */

    /*
     * Copy into the inbound transfer buffer, NOT into a
     * VMS_PRCNAM_SIZE field: the executive decides whether the name is
     * legal, and it can only do that if it receives the name the caller
     * actually passed. Clipping here at VMS_PRCNAM_SIZE would hand the
     * executive a legal-looking name and return SS$_NORMAL for a name
     * VMS rejects with SS$_IVLOGNAM.
     */
    vms_memset(&args, 0, sizeof(args));
    vms_strncpy(args.prcnam, prcnam, VMS_PRCNAM_XFER - 1);
    args.prcnam[VMS_PRCNAM_XFER - 1] = '\0';

    KIF_CALL(VMS_IOCTL_SETPRN, &args);

    return args.status;
}

/*
 * getjpi_common - issue one VMS_IOCTL_GETJPI with a prepared selector.
 */
static uint32_t getjpi_common(struct vms_getjpi_args *args,
                              struct vms_procinfo *info)
{
    KIF_CALL(VMS_IOCTL_GETJPI, args);

    if (info)
        vms_memcpy(info, &args->info, sizeof(*info));

    return args->status;
}

uint32_t vms_kif_getjpi_self(struct vms_procinfo *info)
{
    struct vms_getjpi_args args;

    vms_memset(&args, 0, sizeof(args));
    args.select = VMS_JPI_SEL_SELF;

    return getjpi_common(&args, info);
}

uint32_t vms_kif_getjpi_pid(uint32_t vms_pid, struct vms_procinfo *info)
{
    struct vms_getjpi_args args;

    vms_memset(&args, 0, sizeof(args));
    args.select = VMS_JPI_SEL_PID;
    args.info.vms_pid = vms_pid;

    return getjpi_common(&args, info);
}

uint32_t vms_kif_getjpi_prcnam(const char *prcnam, struct vms_procinfo *info)
{
    struct vms_getjpi_args args;

    if (!prcnam)
        return 0x00000014; /* SS$_BADPARAM */

    /*
     * Same rule as vms_kif_setprn: the lookup key travels untruncated,
     * in sel_prcnam. Clipping an oversized key to VMS_PRCNAM_SIZE would
     * make it resolve whatever process happens to hold the first 15
     * characters -- answering for a DIFFERENT process instead of
     * rejecting the illegal name.
     */
    vms_memset(&args, 0, sizeof(args));
    args.select = VMS_JPI_SEL_PRCNAM;
    vms_strncpy(args.sel_prcnam, prcnam, VMS_PRCNAM_XFER - 1);
    args.sel_prcnam[VMS_PRCNAM_XFER - 1] = '\0';

    return getjpi_common(&args, info);
}

/*
 * vms_kif_setident - hand the executive an AUTHENTICATED identity.
 *
 * The caller has just proved this identity (SYSUAF lookup + password
 * check) while holding privilege. From here the identity belongs to the
 * executive: this process cannot widen it again without SETPRV, and
 * every other process reads it back through $GETJPI.
 *
 * SS$_NOPRIV if the caller lacks SETPRV and the identity is not a
 * weakening of its own; SS$_IVLOGNAM if the user name is malformed.
 *
 * FIXED (vms-fb9 r6): this issued a raw vms_sys_ioctl() instead of going
 * through KIF_CALL, so it never called kif_bind() and reached an
 * unregistered process's entry. Measured against a real /dev/vms:
 * vms_kif_open() followed by a BARE vms_kif_setident() (no explicit
 * vms_kif_register(), no other vms_kif_* call first) returned status=20
 * (SS$_BADPARAM) -- not because the parameters were bad, but because the
 * unbound ioctl was rejected -ESRCH and the old failure path hard-coded
 * SS$_BADPARAM for every failure. Now routed through KIF_CALL, so it
 * binds before its ioctl; the same probe now returns status=1
 * (SS$_NORMAL). See tests/qemu/test_kmod_bind.c suite 0.
 */
uint32_t vms_kif_setident(const char *username, uint32_t uic,
                          uint64_t authorized_privs)
{
    struct vms_ident_args args;

    if (!username)
        return 0x00000014; /* SS$_BADPARAM */

    vms_memset(&args, 0, sizeof(args));
    vms_strncpy(args.username, username, VMS_USERNAME_SIZE - 1);
    args.username[VMS_USERNAME_SIZE - 1] = '\0';
    args.uic = uic;
    args.authorized_privs = authorized_privs;

    KIF_CALL(VMS_IOCTL_SETIDENT, &args);

    return args.status;
}

uint32_t vms_kif_procscan(uint32_t *index, struct vms_procinfo *info)
{
    struct vms_procscan_args args;

    if (!index)
        return 0x00000014; /* SS$_BADPARAM */

    vms_memset(&args, 0, sizeof(args));
    args.index = *index;

    KIF_CALL(VMS_IOCTL_PROCSCAN, &args);

    *index = args.index;
    if (info)
        vms_memcpy(info, &args.info, sizeof(*info));

    return args.status;
}

/* ================================================================
 * Logical name tables (executive-resident LNM$SYSTEM, vms-d37)
 * ================================================================ */

/* VMS_PROT_READ / VMS_MAP_SHARED / VMS_MAP_FAILED come from vms_types.h
 * (via vms_syscall.h) -- the same mmap flags the rest of libvmssys uses. */

/*
 * The read-only view of the executive's logical-name arena, mapped once
 * per PROCESS. Thread-local like vms_dev_fd, and re-mapped after fork()
 * exactly as the descriptor is re-bound: a child inherits the TLS pointer
 * to a mapping that belongs to the parent's fd, so it is discarded and the
 * child maps its own (guarded by the pid the mapping was made under).
 */
static __thread struct vms_lnm_arena *vms_lnm_map = 0;
static __thread vms_pid_t vms_lnm_map_pid = 0;

/*
 * vms_kif_lnm_arena - obtain (once) the read-only arena mapping.
 *
 * Binds first (open + register), then mmaps the arena off the /dev/vms fd.
 * Returns NULL when the executive is absent -- there is NO fallback: the
 * caller must surface that as SS$_NOSUCHDEV, never fake a private table.
 */
static struct vms_lnm_arena *vms_kif_lnm_arena(void)
{
    vms_pid_t pid = vms_sys_getpid();
    void *p;

    if (vms_lnm_map && vms_lnm_map_pid == pid)
        return vms_lnm_map;

    /* Stale inherited mapping from a fork()'d parent: drop it. */
    if (vms_lnm_map) {
        vms_sys_munmap(vms_lnm_map, sizeof(struct vms_lnm_arena));
        vms_lnm_map = 0;
        vms_lnm_map_pid = 0;
    }

    kif_bind();
    if (vms_dev_fd < 0)
        return 0;

    p = vms_sys_mmap(0, sizeof(struct vms_lnm_arena),
                     VMS_PROT_READ, VMS_MAP_SHARED,
                     vms_dev_fd, VMS_LNM_MMAP_OFFSET);
    if (p == VMS_MAP_FAILED || !p)
        return 0;

    /* Trust the mapping only if the executive's header validates. */
    {
        struct vms_lnm_arena *a = (struct vms_lnm_arena *)p;
        if (a->magic != VMS_LNM_ARENA_MAGIC ||
            a->version != VMS_LNM_ARENA_VERSION) {
            vms_sys_munmap(p, sizeof(struct vms_lnm_arena));
            return 0;
        }
    }

    vms_lnm_map = (struct vms_lnm_arena *)p;
    vms_lnm_map_pid = pid;
    return vms_lnm_map;
}

/* Read the seqlock generation with acquire ordering. */
static uint64_t lnm_gen_load(const struct vms_lnm_arena *a)
{
    return __atomic_load_n(&a->generation, __ATOMIC_ACQUIRE);
}

uint32_t vms_kif_lnm_define(uint32_t table, const char *name,
                            const char *const *values, uint8_t num_values,
                            uint32_t attributes, uint8_t acmode)
{
    struct vms_lnm_def_args args;
    unsigned i;

    if (!name || !values || num_values == 0 || num_values > VMS_LNM_MAX_EQUIV)
        return 0x00000014; /* SS$_BADPARAM */

    /*
     * Reaching the executive is REQUIRED. If the mapping is unavailable the
     * executive is absent, so fail honestly rather than mutate nothing and
     * report success (INV-6). The mmap doubles as the executive-present probe
     * the mutation path needs.
     */
    if (!vms_kif_lnm_arena())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.table = table;
    args.attributes = attributes;
    args.acmode = acmode;
    args.num_equiv = num_values;
    vms_strncpy(args.name, name, VMS_LNM_MAX_NAME);
    args.name[VMS_LNM_MAX_NAME] = '\0';
    args.name_length = (uint16_t)vms_strlen(args.name);

    for (i = 0; i < num_values; i++) {
        const char *v = values[i] ? values[i] : "";
        vms_strncpy(args.equiv[i].value, v, VMS_LNM_MAX_VALUE);
        args.equiv[i].value[VMS_LNM_MAX_VALUE] = '\0';
        args.equiv[i].length = (uint16_t)vms_strlen(args.equiv[i].value);
        args.equiv[i].index = (uint8_t)i;
    }

    KIF_CALL(VMS_IOCTL_LNM_DEFINE, &args);

    return args.status;
}

uint32_t vms_kif_lnm_delete(uint32_t table, const char *name, uint8_t acmode)
{
    struct vms_lnm_del_args args;

    if (!name)
        return 0x00000014; /* SS$_BADPARAM */

    if (!vms_kif_lnm_arena())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.table = table;
    args.acmode = acmode;
    vms_strncpy(args.name, name, VMS_LNM_MAX_NAME);
    args.name[VMS_LNM_MAX_NAME] = '\0';
    args.name_length = (uint16_t)vms_strlen(args.name);

    KIF_CALL(VMS_IOCTL_LNM_DELETE, &args);

    return args.status;
}

int vms_kif_lnm_translate(uint32_t table, const char *name,
                          char *value, uint32_t valsz,
                          uint16_t *vallen, uint32_t *attrs)
{
    struct vms_lnm_arena *a;
    uint32_t scope_key;
    int tries;

    if (!name || !value || valsz == 0)
        return -1;

    a = vms_kif_lnm_arena();
    if (!a)
        return -1;   /* executive absent -> caller renders SS$_NOSUCHDEV */

    /* SYSTEM is singular (scope 0). GROUP/JOB scoping is deferred. */
    scope_key = (table == VMS_LNM_TBL_SYSTEM) ? 0u : 0u;

    /*
     * Seqlock read: sample the generation, walk the table, re-sample. Retry
     * on an odd (write in flight) or changed counter. Writes are rare and
     * brief, so this effectively never spins; the bound keeps a pathological
     * writer from wedging a reader.
     */
    for (tries = 0; tries < 1024; tries++) {
        uint64_t g0 = lnm_gen_load(a);
        uint32_t i, max;
        int found = 0;
        char vbuf[VMS_LNM_MAX_VALUE + 1];
        uint16_t vlen = 0;
        uint32_t vattr = 0;

        if (g0 & 1ULL)
            continue;   /* write in flight */

        max = a->max_entries;
        if (max > VMS_LNM_MAX_ENTRIES)
            max = VMS_LNM_MAX_ENTRIES;

        for (i = 0; i < max; i++) {
            const struct vms_lnm_entry *e = &a->entries[i];

            if (!e->in_use || e->table != table || e->scope_key != scope_key)
                continue;
            if (vms_strcasecmp(e->name, name) != 0)
                continue;
            if (e->num_equiv == 0)
                continue;

            vlen = e->equiv[0].length;
            if (vlen > VMS_LNM_MAX_VALUE)
                vlen = VMS_LNM_MAX_VALUE;
            vms_memcpy(vbuf, e->equiv[0].value, vlen);
            vbuf[vlen] = '\0';
            vattr = e->attributes;
            found = 1;
            break;
        }

        /* Re-sample: if the arena changed under us, the read is not stable. */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (lnm_gen_load(a) != g0)
            continue;

        if (!found)
            return 0;

        if (vlen >= valsz)
            vlen = (uint16_t)(valsz - 1);
        vms_memcpy(value, vbuf, vlen);
        value[vlen] = '\0';
        if (vallen)
            *vallen = vlen;
        if (attrs)
            *attrs = vattr;
        return 1;
    }

    /* Writer never quiesced -- treat as unavailable rather than guess. */
    return -1;
}
