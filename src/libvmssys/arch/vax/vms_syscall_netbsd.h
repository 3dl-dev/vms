/*
 * vms_syscall_netbsd.h - the NetBSD (link-libc) implementation of the vms_sys_*
 * wrappers, for the OVMX/NetBSD SYSKRNL. Included by vms_syscall.h under
 * defined(__NetBSD__). Design: docs/design-ovmx-netbsd-syskrnl.md §4.1.
 *
 * WHY THIS IS DIFFERENT FROM THE LINUX/ALPHA PATH. The Linux/Alpha backends are
 * raw-freestanding: hand-written crt0.S/syscall.S issuing raw kernel traps, no
 * libc, and vms_syscall.h's wrappers call the __vms_syscallN trampolines. On
 * NetBSD that is the WRONG choice (§4.1): NetBSD's trap ABI is not a stable
 * public contract -- programs are expected to go through libc -- and VAX has no
 * OVMX-native toolchain, so the freestanding-purity argument does not apply. So
 * on NetBSD the vms_sys_* wrappers resolve to ordinary libc calls: NetBSD's
 * csu/libc provides crt0/TLS/syscall plumbing, and libvmssys contributes only
 * the VMS API surface + the /dev/vms kif transport (kif_transport_netbsd.c).
 *
 * WIDTH/ENDIAN NOTE (VAX = ILP32 little-endian; the P3 audit target). The vms_*
 * scalar typedefs in vms_types.h are all fixed-width or size_t/ptr-derived, so
 * they map onto NetBSD's libc types without a width mismatch on ILP32:
 *   vms_size_t  = unsigned long (32-bit) == size_t   on ILP32
 *   vms_ssize_t = long          (32-bit) == ssize_t  on ILP32
 *   vms_off_t   = int64_t                == off_t     (NetBSD off_t is 64-bit)
 * so every wrapper below is a straight, width-correct pass-through to libc.
 *
 * SCOPE OF THIS SHIM. It provides exactly the libc-backed wrappers the
 * netbsd-vax libvmssys build set uses (vms_kif.c: getpid, mprotect) plus the
 * flag-clean POSIX I/O wrappers, and the raw-return error helpers. It
 * deliberately does NOT provide the wrappers whose VMS_* flag constants (defined
 * in vms_types.h with LINUX numeric values) diverge from NetBSD's own -- notably
 * vms_sys_openat (VMS_O_CREAT etc.) and vms_sys_futex. Those need per-substrate
 * constant resolution or a native-flag translation and are recorded as deferred
 * items in the P3 audit (docs/design-ovmx-netbsd-syskrnl.md §4.2 audit table);
 * the freestanding buffered-I/O and futex facilities (vms_stdio.c, vms_futex.c)
 * that would use them are not part of the netbsd-vax build set -- NetBSD's
 * libc/libpthread supersede them on the link-libc path.
 */

#ifndef _VMS_SYSCALL_NETBSD_H
#define _VMS_SYSCALL_NETBSD_H

#include "vms_types.h"

#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>

/* ================================================================
 * Process
 * ================================================================ */

static inline vms_pid_t vms_sys_getpid(void)
{
    return (vms_pid_t)getpid();
}

static inline vms_uid_t vms_sys_getuid(void)
{
    return (vms_uid_t)getuid();
}

static inline vms_gid_t vms_sys_getgid(void)
{
    return (vms_gid_t)getgid();
}

/* ================================================================
 * Memory management -- the VMS_PROT_ and VMS_MAP_ flags that reach here are the
 * POSIX values, identical on Linux and NetBSD, so no flag translation is needed.
 * ================================================================ */

static inline int vms_sys_mprotect(void *addr, vms_size_t length, int prot)
{
    return mprotect(addr, (size_t)length, prot);
}

static inline void *vms_sys_mmap(void *addr, vms_size_t length, int prot,
                                 int flags, int fd, vms_off_t offset)
{
    return mmap(addr, (size_t)length, prot, flags, fd, (off_t)offset);
}

static inline int vms_sys_munmap(void *addr, vms_size_t length)
{
    return munmap(addr, (size_t)length);
}

/* ================================================================
 * Flag-clean POSIX I/O (no VMS_O_* / VMS_MAP_* constant divergence)
 * ================================================================ */

static inline int vms_sys_close(int fd)
{
    return close(fd);
}

static inline vms_ssize_t vms_sys_read(int fd, void *buf, vms_size_t count)
{
    return (vms_ssize_t)read(fd, buf, (size_t)count);
}

static inline vms_ssize_t vms_sys_write(int fd, const void *buf, vms_size_t count)
{
    return (vms_ssize_t)write(fd, buf, (size_t)count);
}

static inline vms_off_t vms_sys_lseek(int fd, vms_off_t offset, int whence)
{
    return (vms_off_t)lseek(fd, (off_t)offset, whence);
}

static inline int vms_sys_ioctl(int fd, unsigned long request, unsigned long arg)
{
    return ioctl(fd, request, (void *)(unsigned long)arg);
}

/* ================================================================
 * Raw-return error helpers. On the raw-freestanding path a syscall returns
 * negative-errno; libc returns -1 and sets errno. These helpers exist so any
 * substrate-agnostic caller that inspects a raw return keeps working: the
 * libc-backed wrappers above already return the libc convention (-1 / errno),
 * so a caller wanting the negative-errno form can fold errno in. Kept for API
 * parity with the Linux vms_syscall.h.
 * ================================================================ */

static inline int vms_syscall_is_error(long ret)
{
    return ret < 0;
}

static inline int vms_syscall_errno(long ret)
{
    (void)ret;
    return errno;
}

#endif /* _VMS_SYSCALL_NETBSD_H */
