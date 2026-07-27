/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_ioctl.h - Shared ioctl definitions for the VMS kernel module
 *
 * This header is used by both the kernel module and userspace code.
 * It defines all ioctl numbers and the data structures passed between
 * userspace and kernel.
 */

#ifndef _VMS_IOCTL_H
#define _VMS_IOCTL_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
/* ioctl macros for userspace -- use system macros if already defined */
#ifndef _IO
#define _IO(type, nr)           (((type) << 8) | (nr))
#endif
#ifndef _IOR
#define _IOR(type, nr, size)    (((2U) << 30) | ((sizeof(size)) << 16) | ((type) << 8) | (nr))
#endif
#ifndef _IOW
#define _IOW(type, nr, size)    (((1U) << 30) | ((sizeof(size)) << 16) | ((type) << 8) | (nr))
#endif
#ifndef _IOWR
#define _IOWR(type, nr, size)   (((3U) << 30) | ((sizeof(size)) << 16) | ((type) << 8) | (nr))
#endif
#endif

#define VMS_IOC_MAGIC 'V'

/* ================================================================
 * Access Mode (3a)
 * ================================================================ */

#define PSL_C_KERNEL    0
#define PSL_C_EXEC      1
#define PSL_C_SUPER     2
#define PSL_C_USER      3

struct vms_mode_args {
    uint8_t  mode;          /* target access mode (0-3) */
    uint8_t  pad[3];
    uint32_t status;        /* return: SS$_ status */
};

struct vms_priv_args {
    uint64_t mask;          /* privilege mask to set/clear/check */
    uint64_t prev;          /* return: previous privilege mask */
    uint32_t enable;        /* 1=enable, 0=disable */
    uint32_t permanent;     /* 1=permanent, 0=temporary */
    uint32_t status;        /* return: SS$_ status */
    uint32_t pad;
};

struct vms_getmode_args {
    uint8_t  mode;          /* return: current mode */
    uint8_t  pad[3];
    uint64_t cur_privs;     /* return: current privileges */
    uint64_t perm_privs;    /* return: permanent privileges */
};

#define VMS_IOCTL_SETMODE   _IOW(VMS_IOC_MAGIC, 0x01, struct vms_mode_args)
#define VMS_IOCTL_GETMODE   _IOR(VMS_IOC_MAGIC, 0x02, struct vms_getmode_args)
#define VMS_IOCTL_SETPRV    _IOWR(VMS_IOC_MAGIC, 0x03, struct vms_priv_args)
#define VMS_IOCTL_CHKPRIV   _IOWR(VMS_IOC_MAGIC, 0x04, struct vms_priv_args)

/* ================================================================
 * AST Delivery (3b)
 * ================================================================ */

struct vms_ast_args {
    uint64_t astadr;        /* AST routine address (in userspace) */
    uint64_t astprm;        /* AST parameter */
    uint8_t  acmode;        /* access mode for this AST (0-3) */
    uint8_t  pad[3];
    uint32_t status;        /* return: SS$_ status */
};

struct vms_setast_args {
    uint32_t enable;        /* 1=enable, 0=disable */
    uint32_t prev_state;    /* return: previous state */
    uint32_t status;        /* return: SS$_ status */
    uint32_t pad;
};

#define VMS_IOCTL_DCLAST      _IOW(VMS_IOC_MAGIC, 0x10, struct vms_ast_args)
#define VMS_IOCTL_SETAST      _IOWR(VMS_IOC_MAGIC, 0x11, struct vms_setast_args)
/* DELIVERAST: userspace passes a pointer to a vms_ast_args buffer to receive
 * the next pending AST. Changed from _IO to _IOR so the ioctl arg carries the
 * userspace buffer address instead of being ignored. */
#define VMS_IOCTL_DELIVERAST  _IOR(VMS_IOC_MAGIC, 0x12, struct vms_ast_args)

/* ================================================================
 * Event Flags (3c)
 * ================================================================ */

struct vms_ef_args {
    uint32_t efn;           /* event flag number (0-127) */
    uint32_t status;        /* return: SS$_ status */
};

struct vms_ef_wait_args {
    uint32_t efn;           /* cluster base EFN (for wflor/wfland) or single EFN */
    uint32_t mask;          /* bitmask for wflor/wfland */
    uint32_t status;        /* return: SS$_ status */
    uint32_t pad;
};

struct vms_ef_read_args {
    uint32_t efn;           /* event flag number */
    uint32_t state;         /* return: cluster state (32-bit) */
    uint32_t status;        /* return: SS$_ status */
    uint32_t pad;
};

struct vms_ef_common_args {
    uint32_t efn;           /* starting EFN (64 or 96) */
    char     name[32];      /* cluster name */
    uint32_t prot;          /* protection mask */
    uint32_t perm;          /* permanent flag */
    uint32_t status;        /* return: SS$_ status */
    uint32_t pad;
};

#define VMS_IOCTL_SETEF     _IOWR(VMS_IOC_MAGIC, 0x20, struct vms_ef_args)
#define VMS_IOCTL_CLREF     _IOWR(VMS_IOC_MAGIC, 0x21, struct vms_ef_args)
#define VMS_IOCTL_WAITFR    _IOWR(VMS_IOC_MAGIC, 0x22, struct vms_ef_args)
#define VMS_IOCTL_WFLOR     _IOWR(VMS_IOC_MAGIC, 0x23, struct vms_ef_wait_args)
#define VMS_IOCTL_WFLAND    _IOWR(VMS_IOC_MAGIC, 0x24, struct vms_ef_wait_args)
#define VMS_IOCTL_READEF    _IOWR(VMS_IOC_MAGIC, 0x25, struct vms_ef_read_args)
#define VMS_IOCTL_ASCEFC    _IOWR(VMS_IOC_MAGIC, 0x26, struct vms_ef_common_args)
#define VMS_IOCTL_DACEFC    _IOWR(VMS_IOC_MAGIC, 0x27, struct vms_ef_args)

/* ================================================================
 * Lock Manager (3d)
 * ================================================================ */

/* VMS lock modes */
#define LCK_K_NLMODE    0   /* Null */
#define LCK_K_CRMODE    1   /* Concurrent Read */
#define LCK_K_CWMODE    2   /* Concurrent Write */
#define LCK_K_PRMODE    3   /* Protected Read */
#define LCK_K_PWMODE    4   /* Protected Write */
#define LCK_K_EXMODE    5   /* Exclusive */

/* ENQ flags */
#define LCK_M_CONVERT   0x01   /* Convert existing lock */
#define LCK_M_NOQUEUE   0x02   /* Don't queue if not granted */
#define LCK_M_SYSTEM    0x04   /* System-wide resource */
#define LCK_M_VALBLK    0x08   /* Lock has value block */
/*
 * LCK_M_SYNC (OVMX design choice, not a real $LCKDEF bit): request that the
 * kernel ENQ/CONVERT ioctl BLOCK in-kernel until the lock is granted (or a
 * deadlock is detected), instead of returning immediately with the request
 * queued. This is how sys$enqw's synchronous "wait" is realized without a
 * userspace poll loop. Callers that want async ($ENQ) semantics leave it
 * clear. Lives in the kernel LCK_M_* namespace and is never exposed through
 * the public $ENQ flag contract (see src/libvms/syssvc/sys_lock.c).
 */
#define LCK_M_SYNC      0x10   /* Block in-kernel until granted (sync ENQ) */

/* Lock value block size */
#define LCK_VALBLK_SIZE 16

struct vms_enq_args {
    uint32_t efn;               /* event flag for completion */
    uint32_t lkmode;            /* requested lock mode (0-5) */
    uint32_t flags;             /* LCK_M_* flags */
    uint32_t parid;             /* parent lock ID (0 for root) */
    char     resnam[32];        /* resource name (null-terminated) */
    uint64_t astadr;            /* completion AST address */
    uint64_t astprm;            /* AST parameter */
    uint64_t blkastadr;         /* blocking AST address */
    uint32_t lkid;              /* return: lock ID (or input for convert) */
    uint32_t lk_status;         /* return: lock status (granted mode in LKSB) */
    uint8_t  valblk[LCK_VALBLK_SIZE]; /* lock value block */
    uint32_t status;            /* return: SS$_ status */
    uint32_t pad;
};

struct vms_deq_args {
    uint32_t lkid;              /* lock ID to dequeue */
    uint8_t  valblk[LCK_VALBLK_SIZE]; /* value block to write back */
    uint32_t flags;             /* dequeue flags */
    uint32_t status;            /* return: SS$_ status */
};

struct vms_getlki_args {
    uint32_t lkid;              /* lock ID to query */
    uint32_t granted_mode;      /* return: current granted mode */
    uint32_t requested_mode;    /* return: requested mode (if waiting) */
    uint32_t parent_id;         /* return: parent lock ID */
    char     resnam[32];        /* return: resource name */
    uint8_t  valblk[LCK_VALBLK_SIZE]; /* return: value block */
    uint32_t status;            /* return: SS$_ status */
    uint32_t pad;
};

#define VMS_IOCTL_ENQ       _IOWR(VMS_IOC_MAGIC, 0x30, struct vms_enq_args)
#define VMS_IOCTL_DEQ       _IOWR(VMS_IOC_MAGIC, 0x31, struct vms_deq_args)
#define VMS_IOCTL_CONVERT   _IOWR(VMS_IOC_MAGIC, 0x32, struct vms_enq_args)
#define VMS_IOCTL_GETLKI    _IOWR(VMS_IOC_MAGIC, 0x33, struct vms_getlki_args)

/* ================================================================
 * Process registration
 * ================================================================ */

struct vms_register_args {
    uint32_t vms_pid;           /* VMS-style process ID */
    uint64_t init_privs;        /* initial privilege mask */
    uint32_t status;            /* return: SS$_ status */
    uint32_t pad;
};

#define VMS_IOCTL_REGISTER  _IOWR(VMS_IOC_MAGIC, 0x40, struct vms_register_args)

#endif /* _VMS_IOCTL_H */
