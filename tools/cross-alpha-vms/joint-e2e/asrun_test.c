/*
 * asrun_test.c - vms-5303e: the VMS-hosted GCC pipeline's first two stages run
 * ON OVMX/Alpha, one after the other, the way the GCC driver runs them
 * (libiberty pex-unix: vfork + execv with an argument vector, wait, then the
 * next stage): cc1.exe compiles VDA0:[SYSTMP]HELLO.C to HELLO.S, then as.exe
 * (GNU as, the alpha-dec-vms port of binutils 2.43 built --host=alpha-dec-vms
 * over OVMX's C RTL) assembles HELLO.S to the EVAX object HELLO.OBJ. The gate
 * then reads HELLO.OBJ with DCL DUMP (a different accessor) and compares it with
 * the object the same toolchain, built as a cross compiler, writes for the same
 * source. Sentinel 7 = both subprocesses were created, ran and exited 0.
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

/* Progress flushed at once, so a stage that dies before its summary line is
 * still visible in the gate log. */
static void trace(const char *tag, const char *what, int v)
{
    printf("OVMX-AS-TRACE: %s %s %d\n", tag, what, v);
    fflush(0);
}

/* Run image with argv; returns 1 when it was created, exited normally with 0. */
static int stage(const char *tag, const char *image, cp32 const *av)
{
    trace(tag, "vfork", 0);
    int pid = vfork();
    if (pid == 0) {
        int r = execv(image, av);
        trace(tag, "execv-returned", r);
        trace(tag, "errno", *__errno_location());
        _exit(127);
    }
    trace(tag, "vfork-parent-pid", pid);
    int pid_ok = pid > 0;
    int status = -1;
    int w = pid_ok ? waitpid(pid, &status, 0) : -1;
    trace(tag, "waitpid", w);
    trace(tag, "wait-status", status);
    trace(tag, "errno-after-wait", *__errno_location());
    int exited = w == pid && (status & 0x7f) == 0;
    int code = (status >> 8) & 0xff;
    printf("OVMX %s run: pid_ok=%d exited=%d code=%d status=%%X%08X\n",
           tag, pid_ok, exited, code, (unsigned)status);
    fflush(0);
    trace(tag, "summary-printed", 1);
    return pid_ok && exited && code == 0;
}

int main(void)
{
    static cp32 cc[9], as[5];
    cc[0] = "cc1"; cc[1] = "-quiet"; cc[2] = "-nostdinc";
    cc[3] = "-dumpbase"; cc[4] = "hello.c"; cc[5] = "VDA0:[SYSTMP]HELLO.C";
    cc[6] = "-o"; cc[7] = "VDA0:[SYSTMP]HELLO.S"; cc[8] = 0;
    as[0] = "as"; as[1] = "-o"; as[2] = "VDA0:[SYSTMP]HELLO.OBJ";
    as[3] = "VDA0:[SYSTMP]HELLO.S"; as[4] = 0;
    int ok = stage("cc1", "SYS$SYSTEM:CC1.EXE", cc);
    if (ok)
        ok = stage("as", "SYS$SYSTEM:AS.EXE", as);
    return ok ? 7 : 3;
}
