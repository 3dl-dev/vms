/*
 * vms_kif.c - Kernel Interface userspace implementation
 *
 * Wraps /dev/vms ioctl calls behind VMS-style function APIs.
 * Uses the libvmssys syscall wrappers (no glibc).
 */

#include "vms_kif.h"
#include "vms_syscall.h"
#include "vms_string.h"

/* File descriptor for /dev/vms — thread-local so each thread opens/closes
 * independently.  Callers must invoke vms_kif_open() before using any KIF
 * function and vms_kif_close() when done, once per thread. */
static __thread int vms_dev_fd = -1;

/* ================================================================
 * Connection management
 * ================================================================ */

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
}

uint32_t vms_kif_register(uint32_t vms_pid)
{
    struct vms_register_args args;

    /* No "executive absent" check. The executive is INTEGRAL: PID 1 refuses
     * to bring the system up unless /dev/vms is open, and holds it open for
     * the life of the system (src/ovmx_init/ovmx_init.c, executive_attach),
     * so no caller can observe its absence. This guard used to report
     * SS$_BADPARAM for that impossible case, which was doubly wrong -- it
     * described an unreachable state, and it did so with a status that means
     * something else entirely. See CLAUDE.md Rule 9. */

    /* NO privilege argument (vms-2b8). The executive derives the
     * authorized mask from this task's real credentials; there is
     * nothing for the caller to ask for, and so nothing to forge. */
    vms_memset(&args, 0, sizeof(args));
    args.vms_pid = vms_pid;

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_REGISTER, (unsigned long)&args) < 0)
        return 0x00000014;

    return args.status;
}

/* ================================================================
 * Access Mode (3a)
 * ================================================================ */

uint32_t vms_kif_setmode(uint8_t mode)
{
    struct vms_mode_args args;

    vms_memset(&args, 0, sizeof(args));
    args.mode = mode;

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_SETMODE, (unsigned long)&args) < 0)
        return 0x00000014;

    return args.status;
}

uint32_t vms_kif_getmode(uint8_t *mode, uint64_t *cur_privs, uint64_t *perm_privs)
{
    struct vms_getmode_args args;

    vms_memset(&args, 0, sizeof(args));

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_GETMODE, (unsigned long)&args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_SETPRV, (unsigned long)&args) < 0)
        return 0x00000014;

    if (prev) *prev = args.prev;
    return args.status;
}

uint32_t vms_kif_chkpriv(uint64_t mask)
{
    struct vms_priv_args args;

    vms_memset(&args, 0, sizeof(args));
    args.mask = mask;

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_CHKPRIV, (unsigned long)&args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_DCLAST, (unsigned long)&args) < 0)
        return 0x00000014;

    return args.status;
}

uint32_t vms_kif_setast(int enable)
{
    struct vms_setast_args args;

    vms_memset(&args, 0, sizeof(args));
    args.enable = enable ? 1 : 0;

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_SETAST, (unsigned long)&args) < 0)
        return 0x00000014;

    return args.status;
}

int vms_kif_deliverast(uint64_t *astadr, uint64_t *astprm, uint8_t *acmode)
{
    struct vms_ast_args args;

    vms_memset(&args, 0, sizeof(args));

    /* VMS_IOCTL_DELIVERAST is _IOR: pass a pointer to the buffer the kernel
     * fills with the next pending AST. Returns 0 with the buffer populated
     * when an AST is delivered, or <0 (kernel -EAGAIN) when none is pending. */
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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_SETEF, (unsigned long)&args) < 0)
        return 0x00000014;

    return args.status;
}

uint32_t vms_kif_clref(uint32_t efn)
{
    struct vms_ef_args args;

    vms_memset(&args, 0, sizeof(args));
    args.efn = efn;

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_CLREF, (unsigned long)&args) < 0)
        return 0x00000014;

    return args.status;
}

uint32_t vms_kif_waitfr(uint32_t efn)
{
    struct vms_ef_args args;

    vms_memset(&args, 0, sizeof(args));
    args.efn = efn;

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_WAITFR, (unsigned long)&args) < 0)
        return 0x00000014;

    return args.status;
}

uint32_t vms_kif_wflor(uint32_t efn, uint32_t mask)
{
    struct vms_ef_wait_args args;

    vms_memset(&args, 0, sizeof(args));
    args.efn = efn;
    args.mask = mask;

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_WFLOR, (unsigned long)&args) < 0)
        return 0x00000014;

    return args.status;
}

uint32_t vms_kif_wfland(uint32_t efn, uint32_t mask)
{
    struct vms_ef_wait_args args;

    vms_memset(&args, 0, sizeof(args));
    args.efn = efn;
    args.mask = mask;

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_WFLAND, (unsigned long)&args) < 0)
        return 0x00000014;

    return args.status;
}

uint32_t vms_kif_readef(uint32_t efn, uint32_t *state)
{
    struct vms_ef_read_args args;

    vms_memset(&args, 0, sizeof(args));
    args.efn = efn;

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_READEF, (unsigned long)&args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_ASCEFC, (unsigned long)&args) < 0)
        return 0x00000014;

    return args.status;
}

uint32_t vms_kif_dacefc(uint32_t efn)
{
    struct vms_ef_args args;

    vms_memset(&args, 0, sizeof(args));
    args.efn = efn;

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_DACEFC, (unsigned long)&args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_ENQ, (unsigned long)&args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_DEQ, (unsigned long)&args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_CONVERT, (unsigned long)&args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_GETLKI, (unsigned long)&args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_ASSIGN, (unsigned long)&args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, req, (unsigned long)&args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_DASSGN, (unsigned long)&args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_GETDVI, (unsigned long)&args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_GETDVI, (unsigned long)&args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_DEVSCAN, (unsigned long)&args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_TTSETMODE, (unsigned long)&args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_SETPRN, (unsigned long)&args) < 0)
        return 0x00000014;

    return args.status;
}

/*
 * getjpi_common - issue one VMS_IOCTL_GETJPI with a prepared selector.
 */
static uint32_t getjpi_common(struct vms_getjpi_args *args,
                              struct vms_procinfo *info)
{
    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_GETJPI, (unsigned long)args) < 0)
        return 0x00000014;

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

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_SETIDENT, (unsigned long)&args) < 0)
        return 0x00000014;

    return args.status;
}

uint32_t vms_kif_procscan(uint32_t *index, struct vms_procinfo *info)
{
    struct vms_procscan_args args;

    if (!index)
        return 0x00000014; /* SS$_BADPARAM */

    vms_memset(&args, 0, sizeof(args));
    args.index = *index;

    if (vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_PROCSCAN, (unsigned long)&args) < 0)
        return 0x00000014;

    *index = args.index;
    if (info)
        vms_memcpy(info, &args.info, sizeof(*info));

    return args.status;
}
