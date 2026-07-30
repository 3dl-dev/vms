/*
 * vms_errno.h - Linux errno to VMS SS$_ status code mapping
 *
 * When a Linux syscall returns a negative value, the absolute value
 * is a Linux errno.  This header provides a mapping to the closest
 * VMS SS$_ condition code.
 */

#ifndef _VMS_ERRNO_H
#define _VMS_ERRNO_H

#include <stdint.h>

/* Linux errno values (from kernel) */
#define VMS_EPERM        1
#define VMS_ENOENT       2
#define VMS_ESRCH        3
#define VMS_EINTR        4
#define VMS_EIO          5
#define VMS_ENXIO        6
#define VMS_E2BIG        7
#define VMS_ENOEXEC      8
#define VMS_EBADF        9
#define VMS_ECHILD       10
#define VMS_EAGAIN       11
#define VMS_ENOMEM       12
#define VMS_EACCES       13
#define VMS_EFAULT       14
#define VMS_ENOTBLK      15
#define VMS_EBUSY        16
#define VMS_EEXIST       17
#define VMS_EXDEV        18
#define VMS_ENODEV       19
#define VMS_ENOTDIR      20
#define VMS_EISDIR       21
#define VMS_EINVAL       22
#define VMS_ENFILE       23
#define VMS_EMFILE       24
#define VMS_ENOTTY       25
#define VMS_ETXTBSY      26
#define VMS_EFBIG        27
#define VMS_ENOSPC       28
#define VMS_ESPIPE       29
#define VMS_EROFS        30
#define VMS_EMLINK       31
#define VMS_EPIPE        32
#define VMS_EDOM         33
#define VMS_ERANGE       34
#define VMS_EDEADLK      35
#define VMS_ENAMETOOLONG 36
#define VMS_ENOSYS       38
#define VMS_ENOTEMPTY    39
#define VMS_ELOOP        40
#define VMS_EWOULDBLOCK  VMS_EAGAIN
#define VMS_ETIMEDOUT    110
#define VMS_ECONNREFUSED 111
#define VMS_ECONNRESET   104

/* Forward-declare SS$ codes (ssdef.h may not be included yet) */
#ifndef SS$_NORMAL
#define SS$_NORMAL       1
#define SS$_ACCVIO       12
#define SS$_BADPARAM     20
#define SS$_EXQUOTA      28
#define SS$_NOPRIV       36
#define SS$_ABORT        44
#define SS$_INSFMEM      292
#define SS$_TIMEOUT      556
/* ORACLE-PINNED (vms-9fc, 2026-07-30), reference lab VAX1 OpenVMS VAX V7.3.
 * $SSDEF extracted from SYS$LIBRARY:STARLET.MLB gives
 *     $EQU  SS$_ILLIOFUNC   244
 *     $EQU  SS$_BUGCHECK    676
 * and F$MESSAGE round-trips both:
 *     244 -> %SYSTEM-F-ILLIOFUNC, illegal I/O function code
 *     676 -> %SYSTEM-F-BUGCHECK,  internal consistency failure
 * The previous value on the ILLIOFUNC line, 580, is a DIFFERENT condition
 * on the oracle -- F$MESSAGE(580) is %SYSTEM-F-VASFULL, "virtual address
 * space is full". Corrected in ssdef.h at the same time; the two copies of
 * this constant disagreeing is vms-6d3, not this change. */
#define SS$_ILLIOFUNC    244
#define SS$_BUGCHECK     676
#define SS$_NOSUCHDEV    2680
#define SS$_NOSUCHFILE   2696
#define SS$_ENDOFFILE    2160
#define SS$_IVCHAN       602
#define SS$_IVDEVNAM     608
#define SS$_SSFAIL       636
/* ORACLE-PINNED (vms-8019): $SSDEF on the reference lab VAX V7.3 gives
 * SS$_NONEXPR 2280 / SS$_DUPLNAM 148; F$MESSAGE round-trips both. The
 * old 2540 / 434 are SS$_RIGHTSFULL / SS$_NOIOCHAN there. */
#define SS$_NONEXPR      2280
#define SS$_DEADLOCK     708
#define SS$_DUPLNAM      148
#define SS$_FILALRACC    2736
#define SS$_BUGCHECK     676
#define SS$_CANCEL       2096
#define SS$_UNSUPPORTED  2296
#endif

/*
 * vms_errno_to_status - Convert Linux errno to VMS status code.
 *
 * Returns the closest matching SS$_ code.
 */
static inline uint32_t vms_errno_to_status(int linux_errno)
{
    switch (linux_errno) {
    case 0:              return SS$_NORMAL;
    case VMS_EPERM:      return SS$_NOPRIV;
    case VMS_ENOENT:     return SS$_NOSUCHFILE;
    case VMS_ESRCH:      return SS$_NONEXPR;
    case VMS_EINTR:      return SS$_CANCEL;
    case VMS_EIO:        return SS$_SSFAIL;
    case VMS_ENXIO:      return SS$_NOSUCHDEV;
    case VMS_EBADF:      return SS$_IVCHAN;
    case VMS_ENOMEM:     return SS$_INSFMEM;
    case VMS_EACCES:     return SS$_NOPRIV;
    case VMS_EFAULT:     return SS$_ACCVIO;
    case VMS_EEXIST:     return SS$_DUPLNAM;
    case VMS_ENODEV:     return SS$_NOSUCHDEV;
    case VMS_EINVAL:     return SS$_BADPARAM;
    case VMS_EMFILE:     return SS$_EXQUOTA;
    case VMS_ENFILE:     return SS$_EXQUOTA;
    case VMS_ENOSPC:     return SS$_EXQUOTA;
    case VMS_EBUSY:      return SS$_FILALRACC;
    case VMS_EDEADLK:    return SS$_DEADLOCK;
    case VMS_ENOSYS:     return SS$_UNSUPPORTED;
    case VMS_ETIMEDOUT:  return SS$_TIMEOUT;
    case VMS_EPIPE:      return SS$_ABORT;
    case VMS_ENOTDIR:    return SS$_IVDEVNAM;
    case VMS_EISDIR:     return SS$_ILLIOFUNC;
    case VMS_EDOM:       return SS$_BADPARAM;
    case VMS_ERANGE:     return SS$_BADPARAM;
    default:             return SS$_SSFAIL;
    }
}

/*
 * vms_status_from_syscall - Convert a raw syscall return to VMS status.
 *
 * If ret >= 0, returns SS$_NORMAL.
 * If ret < 0, maps -ret through vms_errno_to_status.
 */
static inline uint32_t vms_status_from_syscall(long ret)
{
    if (ret >= 0)
        return SS$_NORMAL;
    return vms_errno_to_status((int)(-ret));
}

#endif /* _VMS_ERRNO_H */
