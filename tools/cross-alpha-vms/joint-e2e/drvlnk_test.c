/*
 * drvlnk_test.c - vms-65a: the REAL GCC driver compiles AND links on OVMX/Alpha.
 * The launcher vfork+execv's SYS$SYSTEM:XGCC.EXE as a shell would, with
 *   xgcc -BSYS$SYSTEM: -L/SYS$SYSTEM -nostdinc -x c HELLO.C -o HELLO.EXE
 * (-B names the VMS directory the programs and start files are in; -L is in the DEC C Unix
 * style, /SYS$SYSTEM/x = SYS$SYSTEM:X, because the port ld wrapper builds library paths as
 * dir "/lib" name ".a")
 * The driver runs cc1, as and then `ld` -- the port's own wrapper (vms-ld.c), which
 * writes an options file and runs the VMS `link` command (DCL LINK -> LINK.EXE). DCL
 * then RUNs HELLO.EXE. Sentinel 7 = the driver was created, ran and exited 0.
 * Before the driver, two system() calls (a command that works, one that does not) print
 * their values: the wrapper's link step is a system() call.
 */
typedef unsigned int size_t;
#if defined(__VMS)
typedef char *cp32 __attribute__((__mode__(__SI__)));   /* a DEC C 32-bit pointer */
#else
typedef char *cp32;     /* host source scans compile every product file */
#endif
extern int printf(const char *, ...);
extern int system(const char *);
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

/* Run image with argv; returns 1 when it was created, exited normally with 0. */
static int stage(const char *tag, const char *image, cp32 const *av)
{
    trace(tag, 0);
    int pid = vfork();
    if (pid == 0) {
        int r = execv(image, av);
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
    printf("OVMX %s run: pid_ok=%d exited=%d code=%d status=%%X%08X\n",
           tag, pid_ok, exited, code, (unsigned)status);
    fflush(0);
    return pid_ok && exited && code == 0;
}

int main(void)
{
    static cp32 av[12], dv[12];
    av[0] = "xgcc"; av[1] = "-BSYS$SYSTEM:"; av[2] = "-L/SYS$SYSTEM"; av[3] = "-nostdinc";
    av[4] = "-x"; av[5] = "c"; av[6] = "VDA0:[SYSTMP]HELLO.C";
    av[7] = "-o"; av[8] = "VDA0:[SYSTMP]HELLO.EXE"; av[9] = 0;
    /* system(): a DCL command in a subprocess, the VMS condition value back (odd =
     * success) -- what the port ld wrapper calls to run LINK. The second command
     * does not exist: its value must be a failure. */
    int ok_rc = system("$ WRITE SYS$OUTPUT \"OVMX-SYSTEM-OK\"");
    int bad_rc = system("$ NOSUCHVERB");
    printf("OVMX system: ok_rc=%d bad_rc=%d\n", ok_rc, bad_rc);
    fflush(0);
    /* what the driver would run, printed and not run (-###): the commands it builds,
     * in the gate log, for reading a wall against */
    dv[0] = av[0]; dv[1] = "-###";
    for (int i = 1; i < 9; i++) dv[i + 1] = av[i];
    dv[10] = 0;
    (void)stage("xgcc-dryrun", "SYS$SYSTEM:XGCC.EXE", dv);
    return stage("xgcc", "SYS$SYSTEM:XGCC.EXE", av) ? 7 : 3;
}
