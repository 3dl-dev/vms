/*
 * vms_syscall.h - Typed inline wrappers for Linux syscalls
 *
 * Each wrapper calls through the assembly trampoline in syscall.S,
 * providing type safety and proper argument counts.  Return value
 * is the raw kernel return (negative = -errno).
 *
 * Supports x86_64, aarch64, and alpha.
 */

#ifndef _VMS_SYSCALL_H
#define _VMS_SYSCALL_H

#include "vms_types.h"

#if defined(__NetBSD__)
/*
 * OVMX/NetBSD SYSKRNL (epic vms-8e8): the vms_sys_* wrappers resolve to NetBSD
 * libc calls rather than raw kernel traps -- the VAX backend LINKS NetBSD libc
 * (docs/design-ovmx-netbsd-syskrnl.md §4.1). This is the whole file on NetBSD;
 * none of the Linux syscall-number tables or asm-trampoline wrappers below are
 * compiled.
 */
#include "arch/vax/vms_syscall_netbsd.h"
#else  /* raw-freestanding Linux/Alpha path */

/* ================================================================
 * Syscall numbers (architecture-specific)
 * ================================================================ */

#if defined(__x86_64__)

#define __NR_read               0
#define __NR_write              1
#define __NR_open               2
#define __NR_close              3
#define __NR_fstat              5
#define __NR_lseek              8
#define __NR_mmap               9
#define __NR_mprotect           10
#define __NR_munmap             11
#define __NR_brk                12
#define __NR_rt_sigaction       13
#define __NR_rt_sigprocmask     14
#define __NR_rt_sigreturn       15
#define __NR_ioctl              16
#define __NR_pipe2              293
#define __NR_dup3               292
#define __NR_nanosleep          35
#define __NR_getpid             39
#define __NR_socket             41
#define __NR_connect            42
#define __NR_accept4            288
#define __NR_sendto             44
#define __NR_recvfrom           45
#define __NR_bind               49
#define __NR_listen             50
#define __NR_clone              56
#define __NR_fork               57
#define __NR_execve             59
#define __NR_exit               60
#define __NR_wait4              61
#define __NR_kill               62
#define __NR_uname              63
#define __NR_fcntl              72
#define __NR_flock              73
#define __NR_fsync              74
#define __NR_ftruncate          77
#define __NR_getcwd             79
#define __NR_chdir              80
#define __NR_mkdirat            258
#define __NR_unlinkat           263
#define __NR_renameat2          316
#define __NR_openat             257
#define __NR_newfstatat         262
#define __NR_getdents64         217
#define __NR_set_tid_address    218
#define __NR_set_robust_list    273
#define __NR_futex              202
#define __NR_setsid             112
#define __NR_getuid             102
#define __NR_getgid             104
#define __NR_gettid             186
#define __NR_clock_gettime      228
#define __NR_clock_nanosleep    230
#define __NR_timer_create       222
#define __NR_timer_settime      223
#define __NR_timer_delete       226
#define __NR_exit_group         231
#define __NR_madvise            28
#define __NR_arch_prctl         158
#define __NR_sysinfo            99
#define __NR_io_uring_setup     425
#define __NR_io_uring_enter     426
#define __NR_io_uring_register  427

#elif defined(__aarch64__)

#define __NR_read               63
#define __NR_write              64
#define __NR_open               (-1)  /* not available on aarch64, use openat */
#define __NR_close              57
#define __NR_fstat              80
#define __NR_lseek              62
#define __NR_mmap               222
#define __NR_mprotect           226
#define __NR_munmap             215
#define __NR_brk                214
#define __NR_rt_sigaction       134
#define __NR_rt_sigprocmask     135
#define __NR_rt_sigreturn       139
#define __NR_ioctl              29
#define __NR_pipe2              59
#define __NR_dup3               24
#define __NR_nanosleep          101
#define __NR_getpid             172
#define __NR_socket             198
#define __NR_connect            203
#define __NR_accept4            242
#define __NR_sendto             206
#define __NR_recvfrom           207
#define __NR_bind               200
#define __NR_listen             201
#define __NR_clone              220
#define __NR_fork               (-1)  /* not available on aarch64, use clone */
#define __NR_execve             221
#define __NR_exit               93
#define __NR_wait4              260
#define __NR_kill               129
#define __NR_uname              160
#define __NR_fcntl              25
#define __NR_flock              32
#define __NR_fsync              82
#define __NR_ftruncate          46
#define __NR_getcwd             17
#define __NR_chdir              49
#define __NR_mkdirat            34
#define __NR_unlinkat           35
#define __NR_renameat2          276
#define __NR_openat             56
#define __NR_newfstatat         79
#define __NR_getdents64         61
#define __NR_set_tid_address    96
#define __NR_set_robust_list    99
#define __NR_futex              98
#define __NR_setsid             157
#define __NR_getuid             174
#define __NR_getgid             176
#define __NR_gettid             178
#define __NR_clock_gettime      113
#define __NR_clock_nanosleep    115
#define __NR_timer_create       107
#define __NR_timer_settime      110
#define __NR_timer_delete       111
#define __NR_exit_group         94
#define __NR_madvise            233
#define __NR_arch_prctl         (-1)  /* x86_64 only */
#define __NR_sysinfo            179
#define __NR_io_uring_setup     425
#define __NR_io_uring_enter     426
#define __NR_io_uring_register  427

#elif defined(__alpha__)

/*
 * Alpha uses the OSF/1-derived syscall table -- numbers differ entirely
 * from x86_64/aarch64.  All values below are verified against the
 * alpha-linux-gnu <asm/unistd.h> in the cross toolchain (not transcribed
 * from memory).  Two Alpha-specific mappings worth calling out:
 *   - Alpha has no getpid(); getxpid() (nr 20) returns the pid in v0, so
 *     aliasing __NR_getpid to it is correct for our wrapper.
 *   - Alpha has no newfstatat(); it exposes fstatat64() (nr 455).  That
 *     call fills a stat64-shaped buffer -- see the __alpha__ struct
 *     vms_stat layout note in vms_types.h before trusting fstat results.
 *   - Alpha's plain __NR_fstat (91) fills the OLD, narrower `struct stat`
 *     (not stat64) -- deliberately NOT wired to vms_sys_fstat.  The
 *     fstat wrapper uses __NR_fstat64 (427) instead, so both stat
 *     entry points (vms_sys_fstat, vms_sys_newfstatat) fill the same
 *     stat64-shaped struct vms_stat.
 *   - Alpha's rt_sigaction is SYSCALL_DEFINE5, not 4-arg like
 *     x86_64/aarch64: the signal restorer is an explicit 5th syscall
 *     argument rather than a struct sigaction field.  See the
 *     Alpha-specific vms_sys_rt_sigaction body below.
 */
#define __NR_read               3
#define __NR_write              4
#define __NR_open               45
#define __NR_close              6
#define __NR_fstat              91     /* OLD struct stat -- NOT used, see above */
#define __NR_fstat64            427    /* stat64 buffer -- what vms_sys_fstat uses */
#define __NR_lseek              19
#define __NR_mmap               71
#define __NR_mprotect           74
#define __NR_munmap             73
#define __NR_brk                17
#define __NR_rt_sigaction       352
#define __NR_rt_sigprocmask     353
#define __NR_rt_sigreturn       351
#define __NR_ioctl              54
#define __NR_pipe2              488
#define __NR_dup3               487
#define __NR_nanosleep          340
#define __NR_getpid             20     /* getxpid: pid in v0 */
#define __NR_socket             97
#define __NR_connect            98
#define __NR_accept4            502
#define __NR_sendto             133
#define __NR_recvfrom           125
#define __NR_bind               104
#define __NR_listen             106
#define __NR_clone              312
#define __NR_fork               2
#define __NR_execve             59
#define __NR_exit               1
#define __NR_wait4              365
#define __NR_kill               37
#define __NR_uname              339
#define __NR_fcntl              92
#define __NR_flock              131
#define __NR_fsync              95
#define __NR_ftruncate          130
#define __NR_getcwd             367
#define __NR_chdir              12
#define __NR_mkdirat            451
#define __NR_unlinkat           456
#define __NR_renameat2          510
#define __NR_openat             450
#define __NR_newfstatat         455    /* fstatat64 -- stat64 buffer, see vms_types.h */
#define __NR_getdents64         377
#define __NR_set_tid_address    411
#define __NR_set_robust_list    466
#define __NR_futex              394
#define __NR_setsid             147
#define __NR_getuid             24
#define __NR_getgid             47
#define __NR_gettid             378
#define __NR_clock_gettime      420
#define __NR_clock_nanosleep    422
#define __NR_timer_create       414
#define __NR_timer_settime      415
#define __NR_timer_delete       418
#define __NR_exit_group         405
#define __NR_madvise            75
#define __NR_arch_prctl         (-1)   /* x86_64 only */
#define __NR_sysinfo            318
#define __NR_io_uring_setup     535
#define __NR_io_uring_enter     536
#define __NR_io_uring_register  537

#else
#error "Unsupported architecture for libvmssys"
#endif

/* ================================================================
 * Assembly trampoline declarations
 * ================================================================ */

extern long __vms_syscall0(long nr);
extern long __vms_syscall1(long nr, long a1);
extern long __vms_syscall2(long nr, long a1, long a2);
extern long __vms_syscall3(long nr, long a1, long a2, long a3);
extern long __vms_syscall4(long nr, long a1, long a2, long a3, long a4);
extern long __vms_syscall5(long nr, long a1, long a2, long a3, long a4, long a5);
extern long __vms_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6);

/* ================================================================
 * File I/O
 * ================================================================ */

static inline int vms_sys_openat(int dirfd, const char *path, int flags, vms_mode_t mode)
{
    return (int)__vms_syscall4(__NR_openat, dirfd, (long)path, flags, mode);
}

static inline int vms_sys_close(int fd)
{
    return (int)__vms_syscall1(__NR_close, fd);
}

static inline vms_ssize_t vms_sys_read(int fd, void *buf, vms_size_t count)
{
    return (vms_ssize_t)__vms_syscall3(__NR_read, fd, (long)buf, count);
}

static inline vms_ssize_t vms_sys_write(int fd, const void *buf, vms_size_t count)
{
    return (vms_ssize_t)__vms_syscall3(__NR_write, fd, (long)buf, count);
}

static inline vms_off_t vms_sys_lseek(int fd, vms_off_t offset, int whence)
{
    return (vms_off_t)__vms_syscall3(__NR_lseek, fd, offset, whence);
}

static inline int vms_sys_fstat(int fd, struct vms_stat *buf)
{
#if defined(__alpha__)
    /* stat64 pair: __NR_fstat (91) fills the OLD struct stat, which does
     * not match the stat64-shaped struct vms_stat defined for Alpha in
     * vms_types.h.  Use __NR_fstat64 (427) instead. */
    return (int)__vms_syscall2(__NR_fstat64, fd, (long)buf);
#else
    return (int)__vms_syscall2(__NR_fstat, fd, (long)buf);
#endif
}

static inline int vms_sys_newfstatat(int dirfd, const char *path, struct vms_stat *buf, int flags)
{
    return (int)__vms_syscall4(__NR_newfstatat, dirfd, (long)path, (long)buf, flags);
}

static inline int vms_sys_ftruncate(int fd, vms_off_t length)
{
    return (int)__vms_syscall2(__NR_ftruncate, fd, length);
}

static inline int vms_sys_fsync(int fd)
{
    return (int)__vms_syscall1(__NR_fsync, fd);
}

static inline int vms_sys_flock(int fd, int operation)
{
    return (int)__vms_syscall2(__NR_flock, fd, operation);
}

static inline int vms_sys_fcntl(int fd, int cmd, long arg)
{
    return (int)__vms_syscall3(__NR_fcntl, fd, cmd, arg);
}

static inline int vms_sys_unlinkat(int dirfd, const char *path, int flags)
{
    return (int)__vms_syscall3(__NR_unlinkat, dirfd, (long)path, flags);
}

static inline int vms_sys_renameat2(int olddirfd, const char *oldpath,
                                     int newdirfd, const char *newpath, unsigned int flags)
{
    return (int)__vms_syscall5(__NR_renameat2, olddirfd, (long)oldpath,
                               newdirfd, (long)newpath, flags);
}

static inline int vms_sys_mkdirat(int dirfd, const char *path, vms_mode_t mode)
{
    return (int)__vms_syscall3(__NR_mkdirat, dirfd, (long)path, mode);
}

static inline vms_ssize_t vms_sys_getdents64(int fd, void *dirp, vms_size_t count)
{
    return (vms_ssize_t)__vms_syscall3(__NR_getdents64, fd, (long)dirp, count);
}

/* ================================================================
 * Memory management
 * ================================================================ */

static inline void *vms_sys_mmap(void *addr, vms_size_t length, int prot,
                                  int flags, int fd, vms_off_t offset)
{
    return (void *)__vms_syscall6(__NR_mmap, (long)addr, length, prot, flags, fd, offset);
}

static inline int vms_sys_munmap(void *addr, vms_size_t length)
{
    return (int)__vms_syscall2(__NR_munmap, (long)addr, length);
}

static inline int vms_sys_mprotect(void *addr, vms_size_t length, int prot)
{
    return (int)__vms_syscall3(__NR_mprotect, (long)addr, length, prot);
}

static inline long vms_sys_brk(void *addr)
{
    return __vms_syscall1(__NR_brk, (long)addr);
}

static inline int vms_sys_madvise(void *addr, vms_size_t length, int advice)
{
    return (int)__vms_syscall3(__NR_madvise, (long)addr, length, advice);
}

/* ================================================================
 * Process management
 * ================================================================ */

static inline long vms_sys_clone(unsigned long flags, void *stack,
                                  int *parent_tid, int *child_tid, unsigned long tls)
{
    return __vms_syscall5(__NR_clone, flags, (long)stack,
                          (long)parent_tid, (long)child_tid, tls);
}

static inline int vms_sys_execve(const char *filename, char *const argv[],
                                  char *const envp[])
{
    return (int)__vms_syscall3(__NR_execve, (long)filename, (long)argv, (long)envp);
}

static inline void vms_sys_exit_group(int status)
{
    __vms_syscall1(__NR_exit_group, status);
    __builtin_unreachable();
}

static inline void vms_sys_exit(int status)
{
    __vms_syscall1(__NR_exit, status);
    __builtin_unreachable();
}

static inline vms_pid_t vms_sys_wait4(vms_pid_t pid, int *wstatus, int options,
                                       struct vms_rusage *rusage)
{
    return (vms_pid_t)__vms_syscall4(__NR_wait4, pid, (long)wstatus, options, (long)rusage);
}

static inline int vms_sys_kill(vms_pid_t pid, int sig)
{
    return (int)__vms_syscall2(__NR_kill, pid, sig);
}

static inline vms_pid_t vms_sys_getpid(void)
{
    return (vms_pid_t)__vms_syscall0(__NR_getpid);
}

static inline vms_uid_t vms_sys_getuid(void)
{
    return (vms_uid_t)__vms_syscall0(__NR_getuid);
}

static inline vms_gid_t vms_sys_getgid(void)
{
    return (vms_gid_t)__vms_syscall0(__NR_getgid);
}

static inline vms_pid_t vms_sys_gettid(void)
{
    return (vms_pid_t)__vms_syscall0(__NR_gettid);
}

static inline vms_pid_t vms_sys_setsid(void)
{
    return (vms_pid_t)__vms_syscall0(__NR_setsid);
}

/* ================================================================
 * Signals
 * ================================================================ */

/* rt_sigreturn trampoline (arch/<arch>/sigreturn.S); forward-declared here so
 * the Alpha rt_sigaction wrapper below can pass it as the syscall's
 * explicit restorer argument. Also re-declared below for reference on
 * the other architectures, where it is wired in via struct sigaction
 * instead (or not required at all). */
extern void __vms_rt_sigreturn(void);

static inline int vms_sys_rt_sigaction(int signum, const struct vms_sigaction *act,
                                        struct vms_sigaction *oldact, vms_size_t sigsetsize)
{
#if defined(__alpha__)
    /* Alpha's rt_sigaction is SYSCALL_DEFINE5(rt_sigaction, sig, act, oact,
     * sigsetsize, restorer) -- arch/alpha/kernel/signal.c.  The restorer
     * is a separate syscall argument, not a struct sigaction field (Alpha's
     * struct vms_sigaction has no sa_restorer member).  Passing
     * __vms_rt_sigreturn unconditionally is harmless when act == NULL
     * (query-only mode): the kernel only consults ka_restorer inside the
     * `if (act)` branch of sys_rt_sigaction. */
    return (int)__vms_syscall5(__NR_rt_sigaction, signum, (long)act, (long)oldact,
                               sigsetsize, (long)__vms_rt_sigreturn);
#else
    return (int)__vms_syscall4(__NR_rt_sigaction, signum, (long)act, (long)oldact, sigsetsize);
#endif
}

static inline int vms_sys_rt_sigprocmask(int how, const vms_sigset_t *set,
                                           vms_sigset_t *oldset, vms_size_t sigsetsize)
{
    return (int)__vms_syscall4(__NR_rt_sigprocmask, how, (long)set, (long)oldset, sigsetsize);
}

/* ================================================================
 * Synchronization (futex)
 * ================================================================ */

static inline long vms_sys_futex(uint32_t *uaddr, int futex_op, uint32_t val,
                                  const struct vms_timespec *timeout,
                                  uint32_t *uaddr2, uint32_t val3)
{
    return __vms_syscall6(__NR_futex, (long)uaddr, futex_op, val,
                          (long)timeout, (long)uaddr2, val3);
}

/* ================================================================
 * Time
 * ================================================================ */

static inline int vms_sys_clock_gettime(vms_clockid_t clockid, struct vms_timespec *tp)
{
    return (int)__vms_syscall2(__NR_clock_gettime, clockid, (long)tp);
}

static inline int vms_sys_clock_nanosleep(vms_clockid_t clockid, int flags,
                                           const struct vms_timespec *request,
                                           struct vms_timespec *remain)
{
    return (int)__vms_syscall4(__NR_clock_nanosleep, clockid, flags,
                               (long)request, (long)remain);
}

static inline int vms_sys_timer_create(vms_clockid_t clockid,
                                        struct vms_sigevent *sevp,
                                        vms_timer_t *timerid)
{
    return (int)__vms_syscall3(__NR_timer_create, clockid, (long)sevp, (long)timerid);
}

static inline int vms_sys_timer_settime(vms_timer_t timerid, int flags,
                                         const struct vms_itimerspec *new_value,
                                         struct vms_itimerspec *old_value)
{
    return (int)__vms_syscall4(__NR_timer_settime, timerid, flags,
                               (long)new_value, (long)old_value);
}

static inline int vms_sys_timer_delete(vms_timer_t timerid)
{
    return (int)__vms_syscall1(__NR_timer_delete, timerid);
}

static inline int vms_sys_nanosleep(const struct vms_timespec *req, struct vms_timespec *rem)
{
    return (int)__vms_syscall2(__NR_nanosleep, (long)req, (long)rem);
}

/* ================================================================
 * Sockets
 * ================================================================ */

static inline int vms_sys_socket(int domain, int type, int protocol)
{
    return (int)__vms_syscall3(__NR_socket, domain, type, protocol);
}

static inline int vms_sys_bind(int sockfd, const void *addr, uint32_t addrlen)
{
    return (int)__vms_syscall3(__NR_bind, sockfd, (long)addr, addrlen);
}

static inline int vms_sys_listen(int sockfd, int backlog)
{
    return (int)__vms_syscall2(__NR_listen, sockfd, backlog);
}

static inline int vms_sys_accept4(int sockfd, void *addr, uint32_t *addrlen, int flags)
{
    return (int)__vms_syscall4(__NR_accept4, sockfd, (long)addr, (long)addrlen, flags);
}

static inline int vms_sys_connect(int sockfd, const void *addr, uint32_t addrlen)
{
    return (int)__vms_syscall3(__NR_connect, sockfd, (long)addr, addrlen);
}

static inline vms_ssize_t vms_sys_sendto(int sockfd, const void *buf, vms_size_t len,
                                          int flags, const void *dest_addr, uint32_t addrlen)
{
    return (vms_ssize_t)__vms_syscall6(__NR_sendto, sockfd, (long)buf, len,
                                        flags, (long)dest_addr, addrlen);
}

static inline vms_ssize_t vms_sys_recvfrom(int sockfd, void *buf, vms_size_t len,
                                            int flags, void *src_addr, uint32_t *addrlen)
{
    return (vms_ssize_t)__vms_syscall6(__NR_recvfrom, sockfd, (long)buf, len,
                                        flags, (long)src_addr, (long)addrlen);
}

/* ================================================================
 * System info
 * ================================================================ */

static inline int vms_sys_uname(struct vms_utsname *buf)
{
    return (int)__vms_syscall1(__NR_uname, (long)buf);
}

static inline int vms_sys_sysinfo(struct vms_sysinfo *info)
{
    return (int)__vms_syscall1(__NR_sysinfo, (long)info);
}

static inline int vms_sys_ioctl(int fd, unsigned long request, unsigned long arg)
{
    return (int)__vms_syscall3(__NR_ioctl, fd, request, arg);
}

static inline vms_ssize_t vms_sys_getcwd(char *buf, vms_size_t size)
{
    return (vms_ssize_t)__vms_syscall2(__NR_getcwd, (long)buf, size);
}

static inline int vms_sys_chdir(const char *path)
{
    return (int)__vms_syscall1(__NR_chdir, (long)path);
}

/* ================================================================
 * io_uring
 * ================================================================ */

static inline int vms_sys_io_uring_setup(uint32_t entries, struct vms_io_uring_params *p)
{
    return (int)__vms_syscall2(__NR_io_uring_setup, entries, (long)p);
}

static inline int vms_sys_io_uring_enter(int ring_fd, uint32_t to_submit,
                                          uint32_t min_complete, uint32_t flags,
                                          const void *sig)
{
    return (int)__vms_syscall5(__NR_io_uring_enter, ring_fd, to_submit,
                               min_complete, flags, (long)sig);
}

static inline int vms_sys_io_uring_register(int ring_fd, uint32_t opcode,
                                             const void *arg, uint32_t nr_args)
{
    return (int)__vms_syscall4(__NR_io_uring_register, ring_fd, opcode, (long)arg, nr_args);
}

/* ================================================================
 * Thread support
 * ================================================================ */

#if defined(__x86_64__)
static inline int vms_sys_arch_prctl(int code, unsigned long addr)
{
    return (int)__vms_syscall2(__NR_arch_prctl, code, addr);
}
#endif

static inline long vms_sys_set_tid_address(int *tidptr)
{
    return __vms_syscall1(__NR_set_tid_address, (long)tidptr);
}

static inline int vms_sys_set_robust_list(void *head, vms_size_t len)
{
    return (int)__vms_syscall2(__NR_set_robust_list, (long)head, len);
}

/* ================================================================
 * Pipe / dup
 * ================================================================ */

static inline int vms_sys_pipe2(int pipefd[2], int flags)
{
    return (int)__vms_syscall2(__NR_pipe2, (long)pipefd, flags);
}

static inline int vms_sys_dup3(int oldfd, int newfd, int flags)
{
    return (int)__vms_syscall3(__NR_dup3, oldfd, newfd, flags);
}

/* ================================================================
 * Convenience: check syscall return for error
 * ================================================================ */

static inline int vms_syscall_is_error(long ret)
{
    return ret < 0 && ret >= -4095;
}

static inline int vms_syscall_errno(long ret)
{
    return (int)(-ret);
}

#endif /* !__NetBSD__ */

#endif /* _VMS_SYSCALL_H */
