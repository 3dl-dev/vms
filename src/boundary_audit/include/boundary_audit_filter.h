/*
 * boundary_audit_filter.h -- SHARED classifier + finding format for the
 * executive-boundary AUDIT tracer (vms-c08 / vms-617, Phase A).
 *
 * This header + its companion boundary_audit_filter.c are the SINGLE SOURCE OF
 * TRUTH for
 *   (a) the VMS-semantic syscall set (the BPF classifier), and
 *   (b) the JSON-line finding format,
 * shared by BOTH consumers so they can never drift:
 *   - the HOSTED module  src/boundary_audit/boundary_audit.c  (pthread
 *     supervisor, the unit-proof under tests/boundary_audit), and
 *   - the FREESTANDING supervisor src/imgact/imgact_boundary_audit.c compiled
 *     into IMGACT.EXE (-nostdlib, raw-clone supervisor), which installs the
 *     SAME BPF program at the real activation site.
 *
 * boundary_audit_filter.c is deliberately libc-free so the -ffreestanding
 * -nostdlib IMGACT build compiles it as an extra translation unit (exactly the
 * way IMGACT already compiles known_images.c / imgact_acp.c). It DOES use the
 * kernel UAPI headers (<linux/audit.h>, <linux/filter.h>, <linux/seccomp.h>)
 * and <sys/syscall.h> for the __NR_* numbers -- compile-time-only, no link
 * dependency, like known_images.c's <sys/mman.h>/<sys/stat.h>.
 *
 * See docs/design-executive-boundary-audit-tracer.md.
 */
#ifndef OVMX_BOUNDARY_AUDIT_FILTER_H
#define OVMX_BOUNDARY_AUDIT_FILTER_H

#include <stddef.h>

/*
 * UAPI SOURCE (vms-617). The shared classifier needs the seccomp/BPF/audit
 * types and constants. The HOSTED build (glibc, linux-libc-dev present) uses
 * the REAL kernel UAPI headers -- byte-identical to what landed. The
 * FREESTANDING IMGACT.EXE build runs in a minimal Alpine/musl container that
 * has the sys/ libc headers but NOT the linux/ kernel UAPI headers (no
 * linux-headers package), so requiring them broke the IMGACT.EXE build there.
 * When they are absent we define the small, stable ABI subset ourselves --
 * exactly IMGACT's own idiom (it already defines its own O_RDONLY / SYS_*
 * rather than pull libc headers). __has_include picks the right source with NO
 * behaviour change: the
 * hosted path is unchanged; only the header-less build gets the local defs.
 */
#if defined(__has_include)
#  if __has_include(<linux/seccomp.h>) && __has_include(<linux/filter.h>) \
      && __has_include(<linux/audit.h>) && __has_include(<linux/ioctl.h>)
#    define OVMX_BA_HAVE_LINUX_UAPI 1
#  endif
#endif

#ifdef OVMX_BA_HAVE_LINUX_UAPI
/* -------- real kernel UAPI (hosted build; unchanged) ------------------------ */
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/ioctl.h>    /* _IOWR, for the SECCOMP_IOCTL_* fallbacks below */
#include <linux/seccomp.h>

/* seccomp user-notif ioctl / flag fallbacks (older headers) */
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

#else /* !OVMX_BA_HAVE_LINUX_UAPI -- self-contained ABI subset (freestanding) */

/* Classic BPF (linux/bpf_common.h + linux/filter.h). Values are the stable
 * kernel ABI. */
struct sock_filter {
	unsigned short code;
	unsigned char  jt;
	unsigned char  jf;
	unsigned int   k;
};
struct sock_fprog {
	unsigned short      len;
	struct sock_filter *filter;
};
#define BPF_LD   0x00
#define BPF_W    0x00
#define BPF_ABS  0x20
#define BPF_JMP  0x05
#define BPF_JEQ  0x10
#define BPF_K    0x00
#define BPF_RET  0x06
#define BPF_STMT(code, k)         { (unsigned short)(code), 0, 0, (unsigned int)(k) }
#define BPF_JUMP(code, k, jt, jf) { (unsigned short)(code), (unsigned char)(jt), \
				    (unsigned char)(jf), (unsigned int)(k) }

/* seccomp (linux/seccomp.h). */
struct seccomp_data {
	int                nr;
	unsigned int       arch;
	unsigned long long instruction_pointer;
	unsigned long long args[6];
};
struct seccomp_notif_sizes {
	unsigned short seccomp_notif;
	unsigned short seccomp_notif_resp;
	unsigned short seccomp_data;
};
struct seccomp_notif {
	unsigned long long id;
	unsigned int       pid;
	unsigned int       flags;
	struct seccomp_data data;
};
struct seccomp_notif_resp {
	unsigned long long id;
	long long          val;
	int                error;
	unsigned int       flags;
};
#define SECCOMP_SET_MODE_FILTER          1
#define SECCOMP_GET_NOTIF_SIZES          3
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#define SECCOMP_RET_ALLOW                0x7fff0000U
#define SECCOMP_RET_USER_NOTIF           0x7fc00000U
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)

/* seccomp user-notif ioctl numbers. asm-generic _IOC encoding (x86_64 +
 * aarch64, the only arches this tracer supports -- see the arch guard below):
 * dir<<30 | size<<16 | type<<8 | nr, with SECCOMP_IOWR dir = 3 (READ|WRITE). */
#define OVMX_BA_IOWR(nr, type) \
	((3U << 30) | ((unsigned)sizeof(type) << 16) | ('!' << 8) | (unsigned)(nr))
#define SECCOMP_IOCTL_NOTIF_RECV OVMX_BA_IOWR(0, struct seccomp_notif)
#define SECCOMP_IOCTL_NOTIF_SEND OVMX_BA_IOWR(1, struct seccomp_notif_resp)

/* audit arch tokens (linux/audit.h): EM | __AUDIT_ARCH_64BIT | __AUDIT_ARCH_LE. */
#define AUDIT_ARCH_X86_64  0xC000003EU
#define AUDIT_ARCH_AARCH64 0xC00000B7U

#endif /* OVMX_BA_HAVE_LINUX_UAPI */

/* -------- arch selection (Phase A: x86_64; a second arch is a table add) ---- */
#if defined(__x86_64__)
#  define OVMX_BA_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#  define OVMX_BA_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#  define OVMX_BA_UNSUPPORTED_ARCH 1
#endif

/*
 * Upper bound on the BPF program length (fixed prologue + one JEQ per filtered
 * nr + two terminal rets). Comfortably above the ~31-entry classifier so a
 * consumer sizes its sock_filter buffer with this constant without depending on
 * the (private) table length. ba_build_filter() checks the real bound against
 * cap and fails honest if it is ever exceeded.
 */
#define BA_FILTER_MAX_INSNS 64

#ifdef __cplusplus
extern "C" {
#endif

/* Name of a filtered syscall nr, or "?" if not in the classifier. */
const char *ba_sc_name(int nr);

/*
 * Build the seccomp-BPF program into a caller-provided buffer (NO malloc, so
 * the freestanding path uses a stack array). `cap` is the number of
 * sock_filter slots available (>= the real length; use BA_FILTER_MAX_INSNS). On
 * success sets *out_len and returns 0. Returns -1 if the arch is unsupported
 * (fail honest) or the buffer is too small.
 */
int ba_build_filter(struct sock_filter *buf, int cap, int *out_len);

/*
 * Format ONE finding as a JSON line (with trailing newline) into `out`,
 * NUL-terminated. Returns bytes written (excluding NUL). The SINGLE finding
 * format used by both supervisors, so the schema can never drift.
 */
size_t ba_format_finding(char *out, size_t outsz,
		const char *image, int pid, const char *sysname,
		unsigned long a0, unsigned long a1, unsigned long a2,
		const char *path, unsigned long count);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_BOUNDARY_AUDIT_FILTER_H */
