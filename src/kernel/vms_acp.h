/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_acp.h - Files-11 (ODS-2) ACP channel + mount ioctls (vms-149, epic vms-208)
 *
 * Shared by the kernel module and userspace, exactly like vms_lnm.h and
 * vms_mbx.h: both sides compile these structures from this one file and pass
 * them across /dev/vms by raw address. Included at the foot of vms_ioctl.h,
 * after _IOWR / VMS_IOC_MAGIC / VMS_DEVNAM_SIZE are in scope.
 *
 * WHAT THIS IS. The FIRST slice of the Files-11 ODS-2 ACP that the design
 * record docs/design-files11-acp-executive.md §4.2 places IN THE EXECUTIVE:
 * the CHANNEL front-end. On real OpenVMS a file operation is a $QIO on a
 * channel $ASSIGNed to the volume's device, serviced by the XQP in the
 * caller's context (IDSM; VSI I/O User's Reference Manual, "ACP-QIO
 * Interface"). OVMX already has $ASSIGN/$QIO/the FIB scaffolding but no ACP
 * arm (design §3.2). This rung adds:
 *   - an executive-global MOUNTED-VOLUME table (design §4.3): a $MOUNT records
 *     a boot unit as an ODS-2 volume that EVERY process then sees, replacing
 *     the userspace adapter's per-process passthrough;
 *   - a FILE-CLASS channel ($ASSIGN of the mounted unit returns an
 *     EXECUTIVE-backed channel bound to the volume -- not a Linux fd), released
 *     by the ordinary $DASSGN (VMS_IOCTL_DASSGN), exactly as a mailbox channel
 *     is (vms_mbx.h).
 *
 * It deliberately does NOT carry the ACP-QIO FILE OPERATIONS themselves
 * (IO$_ACCESS / IO$_CREATE / IO$_READVBLK / the FIB+ATR interface); those are
 * the later rungs of epic vms-208 (vms-204 / vms-c60 / vms-5303) and get the
 * remaining slots in this band. This rung reserves the band and establishes
 * the channel the file operations will later ride.
 *
 * IOCTL BAND -- 0x68-0x6F (OVMX DESIGN CHOICE, reconciling the design's
 * nominal "0x60-0x6F"). docs/design-files11-acp-executive.md §4.2 named the
 * ACP band "0x60-0x6F", written before the low half of that range was taken:
 * 0x60-0x62 are the logical-name ioctls (vms_lnm.h), 0x63-0x65 the P0/P1
 * region mappers (vms_ioctl.h), and 0x66-0x67 the ENTER_IMAGE/IMAGE_RUNDOWN
 * pair. The ACP therefore occupies the still-FREE tail of that nominal band,
 * 0x68-0x6F (mailboxes start at 0x70). This rung uses 0x68-0x6A; 0x6B-0x6F are
 * reserved for the ACP-QIO file operations the later rungs add. The design doc
 * §4.2 is updated to record this reconciliation.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8). The ODS-2 ON-DISK structures are byte-authentic
 * (the codec, src/vmsfs/ods2/, validated against a real VAX volume). The wire
 * BELOW -- these ioctl arg structs -- is NOT a claim of VMS byte-fidelity: VMS's
 * $ASSIGN/$QIO/$MOUNT publish an EFFECT, not a byte-level /dev/vms interface, so
 * OVMX defines its own flat arg-struct layout and labels it an OVMX design
 * choice, exactly as vms_mbx.h and the device-table ioctls do. Only the device
 * name string (ddcu:, at most 15 significant characters, VSI I/O User's
 * Reference) is a VMS-shaped field.
 */

#ifndef _VMS_ACP_H
#define _VMS_ACP_H

/*
 * $MOUNT / $DISMOUNT of an ODS-2 volume in the executive-global mounted table
 * (design §4.3). The unit is named by its VMS device name (e.g. "DKA0:" or the
 * discovered SYS$SYSDEVICE unit); the executive records the volume so every
 * process that $ASSIGNs the unit reaches the SAME mounted volume.
 *
 * THIS RUNG's mount is the volume-TABLE half only: it records the unit as a
 * mounted ODS-2 volume in shared executive state (real, cross-process --
 * NOT a per-process fake, INV-6). Binding the backing block device and
 * validating the home block / SCB (DECFILE11B) via the kernel-resident codec
 * is the later full-$MOUNT rung; this rung does not open the disk.
 */
struct vms_acp_mount_args {
    char     devnam[VMS_DEVNAM_SIZE];   /* in: unit name, e.g. "DKA0:" */
    uint32_t status;                    /* out: SS$_ status */
    uint32_t pad;
};

/* $DISMOUNT: remove the volume from the executive-global mounted table.
 * Refused (SS$_DEVALLOC) while any channel is still assigned to it -- a
 * channel bound to a volume must never outlive the volume it points at. */
struct vms_acp_dmount_args {
    char     devnam[VMS_DEVNAM_SIZE];   /* in: unit name */
    uint32_t status;                    /* out: SS$_ status */
    uint32_t pad;
};

/*
 * $ASSIGN a FILE-CLASS channel to a mounted ODS-2 volume. Returns an
 * EXECUTIVE-backed channel bound to the volume in the executive (design §4.2),
 * drawn from the same per-process channel-number space device and mailbox
 * channels use. SS$_NOSUCHDEV when the named unit is not a mounted volume
 * (fail-honest, INV-6 -- never a fabricated channel to a volume the executive
 * does not have). $DASSGN (VMS_IOCTL_DASSGN) releases it, exactly as it
 * releases a mailbox channel.
 */
struct vms_acp_assign_args {
    char     devnam[VMS_DEVNAM_SIZE];   /* in: mounted volume unit name */
    uint32_t chan;                      /* out: file-class channel number */
    uint32_t status;                    /* out: SS$_ status */
};

#define VMS_IOCTL_ACP_MOUNT   _IOWR(VMS_IOC_MAGIC, 0x68, struct vms_acp_mount_args)
#define VMS_IOCTL_ACP_DMOUNT  _IOWR(VMS_IOC_MAGIC, 0x69, struct vms_acp_dmount_args)
#define VMS_IOCTL_ACP_ASSIGN  _IOWR(VMS_IOC_MAGIC, 0x6A, struct vms_acp_assign_args)

/*
 * Freeze the shared layouts and the ioctl encodings -- see vms_mbx.h's and
 * vms_ioctl.h's identical notes for why: both sides of /dev/vms compile these
 * structs separately and pass them by raw address, and _IOWR folds sizeof into
 * the request number so a size drift silently renumbers the ioctl (-ENOTTY,
 * not a mis-decode). Values are measured; aarch64 and x86_64 agree because
 * every field is a fixed-width type.
 */
_Static_assert(sizeof(struct vms_acp_mount_args) == 24,
               "vms_acp_mount_args changed size -- VMS_IOCTL_ACP_MOUNT ABI break");
_Static_assert(sizeof(struct vms_acp_dmount_args) == 24,
               "vms_acp_dmount_args changed size -- VMS_IOCTL_ACP_DMOUNT ABI break");
_Static_assert(sizeof(struct vms_acp_assign_args) == 24,
               "vms_acp_assign_args changed size -- VMS_IOCTL_ACP_ASSIGN ABI break");
_Static_assert(VMS_IOCTL_ACP_MOUNT == 0xC0185668u,
               "VMS_IOCTL_ACP_MOUNT encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_ACP_DMOUNT == 0xC0185669u,
               "VMS_IOCTL_ACP_DMOUNT encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_ACP_ASSIGN == 0xC018566Au,
               "VMS_IOCTL_ACP_ASSIGN encodes differently here than on the reference build");

#endif /* _VMS_ACP_H */
