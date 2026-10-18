/*
 * drvlnk_test.c - vms-65a: the REAL GCC driver compiles AND links on OVMX/Alpha.
 * The launcher vfork+execv's SYS$SYSTEM:XGCC.EXE as a shell would, with
 *   xgcc -B/SYS$SYSTEM/ -L/SYS$SYSTEM -nostdinc -x c HELLO.C -o HELLO.EXE
 * (the paths in the DEC C Unix style: /SYS$SYSTEM/x is SYS$SYSTEM:X, which is how the
 * port's ld wrapper builds library paths: dir "/lib" name ".a")
 * The driver runs cc1, as and then `ld` -- the port's own wrapper (vms-ld.c), which
 * writes an options file and runs the VMS `link` command (DCL LINK -> LINK.EXE). DCL
 * then RUNs HELLO.EXE. Sentinel 7 = the driver was created, ran and exited 0.
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
extern int fflush(void *);
extern int *__errno_location(void);
int decc$$alloc_vfork_blocks(void);
void *decc$$vfork_jmpbuf(void);
int decc$$vfork_setjmp(void *) __attribute__((__returns_twice__));
#define vfork() (decc$$alloc_vfork_blocks() >= 0 ? decc$$vfork_setjmp(decc$$vfork_jmpbuf()) : -1)
int execv(const char *, cp32 const *) __asm__("decc$$execv32");

static void trace(const char *what, int v)
{
    printf("OVMX-DRV-TRACE: %s %d\n", what, v);
    fflush(0);
}

int main(void)
{
    static cp32 av[12];
    av[0] = "xgcc"; av[1] = "-B/SYS$SYSTEM/"; av[2] = "-L/SYS$SYSTEM"; av[3] = "-nostdinc";
    av[4] = "-x"; av[5] = "c"; av[6] = "VDA0:[SYSTMP]HELLO.C";
    av[7] = "-o"; av[8] = "VDA0:[SYSTMP]HELLO.EXE"; av[9] = 0;
    trace("vfork", 0);
    int pid = vfork();
    if (pid == 0) {
        int r = execv("SYS$SYSTEM:XGCC.EXE", av);
        trace("execv-returned", r);
        trace("errno", *__errno_location());
        _exit(127);
    }
    trace("vfork-parent-pid", pid);
    int pid_ok = pid > 0;
    int status = -1;
    int w = pid_ok ? waitpid(pid, &status, 0) : -1;
    trace("waitpid", w);
    trace("wait-status", status);
    int exited = w == pid && (status & 0x7f) == 0;
    int code = (status >> 8) & 0xff;
    printf("OVMX xgcc run: pid_ok=%d exited=%d code=%d status=%%X%08X\n",
           pid_ok, exited, code, (unsigned)status);
    fflush(0);
    return pid_ok && exited && code == 0 ? 7 : 3;
}
