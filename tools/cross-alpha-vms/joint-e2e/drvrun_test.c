/*
 * drvrun_test.c - vms-1eb: the REAL GCC driver (xgcc.exe, built unchanged for
 * the alpha-dec-vms host) runs ON OVMX/Alpha. The launcher does what a user's
 * shell does: vfork + execv SYS$SYSTEM:XGCC.EXE with an argument vector, then
 * waits. The driver itself finds cc1 and as (-B names SYS$SYSTEM:) and runs
 * them through libiberty pex-unix (vfork + execv, wait) -- the same pipeline
 * asrun_test.c walks by hand. The gate reads the HELLO.OBJ the driver left with
 * DCL DUMP and compares it with the object the cross driver writes.
 * Sentinel 7 = the driver was created, ran and exited 0.
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
    av[0] = "xgcc"; av[1] = "-v"; av[2] = "-BSYS$SYSTEM:"; av[3] = "-nostdinc";
    av[4] = "-x"; av[5] = "c"; av[6] = "-c"; av[7] = "VDA0:[SYSTMP]HELLO.C";
    av[8] = "-o"; av[9] = "VDA0:[SYSTMP]HELLO.OBJ"; av[10] = 0;
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
