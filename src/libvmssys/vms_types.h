/*
 * vms_types.h - Kernel-compatible struct definitions for direct syscall use
 *
 * These structures match the Linux kernel ABI exactly, so we can pass
 * them directly to syscalls without glibc translation layers.
 * All definitions are for x86_64.
 */

#ifndef _VMS_TYPES_H
#define _VMS_TYPES_H

/* Compiler-provided headers (not glibc) */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* ================================================================
 * Fundamental types matching kernel expectations
 * ================================================================ */

typedef int64_t  vms_off_t;
typedef uint32_t vms_mode_t;
typedef int32_t  vms_pid_t;
typedef uint32_t vms_uid_t;
typedef uint32_t vms_gid_t;
typedef unsigned long vms_size_t;
typedef long     vms_ssize_t;
/*
 * vms_time_t is fixed at 64-bit (NOT `long`). On LP64 (x86_64/aarch64/Alpha)
 * `long` is already 64-bit, but on the netbsd-vax substrate `long` is 32-bit
 * (ILP32) while NetBSD's `time_t` is 64-bit on ALL ports -- so a `long`
 * vms_time_t would truncate a NetBSD time value (wrong result + Y2038). Using a
 * fixed int64_t keeps the layout identical on the 64-bit targets and stays
 * time_t-correct at the NetBSD boundary. (rd vms-30a, audit item 5.1.)
 */
typedef int64_t  vms_time_t;
typedef int      vms_clockid_t;
typedef int      vms_timer_t;

/* Width invariant: must hold a 64-bit NetBSD time_t without truncation. This is
 * compiled by BOTH the LP64 targets and the netbsd-vax (ILP32) cross gate, so it
 * proves the fix on VAX, where a bare `long` would be only 32-bit. */
_Static_assert(sizeof(vms_time_t) >= 8, "vms_time_t must be >= 64-bit (NetBSD time_t)");

/* ================================================================
 * NULL
 * ================================================================ */

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ================================================================
 * File-related constants
 *
 * SUBSTRATE-SELECTED (rd vms-30a, audit item 5.2). The VMS_O_* / VMS_AT_* /
 * VMS_MAP_* flags are handed straight to the platform's open/openat/fcntl/mmap.
 * On the raw-freestanding Linux path they are Linux's raw-syscall numeric ABI;
 * on the NetBSD (link-libc) substrate they MUST equal NetBSD's own <fcntl.h> /
 * <sys/mman.h> values (e.g. Linux O_CREAT=0x40 vs NetBSD O_CREAT=0x200,
 * Linux MAP_ANONYMOUS=0x20 vs NetBSD MAP_ANON=0x1000) or they would mean the
 * wrong thing on NetBSD. On NetBSD we ALIAS to the sysroot headers -- so the
 * values are authoritative and substrate-correct by construction, never a
 * transcribed magic number.
 * ================================================================ */

#if defined(__NetBSD__)
#include <fcntl.h>
#include <sys/mman.h>

/* openat flags -- NetBSD <fcntl.h> values */
#define VMS_O_RDONLY     O_RDONLY
#define VMS_O_WRONLY     O_WRONLY
#define VMS_O_RDWR       O_RDWR
#define VMS_O_CREAT      O_CREAT
#define VMS_O_EXCL       O_EXCL
#define VMS_O_NOCTTY     O_NOCTTY
#define VMS_O_TRUNC      O_TRUNC
#define VMS_O_APPEND     O_APPEND
#define VMS_O_NONBLOCK   O_NONBLOCK
#define VMS_O_CLOEXEC    O_CLOEXEC
#define VMS_O_DIRECTORY  O_DIRECTORY

/* Prove the substrate-select actually took the NetBSD header value (compiled by
 * the netbsd-vax gate). If this ever fired, a stale Linux constant leaked. */
_Static_assert(VMS_O_CREAT == O_CREAT, "VMS_O_CREAT must resolve to NetBSD O_CREAT");
_Static_assert(VMS_O_RDWR  == O_RDWR,  "VMS_O_RDWR must resolve to NetBSD O_RDWR");

/* AT_* constants -- NetBSD <fcntl.h> values */
#define VMS_AT_FDCWD            AT_FDCWD
#define VMS_AT_REMOVEDIR        AT_REMOVEDIR
#define VMS_AT_SYMLINK_NOFOLLOW AT_SYMLINK_NOFOLLOW
#ifdef AT_EMPTY_PATH
#define VMS_AT_EMPTY_PATH       AT_EMPTY_PATH   /* Linux-ism; NetBSD may lack it */
#endif

#else /* !__NetBSD__ : Linux raw-syscall numeric ABI (freestanding path) */

/* openat flags */
#define VMS_O_RDONLY     0x0000
#define VMS_O_WRONLY     0x0001
#define VMS_O_RDWR       0x0002
#define VMS_O_CREAT      0x0040
#define VMS_O_EXCL       0x0080
#define VMS_O_NOCTTY     0x0100
#define VMS_O_TRUNC      0x0200
#define VMS_O_APPEND     0x0400
#define VMS_O_NONBLOCK   0x0800
#define VMS_O_CLOEXEC    0x80000
#define VMS_O_DIRECTORY  0x10000

/* AT_* constants */
#define VMS_AT_FDCWD          (-100)
#define VMS_AT_REMOVEDIR      0x200
#define VMS_AT_SYMLINK_NOFOLLOW 0x100
#define VMS_AT_EMPTY_PATH     0x1000

#endif /* __NetBSD__ */

/* seek whence */
#define VMS_SEEK_SET     0
#define VMS_SEEK_CUR     1
#define VMS_SEEK_END     2

/* fcntl commands */
#define VMS_F_DUPFD      0
#define VMS_F_GETFD      1
#define VMS_F_SETFD      2
#define VMS_F_GETFL      3
#define VMS_F_SETFL      4
#define VMS_F_SETLK      6
#define VMS_F_SETLKW     7
#define VMS_F_GETLK      5
#define VMS_FD_CLOEXEC   1

/* flock operations */
#define VMS_LOCK_SH      1
#define VMS_LOCK_EX      2
#define VMS_LOCK_NB      4
#define VMS_LOCK_UN      8

/* ================================================================
 * struct stat (kernel stat, architecture-specific layout)
 * ================================================================ */

#if defined(__x86_64__)
struct vms_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime_sec;
    uint64_t st_atime_nsec;
    uint64_t st_mtime_sec;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime_sec;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
};
#elif defined(__aarch64__)
struct vms_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  __pad2;
    int64_t  st_blocks;
    uint64_t st_atime_sec;
    uint64_t st_atime_nsec;
    uint64_t st_mtime_sec;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime_sec;
    uint64_t st_ctime_nsec;
    uint32_t __unused[2];
};
#elif defined(__alpha__)
/*
 * Alpha stat64 layout, from arch/alpha/include/uapi/asm/stat.h
 * (`struct stat64`) in the alpha-linux-gnu cross toolchain / Linux
 * 6.6.52 source -- verified field-for-field (offsetof + sizeof) against
 * that header under qemu-alpha, not transcribed from memory.  This is
 * the buffer shape __NR_fstat64 (427) and __NR_fstatat64 (455, aliased
 * to __NR_newfstatat below) fill.  Alpha's OLD __NR_fstat (91) target
 * -- struct stat, not stat64 -- is a DIFFERENT, narrower layout and
 * must never be pointed at this struct; the wrappers below are wired
 * to the stat64 pair only.
 */
struct vms_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_rdev;
    int64_t  st_size;
    uint64_t st_blocks;

    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_blksize;
    uint32_t st_nlink;
    uint32_t __pad0;

    uint64_t st_atime_sec;
    uint64_t st_atime_nsec;
    uint64_t st_mtime_sec;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime_sec;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
};
#endif

/* stat mode bits */
#define VMS_S_IFMT   0170000
#define VMS_S_IFDIR  0040000
#define VMS_S_IFREG  0100000
#define VMS_S_IFLNK  0120000
#define VMS_S_IFIFO  0010000
#define VMS_S_IFSOCK 0140000
#define VMS_S_IFCHR  0020000
#define VMS_S_IFBLK  0060000

#define VMS_S_ISDIR(m)  (((m) & VMS_S_IFMT) == VMS_S_IFDIR)
#define VMS_S_ISREG(m)  (((m) & VMS_S_IFMT) == VMS_S_IFREG)
#define VMS_S_ISLNK(m)  (((m) & VMS_S_IFMT) == VMS_S_IFLNK)

/* ================================================================
 * Directory entry (getdents64)
 * ================================================================ */

struct vms_linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];   /* null-terminated */
};

#define VMS_DT_UNKNOWN 0
#define VMS_DT_FIFO    1
#define VMS_DT_CHR     2
#define VMS_DT_DIR     4
#define VMS_DT_BLK     6
#define VMS_DT_REG     8
#define VMS_DT_LNK     10
#define VMS_DT_SOCK    12

/* ================================================================
 * Memory mapping
 * ================================================================ */

/* PROT_* values are identical on Linux and NetBSD -- no substrate split. */
#define VMS_PROT_NONE    0x0
#define VMS_PROT_READ    0x1
#define VMS_PROT_WRITE   0x2
#define VMS_PROT_EXEC    0x4

/* MAP_* flags DO differ by substrate (see the file-constants note above);
 * <sys/mman.h> was already included on the NetBSD path. */
#if defined(__NetBSD__)
#define VMS_MAP_SHARED    MAP_SHARED
#define VMS_MAP_PRIVATE   MAP_PRIVATE
#define VMS_MAP_FIXED     MAP_FIXED
#define VMS_MAP_ANONYMOUS MAP_ANON        /* NetBSD spells it MAP_ANON (0x1000) */
#ifdef MAP_NORESERVE
#define VMS_MAP_NORESERVE MAP_NORESERVE
#else
#define VMS_MAP_NORESERVE 0               /* NetBSD has no MAP_NORESERVE; benign 0 */
#endif
#elif defined(__alpha__)
/*
 * Alpha's mmap flag bits are NOT the generic Linux ones -- confirmed
 * against arch/alpha/include/uapi/asm/mman.h in the alpha-linux-gnu cross
 * toolchain (that header's own comment: "0x01 - 0x03 are defined in
 * linux/mman.h" i.e. SHARED/PRIVATE are common, but MAP_FIXED and
 * MAP_ANONYMOUS are OSF/1-heritage values, and every "Linux-specific"
 * flag from MAP_GROWSDOWN up is shifted relative to x86_64/aarch64/the
 * asm-generic values used by the #else branch below). Found by tracing a
 * real qemu-alpha segfault in tests/libvmssys/test_syscall.c's mmap/
 * munmap test: with the generic (wrong) MAP_ANONYMOUS bit, the kernel
 * returned a small negative errno that didn't equal VMS_MAP_FAILED
 * ((void*)-1), so the test's "mmap succeeded" branch dereferenced a
 * bogus pointer.
 */
#define VMS_MAP_SHARED    0x01
#define VMS_MAP_PRIVATE   0x02
#define VMS_MAP_FIXED     0x100
#define VMS_MAP_ANONYMOUS 0x10
#define VMS_MAP_NORESERVE 0x10000
#else
#define VMS_MAP_SHARED   0x01
#define VMS_MAP_PRIVATE  0x02
#define VMS_MAP_FIXED    0x10
#define VMS_MAP_ANONYMOUS 0x20
#define VMS_MAP_NORESERVE 0x4000
#endif

#define VMS_MAP_FAILED   ((void *)-1)     /* identical on Linux and NetBSD */

#define VMS_MADV_NORMAL    0
#define VMS_MADV_RANDOM    1
#define VMS_MADV_SEQUENTIAL 2
#define VMS_MADV_WILLNEED  3
#define VMS_MADV_DONTNEED  4

/* ================================================================
 * Signal definitions
 * ================================================================ */

#if defined(__alpha__)
/*
 * Alpha signal numbers are NOT the generic Linux ones below -- Alpha
 * keeps OSF/1-compatible numbering (arch/alpha/include/uapi/asm/
 * signal.h in the alpha-linux-gnu cross toolchain / Linux 6.6.52
 * source; also src/libvmssys/vms_syscall.h's __alpha__ block header
 * comment). Several of the low numbers happen to coincide with the
 * generic set (HUP/INT/QUIT/ILL/ABRT/FPE/KILL/SEGV/PIPE/ALRM/TERM), but
 * 10/12/17-20 do NOT: on Alpha, 10=SIGBUS and 12=SIGSYS (not USR1/USR2
 * at all), and CHLD/CONT/STOP are renumbered. This was found the hard
 * way: tests/libvmssys/test_futex.c's clone()-based fork-emulation
 * passed VMS_SIGCHLD (17, i.e. Alpha's real SIGSTOP) as the clone exit
 * signal and got EINVAL back from the kernel every time; passing
 * Alpha's real SIGCHLD (20) succeeds (confirmed under qemu-alpha).
 * VMS_SIGRTMIN here is the kernel's raw first-realtime-signal number
 * (Alpha NSIG=32, so 32) -- NOT glibc's userspace-reserved SIGRTMIN
 * (commonly 34 on other targets, itself a libc convention this
 * freestanding layer doesn't otherwise implement).
 */
#define VMS_SIGHUP     1
#define VMS_SIGINT     2
#define VMS_SIGQUIT    3
#define VMS_SIGILL     4
#define VMS_SIGABRT    6
#define VMS_SIGFPE     8
#define VMS_SIGKILL    9
#define VMS_SIGSEGV    11
#define VMS_SIGPIPE    13
#define VMS_SIGALRM    14
#define VMS_SIGTERM    15
#define VMS_SIGUSR1    30
#define VMS_SIGUSR2    31
#define VMS_SIGCHLD    20
#define VMS_SIGCONT    19
#define VMS_SIGSTOP    17
#define VMS_SIGRTMIN   32
#else
#define VMS_SIGHUP     1
#define VMS_SIGINT     2
#define VMS_SIGQUIT    3
#define VMS_SIGILL     4
#define VMS_SIGABRT    6
#define VMS_SIGFPE     8
#define VMS_SIGKILL    9
#define VMS_SIGSEGV    11
#define VMS_SIGPIPE    13
#define VMS_SIGALRM    14
#define VMS_SIGTERM    15
#define VMS_SIGUSR1    10
#define VMS_SIGUSR2    12
#define VMS_SIGCHLD    17
#define VMS_SIGCONT    18
#define VMS_SIGSTOP    19
#define VMS_SIGRTMIN   34
#endif

#define VMS_SIG_DFL    ((void (*)(int))0)
#define VMS_SIG_IGN    ((void (*)(int))1)
#define VMS_SIG_ERR    ((void (*)(int))-1)

#define VMS_SA_RESTORER  0x04000000
#define VMS_SA_RESTART   0x10000000
#define VMS_SA_SIGINFO   0x00000004
#define VMS_SA_NODEFER   0x40000000

#define VMS_SIG_BLOCK    0
#define VMS_SIG_UNBLOCK  1
#define VMS_SIG_SETMASK  2

/* Kernel sigset_t is 8 bytes (64 signals) */
typedef struct {
    unsigned long sig[1];  /* 64 bits */
} vms_sigset_t;

/* Kernel sigaction structure (architecture-specific) */
#if defined(__x86_64__)
struct vms_sigaction {
    void     (*sa_handler)(int);
    unsigned long sa_flags;
    void     (*sa_restorer)(void);
    vms_sigset_t sa_mask;
};
#elif defined(__aarch64__)
struct vms_sigaction {
    void     (*sa_handler)(int);
    unsigned long sa_flags;
    vms_sigset_t sa_mask;
};
#elif defined(__alpha__)
/*
 * Alpha kernel sigaction layout, from
 * arch/alpha/include/uapi/asm/signal.h (`struct sigaction`) in the
 * alpha-linux-gnu cross toolchain / Linux 6.6.52 source -- same shape
 * in glibc's alpha bits/sigaction.h.  Verified field-for-field
 * (offsetof + sizeof) against that header under qemu-alpha.  Order is
 * handler / mask / flags -- NOT flags-then-mask like x86_64/aarch64 --
 * and there is no sa_restorer field: unlike x86_64, Alpha's rt_sigaction
 * syscall takes the restorer as an explicit 5th syscall argument (see
 * arch/alpha/kernel/signal.c SYSCALL_DEFINE5(rt_sigaction, ...)), so
 * vms_sys_rt_sigaction has an Alpha-specific 5-arg body in
 * vms_syscall.h rather than sharing the generic 4-arg wrapper.
 */
struct vms_sigaction {
    void     (*sa_handler)(int);
    vms_sigset_t sa_mask;
    int      sa_flags;
};
#endif

/* ================================================================
 * Time structures
 * ================================================================ */

struct vms_timespec {
    vms_time_t tv_sec;
    long       tv_nsec;
};

struct vms_timeval {
    vms_time_t tv_sec;
    long       tv_usec;
};

struct vms_itimerspec {
    struct vms_timespec it_interval;
    struct vms_timespec it_value;
};

/* clock IDs */
#define VMS_CLOCK_REALTIME         0
#define VMS_CLOCK_MONOTONIC        1
#define VMS_CLOCK_PROCESS_CPUTIME  2
#define VMS_CLOCK_THREAD_CPUTIME   3
#define VMS_CLOCK_MONOTONIC_RAW    4

/* timer_create signal event */
#define VMS_SIGEV_SIGNAL  0
#define VMS_SIGEV_NONE    1
#define VMS_SIGEV_THREAD  2

struct vms_sigevent {
    int      sigev_value;
    int      sigev_signo;
    int      sigev_notify;
    int      _pad[12];     /* union padding to match kernel size */
};

/* ================================================================
 * Process / clone
 * ================================================================ */

/* VMS_SIGCHLD is already defined above (Signal definitions, per-arch:
 * 17 generic / 20 on Alpha) -- do not redefine it here. A duplicate,
 * unconditional `#define VMS_SIGCHLD 17` used to live in this section
 * and silently clobbered the Alpha value via macro redefinition (found
 * while fixing the clone()-exit-signal EINVAL below). */

/* clone flags */
#define VMS_CLONE_VM            0x00000100
#define VMS_CLONE_FS            0x00000200
#define VMS_CLONE_FILES         0x00000400
#define VMS_CLONE_SIGHAND       0x00000800
#define VMS_CLONE_THREAD        0x00010000
#define VMS_CLONE_SYSVSEM       0x00040000
#define VMS_CLONE_SETTLS        0x00080000
#define VMS_CLONE_PARENT_SETTID 0x00100000
#define VMS_CLONE_CHILD_CLEARTID 0x00200000
#define VMS_CLONE_CHILD_SETTID  0x01000000

/* wait options */
#define VMS_WNOHANG   1
#define VMS_WUNTRACED 2

#define VMS_WIFEXITED(s)   (!((s) & 0x7F))
#define VMS_WEXITSTATUS(s) (((s) >> 8) & 0xFF)
#define VMS_WIFSIGNALED(s) (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define VMS_WTERMSIG(s)    ((s) & 0x7F)

/* ================================================================
 * struct rusage (for wait4)
 * ================================================================ */

struct vms_rusage {
    struct vms_timeval ru_utime;
    struct vms_timeval ru_stime;
    long ru_maxrss;
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

/* ================================================================
 * uname
 * ================================================================ */

struct vms_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

/* ================================================================
 * sysinfo
 * ================================================================ */

struct vms_sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    uint16_t procs;
    uint16_t pad;
    uint32_t pad2;
    unsigned long totalhigh;
    unsigned long freehigh;
    uint32_t mem_unit;
    char _pad[4];
};

/* ================================================================
 * Socket definitions
 * ================================================================ */

#define VMS_AF_UNIX      1
#define VMS_AF_INET      2
#define VMS_AF_INET6     10

#define VMS_SOCK_STREAM  1
#define VMS_SOCK_DGRAM   2
#define VMS_SOCK_CLOEXEC 0x80000
#define VMS_SOCK_NONBLOCK 0x800

struct vms_sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

struct vms_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char     sin_zero[8];
};

struct vms_sockaddr_un {
    uint16_t sun_family;
    char     sun_path[108];
};

/* ================================================================
 * io_uring structures
 * ================================================================ */

struct vms_io_uring_params {
    uint32_t sq_entries;
    uint32_t cq_entries;
    uint32_t flags;
    uint32_t sq_thread_cpu;
    uint32_t sq_thread_idle;
    uint32_t features;
    uint32_t wq_fd;
    uint32_t resv[3];
    struct {
        uint32_t head;
        uint32_t tail;
        uint32_t ring_mask;
        uint32_t ring_entries;
        uint32_t flags;
        uint32_t dropped;
        uint32_t array;
        uint32_t resv1;
        uint64_t resv2;
    } sq_off;
    struct {
        uint32_t head;
        uint32_t tail;
        uint32_t ring_mask;
        uint32_t ring_entries;
        uint32_t overflow;
        uint32_t cqes;
        uint32_t flags;
        uint32_t resv1;
        uint64_t resv2;
    } cq_off;
};

/* ================================================================
 * Pipe / dup
 * ================================================================ */

#define VMS_O_CLOEXEC_PIPE 0x80000

/* ================================================================
 * arch_prctl operations
 * ================================================================ */

#define VMS_ARCH_SET_GS  0x1001
#define VMS_ARCH_SET_FS  0x1002
#define VMS_ARCH_GET_FS  0x1003
#define VMS_ARCH_GET_GS  0x1004

/* ================================================================
 * Futex operations
 *
 * futex(2) op numbers, substrate-selected like VMS_O_* above (vms-706). The op
 * VALUES happen to match across Linux and NetBSD, but each substrate takes them
 * from ITS OWN header so no transcribed magic number reaches the wrong syscall:
 * Linux hardcodes the well-known futex(2) numbers; NetBSD aliases <sys/futex.h>
 * FUTEX_*, and vms_sys_futex (arch/vax/vms_syscall_netbsd.h) issues __futex(2).
 * (Supersedes the earlier "deferred, NetBSD-excluded" audit note, rd vms-30a
 * item 5.2: vms_futex.c is no longer excluded from the netbsd build.)
 * ================================================================ */

#if defined(__NetBSD__)
#include <sys/futex.h>
#define VMS_FUTEX_WAIT                 FUTEX_WAIT
#define VMS_FUTEX_WAKE                 FUTEX_WAKE
#define VMS_FUTEX_WAIT_PRIVATE         (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define VMS_FUTEX_WAKE_PRIVATE         (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#define VMS_FUTEX_WAIT_BITSET          FUTEX_WAIT_BITSET
#define VMS_FUTEX_WAIT_BITSET_PRIVATE  (FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG)
#define VMS_FUTEX_BITSET_MATCH_ANY     FUTEX_BITSET_MATCH_ANY
/* Prove the substrate-select took the NetBSD header value, not a stale copy. */
_Static_assert(VMS_FUTEX_WAIT == FUTEX_WAIT, "VMS_FUTEX_WAIT must resolve to NetBSD FUTEX_WAIT");
_Static_assert(VMS_FUTEX_WAIT_BITSET == FUTEX_WAIT_BITSET, "VMS_FUTEX_WAIT_BITSET must resolve to NetBSD FUTEX_WAIT_BITSET");
#else
#define VMS_FUTEX_WAIT          0
#define VMS_FUTEX_WAKE          1
#define VMS_FUTEX_WAIT_PRIVATE  (VMS_FUTEX_WAIT | 128)
#define VMS_FUTEX_WAKE_PRIVATE  (VMS_FUTEX_WAKE | 128)
#define VMS_FUTEX_WAIT_BITSET   9
#define VMS_FUTEX_WAIT_BITSET_PRIVATE  (VMS_FUTEX_WAIT_BITSET | 128)
#define VMS_FUTEX_BITSET_MATCH_ANY  0xFFFFFFFF
#endif /* __NetBSD__ */

/* ================================================================
 * Auxiliary vector (from kernel ELF loader)
 * ================================================================ */

#define VMS_AT_NULL      0
#define VMS_AT_IGNORE    1
#define VMS_AT_EXECFD    2
#define VMS_AT_PHDR      3
#define VMS_AT_PHENT     4
#define VMS_AT_PHNUM     5
#define VMS_AT_PAGESZ    6
#define VMS_AT_BASE      7
#define VMS_AT_FLAGS     8
#define VMS_AT_ENTRY     9
#define VMS_AT_UID       11
#define VMS_AT_EUID      12
#define VMS_AT_GID       13
#define VMS_AT_EGID      14
#define VMS_AT_RANDOM    25
#define VMS_AT_HWCAP     16
#define VMS_AT_HWCAP2    26
#define VMS_AT_SECURE    23

/* ================================================================
 * ioctl
 * ================================================================ */

/* Terminal ioctl codes (for basic terminal control) */
#define VMS_TCGETS       0x5401
#define VMS_TCSETS       0x5402
#define VMS_TCSETSW      0x5403
#define VMS_TIOCGWINSZ   0x5413

struct vms_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/* termios structure for terminal control */
#define VMS_NCCS 19

struct vms_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[VMS_NCCS];
};

/* c_lflag bits */
#define VMS_ECHO    0x0008
#define VMS_ICANON  0x0002
#define VMS_ISIG    0x0001
#define VMS_ECHONL  0x0040

/* ================================================================
 * Boolean helpers
 * ================================================================ */

#ifndef __bool_true_false_are_defined
#define bool    _Bool
#define true    1
#define false   0
#define __bool_true_false_are_defined 1
#endif

#endif /* _VMS_TYPES_H */
