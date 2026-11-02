/*
 * vfork_test.c - vms-fb4: DEC C vfork()/exec*() on OVMX/Alpha, 32-bit pointers.
 *
 * The parent opens two pipes, vfork()s, and -- in the "child" context between
 * vfork and exec, which DEC C runs in the parent -- redirects stdout to one and
 * stdin to the other, then execv()s this same image (SYS$SYSTEM:JOINT_E2E.EXE)
 * with arguments. exec*() creates the subprocess (SYS$CREPRC, rd vms-e70f) and
 * control returns to the vfork() point with its VMS PID; the parent's stdin and
 * stdout are back (the redirections were the child's).
 *
 * The child (argv[1] == "child") checks the arguments it received, writes one
 * line carrying its own getpid() to the pipe, waits for the parent's go on its
 * stdin, and returns 1 from main: its completion is %X1035A00A (the C RTL's
 * C$_EXIT1 with error severity, message inhibited -- measured on OpenVMS Alpha
 * V8.4, tests/lab/captures/decc-exit-alpha-20261021) -- an ERROR completion. While it waits, the parent asks the executive about it: $GETJPI
 * of the PID vfork() returned must answer that same PID (a VMS process), and
 * the child's own getpid() must be it. The parent then waits: the _VMS_WAIT
 * status (DECC$__VMS_WAITPID) must be the child's completion code %X1035A00A. When every check
 * holds the parent exits with the child's completion, so DCL's $STATUS for the
 * parent carries it; any failure exits 3 instead.
 * The client <unistd.h> declarations are repeated here (the joint harness
 * compiles with the bare cross cc1, no RTL headers).
 */
typedef unsigned int size_t;
#if defined(__VMS)
typedef char *cp32 __attribute__((__mode__(__SI__)));   /* a DEC C 32-bit pointer */
#else
typedef char *cp32;     /* host source scans compile every product file */
#endif
extern int printf(const char *, ...);
extern int strcmp(const char *, const char *);
extern int strncmp(const char *, const char *, size_t);
extern size_t strlen(const char *);
extern int pipe(int *);
extern int close(int);
extern int dup2(int, int);
extern int read(int, void *, size_t);
extern int write(int, const void *, size_t);
extern int waitpid(int, int *, int);
extern void _exit(int);
extern void exit(int);
extern int getpid(void);
extern void (*signal(int, void (*)(int)))(int);
extern int *__errno_location(void);
extern int sscanf(const char *, const char *, ...);
int vms_waitpid(int, int *, int) __asm__("decc$__vms_waitpid");
/* $GETJPIW (LIBVMS$SHR): its item list cells are 64-bit (the OVMX ABI), so the
 * 32-bit addresses this program holds are stored sign-extended. */
struct ile3_64 { unsigned short len, code; unsigned long long addr, retlen; };
extern unsigned int sys$getjpiw(unsigned int, unsigned int *, void *, void *, void *,
                                void *, unsigned int);
int decc$$alloc_vfork_blocks(void);
void *decc$$vfork_jmpbuf(void);
int decc$$vfork_setjmp(void *) __attribute__((__returns_twice__));
#define vfork() (decc$$alloc_vfork_blocks() >= 0 ? decc$$vfork_setjmp(decc$$vfork_jmpbuf()) : -1)
int execv(const char *, cp32 const *) __asm__("decc$$execv32");

#define CHILD_STATUS 0x1035A00A          /* main() returning 1: C$_EXIT1 as an error, message inhibited */

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "child") == 0) {
        write(2, "OVMX vfork child: started\n", 26);
        int ok = argc == 4 && strcmp(argv[2], "alpha") == 0 && strcmp(argv[3], "42") == 0;
        char line[64];
        int n = 0;
        const char *m = ok ? "CHILD-ARGS-OK " : "CHILD-ARGS-BAD ";
        while (*m)
            line[n++] = *m++;
        unsigned int me = (unsigned int)getpid();
        for (int sh = 28; sh >= 0; sh -= 4)
            line[n++] = "0123456789ABCDEF"[(me >> sh) & 15];
        write(1, line, n);
        char go = 0;
        int gr = read(0, &go, 1);       /* held until the parent has looked */
        char d[40] = "OVMX vfork child: go=? r=? exiting\n";
        d[21] = go ? go : '0';
        d[25] = (char)('0' + (gr & 7));
        write(2, d, 35);
        return ok ? 1 : 3;              /* main's return: the image's $EXIT */
    }

    int p[2], q[2];
    if (pipe(p) != 0 || pipe(q) != 0) {
        printf("OVMX vfork test: pipe failed\n");
        return 3;
    }
    static cp32 av[5];
    av[0] = "JOINT_E2E";
    av[1] = "child";
    av[2] = "alpha";
    av[3] = "42";
    av[4] = 0;

    signal(13, (void (*)(int))1);       /* SIGPIPE: SIG_IGN -- a dead child is a verdict, not a crash */
    int pid = vfork();
    if (pid == 0) {
        /* DEC C: this runs in the parent, on the child's behalf. */
        close(p[0]);
        dup2(p[1], 1);
        close(p[1]);
        close(q[1]);
        dup2(q[0], 0);
        close(q[0]);
        execv("SYS$SYSTEM:JOINT_E2E.EXE", av);
        _exit(127);                     /* not reached: exec returns via vfork */
    }
    int pid_ok = pid > 0;
    printf("OVMX vfork test: exec returned pid=%X errno=%d\n", (unsigned int)pid, *__errno_location());
    close(p[1]);
    close(q[0]);
    char buf[64];
    int n = 0, r;
    while (n < 22 && (r = read(p[0], buf + n, 22 - n)) > 0)
        n += r;
    buf[n] = 0;

    /* The child is alive (it waits on q): the executive knows it by that PID. */
    unsigned int jpid = 0, target = (unsigned int)pid;
    unsigned short jlen = 0;
    static struct ile3_64 il[2];
    il[0].len = 4;
    il[0].code = 0x0319;                /* JPI$_PID */
    il[0].addr = (unsigned long long)(long long)(int)(long)&jpid;
    il[0].retlen = (unsigned long long)(long long)(int)(long)&jlen;
    unsigned int jst = pid_ok ? sys$getjpiw(0, &target, 0, il, 0, 0, 0) : 0;
    int jpi_ok = (jst & 1) && jpid == (unsigned int)pid;
    unsigned int cpid = 0;
    int piped = n == 22 && strncmp(buf, "CHILD-ARGS-OK ", 14) == 0;
    if (piped) {
        for (int i = 14; i < 22; i++) {
            char c = buf[i];
            cpid = cpid * 16 + (c >= 'A' ? c - 'A' + 10 : c - '0');
        }
    }
    int getpid_ok = piped && cpid == (unsigned int)pid;
    write(q[1], "g", 1);
    close(q[1]);
    while (read(p[0], buf + 32, 16) > 0)
        ;
    close(p[0]);

    int vst = -1;
    int w = pid_ok ? vms_waitpid(pid, &vst, 0) : -1;
    int vms_ok = w == pid && (unsigned int)vst == CHILD_STATUS;
    int ok = pid_ok && piped && jpi_ok && getpid_ok && vms_ok;
    /* This line reaching the console is itself the proof that the parent's
     * stdout was restored after the child-context dup2. */
    printf("OVMX vfork diag: n=%d buf=%s jst=%X jpid=%X w=%d\n", n, buf, jst, jpid, w);
    printf("OVMX vfork test: pid_ok=%d piped=%d jpi=%d getpid=%d vmswait=%d status=%X argc=%d\n",
           pid_ok, piped, jpi_ok, getpid_ok, vms_ok, (unsigned int)vst, argc);
    if (!ok)
        return 3;
    exit(CHILD_STATUS);                 /* DCL's $STATUS carries the child's */
}
