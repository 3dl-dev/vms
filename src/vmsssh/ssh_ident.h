/*
 * ssh_ident.h - Establish the authenticated SSH session's identity in the
 * executive, fail-honest.
 *
 * vms-6ae (INV-6 / Rule 11, found by the vms-040 executive-boundary audit,
 * docs/audit-vms-040-executive-boundary.md §3.7): vmssshd authenticates a
 * user against SYSUAF, then asks the executive to establish that VMS identity
 * on the session's process (vms_kif_setident). Historically, when the
 * executive REFUSED the identity, vmssshd logged %OVMX-W-NOIDENT (a WARNING)
 * and PROCEEDED to build a local PCB and exec DCL anyway -- a network service
 * granting a privileged VMS session the executive had explicitly denied. That
 * is the exact fabrication class the project forbids (a userspace fake of an
 * executive-owned facility) AND a straight auth bypass.
 *
 * The console path (tools/vms_login.c / LOGINOUT) already fails honest here:
 * on refusal it prints %OVMX-F-NOIDENT and _exit(1)s, constructing no local
 * identity. This is the SSH mirror of that decision, factored out (like
 * cred_drop.c) so the security-critical policy -- refusal DENIES the session,
 * never a local-PCB fallback -- can be unit-tested with an injected setident
 * that returns a refusal, WITHOUT needing a live /dev/vms or libssh.
 */

#ifndef OVMX_VMSSSH_SSH_IDENT_H
#define OVMX_VMSSSH_SSH_IDENT_H

#include <stdint.h>

/*
 * Injectable identity call. Production wires this to the real
 * vms_kif_setident() (ovmx_ident_real_syscalls, defined in vmssshd.c where
 * libvmssys is linked); the unit test wires it to a mock that can be made to
 * refuse (return an even/error status) or accept (return an odd/success
 * status), so the fail-honest policy is exercised without a live executive.
 *
 * The function returns a VMS status code: odd = accepted, even = refused,
 * exactly as vms_kif_setident() does.
 */
struct ovmx_ident_syscalls {
    uint32_t (*fn_setident)(const char *username, uint32_t uic,
                            uint64_t authorized_privs);
};

/* The production syscall table (real vms_kif_setident). Defined in vmssshd.c. */
extern const struct ovmx_ident_syscalls ovmx_ident_real_syscalls;

/*
 * Ask the executive to establish the authenticated identity [username, uic,
 * authorized_privs] on the calling process, and REPORT whether it accepted.
 *
 * Returns 0 if the executive ACCEPTED the identity (odd status). Returns -1 if
 * the executive REFUSED it (even status) -- including the /dev/vms-absent case,
 * where the ioctl fails and the status is even. The caller MUST treat -1 as
 * fatal and DENY the session: no local PCB, no shell, connection refused.
 * Failing closed on refusal is the whole point of this fix (INV-6).
 *
 * On refusal, *out_status (if non-NULL) receives the raw VMS status the
 * executive returned, for the caller's %OVMX-F-NOIDENT diagnostic.
 *
 * Mirrors the inline identity establishment in tools/vms_login.c (LOGINOUT).
 */
int ovmx_ssh_establish_identity(const char *username, uint32_t uic,
                                uint64_t authorized_privs,
                                const struct ovmx_ident_syscalls *sc,
                                uint32_t *out_status);

#endif /* OVMX_VMSSSH_SSH_IDENT_H */
