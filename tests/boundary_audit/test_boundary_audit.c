/*
 * test_boundary_audit.c -- unit proof for the executive-boundary AUDIT tracer
 * (vms-c08). This is the DE-RISKING CORE + an anti-LARP instrument, so its OWN
 * test must be real: it runs raw syscalls against a genuine seccomp user-notif
 * listener, never a mock of the tracer.
 *
 * Done-condition coverage proven here (design doc §Done-condition):
 *   (1) POSITIVE  -- a raw openat on a VMS-volume-style path + a raw socket +
 *                    a raw clone each produce a finding naming the syscall and
 *                    the image.
 *   (2) NEGATIVE  -- ioctl on the exempt (/dev/vms stand-in) fd => ZERO
 *                    findings (the by-fd exemption works; not "log everything").
 *   (4) NEGCTL    -- a planted ioctl on a NON-exempt fd => finding present
 *                    (the instrument detects a bypass; it would not silently
 *                    pass one). Unit-level; the imgact-path negctl is vms-c08's
 *                    integration follow-up.
 *   (3) TRANSPARENCY (unit-level) -- with the tracer armed, a real openat+write
 *                    still lands the bytes on disk unchanged (CONTINUE does not
 *                    alter execution). The full on-vs-off image-level check is
 *                    the imgact integration follow-up.
 *
 * Each sub-test runs in a FORKED child so the seccomp filter (installed on the
 * calling thread, irreversible) never touches the ctest harness itself. The
 * child writes findings to a per-test JSON-line log; the parent reads and
 * asserts -- which also proves the file sink end-to-end.
 *
 * Rule-9 / INV-6: if the kernel refuses an unprivileged seccomp user-notif
 * listener, this exits 77 (ctest SKIP with a reason) -- never a fake PASS --
 * mirroring tests/qemu's "SKIP, never fake" idiom for an absent capability.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "boundary_audit.h"

#define VMS_VOL_PATH "/SYS$SYSDEVICE/VMS$COMMON/SYSEXE/LOGINOUT.EXE"
#define IMG_NAME     "TEST_BYPASS.EXE"

static int fails = 0;
#define CHECK(cond, msg) do {                                          \
	if (cond) {                                                    \
		printf("  ok    - %s\n", msg);                        \
	} else {                                                      \
		printf("  FAIL  - %s\n", msg);                        \
		fails++;                                              \
	}                                                             \
} while (0)

/* ---- capability probe: can we install an unprivileged user-notif listener? */
static int seccomp_usernotif_available(void)
{
	pid_t p = fork();
	if (p < 0)
		return 0;
	if (p == 0) {
		struct sock_filter f[] = {
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		};
		struct sock_fprog prog = { .len = 1, .filter = f };
		if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
			_exit(2);
		long r = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
				 SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
		_exit(r < 0 ? 1 : 0);
	}
	int st = 0;
	waitpid(p, &st, 0);
	return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* ---- log helpers (parent side) --------------------------------------------- */
static char *slurp(const char *path, size_t *outlen)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	fseek(fp, 0, SEEK_END);
	long n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (n < 0) {
		fclose(fp);
		return NULL;
	}
	char *b = malloc((size_t)n + 1);
	if (!b) {
		fclose(fp);
		return NULL;
	}
	size_t rd = fread(b, 1, (size_t)n, fp);
	b[rd] = '\0';
	fclose(fp);
	if (outlen)
		*outlen = rd;
	return b;
}

/* number of "syscall":"NAME" occurrences in the log */
static int log_has_syscall(const char *log, const char *name)
{
	char needle[64];
	snprintf(needle, sizeof(needle), "\"syscall\":\"%s\"", name);
	return log && strstr(log, needle) != NULL;
}

/* ---- the three child bodies ------------------------------------------------ */

/* POSITIVE (1) + TRANSPARENCY (3, unit-level): raw openat/socket/clone are
 * caught; a real write through CONTINUE still lands its bytes. */
static void child_positive(const char *log, const char *xfer_out)
{
	struct boundary_audit *ba =
		boundary_audit_start(IMG_NAME, -1 /* no exempt fd */, log);
	if (!ba)
		_exit(77);   /* capability vanished between probe and here */

	/* raw openat on a VMS-volume-style path (ENOENT is fine; the SYSCALL
	 * is what matters) */
	long ofd = syscall(SYS_openat, AT_FDCWD, VMS_VOL_PATH, O_RDONLY);
	if (ofd >= 0)
		close((int)ofd);

	/* raw socket */
	long sfd = syscall(SYS_socket, AF_INET, SOCK_STREAM, 0);
	if (sfd >= 0)
		close((int)sfd);

	/* raw clone (fork-equivalent: flags=SIGCHLD, NULL stack => COW child).
	 * Names the "clone" syscall in the finding. */
	long c = syscall(SYS_clone, (unsigned long)SIGCHLD, 0UL, 0UL, 0UL, 0UL);
	if (c == 0)
		syscall(SYS_exit_group, 0);   /* grandchild: leave immediately */
	if (c > 0) {
		int st = 0;
		waitpid((pid_t)c, &st, 0);
	}

	/* TRANSPARENCY: a real openat+write through the armed filter must still
	 * put the exact bytes on disk (CONTINUE did not alter execution). */
	long wfd = syscall(SYS_openat, AT_FDCWD, xfer_out,
			   O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (wfd >= 0) {
		const char *msg = "hello-executive-boundary";
		ssize_t w = write((int)wfd, msg, strlen(msg));
		(void)w;
		close((int)wfd);
	}

	boundary_audit_stop(ba);   /* flushes the log */
	_exit(0);
}

/* NEGATIVE (2): the same logical op through the executive (ioctl on the
 * exempt /dev/vms stand-in fd) yields ZERO findings. */
static void child_negative(const char *log)
{
	/* Open the exempt fd BEFORE arming, so opening it is not itself audited;
	 * /dev/null stands in for /dev/vms -- the exemption is purely by fd
	 * number, exactly as it is for the real executive fd. */
	int exempt = open("/dev/null", O_RDWR);
	if (exempt < 0)
		_exit(3);

	struct boundary_audit *ba = boundary_audit_start(IMG_NAME, exempt, log);
	if (!ba)
		_exit(77);

	/* several ioctls on the exempt fd -- all must be dropped */
	int n = 0;
	for (int i = 0; i < 5; i++)
		ioctl(exempt, FIONREAD, &n);

	boundary_audit_stop(ba);
	_exit(0);
}

/* NEGCTL (4, unit-level): a planted ioctl on a NON-exempt fd MUST surface as a
 * finding. If the tracer were disabled/mis-installed this log would be empty
 * and the parent assertion fails -- proving the instrument cannot silently pass
 * a bypass. */
static void child_negctl(const char *log)
{
	int exempt = open("/dev/null", O_RDWR);      /* the executive fd */
	int other  = open("/dev/null", O_RDWR);      /* an unrelated device fd */
	if (exempt < 0 || other < 0)
		_exit(3);

	struct boundary_audit *ba = boundary_audit_start(IMG_NAME, exempt, log);
	if (!ba)
		_exit(77);

	int n = 0;
	ioctl(exempt, FIONREAD, &n);   /* exempt: must NOT appear */
	ioctl(other,  FIONREAD, &n);   /* planted bypass: MUST appear */

	boundary_audit_stop(ba);
	_exit(0);
}

/* ---- fork/run one child, return its exit code ------------------------------ */
static int run_child(void (*body)(const char *, const char *),
		     const char *log, const char *extra)
{
	unlink(log);
	pid_t p = fork();
	if (p < 0)
		return -1;
	if (p == 0)
		body(log, extra);
	int st = 0;
	waitpid(p, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static void neg_body_wrap(const char *log, const char *extra)
{
	(void)extra;
	child_negative(log);
}
static void negctl_body_wrap(const char *log, const char *extra)
{
	(void)extra;
	child_negctl(log);
}

int main(void)
{
	printf("test_boundary_audit (vms-c08): executive-boundary AUDIT tracer\n");

	if (!seccomp_usernotif_available()) {
		printf("SKIP: unprivileged seccomp user-notif listener refused "
		       "by the kernel (needs no_new_privs + user-notif). "
		       "This is the tracer's own mechanism -- not faking a pass "
		       "(INV-6).\n");
		return 77;   /* ctest SKIP, with a reason */
	}

	char dir[] = "/tmp/bac08_XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	char logp[512], logn[512], logc[512], xfer[512];
	snprintf(logp, sizeof(logp), "%s/pos.jsonl", dir);
	snprintf(logn, sizeof(logn), "%s/neg.jsonl", dir);
	snprintf(logc, sizeof(logc), "%s/negctl.jsonl", dir);
	snprintf(xfer, sizeof(xfer), "%s/transparency.out", dir);

	/* ---------- POSITIVE (done-condition 1) ---------- */
	printf("[positive] raw openat/socket/clone are each caught:\n");
	int rc = run_child(child_positive, logp, xfer);
	if (rc == 77) {
		printf("SKIP: tracer start failed post-probe\n");
		return 77;
	}
	CHECK(rc == 0, "positive child exited cleanly");
	size_t plen = 0;
	char *plog = slurp(logp, &plen);
	CHECK(plog && plen > 0, "positive findings log is non-empty");
	CHECK(log_has_syscall(plog, "openat"), "finding names raw openat");
	CHECK(log_has_syscall(plog, "socket"), "finding names raw socket");
	CHECK(log_has_syscall(plog, "clone"),  "finding names raw clone");
	CHECK(plog && strstr(plog, "\"image\":\"" IMG_NAME "\""),
	      "finding names the image");
	CHECK(plog && strstr(plog, "VMS$COMMON"),
	      "openat finding carries the VMS-volume path");

	/* TRANSPARENCY (done-condition 3, unit-level): the real write landed */
	size_t xlen = 0;
	char *xbuf = slurp(xfer, &xlen);
	CHECK(xbuf && strcmp(xbuf, "hello-executive-boundary") == 0,
	      "CONTINUE is behaviour-transparent: real write landed unchanged");

	/* ---------- NEGATIVE (done-condition 2) ---------- */
	printf("[negative] ioctl on the exempt /dev/vms-stand-in fd is clean:\n");
	rc = run_child(neg_body_wrap, logn, NULL);
	CHECK(rc == 0, "negative child exited cleanly");
	size_t nlen = 1;
	char *nlog = slurp(logn, &nlen);
	/* the log may not exist at all (no findings => nothing flushed) */
	CHECK(nlog == NULL || nlen == 0,
	      "ZERO findings for ioctl on the exempt fd");
	free(nlog);

	/* ---------- NEGCTL (done-condition 4, unit-level) ---------- */
	printf("[negctl] a planted non-exempt ioctl bypass IS surfaced:\n");
	rc = run_child(negctl_body_wrap, logc, NULL);
	CHECK(rc == 0, "negctl child exited cleanly");
	size_t clen = 0;
	char *clog = slurp(logc, &clen);
	CHECK(clog && clen > 0 && log_has_syscall(clog, "ioctl"),
	      "planted non-exempt ioctl surfaced as a finding");
	/* exactly one finding: the exempt ioctl was dropped, the other kept */
	int count_ioctl = 0;
	if (clog) {
		const char *q = clog;
		while ((q = strstr(q, "\"syscall\":\"ioctl\"")) != NULL) {
			count_ioctl++;
			q++;
		}
	}
	CHECK(count_ioctl == 1,
	      "exactly one ioctl finding (exempt dropped, non-exempt kept)");

	free(plog);
	free(xbuf);
	free(clog);

	printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
	       fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}
