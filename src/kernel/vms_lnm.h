/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_lnm.h - Executive-resident logical name tables (LNM$SYSTEM et al.)
 *
 * Shared by the kernel module and userspace, exactly like vms_ioctl.h:
 * both sides compile these structures from this one file and pass them
 * across /dev/vms by raw address, and the READ path maps the arena below
 * into userspace read-only. Freeze the layouts with the _Static_asserts at
 * the foot of the file so any divergence is a compile error, not a runtime
 * mis-read.
 *
 * WHO OWNS WHAT (vms-d37, design docs/design-logical-name-placement.md,
 * ruled option "C-corrected"). On OpenVMS the SYSTEM/GROUP/JOB logical name
 * tables are EXECUTIVE-RESIDENT -- a name defined in LNM$SYSTEM by one
 * process is visible to every other process on the node. vms.ko owns their
 * storage here. Userspace TRANSLATES by reading a read-only mmap of the
 * arena (no syscall on the hot path) and MUTATES (define/deassign) through
 * an ioctl. LNM$PROCESS stays per-process and never appears in this arena.
 *
 * OVMX DESIGN CHOICE (CLAUDE.md Rule 8), stated rather than implied: the
 * BYTE-LEVEL LAYOUT below is ours. Public OpenVMS documentation describes
 * logical-name BEHAVIOUR (the table hierarchy, the LNM$FILE_DEV search
 * order, the attributes, the privileges) and that behaviour is what OVMX
 * reproduces -- but it does NOT publish the layout of the executive's
 * logical name database. This arena format, its seqlock, its fixed sizing
 * and its offset addressing are an OVMX construct and are never to be
 * presented as VMS-authentic, exactly as docs/design-link-native-toolchain.md
 * treats the image formats.
 *
 * VMS CONSTANT PROVENANCE (CLAUDE.md Rule 8, clean-room method). The VMS
 * symbolic names LNM$SYSTEM / LNM$GROUP / LNM$JOB are documented behaviour
 * (OpenVMS System Services Reference, $CRELNM/$TRNLNM; $LNMDEF). The
 * LNM_ATTR_* attribute bit values this arena stores are defined in
 * src/vmslnm/include/vms/logical.h and are not re-derived here. The numeric
 * VMS_LNM_TBL_* ids and the byte-level arena layout below are OVMX design
 * choices, not VMS values. No VMS value is self-certified here; the one
 * status value introduced for this facility (SS$_EXLNMQUOTA) is oracle-pinned
 * in vms_internal.h with its lab-1 F$MESSAGE provenance.
 */

#ifndef _VMS_LNM_H
#define _VMS_LNM_H

/*
 * Relies on the integer typedefs and on _IOWR / VMS_IOC_MAGIC already
 * being in scope. vms_ioctl.h includes this file at its foot, after it has
 * set both up for the kernel and userspace builds alike.
 */

/*
 * Executive-resident tables. LNM$PROCESS is deliberately NOT here: it is
 * per-process and never leaves the process (design §3.1).
 */
#define VMS_LNM_TBL_SYSTEM  1
#define VMS_LNM_TBL_GROUP   2
#define VMS_LNM_TBL_JOB     3

/*
 * Sizing. OVMX design choices (Rule 8) -- these are OVMX limits, not VMS
 * values. Real VMS sizes its logical-name hash tables from SYSGEN
 * parameters (LNMSHASHTBL class -- names pending operator sign-off, design
 * §4.3) and this arena adopts the SHAPE (fixed size chosen at module load,
 * fail honestly when full) rather than a VMS number.
 */
#define VMS_LNM_MAX_NAME    255     /* == LNM_MAX_NAME (vms/logical.h) */
#define VMS_LNM_MAX_VALUE   255     /* == LNM_MAX_VALUE */
#define VMS_LNM_MAX_EQUIV   8       /* inline equivalence strings per entry */
#define VMS_LNM_MAX_ENTRIES 512     /* fixed arena capacity */

#define VMS_LNM_ARENA_MAGIC   0x4C4E4D41u  /* 'LNMA' */
#define VMS_LNM_ARENA_VERSION 1u

/* The arena maps at this offset on the /dev/vms fd. */
#define VMS_LNM_MMAP_OFFSET   0u

/* One equivalence string of a (possibly multi-valued) logical name. */
struct vms_lnm_equiv {
    uint16_t length;
    uint8_t  index;                     /* equivalence index (0-based) */
    uint8_t  pad;
    char     value[VMS_LNM_MAX_VALUE + 1];
};

/*
 * One logical name. Stored upcased (VMS logical names are case-insensitive;
 * the writer upcases, so readers compare upcased). Offsets, never pointers:
 * the arena sits at a different virtual address in every process, so
 * nothing here may be a pointer.
 */
struct vms_lnm_entry {
    uint32_t in_use;                    /* 0 = free slot */
    uint32_t table;                     /* VMS_LNM_TBL_* */
    /*
     * Scope key. SYSTEM is singular (key 0). GROUP is per-UIC-group; JOB is
     * per-job-tree. The key is carried from day one so adding GROUP/JOB is a
     * data change, not a flag-day (design §3.2). It is DERIVED IN THE KERNEL
     * from the caller's PCB, never supplied by the caller (design §3.3).
     */
    uint32_t scope_key;
    uint32_t attributes;                /* LNM_ATTR_* */
    uint8_t  acmode;
    uint8_t  num_equiv;
    uint16_t name_length;
    uint32_t pad;
    char     name[VMS_LNM_MAX_NAME + 1];
    struct vms_lnm_equiv equiv[VMS_LNM_MAX_EQUIV];
};

/*
 * The arena. A fixed-size table plus a seqlock generation counter in the
 * header. The executive is the ONLY writer; readers take no lock -- they
 * sample `generation`, read, re-sample, and retry on mismatch (writes are
 * rare, so retries are effectively never). The executive makes `generation`
 * odd before mutating and even after, so a reader that samples an odd value
 * or a changed value retries and never observes a torn entry.
 */
struct vms_lnm_arena {
    uint32_t magic;                     /* VMS_LNM_ARENA_MAGIC */
    uint32_t version;                   /* VMS_LNM_ARENA_VERSION */
    uint32_t arena_size;                /* sizeof(struct vms_lnm_arena) */
    uint32_t max_entries;               /* VMS_LNM_MAX_ENTRIES */
    uint32_t entry_count;               /* hint: entries currently in use */
    uint32_t reserved;
    uint64_t generation;                /* seqlock; odd while a write is in flight */
    struct vms_lnm_entry entries[VMS_LNM_MAX_ENTRIES];
};

/* ---- mutation ioctls (design §3.3) --------------------------------- */

/* Create or supersede a logical name in an executive-resident table. */
struct vms_lnm_def_args {
    uint32_t table;                     /* VMS_LNM_TBL_* */
    uint32_t attributes;                /* LNM_ATTR_* */
    uint32_t status;                    /* return: SS$_ status */
    uint8_t  acmode;
    uint8_t  num_equiv;                 /* 1..VMS_LNM_MAX_EQUIV */
    uint16_t name_length;
    char     name[VMS_LNM_MAX_NAME + 1];
    struct vms_lnm_equiv equiv[VMS_LNM_MAX_EQUIV];
};

/* Delete a logical name from an executive-resident table. */
struct vms_lnm_del_args {
    uint32_t table;                     /* VMS_LNM_TBL_* */
    uint32_t status;                    /* return: SS$_ status */
    uint8_t  acmode;
    uint8_t  pad1;
    uint16_t name_length;
    char     name[VMS_LNM_MAX_NAME + 1];
};

#define VMS_IOCTL_LNM_DEFINE  _IOWR(VMS_IOC_MAGIC, 0x60, struct vms_lnm_def_args)
#define VMS_IOCTL_LNM_DELETE  _IOWR(VMS_IOC_MAGIC, 0x61, struct vms_lnm_del_args)

/*
 * VMS_IOCTL_LNM_GETSCOPE - hand the CALLER its own GROUP and JOB scope keys
 * (vms-aba). The read-only mmap arena is what keeps translation a zero-
 * syscall operation (design "C-corrected", docs/design-logical-name-
 * placement.md), but a reader still has to know WHICH scope_key belongs to
 * it to filter the arena locally -- and that key is derived facts the
 * kernel holds (proc->uic >> 16, proc->job_id), never something the caller
 * may assert (see derive_scope_key() in vms_lnm.c, the ONLY place a
 * mutation's scope is computed). This ioctl is the read path's equivalent:
 * it returns the same two derived values so vms_kif can cache them once per
 * process (exactly like the arena mapping itself) and filter entries
 * locally on every translate without a further round trip.
 */
struct vms_lnm_scope_args {
    uint32_t group_key;                 /* this caller's LNM$GROUP scope key */
    uint32_t job_key;                   /* this caller's LNM$JOB scope key */
    uint32_t status;                    /* return: SS$_ status */
};

#define VMS_IOCTL_LNM_GETSCOPE \
                              _IOWR(VMS_IOC_MAGIC, 0x62, struct vms_lnm_scope_args)

/*
 * Freeze the shared layouts. Every field is a fixed-width type, so aarch64
 * and x86_64 agree; a size change also renumbers the _IOWR request, so a
 * mismatched build fails here rather than mis-decoding at runtime.
 */
_Static_assert(sizeof(struct vms_lnm_equiv) == 260,
               "vms_lnm_equiv changed size -- arena/ioctl ABI break");
_Static_assert(sizeof(struct vms_lnm_entry) == 2360,
               "vms_lnm_entry changed size -- arena mmap ABI break");
_Static_assert(sizeof(struct vms_lnm_def_args) == 2352,
               "vms_lnm_def_args changed size -- VMS_IOCTL_LNM_DEFINE ABI break");
_Static_assert(sizeof(struct vms_lnm_del_args) == 268,
               "vms_lnm_del_args changed size -- VMS_IOCTL_LNM_DELETE ABI break");
_Static_assert(sizeof(struct vms_lnm_scope_args) == 12,
               "vms_lnm_scope_args changed size -- VMS_IOCTL_LNM_GETSCOPE ABI break");

#endif /* _VMS_LNM_H */
