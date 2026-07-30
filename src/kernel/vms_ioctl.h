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

/* ================================================================
 * Device table (executive-resident I/O database)
 *
 * On OpenVMS a device is not something a process owns a private idea
 * of: the driver enters a Unit Control Block in the executive's I/O
 * database at boot, and from then on the device EXISTS for every
 * process on the node. $ASSIGN takes a channel to it, $GETDVI reads
 * its attributes, $DEVICE_SCAN enumerates it, and SHOW DEVICE /
 * SHOW TERMINAL are readers of that one table. The owner, the
 * reference count and the terminal characteristics are properties of
 * the device, not of whoever happens to be asking.
 *
 * These ioctls put the same property behind /dev/vms (vms-d0b). The
 * console terminal OPA0: is created by the executive at module init,
 * exactly as the terminal driver creates it at VMS boot -- no process
 * registers it, and no process can be the only one that sees it.
 * ================================================================ */

/*
 * VMS device names: at most 15 significant characters plus the
 * terminating NUL (OpenVMS I/O User's Reference Manual; the physical
 * name form is ddcu:, e.g. OPA0:).
 */
#define VMS_DEVNAM_SIZE 16

/*
 * Terminal characteristics.
 *
 * PROVENANCE (CLAUDE.md rules 8 and 10): the NAMES below and the fact
 * that each is a two-state characteristic are pinned to the oracle --
 * SHOW TERMINAL on the ~/vax OpenVMS VAX V7.3 lab console (nodes VAX1
 * and VAX2, 30-JUL-2026), captured verbatim in
 * docs/oracle/vax73-terminal-device.md. Every name here appears in
 * that output; no name was invented, and no characteristic VMS does
 * not display was added.
 *
 * The BIT POSITIONS are an OVMX design choice and are NOT VMS's
 * $TTDEF layout. The public OpenVMS documentation available to this
 * work does not publish the byte-level TT$M_ layout, so rather than
 * guess at it OVMX defines its own vector and labels it as its own
 * (rule 8: define our representation, never present it as
 * VMS-authentic). Nothing outside the executive and its client may
 * assume these values match VMS.
 */
#define VMS_TTC_INTERACTIVE     (1ULL << 0)
#define VMS_TTC_ECHO            (1ULL << 1)
#define VMS_TTC_TYPEAHEAD       (1ULL << 2)
#define VMS_TTC_ESCAPE          (1ULL << 3)
#define VMS_TTC_HOSTSYNC        (1ULL << 4)
#define VMS_TTC_TTSYNC          (1ULL << 5)
#define VMS_TTC_LOWERCASE       (1ULL << 6)
#define VMS_TTC_TAB             (1ULL << 7)
#define VMS_TTC_WRAP            (1ULL << 8)
#define VMS_TTC_HARDCOPY        (1ULL << 9)
#define VMS_TTC_REMOTE          (1ULL << 10)
#define VMS_TTC_EIGHTBIT        (1ULL << 11)
#define VMS_TTC_BROADCAST       (1ULL << 12)
#define VMS_TTC_READSYNC        (1ULL << 13)
#define VMS_TTC_FORM            (1ULL << 14)
#define VMS_TTC_FULLDUP         (1ULL << 15)
#define VMS_TTC_MODEM           (1ULL << 16)
#define VMS_TTC_LOCAL_ECHO      (1ULL << 17)
#define VMS_TTC_AUTOBAUD        (1ULL << 18)
#define VMS_TTC_HANGUP          (1ULL << 19)
#define VMS_TTC_BRDCSTMBX       (1ULL << 20)
#define VMS_TTC_DMA             (1ULL << 21)
#define VMS_TTC_ALTYPEAHD       (1ULL << 22)
#define VMS_TTC_SET_SPEED       (1ULL << 23)
#define VMS_TTC_COMMSYNC        (1ULL << 24)
#define VMS_TTC_LINE_EDITING    (1ULL << 25)
#define VMS_TTC_INSERT_EDITING  (1ULL << 26)
#define VMS_TTC_FALLBACK        (1ULL << 27)
#define VMS_TTC_DIALUP          (1ULL << 28)
#define VMS_TTC_SECURE_SERVER   (1ULL << 29)
#define VMS_TTC_DISCONNECT      (1ULL << 30)
#define VMS_TTC_PASTHRU         (1ULL << 31)
#define VMS_TTC_SYSPASSWORD     (1ULL << 32)
#define VMS_TTC_SIXEL           (1ULL << 33)
#define VMS_TTC_SOFT_CHARACTERS (1ULL << 34)
#define VMS_TTC_PRINTER_PORT    (1ULL << 35)
#define VMS_TTC_NUMERIC_KEYPAD  (1ULL << 36)
#define VMS_TTC_ANSI_CRT        (1ULL << 37)
#define VMS_TTC_REGIS           (1ULL << 38)
#define VMS_TTC_BLOCK_MODE      (1ULL << 39)
#define VMS_TTC_ADVANCED_VIDEO  (1ULL << 40)
#define VMS_TTC_EDIT_MODE       (1ULL << 41)
#define VMS_TTC_DEC_CRT         (1ULL << 42)
#define VMS_TTC_DEC_CRT2        (1ULL << 43)
#define VMS_TTC_DEC_CRT3        (1ULL << 44)
#define VMS_TTC_DEC_CRT4        (1ULL << 45)
#define VMS_TTC_DEC_CRT5        (1ULL << 46)
#define VMS_TTC_ANSI_COLOR      (1ULL << 47)
#define VMS_TTC_VMS_STYLE_INPUT (1ULL << 48)

/*
 * One row of the executive device table, as handed to userspace.
 *
 * owner_pid and `allocated` are TWO DIFFERENT THINGS, exactly as the
 * oracle prints them as two different things (measured; see
 * docs/oracle/vax73-terminal-device.md section 7):
 *
 *   - owner_pid is "Owner process ID". A device can be owned with no
 *     allocation at all: on the lab a bare OPEN/WRITE to the
 *     non-shareable terminal TTA0: moved it from Owner "" to
 *     Owner "SYSTEM" / 20400216 with no "allocated" in its status
 *     clause, and the console OPA0: shows Owner "SYSTEM" on a system
 *     where nobody has run ALLOCATE. A channel to a SHAREABLE device
 *     (NLA0:) confers nothing.
 *   - `allocated` is the flag behind the word "allocated" in SHOW
 *     DEVICE/FULL's status clause, and only $ALLOC sets it.
 *
 * refcnt is the "Reference count": one per assigned channel plus one
 * for an outstanding allocation. Ownership itself costs no reference.
 *
 * opcnt/errcnt are the
 * "Operations completed" and "Error count" SHOW DEVICE/FULL reports.
 *
 * NOTE what is deliberately ABSENT (rule 10 -- hide what we cannot
 * answer faithfully rather than reporting a plausible value): the
 * oracle's SHOW TERMINAL also prints Input/Output speed, Parity, and
 * LFfill/CRfill. OVMX's console is a QEMU serial line with no such
 * physical parameters to report, so the executive does not carry a
 * value for them and no reader can print one.
 */
struct vms_devinfo {
    char     devnam[VMS_DEVNAM_SIZE];   /* physical name, e.g. "OPA0:" */
    uint32_t devclass;                  /* DC$_ device class */
    uint32_t devtype;                   /* device type code; 0 = Unknown */
    uint32_t owner_pid;                 /* VMS pid of the owner, 0 = unowned */
    uint32_t owner_uic;                 /* (group << 16) | member */
    uint32_t refcnt;                    /* channels assigned + allocation */
    uint32_t errcnt;                    /* Error count */
    uint64_t opcnt;                     /* Operations completed */
    uint64_t devchar;                   /* VMS_TTC_* (terminals only) */
    uint32_t width;                     /* terminal width */
    uint32_t page;                      /* terminal page length */
    uint32_t allocated;                 /* 1 = allocated to owner_pid */
    uint32_t pad;
};

/*
 * $ALLOC / $DALLOC: allocate a device to this process, and give it
 * back. Allocation is not the only route to ownership -- see
 * struct vms_devinfo -- but it is the only thing that makes a device
 * "allocated", and it holds the device after the last channel is gone.
 *
 * $ALLOC returns SS$_DEVALLOC when the device is OWNED by another
 * process, whether that owner allocated it (ALLOCATE OPA0: from a
 * detached process while the interactive job held the console) or
 * merely assigned a channel to it (ALLOCATE TTA0: while the detached
 * CHANHOLD process held one channel and no allocation). Both are
 * measured; see docs/oracle/vax73-terminal-device.md section 7.
 *
 * $DALLOC returns SS$_DEVNOTALLOC when this process does not have the
 * device ALLOCATED -- including when it owns the device by channel.
 */
struct vms_alloc_args {
    char     devnam[VMS_DEVNAM_SIZE];
    uint32_t status;
    uint32_t pad;
};

/* $ASSIGN: take a channel to a device by name. */
struct vms_assign_args {
    char     devnam[VMS_DEVNAM_SIZE];   /* in: device name (with or without ':') */
    uint32_t chan;                      /* out: channel number */
    uint32_t status;                    /* return: SS$_ status */
};

/* $DASSGN: give a channel back. */
struct vms_dassgn_args {
    uint32_t chan;
    uint32_t status;
};

/* Selector for VMS_IOCTL_GETDVI: how the device is named. */
#define VMS_DVI_SEL_DEVNAM  0   /* by info.devnam */
#define VMS_DVI_SEL_CHAN    1   /* by an assigned channel */

struct vms_getdvi_args {
    uint32_t select;            /* VMS_DVI_SEL_* */
    uint32_t chan;              /* in: channel, for VMS_DVI_SEL_CHAN */
    uint32_t status;            /* return: SS$_ status */
    uint32_t pad;
    struct vms_devinfo info;    /* in: name for SEL_DEVNAM; out: the row */
};

/*
 * Cursor-driven enumeration of the device table (the reader behind
 * SHOW DEVICE). Set index to 0 for the first row; each call returns
 * one row and advances index. SS$_NOMOREDEV terminates the scan,
 * which is what $DEVICE_SCAN returns when the search is exhausted.
 */
struct vms_devscan_args {
    uint32_t index;             /* in: cursor; out: cursor for next call */
    uint32_t status;            /* return: SS$_ status */
    struct vms_devinfo info;    /* out: the row at the incoming cursor */
};

/*
 * Modify terminal characteristics THROUGH AN ASSIGNED CHANNEL, the
 * way VMS does it: SET TERMINAL is $QIO IO$_SETMODE on a channel, not
 * an operation that names a device out of nowhere. A caller with no
 * channel to the device gets SS$_IVCHAN, which is why a process
 * cannot quietly redefine a terminal it never opened.
 */
#define VMS_TTSET_CHAR      0x1     /* apply setchar/clrchar */
#define VMS_TTSET_WIDTH     0x2     /* apply width */
#define VMS_TTSET_PAGE      0x4     /* apply page */

struct vms_setmode_args {
    uint32_t chan;              /* channel assigned to the terminal */
    uint32_t flags;             /* VMS_TTSET_* : which fields are being set */
    uint64_t setchar;           /* VMS_TTC_* bits to set */
    uint64_t clrchar;           /* VMS_TTC_* bits to clear */
    uint32_t width;
    uint32_t page;
    uint32_t status;            /* return: SS$_ status */
    uint32_t pad;
};

#define VMS_IOCTL_ASSIGN    _IOWR(VMS_IOC_MAGIC, 0x50, struct vms_assign_args)
#define VMS_IOCTL_DASSGN    _IOWR(VMS_IOC_MAGIC, 0x51, struct vms_dassgn_args)
#define VMS_IOCTL_GETDVI    _IOWR(VMS_IOC_MAGIC, 0x52, struct vms_getdvi_args)
#define VMS_IOCTL_DEVSCAN   _IOWR(VMS_IOC_MAGIC, 0x53, struct vms_devscan_args)
#define VMS_IOCTL_TTSETMODE _IOWR(VMS_IOC_MAGIC, 0x54, struct vms_setmode_args)
#define VMS_IOCTL_ALLOC     _IOWR(VMS_IOC_MAGIC, 0x55, struct vms_alloc_args)
#define VMS_IOCTL_DALLOC    _IOWR(VMS_IOC_MAGIC, 0x56, struct vms_alloc_args)

/*
 * The kernel module and the userspace client compile these structures
 * separately, from this one header, and then pass them across the
 * /dev/vms boundary by raw address. If a field is ever reordered,
 * widened or padded differently on one side, every ioctl above starts
 * reading the wrong offsets -- silently, and only at runtime, and only
 * for the fields past the change. Freeze the layouts here so that
 * failure is a compile error on whichever side moved.
 *
 * The ioctl encodings are asserted for the same reason and one more:
 * _IOWR folds sizeof(struct) into the request number, so a size change
 * ALSO renumbers the request. The executive would then reject it with
 * -ENOTTY rather than mis-decode it -- a different symptom, same root
 * cause, and equally worth catching before it ships.
 *
 * These values are measured, not chosen: aarch64 and x86_64 agree,
 * because every field is a fixed-width type.
 */
_Static_assert(sizeof(struct vms_devinfo) == 72,
               "struct vms_devinfo changed size -- kernel and userspace would disagree on device attribute offsets");
_Static_assert(sizeof(struct vms_assign_args) == 24,
               "struct vms_assign_args changed size -- $ASSIGN would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_dassgn_args) == 8,
               "struct vms_dassgn_args changed size -- $DASSGN would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_getdvi_args) == 88,
               "struct vms_getdvi_args changed size -- $GETDVI would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_devscan_args) == 80,
               "struct vms_devscan_args changed size -- $DEVICE_SCAN would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_setmode_args) == 40,
               "struct vms_setmode_args changed size -- IO$_SETMODE would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_alloc_args) == 24,
               "struct vms_alloc_args changed size -- $ALLOC/$DALLOC would decode at the wrong offsets");

_Static_assert(VMS_IOCTL_ASSIGN == 0xC0185650u,
               "VMS_IOCTL_ASSIGN encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_DASSGN == 0xC0085651u,
               "VMS_IOCTL_DASSGN encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_GETDVI == 0xC0585652u,
               "VMS_IOCTL_GETDVI encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_DEVSCAN == 0xC0505653u,
               "VMS_IOCTL_DEVSCAN encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_TTSETMODE == 0xC0285654u,
               "VMS_IOCTL_TTSETMODE encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_ALLOC == 0xC0185655u,
               "VMS_IOCTL_ALLOC encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_DALLOC == 0xC0185656u,
               "VMS_IOCTL_DALLOC encodes differently here than on the reference build");

/* ================================================================
 * Process table (executive-resident PCB directory)
 *
 * On OpenVMS the process name lives in the executive's process
 * database, not in the process's own address space: $SETPRN writes it,
 * $GETJPI resolves a process by it, and SHOW SYSTEM enumerates the
 * table. That is why a VMS process name means anything at all -- every
 * other process can see it.
 *
 * These ioctls put the same property behind /dev/vms. The entry is
 * keyed by the Linux pid, which is invariant across execve(), so the
 * name survives image activation without any userspace carrier.
 * ================================================================ */

/*
 * VMS process names are 1-15 characters (OpenVMS System Services
 * Reference, $SETPRN / $CREPRC prcnam argument). 16 bytes = 15
 * significant characters plus the NUL terminator, matching the
 * in-tree struct vms_pcb prcnam[16].
 */
#define VMS_PRCNAM_SIZE 16

/*
 * Inbound name transfer buffer -- OVMX DESIGN CHOICE, not a VMS format.
 *
 * A name travelling FROM userspace INTO the executive is carried in a
 * buffer strictly larger than the longest legal VMS process name, so an
 * OVERSIZED name arrives INTACT and the executive is the thing that
 * rejects it. Copying an inbound name into a VMS_PRCNAM_SIZE field in
 * userspace would truncate it into a legal-looking name and convert a
 * rejection into a success -- for $SETPRN, silently naming the process
 * something the caller never asked for; for the $GETJPI prcnam
 * selector, silently resolving a DIFFERENT process.
 *
 * Oracle (VAX1, OpenVMS VAX V7.3, 2026-07-30, documented tool output):
 *   $ SET PROCESS/NAME="IMPL8019NAM15XY"     ! 15 chars -> accepted
 *   $ SET PROCESS/NAME="IMPL8019NAM15XYZ"    ! 16 chars
 *   %SET-E-NOTSET, error modifying process name
 *   -SYSTEM-F-IVLOGNAM, invalid logical name
 *   $ WRITE SYS$OUTPUT F$GETJPI("","PRCNAM") ! old name UNCHANGED
 *   IMPL8019NAM15XY
 * VMS rejects the oversized name outright and leaves the existing name
 * in place. It does not truncate, and it does not partially apply.
 *
 * The userspace copy is still bounded (at VMS_PRCNAM_XFER - 1), but
 * that bound cannot turn a rejection into an acceptance: every name too
 * long to fit in VMS_PRCNAM_SIZE is illegal, and VMS_PRCNAM_XFER is far
 * larger than VMS_PRCNAM_SIZE, so a clipped name still has no NUL
 * within the executive's inspection window and is still rejected.
 */
#define VMS_PRCNAM_XFER 64

/*
 * One row of the executive process table.
 *
 * uic is [group,member] packed as (group << 16) | member -- the same
 * packing sys$getjpi's JPI$_UIC item returns. The executive derives it
 * from the task's credentials; it is never supplied by the process
 * itself (a process must not be able to declare its own UIC).
 */
struct vms_procinfo {
    uint32_t vms_pid;                   /* VMS-style process ID */
    uint32_t linux_pid;                 /* Linux pid backing the process */
    char     prcnam[VMS_PRCNAM_SIZE];   /* process name ("" if unnamed) */
    uint32_t uic;                       /* (group << 16) | member */
    uint8_t  current_mode;              /* PSL_C_KERNEL..PSL_C_USER */
    uint8_t  pad[3];
    uint64_t cur_privs;                 /* current privilege mask */
};

/* Selector for VMS_IOCTL_GETJPI: how the target process is named. */
#define VMS_JPI_SEL_SELF    0   /* the calling process */
#define VMS_JPI_SEL_PID     1   /* by vms_pid */
#define VMS_JPI_SEL_PRCNAM  2   /* by prcnam, within the caller's UIC group */

struct vms_getjpi_args {
    uint32_t select;            /* VMS_JPI_SEL_* */
    uint32_t status;            /* return: SS$_ status */
    struct vms_procinfo info;   /* in: vms_pid selector; out: the row */
    /*
     * The name selector lives OUTSIDE info, in an inbound transfer
     * buffer, so an oversized name reaches the executive untruncated.
     * info.prcnam is output-only: it is the row's name, never the
     * lookup key.
     */
    char     sel_prcnam[VMS_PRCNAM_XFER];
};

/*
 * Cursor-driven enumeration of the process table (the reader behind
 * SHOW SYSTEM). Set index to 0 for the first row; each call returns
 * one row and advances index. SS$_NONEXPR terminates the scan, which
 * is what $PROCESS_SCAN returns when the wildcard search is exhausted.
 */
struct vms_procscan_args {
    uint32_t index;             /* in: cursor; out: cursor for next call */
    uint32_t status;            /* return: SS$_ status */
    struct vms_procinfo info;   /* out: the row at the incoming cursor */
};

struct vms_setprn_args {
    char     prcnam[VMS_PRCNAM_XFER];   /* new process name, untruncated */
    uint32_t status;                    /* return: SS$_ status */
    uint32_t pad;
};

#define VMS_IOCTL_SETPRN    _IOWR(VMS_IOC_MAGIC, 0x41, struct vms_setprn_args)
#define VMS_IOCTL_GETJPI    _IOWR(VMS_IOC_MAGIC, 0x42, struct vms_getjpi_args)
#define VMS_IOCTL_PROCSCAN  _IOWR(VMS_IOC_MAGIC, 0x43, struct vms_procscan_args)

/*
 * ABI lock for the process-table ioctls (vms-8019).
 *
 * The kernel side of this header gets _IOWR from <linux/ioctl.h>; the
 * userspace side may instead fall back to the hand-rolled macros at the
 * top of this file, and OVMX builds on two architectures. Nothing
 * previously checked that all four combinations produce the same
 * numbers -- the executive proof has only ever been RUN on aarch64, so
 * the x86_64 half of that agreement was an assumption.
 *
 * These assertions turn it into a build failure instead. They are
 * evaluated by every translation unit that includes this header, kernel
 * or userspace, on whatever architecture is compiling -- so the CI
 * x86_64 build proves the layout even where the QEMU proof cannot run.
 *
 * The literals are the asm-generic _IOC encoding written out by hand:
 *   (dir << 30) | (sizeof(struct) << 16) | ('V' << 8) | nr
 * with dir == 3 (_IOC_READ|_IOC_WRITE). If a struct grows, these fail
 * and the ioctl NUMBER has changed -- which is a wire break, not a
 * cosmetic one, and must be handled deliberately.
 */
_Static_assert(sizeof(struct vms_procinfo) == 40,
               "vms_procinfo layout changed: process-table ioctl ABI break");
_Static_assert(sizeof(struct vms_setprn_args) == 72,
               "vms_setprn_args layout changed: VMS_IOCTL_SETPRN ABI break");
_Static_assert(sizeof(struct vms_getjpi_args) == 112,
               "vms_getjpi_args layout changed: VMS_IOCTL_GETJPI ABI break");
_Static_assert(sizeof(struct vms_procscan_args) == 48,
               "vms_procscan_args layout changed: VMS_IOCTL_PROCSCAN ABI break");
/*
 * The inbound transfer buffer must be strictly larger than the
 * executive's inspection window, or an oversized name would be clipped
 * to exactly VMS_PRCNAM_SIZE-1 characters and pass name_is_valid() --
 * reintroducing the silent truncation this split exists to kill.
 */
_Static_assert(VMS_PRCNAM_XFER > VMS_PRCNAM_SIZE,
               "VMS_PRCNAM_XFER must exceed VMS_PRCNAM_SIZE or oversized names get truncated into valid ones");
_Static_assert(VMS_IOCTL_SETPRN == 0xC0485641u,
               "VMS_IOCTL_SETPRN encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_GETJPI == 0xC0705642u,
               "VMS_IOCTL_GETJPI encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_PROCSCAN == 0xC0305643u,
               "VMS_IOCTL_PROCSCAN encodes differently here than on the reference build");

#endif /* _VMS_IOCTL_H */
