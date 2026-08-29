/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_proctab_nb.h - the shared /dev/vms PROCESS-TABLE contract for the
 * OVMX/NetBSD substrate (rd vms-ca7, parent vms-9dc, epic vms-8e8;
 * docs/design-ovmx-netbsd-syskrnl.md, docs/design-netbsd-executive-core.md §11).
 *
 * P4-A (vms-ca7) compiles the SAME executive-resident process-table facility --
 * src/kernel-core/vms_proctab.c -- into the NetBSD `vms' pseudo-device that it
 * compiles into the Linux vms.ko, exactly as P2c did for event flags
 * (vms_eflag_nb.h) and P4-A did for ASTs / access modes / mailboxes. The process
 * table is the FIRST facility to bind the host TASK: $GETJPI / SHOW SYSTEM read a
 * process's identity and real accounting out of module-global KERNEL memory, and
 * $SETPRN records a name there where every OTHER process can resolve it -- the
 * INV-6-decisive property (CLAUDE.md Rule 9). A per-process userspace fake could
 * report a name only its own process could see; this cannot, because the name
 * lives in the one executive process database the whole system shares.
 *
 * This header is the ONE wire contract for that facility on NetBSD, included
 * IDENTICALLY by every side of the /dev/vms boundary:
 *   - the in-kernel `vms' pseudo-device      (src/kernel-netbsd/vms_netbsd.c),
 *   - the shared facility itself, via the NetBSD struct twin
 *     (src/kernel-netbsd/vms_internal.h, which includes this file for the arg
 *     structs the facility copies in/out and for the sizing constants), and
 *   - the userspace test program that reaches it through the transport seam.
 *
 * ONE FACILITY SOURCE, TWO SUBSTRATES. The argument STRUCTS below are the SAME
 * structs the Linux executive's ioctl surface uses (src/kernel/vms_ioctl.h) --
 * byte-for-byte identical layouts (the _Static_asserts here freeze them, the
 * same sizes the Linux header freezes) -- because ONE facility source
 * (src/kernel-core/vms_proctab.c) copies them in and out on BOTH substrates.
 * Every process-table ioctl is _IOWR and <= 288 bytes (getjpi_args, the
 * largest), well under NetBSD's one-page IOCPARM_MAX, so all of them ride the
 * cdevsw framework's pre-copy path -- no IOC_VOID big-io shape is needed (unlike
 * the mailbox WRITE/READ transfer ops, vms_mbx_nb.h).
 *
 * CLEAN ROOM (CLAUDE.md Rule 8). The process-table SEMANTICS are the publicly
 * documented OpenVMS $GETJPI / $SETPRN / $PROCESS_SCAN / $HIBER / $WAKE behaviour
 * (System Services Reference); the argument layouts are OVMX's own, shared with
 * the Linux executive (src/kernel/vms_ioctl.h), and the who-may-read rule they
 * gate on is oracle-pinned in docs/oracle/vax73-privileges.md. No NetBSD or VSI
 * source is copied.
 */

#ifndef _VMS_PROCTAB_NB_H
#define _VMS_PROCTAB_NB_H

/* Fixed-width types: NetBSD kernel via <sys/types.h>; userspace via <stdint.h>.
 * Same split as vms_ping.h / vms_eflag_nb.h / vms_mbx_nb.h. */
#if defined(_KERNEL)
#include <sys/types.h>
#else
#include <stdint.h>
#endif

/* Prefer the substrate's own _IOWR (identical dance to vms_mbx_nb.h). Every
 * process-table ioctl is _IOWR -- no _IO/IOC_VOID op here. */
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

/* Same magic byte as src/kernel/vms_ioctl.h (VMS_IOC_MAGIC 'V'): one /dev/vms
 * contract, one magic space. */
#define VMS_PROCTAB_IOC_MAGIC 'V'

/* ================================================================
 * Field-width constants -- byte-identical to src/kernel/vms_ioctl.h. Guarded so
 * this header composes with any other /dev/vms contract header that also defines
 * one (vms_mbx_nb.h already defines VMS_DEVNAM_SIZE).
 * ================================================================ */
#ifndef VMS_PRCNAM_SIZE
#define VMS_PRCNAM_SIZE   16     /* process-name field width */
#endif
#ifndef VMS_PRCNAM_XFER
#define VMS_PRCNAM_XFER   64     /* inbound name transfer buffer (> _SIZE, so an
                                  * oversized name reaches the executive
                                  * untruncated -- see vms_ioctl.h) */
#endif
#ifndef VMS_USERNAME_SIZE
#define VMS_USERNAME_SIZE 32     /* SYSUAF-key user-name width */
#endif
#ifndef VMS_DEVNAM_SIZE
#define VMS_DEVNAM_SIZE   16     /* device-name width (also in vms_mbx_nb.h) */
#endif
#ifndef VMS_CLI_CMDLINE_SIZE
#define VMS_CLI_CMDLINE_SIZE 256 /* invoking DCL command-line bound (vms-f60d) */
#endif

/* ================================================================
 * VMS status codes the process-table facility returns that are NOT already in
 * the NetBSD struct twin's shared subset (SS__NORMAL/BADPARAM/NOPRIV are there).
 * Values match src/kernel/vms_internal.h exactly (ssdef.h SS$_ codes). Guarded.
 * ================================================================ */
#ifndef SS__DUPLNAM
#define SS__DUPLNAM   148        /* SS$_DUPLNAM  -- duplicate process name */
#endif
#ifndef SS__NONEXPR
#define SS__NONEXPR   2280       /* SS$_NONEXPR  -- nonexistent process */
#endif
#ifndef SS__IVLOGNAM
#define SS__IVLOGNAM  340        /* SS$_IVLOGNAM -- invalid name string */
#endif

/* ================================================================
 * Privilege bits the process table gates on. VMS_PRV_M_SETPRV comes from
 * vms_access_nb.h (bit 14); WORLD is added here (the cross-UIC-group read gate,
 * oracle-pinned in vms_proc_may_read). Bit positions match src/kernel/vms_ioctl.h.
 * The SYSTEM identity constants back vms_ioctl_establish_system.
 * ================================================================ */
#ifndef VMS_PRV_V_WORLD
#define VMS_PRV_V_WORLD   16
#endif
#ifndef VMS_PRV_M_WORLD
#define VMS_PRV_M_WORLD   (1ULL << VMS_PRV_V_WORLD)   /* PRV$M_WORLD */
#endif
#ifndef VMS_SYSTEM_UIC
#define VMS_SYSTEM_UIC    ((1u << 16) | 4u)           /* [1,4] */
#endif
#ifndef VMS_PRV_M_SYSTEM_ALL
#define VMS_PRV_M_SYSTEM_ALL  (~(uint64_t)0)          /* PRV$M_ALL */
#endif

/* ================================================================
 * $GETJPI target selector (VMS_JPI_SEL_*) and the fields_valid bitmask
 * (VMS_PI_V_*). Byte-identical to src/kernel/vms_ioctl.h.
 * ================================================================ */
#define VMS_JPI_SEL_SELF    0   /* the calling process */
#define VMS_JPI_SEL_PID     1   /* by vms_pid */
#define VMS_JPI_SEL_PRCNAM  2   /* by prcnam, within the caller's UIC group */
#define VMS_JPI_SEL_LINUX_PID 3 /* GETEXIT only: by backing Linux pid (vms-707) */

#define VMS_PI_V_CPUTIM     0x00000001u  /* cputim is sourced */
#define VMS_PI_V_PAGEFLTS   0x00000002u  /* pageflts is sourced */
#define VMS_PI_V_PAGES      0x00000004u  /* pages (resident) is sourced */
#define VMS_PI_V_LOGINTIM   0x00000008u  /* logintim is sourced */
#define VMS_PI_V_STATE      0x00000010u  /* sched_state is sourced */
#define VMS_PI_V_PRI        0x00000020u  /* cur_pri is sourced */
#define VMS_PI_V_PRIB       0x00000040u  /* base_pri is sourced */
#define VMS_PI_V_DIRIO      0x00000080u  /* dirio is sourced */
#define VMS_PI_V_BUFIO      0x00000100u  /* bufio is sourced */
#define VMS_PI_V_QUOTA      0x00000200u  /* quota block is sourced */

/* proc_type classification (vms-c17). Byte-identical to src/kernel/vms_ioctl.h;
 * see there for the full rationale. BATCH is reserved and never set today (no
 * batch execution engine). On NetBSD the PCB's job_id is 0 until the job glue
 * joins (vms_internal.h), so proc_fill_info() falls back to per-row terminal
 * classification there -- honest, never fabricated. */
#define VMS_PROC_T_OTHER        0u  /* detached / system process (not a "user") */
#define VMS_PROC_T_INTERACTIVE  1u  /* job root with a terminal (login) */
#define VMS_PROC_T_SUBPROCESS   2u  /* belongs to a parent's job (SPAWN) */
#define VMS_PROC_T_BATCH        3u  /* batch job root (reserved -- no engine yet) */

/* ================================================================
 * Argument structs -- byte-identical to src/kernel/vms_ioctl.h. The shared
 * facility (src/kernel-core/vms_proctab.c) copies exactly these in and out.
 * See vms_ioctl.h for the per-field design rationale (redaction, oracle pins).
 * ================================================================ */

/* Per-process JIB quota block -- STRUCTURAL (carried with VMS_PI_V_QUOTA clear;
 * OVMX has no quota facility yet). Byte-identical to src/kernel/vms_ioctl.h. */
struct vms_jib_quota {
	uint32_t astlm;
	uint32_t biolm;
	uint32_t bytlm;
	uint32_t diolm;
	uint32_t enqlm;
	uint32_t fillm;
	uint32_t pgflquota;
	uint32_t prclm;
	uint32_t tqelm;
	uint32_t wsdefault;
	uint32_t wsquota;
	uint32_t wsextent;
};

/* One process-table row. `redacted` says whether the identity fields were
 * withheld (a row the caller may not $GETJPI); `fields_valid` says which
 * accounting/quota fields carry a measured value. */
struct vms_procinfo {
	uint32_t vms_pid;
	uint32_t linux_pid;
	char     prcnam[VMS_PRCNAM_SIZE];
	uint32_t uic;
	uint8_t  current_mode;
	uint8_t  redacted;
	uint8_t  proc_type;   /* VMS_PROC_T_* -- process classification (vms-c17) */
	uint8_t  pad[1];
	uint64_t cur_privs;
	uint64_t perm_privs;
	char     username[VMS_USERNAME_SIZE];
	char     terminal[VMS_DEVNAM_SIZE];
	uint64_t p0_base;
	uint64_t p0_limit;
	uint64_t p1_base;
	uint64_t p1_limit;
	uint32_t fields_valid;
	uint8_t  sched_state;
	uint8_t  base_pri;
	uint8_t  cur_pri;
	uint8_t  acct_pad;
	uint32_t dirio;
	uint32_t bufio;
	uint32_t pageflts;
	uint32_t pages;
	uint32_t cputim;
	uint32_t quad_pad;
	uint64_t logintim;
	struct vms_jib_quota quota;
};

struct vms_getjpi_args {
	uint32_t select;                    /* VMS_JPI_SEL_* */
	uint32_t status;                    /* return: SS$_ status */
	struct vms_procinfo info;           /* in: vms_pid selector; out: the row */
	char     sel_prcnam[VMS_PRCNAM_XFER]; /* in: name selector, untruncated */
};

/* System memory statistics ($GETSYI-style; SHOW MEMORY physical section,
 * vms-a3cd). MUST match src/kernel/vms_ioctl.h byte-for-byte. Bytes cross the
 * wire (arch-neutral); no per-process identity. See vms_ioctl.h for the field
 * rationale and the INV-6 honest-omission contract. */
struct vms_syi_meminfo {
	uint64_t total_bytes;    /* out: total managed physical memory (bytes) */
	uint64_t free_bytes;     /* out: current free memory (bytes) */
	uint32_t fields_valid;   /* out: VMS_SYIMEM_V_* -- which figures are real */
	uint32_t reserved;       /* must be zero */
};

#define VMS_SYIMEM_V_PHYS 0x00000001u  /* total_bytes/free_bytes are measured */

struct vms_getsyi_mem_args {
	struct vms_syi_meminfo info;   /* out: the memory figures */
	uint32_t status;               /* return: SS$_ status */
	uint32_t reserved;             /* must be zero */
};

struct vms_procscan_args {
	uint32_t index;                     /* in: cursor; out: next cursor */
	uint32_t status;                    /* return: SS$_ status */
	struct vms_procinfo info;           /* out: the row at the incoming cursor */
};

struct vms_setprn_args {
	char     prcnam[VMS_PRCNAM_XFER];   /* new process name, untruncated */
	uint32_t status;                    /* return: SS$_ status */
	uint32_t pad;
};

/* Byte-for-byte the same as src/kernel/vms_ioctl.h. The JIB quota block rides
 * SETIDENT (vms-14a): identity and the authorized quota set arrive together
 * from the SYSUAF record LOGINOUT authenticated. Growing this struct moves
 * VMS_IOCTL_SETIDENT's request number deliberately (the shared
 * kernel-core/vms_proctab.c reads args.quota_valid/args.quota). */
struct vms_ident_args {
	char     username[VMS_USERNAME_SIZE]; /* authenticated user name */
	uint32_t uic;                         /* (group << 16) | member */
	uint32_t status;                      /* return: SS$_ status */
	uint64_t authorized_privs;            /* SYSUAF uaf$q_priv */
	uint32_t quota_valid;                 /* 1 = quota below is sourced */
	uint32_t quota_pad;                   /* keep the quota block size stable */
	struct vms_jib_quota quota;           /* authorized JIB quota set (SYSUAF) */
};

struct vms_establish_system_args {
	uint32_t status;      /* return: SS$_ status */
	uint32_t pad;
};

struct vms_hiber_args {
	uint32_t woken;       /* return: 1 = released by $WAKE, 0 = by an AST */
	uint32_t status;      /* return: SS$_ status */
};

struct vms_wake_args {
	uint32_t vms_pid;     /* target VMS PID; 0 = the calling process */
	uint32_t status;      /* return: SS$_ status */
};

/*
 * $EXIT/$STATUS + CLI invocation context (vms-f60d). Byte-for-byte the same
 * structs as src/kernel/vms_ioctl.h -- the one shared facility source
 * (src/kernel-core/vms_proctab.c) copies exactly these in and out on both
 * substrates. See src/kernel/vms_ioctl.h for the full field semantics.
 */
struct vms_exit_args {
	uint32_t condition;   /* in:  VMS condition value to record as $STATUS */
	uint32_t status;      /* out: SS$_ status of the record operation */
	uint32_t exit_code;   /* out: OVMX POSIX exit code mapped from condition */
	uint8_t  success;     /* out: bit<0> of condition (STS$M_SUCCESS) */
	uint8_t  severity;    /* out: bits<2:0> of condition (STS$V_SEVERITY) */
	uint8_t  pad[2];
};

struct vms_getexit_args {
	uint32_t select;      /* in:  VMS_JPI_SEL_SELF or VMS_JPI_SEL_PID */
	uint32_t vms_pid;     /* in:  target VMS PID when select == SEL_PID */
	uint32_t condition;   /* out: recorded $STATUS condition value */
	uint32_t status;      /* out: SS$_ status of the read */
	uint8_t  has_exited;  /* out: 1 iff an image completion status exists */
	uint8_t  success;     /* out: bit<0> of condition (STS$M_SUCCESS) */
	uint8_t  severity;    /* out: bits<2:0> of condition (STS$V_SEVERITY) */
	uint8_t  pad;
};

struct vms_setcli_args {
	uint8_t  cliflag;     /* in:  1 = invoked from a CLI/DCL */
	uint8_t  pad;
	uint16_t length;      /* in:  command-line length in bytes */
	uint32_t status;      /* out: SS$_ status */
	char     command[VMS_CLI_CMDLINE_SIZE];  /* in:  invoking DCL command line */
};

struct vms_getcli_args {
	uint8_t  cliflag;     /* out: 1 = invoked from a CLI/DCL */
	uint8_t  pad;
	uint16_t length;      /* out: command-line length in bytes */
	uint32_t status;      /* out: SS$_ status */
	char     command[VMS_CLI_CMDLINE_SIZE];  /* out: invoking DCL command line */
};

/* ================================================================
 * Request numbers. All _IOWR carrying the SAME structs and NR bytes as
 * src/kernel/vms_ioctl.h, so their numbers are identical across substrates (the
 * framework pre-copy path -- no IOC_VOID op here). SETTERM (0x45) is deliberately
 * ABSENT: it needs the device table (channel -> terminal), which is not in this
 * module's SRCS, so its facility handler is not linked here and the row's
 * `terminal` field stays "" -- honestly empty, not fabricated (INV-6).
 * ================================================================ */
/*
 * VMS_IOCTL_REGISTER / _REGISTER_CONTINUE (vms-329). The /dev/vms contract's
 * "adopt-or-create this process's PCB" op. It was MISSING from this substrate
 * because every NetBSD ioctl path find-or-creates the PCB implicitly
 * (vms_proc_get, vms_netbsd.c) -- so nothing the kernel itself needed ever
 * issued it. But the SHARED userspace ACP client does: imgact_acp.c (the file-
 * access walk IMGACT.EXE, RMS and PID 1's boot bridge all run) opens with
 * acp_register() and treats its failure as fatal, so on the VAX every ACP file
 * open returned SS$_NOSUCHDEV before it ever touched the disk -- observed as
 * the boot mounting SYS$DISK over the ACP and then declaring the volume "not an
 * installed OVMX system volume". Answering it here is not new policy: it hands
 * back the PCB vms_proc_get would have created anyway, which is exactly what
 * the Linux twin's register does for an existing process (src/kernel/
 * vms_module.c: "register a process that already exists -> hand back the
 * process that already exists").
 *
 * NR 0x40/0x41 with the 8-byte struct, so the command word is identical to the
 * Linux build's (asserted below). Note 0x41 is shared with SETPRN by NR alone;
 * the encoded size differs (8 vs 72), so the command words do not collide --
 * the same overlap src/kernel/vms_ioctl.h already carries.
 */
struct vms_register_args {
	uint32_t vms_pid;     /* return: the VMS process ID assigned/adopted */
	uint32_t status;      /* return: SS$_ status */
};

#define VMS_IOCTL_REGISTER          _IOWR(VMS_PROCTAB_IOC_MAGIC, 0x40, struct vms_register_args)
#define VMS_IOCTL_REGISTER_CONTINUE _IOWR(VMS_PROCTAB_IOC_MAGIC, 0x41, struct vms_register_args)

/*
 * VMS_IOCTL_DASSGN (vms-329) -- $DASSGN, release one assigned channel. Also
 * missing here, and also needed by the shared ACP client: imgact_acp.c and the
 * RMS ACP arm $DASSGN every channel they take, and PID 1's boot bridge stages
 * ~20 images per boot, so an unanswered $DASSGN leaks a file-class channel per
 * open.
 *
 * HONEST SCOPE. The Linux twin (vms_devtab.c) tries the DEVICE channel table
 * first, then falls back to mailbox and file-class channels. This substrate has
 * no device table in SRCS, so only the mailbox and file-class fallbacks exist
 * here; a channel that is neither reports SS$_IVCHAN rather than a fabricated
 * success (INV-6).
 */
struct vms_dassgn_args {
	uint32_t chan;
	uint32_t status;      /* return: SS$_ status */
};

#define VMS_IOCTL_DASSGN            _IOWR(VMS_PROCTAB_IOC_MAGIC, 0x51, struct vms_dassgn_args)

#define VMS_IOCTL_SETPRN            _IOWR(VMS_PROCTAB_IOC_MAGIC, 0x41, struct vms_setprn_args)
#define VMS_IOCTL_GETJPI            _IOWR(VMS_PROCTAB_IOC_MAGIC, 0x42, struct vms_getjpi_args)
#define VMS_IOCTL_PROCSCAN          _IOWR(VMS_PROCTAB_IOC_MAGIC, 0x43, struct vms_procscan_args)
#define VMS_IOCTL_SETIDENT          _IOWR(VMS_PROCTAB_IOC_MAGIC, 0x44, struct vms_ident_args)
#define VMS_IOCTL_ESTABLISH_SYSTEM  _IOWR(VMS_PROCTAB_IOC_MAGIC, 0x46, struct vms_establish_system_args)
#define VMS_IOCTL_HIBER             _IOWR(VMS_PROCTAB_IOC_MAGIC, 0x47, struct vms_hiber_args)
#define VMS_IOCTL_WAKE              _IOWR(VMS_PROCTAB_IOC_MAGIC, 0x48, struct vms_wake_args)
#define VMS_IOCTL_SETEXIT           _IOWR(VMS_PROCTAB_IOC_MAGIC, 0x49, struct vms_exit_args)
#define VMS_IOCTL_GETEXIT           _IOWR(VMS_PROCTAB_IOC_MAGIC, 0x4A, struct vms_getexit_args)
#define VMS_IOCTL_SETCLI            _IOWR(VMS_PROCTAB_IOC_MAGIC, 0x4B, struct vms_setcli_args)
#define VMS_IOCTL_GETCLI            _IOWR(VMS_PROCTAB_IOC_MAGIC, 0x4C, struct vms_getcli_args)
/* System-info facility ($GETSYI-style; SHOW MEMORY physical section, vms-a3cd) */
#define VMS_IOCTL_GETSYIMEM         _IOWR(VMS_PROCTAB_IOC_MAGIC, 0x68, struct vms_getsyi_mem_args)

/*
 * Freeze the shared layouts -- the SAME sizes src/kernel/vms_ioctl.h freezes.
 * Both sides of /dev/vms compile these structs separately and pass them by raw
 * address, so a size drift is an ABI break. These MUST match vms_ioctl.h.
 */
_Static_assert(sizeof(struct vms_procinfo) == 216,
               "vms_procinfo layout changed: process-table ioctl ABI break");
_Static_assert(sizeof(struct vms_getjpi_args) == 288,
               "vms_getjpi_args layout changed: VMS_IOCTL_GETJPI ABI break");
_Static_assert(sizeof(struct vms_getsyi_mem_args) == 32,
               "vms_getsyi_mem_args layout changed: VMS_IOCTL_GETSYIMEM ABI break");
_Static_assert(sizeof(struct vms_procscan_args) == 224,
               "vms_procscan_args layout changed: VMS_IOCTL_PROCSCAN ABI break");
_Static_assert(sizeof(struct vms_setprn_args) == 72,
               "vms_setprn_args layout changed: VMS_IOCTL_SETPRN ABI break");
_Static_assert(sizeof(struct vms_ident_args) == 104,
               "vms_ident_args layout changed: VMS_IOCTL_SETIDENT ABI break");
_Static_assert(sizeof(struct vms_establish_system_args) == 8,
               "vms_establish_system_args layout changed: VMS_IOCTL_ESTABLISH_SYSTEM ABI break");
_Static_assert(sizeof(struct vms_hiber_args) == 8,
               "vms_hiber_args layout changed: VMS_IOCTL_HIBER ABI break");
_Static_assert(sizeof(struct vms_wake_args) == 8,
               "vms_wake_args layout changed: VMS_IOCTL_WAKE ABI break");
_Static_assert(sizeof(struct vms_exit_args) == 16,
               "vms_exit_args layout changed: VMS_IOCTL_SETEXIT ABI break");
_Static_assert(sizeof(struct vms_getexit_args) == 20,
               "vms_getexit_args layout changed: VMS_IOCTL_GETEXIT ABI break");
_Static_assert(sizeof(struct vms_setcli_args) == 8 + VMS_CLI_CMDLINE_SIZE,
               "vms_setcli_args layout changed: VMS_IOCTL_SETCLI ABI break");
_Static_assert(sizeof(struct vms_getcli_args) == 8 + VMS_CLI_CMDLINE_SIZE,
               "vms_getcli_args layout changed: VMS_IOCTL_GETCLI ABI break");
_Static_assert(VMS_PRCNAM_XFER > VMS_PRCNAM_SIZE,
               "VMS_PRCNAM_XFER must exceed VMS_PRCNAM_SIZE or oversized names get truncated into valid ones");

/*
 * Freeze the ioctl-number ENCODING too -- the same numeric literals
 * src/kernel/vms_ioctl.h asserts, proving the NetBSD _IOWR command word matches
 * the reference build byte for byte (dir=3, size<<16, 'V'<<8, nr). If a struct
 * grows, the number changes and this fails -- a wire break, caught at compile.
 */
_Static_assert(sizeof(struct vms_register_args) == 8,
               "vms_register_args layout changed: VMS_IOCTL_REGISTER ABI break");
_Static_assert(sizeof(struct vms_dassgn_args) == 8,
               "vms_dassgn_args layout changed: VMS_IOCTL_DASSGN ABI break");
_Static_assert(VMS_IOCTL_REGISTER == 0xC0085640u,
               "VMS_IOCTL_REGISTER encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_REGISTER_CONTINUE == 0xC0085641u,
               "VMS_IOCTL_REGISTER_CONTINUE encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_DASSGN == 0xC0085651u,
               "VMS_IOCTL_DASSGN encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_SETPRN == 0xC0485641u,
               "VMS_IOCTL_SETPRN encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_GETJPI == 0xC1205642u,
               "VMS_IOCTL_GETJPI encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_GETSYIMEM == 0xC0205668u,
               "VMS_IOCTL_GETSYIMEM encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_PROCSCAN == 0xC0E05643u,
               "VMS_IOCTL_PROCSCAN encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_SETIDENT == 0xC0685644u,
               "VMS_IOCTL_SETIDENT encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_ESTABLISH_SYSTEM == 0xC0085646u,
               "VMS_IOCTL_ESTABLISH_SYSTEM encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_SETEXIT == 0xC0105649u,
               "VMS_IOCTL_SETEXIT encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_GETEXIT == 0xC014564Au,
               "VMS_IOCTL_GETEXIT encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_SETCLI == 0xC108564Bu,
               "VMS_IOCTL_SETCLI encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_GETCLI == 0xC108564Cu,
               "VMS_IOCTL_GETCLI encodes differently here than on the reference build");

#endif /* _VMS_PROCTAB_NB_H */
