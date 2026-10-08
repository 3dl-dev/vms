/*
 * ovmx_decc_vfork.c - DEC C vfork()/exec*() for OVMX/Alpha (vms-fb4).
 *
 * THE DEC C MODEL. On OpenVMS a DEC C program does not fork: vfork() records a
 * context and returns 0, the code between vfork() and exec*() runs in the
 * PARENT, and exec*() then creates a subprocess running the named image with
 * the given arguments and returns control to the vfork() point -- which now
 * returns the subprocess's PID. The parent later wait()s for it. This file is
 * that model, over OVMX's executive-backed process creation:
 *
 *   vfork()   -> the client header expands it to decc$$alloc_vfork_blocks()
 *                (opens the context, snapshots the file-descriptor table) and
 *                decc$$vfork_setjmp(decc$$vfork_jmpbuf()) (the C RTL's setjmp
 *                on the context's jump buffer, in the CALLER's frame, so it
 *                can return a second time).
 *   exec*()   -> in a vfork context: fork() a process that inherits the
 *                creator's executive identity (the OVMX subprocess path, as
 *                $CREPRC's), which execve()s IMGACT.EXE with the image spec and
 *                arguments; IMGACT activates the image off the ODS-2 volume
 *                through the ACP (imgact.c, vms-fb4 launcher) and decc$main
 *                hands the arguments to its main(). The parent's descriptor
 *                table is restored to the vfork-time snapshot (descriptor work
 *                done "in the child" between vfork and exec is the child's),
 *                and control returns to the vfork() point with the PID.
 *                Outside a vfork context the calling process is replaced by the
 *                image (execve of the same IMGACT launcher), the POSIX meaning.
 *
 * ARGUMENT VECTORS are arrays of the CLIENT's pointers: 32-bit entry points
 * (decc$$exec*32) for the DEC C default pointer size, 64-bit ones
 * (decc$$exec*64) for -mpointer-size=64 callers such as libgcc's gcov exec
 * wrappers. (DEC C's 64-bit headers take 32-bit vectors -- what libiberty's
 * __LONG_POINTERS to_ptr32 code builds; OVMX follows the client's own vector
 * type so a 64-bit caller's array is never misread.) argv[0] is not passed
 * on: the created image's argv[0] is its image spec, as DEC C supplies it.
 *
 * NOT YET: wait status is the Linux one (the image's VMS condition maps to its
 * POSIX exit code through IMGACT's $EXIT); _exit() in a vfork context (an exec
 * that failed) ends the calling process, as DEC C's own documentation warns
 * against; one vfork context per process (DECC$SHR is a non-TLS producer).
 */
#if defined(__alpha) && defined(__VMS)

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ovmx_layout.h"               /* OVMX_BOOT_STAGE_DIR */

#define OVMX_IMGACT_LAUNCHER  OVMX_BOOT_STAGE_DIR "/IMGACT.EXE"
extern char **__environ;
#define VFORK_MAX_FD          256
#define VFORK_SAVE_BASE       768      /* snapshot copies live above any client fd */

static struct {
    jmp_buf jb;
    int     active;
    int     saved[VFORK_MAX_FD];       /* -1: fd was closed at vfork time */
} vblk;

int ovmx_alloc_vfork_blocks(void) __asm__("decc$$alloc_vfork_blocks");
void *ovmx_vfork_jmpbuf(void) __asm__("decc$$vfork_jmpbuf");

int ovmx_alloc_vfork_blocks(void)
{
    if (vblk.active) {                  /* nested vfork without an exec */
        errno = EAGAIN;
        return -1;
    }
    for (int fd = 0; fd < VFORK_MAX_FD; fd++) {
        vblk.saved[fd] = -1;
        if (fcntl(fd, F_GETFD) >= 0)
            vblk.saved[fd] = fcntl(fd, F_DUPFD_CLOEXEC, VFORK_SAVE_BASE);
    }
    vblk.active = 1;
    return 0;
}

void *ovmx_vfork_jmpbuf(void)
{
    return vblk.jb;
}

/* Put the parent's descriptor table back the way vfork() found it. */
static void vfork_restore_fds(void)
{
    for (int fd = 0; fd < VFORK_MAX_FD; fd++) {
        if (vblk.saved[fd] >= 0) {
            dup2(vblk.saved[fd], fd);
            close(vblk.saved[fd]);
        } else if (fcntl(fd, F_GETFD) >= 0) {
            close(fd);                  /* opened in the child context */
        }
        vblk.saved[fd] = -1;
    }
}

/* A DEC C argument vector: 32-bit pointers, NULL-terminated. */
static size_t v32_len(const unsigned int *v)
{
    size_t n = 0;
    if (v)
        while (v[n])
            n++;
    return n;
}

static char *p32(unsigned int a)
{
    return (char *)(unsigned long)(int)a;    /* sign-extended longword */
}

/* Run `spec` with arguments args[0..nargs) and environment `envp`. */
static int ovmx_exec_image(const char *spec, char **args, size_t nargs, char **envp)
{
    char **av = malloc((nargs + 3) * sizeof *av);
    if (!av) {
        errno = ENOMEM;
        return -1;
    }
    av[0] = OVMX_IMGACT_LAUNCHER;
    av[1] = (char *)spec;
    for (size_t i = 0; i < nargs; i++)
        av[2 + i] = args[i];
    av[2 + nargs] = 0;
    if (!envp)
        envp = __environ;

    if (!vblk.active) {
        execve(OVMX_IMGACT_LAUNCHER, av, envp);
        int e = errno;
        free(av);
        errno = e;
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        execve(OVMX_IMGACT_LAUNCHER, av, envp);
        _exit(127);
    }
    int e = errno;
    free(av);
    vfork_restore_fds();
    vblk.active = 0;
    if (pid < 0) {
        errno = e;
        return -1;                      /* no subprocess: exec*() fails */
    }
    longjmp(vblk.jb, (int)pid);         /* back to the vfork() point */
}

static int exec32(const char *spec, const unsigned int *argv32, const unsigned int *envp32)
{
    size_t n = v32_len(argv32);
    size_t na = n ? n - 1 : 0;          /* argv[0] is not passed on */
    char **args = malloc((na + 1) * sizeof *args);
    char **env = 0;
    if (!args) {
        errno = ENOMEM;
        return -1;
    }
    for (size_t i = 0; i < na; i++)
        args[i] = p32(argv32[1 + i]);
    if (envp32) {
        size_t ne = v32_len(envp32);
        env = malloc((ne + 1) * sizeof *env);
        if (!env) {
            free(args);
            errno = ENOMEM;
            return -1;
        }
        for (size_t i = 0; i < ne; i++)
            env[i] = p32(envp32[i]);
        env[ne] = 0;
    }
    int r = ovmx_exec_image(spec, args, na, env);
    free(args);
    free(env);
    return r;
}

/* 64-bit-pointer clients: native (64-bit) argument vectors. */
static int exec64(const char *spec, char *const *argv, char *const *envp)
{
    size_t n = 0;
    if (argv)
        while (argv[n])
            n++;
    return ovmx_exec_image(spec, (char **)(argv ? argv + (n ? 1 : 0) : argv),
                           n ? n - 1 : 0, (char **)envp);
}
int ovmx_execve64(const char *s, char *const *a, char *const *e) __asm__("decc$$execve64");
int ovmx_execv64(const char *s, char *const *a) __asm__("decc$$execv64");
int ovmx_execvp64(const char *s, char *const *a) __asm__("decc$$execvp64");
int ovmx_execve64(const char *s, char *const *a, char *const *e) { return exec64(s, a, e); }
int ovmx_execv64(const char *s, char *const *a) { return exec64(s, a, 0); }
int ovmx_execvp64(const char *s, char *const *a) { return exec64(s, a, 0); }

int ovmx_execve32(const char *spec, const unsigned int *argv, const unsigned int *envp)
    __asm__("decc$$execve32");
int ovmx_execv32(const char *spec, const unsigned int *argv) __asm__("decc$$execv32");
int ovmx_execvp32(const char *spec, const unsigned int *argv) __asm__("decc$$execvp32");
int ovmx_execl32(const char *spec, const char *arg0, ...) __asm__("decc$$execl32");
int ovmx_execlp32(const char *spec, const char *arg0, ...) __asm__("decc$$execlp32");
int ovmx_execle32(const char *spec, const char *arg0, ...) __asm__("decc$$execle32");
int ovmx_execle64(const char *spec, const char *arg0, ...) __asm__("decc$$execle64");

int ovmx_execve32(const char *spec, const unsigned int *argv, const unsigned int *envp)
{
    return exec32(spec, argv, envp);
}

int ovmx_execv32(const char *spec, const unsigned int *argv)
{
    return exec32(spec, argv, 0);
}

/* The image spec names the image; no PATH search applies to a VMS spec. */
int ovmx_execvp32(const char *spec, const unsigned int *argv)
{
    return exec32(spec, argv, 0);
}

static int execl_common(const char *spec, va_list ap, int with_env)  /* with_env: 1 = 32-bit envp, 2 = 64-bit */
{
    char *args[256];
    size_t n = 0;
    char *a;
    while ((a = va_arg(ap, char *)) != 0) {
        if (n == sizeof args / sizeof args[0]) {
            errno = E2BIG;
            return -1;
        }
        args[n++] = a;
    }
    if (!with_env)
        return ovmx_exec_image(spec, args, n, 0);
    if (with_env == 2)
        return ovmx_exec_image(spec, args, n, va_arg(ap, char **));
    /* execle: the environment is a DEC C (32-bit pointer) vector too. */
    const unsigned int *e32 = va_arg(ap, const unsigned int *);
    size_t ne = v32_len(e32);
    char **env = malloc((ne + 1) * sizeof *env);
    if (!env) {
        errno = ENOMEM;
        return -1;
    }
    for (size_t i = 0; i < ne; i++)
        env[i] = p32(e32[i]);
    env[ne] = 0;
    int r = ovmx_exec_image(spec, args, n, env);
    free(env);
    return r;
}

int ovmx_execl32(const char *spec, const char *arg0, ...)
{
    (void)arg0;
    va_list ap;
    va_start(ap, arg0);
    int r = execl_common(spec, ap, 0);
    va_end(ap);
    return r;
}

int ovmx_execlp32(const char *spec, const char *arg0, ...)
{
    (void)arg0;
    va_list ap;
    va_start(ap, arg0);
    int r = execl_common(spec, ap, 0);
    va_end(ap);
    return r;
}

int ovmx_execle32(const char *spec, const char *arg0, ...)
{
    (void)arg0;
    va_list ap;
    va_start(ap, arg0);
    int r = execl_common(spec, ap, 1);
    va_end(ap);
    return r;
}

int ovmx_execle64(const char *spec, const char *arg0, ...)
{
    (void)arg0;
    va_list ap;
    va_start(ap, arg0);
    int r = execl_common(spec, ap, 2);
    va_end(ap);
    return r;
}

#endif /* __alpha && __VMS */
