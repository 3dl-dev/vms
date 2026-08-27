/*
 * boundary_audit_filter.c -- SHARED classifier + finding format for the
 * executive-boundary AUDIT tracer (vms-c08 / vms-617, Phase A).
 *
 * The SINGLE source of the VMS-semantic syscall table, the seccomp-BPF program
 * builder, and the JSON finding format. Compiled BOTH into the hosted
 * boundary_audit library (pthread supervisor + unit test) AND as an extra
 * -ffreestanding -nostdlib translation unit of IMGACT.EXE (raw-clone
 * supervisor), so the two supervisors can never drift.
 *
 * LIBC-FREE by construction (no malloc/stdio/string.h): every routine here must
 * be safe under IMGACT's -ffreestanding -nostdlib build. See the header.
 */
#include "boundary_audit_filter.h"

#include <sys/syscall.h>   /* __NR_* (compile-time only; no link dependency) */

/*
 * The classifier: the VMS-semantic syscalls that in a faithful system would
 * trap to the executive. Everything else -- pure compute / memory / thread
 * (mmap, mprotect, brk, futex, the rt_sig family, the clock family, nanosleep,
 * exit) -- is SECCOMP_RET_ALLOW and never notified, so overhead is near-zero.
 * __NR_* are already arch-specific via <sys/syscall.h>; guard the ones absent
 * on an arch.
 */
struct ba_sc { int nr; const char *name; };

static const struct ba_sc ba_filtered[] = {
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
#define BA_N_FILTERED ((int)(sizeof(ba_filtered) / sizeof(ba_filtered[0])))

const char *ba_sc_name(int nr)
{
	for (int i = 0; i < BA_N_FILTERED; i++)
		if (ba_filtered[i].nr == nr)
			return ba_filtered[i].name;
	return "?";
}

/*
 * Build the classifier BPF program. Logic is IDENTICAL to the original hosted
 * build_filter(): verify arch (a mismatch is not our target -> ALLOW), load the
 * syscall nr, one JEQ per filtered nr jumping to the terminal USER_NOTIF ret,
 * with a default ALLOW ret in between. A match at compare i (0-based) must skip
 * (n-1-i) later compares + the default-allow ret, so its jt = n - i.
 */
int ba_build_filter(struct sock_filter *buf, int cap, int *out_len)
{
#ifdef OVMX_BA_UNSUPPORTED_ARCH
	(void)buf; (void)cap; (void)out_len;
	return -1;
#else
	int n = BA_N_FILTERED;
	int total = 4 + n + 2;
	if (!buf || cap < total)
		return -1;
	int k = 0;
	/* verify arch; a mismatched arch is not our target -> ALLOW */
	buf[k++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			offsetof(struct seccomp_data, arch));
	buf[k++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
			OVMX_BA_AUDIT_ARCH, 1, 0);
	buf[k++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
			SECCOMP_RET_ALLOW);
	/* load syscall nr */
	buf[k++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			offsetof(struct seccomp_data, nr));
	for (int i = 0; i < n; i++)
		buf[k++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
				ba_filtered[i].nr, n - i, 0);
	buf[k++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
			SECCOMP_RET_ALLOW);
	buf[k++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
			SECCOMP_RET_USER_NOTIF);
	if (out_len)
		*out_len = k;   /* == total */
	return 0;
#endif
}

/* -------- shared, libc-free finding formatter ------------------------------- */
static void ba_fmt_u(char *buf, size_t cap, size_t *pos, unsigned long v)
{
	char tmp[24];
	int i = 0;
	if (v == 0)
		tmp[i++] = '0';
	while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
	while (i-- > 0 && *pos + 1 < cap)
		buf[(*pos)++] = tmp[i];
}
static void ba_fmt_d(char *buf, size_t cap, size_t *pos, long v)
{
	if (v < 0 && *pos + 1 < cap) {
		buf[(*pos)++] = '-';
		ba_fmt_u(buf, cap, pos, (unsigned long)(-v));
	} else {
		ba_fmt_u(buf, cap, pos, (unsigned long)v);
	}
}
static void ba_fmt_s(char *buf, size_t cap, size_t *pos, const char *s)
{
	if (!s)
		return;
	while (*s && *pos + 1 < cap)
		buf[(*pos)++] = *s++;
}

/*
 * NB: paths are copied verbatim; the audited paths are OVMX volume paths and
 * syscall args, which never contain a double-quote in practice -- Phase A does
 * not JSON-escape (the consumers grep by substring). Keep this in sync if the
 * consumer ever parses strictly.
 */
size_t ba_format_finding(char *out, size_t outsz,
		const char *image, int pid, const char *sysname,
		unsigned long a0, unsigned long a1, unsigned long a2,
		const char *path, unsigned long count)
{
	size_t p = 0;
	ba_fmt_s(out, outsz, &p, "{\"image\":\"");
	ba_fmt_s(out, outsz, &p, image ? image : "");
	ba_fmt_s(out, outsz, &p, "\",\"pid\":");
	ba_fmt_d(out, outsz, &p, pid);
	ba_fmt_s(out, outsz, &p, ",\"syscall\":\"");
	ba_fmt_s(out, outsz, &p, sysname ? sysname : "?");
	ba_fmt_s(out, outsz, &p, "\",\"key_args\":[");
	ba_fmt_u(out, outsz, &p, a0);
	ba_fmt_s(out, outsz, &p, ",");
	ba_fmt_u(out, outsz, &p, a1);
	ba_fmt_s(out, outsz, &p, ",");
	ba_fmt_u(out, outsz, &p, a2);
	ba_fmt_s(out, outsz, &p, "],\"path\":\"");
	ba_fmt_s(out, outsz, &p, path ? path : "");
	ba_fmt_s(out, outsz, &p, "\",\"count\":");
	ba_fmt_u(out, outsz, &p, count);
	ba_fmt_s(out, outsz, &p, "}\n");
	if (p < outsz)
		out[p] = '\0';
	else if (outsz)
		out[outsz - 1] = '\0';
	return p;
}
