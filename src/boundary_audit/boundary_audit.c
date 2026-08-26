/*
 * boundary_audit.c -- Executive-boundary AUDIT tracer (vms-c08, Phase A).
 *
 * See boundary_audit.h and docs/design-executive-boundary-audit-tracer.md.
 *
 * Observe-only: the seccomp filter returns SECCOMP_RET_USER_NOTIF for the
 * VMS-semantic syscall set; the supervisor records a finding and continues the
 * syscall with SECCOMP_USER_NOTIF_FLAG_CONTINUE. No blocking, no rerouting.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "boundary_audit.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>

/* -------- arch selection (Phase A: x86_64; a second arch is a table add) ---- */
#if defined(__x86_64__)
#  define OVMX_BA_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#  define OVMX_BA_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#  define OVMX_BA_UNSUPPORTED_ARCH 1
#endif

/* -------- seccomp user-notif ioctl / struct fallbacks (older headers) ------- */
#ifndef SECCOMP_IOC_MAGIC
#  define SECCOMP_IOC_MAGIC '!'
#endif
#ifndef SECCOMP_IOWR
#  define SECCOMP_IOWR(nr, type) _IOWR(SECCOMP_IOC_MAGIC, (nr), type)
#endif
#ifndef SECCOMP_IOCTL_NOTIF_RECV
#  define SECCOMP_IOCTL_NOTIF_RECV SECCOMP_IOWR(0, struct seccomp_notif)
#endif
#ifndef SECCOMP_IOCTL_NOTIF_SEND
#  define SECCOMP_IOCTL_NOTIF_SEND SECCOMP_IOWR(1, struct seccomp_notif_resp)
#endif
#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#  define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)
#endif

/*
 * The classifier: the VMS-semantic syscalls that in a faithful system would
 * trap to the executive. Single source of truth for BOTH the BPF program and
 * the supervisor's name lookup, so they can never drift. __NR_* are already
 * arch-specific via <sys/syscall.h>; guard the ones absent on some arches.
 */
struct ba_sc { int nr; const char *name; };
static const struct ba_sc FILTERED[] = {
	/* process create */
#ifdef __NR_clone
	{ __NR_clone,     "clone"     },
#endif
#ifdef __NR_clone3
	{ __NR_clone3,    "clone3"    },
#endif
#ifdef __NR_fork
	{ __NR_fork,      "fork"      },
#endif
#ifdef __NR_vfork
	{ __NR_vfork,     "vfork"     },
#endif
#ifdef __NR_execve
	{ __NR_execve,    "execve"    },
#endif
#ifdef __NR_execveat
	{ __NR_execveat,  "execveat"  },
#endif
	/* file / volume */
#ifdef __NR_open
	{ __NR_open,      "open"      },
#endif
#ifdef __NR_openat
	{ __NR_openat,    "openat"    },
#endif
#ifdef __NR_openat2
	{ __NR_openat2,   "openat2"   },
#endif
#ifdef __NR_creat
	{ __NR_creat,     "creat"     },
#endif
#ifdef __NR_rename
	{ __NR_rename,    "rename"    },
#endif
#ifdef __NR_renameat
	{ __NR_renameat,  "renameat"  },
#endif
#ifdef __NR_renameat2
	{ __NR_renameat2, "renameat2" },
#endif
#ifdef __NR_unlink
	{ __NR_unlink,    "unlink"    },
#endif
#ifdef __NR_unlinkat
	{ __NR_unlinkat,  "unlinkat"  },
#endif
#ifdef __NR_mkdir
	{ __NR_mkdir,     "mkdir"     },
#endif
#ifdef __NR_mkdirat
	{ __NR_mkdirat,   "mkdirat"   },
#endif
#ifdef __NR_chmod
	{ __NR_chmod,     "chmod"     },
#endif
#ifdef __NR_fchmod
	{ __NR_fchmod,    "fchmod"    },
#endif
#ifdef __NR_fchmodat
	{ __NR_fchmodat,  "fchmodat"  },
#endif
#ifdef __NR_truncate
	{ __NR_truncate,  "truncate"  },
#endif
	/* network */
#ifdef __NR_socket
	{ __NR_socket,    "socket"    },
#endif
#ifdef __NR_socketcall
	{ __NR_socketcall,"socketcall"},
#endif
#ifdef __NR_connect
	{ __NR_connect,   "connect"   },
#endif
#ifdef __NR_bind
	{ __NR_bind,      "bind"      },
#endif
#ifdef __NR_sendto
	{ __NR_sendto,    "sendto"    },
#endif
#ifdef __NR_recvfrom
	{ __NR_recvfrom,  "recvfrom"  },
#endif
	/* device */
#ifdef __NR_ioctl
	{ __NR_ioctl,     "ioctl"     },
#endif
};
#define N_FILTERED ((int)(sizeof(FILTERED) / sizeof(FILTERED[0])))

static const char *sc_name(int nr)
{
	for (int i = 0; i < N_FILTERED; i++)
		if (FILTERED[i].nr == nr)
			return FILTERED[i].name;
	return "?";
}

/* -------- findings (supervisor-owned; coalesced by nr + scalar key args) ---- */
struct finding {
	int nr;
	int pid;
	unsigned long a0, a1, a2;   /* scalar key args */
	char path[192];             /* best-effort, for openat/open family */
	unsigned long count;
};

struct boundary_audit {
	int listener_fd;            /* seccomp NEW_LISTENER fd */
	int exempt_fd;              /* /dev/vms fd; ioctl on it is not a finding */
	char image[128];
	char log_path[512];
	int have_log;

	pthread_t sup;
	int sup_started;
	volatile int stop;

	/* findings array -- appended only by the supervisor thread */
	struct finding *f;
	size_t nf, cap;

	/* handshake: supervisor waits for the armed listener fd */
	pthread_mutex_t mtx;
	pthread_cond_t  cv;
	int armed;                  /* 1 once listener_fd is valid */
};

/* Best-effort read of a NUL-terminated path from the target's address space.
 * Same process (same mm), so process_vm_readv is cheap and safe; failure just
 * leaves an empty path. */
static void read_target_path(int pid, unsigned long remote_addr, char *out,
			     size_t outsz)
{
	out[0] = '\0';
	if (!remote_addr || outsz == 0)
		return;
	char buf[192];
	size_t want = sizeof(buf) < outsz ? sizeof(buf) : outsz;
	struct iovec local = { buf, want - 1 };
	struct iovec remote = { (void *)remote_addr, want - 1 };
	ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
	if (n <= 0)
		return;
	buf[n] = '\0';
	/* stop at first NUL */
	size_t len = strnlen(buf, (size_t)n);
	if (len >= outsz)
		len = outsz - 1;
	memcpy(out, buf, len);
	out[len] = '\0';
}

static void record(struct boundary_audit *ba, int pid, int nr,
		   const unsigned long *args)
{
	/* coalesce by (nr, a0, a1, a2) */
	for (size_t i = 0; i < ba->nf; i++) {
		struct finding *e = &ba->f[i];
		if (e->nr == nr && e->a0 == args[0] && e->a1 == args[1] &&
		    e->a2 == args[2]) {
			e->count++;
			return;
		}
	}
	if (ba->nf == ba->cap) {
		size_t ncap = ba->cap ? ba->cap * 2 : 16;
		struct finding *nf = realloc(ba->f, ncap * sizeof(*nf));
		if (!nf)
			return;   /* fail honest: drop, never fake */
		ba->f = nf;
		ba->cap = ncap;
	}
	struct finding *e = &ba->f[ba->nf++];
	memset(e, 0, sizeof(*e));
	e->nr = nr;
	e->pid = pid;
	e->a0 = args[0];
	e->a1 = args[1];
	e->a2 = args[2];
	e->count = 1;

	/* enrich file-family findings with the target path (best effort) */
#ifdef __NR_openat
	if (nr == __NR_openat)
		read_target_path(pid, args[1], e->path, sizeof(e->path));
#endif
#ifdef __NR_openat2
	if (nr == __NR_openat2)
		read_target_path(pid, args[1], e->path, sizeof(e->path));
#endif
#ifdef __NR_open
	if (nr == __NR_open)
		read_target_path(pid, args[0], e->path, sizeof(e->path));
#endif
#ifdef __NR_creat
	if (nr == __NR_creat)
		read_target_path(pid, args[0], e->path, sizeof(e->path));
#endif
#ifdef __NR_execve
	if (nr == __NR_execve)
		read_target_path(pid, args[0], e->path, sizeof(e->path));
#endif
}

static void flush_log(struct boundary_audit *ba);

static void *supervisor(void *arg)
{
	struct boundary_audit *ba = arg;

	/* Wait for the audited thread to arm the filter and publish the fd. */
	pthread_mutex_lock(&ba->mtx);
	while (!ba->armed && !ba->stop)
		pthread_cond_wait(&ba->cv, &ba->mtx);
	int lfd = ba->listener_fd;
	pthread_mutex_unlock(&ba->mtx);
	if (ba->stop || lfd < 0)
		return NULL;

	struct seccomp_notif_sizes sizes;
	memset(&sizes, 0, sizeof(sizes));
	if (syscall(SYS_seccomp, SECCOMP_GET_NOTIF_SIZES, 0, &sizes) < 0)
		return NULL;

	struct seccomp_notif      *req = calloc(1, sizes.seccomp_notif);
	struct seccomp_notif_resp *rsp = calloc(1, sizes.seccomp_notif_resp);
	if (!req || !rsp) {
		free(req);
		free(rsp);
		return NULL;
	}

	while (!ba->stop) {
		struct pollfd pfd = { .fd = lfd, .events = POLLIN };
		int pr = poll(&pfd, 1, 100 /* ms: bounded so stop is honoured */);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pr == 0)
			continue;   /* timeout -> re-check stop */
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			break;      /* listener gone */

		memset(req, 0, sizes.seccomp_notif);
		if (ioctl(lfd, SECCOMP_IOCTL_NOTIF_RECV, req) < 0) {
			if (errno == EINTR || errno == ENOENT)
				continue;   /* target vanished mid-notify */
			break;
		}

		int nr = req->data.nr;
		const unsigned long *args = (const unsigned long *)req->data.args;

		/*
		 * The /dev/vms exemption -- the whole thing hinges on it. seccomp
		 * cannot deref fd->path in BPF, so we drop the finding HERE when an
		 * ioctl targets the executive fd we were told at install time. We
		 * still send the CONTINUE response so the ioctl proceeds.
		 */
		int exempt = 0;
#ifdef __NR_ioctl
		if (nr == __NR_ioctl && ba->exempt_fd >= 0 &&
		    (int)args[0] == ba->exempt_fd)
			exempt = 1;
#endif
		if (!exempt)
			record(ba, req->pid, nr, args);

		/*
		 * CONTINUE-TOCTOU note: SECCOMP_USER_NOTIF_FLAG_CONTINUE is
		 * documented as unsafe for security ENFORCEMENT because the args
		 * can change between notify and execute. That is fine here: this
		 * is AUDIT, we record what was attempted and never gate on it.
		 * When vms-48e brings enforcement it will NOT use CONTINUE; it
		 * will resolve the syscall in the supervisor. Do not copy this
		 * pattern into an enforcement path.
		 */
		memset(rsp, 0, sizes.seccomp_notif_resp);
		rsp->id = req->id;
		rsp->val = 0;
		rsp->error = 0;
		rsp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
		if (ioctl(lfd, SECCOMP_IOCTL_NOTIF_SEND, rsp) < 0) {
			if (errno == ENOENT || errno == EINTR)
				continue;   /* target died before we answered */
			break;
		}
	}

	free(req);
	free(rsp);

	/*
	 * Flush findings from HERE, the supervisor thread -- it carries NO
	 * seccomp filter, so its fopen()/openat() is not intercepted. Doing the
	 * flush on the audited thread instead would issue a filtered openat with
	 * no supervisor left to service it, freezing that thread forever in
	 * seccomp_do_user_notification. (The audited thread must never issue a
	 * filtered syscall once the supervisor is gone.)
	 */
	flush_log(ba);
	return NULL;
}

/* Build the BPF program from FILTERED[]; caller frees. Returns len via *len. */
static struct sock_filter *build_filter(int *len)
{
#ifdef OVMX_BA_UNSUPPORTED_ARCH
	(void)len;
	return NULL;
#else
	int n = N_FILTERED;
	int total = 4 + n + 2;   /* load arch, jeq, ret(wrongarch), load nr,
				  * n compares, ret(allow), ret(notif) */
	struct sock_filter *p = calloc(total, sizeof(*p));
	if (!p)
		return NULL;
	int k = 0;
	/* verify arch; a mismatched arch is not our target -> ALLOW */
	p[k++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			offsetof(struct seccomp_data, arch));
	p[k++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
			OVMX_BA_AUDIT_ARCH, 1, 0);
	p[k++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
			SECCOMP_RET_ALLOW);
	/* load syscall nr */
	p[k++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			offsetof(struct seccomp_data, nr));
	/* one compare per filtered nr; match jumps to the USER_NOTIF ret. The
	 * default-allow ret sits between the last compare and the notif ret, so
	 * a match at compare i (0-based) skips (n-1-i) later compares + the
	 * default-allow ret => jt = n - i. */
	for (int i = 0; i < n; i++)
		p[k++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
				FILTERED[i].nr, n - i, 0);
	p[k++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
			SECCOMP_RET_ALLOW);
	p[k++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
			SECCOMP_RET_USER_NOTIF);
	*len = k;   /* == total */
	return p;
#endif
}

struct boundary_audit *boundary_audit_start(const char *image, int exempt_fd,
					    const char *log_path)
{
#ifdef OVMX_BA_UNSUPPORTED_ARCH
	/* fail honest -- no fake tracer on an arch we have no NR table for */
	errno = ENOSYS;
	return NULL;
#else
	struct boundary_audit *ba = calloc(1, sizeof(*ba));
	if (!ba)
		return NULL;
	ba->listener_fd = -1;
	ba->exempt_fd = exempt_fd;
	snprintf(ba->image, sizeof(ba->image), "%s", image ? image : "");
	if (log_path && log_path[0]) {
		snprintf(ba->log_path, sizeof(ba->log_path), "%s", log_path);
		ba->have_log = 1;
	}
	pthread_mutex_init(&ba->mtx, NULL);
	pthread_cond_init(&ba->cv, NULL);

	/*
	 * Create the supervisor FIRST -- before the filter is armed. If we
	 * armed first, this very pthread_create (a clone, a filtered syscall)
	 * would notify a supervisor that does not yet exist and block forever.
	 * The supervisor thread carries NO seccomp filter (the filter is armed
	 * only on THIS thread, no TSYNC), so it can freely issue the RECV/SEND
	 * ioctls that service notifications.
	 */
	if (pthread_create(&ba->sup, NULL, supervisor, ba) != 0) {
		pthread_mutex_destroy(&ba->mtx);
		pthread_cond_destroy(&ba->cv);
		free(ba);
		return NULL;
	}
	ba->sup_started = 1;

	int flen = 0;
	struct sock_filter *filter = build_filter(&flen);
	if (!filter)
		goto fail;

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
		free(filter);
		goto fail;
	}
	struct sock_fprog prog = { .len = (unsigned short)flen, .filter = filter };
	long lfd = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
			   SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
	free(filter);
	if (lfd < 0)
		goto fail;   /* fail honest: kernel refused the listener */

	/* Publish the armed fd and wake the supervisor. */
	pthread_mutex_lock(&ba->mtx);
	ba->listener_fd = (int)lfd;
	ba->armed = 1;
	pthread_cond_signal(&ba->cv);
	pthread_mutex_unlock(&ba->mtx);
	return ba;

fail:
	/* Tear down the supervisor we started before the failure. */
	pthread_mutex_lock(&ba->mtx);
	ba->stop = 1;
	pthread_cond_signal(&ba->cv);
	pthread_mutex_unlock(&ba->mtx);
	pthread_join(ba->sup, NULL);
	pthread_mutex_destroy(&ba->mtx);
	pthread_cond_destroy(&ba->cv);
	free(ba->f);
	free(ba);
	return NULL;
#endif
}

static void flush_log(struct boundary_audit *ba)
{
	if (!ba->have_log || ba->nf == 0)
		return;   /* nothing recorded -> no file littered (audit was clean) */
	FILE *fp = fopen(ba->log_path, "a");
	if (!fp)
		return;   /* fail honest: no fake on a log we cannot open */
	for (size_t i = 0; i < ba->nf; i++) {
		struct finding *e = &ba->f[i];
		fprintf(fp,
			"{\"image\":\"%s\",\"pid\":%d,\"syscall\":\"%s\","
			"\"key_args\":[%lu,%lu,%lu],\"path\":\"%s\","
			"\"count\":%lu}\n",
			ba->image, e->pid, sc_name(e->nr),
			e->a0, e->a1, e->a2, e->path, e->count);
	}
	fclose(fp);
}

void boundary_audit_stop(struct boundary_audit *ba)
{
	if (!ba)
		return;
	pthread_mutex_lock(&ba->mtx);
	ba->stop = 1;
	pthread_cond_signal(&ba->cv);
	pthread_mutex_unlock(&ba->mtx);
	if (ba->sup_started)
		pthread_join(ba->sup, NULL);   /* supervisor flushed the log */

	if (ba->listener_fd >= 0)
		close(ba->listener_fd);
	pthread_mutex_destroy(&ba->mtx);
	pthread_cond_destroy(&ba->cv);
	free(ba->f);
	free(ba);
}

size_t boundary_audit_finding_count(const struct boundary_audit *ba)
{
	return ba ? ba->nf : 0;
}

int boundary_audit_saw_syscall(const struct boundary_audit *ba,
			       const char *sysname)
{
	if (!ba || !sysname)
		return 0;
	for (size_t i = 0; i < ba->nf; i++)
		if (strcmp(sc_name(ba->f[i].nr), sysname) == 0)
			return 1;
	return 0;
}
