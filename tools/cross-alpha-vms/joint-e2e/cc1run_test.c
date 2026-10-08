/*
 * cc1run_test.c - vms-9a63: the VMS-hosted GCC compiler proper (cc1.exe, the
 * alpha-dec-vms port of GCC 14.2 built --host=alpha-dec-vms over OVMX's C RTL)
 * compiles a C file ON OVMX/Alpha.
 *
 * This 32-bit DEC C program does what the GCC driver does to run cc1 (gcc.cc
 * via libiberty pex-unix: vfork + execv with an argument vector): it runs
 * SYS$SYSTEM:CC1.EXE on VDA0:[SYSTMP]HELLO.C (written by DCL in the proof
 * SYSTARTUP) with -o VDA0:[SYSTMP]HELLO.S and waits for it. The gate then TYPEs
 * HELLO.S with DCL and compares it with what the same GCC, built as a cross
 * compiler, writes for the same source.
 * Sentinel 7 = the subprocess was created, ran and was reaped with a success
 * exit. The client <unistd.h> declarations are repeated here (the joint harness
 * compiles with the bare cross cc1, no RTL headers).
 */
typedef unsigned int size_t;
#if defined(__VMS)
typedef char *cp32 __attribute__((__mode__(__SI__)));   /* a DEC C 32-bit pointer */
#else
typedef char *cp32;     /* host source scans compile every product file */
#endif
extern int printf(const char *, ...);
extern int waitpid(int, int *, int);
extern void _exit(int);
int decc$$alloc_vfork_blocks(void);
void *decc$$vfork_jmpbuf(void);
int decc$$vfork_setjmp(void *) __attribute__((__returns_twice__));
#define vfork() (decc$$alloc_vfork_blocks() >= 0 ? decc$$vfork_setjmp(decc$$vfork_jmpbuf()) : -1)
int execv(const char *, cp32 const *) __asm__("decc$$execv32");

int main(void)
{
    /* -nostdinc: the source includes nothing, so cc1 is not sent to look at
     * the standard include directories (/gnu/alpha-dec-vms/include,
     * /usr/include); stat() of a path on a device that does not exist is a
     * separate rung of the C RTL (it must fail ENOENT, see vms-9a63's
     * follow-up), not what this proof is about. */
    static cp32 av[9];
    av[0] = "cc1";
    av[1] = "-quiet";
    av[2] = "-nostdinc";
    av[3] = "-dumpbase";
    av[4] = "hello.c";
    av[5] = "VDA0:[SYSTMP]HELLO.C";
    av[6] = "-o";
    av[7] = "VDA0:[SYSTMP]HELLO.S";
    av[8] = 0;
    int pid = vfork();
    if (pid == 0) {
        /* DEC C: this runs in the parent, on the child's behalf. */
        execv("SYS$SYSTEM:CC1.EXE", av);
        _exit(127);                     /* not reached: exec returns via vfork */
    }
    int pid_ok = pid > 0;
    int status = -1;
    int w = pid_ok ? waitpid(pid, &status, 0) : -1;
    int exited = w == pid && (status & 0x7f) == 0;
    int code = (status >> 8) & 0xff;
    printf("OVMX cc1 run: pid_ok=%d exited=%d code=%d status=%%X%08X\n",
           pid_ok, exited, code, (unsigned)status);
    return pid_ok && exited && code == 0 ? 7 : 3;
}
