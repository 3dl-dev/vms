/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * vmsfs/sysdisk.h - the SYS$DISK read adapter (epic vms-5eb, the ODS-2 runtime
 * flip): the ONE choke point that reroutes a resolved SYS$DISK Linux path
 * ("/vms/SYS0/SYSCOMMON/SYSEXE/LOGIN.COM;3") to a genuine-ODS-2 read over the
 * registered block-device volume handle (vmsfs_volume_handle(SYSDISK_DEVICE)),
 * instead of open(2)/opendir(2) on the POSIX /vms passthrough.
 *
 * WHY THIS EXISTS, AND WHAT IT IS NOT (read before wiring a consumer).
 *
 * docs/design-ods2-runtime-flip.md (architecture A1) retires the
 * `vmsfs_to_linux_path -> /vms -> POSIX` passthrough for SYS$DISK: the live
 * userspace RMS/DCL/MOUNT path must read genuine ODS-2 over a raw block device
 * through src/vmsfs/ods2/ (ods2_bdev_*). B2 (src/vmsfs/vmsfs_volume.c) landed
 * the substrate -- a process-wide DKA0: -> {open fd + validated ods2_bdev_t}
 * table populated by PID 1. But every flip consumer (RMS rms_impl_open's
 * open(2), DCL cmd_directory/$SEARCH's opendir(2), MOUNT's home-block probe)
 * still needs the SAME small bridge: take the VMS filespec they have ALREADY
 * resolved to a `/vms/...` Linux path via vmsfs_to_linux_path, and turn that
 * path into an ods2_bdev directory-component walk + file resolve/read over the
 * cached handle. Rather than re-derive that bridge in RMS, in DCL, and in
 * MOUNT (three chances to diverge), it is factored HERE, tested once, and
 * consumed by all three when the atomic group (R2/R3/R5) lands.
 *
 * This module FLIPS NO LIVE PATH. It is additive the way ods2_path.c and
 * vmsfs_volume.c were: it makes the reroute AVAILABLE. No RMS/DCL/MOUNT
 * consumer is rerouted onto it by this header; that rerouting + retiring the
 * /vms passthrough IS the atomic group and is out of scope here. Boot still
 * reaches login exactly as before.
 *
 * BYTE-IDENTICAL READ (architecture A1). The R6 boot-master lays every regular
 * file onto the ODS-2 volume VERBATIM (ods2_wvolume_create_file_raw), so
 * ods2_sysdisk_read_file() returns the SAME bytes today's POSIX read(2) of the
 * /vms tree returns -- the flip changes the substrate, not the content.
 *
 * FAIL-HONEST (CLAUDE.md Rule 9 / INV-6). If no genuine-ODS-2 SYS$DISK volume
 * is registered, these return SS$_DEVNOTMOUNT and read NOTHING -- never a
 * silent POSIX fallback (the exact LARP the authenticity invariants forbid).
 * A path that is not on SYS$DISK (not under "/vms") is not this module's job
 * and returns SS$_BADPARAM so the caller keeps its own raw-Linux-path handling
 * for /tmp and friends -- an HONEST "not mine", not a fake success.
 */

#ifndef __VMSFS_SYSDISK_H
#define __VMSFS_SYSDISK_H

#include <stddef.h>

#include "vmsfs/ods2.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reroute predicate: does `linux_path` name a file on the genuine-ODS-2
 * SYS$DISK -- i.e. is it under SYSDISK_MOUNT ("/vms", with a '/' or end-of-
 * string boundary, never "/vmsXYZ")? A `/vms/...` path is served by the ODS-2
 * volume handle; anything else stays a raw Linux path the caller owns. Returns
 * 1 if on SYS$DISK, 0 otherwise (including NULL).
 */
int ods2_sysdisk_owns_path(const char *linux_path);

/*
 * Resolve a SYS$DISK file addressed by its resolved
 * "/vms/A/B/.../NAME.EXT;ver" Linux path to a validated ODS-2 file header +
 * FID, via the registered SYS$DISK volume handle. The path's components after
 * "/vms" are the directory chain (walked from the MFD, FID 4); the last
 * component is the filename (its ";ver" suffix, if any, selects the version --
 * absent or ";0" == highest). `bv_out` (may be NULL) receives the borrowed
 * volume handle for a following ods2_sysdisk_read via the header. `header_out`
 * (>= ODS2_BLOCK_SIZE) receives the validated file header block; `fid_out`
 * (may be NULL) the file's FID.
 *
 * Returns (VMS status codes, odd == success):
 *   SS$_NORMAL       resolved.
 *   SS$_BADPARAM     path NULL, not under "/vms", or malformed.
 *   SS$_DEVNOTMOUNT  no genuine-ODS-2 SYS$DISK volume registered (fail-honest).
 *   SS$_NOSUCHFILE   the directory chain or file does not exist on the volume.
 */
int ods2_sysdisk_resolve_file(const char *linux_path,
                              const ods2_bdev_t **bv_out,
                              ods2_fid_t *fid_out,
                              void *header_out, size_t header_len);

/*
 * Read a SYS$DISK file's raw content bytes (up to its recattr valid byte
 * count) into `out` (capacity `out_cap`), addressed by its resolved
 * "/vms/..." Linux path. *out_len (may be NULL) receives the byte count.
 * Byte-identical to the POSIX read of the /vms tree it replaces.
 *
 * Returns SS$_NORMAL, or the same failure codes as ods2_sysdisk_resolve_file,
 * plus SS$_DATACHECK if the content exceeds `out_cap`.
 */
int ods2_sysdisk_read_file(const char *linux_path,
                           void *out, size_t out_cap, size_t *out_len);

/*
 * List a SYS$DISK directory addressed by its resolved "/vms/A/B/..." Linux
 * path (a trailing '/' is tolerated; "/vms" itself lists the MFD). `cb` is
 * invoked once per directory entry (name, version, FID) exactly as
 * ods2_bdev_list_dir delivers them -- the ODS-2 twin of the opendir/readdir
 * loop rms_search.c / dcl_cmd_file.c run over the /vms tree today.
 *
 * Returns SS$_NORMAL, SS$_BADPARAM, SS$_DEVNOTMOUNT, or SS$_NOSUCHFILE.
 */
int ods2_sysdisk_list_dir(const char *linux_path,
                          ods2_dir_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* __VMSFS_SYSDISK_H */
