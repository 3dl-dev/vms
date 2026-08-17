/*
 * imgact_acp.h - IMGACT reads images through the executive Files-11 (ODS-2) ACP
 * (vms-3e8e, rung of epic vms-208).
 *
 * On real OpenVMS an image is activated by ACCESSing its file on a channel
 * $ASSIGNed to the volume and mapping its sections through the file's window
 * ($QIO IO$_ACCESS + the image sections mapped through the retrieval-pointer
 * window; VSI I/O User's Reference, "ACP-QIO Interface"). This module is the
 * OVMX realisation of that model for the freestanding IMGACT.EXE: instead of
 * open()/pread()/mmap() on a /vms POSIX path (the passthrough the Files-11 ACP
 * pivot retires, docs/design-files11-acp-executive.md §4.6), it $ASSIGNs a
 * file-class channel to the mounted ODS-2 volume, IO$_ACCESSes the image file
 * by walking its directory chain, and reads its bytes with IO$_READVBLK -- all
 * over /dev/vms.
 *
 * NO POSIX FALLBACK (CLAUDE.md Rule 9 / INV-6). Every entry point below fails
 * honestly -- SS$_NOSUCHDEV when /dev/vms is unreachable or the boot volume is
 * not ACP-mounted, SS$_NOSUCHFILE when the file is not on the volume -- and
 * NEVER silently reads the image off a /vms POSIX tree instead. That silent
 * fallback is exactly the LARP bug class the pivot killed.
 *
 * ATOMIC-FLIP-GROUP MEMBER (red-by-design). Boot does not yet ACP-mount
 * SYS$DISK, so IMGACT's image reads through this path fail-honest at boot until
 * the flip lands the executive-global $MOUNT of the system disk. Proven now
 * against a real /dev/vms over an ACP-mounted fixture volume
 * (tests/qemu/test_syssvc_imgact_acp.c).
 *
 * FREESTANDING + TESTABLE. IMGACT.EXE is -ffreestanding/-nostdlib and reaches
 * /dev/vms via raw syscalls; the QEMU test is a hosted binary and reaches it
 * via libc. The ONLY seam that differs is the three host primitives below --
 * so the test exercises the EXACT ACP read logic IMGACT runs, not a
 * re-implementation of it.
 */

#ifndef IMGACT_ACP_H
#define IMGACT_ACP_H

#include <stdint.h>

/* --------------------------------------------------------------------------
 * Host primitives (the freestanding/hosted seam). IMGACT.EXE backs these with
 * raw syscall6(); tests/qemu/test_syssvc_imgact_acp.c backs them with libc.
 * -------------------------------------------------------------------------- */

/* Open /dev/vms for read/write; return a descriptor, or -1 on failure. */
int  imgact_acp_dev_open(void);
/* Close a descriptor returned by imgact_acp_dev_open(). */
void imgact_acp_dev_close(int fd);
/* Issue one ioctl on `fd`; return 0 on success, a negative errno on failure.
 * (The VMS SS$_ status is carried in the arg struct, not this return.) */
long imgact_acp_dev_ioctl(int fd, unsigned long req, void *arg);

/* --------------------------------------------------------------------------
 * An image file accessed over the Files-11 ACP.
 * -------------------------------------------------------------------------- */

struct imgact_acp_file {
	int      dev_fd;    /* /dev/vms descriptor owned by this handle */
	uint32_t chan;      /* file-class channel $ASSIGNed to the volume */
	uint32_t valid;     /* total valid bytes in the accessed file */
	int      accessed;  /* non-zero once IO$_ACCESS has the file open */
};

/*
 * $ASSIGN a file-class channel to `dev` (a mounted ODS-2 unit, e.g. "DKA0:")
 * and IO$_ACCESS the file at `path`. `path` is '/'-separated; a leading "/vms"
 * (the retired POSIX mount point) is stripped, each non-final component names
 * an ODS-2 directory (walked as "NAME.DIR", DID chaining to each resolved
 * sub-directory FID -- the VMS model), and the final component is the file
 * "NAME.TYPE" (highest version). On success *f is left with the file accessed
 * for read and ready for imgact_acp_pread().
 *
 * Returns a VMS status (bit 0 set == success). Fail-honest: SS$_NOSUCHDEV
 * (no /dev/vms, or the volume is not ACP-mounted), SS$_NOSUCHFILE (a path
 * component is not on the volume). Never a POSIX fallback.
 */
uint32_t imgact_acp_open(struct imgact_acp_file *f, const char *dev,
			 const char *path);

/*
 * Read up to `n` bytes at byte offset `off` from the accessed file via
 * IO$_READVBLK (the byte offset is resolved to {VBN, in-block offset} through
 * the file's window). Returns the byte count transferred (>= 0; short, or 0,
 * at end-of-file), or a negative value on error. Modelled on pread(2) so the
 * activator's existing exact-count read checks keep working.
 */
long imgact_acp_pread(struct imgact_acp_file *f, void *buf,
		      unsigned long n, long off);

/* IO$_DEACCESS the file, $DASSGN the channel, and close the /dev/vms fd. */
void imgact_acp_close(struct imgact_acp_file *f);

#endif /* IMGACT_ACP_H */
