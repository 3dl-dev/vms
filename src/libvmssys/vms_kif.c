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

uint32_t vms_kif_register(uint32_t vms_pid, uint64_t init_privs)
{
    struct vms_register_args args;

    if (vms_dev_fd < 0)
        return 0x00000014; /* SS$_BADPARAM */

    vms_memset(&args, 0, sizeof(args));
    args.vms_pid = vms_pid;
    args.init_privs = init_privs;

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
    int ret = vms_sys_ioctl(vms_dev_fd, VMS_IOCTL_DELIVERAST, 0);
    if (ret < 0)
        return -1;

    /* Output parameters (astadr, astprm, acmode) are part of the VMS
     * SYS$DELIVERAST interface and must remain in the signature for API
     * compatibility.  DELIVERAST currently uses _IO (no data transfer);
     * a full implementation would use _IOR to return the AST entry.
     * For now, the return value indicates whether an AST was delivered. */
    (void)astadr;
    (void)astprm;
    (void)acmode;
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
