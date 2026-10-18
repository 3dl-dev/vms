/*
 * asrun_test.c - vms-5303e, vms-e40: the VMS-hosted GCC pipeline's stages run
 * ON OVMX/Alpha, one after the other, the way the GCC driver runs them
 * (libiberty pex-unix: vfork + execv with an argument vector, wait, then the
 * next stage): cc1.exe compiles VDA0:[SYSTMP]HELLO.C to HELLO.S, then as.exe
 * (GNU as, the alpha-dec-vms port of binutils 2.43 built --host=alpha-dec-vms
 * over OVMX's C RTL) assembles HELLO.S to the EVAX object HELLO.OBJ. The gate
 * then reads HELLO.OBJ with DCL DUMP (a different accessor) and compares it with
 * the object the same toolchain, built as a cross compiler, writes for the same
 * source. Then LINK.EXE (OVMX's linker, built --host=alpha-dec-vms) links
 * HELLO.OBJ with the port's crt0/crtbegin/crtend and libgcc into HELLO.EXE, with
 * the argument vector collect2 gives ld; DCL RUNs the image. Sentinel 7 = all
 * three subprocesses were created, ran and exited 0.
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
extern int fprintf(void *, const char *, ...);
extern void *stderr;
extern int open(const char *, int, ...);
extern int dup2(int, int);
extern int pipe(int *);
extern int read(int, void *, size_t);
extern int close(int);
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
static int stage(const char *tag, const char *image, cp32 const *av, int capture)
{
    int pp[2] = { -1, -1 };
    if (capture && pipe(pp) != 0)
        pp[0] = pp[1] = -1;
    trace(tag, "vfork", 0);
    int pid = vfork();
    if (pid == 0) {
        /* the stage writes its messages into a pipe the launcher prints after the wait
         * (descriptor work between vfork and exec is the child's: the parent table is
         * restored when exec returns) */
        if (pp[1] >= 0) {
            dup2(pp[1], 1); dup2(pp[1], 2);
            close(pp[0]); close(pp[1]);
        }
        int r = execv(image, av);
        trace(tag, "execv-returned", r);
        trace(tag, "errno", *__errno_location());
        _exit(127);
    }
    trace(tag, "vfork-parent-pid", pid);
    if (pp[1] >= 0)
        close(pp[1]);
    int pid_ok = pid > 0;
    int status = -1;
    int w = pid_ok ? waitpid(pid, &status, 0) : -1;
    trace(tag, "waitpid", w);
    trace(tag, "wait-status", status);
    trace(tag, "errno-after-wait", *__errno_location());
    if (pp[0] >= 0) {
        char b[512];
        int n;
        while ((n = read(pp[0], b, sizeof b - 1)) > 0) {
            b[n] = 0;
            printf("OVMX-%s-OUT: %s", tag, b);
        }
        printf("\n");
        fflush(0);
        close(pp[0]);
    }
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
    static cp32 cc[9], as[5], lk[32];
    cc[0] = "cc1"; cc[1] = "-quiet"; cc[2] = "-nostdinc";
    cc[3] = "-dumpbase"; cc[4] = "hello.c"; cc[5] = "VDA0:[SYSTMP]HELLO.C";
    cc[6] = "-o"; cc[7] = "VDA0:[SYSTMP]HELLO.S"; cc[8] = 0;
    as[0] = "as"; as[1] = "-o"; as[2] = "VDA0:[SYSTMP]HELLO.OBJ";
    as[3] = "VDA0:[SYSTMP]HELLO.S"; as[4] = 0;
    /* the link line collect2 gives ld for a C program (gcc -### hello.c), as ovmx-ld
     * maps it onto LINK.EXE: crt0/crtbegin, the object, libgcc, crtend, then the
     * C RTL and the OVMX shareables as producers and STARLET as the last library */
    {
        int n = 0;
        lk[n++] = "link"; lk[n++] = "--transfer"; lk[n++] = "__main";
        lk[n++] = "--use"; lk[n++] = "SYS$SHARE:DECC$SHR.EXE";
        lk[n++] = "--use"; lk[n++] = "SYS$SHARE:LIBOTS_SHR.EXE";
        lk[n++] = "--use"; lk[n++] = "SYS$SHARE:LIBVMSRMS$SHR.EXE";
        lk[n++] = "--use"; lk[n++] = "SYS$SHARE:LIBVMS$SHR.EXE";
        lk[n++] = "-o"; lk[n++] = "VDA0:[SYSTMP]HELLO.EXE";
        lk[n++] = "VDA0:[SYSTMP]VMS-DWARF2EH.O";
        lk[n++] = "VDA0:[SYSTMP]CRT0.O";
        lk[n++] = "VDA0:[SYSTMP]CRTBEGIN.O";
        lk[n++] = "VDA0:[SYSTMP]HELLO.OBJ";
        lk[n++] = "--library"; lk[n++] = "VDA0:[SYSTMP]LIBGCC.A";
        lk[n++] = "VDA0:[SYSTMP]CRTEND.O";
        lk[n++] = "--library"; lk[n++] = "VDA0:[SYSTMP]STARLET.A";
        lk[n] = 0;
    }
    fprintf(stderr, "OVMX-STDERR-PROBE: this line is written to stderr by the launcher\n");
    fflush(0);
    {
        /* a link with no input: LINK.EXE must say so on stderr, which the pipe carries --
         * proof that a message from the real link below would be seen */
        static cp32 nolk[2];
        nolk[0] = "link"; nolk[1] = 0;
        (void)stage("link-noargs-pipe", "SYS$SYSTEM:LINK.EXE", nolk, 1);
        (void)stage("link-noargs-console", "SYS$SYSTEM:LINK.EXE", nolk, 0);
    }
    int ok = stage("cc1", "SYS$SYSTEM:CC1.EXE", cc, 0);
    if (ok)
        ok = stage("as", "SYS$SYSTEM:AS.EXE", as, 0);
    if (ok)
        ok = stage("link", "SYS$SYSTEM:LINK.EXE", lk, 1);
    return ok ? 7 : 3;
}
