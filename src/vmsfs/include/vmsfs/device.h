#ifndef __VMSFS_DEVICE_H
#define __VMSFS_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#include "vmsfs/filespec.h"   /* VMSFS_MAX_FILESPEC (composed-candidate width) */

/*
 * VMS Device Table
 *
 * Maps VMS device names (DKA0:, DKB0:, etc.) to their Linux mount points.
 * This is the ONE place in the system where Unix paths are acknowledged.
 * Everything above this layer speaks VMS.
 *
 * Populated by STARTUP.EXE after mounting filesystems.
 * Queried by vmsfs_resolve_device() during path translation.
 */

#define VMS_MAX_DEVICES     16
#define VMS_DEVNAM_MAX      32
#define VMS_MOUNT_POINT_MAX 256

/* Register a device with its Linux mount point */
int vmsfs_device_add(const char *devname, const char *mount_point);

/* Look up a device's Linux mount point. Returns SS$_NORMAL or SS$_NOSUCHDEV */
int vmsfs_device_resolve(const char *devname, char *mount_point, size_t size);

/*
 * vms-b3e: is the DEVICE at the head of VMS filespec `spec` a unit genuinely
 * mounted RIGHT NOW in the kernel's own mount table (/proc/mounts) as
 * /mnt/<dev> -- a real, cross-process `mount -t vmsfs` volume the Files-11 ACP
 * does not own? Returns 1 iff so. Reads only /proc/mounts (global, cross-
 * process, INV-6); returns 0 for the seeded /vms SYS$DISK passthrough and for
 * any unmounted unit, so it never resurrects the /vms masquerade. Lets a file
 * command that failed to reach a unit through the ACP fall back to reading a
 * genuinely-mounted vmsfs volume without a silent host-passthrough bypass.
 */
int vmsfs_device_spec_kernel_mounted(const char *spec);

/* Remove a device from the table */
int vmsfs_device_remove(const char *devname);

/* Get number of registered devices */
int vmsfs_device_count(void);

/*
 * vms-d8e: Query a filespec device's concealed / rooted-directory attributes.
 *
 * Translates `device` (a bare device name, with or without a trailing ':')
 * as a logical name through LNM$FILE_DEV and inspects the logical's
 * translation attributes. A logical defined
 * /TRANSLATION_ATTRIBUTES=CONCEALED (LNM$M_CONCEALED, e.g.
 * DEFINE/TRANS=CONCEALED DISK$USER DKA100:[USERS.]) is a concealed device;
 * when its equivalence names a rooted directory (a '.' immediately before
 * the closing ']', "dev:[root.]") it is also a rooted-directory logical.
 * These are the LNM$M_CONCEALED / rooted properties RMS $PARSE reports as
 * NAM$M_CNCL_DEV / NAM$M_ROOT_DIR (VSI OpenVMS RMS Reference, NAM block
 * nam$l_fnb; VSI OpenVMS DCL Dictionary, DEFINE /TRANSLATION_ATTRIBUTES;
 * VSI OpenVMS User's Manual, "Rooted Directories").
 *
 * On return *is_concealed / *is_rooted (either may be NULL) are 0/1. Returns
 * SS$_NORMAL if `device` translated as a logical name (concealed or not),
 * SS$_NOLOGNAM if it is not a logical name at all (both outputs 0), or
 * SS$_BADPARAM for a bad argument.
 */
uint32_t vmsfs_device_concealed_rooted(const char *device,
                                       int *is_concealed, int *is_rooted);

/*
 * vmsfs_resolve_filespec_device - resolve the DEVICE field of `filespec` by
 * iterative logical-name translation through LNM$FILE_DEV, writing the
 * substituted spec to `result`. Wraps the one filespec-aware,
 * LNM$M_TERMINAL-honoring driver (lnm_translate_filespec, src/vmslnm): a
 * non-concealed device logical is substituted, an A -> B -> C chain composes,
 * iteration stops at a terminal translation, and a concealed device logical is
 * kept in the spec. The directory/name/type/version tail is preserved.
 *
 * This is the entry point $PARSE (src/vmsrms) uses so that vmsrms reaches
 * logical-name translation through its existing vmsfs dependency rather than
 * importing lnm_* directly (LIBVMSRMS$SHR does not --use LIBVMSLNM$SHR). On no
 * LNM manager or a no-op translation the spec is passed through unchanged.
 * Returns SS$_NORMAL, or SS$_BADPARAM for a bad argument.
 */
uint32_t vmsfs_resolve_filespec_device(const char *filespec, char *result,
                                       size_t result_size);

/*
 * vmsfs_compose_ods2_candidates - vms-0044 (flip-blocker, epic vms-208).
 *
 * Resolve a VMS filespec whose DEVICE field may be a directory logical
 * (SYS$SYSTEM), a concealed rooted search-list logical (SYS$SYSROOT), or a
 * chain of them, into one or more FULLY-COMPOSED on-volume ODS-2 candidate
 * specs -- PHYSICALDEV:[DIR.SUB.SUB]NAME.TYPE;VERSION -- in search-list order
 * (node-specific member first, common member second). This is the VMS-spec
 * sibling of vmsfs_to_linux_path(): same concealed-rooted composition, but it
 * stops at the resolved VMS spec the Files-11 ACP directory walk consumes
 * (the [SYSn.SYSCOMMON.SYSEXE] path -> a directory FID; the file within -> the
 * file FID), instead of continuing to a host path.
 *
 * Writes up to `max_out` candidate strings (each up to VMSFS_MAX_FILESPEC) and
 * returns the count. Fail-honest (INV-6): an unresolvable device chain returns
 * 0 -- never a fabricated path; the caller reports the honest RMS error
 * (RMS$_DNF / RMS$_FNF).
 */
int vmsfs_compose_ods2_candidates(const char *filespec,
                                  char out[][VMSFS_MAX_FILESPEC], int max_out);

#endif /* __VMSFS_DEVICE_H */
