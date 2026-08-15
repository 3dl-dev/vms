/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * vmsfs/volume.h - the userspace MOUNTED-VOLUME table (vms-351, epic vms-5eb
 * "B2"): device name -> (open backing fd + cached genuine-ODS-2 block-backed
 * reader handle, ods2_bdev_t).
 *
 * WHY THIS EXISTS, AND WHAT IT IS NOT (read before wiring a consumer).
 *
 * The ODS-2 real-runtime program (docs/design-ods2-runtime-flip.md, epic
 * vms-5eb) retires the `vmsfs_to_linux_path -> /vms -> POSIX` passthrough for
 * SYS$DISK and has the live userspace RMS/DCL/MOUNT path read genuine ODS-2
 * over a raw block device through src/vmsfs/ods2/ (ods2_bdev_*). §4 of that
 * record ("B2") identifies the missing substrate the atomic flip (rungs
 * R2/R3/R5/R6) will consume: a process-wide mapping from a VMS device (DKA0:)
 * to an OPEN fd on its backing block device plus a cached, validated
 * ods2_bdev_t volume handle, populated at boot by PID 1.
 *
 * This module is that substrate, landed ADDITIVELY the way R1/R4/R8 were: it
 * FLIPS NO LIVE PATH. It only makes the volume handle AVAILABLE. Nothing here
 * removes or bypasses the /vms passthrough, and no RMS/DCL/IMGACT/LOGINOUT/
 * INSTALL consumer is rerouted onto the handle by this header -- that
 * rerouting + retiring the passthrough IS the atomic group (R2+R3+R5+R6) and
 * is out of scope here. Boot still reaches login exactly as before.
 *
 * FAIL-HONEST (CLAUDE.md Rule 9 / INV-6). A registered handle reads the REAL
 * block device via pread (ods2_bdev over the fd). If the backing store is
 * absent, unopenable, or is not a genuine ODS-2 (DECFILE11B) volume,
 * registration returns the honest error and caches NOTHING -- never a
 * per-process fake that reports a volume that is not there. A lookup for an
 * unregistered device returns NULL, exactly as sys_lock.c fails SS$_NOSUCHDEV
 * rather than faking an executive facility.
 *
 * PROCESS-LOCAL BY CONSTRUCTION. An fd and an ods2_bdev_t are per-process, so
 * this table is per-process, populated at process start (PID 1 at boot; each
 * future RMS/DCL consumer at its own init). That is honest, not a fake: every
 * process opens its OWN fd onto the ONE real shared block device and reads the
 * same bytes -- the volume is genuinely shared at the device, the way a real
 * VMS mounted volume is shared executive state while each accessor holds its
 * own channel. (Contrast the per-process userspace fakes INV-6 forbids, which
 * share NOTHING because they invent state instead of reading the real device.)
 */

#ifndef __VMSFS_VOLUME_H
#define __VMSFS_VOLUME_H

#include <stddef.h>

#include "vmsfs/ods2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum simultaneously-registered mounted volumes (DKA0:, DKB0:, ...). */
#define VMS_VOLUME_MAX_DEVICES 16

/*
 * Register device `devname` (e.g. "DKA0", with or without a trailing ':'),
 * backed by the Linux block device / image file `backing_path`, into the
 * process's mounted-volume table:
 *
 *   - opens `backing_path` O_RDONLY | O_CLOEXEC (a genuine ODS-2 SYS$DISK is
 *     read through this table; the block-backed writer is a separate rung, B1);
 *   - attaches a block-backed genuine-ODS-2 reader over the fd via
 *     ods2_bdev_open() (validates the DECFILE11B home block + both checksums +
 *     structure level at LBN 1), auto-detecting the volume size via lseek;
 *   - caches {devname, fd, ods2_bdev_t} so vmsfs_volume_handle(devname) returns
 *     the validated handle for as long as the registration lives.
 *
 * Returns (VMS status codes, odd == success):
 *   SS$_NORMAL       registered; *ods2_out (if non-NULL) == ODS2_OK.
 *   SS$_BADPARAM     devname/backing_path NULL, or devname too long.
 *   SS$_NOSUCHDEV    backing_path could not be opened (errno-class failure).
 *   SS$_DEVNOTMOUNT  opened, but NOT a genuine ODS-2 volume -- NOTHING is
 *                    registered and the fd is closed; *ods2_out carries the
 *                    ods2_bdev_open() error (fail-honest, Rule 9 / INV-6).
 *   SS$_DEVALLOC     the table is full.
 *
 * Re-registering an already-registered devname first drops the old handle
 * (closes its fd), then registers the new backing store. `ods2_out` may be
 * NULL. Thread-safe.
 */
int vmsfs_volume_register(const char *devname, const char *backing_path,
                          ods2_status_t *ods2_out);

/*
 * Return the cached, validated block-backed ODS-2 handle for `devname`, or
 * NULL if no genuine-ODS-2 volume is registered under that name. The handle
 * (and its backing fd) are owned by the table and stay valid until
 * vmsfs_volume_unregister(devname) or process exit. The returned pointer is
 * suitable as the first argument to ods2_bdev_resolve_dir/_resolve_file/
 * _read_file/_read_file_text and friends. Thread-safe with respect to the
 * table; the handle itself is read-only after registration.
 */
const ods2_bdev_t *vmsfs_volume_handle(const char *devname);

/*
 * Drop `devname`'s registration: close its backing fd and free its slot.
 * Returns SS$_NORMAL, SS$_NOSUCHDEV if not registered, or SS$_BADPARAM.
 * Thread-safe.
 */
int vmsfs_volume_unregister(const char *devname);

/* Number of currently-registered volume handles. Thread-safe. */
int vmsfs_volume_count(void);

#ifdef __cplusplus
}
#endif

#endif /* __VMSFS_VOLUME_H */
