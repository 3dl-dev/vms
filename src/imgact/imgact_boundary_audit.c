/*
 * imgact_boundary_audit.c -- freestanding executive-boundary AUDIT tracer,
 * installed at the real IMGACT activation site (vms-617, Phase A under vms-040).
 *
 * IMGACT.EXE is -nostdlib/-ffreestanding: no pthread, no libc. The hosted
 * supervisor (src/boundary_audit/boundary_audit.c) uses pthread_create + stdio +
 * malloc and cannot be linked here. This file is the FREESTANDING port of the
 * SAME instrument:
 *
 *   - The seccomp-BPF classifier (which syscalls are VMS-semantic) and the JSON
 *     finding format are shared VERBATIM with the hosted module through
 *     boundary_audit_filter.{h,c} -- that .c is compiled here as an extra
 *     freestanding TU (exactly like known_images.c / imgact_acp.c), so the two
 *     supervisors can never drift.
 *
 *   - The supervisor is a raw clone() child (CLONE_FILES, fork-like COW memory,
 *     NO pthread_create) that reads the seccomp NEW_LISTENER fd, records a
 *     coalesced finding per VMS-semantic syscall, and CONTINUEs the syscall so
 *     behaviour is byte-identical with the tracer on or off. It flushes findings
 *     to the log when the audited image exits (listener POLLHUP), then exits.
 *
 * Observe-only. No blocking, no rerouting (that is enforcement = vms-48e,
 * post-1.0). Fail honest per Rule 9 / INV-6: if clone/pipe/seccomp is refused,
 * the filter is NOT armed and the image runs exactly as it would un-audited --
 * never a fake success.
 *
 * See docs/design-executive-boundary-audit-tracer.md and boundary_audit.h.
 */
#include "imgact_boundary_audit.h"

#if defined(__aarch64__)
#  include "arch/aarch64/imgact_arch.h"   /* syscall6() */
#elif defined(__x86_64__)
#  include "arch/x86_64/imgact_arch.h"
#elif defined(__alpha__)
#  include "arch/alpha/imgact_arch.h"
#else
#  error "imgact_boundary_audit: unsupported architecture"
#endif

#include "boundary_audit_filter.h"   /* shared classifier + finding format */

/* --------------------------------------------------------------------------
 * Extra raw syscall numbers (imgact_arch.h provides only the subset IMGACT
 * itself uses; the tracer needs a few more). Kept as BA_NR_* so there is no
 * macro clash with the arch header's SYS_* set. Phase A: x86_64 + aarch64; the
 * classifier/build_filter already fail honest on any other arch.
 * -------------------------------------------------------------------------- */
#if defined(__x86_64__)
#  define BA_NR_read         0
#  define BA_NR_close        3
#  define BA_NR_ioctl        16
#  define BA_NR_pread64      17
#  define BA_NR_write        1
#  define BA_NR_exit_group   231
#  define BA_NR_clone        56
#  define BA_NR_openat       257
#  define BA_NR_pipe2        293
#  define BA_NR_ppoll        271
#  define BA_NR_readlinkat   267
#  define BA_NR_seccomp      317
#  define BA_NR_prctl        157
#elif defined(__aarch64__)
#  define BA_NR_read         63
#  define BA_NR_close        57
#  define BA_NR_ioctl        29
#  define BA_NR_pread64      67
#  define BA_NR_write        64
#  define BA_NR_exit_group   94
#  define BA_NR_clone        220
#  define BA_NR_openat       56
#  define BA_NR_pipe2        59
#  define BA_NR_ppoll        73
#  define BA_NR_readlinkat   78
#  define BA_NR_seccomp      277
#  define BA_NR_prctl        167
#else
#  define OVMX_BA_IMGACT_NOARCH 1
#endif

#define BA_AT_FDCWD          (-100)
#define BA_PR_SET_NO_NEW_PRIVS 38
#define BA_CLONE_FILES       0x00000400UL   /* share the fd table (no CLONE_VM) */

#define BA_O_WRONLY          0x0001
#define BA_O_CREAT           0x0040
#define BA_O_APPEND          0x0400

#define BA_POLLIN            0x0001
#define BA_POLLERR           0x0008
#define BA_POLLHUP           0x0010
#define BA_POLLNVAL          0x0020

#ifdef OVMX_BA_IMGACT_NOARCH
/*
 * Unsupported arch (e.g. the alpha IMGACT build): no seccomp NR table, so fail
 * honest -- the image simply runs un-audited, never a fake (INV-6). IMGACT is
 * built for alpha too, so this TU must still COMPILE there; everything below is
 * x86_64/aarch64-only and elided. A second arch is a table add (design §Width).
 */
int imgact_boundary_audit_install(const char *image, const char *log_path)
{
	(void)image; (void)log_path;
	return -1;
}
#else

/* --------------------------------------------------------------------------
 * Freestanding syscall wrappers (built on the arch syscall6 primitive).
 * -------------------------------------------------------------------------- */
static long ba_write(int fd, const void *buf, unsigned long n)
{
	return syscall6(BA_NR_write, fd, (long)buf, (long)n, 0, 0, 0);
}
static long ba_read(int fd, void *buf, unsigned long n)
{
	return syscall6(BA_NR_read, fd, (long)buf, (long)n, 0, 0, 0);
}
static long ba_close(int fd)
{
	return syscall6(BA_NR_close, fd, 0, 0, 0, 0, 0);
}
static void ba_exit_group(int code)
{
	syscall6(BA_NR_exit_group, code, 0, 0, 0, 0, 0);
	for (;;) { }
}
static long ba_ioctl(int fd, unsigned long req, void *arg)
{
	return syscall6(BA_NR_ioctl, fd, (long)req, (long)arg, 0, 0, 0);
}
static long ba_openat(const char *path, int flags, int mode)
{
	return syscall6(BA_NR_openat, BA_AT_FDCWD, (long)path, flags, mode, 0, 0);
}
static long ba_pread(int fd, void *buf, unsigned long n, long off)
{
	return syscall6(BA_NR_pread64, fd, (long)buf, (long)n, off, 0, 0);
}
static long ba_readlinkat(const char *path, char *buf, unsigned long n)
{
	return syscall6(BA_NR_readlinkat, BA_AT_FDCWD, (long)path, (long)buf,
			(long)n, 0, 0);
}
static long ba_prctl(int op, unsigned long a)
{
	return syscall6(BA_NR_prctl, op, (long)a, 0, 0, 0, 0);
}

/* --------------------------------------------------------------------------
 * Small freestanding string/format helpers (no libc).
 * -------------------------------------------------------------------------- */
static int ba_streq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}
/* Append "/proc/<pid>/fd/<fd>" (or "/proc/<pid>/mem") into buf. */
static void ba_u_to_str(char *dst, unsigned long *pos, unsigned long v)
{
	char tmp[24];
	int i = 0;
	if (v == 0) tmp[i++] = '0';
	while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
	while (i-- > 0) dst[(*pos)++] = tmp[i];
}
static void ba_proc_path(char *out, unsigned long pid, const char *leaf,
			 unsigned long fd, int have_fd)
{
	unsigned long p = 0;
	const char *pre = "/proc/";
	for (const char *q = pre; *q; q++) out[p++] = *q;
	ba_u_to_str(out, &p, pid);
	out[p++] = '/';
	for (const char *q = leaf; *q; q++) out[p++] = *q;
	if (have_fd) {
		out[p++] = '/';
		ba_u_to_str(out, &p, fd);
	}
	out[p] = '\0';
}

/* --------------------------------------------------------------------------
 * Findings: FIXED static storage (no malloc). Coalesced by (nr,a0,a1,a2). A
 * cap overflow is LOGGED (never silently dropped -- design + INV-6).
 * -------------------------------------------------------------------------- */
#define BA_MAX_FINDINGS 512
struct ba_find {
	int nr;
	int pid;
	unsigned long a0, a1, a2;
	char path[192];
	unsigned long count;
};
static struct ba_find g_find[BA_MAX_FINDINGS];
static int g_nfind;
static int g_dropped;   /* count of findings dropped past the cap */

static const char *g_image;
static const char *g_log_path;

/* Read a NUL-terminated path from the audited image's memory via /proc/<pid>/
 * mem (best effort; yama may deny cross-process reads, in which case the path
 * is left empty and only the syscall name is recorded). */
static void ba_read_target_path(unsigned long pid, unsigned long addr,
				char *out, unsigned long outsz)
{
	out[0] = '\0';
	if (!addr || outsz == 0)
		return;
	char proc[64];
	ba_proc_path(proc, pid, "mem", 0, 0);
	long fd = ba_openat(proc, 0 /*O_RDONLY*/, 0);
	if (fd < 0)
		return;
	char buf[192];
	unsigned long want = sizeof(buf) < outsz ? sizeof(buf) : outsz;
	long n = ba_pread((int)fd, buf, want - 1, (long)addr);
	ba_close((int)fd);
	if (n <= 0)
		return;
	unsigned long i = 0;
	while (i < (unsigned long)n && i + 1 < outsz && buf[i] != '\0') {
		out[i] = buf[i];
		i++;
	}
	out[i] = '\0';
}

static void ba_record(int pid, int nr, const unsigned long *args)
{
	for (int i = 0; i < g_nfind; i++) {
		struct ba_find *e = &g_find[i];
		if (e->nr == nr && e->a0 == args[0] && e->a1 == args[1] &&
		    e->a2 == args[2]) {
			e->count++;
			return;
		}
	}
	if (g_nfind >= BA_MAX_FINDINGS) {
		g_dropped++;   /* logged at flush; never silently dropped */
		return;
	}
	struct ba_find *e = &g_find[g_nfind++];
	e->nr = nr;
	e->pid = pid;
	e->a0 = args[0];
	e->a1 = args[1];
	e->a2 = args[2];
	e->count = 1;
	e->path[0] = '\0';

	/*
	 * Enrich the open/exec family with the target path (best effort). Use
	 * the shared classifier name rather than __NR_* (this TU deliberately
	 * does not include <sys/syscall.h>, to avoid a SYS_* macro clash with
	 * imgact_arch.h). openat/openat2 carry the path in args[1]; open/creat/
	 * execve in args[0].
	 */
	const char *name = ba_sc_name(nr);
	if (ba_streq(name, "openat") || ba_streq(name, "openat2"))
		ba_read_target_path((unsigned long)pid, args[1], e->path,
				    sizeof(e->path));
	else if (ba_streq(name, "open") || ba_streq(name, "creat") ||
		 ba_streq(name, "execve"))
		ba_read_target_path((unsigned long)pid, args[0], e->path,
				    sizeof(e->path));
}

/*
 * The /dev/vms exemption -- the load-bearing one (design §"The one exemption").
 * seccomp-BPF cannot deref fd->path, so an ioctl on the executive device is
 * exempted HERE by resolving the caller's fd via readlink(/proc/<pid>/fd/<n>).
 * readlink needs only PTRACE_MODE_READ (yama-safe), unlike a cross-process
 * memory read. Returns 1 if this notification should be DROPPED (legitimate
 * executive path), 0 if it is a finding.
 */
static int ba_is_exempt(int pid, int nr, const unsigned long *args)
{
	if (nr == BA_NR_ioctl) {
		char proc[64], target[128];
		ba_proc_path(proc, (unsigned long)pid, "fd", args[0], 1);
		long n = ba_readlinkat(proc, target, sizeof(target) - 1);
		if (n <= 0)
			return 0;   /* cannot resolve -> record (fail visible) */
		target[n] = '\0';
		if (ba_streq(target, "/dev/vms"))
			return 1;
	}
	return 0;
}

static void ba_flush(void)
{
	if (!g_log_path || g_log_path[0] == '\0')
		return;
	if (g_nfind == 0 && g_dropped == 0)
		return;   /* clean audit -> no file littered */
	long fd = ba_openat(g_log_path, BA_O_WRONLY | BA_O_CREAT | BA_O_APPEND,
			    0644);
	if (fd < 0)
		return;   /* fail honest: no fake on a log we cannot open */
	char line[1024];
	for (int i = 0; i < g_nfind; i++) {
		struct ba_find *e = &g_find[i];
		unsigned long len = ba_format_finding(line, sizeof(line),
				g_image, e->pid, ba_sc_name(e->nr),
				e->a0, e->a1, e->a2, e->path, e->count);
		ba_write((int)fd, line, len);
	}
	if (g_dropped > 0) {
		/* Surface the cap so nothing is silently dropped (design/INV-6). */
		unsigned long len = ba_format_finding(line, sizeof(line),
				g_image, 0, "OVMX_BOUNDARY_AUDIT_CAP",
				(unsigned long)BA_MAX_FINDINGS,
				(unsigned long)g_dropped, 0,
				"findings past cap were dropped",
				(unsigned long)g_dropped);
		ba_write((int)fd, line, len);
	}
	ba_close((int)fd);
}

/* The supervisor loop: runs in the raw-clone child. `lfd` is the seccomp
 * NEW_LISTENER fd (valid via the shared fd table). Never returns. */
static void ba_supervise(int lfd)
{
	struct seccomp_notif_sizes sizes = { 0, 0, 0 };
	if (syscall6(BA_NR_seccomp, SECCOMP_GET_NOTIF_SIZES, 0,
		     (long)&sizes, 0, 0, 0) < 0)
		ba_exit_group(0);

	/* The kernel structs are ABI-stable and small; static buffers avoid
	 * malloc. Guard against a kernel reporting a size we did not budget. */
	static char reqbuf[512];
	static char rspbuf[128];
	if (sizes.seccomp_notif > sizeof(reqbuf) ||
	    sizes.seccomp_notif_resp > sizeof(rspbuf))
		ba_exit_group(0);
	struct seccomp_notif      *req = (struct seccomp_notif *)reqbuf;
	struct seccomp_notif_resp *rsp = (struct seccomp_notif_resp *)rspbuf;

	/* poll timeout: 200ms, bounded so a lost POLLHUP still terminates us. */
	struct { long sec; long nsec; } ts = { 0, 200L * 1000L * 1000L };
	struct { int fd; short events; short revents; } pfd;

	for (;;) {
		pfd.fd = lfd;
		pfd.events = BA_POLLIN;
		pfd.revents = 0;
		long pr = syscall6(BA_NR_ppoll, (long)&pfd, 1, (long)&ts, 0, 8, 0);
		if (pr < 0) {
			if (pr == -4 /*EINTR*/)
				continue;
			break;
		}
		if (pr == 0)
			continue;   /* timeout -> re-check */
		if (pfd.revents & (BA_POLLERR | BA_POLLHUP | BA_POLLNVAL))
			break;      /* audited image gone */

		for (unsigned long z = 0; z < sizes.seccomp_notif; z++)
			reqbuf[z] = 0;
		if (ba_ioctl(lfd, SECCOMP_IOCTL_NOTIF_RECV, req) < 0)
			continue;   /* EINTR / target vanished mid-notify */

		int nr = req->data.nr;
		const unsigned long *args = (const unsigned long *)req->data.args;
		int pid = (int)req->pid;

		if (!ba_is_exempt(pid, nr, args))
			ba_record(pid, nr, args);

		/*
		 * CONTINUE-TOCTOU note: SECCOMP_USER_NOTIF_FLAG_CONTINUE is
		 * documented as unsafe for security ENFORCEMENT because the args
		 * can change between notify and execute. That is fine here: this
		 * is AUDIT, we record what was attempted and never gate on it.
		 * When vms-48e brings enforcement it will NOT use CONTINUE; it
		 * will resolve the syscall in the supervisor. Do not copy this
		 * pattern into an enforcement path.
		 */
		for (unsigned long z = 0; z < sizes.seccomp_notif_resp; z++)
			rspbuf[z] = 0;
		rsp->id = req->id;
		rsp->val = 0;
		rsp->error = 0;
		rsp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
		(void)ba_ioctl(lfd, SECCOMP_IOCTL_NOTIF_SEND, rsp);
	}

	ba_flush();
	ba_exit_group(0);
}

int imgact_boundary_audit_install(const char *image, const char *log_path)
{
	g_image = image ? image : "";
	g_log_path = log_path;

	/*
	 * Build the classifier BPF program (shared with the hosted module) into
	 * a stack buffer -- no malloc under -nostdlib. Fail honest on an arch we
	 * have no table for.
	 */
	struct sock_filter filter[BA_FILTER_MAX_INSNS];
	int flen = 0;
	if (ba_build_filter(filter, BA_FILTER_MAX_INSNS, &flen) != 0)
		return -1;

	/*
	 * Sync pipe: the supervisor child blocks on it until the parent has
	 * armed the filter and can hand over the listener fd number. If arming
	 * fails the parent closes the write end without sending, and the child
	 * reads EOF and exits cleanly (never a fake).
	 */
	int sync_fd[2] = { -1, -1 };
	if (syscall6(BA_NR_pipe2, (long)sync_fd, 0, 0, 0, 0, 0) < 0)
		return -1;

	/*
	 * Create the supervisor FIRST, before arming -- exactly like the hosted
	 * module. CLONE_FILES (share the fd table so the child sees the listener
	 * fd the parent creates next) WITHOUT CLONE_VM: the child gets a
	 * fork-like COW copy of memory and its own stack, so it runs plain C with
	 * no asm trampoline. Termination signal 0 => no SIGCHLD is delivered to
	 * the image (transparency) and a plain wait()/waitpid() in the image does
	 * not reap or even see the supervisor (man clone: non-SIGCHLD children
	 * need __WCLONE). The child was cloned BEFORE the seccomp filter is armed,
	 * so it carries NO filter and can freely service notifications.
	 */
	long child = syscall6(BA_NR_clone, (long)BA_CLONE_FILES, 0, 0, 0, 0, 0);
	if (child < 0) {
		ba_close(sync_fd[0]);   /* clone failed: no child; safe to close */
		ba_close(sync_fd[1]);
		return -1;
	}
	if (child == 0) {
		/*
		 * ---- supervisor child ----
		 * CLONE_FILES shares the fd table with the parent, so we must NOT
		 * close one pipe end here (that would also close it in the parent
		 * mid-handshake -- the classic fork/pipe close-your-end idiom does
		 * NOT apply to a shared table). Just block on the read until the
		 * parent hands over the listener fd (or a -1 sentinel on failure),
		 * THEN close both ends (the parent has finished with them by then)
		 * and either supervise or exit -- never a fake either way.
		 */
		int lfd = -1;
		long n = ba_read(sync_fd[0], &lfd, sizeof(lfd));
		ba_close(sync_fd[0]);
		ba_close(sync_fd[1]);
		if (n != (long)sizeof(lfd) || lfd < 0)
			ba_exit_group(0);   /* arming failed: nothing to audit */
		ba_supervise(lfd);       /* never returns */
		ba_exit_group(0);
	}

	/*
	 * ---- parent (the thread that will become the activated image) ----
	 * Do NOT close either pipe end here: the fd table is shared and the child
	 * owns the final close. ALWAYS write a value so the child never blocks --
	 * the armed listener fd on success, or -1 on any failure so the child
	 * exits cleanly and the image runs un-audited (fail honest, INV-6).
	 */
	int lfd_i = -1;
	if (ba_prctl(BA_PR_SET_NO_NEW_PRIVS, 1) == 0) {
		struct sock_fprog prog;
		prog.len = (unsigned short)flen;
		prog.filter = filter;
		long lfd = syscall6(BA_NR_seccomp, SECCOMP_SET_MODE_FILTER,
				    SECCOMP_FILTER_FLAG_NEW_LISTENER,
				    (long)&prog, 0, 0, 0);
		if (lfd >= 0)
			lfd_i = (int)lfd;
	}
	(void)ba_write(sync_fd[1], &lfd_i, sizeof(lfd_i));
	return lfd_i >= 0 ? 0 : -1;
}

#endif /* !OVMX_BA_IMGACT_NOARCH */
