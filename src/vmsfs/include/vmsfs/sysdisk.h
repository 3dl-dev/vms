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

/* ================================================================
 * MOUNT VOLUME VALIDATION (vms-6f5, R5-MOUNT of epic vms-5eb -- the ODS-2
 * runtime flip). The MOUNT half of the reroute: instead of mount(2)-ing an
 * arbitrary backing device as vmsfs and reporting success (the retired /vms
 * passthrough, which never proved the media was ODS-2), MOUNT parses the
 * candidate volume's genuine ODS-2 structures -- its HOME block (LBN 1:
 * DECFILE11B + both additive checksums + structure level, via ods2_bdev_open)
 * and its STORAGE CONTROL BLOCK (SCB, BITMAP.SYS VBN 1, via ods2_scb_parse) --
 * and ACCEPTS the volume only if both validate. Non-ODS-2 media is REJECTED
 * honestly (SS$_DEVNOTMOUNT), never aliased to success (INV-6 / Rule 9).
 *
 * ADDITIVE, EXACTLY LIKE THE READ ADAPTER. These entry points are consumed by
 * cmd_mount when the atomic group lands; they flip no live path and add no
 * on-disk format fact (Rule 8) -- they only sequence the reader's existing
 * home/SCB validators over the block device.
 * ================================================================ */

/*
 * Validate the volume on backing block-device / image path `backing_path`
 * (e.g. "/dev/vda"): open it read-only, validate the home block via
 * ods2_bdev_open() (DECFILE11B + checksums + strict structure level), then
 * read + validate the SCB (BITMAP.SYS VBN 1). On SS$_NORMAL, *home_out /
 * *scb_out (either may be NULL) carry the parsed structures -- volume label
 * (home hm2_volname), cluster factor (home hm2_cluster / scb_cluster), volume
 * size in blocks (scb_volsize).
 *
 * Returns (VMS status codes, odd == success):
 *   SS$_NORMAL       genuine ODS-2 volume; *home_out / *scb_out filled.
 *   SS$_BADPARAM     backing_path NULL.
 *   SS$_NOSUCHDEV    backing_path could not be opened.
 *   SS$_DEVNOTMOUNT  opened, but NOT a genuine ODS-2 volume (home invalid) --
 *                    fail-honest, the honest MOUNT rejection for non-ODS-2 media.
 *   SS$_DATACHECK    home valid but the SCB could not be read/validated
 *                    (a real medium fault, reported honestly not masked).
 */
int ods2_sysdisk_validate_backing(const char *backing_path,
                                  ods2_home_t *home_out, ods2_scb_t *scb_out);

/*
 * Validate the already-registered SYS$DISK volume (DKA0:) through the
 * process's registered ODS-2 volume handle (lazily registered from
 * OVMX_SYSDISK_DEV, exactly as every SYS$DISK read/write consumer does),
 * reading its cached validated home block and its SCB. Used by MOUNT of the
 * boot SYS$DISK, which under architecture A1 is NOT re-mounted as vmsfs but
 * resolved through the registered handle.
 *
 * Returns SS$_NORMAL (+ *home_out / *scb_out, either may be NULL),
 * SS$_DEVNOTMOUNT (no genuine-ODS-2 SYS$DISK volume registered -- fail-honest),
 * or SS$_DATACHECK (SCB unreadable).
 */
int ods2_sysdisk_validate_handle(ods2_home_t *home_out, ods2_scb_t *scb_out);

/* ================================================================
 * SYS$DISK WRITE adapter (vms-02e, epic vms-5eb -- the WRITE half of the
 * ODS-2 runtime flip). The write twin of the read adapter above: it turns a
 * resolved "/vms/A/B/.../NAME.EXT;ver" Linux path into a genuine-ODS-2 WRITE
 * over the SAME registered SYS$DISK volume handle (vmsfs_volume_handle(
 * SYSDISK_DEVICE)), through a per-call block-device-backed writer
 * (ods2_wvolume_open_bdev/_append_file/_create_file_raw/_create_dir/
 * _dir_insert) opened over the registered volume's fd.
 *
 * ADDITIVE, EXACTLY LIKE THE READ ADAPTER. This header FLIPS NO LIVE PATH: no
 * RMS/DCL/boot writer is rerouted onto it (that is the atomic group,
 * NonRMSWriters vms-d75 + R2). It only makes the write reroute AVAILABLE and
 * proves it against a live registered volume. Boot still reaches login
 * exactly as before.
 *
 * FAIL-HONEST (Rule 9 / INV-6). With no genuine-ODS-2 SYS$DISK volume
 * registered these return SS$_DEVNOTMOUNT and write NOTHING -- never a silent
 * POSIX fallback. A path not under "/vms" returns SS$_BADPARAM ("not mine").
 * If the registered volume was opened read-only (unwritable backing store),
 * the underlying pwrite fails and the write surfaces an honest SS$_DATACHECK,
 * never a fabricated success.
 *
 * CONCURRENCY: each call opens, mutates, flushes, and closes its own
 * block-device-backed writer over the registered fd (whose reads/writes use
 * absolute pread/pwrite offsets, independent of the reader handle). Intended
 * for the additive boot/runtime write path; not a multi-writer transaction
 * manager.
 * ================================================================ */

/*
 * Create a NEW file on SYS$DISK at resolved path "/vms/A/B/.../NAME.EXT;ver",
 * writing `data` (`data_len` bytes) VERBATIM (RFM=FIXED, create_file_raw
 * shape -- byte-identical read-back), and insert its directory record into
 * the (already-existing) parent directory. An absent ";ver" defaults to
 * version 1. Returns SS$_NORMAL, SS$_BADPARAM (path NULL / not under /vms /
 * no filename component), SS$_DEVNOTMOUNT (no volume), SS$_NOSUCHFILE (parent
 * directory chain does not exist), or SS$_DATACHECK (write / medium failure).
 */
int ods2_sysdisk_create_file(const char *linux_path,
                             const void *data, size_t data_len);

/*
 * Append `data_len` bytes VERBATIM to the END of an EXISTING SYS$DISK file at
 * resolved path "/vms/A/B/.../NAME.EXT;ver" -- the novel runtime path (an
 * OPERATOR.LOG-style repeated append). Read-back returns the full pre-existing
 * + appended content, byte-exact. Returns SS$_NORMAL, SS$_BADPARAM,
 * SS$_DEVNOTMOUNT, SS$_NOSUCHFILE (no such file), or SS$_DATACHECK.
 */
int ods2_sysdisk_append_file(const char *linux_path,
                             const void *data, size_t data_len);

/*
 * Create a NEW directory on SYS$DISK at resolved path "/vms/A/B/.../NEWDIR"
 * (the last component names the directory; its on-disk entry is "NEWDIR.DIR")
 * and insert its record into the (already-existing) parent. Returns
 * SS$_NORMAL, SS$_BADPARAM, SS$_DEVNOTMOUNT, SS$_NOSUCHFILE (parent chain
 * absent), or SS$_DATACHECK.
 */
int ods2_sysdisk_mkdir(const char *linux_path);

#ifdef __cplusplus
}
#endif

#endif /* __VMSFS_SYSDISK_H */
