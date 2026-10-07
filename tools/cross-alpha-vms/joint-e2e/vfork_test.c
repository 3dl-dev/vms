/*
 * vfork_test.c - vms-fb4: DEC C vfork()/exec*() on OVMX/Alpha, 32-bit pointers.
 *
 * The parent opens a pipe, vfork()s, and -- in the "child" context between
 * vfork and exec, which DEC C runs in the parent -- redirects stdout to the
 * pipe and execv()s this same image (SYS$SYSTEM:JOINT_E2E.EXE) with arguments.
 * exec*() creates the subprocess and control returns to the vfork() point with
 * its PID; the parent's stdout is back (the redirection was the child's). The
 * child (argv[1] == "child") checks the arguments it received and writes one
 * line to the pipe; the parent reads it and waitpid()s the child.
 * Sentinel 7 = all held. The client <unistd.h> declarations are repeated here
 * (the joint harness compiles with the bare cross cc1, no RTL headers).
 */
typedef unsigned int size_t;
#if defined(__VMS)
typedef char *cp32 __attribute__((__mode__(__SI__)));   /* a DEC C 32-bit pointer */
#else
typedef char *cp32;     /* host source scans compile every product file */
#endif
extern int printf(const char *, ...);
extern int strcmp(const char *, const char *);
extern size_t strlen(const char *);
extern int pipe(int *);
extern int close(int);
extern int dup2(int, int);
extern int read(int, void *, size_t);
extern int write(int, const void *, size_t);
extern int waitpid(int, int *, int);
extern void _exit(int);
int decc$$alloc_vfork_blocks(void);
void *decc$$vfork_jmpbuf(void);
int decc$$vfork_setjmp(void *) __attribute__((__returns_twice__));
#define vfork() (decc$$alloc_vfork_blocks() >= 0 ? decc$$vfork_setjmp(decc$$vfork_jmpbuf()) : -1)
int execv(const char *, cp32 const *) __asm__("decc$$execv32");

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "child") == 0) {
        int ok = argc == 4 && strcmp(argv[2], "alpha") == 0 && strcmp(argv[3], "42") == 0;
        const char *m = ok ? "CHILD-ARGS-OK" : "CHILD-ARGS-BAD";
        write(1, m, strlen(m));
        return ok ? 1 : 3;              /* VMS success / a failure status */
    }

    int p[2];
    if (pipe(p) != 0) {
        printf("OVMX vfork test: pipe failed\n");
        return 3;
    }
    static cp32 av[5];
    av[0] = "JOINT_E2E";
    av[1] = "child";
    av[2] = "alpha";
    av[3] = "42";
    av[4] = 0;

    int pid = vfork();
    if (pid == 0) {
        /* DEC C: this runs in the parent, on the child's behalf. */
        close(p[0]);
        dup2(p[1], 1);
        close(p[1]);
        execv("SYS$SYSTEM:JOINT_E2E.EXE", av);
        _exit(127);                     /* not reached: exec returns via vfork */
    }
    int pid_ok = pid > 0;
    close(p[1]);
    char buf[64];
    int n = 0, r;
    while (n < (int)sizeof buf - 1 && (r = read(p[0], buf + n, sizeof buf - 1 - n)) > 0)
        n += r;
    buf[n] = 0;
    close(p[0]);
    int status = -1;
    int w = pid_ok ? waitpid(pid, &status, 0) : -1;
    int exited = w == pid && (status & 0x7f) == 0;
    int piped = strcmp(buf, "CHILD-ARGS-OK") == 0;
    int ok = pid_ok && exited && piped;
    /* This line reaching the console is itself the proof that the parent's
     * stdout was restored after the child-context dup2. */
    printf("OVMX vfork test: pid_ok=%d exited=%d piped=%d buf=%s argc=%d\n",
           pid_ok, exited, piped, buf, argc);
    return ok ? 7 : 3;
}
