/*
 * kif_transport_linux.c - the OVMX/Linux implementation of the vms_kif
 * substrate transport seam (vms-a3b, epic vms-8e8;
 * docs/design-ovmx-netbsd-syskrnl.md §3).
 *
 * This is the ONLY transport implementation that exists today. It reaches the
 * Linux executive (vms.ko) through its /dev/vms miscdevice using the
 * freestanding syscall wrappers in vms_syscall.h -- the exact calls that used
 * to live inline in vms_kif.c before the policy/transport split. There is no
 * behaviour change: each function below is the code vms_kif.c ran, moved
 * behind the kif_transport.h contract so a future kif_transport_netbsd.c (P2)
 * can supply the same operations against a NetBSD `vms` pseudo-device without
 * the policy layer knowing which substrate it runs on.
 *
 * Nothing NetBSD-specific belongs in this file; it is the Linux leaf.
 */

#include "kif_transport.h"
#include "vms_syscall.h"
#include "vms_types.h"

int kif_xport_dev_open(void)
{
    /* AT_FDCWD (-100), O_RDWR (2). The device name is the transport's own
     * knowledge, per the contract -- the policy layer never spells it. */
    return vms_sys_openat(-100 /* AT_FDCWD */, "/dev/vms", 2 /* O_RDWR */, 0);
}

void kif_xport_dev_close(int fd)
{
    vms_sys_close(fd);
}

int kif_xport_ioctl(int fd, unsigned long req, void *arg)
{
    return vms_sys_ioctl(fd, req, (unsigned long)arg);
}

void *kif_xport_mmap(int fd, unsigned long length, unsigned long offset)
{
    void *p = vms_sys_mmap(0, (vms_size_t)length,
                           VMS_PROT_READ, VMS_MAP_SHARED,
                           fd, offset);

    /* Normalise the Linux failure sentinel to NULL so the policy layer tests
     * one condition. (A genuine NULL mapping is likewise a failure here.) */
    if (p == VMS_MAP_FAILED)
        return 0;
    return p;
}

void kif_xport_munmap(void *addr, unsigned long length)
{
    vms_sys_munmap(addr, (vms_size_t)length);
}
