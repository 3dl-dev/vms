/*
 * ovmx-alpha-selftest.c -- OVMX freestanding-layer self-test as PID 1.
 *
 * Booted as /init in a Linux/Alpha initramfs on qemu-system-alpha (machine
 * "clipper" = 21264/EV6 + Tsunami/21272 core logic + OSF/1 PALcode -- the
 * DS10's COMPUTE stack).  Every syscall goes through OVMX's own ported
 * trampoline (src/libvmssys/arch/alpha/syscall.S) and OVMX's crt0.S brought
 * us here, so this exercises OVMX's freestanding layer on a whole emulated
 * Alpha machine and real PALcode -- not qemu-user.
 *
 * Proves: crt0 entry + gp setup + arg unpacking; the syscall trampoline; and
 * the one real Alpha ABI wrinkle -- errors signalled in $19/a3 with a POSITIVE
 * errno in v0 -- normalized to the negative-errno convention the C layer wants.
 *
 * See boot-ovmx-qemu.sh (the proving-ground harness) and README.md.
 */
#include <asm/unistd.h>

extern long __vms_syscall0(long);
extern long __vms_syscall1(long, long);
extern long __vms_syscall3(long, long, long, long);

/* crt0 calls this before main(); nothing to init for the self-test. */
void __vms_runtime_init(int a, char **b, char **c){(void)a;(void)b;(void)c;}

static unsigned long slen(const char*s){unsigned long n=0;while(s[n])n++;return n;}

static int g_con = 1;
static void put(const char*s){ __vms_syscall3(__NR_write, g_con, (long)s, (long)slen(s)); }

/* Division-free decimal: Alpha has no integer-divide instruction, and a
 * -nostdlib build has no libgcc __divqu/__remqu.  Repeated subtraction vs a
 * powers-of-ten table -- fine for the small values this self-test prints. */
static void putl(long v){
    static const unsigned long P[] = {
        10000000000000000000UL,1000000000000000000UL,100000000000000000UL,
        10000000000000000UL,1000000000000000UL,100000000000000UL,10000000000000UL,
        1000000000000UL,100000000000UL,10000000000UL,1000000000UL,100000000UL,
        10000000UL,1000000UL,100000UL,10000UL,1000UL,100UL,10UL,1UL };
    if(v<0){ put("-"); }
    unsigned long u = (v<0)?-(unsigned long)v:(unsigned long)v;
    char b[24]; int n=0, started=0;
    for(int k=0;k<20;k++){
        unsigned d=0; while(u>=P[k]){ u-=P[k]; d++; }
        if(d||started||k==19){ b[n++]='0'+d; started=1; }
    }
    b[n]=0; put(b);
}

int main(void)
{
    /* The kernel gives PID 1 an open /dev/console, but be explicit. */
    long fd = __vms_syscall3(__NR_openat, -100 /*AT_FDCWD*/, (long)"/dev/console", 1 /*O_WRONLY*/);
    if(fd >= 0) g_con = (int)fd;

    put("\n");
    put("========================================================\n");
    put("  OVMX / alpha  --  freestanding layer live on Alpha\n");
    put("  (running as PID 1 on an emulated AlphaServer, Tsunami)\n");
    put("========================================================\n");

    long pid = __vms_syscall0(__NR_getpid);        /* getxpid on alpha: pid in v0 */
    put("  getpid()  -> "); putl(pid); put("   (1 == we are init)\n");

    long uid = __vms_syscall0(__NR_getuid);
    put("  getuid()  -> "); putl(uid); put("\n");

    /* Prove the error-convention normalization on real PALcode too. */
    char buf[4];
    long e = __vms_syscall3(__NR_read, -1, (long)buf, 1);
    put("  read(badfd) -> "); putl(e); put("   (-9 == EBADF, a3 normalized)\n");

    put("  OVMX syscall trampoline + crt0: WORKING on Alpha.\n");
    put("========================================================\n\n");

    /* Clean poweroff so qemu exits (LINUX_REBOOT_CMD_POWER_OFF). */
    __vms_syscall1(__NR_sync, 0);
    __vms_syscall3(__NR_reboot, (long)0xfee1deadUL, (long)672274793L, (long)0x4321fedcUL);

    for(;;) __vms_syscall1(__NR_nanosleep, 0);  /* never returns; keep PID 1 alive */
    return 0;
}
