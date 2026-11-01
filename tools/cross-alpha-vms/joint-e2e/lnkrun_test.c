/*
 * lnkrun_test.c - vms-e40/vms-b59: the whole native toolchain, one boot. A
 * launcher does what the GCC driver does (libiberty pex-unix: vfork + execv with
 * an argument vector, wait, next stage): CC1.EXE compiles VDA0:[SYSTMP]HELLO.C to
 * HELLO.S, AS.EXE assembles it to HELLO.OBJ, and LINK.EXE -- OVMX's linker built
 * for the alpha-dec-vms host, the stage the cross toolchain's ld used to be --
 * links HELLO.OBJ with crt0 against DECC$SHR into HELLO.EXE. DCL then RUNs
 * HELLO.EXE. Sentinel 7 = all three stages were created, ran and exited 0.
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

static void trace(const char *tag, const char *what, int v)
{
    printf("OVMX-LNK-TRACE: %s %s %d\n", tag, what, v);
    fflush(0);
}

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
    int exited = w == pid && (status & 0x7f) == 0;
    int code = (status >> 8) & 0xff;
    printf("OVMX %s run: pid_ok=%d exited=%d code=%d status=%%X%08X\n",
           tag, pid_ok, exited, code, (unsigned)status);
    fflush(0);
    return pid_ok && exited && code == 0;
}

int main(void)
{
    static cp32 cc[9], as[5], ln[16];
    cc[0] = "cc1"; cc[1] = "-quiet"; cc[2] = "-nostdinc";
    cc[3] = "-dumpbase"; cc[4] = "hello.c"; cc[5] = "VDA0:[SYSTMP]HELLO.C";
    cc[6] = "-o"; cc[7] = "VDA0:[SYSTMP]HELLO.S"; cc[8] = 0;
    as[0] = "as"; as[1] = "-o"; as[2] = "VDA0:[SYSTMP]HELLO.OBJ";
    as[3] = "VDA0:[SYSTMP]HELLO.S"; as[4] = 0;
    ln[0] = "link"; ln[1] = "--transfer"; ln[2] = "__main";
    ln[3] = "--use"; ln[4] = "SYS$SHARE:DECC$SHR.EXE";
    ln[5] = "--use"; ln[6] = "SYS$SHARE:LIBOTS_SHR.EXE";
    ln[7] = "-o"; ln[8] = "VDA0:[SYSTMP]HELLO.EXE";
    ln[9] = "SYS$SYSTEM:CRT0.OBJ"; ln[10] = "VDA0:[SYSTMP]HELLO.OBJ";
    ln[11] = "--library"; ln[12] = "SYS$SYSTEM:STARLET.A"; ln[13] = 0;
    int ok = stage("cc1", "SYS$SYSTEM:CC1.EXE", cc);
    if (ok)
        ok = stage("as", "SYS$SYSTEM:AS.EXE", as);
    if (ok)
        ok = stage("link", "SYS$SYSTEM:LINK.EXE", ln);
    return ok ? 7 : 3;
}
