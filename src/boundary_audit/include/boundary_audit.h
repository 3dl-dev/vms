/*
 * boundary_audit.h -- Executive-boundary AUDIT tracer (vms-c08, Phase A).
 *
 * See docs/design-executive-boundary-audit-tracer.md and parent vms-040.
 *
 * OVMX images are musl-linked and issue RAW Linux syscalls (clone/open/socket/
 * ioctl), so an activated image has a live path straight to the Linux kernel
 * that BYPASSES the executive (/dev/vms). This tracer makes every such bypass
 * VISIBLE as a structured finding -- it does NOT block or reroute anything
 * (that is enforcement = vms-48e, post-1.0). Behaviour is byte-identical with
 * the tracer on or off: every filtered syscall is allowed to proceed unchanged
 * via SECCOMP_USER_NOTIF_FLAG_CONTINUE.
 *
 * Mechanism: a seccomp SECCOMP_RET_USER_NOTIF filter installed on the CALLING
 * thread, plus an in-process supervisor thread that reads each seccomp_notif,
 * records a coalesced finding, then continues the syscall. The one exemption is
 * ioctl() on the /dev/vms fd -- the legitimate executive path -- dropped in the
 * SUPERVISOR by fd number (seccomp-BPF cannot dereference the fd->path).
 *
 * INV-6 (fail honest, never fake): if seccomp user-notif is unavailable or the
 * arch is unsupported, boundary_audit_start() returns NULL. It NEVER installs a
 * userspace fake that reports success.
 */
#ifndef OVMX_BOUNDARY_AUDIT_H
#define OVMX_BOUNDARY_AUDIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct boundary_audit;

/*
 * Start the AUDIT tracer on the CALLING thread.
 *
 *   image     : label recorded in every finding (the activated image name).
 *               Copied; may be NULL ("" recorded).
 *   exempt_fd : the /dev/vms executive fd whose ioctl()s are the legitimate
 *               path and must NOT be recorded as a bypass; pass -1 for none.
 *   log_path  : file to which JSON-line findings are flushed on stop; NULL to
 *               skip the file sink (findings stay queryable in-process).
 *
 * The supervisor thread is created BEFORE the seccomp filter is armed, so a
 * process-create (clone) issued by the audited thread does not deadlock waiting
 * on a supervisor that has not started.
 *
 * Returns a handle, or NULL on failure (fail honest -- inspect errno). The
 * caller MUST invoke boundary_audit_start() before issuing the syscalls to be
 * audited; the filter is inherited by children of the calling thread.
 */
struct boundary_audit *boundary_audit_start(const char *image, int exempt_fd,
					    const char *log_path);

/*
 * Stop the supervisor thread (joins it), flush findings to log_path if set,
 * and free the handle. Safe to call with NULL. The seccomp filter itself is
 * NOT removable (seccomp filters are one-way) -- but with the supervisor gone
 * the listener fd is closed, which the kernel treats as "allow" for any pending
 * or future notification, so the audited thread keeps running unperturbed.
 */
void boundary_audit_stop(struct boundary_audit *ba);

/*
 * Number of DISTINCT findings recorded (coalesced by image,syscall,key_args).
 * Valid after boundary_audit_stop() has joined the supervisor, or at any time
 * from the audited thread (the supervisor only appends). For tests.
 */
size_t boundary_audit_finding_count(const struct boundary_audit *ba);

/* True iff any recorded finding names syscall `sysname` (e.g. "openat"). */
int boundary_audit_saw_syscall(const struct boundary_audit *ba,
			       const char *sysname);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_BOUNDARY_AUDIT_H */
