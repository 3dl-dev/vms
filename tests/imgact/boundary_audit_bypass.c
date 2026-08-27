/*
 * boundary_audit_bypass.c -- test image for the executive-boundary AUDIT tracer
 * integration proof (vms-617). A real OVMX image: freestanding (-nostdlib),
 * linked -pie with PT_INTERP=IMGACT.EXE so the kernel activates it THROUGH the
 * real IMGACT path. In main it issues RAW VMS-semantic syscalls that bypass the
 * executive (socket / openat on a VMS-volume path / clone), then does a real
 * openat+write (transparency probe), prints "BOUNDARY-BYPASS: PASS", exits 0.
 *
 * When OVMX_BOUNDARY_AUDIT=1, IMGACT arms the tracer just before transferring
 * control here, so each raw syscall above must surface as a finding -- while the
 * observable result (stdout, exit status, the written marker) stays byte-
 * identical to a run with the tracer OFF (CONTINUE does not alter execution).
 *
 * x86_64 Phase A (design §Width note): the syscall stub is x86_64.
 */
#if !defined(__x86_64__)
#error "boundary_audit_bypass.c: Phase A is x86_64 only"
#endif

#define SC_write       1
#define SC_openat      257
#define SC_close       3
#define SC_socket      41
#define SC_clone       56
#define SC_exit_group  231
#define AT_FDCWD       (-100)
#define O_WRONLY       0x0001
#define O_CREAT        0x0040
#define O_TRUNC        0x0200

/* Marker path is compiled in by the harness (-DBA_MARKER_PATH="..."); a default
 * keeps the file self-contained for a manual build. */
#ifndef BA_MARKER_PATH
#define BA_MARKER_PATH "/tmp/ovmx_ba_bypass_marker.out"
#endif

static long sc(long n, long a, long b, long c, long d, long e, long f)
{
	long r;
	register long r10 __asm__("r10") = d;
	register long r8  __asm__("r8")  = e;
	register long r9  __asm__("r9")  = f;
	__asm__ volatile("syscall"
			 : "=a"(r)
			 : "a"(n), "D"(a), "S"(b), "d"(c),
			   "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory");
	return r;
}
static void out(const char *s)
{
	const char *p = s;
	while (*p) p++;
	sc(SC_write, 1, (long)s, p - s, 0, 0, 0);
}

/* A VMS-volume-style path: a raw openat here is a genuine executive bypass. */
static const char VMSPATH[] =
	"/SYS$SYSDEVICE/VMS$COMMON/SYSEXE/LOGINOUT.EXE";

void _start(void)
{
	/* raw socket -- unambiguous bypass (no path/fd exemption possible) */
	long s = sc(SC_socket, 2 /*AF_INET*/, 1 /*SOCK_STREAM*/, 0, 0, 0, 0);
	if (s >= 0)
		sc(SC_close, s, 0, 0, 0, 0, 0);

	/* raw openat on a VMS-volume path (ENOENT is fine; the SYSCALL matters) */
	long of = sc(SC_openat, AT_FDCWD, (long)VMSPATH, 0 /*O_RDONLY*/, 0, 0, 0);
	if (of >= 0)
		sc(SC_close, of, 0, 0, 0, 0, 0);

	/* raw clone (fork-like: NULL stack => COW child); child exits at once */
	long c = sc(SC_clone, 17 /*SIGCHLD*/, 0, 0, 0, 0, 0);
	if (c == 0)
		sc(SC_exit_group, 0, 0, 0, 0, 0, 0);

	/* TRANSPARENCY probe: a real openat+write must land the exact bytes even
	 * with the tracer armed (CONTINUE did not alter execution). */
	long wf = sc(SC_openat, AT_FDCWD, (long)BA_MARKER_PATH,
		     O_WRONLY | O_CREAT | O_TRUNC, 0600, 0, 0);
	if (wf >= 0) {
		const char *m = "marker-ok";
		const char *p = m;
		while (*p) p++;
		sc(SC_write, wf, (long)m, p - m, 0, 0, 0);
		sc(SC_close, wf, 0, 0, 0, 0, 0);
	}

	out("BOUNDARY-BYPASS: PASS\n");
	sc(SC_exit_group, 0, 0, 0, 0, 0, 0);
	for (;;) { }
}
