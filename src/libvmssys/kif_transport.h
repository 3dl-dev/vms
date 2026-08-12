/*
 * kif_transport.h - the vms_kif SUBSTRATE TRANSPORT SEAM (vms-a3b, epic
 * vms-8e8; docs/design-ovmx-netbsd-syskrnl.md §3).
 *
 * vms_kif.c holds the SUBSTRATE-AGNOSTIC POLICY of the kernel-interface layer:
 * "attach to the executive", the open->register binding, the caller-census /
 * INV-6 honest-fail behaviour, and the errno->SS$ status mapping. NONE of that
 * knows how a request physically reaches the executive.
 *
 * This header is the CONTRACT for the SUBSTRATE-SPECIFIC TRANSPORT that does:
 * opening the /dev/vms device, issuing one request against it, and mapping the
 * read-only arena off it. On OVMX/Linux the executive is vms.ko and the
 * transport is Linux ioctl(2)/mmap(2) on a /dev/vms miscdevice -- implemented
 * in kif_transport_linux.c, the ONLY implementation that exists today.
 *
 * THE SEAM EXISTS SO A SECOND SYSKRNL CAN PLUG IN WITHOUT TOUCHING THE POLICY
 * LAYER. When OVMX/NetBSD lands (epic vms-8e8, P2), its executive is a NetBSD
 * `vms` pseudo-device and its transport is BSD ioctl/mmap; that is a future
 * kif_transport_netbsd.c implementing THIS SAME CONTRACT, selected at build
 * time in place of kif_transport_linux.c. vms_kif.c does not change. Phase 1
 * (this change) is a pure Linux refactor proving the seam with zero behaviour
 * change; it adds NO NetBSD code.
 *
 * The /dev/vms contract and every request code are UNCHANGED by this seam: the
 * request number is still computed by the policy layer from vms_ioctl.h's
 * VMS_IOCTL_* macros and passed through opaquely as `req`. Splitting the _IO*
 * request ENCODING per substrate (design §3 point 1) is a separate, later
 * concern -- it is not part of this transport contract.
 *
 * The functions below are INTERNAL to the libvmssys shareable image (they are
 * resolved between vms_kif.o and the transport object at link time and are NOT
 * part of the VMS symbol vector -- see src/vmslink/mk_vmssys_shr.sh). They
 * carry the kif_xport_ prefix, deliberately NOT vms_kif_, so they are neither
 * executive entry points nor part of the vms_kif caller census
 * (tests/integration/test_kif_caller_census.sh, whose universe is vms_kif.c's
 * own translation unit).
 */

#ifndef _KIF_TRANSPORT_H
#define _KIF_TRANSPORT_H

/*
 * kif_xport_dev_open - open the executive device.
 *
 * Returns a descriptor >= 0 on success, or a negative value on failure (the
 * raw open(2) return; the policy layer treats any negative as "no device").
 * Takes no arguments: the device name is the transport's own knowledge, not
 * the policy layer's.
 */
int kif_xport_dev_open(void);

/*
 * kif_xport_dev_close - close a descriptor returned by kif_xport_dev_open.
 * The policy layer decides WHEN to close (it only calls this on a valid
 * descriptor); the transport just performs it.
 */
void kif_xport_dev_close(int fd);

/*
 * kif_xport_ioctl - issue ONE request against the executive device.
 *
 * `req` is the substrate request number the policy layer computed (from
 * vms_ioctl.h). `arg` points at the request's struct vms_*_args payload.
 * Returns 0 on delivery, or a NEGATIVE ERRNO on failure -- the policy layer
 * owns the errno->SS$ translation (vms_kif_kerr_to_ss); the transport reports
 * the raw failure and interprets nothing.
 */
int kif_xport_ioctl(int fd, unsigned long req, void *arg);

/*
 * kif_xport_mmap - map the executive's read-only arena off the device.
 *
 * `length`/`offset` are the arena size and its device mmap offset, both the
 * policy layer's knowledge (VMS_LNM_MMAP_OFFSET). Returns the mapping, or NULL
 * on failure -- the substrate-specific failure sentinel (Linux MAP_FAILED) is
 * normalised to NULL here so the policy layer tests one thing.
 */
void *kif_xport_mmap(int fd, unsigned long length, unsigned long offset);

/*
 * kif_xport_munmap - drop a mapping returned by kif_xport_mmap.
 */
void kif_xport_munmap(void *addr, unsigned long length);

#endif /* _KIF_TRANSPORT_H */
