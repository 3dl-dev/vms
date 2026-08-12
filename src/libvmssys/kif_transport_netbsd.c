/*
 * kif_transport_netbsd.c - the OVMX/NetBSD implementation of the vms_kif
 * substrate transport seam (kif_transport.h; rd vms-bfe, epic vms-8e8;
 * docs/design-ovmx-netbsd-syskrnl.md §3).
 *
 * The NetBSD leaf of the P1 seam, mirroring kif_transport_linux.c function for
 * function against the NetBSD `vms' pseudo-device (src/kernel-netbsd/): it opens
 * /dev/vms, issues one request against it, and maps the read-only arena off it,
 * using ordinary BSD open/close/ioctl/mmap. The policy layer (vms_kif.c) does
 * not change and does not know which substrate answered.
 *
 * BUILD PLACEMENT -- CRITICAL. This file is compiled FOR/INSIDE the NetBSD
 * target only. It is deliberately NOT in the Linux CMake source list:
 * src/libvmssys/CMakeLists.txt lists kif_transport_linux.c (the Linux leaf) and
 * NOT this file, and the native-link scripts (src/vmslink/mk_vmssys_shr.sh and
 * the src/imgact/test/run_*_native.sh harnesses) name kif_transport_linux too.
 * So this file sits beside its Linux sibling in the source tree but never enters
 * a Linux build -- it uses NetBSD userspace headers (BSD ioctl(2)) and would not
 * compile there. Nothing Linux-specific belongs in this file.
 *
 * Unlike the Linux leaf (which uses libvmssys' freestanding syscall wrappers
 * because that build has no libc), the NetBSD probe is an ordinary userspace
 * program, so this leaf uses libc's open/ioctl/mmap directly.
 */

#include "kif_transport.h"

#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int kif_xport_dev_open(void)
{
	/* The device name is the transport's own knowledge, per the contract;
	 * the policy layer never spells it. A negative return is the raw open(2)
	 * result -- the policy layer treats any negative as "no device" and fails
	 * honestly (SS$_NOSUCHDEV); it is never turned into a fake success. */
	return open("/dev/vms", O_RDWR);
}

void kif_xport_dev_close(int fd)
{
	(void)close(fd);
}

int kif_xport_ioctl(int fd, unsigned long req, void *arg)
{
	/* Returns 0 on delivery, or a NEGATIVE errno on failure -- the policy
	 * layer owns the errno->SS$ translation (vms_kif_kerr_to_ss). */
	if (ioctl(fd, (unsigned long)req, arg) < 0)
		return -errno;
	return 0;
}

void *kif_xport_mmap(int fd, unsigned long length, unsigned long offset)
{
	void *p = mmap(NULL, (size_t)length, PROT_READ, MAP_SHARED,
	    fd, (off_t)offset);

	/* Normalise the BSD failure sentinel to NULL so the policy layer tests
	 * one condition, exactly as the Linux leaf normalises MAP_FAILED. */
	if (p == MAP_FAILED)
		return (void *)0;
	return p;
}

void kif_xport_munmap(void *addr, unsigned long length)
{
	(void)munmap(addr, (size_t)length);
}
