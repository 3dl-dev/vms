/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_lnm_nb.h - the shared /dev/vms LOGICAL-NAME contract for the OVMX/NetBSD
 * substrate (rd vms-72da, epic vms-8e8; docs/design-ovmx-netbsd-syskrnl.md,
 * docs/design-netbsd-executive-core.md, docs/design-logical-name-placement.md).
 *
 * This is the NetBSD twin of the LOGICAL-NAME half of src/kernel/vms_lnm.h,
 * exactly as vms_eflag_nb.h / vms_proctab_nb.h twin their facilities: it carries
 * the argument + arena structs the SAME shared facility source
 * (src/kernel-core/vms_lnm.c) copies in and out on BOTH substrates, plus the
 * ioctl request numbers, byte-for-byte identical to vms_lnm.h. Because the
 * numbers are _IOWR carrying the same structs and the same NR bytes as
 * vms_lnm.h, the request NUMBERS are identical across substrates -- so a
 * PROVISION.EXE built against the Linux vms_ioctl.h (via libvmssys' vms_kif.h)
 * issues exactly the numbers this NetBSD kernel dispatches on.
 *
 * lnm is the LAST executive facility to join the NetBSD `vms' module's SRCS
 * (vms-d61 left the arena seam contract-only, following the exec_blockdev
 * precedent; vms-72da binds it). It is what makes PROVISION's STARTUP phase able
 * to CREATE the system logical names (SYS$STARTUP / SYS$LOGIN / SYS$UPDATE) and
 * TRANSLATE them, on NetBSD/vax, so the boot proceeds past %DCL-E-LNMFAIL toward
 * Username:.
 *
 * THE COPY MODEL: _IOWR + the framework owns the user boundary -- identical to
 * event flags / proctab. NetBSD's generic cdevsw ioctl path PRE-COPIES an _IOWR
 * argument into a kernel buffer, hands the driver that buffer, and copies the
 * driver's answer back out. The driver passes that kernel buffer straight to the
 * shared facility; its exec_copyin/exec_copyout are in-kernel copies on the
 * NetBSD backend. This works for LNM because every LNM ioctl argument is <= one
 * page (IOCPARM_MAX == NBPG): vms_lnm_def_args is 2352 bytes and NetBSD/vax's
 * NBPG is 4096 (the VAX 512-byte hardware page is clustered 8:1 into a 4096-byte
 * software page, arch/vax/include/param.h PGSHIFT==12), so no IOC_VOID big-io
 * shape is needed (unlike the mailbox WRITE/READ transfer ops, whose 4112/4116-
 * byte buffers exceed one page).
 *
 * THE READ PATH IS A read-only mmap, NOT an ioctl. Userspace TRANSLATES by
 * reading a read-only mmap of the arena below (no syscall on the hot path); only
 * MUTATION (define/delete/getscope) is an ioctl. On NetBSD the char device's
 * d_mmap publishes the arena's wired pages read-only (vms_netbsd.c vms_mmap);
 * the arena is allocated by the shared facility through the exec_arena seam
 * (exec_kbackend.h S10 / exec_kbackend_netbsd.h uvm_km_alloc(UVM_KMF_WIRED)).
 *
 * OVMX DESIGN CHOICE (CLAUDE.md Rule 8), same as vms_lnm.h: the BYTE-LEVEL
 * arena/ioctl LAYOUT below is OVMX's own. Public OpenVMS documentation describes
 * logical-name BEHAVIOUR (the table hierarchy, the LNM$FILE_DEV search order,
 * the attributes, the privileges) and that is what OVMX reproduces; it does NOT
 * publish the executive's logical-name database layout. This format is never to
 * be presented as VMS-authentic. No NetBSD or VSI source is copied.
 */

#ifndef _VMS_LNM_NB_H
#define _VMS_LNM_NB_H

/* Fixed-width types: NetBSD kernel via <sys/types.h>; userspace via <stdint.h>.
 * Same split as vms_eflag_nb.h / vms_ping.h. */
#if defined(_KERNEL)
#include <sys/types.h>
#else
#include <stdint.h>
#endif

/* Prefer the substrate's own _IO* macros (identical dance to vms_eflag_nb.h). On
 * NetBSD they come from <sys/ioccom.h> (kernel) or <sys/ioctl.h> (userspace);
 * the fallback matches vms_ioctl.h's Linux-style encoding, which for these
 * structs yields the identical request number. */
#if !defined(_IOWR)
# if defined(__NetBSD__)
#  if defined(_KERNEL)
#   include <sys/ioccom.h>
#  else
#   include <sys/ioctl.h>
#  endif
# else
#  define _IOWR(type, nr, size) \
        (((3U) << 30) | ((sizeof(size)) << 16) | ((type) << 8) | (nr))
# endif
#endif

/* Same magic byte as src/kernel/vms_ioctl.h (VMS_IOC_MAGIC 'V') and the other
 * _nb twins: one /dev/vms contract, one magic space. */
#define VMS_LNM_IOC_MAGIC 'V'

/* ================================================================
 * Table ids and sizing -- OVMX design choices (Rule 8), byte-identical to
 * src/kernel/vms_lnm.h. LNM$PROCESS is deliberately absent: it is per-process
 * and never leaves the process, so it never reaches vms.ko.
 * ================================================================ */
#define VMS_LNM_TBL_SYSTEM  1
#define VMS_LNM_TBL_GROUP   2
#define VMS_LNM_TBL_JOB     3

#define VMS_LNM_MAX_NAME    255     /* == LNM_MAX_NAME (vms/logical.h) */
#define VMS_LNM_MAX_VALUE   255     /* == LNM_MAX_VALUE */
#define VMS_LNM_MAX_EQUIV   8       /* inline equivalence strings per entry */
#define VMS_LNM_MAX_ENTRIES 512     /* fixed arena capacity */

#define VMS_LNM_ARENA_MAGIC   0x4C4E4D41u  /* 'LNMA' */
#define VMS_LNM_ARENA_VERSION 1u

/* The arena maps at this offset on the /dev/vms fd. */
#define VMS_LNM_MMAP_OFFSET   0u

/* ================================================================
 * Privilege bits lnm_priv_check() (src/kernel-core/vms_lnm.c) gates DEFINE and
 * DELETE on -- the subset of vms_ioctl.h's oracle-pinned $PRVDEF table the LNM
 * facility consults (SYSNAM/SYSPRV for LNM$SYSTEM; GRPNAM/GRPPRV/SYSPRV for
 * LNM$GROUP; LNM$JOB needs none). Bit positions match vms_ioctl.h exactly.
 * Guarded so a TU that also pulls another twin defining a bit does not redefine
 * it (the exec_hash/exec_list #ifndef precedent, and vms_proctab_nb.h's WORLD).
 * ================================================================ */
#ifndef VMS_PRV_V_SYSNAM
#define VMS_PRV_V_SYSNAM     2
#endif
#ifndef VMS_PRV_V_GRPNAM
#define VMS_PRV_V_GRPNAM     3
#endif
#ifndef VMS_PRV_V_SYSPRV
#define VMS_PRV_V_SYSPRV    28
#endif
#ifndef VMS_PRV_V_GRPPRV
#define VMS_PRV_V_GRPPRV    34
#endif
#ifndef VMS_PRV_M_SYSNAM
#define VMS_PRV_M_SYSNAM    (1ULL << VMS_PRV_V_SYSNAM)
#endif
#ifndef VMS_PRV_M_GRPNAM
#define VMS_PRV_M_GRPNAM    (1ULL << VMS_PRV_V_GRPNAM)
#endif
#ifndef VMS_PRV_M_SYSPRV
#define VMS_PRV_M_SYSPRV    (1ULL << VMS_PRV_V_SYSPRV)
#endif
#ifndef VMS_PRV_M_GRPPRV
#define VMS_PRV_M_GRPPRV    (1ULL << VMS_PRV_V_GRPPRV)
#endif

/* ================================================================
 * Arena + argument structs -- byte-identical to src/kernel/vms_lnm.h. The shared
 * facility (src/kernel-core/vms_lnm.c) copies exactly these in and out, and the
 * arena is what the char device's mmap publishes read-only. Offsets, never
 * pointers: the arena sits at a different virtual address in every process.
 * ================================================================ */

/* One equivalence string of a (possibly multi-valued) logical name. */
struct vms_lnm_equiv {
	uint16_t length;
	uint8_t  index;                     /* equivalence index (0-based) */
	uint8_t  pad;
	char     value[VMS_LNM_MAX_VALUE + 1];
};

/* One logical name. Stored upcased (VMS logical names are case-insensitive). */
struct vms_lnm_entry {
	uint32_t in_use;                    /* 0 = free slot */
	uint32_t table;                     /* VMS_LNM_TBL_* */
	uint32_t scope_key;                 /* SYSTEM=0; GROUP=UIC group; JOB=job tree */
	uint32_t attributes;                /* LNM_ATTR_* */
	uint8_t  acmode;
	uint8_t  num_equiv;
	uint16_t name_length;
	uint32_t pad;
	char     name[VMS_LNM_MAX_NAME + 1];
	struct vms_lnm_equiv equiv[VMS_LNM_MAX_EQUIV];
};

/* The arena: a fixed-size table plus a seqlock generation counter. The executive
 * is the ONLY writer; readers (userspace, over the mmap) take no lock. */
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

/* ---- mutation ioctls ----------------------------------------------- */

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

/* Hand the caller its own derived GROUP and JOB scope keys (the read path needs
 * them to filter the mmap'd arena locally without a further round trip). */
struct vms_lnm_scope_args {
	uint32_t group_key;                 /* this caller's LNM$GROUP scope key */
	uint32_t job_key;                   /* this caller's LNM$JOB scope key */
	uint32_t status;                    /* return: SS$_ status */
};

#define VMS_IOCTL_LNM_DEFINE   _IOWR(VMS_LNM_IOC_MAGIC, 0x60, struct vms_lnm_def_args)
#define VMS_IOCTL_LNM_DELETE   _IOWR(VMS_LNM_IOC_MAGIC, 0x61, struct vms_lnm_del_args)
#define VMS_IOCTL_LNM_GETSCOPE _IOWR(VMS_LNM_IOC_MAGIC, 0x62, struct vms_lnm_scope_args)

/*
 * Freeze the shared layouts -- see src/kernel/vms_lnm.h's identical asserts.
 * Every field is a fixed-width type, so elf32-vax (ILP32) and LP64 agree; a size
 * change also renumbers the _IOWR request, so a mismatched build fails HERE
 * rather than mis-decoding at runtime.
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

#endif /* _VMS_LNM_NB_H */
