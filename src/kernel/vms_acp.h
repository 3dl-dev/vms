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
 * ============================================================================
 * IO$_ACCESS / IO$_DEACCESS -- open a file by name or by FID (vms-204, epic
 * vms-208). The FOURTH rung: the ACP-QIO FILE OPERATIONS the channel/mount
 * front-end above (0x68-0x6A) was cut to carry, occupying the reserved tail
 * 0x6B-0x6C of the ACP band.
 *
 * On real OpenVMS these are $QIO function codes (IO$_ACCESS/IO$_DEACCESS, from
 * $IODEF) on a channel $ASSIGNed to the volume, with five function-dependent
 * QIO parameters: P1 = the FIB (File Information Block), P2 = the file-name
 * string, P3/P4 = the resultant-name buffer, P5 = the attribute (ATR) control
 * list (VSI OpenVMS I/O User's Reference Manual, "ACP-QIO Interface"). OVMX
 * reaches the executive over /dev/vms rather than a byte-level $QIO wire, so
 * the FIB (P1)/name (P2)/ATR list (P5) are marshalled into the ONE flat,
 * fixed-width arg struct below and issued as an ioctl -- exactly the executive
 * convention the mount/assign structs above use.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8, posture D2). The FIB field NAMES/roles
 * (FIB$W_FID, FIB$W_DID, FIB$L_ACCTL) and the ATR codes (ATR$C_UCHAR,
 * ATR$C_RECATTR, ATR$C_FPRO, ATR$C_UIC, ATR$C_CREDATE/_REVDATE/...) are from
 * the public I/O manual + $FIBDEF/$ATRDEF. The BYTE LAYOUT of the structs below
 * is an OVMX DESIGN CHOICE (labelled), NOT a claim of VMS byte-fidelity -- the
 * manual publishes the FIB/ATR EFFECT, not a /dev/vms wire. Only the ODS-2
 * ON-DISK bytes these fields are read FROM are byte-authentic (the codec,
 * src/vmsfs/ods2/, validated against a real VAX volume). This is the same
 * resolution fibdef.h's pending sign-off (vms-531) points at.
 */

/* FIB$L_ACCTL access-control flags the ACP consumes. OVMX-original bit values
 * (the manual publishes FIB$V_WRITE/FIB$V_NOWRITE as a role, not a wire bit);
 * READ is the default (FIB$V_NOWRITE semantics) when WRITE is clear. */
#define VMS_ACP_ACCTL_WRITE   0x00000001u   /* FIB$V_WRITE: access for write */

/* Longest "NAME.TYPE" (P2) an IO$_ACCESS directory search takes. A directory
 * component is searched as "NAME.DIR". Fixed so the arg struct's size (hence
 * its ioctl encoding) is stable. */
#define VMS_ACP_NAME_SIZE     80

/*
 * The ATR-list (P5) SUBSET IO$_ACCESS returns and IO$_DEACCESS could write
 * back -- the file characteristics, record attributes (the 32-byte FAT), owner,
 * protection, the four VMS dates, and the derived size fields RMS $OPEN needs,
 * read verbatim from the file's on-disk FH2. A flat block rather than a
 * pointer-chased {size,code,addr} list: OVMX design choice (labelled), the
 * executive-ioctl convention. recattr[] is the FAT copied byte-for-byte (the
 * byte-authentic ATR$C_RECATTR); the decoded efblk/hiblk/ffbyte are the codec's
 * own accessors over it, surfaced so a caller need not re-decode the FAT.
 */
struct vms_acp_fileattr {
    uint32_t filechar;      /* ATR$C_UCHAR: fh2_filechar (bit 0x2000 = directory) */
    uint32_t efblk;         /* end-of-file VBN (decoded FAT) */
    uint32_t hiblk;         /* highest allocated VBN (decoded FAT) */
    uint16_t ffbyte;        /* first free byte in the EOF block */
    uint16_t fileprot;      /* ATR$C_FPRO: fh2_fileprot (4 nibbles S/O/G/W) */
    uint16_t uic_group;     /* ATR$C_UIC: fh2_fileowner group */
    uint16_t uic_member;    /* ATR$C_UIC: fh2_fileowner member */
    uint16_t revision;      /* fi2_revision (ident area) */
    uint16_t pad0;
    uint8_t  recattr[32];   /* ATR$C_RECATTR: the 32-byte FAT, verbatim */
    uint8_t  credate[8];    /* ATR$C_CREDATE (VMS 64-bit absolute time) */
    uint8_t  revdate[8];    /* ATR$C_REVDATE */
    uint8_t  expdate[8];    /* ATR$C_EXPDATE */
    uint8_t  bakdate[8];    /* ATR$C_BAKDATE */
};

/*
 * IO$_ACCESS: resolve a file and open it on a file-class channel.
 *
 *  - BY NAME (fidmode == 0): search the directory named by FIB$W_DID (a FID;
 *    all-zero => the MFD [000000], FID 4) for `name` ("NAME.TYPE", version in
 *    `version`, 0 => highest). A multi-level filespec is walked one IO$_ACCESS
 *    per level, DID chaining to each resolved sub-directory FID -- the VMS
 *    model (RMS composes the chain).
 *  - BY FID (fidmode != 0): open the file whose FIB$W_FID is {fid_num/nmx,
 *    fid_seq, fid_rvn} directly, no directory search.
 *
 * On success the executive reads the file's on-disk FH2, builds the VBN->LBN
 * retrieval-pointer WINDOW into the channel's per-process ACP state, gates the
 * requested access (read, or write if acctl&WRITE) on proc->uic/privs
 * (INV-6 -- a denied open is SS$_NOPRIV, never a silent allow), and returns the
 * resolved FID (out), the attributes (out `attr`), and a window summary. A
 * caller may pass probe_vbn to have the executive resolve that VBN through the
 * freshly-built window and return its LBN (probe_lbn) -- the VBN->LBN mapping
 * proof.  Fail-honest: an unknown name/FID => SS$_NOSUCHFILE; a denied open =>
 * SS$_NOPRIV; an invalid channel => SS$_IVCHAN.
 */
struct vms_acp_access_args {
    uint32_t chan;              /* in: file-class channel ($ASSIGN of the volume) */
    uint32_t acctl;            /* in: FIB$L_ACCTL flags (VMS_ACP_ACCTL_*) */
    uint16_t did_num;          /* in: FIB$W_DID directory FID (0/0/0 => MFD) */
    uint16_t did_seq;
    uint8_t  did_rvn;
    uint8_t  did_nmx;
    uint8_t  fidmode;          /* in: !=0 => open by FID below, ignore name/DID */
    uint8_t  pad0;
    uint16_t fid_num;          /* in (fidmode) / out: FIB$W_FID file number low 16 */
    uint16_t fid_seq;          /* in (fidmode) / out: sequence */
    uint8_t  fid_rvn;          /* in (fidmode) / out: relative volume */
    uint8_t  fid_nmx;          /* in (fidmode) / out: file number high 8 */
    uint16_t version;          /* in: wanted version (0 => highest) */
    uint16_t out_version;      /* out: resolved version */
    uint16_t pad1;
    uint32_t probe_vbn;        /* in: VBN to resolve through the window (0 => none) */
    uint32_t probe_lbn;        /* out: LBN probe_vbn maps to (0 if none/unmapped) */
    uint32_t window_nextents;  /* out: extents in the built window */
    uint32_t total_blocks;     /* out: sum of window extent counts */
    uint32_t first_lbn;        /* out: LBN of the window's first extent (VBN 1) */
    char     name[VMS_ACP_NAME_SIZE]; /* in (P2): "NAME.TYPE" incl type */
    struct vms_acp_fileattr attr;     /* out (P5 subset) */
    uint32_t status;           /* out: SS$_ */
    uint32_t pad2;
};

/*
 * IO$_DEACCESS: release the file accessed on `chan` -- tear down its window and
 * mark the channel not-accessed. (Attribute write-back is a no-op for a
 * read-mode open, which modifies nothing; the write path is a later rung.)
 * SS$_FILNOTACC if the channel has no accessed file; SS$_IVCHAN if invalid.
 */
struct vms_acp_deaccess_args {
    uint32_t chan;             /* in: file-class channel */
    uint32_t status;           /* out: SS$_ */
};

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

#define VMS_IOCTL_ACP_ACCESS  _IOWR(VMS_IOC_MAGIC, 0x6B, struct vms_acp_access_args)
#define VMS_IOCTL_ACP_DEACCESS _IOWR(VMS_IOC_MAGIC, 0x6C, struct vms_acp_deaccess_args)

/*
 * ============================================================================
 * IO$_READVBLK / IO$_WRITEVBLK -- virtual-block transfer on an ACCESSED file
 * channel (vms-c60, epic vms-208). The FIFTH rung: the ACP-QIO record/block I/O
 * the IO$_ACCESS window (0x6B) was built to carry, occupying the reserved tail
 * 0x6D-0x6E of the ACP band (0x6F is IO$_ACPCONTROL / wildcard $SEARCH).
 *
 * On real OpenVMS these are $QIO function codes (IO$_READVBLK/IO$_WRITEVBLK,
 * $IODEF) on a channel $ASSIGNed to the volume with an accessed file: P1 = the
 * buffer, P2 = the byte count, P3 = the starting VBN (VSI OpenVMS I/O User's
 * Reference Manual, "ACP-QIO Interface"; the ACP window maps VBN->LBN and RMS
 * $GET/$PUT are layered on these). OVMX reaches the executive over /dev/vms, so
 * the transfer control fields are marshalled into the flat arg struct below and
 * the DATA is copied separately to/from `buffer` (a user address) by the
 * handler -- the executive convention (mount/access structs above), not a
 * byte-level $QIO wire.
 *
 * ONE struct serves BOTH directions (a virtual-block transfer is symmetric; the
 * ioctl code selects read vs write). A WRITE whose end position lies past the
 * file's current highest allocated VBN triggers an IMPLICIT EXTEND: the
 * executive allocates the shortfall from the volume's BITMAP.SYS storage bitmap,
 * appends a retrieval pointer to the file's FH2 (growing its window), and
 * updates the header's HIBLK/EOF -- matching RMS $EXTEND / a $PUT past EOF. This
 * is the executive doing the file-system's allocation, in the caller's context,
 * exactly as the ODS-2 XQP does (design §4.2).
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8, posture D2): the arg-struct byte layout is an
 * OVMX design choice (labelled), as for the other ACP structs; the on-disk
 * bytes it reads/writes (FM2 map, RECATTR EOF/HIBLK, BITMAP.SYS bits, the FH2
 * checksum) are byte-authentic via the codec (src/vmsfs/ods2/, ods2_edit.c).
 */
struct vms_acp_rw_args {
    uint32_t chan;        /* in: file-class channel with an accessed file */
    uint32_t vbn;         /* in: starting virtual block number (1-based) */
    uint32_t offset;      /* in: byte offset within the starting block (0..511) */
    uint32_t length;      /* in: transfer length in bytes */
    uint64_t buffer;      /* in: user data buffer (READ: dest; WRITE: src) */
    uint32_t xferred;     /* out: bytes transferred */
    uint32_t new_hiblk;   /* out: highest allocated VBN after (grows on extend) */
    uint32_t new_efblk;   /* out: end-of-file VBN after */
    uint32_t extended;    /* out: blocks newly allocated by an implicit extend */
    uint32_t status;      /* out: SS$_ */
    uint32_t pad;
};

#define VMS_IOCTL_ACP_READVBLK  _IOWR(VMS_IOC_MAGIC, 0x6D, struct vms_acp_rw_args)
#define VMS_IOCTL_ACP_WRITEVBLK _IOWR(VMS_IOC_MAGIC, 0x6E, struct vms_acp_rw_args)

_Static_assert(sizeof(struct vms_acp_rw_args) == 48,
               "vms_acp_rw_args changed size -- ACP READVBLK/WRITEVBLK ABI break");
_Static_assert(VMS_IOCTL_ACP_READVBLK == 0xC030566Du,
               "VMS_IOCTL_ACP_READVBLK encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_ACP_WRITEVBLK == 0xC030566Eu,
               "VMS_IOCTL_ACP_WRITEVBLK encodes differently here than on the reference build");

_Static_assert(sizeof(struct vms_acp_fileattr) == 88,
               "vms_acp_fileattr changed size -- IO$_ACCESS ATR ABI break");
_Static_assert(sizeof(struct vms_acp_access_args) == 224,
               "vms_acp_access_args changed size -- VMS_IOCTL_ACP_ACCESS ABI break");
_Static_assert(sizeof(struct vms_acp_deaccess_args) == 8,
               "vms_acp_deaccess_args changed size -- VMS_IOCTL_ACP_DEACCESS ABI break");
_Static_assert(VMS_IOCTL_ACP_ACCESS == 0xC0E0566Bu,
               "VMS_IOCTL_ACP_ACCESS encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_ACP_DEACCESS == 0xC008566Cu,
               "VMS_IOCTL_ACP_DEACCESS encodes differently here than on the reference build");

#endif /* _VMS_ACP_H */
