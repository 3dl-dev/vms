/*
 * test_prog.c — proof executable for IMGACT.EXE (bead vms-913.2).
 *
 * Linked --dynamic-linker=/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE with a
 * DT_NEEDED on LIBTEST$SHR.EXE. Freestanding (-nostdlib): its only external
 * references are the four functions exported by the test shareable image,
 * reached through the PLT (exe -> lib JUMP_SLOT relocations).
 *
 * Prints "IMGACT-TEST: PASS" and exits 0 iff activation worked end to end:
 *   lib_add(40,2)==42   (exe->lib call, lib->interp call, arithmetic)
 *   get_tls_x()==42     (TLSDESC + TLS block initialized to .tdata image)
 *   get_lp()==7         (RELATIVE relocation in the shareable image)
 *   read_counter()==0   (GLOB_DAT against interpreter-provided data)
 */

extern int lib_add(int, int);
extern int get_tls_x(void);
extern int get_lp(void);
extern int read_counter(void);

#if defined(__aarch64__)
/* aarch64: syscall number in x8, args in x0..x2, `svc 0`. */
#define SC_write       64
#define SC_exit_group  94
static long syscall3(long n, long a, long b, long c)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile("svc 0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2)
			 : "memory", "cc");
	return x0;
}
#elif defined(__x86_64__)
/* x86_64: syscall number in rax, args in rdi/rsi/rdx, `syscall`. */
#define SC_write       1
#define SC_exit_group  231
static long syscall3(long n, long a, long b, long c)
{
	long r;
	__asm__ volatile("syscall"
			 : "=a"(r)
			 : "a"(n), "D"(a), "S"(b), "d"(c)
			 : "rcx", "r11", "memory");
	return r;
}
#elif defined(__alpha__)
/* Alpha: syscall number in $0 (v0), args in $16-$18 (a0-a2), `callsys`.
 * Error is out-of-band in $19 (a3); this proof harness only ever calls
 * write()/exit_group() with valid arguments, so the return value (positive
 * on success) is all it needs -- no negative-errno normalization here (see
 * src/imgact/arch/alpha/imgact_arch.h's syscall6() for that, used by
 * IMGACT.EXE itself). */
#define SC_write       4
#define SC_exit_group  405
static long syscall3(long n, long a, long b, long c)
{
	register long v0 __asm__("$0")  = n;
	register long a0 __asm__("$16") = a;
	register long a1 __asm__("$17") = b;
	register long a2 __asm__("$18") = c;
	__asm__ volatile("callsys"
			 : "+r"(v0)
			 : "r"(a0), "r"(a1), "r"(a2)
			 : "$19", "$20", "$21", "$22", "$23", "$24", "$25",
			   "$27", "$28", "memory");
	return v0;
}
#else
#error "test_prog: unsupported architecture"
#endif
static void out(const char *s)
{
	const char *p = s;
	while (*p) p++;
	syscall3(SC_write, 1, (long)s, p - s);
}
static void out_int(int v)
{
	char buf[16];
	int i = 15;
	int neg = v < 0;
	unsigned uv = neg ? (unsigned)(-v) : (unsigned)v;
	buf[i--] = 0;
	if (uv == 0)
		buf[i--] = '0';
	while (uv) { buf[i--] = '0' + (uv % 10); uv /= 10; }
	if (neg)
		buf[i--] = '-';
	out(&buf[i + 1]);
}

void _start(void)
{
	int r1 = lib_add(40, 2);
	int r2 = get_tls_x();
	int r3 = get_lp();
	int r4 = read_counter();

	int ok = (r1 == 42 && r2 == 42 && r3 == 7 && r4 == 0);

	out(ok ? "IMGACT-TEST: PASS" : "IMGACT-TEST: FAIL");
	out(" (r1="); out_int(r1);
	out(" r2=");  out_int(r2);
	out(" r3=");  out_int(r3);
	out(" r4=");  out_int(r4);
	out(")\n");

	syscall3(SC_exit_group, ok ? 0 : 1, 0, 0);
	for (;;) { }
}
