/*
 * vms_kif.c - Kernel Interface userspace implementation
 *
 * Holds the SUBSTRATE-AGNOSTIC POLICY of the executive interface: the
 * open->register binding, the caller-census / INV-6 honest-fail behaviour, and
 * the errno->SS$ status mapping. It reaches the executive only through the
 * transport seam (kif_transport.h): device open, request issue and arena mmap
 * are the substrate's job (kif_transport_linux.c on OVMX/Linux), so a future
 * OVMX/NetBSD transport can plug in without touching this policy layer
 * (vms-a3b, docs/design-ovmx-netbsd-syskrnl.md §3).
 *
 * Uses the libvmssys syscall wrappers (no glibc) for the non-device syscalls
 * that remain policy's own: getpid() (identity) and mprotect() (the P1
 * critical-page primitive, which has no /dev/vms dependency at all).
 */

#include "vms_kif.h"
#include "kif_transport.h"
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

    vms_dev_fd = kif_xport_dev_open();
    return vms_dev_fd;
}

void vms_kif_close(void)
{
    if (vms_dev_fd >= 0) {
        kif_xport_dev_close(vms_dev_fd);
        vms_dev_fd = -1;
    }
    vms_bound_pid = 0;
}

/*
 * kif_register_req - issue ONE registration ioctl (REGISTER or the
 * image-activation REGISTER_CONTINUE) and record the binding.
 *
 * No "executive absent" check. The executive is INTEGRAL: PID 1 refuses to
 * bring the system up unless /dev/vms is open, and holds it open for the
 * life of the system (src/ovmx_init/ovmx_init.c, executive_attach), so no
 * caller can observe its absence. See CLAUDE.md Rule 9.
 *
 * NO privilege argument and NO process-ID argument (vms-2b8), and none for
 * REGISTER_CONTINUE either (vms-4d7): the executive derives or continues
 * the identity from the task hierarchy, never from anything the caller
 * hands it. The assigned/shared VMS PID comes back out.
 */
static uint32_t kif_register_req(unsigned long req, uint32_t *vms_pid)
{
    struct vms_register_args args;
    int rc;

    vms_memset(&args, 0, sizeof(args));

    rc = kif_xport_ioctl(vms_dev_fd, req, &args);
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

uint32_t vms_kif_register(uint32_t *vms_pid)
{
    return kif_register_req(VMS_IOCTL_REGISTER, vms_pid);
}

/*
 * vms_kif_register_continue - register the CALLING task as a continuation
 * of its VMS parent's process, not as a new one (vms-4d7, Option B).
 *
 * DCL calls this in the forked child of an image activation, BEFORE execve
 * replaces the image (src/vmsdcl/dcl_cmd_process.c, dcl_activate_image).
 * At that instant the child's real_parent is still DCL, so the executive
 * can read DCL's identity and share its VMS PID, UIC, user name and
 * privileges onto this task -- which is why SYSTEM's RUN AUTHORIZE now
 * holds SYSPRV. The PCB is keyed on the thread group and survives the
 * execve, so the activated image inherits the continued identity without
 * doing anything itself (its own first kif call ADOPTS the existing row).
 *
 * The /dev/vms descriptor found in TLS here belongs to the PARENT (it was
 * dup'd across fork), so it is dropped and a fresh one taken, exactly as
 * kif_bind() does -- this child is accounted separately even though it
 * shares the parent's VMS identity.
 *
 * Returns the registration status; on success vms_bound_pid is set so the
 * post-execve image's kif_bind() is a no-op adopt.
 */
uint32_t vms_kif_register_continue(void)
{
    if (vms_dev_fd >= 0) {
        kif_xport_dev_close(vms_dev_fd);
        vms_dev_fd = -1;
    }
    vms_bound_pid = 0;

    (void)vms_kif_open();
    return kif_register_req(VMS_IOCTL_REGISTER_CONTINUE, (uint32_t *)0);
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
        kif_xport_dev_close(vms_dev_fd);
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

    rc = kif_xport_ioctl(vms_dev_fd, req, args);
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
        rc = kif_xport_ioctl(vms_dev_fd, req, args);
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

    if (kif_xport_ioctl(vms_dev_fd, VMS_IOCTL_DELIVERAST, &args) < 0)
        return -1;

    if (astadr) *astadr = args.astadr;
    if (astprm) *astprm = args.astprm;
    if (acmode) *acmode = args.acmode;
    return 0;
}

/*
 * vms_kif_hiber - block until a $WAKE is pending or an AST is deliverable.
 *
 * Blocking, like the event-flag waits: kif_wait_call re-enters past a bare
 * signal (EINTR) so a signal does not end $HIBER -- only the executive
 * returning (a $WAKE consumed, or an AST become deliverable) does. Returns the
 * `woken` flag the executive set: 1 = released by $WAKE, 0 = by an AST.
 */
uint32_t vms_kif_hiber(int *woken)
{
    struct vms_hiber_args args;
    uint32_t st;

    vms_memset(&args, 0, sizeof(args));

    /* kif_wait_call returns 0 when the ioctl was delivered (re-entering past a
     * bare signal), or a VMS status when it could not be delivered (no
     * executive). Surface that status so sys$hiber fails honestly instead of
     * busy-looping when there is no /dev/vms. */
    st = kif_wait_call(VMS_IOCTL_HIBER, &args);
    if (st)
        return st;

    if (woken)
        *woken = (int)args.woken;
    return args.status;
}

uint32_t vms_kif_wake(uint32_t vms_pid)
{
    struct vms_wake_args args;

    vms_memset(&args, 0, sizeof(args));
    args.vms_pid = vms_pid;

    KIF_CALL(VMS_IOCTL_WAKE, &args);

    return args.status;
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

/*
 * vms_kif_get_resmaster - read a resource's DLM directory + mastering state
 * (vms-ci.5 DB). Issues VMS_IOCTL_GET_RESMASTER, the read-only diagnostic that
 * reports which node is the directory for the name, which node masters the
 * resource (0 = unmastered), whether this node is the master, and how many
 * locks are granted -- WITHOUT creating or mastering the resource. No sys$
 * service calls this; it exists so the kernel's local resource-directory +
 * mastering scaffolding is observable against a real /dev/vms (see the
 * OVMX-UNWIRED declaration in vms_kif.h and tests/qemu/test_kmod_resdir.c).
 */
uint32_t vms_kif_get_resmaster(const char *resnam, uint32_t *found,
                               uint32_t *local_csid, uint32_t *dir_csid,
                               uint32_t *master_csid, uint32_t *is_local_master,
                               uint32_t *n_granted,
                               uint32_t *remote_holder_csid)
{
    struct vms_resmaster_args args;

    if (!resnam)
        return 0x00000014; /* SS$_BADPARAM */

    vms_memset(&args, 0, sizeof(args));
    vms_strncpy(args.resnam, resnam, sizeof(args.resnam) - 1);
    args.resnam[sizeof(args.resnam) - 1] = '\0';

    KIF_CALL(VMS_IOCTL_GET_RESMASTER, &args);

    if (found) *found = args.found;
    if (local_csid) *local_csid = args.local_csid;
    if (dir_csid) *dir_csid = args.dir_csid;
    if (master_csid) *master_csid = args.master_csid;
    if (is_local_master) *is_local_master = args.is_local_master;
    if (n_granted) *n_granted = args.n_granted;
    if (remote_holder_csid) *remote_holder_csid = args.remote_holder_csid;

    return args.status;
}

/*
 * vms_kif_dlm_xnode - dispatch a decoded cross-node DLM request to the kernel
 * lock manager's cross-node handler (vms-94c, DLM epic vms-7fa rung 1). Issues
 * VMS_IOCTL_DLM_XNODE, which reaches vms_lock_dlm_xnode_dispatch(): rung 1 the
 * message arrives DECODED and the handler returns SS$_UNSUPPORTED (no fabricated
 * cross-node grant, INV-6). `op` is a VMS_DLM_OP_* value. Returns the executive
 * status; on a failed ioctl the honest translated status, never a fake grant.
 */
uint32_t vms_kif_dlm_xnode(uint32_t op, uint32_t lkmode, uint32_t flags,
                           uint32_t req_lkid, uint32_t master_lkid,
                           uint32_t req_csid, uint32_t master_csid,
                           const char *resnam, const uint8_t *valblk,
                           uint32_t *out_master_lkid, uint32_t *out_queued,
                           uint32_t *out_blocking_csid,
                           uint32_t *out_blocking_master_lkid,
                           uint32_t *out_req_lkid, uint32_t *out_lkmode)
{
    struct vms_dlm_xnode_args args;

    vms_memset(&args, 0, sizeof(args));
    args.op = op;
    args.lkmode = lkmode;
    args.flags = flags;
    args.req_lkid = req_lkid;
    args.master_lkid = master_lkid;
    args.req_csid = req_csid;
    args.master_csid = master_csid;
    if (resnam) {
        vms_strncpy(args.resnam, resnam, sizeof(args.resnam) - 1);
        args.resnam[sizeof(args.resnam) - 1] = '\0';
    }
    if (valblk)
        vms_memcpy(args.valblk, valblk, sizeof(args.valblk));

    KIF_CALL(VMS_IOCTL_DLM_XNODE, &args);

    /* Contention-rung outputs (vms-904c): the master's lock handle for the
     * granted-or-queued request, whether it was queued (blocked), and -- when it
     * blocked a cross-node holder -- the identity + lock handle that must receive
     * a BLKAST. All optional. */
    if (out_master_lkid) *out_master_lkid = args.master_lkid;
    if (out_queued) *out_queued = args.queued;
    if (out_blocking_csid) *out_blocking_csid = args.blocking_csid;
    if (out_blocking_master_lkid) *out_blocking_master_lkid = args.blocking_master_lkid;
    /* Deferred-GRANT readback (vms-6ca, H5): on a cross-node $DEQ that flipped a
     * queued cross-node waiter to granted, the executive names that waiter in the
     * fields a DEQ otherwise leaves 0 -- queued=1 (a waiter flipped), blocking_*
     * = the granted waiter's CSID + master handle, req_lkid = its requester
     * handle, lkmode = the mode it was granted at. The daemon reads these to wire
     * the deferred GRANT back to the requester. */
    if (out_req_lkid) *out_req_lkid = args.req_lkid;
    if (out_lkmode) *out_lkmode = args.lkmode;

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

uint32_t vms_kif_disk_resolve(const char *devnam, char *backing,
                              uint32_t backing_size,
                              uint32_t *major, uint32_t *minor)
{
    struct vms_diskresolve_args args;

    if (!devnam)
        return 0x00000014; /* SS$_BADPARAM */

    vms_memset(&args, 0, sizeof(args));
    vms_strncpy(args.devnam, devnam, VMS_DEVNAM_SIZE - 1);
    args.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    KIF_CALL(VMS_IOCTL_DISK_RESOLVE, &args);

    /* Copy the backing device out only on success (odd status); a failed
     * resolve must not hand the caller a half-filled backing string. */
    if (args.status & 1) {
        if (backing && backing_size) {
            vms_strncpy(backing, args.backing, backing_size - 1);
            backing[backing_size - 1] = '\0';
        }
        if (major)
            *major = args.backing_major;
        if (minor)
            *minor = args.backing_minor;
    }

    return args.status;
}

uint32_t vms_kif_getvol(const char *devnam, struct vms_getvol_args *out)
{
    struct vms_getvol_args args;

    if (!devnam)
        return 0x00000014; /* SS$_BADPARAM */

    vms_memset(&args, 0, sizeof(args));
    vms_strncpy(args.devnam, devnam, VMS_DEVNAM_SIZE - 1);
    args.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    KIF_CALL(VMS_IOCTL_GETVOL, &args);

    if (out)
        vms_memcpy(out, &args, sizeof(*out));

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

/*
 * vms_kif_establish_system - ask the executive to construct the SYSTEM
 * identity onto this process (vms-a17e).
 *
 * UNLIKE vms_kif_setident(), THIS TAKES NO IDENTITY ARGUMENTS. There is no
 * username, uic, or privilege mask to supply: SYSTEM/[1,4]/ALL are
 * constants the executive already owns (VMS_SYSTEM_UIC and
 * VMS_PRV_M_SYSTEM_ALL, src/kernel/vms_internal.h), constructed the same
 * way OPA0: is -- by the executive itself, from module init, not read out
 * of a file by whoever calls this. PROVISION.EXE (src/ovmx_provision/
 * ovmx_provision.c) is the one caller, and calling this is now the whole
 * of what it does to become SYSTEM: no SYSUAF lookup precedes it.
 *
 * SS$_NOPRIV if the caller lacks CAP_SYS_ADMIN (see
 * vms_ioctl_establish_system()'s comment in vms_ioctl.h for why that gate,
 * not SETPRV, is what this checks).
 */
uint32_t vms_kif_establish_system(void)
{
    struct vms_establish_system_args args;

    vms_memset(&args, 0, sizeof(args));

    KIF_CALL(VMS_IOCTL_ESTABLISH_SYSTEM, &args);

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
 * $EXIT / $STATUS + CLI invocation context (vms-f60d)
 *
 * The executive owns the image-completion status ($STATUS/$SEVERITY) and the
 * invoking CLI's command line, exactly as it owns the process name and
 * identity above. These four wrappers are the userspace face of the executive
 * facility the process-table half of PR #724 landed (VMS_IOCTL_SETEXIT..GETCLI):
 * the exiting image RECORDS its completion status (vms_kif_setexit) and the
 * invoking CLI READS it back (vms_kif_getexit); the CLI RECORDS its command
 * line (vms_kif_setcli) and the activated image READS it back (vms_kif_getcli).
 * INV-6: with no /dev/vms KIF_CALL returns the transport status (SS$_NOSUCHDEV)
 * and NOTHING is faked -- the caller then behaves as "no executive-recorded
 * status / no CLI line", never as a fabricated success.
 * ================================================================ */

/* $EXIT: record `condition` as this process's image-completion $STATUS in the
 * executive PCB. On success *exit_code (if given) receives the OVMX POSIX exit
 * code the executive mapped from the condition (0 iff STS$M_SUCCESS). */
uint32_t vms_kif_setexit(uint32_t condition, uint32_t *exit_code)
{
    struct vms_exit_args args;

    vms_memset(&args, 0, sizeof(args));
    args.condition = condition;

    KIF_CALL(VMS_IOCTL_SETEXIT, &args);

    if (exit_code)
        *exit_code = args.exit_code;
    return args.status;
}

/* Read this process's recorded image-completion $STATUS (the invoking CLI
 * reading the status of the image it just ran). *has_exited (if given) is set
 * nonzero iff an image has actually recorded a status -- a caller MUST NOT
 * infer "not exited" from a zero *condition, since 0 is a legal value. */
uint32_t vms_kif_getexit(uint32_t *condition, int *has_exited)
{
    struct vms_getexit_args args;

    vms_memset(&args, 0, sizeof(args));
    args.select = VMS_JPI_SEL_SELF;

    KIF_CALL(VMS_IOCTL_GETEXIT, &args);

    if (condition)
        *condition = args.condition;
    if (has_exited)
        *has_exited = (int)args.has_exited;
    return args.status;
}

/* Read the image-completion $STATUS of a process named by its backing Linux pid
 * (vms-707). This is how DCL's RUN recovers the true condition value of an image
 * it activated through the fork()+execve() fallback: the child shares DCL's VMS
 * PID (REGISTER_CONTINUE), so a by-VMS-PID read is ambiguous, but the child's
 * Linux pid -- which DCL holds from fork() -- names its PCB uniquely. Must be
 * called BEFORE the child is reaped (waitpid), or the row is gone. *has_exited
 * (if given) is nonzero iff an image actually recorded a status -- a foreign
 * tool that never calls $EXIT leaves it 0, and the caller then derives $STATUS
 * from the POSIX exit as before. INV-6: with no /dev/vms this returns the
 * transport status and records nothing. */
uint32_t vms_kif_getexit_linux(uint32_t linux_pid, uint32_t *condition,
                               int *has_exited)
{
    struct vms_getexit_args args;

    vms_memset(&args, 0, sizeof(args));
    args.select  = VMS_JPI_SEL_LINUX_PID;
    args.vms_pid = linux_pid;               /* field carries the Linux pid here */

    KIF_CALL(VMS_IOCTL_GETEXIT, &args);

    if (condition)
        *condition = args.condition;
    if (has_exited)
        *has_exited = (int)args.has_exited;
    return args.status;
}

/* Record this (CLI) process's invoking command line + cliflag in the executive,
 * so an image it activates reads the SAME context back (inherited from this
 * PCB at REGISTER_CONTINUE time). cliflag == 0 means "no CLI" and the command
 * is ignored. The command travels by length (it is not necessarily NUL-clean),
 * clipped to the executive's window. */
uint32_t vms_kif_setcli(uint32_t cliflag, const char *command)
{
    struct vms_setcli_args args;

    vms_memset(&args, 0, sizeof(args));
    args.cliflag = (uint8_t)(cliflag ? 1 : 0);

    if (args.cliflag && command) {
        vms_size_t len = vms_strlen(command);
        if (len > VMS_CLI_CMDLINE_SIZE - 1)
            len = VMS_CLI_CMDLINE_SIZE - 1;
        vms_memcpy(args.command, command, len);
        args.command[len] = '\0';
        args.length = (uint16_t)len;
    }

    KIF_CALL(VMS_IOCTL_SETCLI, &args);

    return args.status;
}

/* Read this process's own invoking CLI context. *cliflag (if given) is nonzero
 * iff a CLI launched this image; command/command_size receive the NUL-terminated
 * command line (clipped to command_size), and *length (if given) its true byte
 * length as the executive holds it. */
uint32_t vms_kif_getcli(uint32_t *cliflag, char *command,
                        uint32_t command_size, uint32_t *length)
{
    struct vms_getcli_args args;

    vms_memset(&args, 0, sizeof(args));

    KIF_CALL(VMS_IOCTL_GETCLI, &args);

    if (cliflag)
        *cliflag = args.cliflag;
    if (length)
        *length = args.length;
    if (command && command_size) {
        uint32_t n = args.length;
        if (n > command_size - 1)
            n = command_size - 1;
        vms_memcpy(command, args.command, n);
        command[n] = '\0';
    }
    return args.status;
}

/* ================================================================
 * P0 program region (vms-68f.i, in-process image activation foundation)
 * ================================================================ */

/*
 * Both calls below bind (open + register) and check the descriptor
 * DIRECTLY, inline, rather than through a shared static helper: this
 * facility has no read-only arena to mmap the way vms_kif_lnm_arena()
 * does, and a named helper not itself prefixed vms_kif_ would be a new,
 * unreachable entry in tests/integration/test_kif_caller_census.sh's
 * definition-side universe with no way to declare it unwired (the
 * escape hatch's own pattern requires the vms_kif_ prefix -- see that
 * script's OVMX-UNWIRED regex). Two lines duplicated twice is cheaper
 * than a helper the census cannot let through cleanly.
 */

uint32_t vms_kif_p0_map(uint64_t base, uint64_t limit)
{
    struct vms_p0_args args;

    if (base == 0 || limit <= base)
        return SS$_BADPARAM;

    kif_bind();
    if (vms_dev_fd < 0)
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.base = base;
    args.limit = limit;

    KIF_CALL(VMS_IOCTL_P0_MAP, &args);

    return args.status;
}

uint32_t vms_kif_p0_unmap(uint64_t *old_base, uint64_t *old_limit)
{
    struct vms_p0_args args;

    kif_bind();
    if (vms_dev_fd < 0)
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));

    KIF_CALL(VMS_IOCTL_P0_UNMAP, &args);

    if (args.status & 1) {
        if (old_base) *old_base = args.base;
        if (old_limit) *old_limit = args.limit;
    }
    return args.status;
}

uint32_t vms_kif_p1_map(uint64_t base, uint64_t limit)
{
    struct vms_p1_args args;

    if (base == 0 || limit <= base)
        return SS$_BADPARAM;

    kif_bind();
    if (vms_dev_fd < 0)
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.base = base;
    args.limit = limit;

    KIF_CALL(VMS_IOCTL_P1_MAP, &args);

    return args.status;
}

/* ================================================================
 * Access-mode transition primitive (vms-68f.iii, in-process image
 * activation foundation, increment (iii))
 *
 * Both calls bind and check the descriptor directly, inline, for the
 * same reason vms_kif_p0_map/p0_unmap/p1_map do above: no shared arena,
 * and a named helper without the vms_kif_ prefix would be an unreachable
 * entry in tests/integration/test_kif_caller_census.sh's universe.
 * ================================================================ */

uint32_t vms_kif_enter_image(uint8_t *prev_mode, uint8_t *new_mode)
{
    struct vms_modexfer_args args;

    kif_bind();
    if (vms_dev_fd < 0)
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));

    KIF_CALL(VMS_IOCTL_ENTER_IMAGE, &args);

    if (prev_mode) *prev_mode = args.prev_mode;
    if (new_mode) *new_mode = args.new_mode;
    return args.status;
}

uint32_t vms_kif_image_rundown(uint8_t *prev_mode, uint8_t *new_mode)
{
    struct vms_modexfer_args args;

    kif_bind();
    if (vms_dev_fd < 0)
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));

    KIF_CALL(VMS_IOCTL_IMAGE_RUNDOWN, &args);

    if (prev_mode) *prev_mode = args.prev_mode;
    if (new_mode) *new_mode = args.new_mode;
    return args.status;
}

/*
 * vms_kif_p1_protect - NOT an ioctl. See the header comment for why:
 * this wraps a real mprotect(2) on the caller's own address space, the
 * enforced half of the design's critical-P1 mechanism, and it has no
 * /dev/vms dependency at all.
 */
uint32_t vms_kif_p1_protect(uint64_t base, uint64_t limit, int writable)
{
    int prot;

    if (base == 0 || limit <= base)
        return SS$_BADPARAM;

    prot = writable ? (VMS_PROT_READ | VMS_PROT_WRITE) : VMS_PROT_READ;

    if (vms_sys_mprotect((void *)(unsigned long)base,
                          (vms_size_t)(limit - base), prot) != 0)
        return SS$_ACCVIO;

    return SS$_NORMAL;
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
 * This process's GROUP/JOB scope keys (vms-aba), cached the same way as
 * the arena mapping above and for the same reason: fetching them is one
 * ioctl (VMS_IOCTL_LNM_GETSCOPE), and paying it once per process keeps the
 * translate hot path free of syscalls after the first call, exactly as the
 * arena mmap does. NEITHER key may be recomputed locally from raw Linux
 * credentials -- SETIDENT can set a UIC that does not match this task's
 * uid/gid (see vms_ioctl_setident()), and JOB has no userspace
 * representation at all -- so this cache is filled from the executive,
 * never derived here.
 */
static __thread uint32_t vms_lnm_scope_group = 0;
static __thread uint32_t vms_lnm_scope_job = 0;
static __thread int vms_lnm_scope_valid = 0;
static __thread vms_pid_t vms_lnm_scope_pid = 0;

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
        kif_xport_munmap(vms_lnm_map, sizeof(struct vms_lnm_arena));
        vms_lnm_map = 0;
        vms_lnm_map_pid = 0;
    }

    kif_bind();
    if (vms_dev_fd < 0)
        return 0;

    p = kif_xport_mmap(vms_dev_fd, sizeof(struct vms_lnm_arena),
                       VMS_LNM_MMAP_OFFSET);
    if (!p)
        return 0;

    /* Trust the mapping only if the executive's header validates. */
    {
        struct vms_lnm_arena *a = (struct vms_lnm_arena *)p;
        if (a->magic != VMS_LNM_ARENA_MAGIC ||
            a->version != VMS_LNM_ARENA_VERSION) {
            kif_xport_munmap(p, sizeof(struct vms_lnm_arena));
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

/*
 * vms_kif_lnm_myscope - fetch (once per process) this caller's own GROUP
 * and JOB scope keys via VMS_IOCTL_LNM_GETSCOPE.
 *
 * Cached exactly like vms_kif_lnm_arena()'s mapping, and invalidated the
 * same way: a fork()'d child inherits the parent's TLS cache by value, but
 * the executive keys scope by PROCESS (proc->job_id is set once at THIS
 * process's own registration, vms_module.c), so a cached value under a
 * different pid is stale and must be refetched.
 *
 * Returns 1 and fills *group_key / *job_key on success, 0 when the executive
 * is unavailable (the caller treats that the same as "name not found" --
 * vms_kif_lnm_arena() already failed the mmap bind in that case, so a
 * GROUP/JOB translate never reaches here with no executive).
 *
 * Calls kif_call() DIRECTLY rather than through the KIF_CALL macro: that
 * macro does `return _kif_st;` on a nonzero (dispatch-level failure) status,
 * which is the right shape for the many vms_kif_* wrappers that themselves
 * return a raw uint32_t SS$ status -- but this function returns an int
 * success flag through OUT parameters, and a bare `return` of a nonzero
 * SS$ code from inside it would come back to the caller as a truthy "1",
 * with *group_key / *job_key left unset. kif_bind() still runs (kif_call()
 * calls it internally), so there is no separate bind step needed here.
 */
static int vms_kif_lnm_myscope(uint32_t *group_key, uint32_t *job_key)
{
    struct vms_lnm_scope_args args;
    vms_pid_t pid = vms_sys_getpid();
    uint32_t kst;

    if (vms_lnm_scope_valid && vms_lnm_scope_pid == pid) {
        *group_key = vms_lnm_scope_group;
        *job_key = vms_lnm_scope_job;
        return 1;
    }

    vms_memset(&args, 0, sizeof(args));
    kst = kif_call(VMS_IOCTL_LNM_GETSCOPE, &args);
    if (kst != 0) {
        /* Dispatch-level failure (unreachable executive, unregistered
         * process): report it, no in-process substitute. */
        return 0;
    }

    /* VMS odd/even success convention, checked directly: this is
     * libvmssys, the freestanding base layer, and does not include
     * libvms/ssdef.h's $VMS_STATUS_SUCCESS (that would invert the library
     * dependency graph in CLAUDE.md). */
    if (!(args.status & 1u))
        return 0;

    vms_lnm_scope_group = args.group_key;
    vms_lnm_scope_job = args.job_key;
    vms_lnm_scope_valid = 1;
    vms_lnm_scope_pid = pid;

    *group_key = vms_lnm_scope_group;
    *job_key = vms_lnm_scope_job;
    return 1;
}

int vms_kif_lnm_translate(uint32_t table, const char *name, uint8_t index,
                          char *value, uint32_t valsz,
                          uint16_t *vallen, uint32_t *attrs,
                          uint8_t *num_equiv)
{
    struct vms_lnm_arena *a;
    uint32_t scope_key;
    uint32_t group_key, job_key;
    int tries;

    if (num_equiv)
        *num_equiv = 0;

    if (!name || !value || valsz == 0)
        return -1;

    a = vms_kif_lnm_arena();
    if (!a)
        return -1;   /* executive absent -> caller renders SS$_NOSUCHDEV */

    /* SYSTEM is singular (scope 0). GROUP/JOB are this caller's own
     * executive-derived scope keys (vms-aba) -- never recomputed locally
     * (see vms_kif_lnm_myscope()). */
    switch (table) {
    case VMS_LNM_TBL_GROUP:
    case VMS_LNM_TBL_JOB:
        if (!vms_kif_lnm_myscope(&group_key, &job_key))
            return -1;   /* executive absent -> SS$_NOSUCHDEV */
        scope_key = (table == VMS_LNM_TBL_GROUP) ? group_key : job_key;
        break;
    case VMS_LNM_TBL_SYSTEM:
    default:
        scope_key = 0u;
        break;
    }

    /*
     * Seqlock read: sample the generation, walk the table, re-sample. Retry
     * on an odd (write in flight) or changed counter. Writes are rare and
     * brief, so this effectively never spins; the bound keeps a pathological
     * writer from wedging a reader.
     */
    for (tries = 0; tries < 1024; tries++) {
        uint64_t g0 = lnm_gen_load(a);
        uint32_t i, max;
        int name_found = 0;   /* the logical name itself exists in-scope */
        int idx_found = 0;    /* AND `index` is one of its equivalence strings */
        char vbuf[VMS_LNM_MAX_VALUE + 1];
        uint16_t vlen = 0;
        uint32_t vattr = 0;
        uint8_t nequiv = 0;

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

            name_found = 1;
            nequiv = e->num_equiv;
            vattr = e->attributes;

            if (index < e->num_equiv) {
                vlen = e->equiv[index].length;
                if (vlen > VMS_LNM_MAX_VALUE)
                    vlen = VMS_LNM_MAX_VALUE;
                vms_memcpy(vbuf, e->equiv[index].value, vlen);
                vbuf[vlen] = '\0';
                idx_found = 1;
            }
            break;
        }

        /* Re-sample: if the arena changed under us, the read is not stable. */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (lnm_gen_load(a) != g0)
            continue;

        if (num_equiv)
            *num_equiv = name_found ? nequiv : 0;

        if (!idx_found)
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

/* The public enumerate record tracks the arena's name/value/equiv widths. If
 * the arena ABI ever changes width this fails at compile time rather than
 * silently truncating a listed name, value, or search-list entry. */
_Static_assert(sizeof(((struct vms_kif_lnm_enum_rec *)0)->name)
               == VMS_LNM_MAX_NAME + 1,
               "vms_kif_lnm_enum_rec.name width != arena name width");
_Static_assert(sizeof(((struct vms_kif_lnm_enum_rec *)0)->values[0])
               == VMS_LNM_MAX_VALUE + 1,
               "vms_kif_lnm_enum_rec.values width != arena value width");
_Static_assert(sizeof(((struct vms_kif_lnm_enum_rec *)0)->values)
               / sizeof(((struct vms_kif_lnm_enum_rec *)0)->values[0])
               == VMS_LNM_MAX_EQUIV,
               "vms_kif_lnm_enum_rec.values count != arena equiv count");

int vms_kif_lnm_enumerate(uint32_t table,
                          struct vms_kif_lnm_enum_rec *out, uint32_t max_out)
{
    struct vms_lnm_arena *a;
    uint32_t scope_key;
    uint32_t group_key, job_key;
    int tries;

    if (!out || max_out == 0)
        return -1;

    a = vms_kif_lnm_arena();
    if (!a)
        return -1;   /* executive absent -> caller renders SS$_NOSUCHDEV */

    /* Same scope resolution as vms_kif_lnm_translate: SYSTEM is singular
     * (scope 0); GROUP/JOB use this caller's own executive-derived keys,
     * never recomputed here. */
    switch (table) {
    case VMS_LNM_TBL_GROUP:
    case VMS_LNM_TBL_JOB:
        if (!vms_kif_lnm_myscope(&group_key, &job_key))
            return -1;
        scope_key = (table == VMS_LNM_TBL_GROUP) ? group_key : job_key;
        break;
    case VMS_LNM_TBL_SYSTEM:
    default:
        scope_key = 0u;
        break;
    }

    /*
     * Seqlock read, exactly as the translate path: sample the generation,
     * walk the table collecting matches into out[], re-sample. A retry
     * overwrites out[] from the start (idempotent), so a torn snapshot is
     * never returned. The callback is invoked by the caller AFTER this
     * returns a stable snapshot -- never mid-walk, so a retry cannot
     * double-deliver an entry.
     */
    for (tries = 0; tries < 1024; tries++) {
        uint64_t g0 = lnm_gen_load(a);
        uint32_t i, max;
        uint32_t n = 0;

        if (g0 & 1ULL)
            continue;   /* write in flight */

        max = a->max_entries;
        if (max > VMS_LNM_MAX_ENTRIES)
            max = VMS_LNM_MAX_ENTRIES;

        for (i = 0; i < max && n < max_out; i++) {
            const struct vms_lnm_entry *e = &a->entries[i];
            uint8_t nv, k;

            if (!e->in_use || e->table != table || e->scope_key != scope_key)
                continue;
            if (e->num_equiv == 0)
                continue;

            vms_strncpy(out[n].name, e->name, VMS_LNM_MAX_NAME);
            out[n].name[VMS_LNM_MAX_NAME] = '\0';

            nv = e->num_equiv;
            if (nv > VMS_LNM_MAX_EQUIV)
                nv = VMS_LNM_MAX_EQUIV;
            for (k = 0; k < nv; k++) {
                uint16_t vlen = e->equiv[k].length;
                if (vlen > VMS_LNM_MAX_VALUE)
                    vlen = VMS_LNM_MAX_VALUE;
                vms_memcpy(out[n].values[k], e->equiv[k].value, vlen);
                out[n].values[k][vlen] = '\0';
            }
            out[n].num_values = nv;

            out[n].attributes = e->attributes;
            n++;
        }

        /* Re-sample: if the arena changed under us, the snapshot is not
         * stable -- retry the whole walk. */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (lnm_gen_load(a) != g0)
            continue;

        return (int)n;
    }

    /* Writer never quiesced -- treat as unavailable rather than guess. */
    return -1;
}

/* ================================================================
 * Mailboxes (executive-resident MBAn:, vms-d44)
 * ================================================================ */

/*
 * mbx_bind_ok - bind (open + register) and report whether /dev/vms is
 * actually reachable, so a mailbox call can fail honestly with
 * SS$_NOSUCHDEV rather than let a bad-fd ioctl fall through kif_call()'s
 * generic errno mapping to SS$_BUGCHECK (CLAUDE.md Rule 9 / INV-6). Same
 * technique as vms_kif_lnm_arena()'s mmap probe, without an mmap step of
 * its own: mailboxes have no read-only arena to map, so this checks the
 * bound descriptor directly.
 */
static int mbx_bind_ok(void)
{
    kif_bind();
    return vms_dev_fd >= 0;
}

uint32_t vms_kif_mbx_create(int permanent, uint32_t maxmsg, uint32_t bufquo,
                            uint32_t *exec_chan, uint32_t *unit,
                            char *devnam, uint32_t devnam_sz)
{
    struct vms_mbx_create_args args;

    if (!mbx_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.permanent = permanent ? 1 : 0;
    args.maxmsg = maxmsg;
    args.bufquo = bufquo;

    KIF_CALL(VMS_IOCTL_MBX_CREATE, &args);

    if (args.status & 1) {
        if (exec_chan) *exec_chan = args.chan;
        if (unit) *unit = args.unit;
        if (devnam && devnam_sz > 0) {
            vms_strncpy(devnam, args.devnam, devnam_sz - 1);
            devnam[devnam_sz - 1] = '\0';
        }
    }
    return args.status;
}

uint32_t vms_kif_mbx_assign(const char *devnam, uint32_t *exec_chan)
{
    struct vms_mbx_assign_args args;

    if (!devnam || !exec_chan)
        return SS$_BADPARAM;
    if (!mbx_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    vms_strncpy(args.devnam, devnam, sizeof(args.devnam) - 1);
    args.devnam[sizeof(args.devnam) - 1] = '\0';

    KIF_CALL(VMS_IOCTL_MBX_ASSIGN, &args);

    if (args.status & 1) *exec_chan = args.chan;
    return args.status;
}

uint32_t vms_kif_mbx_delmbx(uint32_t exec_chan)
{
    struct vms_mbx_delmbx_args args;

    if (!mbx_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;

    KIF_CALL(VMS_IOCTL_MBX_DELMBX, &args);

    return args.status;
}

uint32_t vms_kif_mbx_write(uint32_t exec_chan, const void *buf, uint32_t len)
{
    struct vms_mbx_write_args args;

    if (!buf)
        return SS$_BADPARAM;
    if (len > VMS_MBX_IOCTL_MAXLEN)
        return SS$_EXQUOTA;
    if (!mbx_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.len = len;
    vms_memcpy(args.data, buf, len);

    KIF_CALL(VMS_IOCTL_MBX_WRITE, &args);

    return args.status;
}

/*
 * vms_kif_mbx_read - read one message from a mailbox.
 *
 * BLOCKING (nowait == 0): waits until a message is queued. Uses
 * kif_wait_call(), the same EINTR-retry helper $WAITFR/$WFLOR/$WFLAND use:
 * the executive's VMS_IOCTL_MBX_READ handler returns -ERESTARTSYS with no
 * status on a signal (see vms_mbx.c), exactly like a VMS mailbox read,
 * which has no "the wait was interrupted" condition value to report.
 *
 * NON-BLOCKING (nowait != 0, the $QIO IO$M_NOW modifier): the executive
 * completes the read at once (VMS_MBX_READ_NOW) -- if a message is queued it
 * is returned, otherwise the read completes with SS$_ENDOFFILE and does not
 * block. That path never returns -ERESTARTSYS (there is no wait to interrupt),
 * so the kif_wait_call() retry loop degenerates to a single ioctl round trip.
 */
uint32_t vms_kif_mbx_read(uint32_t exec_chan, void *buf, uint32_t bufsz,
                          uint32_t *actlen, int nowait)
{
    struct vms_mbx_read_args args;
    uint32_t n;

    if (!buf)
        return SS$_BADPARAM;
    if (!mbx_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.bufsz = (bufsz > VMS_MBX_IOCTL_MAXLEN) ? VMS_MBX_IOCTL_MAXLEN : bufsz;
    args.flags = nowait ? VMS_MBX_READ_NOW : 0u;

    KIF_WAIT_CALL(VMS_IOCTL_MBX_READ, &args);

    if (args.status & 1) {
        n = args.len;
        if (n > args.bufsz) n = args.bufsz;
        vms_memcpy(buf, args.data, n);
        if (actlen) *actlen = args.len;
    }
    return args.status;
}

/*
 * vms_kif_mbx_set_wrtattn - arm a write-attention AST on a mailbox channel
 * (vms-9003, IO$_SETMODE|IO$M_WRTATTN). When another process writes a message
 * to the mailbox, the executive queues astadr(astprm) into THIS process's AST
 * queue at `acmode`; the process drains it through $SETAST/DELIVERAST, exactly
 * as $DCLAST-declared ASTs are drained. One-shot -- re-arm with another call.
 */
uint32_t vms_kif_mbx_set_wrtattn(uint32_t exec_chan, uint8_t acmode,
                                 uint64_t astadr, uint64_t astprm)
{
    struct vms_mbx_wrtattn_args args;

    if (!mbx_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.acmode = acmode;
    args.astadr = astadr;
    args.astprm = astprm;

    KIF_CALL(VMS_IOCTL_MBX_SET_WRTATTN, &args);

    return args.status;
}

/* ================================================================
 * INET pseudo-device BGn: (vms-527)
 *
 * Thin marshalling wrappers over the VMS_IOCTL_BG_* driver in vms.ko
 * (src/kernel/vms_bg.c), the exact analogue of the vms_kif_mbx_* wrappers
 * above: bind to /dev/vms (honest SS$_NOSUCHDEV if the executive is absent --
 * INV-6, never a userspace socket fallback), issue one ioctl, hand back the
 * driver's own status. There is NO userspace socket here; the socket lives in
 * the executive.
 * ================================================================ */

/* Same bind-and-check as mbx_bind_ok(): SS$_NOSUCHDEV when /dev/vms is absent
 * is the honest state for an executive device whose driver is unreachable. */
static int bg_bind_ok(void)
{
    kif_bind();
    return vms_dev_fd >= 0;
}

uint32_t vms_kif_bg_create(uint32_t *exec_chan, uint32_t *unit,
                           char *devnam, uint32_t devnam_sz)
{
    struct vms_bg_create_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));

    KIF_CALL(VMS_IOCTL_BG_CREATE, &args);

    if (args.status & 1) {
        if (exec_chan) *exec_chan = args.chan;
        if (unit) *unit = args.unit;
        if (devnam && devnam_sz > 0) {
            vms_strncpy(devnam, args.devnam, devnam_sz - 1);
            devnam[devnam_sz - 1] = '\0';
        }
    }
    return args.status;
}

uint32_t vms_kif_bg_setmode(uint32_t exec_chan)
{
    struct vms_bg_chanonly_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;

    KIF_CALL(VMS_IOCTL_BG_SETMODE, &args);

    return args.status;
}

uint32_t vms_kif_bg_connect(uint32_t exec_chan, uint16_t family,
                            uint16_t port, uint32_t addr)
{
    struct vms_bg_connect_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.sin_family = family;
    args.sin_port = port;   /* network byte order, caller's responsibility */
    args.sin_addr = addr;   /* network byte order */

    KIF_CALL(VMS_IOCTL_BG_CONNECT, &args);

    return args.status;
}

uint32_t vms_kif_bg_send(uint32_t exec_chan, const void *buf, uint32_t len,
                         uint32_t *actlen)
{
    struct vms_bg_io_args args;

    if (!buf)
        return SS$_BADPARAM;
    if (len > VMS_BG_IOCTL_MAXLEN)
        return SS$_BADPARAM;
    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.len = len;
    vms_memcpy(args.data, buf, len);

    KIF_CALL(VMS_IOCTL_BG_SEND, &args);

    if ((args.status & 1) && actlen) *actlen = args.len;
    return args.status;
}

uint32_t vms_kif_bg_recv(uint32_t exec_chan, void *buf, uint32_t bufsz,
                         uint32_t *actlen)
{
    struct vms_bg_io_args args;
    uint32_t n;

    if (!buf)
        return SS$_BADPARAM;
    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.len = (bufsz > VMS_BG_IOCTL_MAXLEN) ? VMS_BG_IOCTL_MAXLEN : bufsz;

    KIF_CALL(VMS_IOCTL_BG_RECV, &args);

    if (args.status & 1) {
        n = args.len;
        if (n > bufsz) n = bufsz;
        if (n > VMS_BG_IOCTL_MAXLEN) n = VMS_BG_IOCTL_MAXLEN;
        vms_memcpy(buf, args.data, n);
        if (actlen) *actlen = args.len;
    }
    return args.status;
}

uint32_t vms_kif_bg_deaccess(uint32_t exec_chan)
{
    struct vms_bg_chanonly_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;

    KIF_CALL(VMS_IOCTL_BG_DEACCESS, &args);

    return args.status;
}

uint32_t vms_kif_bg_dassgn(uint32_t exec_chan)
{
    struct vms_bg_chanonly_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;

    KIF_CALL(VMS_IOCTL_BG_DASSGN, &args);

    return args.status;
}

uint32_t vms_kif_bg_pollfd(uint32_t exec_chan, int *out_fd)
{
    struct vms_bg_pollfd_args args;

    if (out_fd)
        *out_fd = -1;
    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.fd = -1;

    KIF_CALL(VMS_IOCTL_BG_POLLFD, &args);

    if ((args.status & 1) && out_fd)
        *out_fd = args.fd;
    return args.status;
}

/*
 * Materialize the channel's executive socket as a REAL, data-carrying fd (RUNG-3b).
 * Unlike vms_kif_bg_pollfd's readiness-only fd, the fd returned here has real
 * read/write that route to the executive socket, and NO O_CLOEXEC so it survives
 * execve -- the primitive the --wrap dup2/dup uses to hand a BGn: connection to a
 * ported daemon's exec'd child. Fail-honest: no /dev/vms -> SS$_NOSUCHDEV; bad
 * channel / no socket -> SS$_IVCHAN (from the executive). Never a fabricated fd.
 */
uint32_t vms_kif_bg_materialize_fd(uint32_t exec_chan, int *out_fd)
{
    struct vms_bg_datafd_args args;

    if (out_fd)
        *out_fd = -1;
    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.fd = -1;

    KIF_CALL(VMS_IOCTL_BG_MATERIALIZE_FD, &args);

    if ((args.status & 1) && out_fd)
        *out_fd = args.fd;
    return args.status;
}

uint32_t vms_kif_bg_getname(uint32_t exec_chan, uint32_t which,
                            uint16_t *family, uint16_t *port, uint32_t *addr)
{
    struct vms_bg_name_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.which = which;

    KIF_CALL(VMS_IOCTL_BG_GETNAME, &args);

    if (args.status & 1) {
        if (family) *family = args.sin_family;
        if (port)   *port   = args.sin_port;
        if (addr)   *addr   = args.sin_addr;
    }
    return args.status;
}

/* Server path (vms-698). bind reads the effective local address back (ephemeral
 * port); accept installs the connection onto a second, pre-$ASSIGNed BG channel
 * (accept_exec_chan) and returns the peer address. /dev/vms absent -> SS$_NOSUCHDEV. */
uint32_t vms_kif_bg_bind(uint32_t exec_chan, uint16_t family, uint16_t port,
                         uint32_t addr, uint16_t *out_port, uint32_t *out_addr)
{
    struct vms_bg_bind_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.sin_family = family;
    args.sin_port = port;   /* network byte order (0 = ephemeral) */
    args.sin_addr = addr;   /* network byte order (0 = INADDR_ANY) */

    KIF_CALL(VMS_IOCTL_BG_BIND, &args);

    if (args.status & 1) {  /* effective local address read back */
        if (out_port) *out_port = args.sin_port;
        if (out_addr) *out_addr = args.sin_addr;
    }
    return args.status;
}

uint32_t vms_kif_bg_listen(uint32_t exec_chan, int backlog)
{
    struct vms_bg_listen_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.backlog = backlog;

    KIF_CALL(VMS_IOCTL_BG_LISTEN, &args);

    return args.status;
}

uint32_t vms_kif_bg_accept(uint32_t listen_exec_chan, uint32_t accept_exec_chan,
                           uint16_t *family, uint16_t *port, uint32_t *addr)
{
    struct vms_bg_accept_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.listen_chan = listen_exec_chan;
    args.accept_chan = accept_exec_chan;

    KIF_CALL(VMS_IOCTL_BG_ACCEPT, &args);

    if (args.status & 1) {  /* peer address */
        if (family) *family = args.sin_family;
        if (port)   *port   = args.sin_port;
        if (addr)   *addr   = args.sin_addr;
    }
    return args.status;
}

uint32_t vms_kif_bg_setsockopt(uint32_t exec_chan, int level, int optname,
                               int optval)
{
    struct vms_bg_sockopt_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.op = 0;                        /* set */
    args.level = level;
    args.optname = optname;
    args.optval = optval;

    KIF_CALL(VMS_IOCTL_BG_SOCKOPT, &args);

    return args.status;
}

uint32_t vms_kif_bg_getsockopt(uint32_t exec_chan, int level, int optname,
                               int *out_optval)
{
    struct vms_bg_sockopt_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.op = 1;                        /* get */
    args.level = level;
    args.optname = optname;

    KIF_CALL(VMS_IOCTL_BG_SOCKOPT, &args);

    if ((args.status & 1) && out_optval)
        *out_optval = args.optval;
    return args.status;
}

/* ================================================================
 * Files-11 (ODS-2) ACP -- channel + mount front-end (vms-149, epic vms-208)
 *
 * See vms_kif.h for the residency story and the no-fallback (INV-6) contract.
 * ================================================================ */

/*
 * acp_bind_ok - bind (open + register) and report whether /dev/vms is
 * reachable, so an ACP call fails honestly with SS$_NOSUCHDEV rather than let a
 * bad-fd ioctl fall through kif_call()'s generic errno mapping (CLAUDE.md
 * Rule 9 / INV-6). Same technique as mbx_bind_ok().
 */
static int acp_bind_ok(void)
{
    kif_bind();
    return vms_dev_fd >= 0;
}

uint32_t vms_kif_acp_mount(const char *devnam)
{
    struct vms_acp_mount_args args;

    if (!devnam)
        return SS$_BADPARAM;
    if (!acp_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    vms_strncpy(args.devnam, devnam, sizeof(args.devnam) - 1);
    args.devnam[sizeof(args.devnam) - 1] = '\0';

    KIF_CALL(VMS_IOCTL_ACP_MOUNT, &args);

    return args.status;
}

uint32_t vms_kif_acp_dmount(const char *devnam)
{
    struct vms_acp_dmount_args args;

    if (!devnam)
        return SS$_BADPARAM;
    if (!acp_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    vms_strncpy(args.devnam, devnam, sizeof(args.devnam) - 1);
    args.devnam[sizeof(args.devnam) - 1] = '\0';

    KIF_CALL(VMS_IOCTL_ACP_DMOUNT, &args);

    return args.status;
}

uint32_t vms_kif_acp_assign(const char *devnam, uint32_t *exec_chan)
{
    struct vms_acp_assign_args args;

    if (!devnam || !exec_chan)
        return SS$_BADPARAM;
    if (!acp_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    vms_strncpy(args.devnam, devnam, sizeof(args.devnam) - 1);
    args.devnam[sizeof(args.devnam) - 1] = '\0';

    KIF_CALL(VMS_IOCTL_ACP_ASSIGN, &args);

    if (args.status & 1)
        *exec_chan = args.chan;
    return args.status;
}

uint32_t vms_kif_acp_access(struct vms_acp_access_args *args)
{
    if (!args)
        return SS$_BADPARAM;
    if (!acp_bind_ok())
        return SS$_NOSUCHDEV;

    args->name[VMS_ACP_NAME_SIZE - 1] = '\0';
    args->status = 0;

    KIF_CALL(VMS_IOCTL_ACP_ACCESS, args);

    return args->status;
}

uint32_t vms_kif_acp_deaccess(uint32_t chan)
{
    struct vms_acp_deaccess_args args;

    if (!acp_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = chan;

    KIF_CALL(VMS_IOCTL_ACP_DEACCESS, &args);

    return args.status;
}

uint32_t vms_kif_acp_readvb(struct vms_acp_rw_args *args)
{
    if (!args)
        return SS$_BADPARAM;
    if (!acp_bind_ok())
        return SS$_NOSUCHDEV;

    args->status = 0;
    KIF_CALL(VMS_IOCTL_ACP_READVBLK, args);
    return args->status;
}

uint32_t vms_kif_acp_writevb(struct vms_acp_rw_args *args)
{
    if (!args)
        return SS$_BADPARAM;
    if (!acp_bind_ok())
        return SS$_NOSUCHDEV;

    args->status = 0;
    KIF_CALL(VMS_IOCTL_ACP_WRITEVBLK, args);
    return args->status;
}

uint32_t vms_kif_acp_acpcontrol(struct vms_acp_acpcontrol_args *args)
{
    if (!args)
        return SS$_BADPARAM;
    if (!acp_bind_ok())
        return SS$_NOSUCHDEV;

    args->pattern[VMS_ACP_NAME_SIZE - 1] = '\0';
    args->status = 0;

    KIF_CALL(VMS_IOCTL_ACP_ACPCONTROL, args);

    return args->status;
}

uint32_t vms_kif_acp_fileop(struct vms_acp_fileop_args *args)
{
    if (!args)
        return SS$_BADPARAM;
    if (!acp_bind_ok())
        return SS$_NOSUCHDEV;

    args->name[VMS_ACP_NAME_SIZE - 1] = '\0';
    args->status = 0;

    KIF_CALL(VMS_IOCTL_ACP_FILEOP, args);

    return args->status;
}
