/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ovmx_sshd_exec.c - OVMX OpenSSH sshd SESSION exec seam (rd vms-0cd / vms-29b).
 * The `--wrap=execve` half of the session shim, kept in its OWN translation unit
 * (separate from the permanently_set_uid wrap in ovmx_sshd_session.c) ON PURPOSE:
 *
 *   Every OpenSSH server binary references execve (libc), so --wrap=execve pulls
 *   THIS object into each of them -- including the sshd LISTENER, which does NOT
 *   link uidswap.o. If __wrap_execve and __wrap_permanently_set_uid shared one
 *   object, pulling it for execve would drag an unresolved __real_permanently_
 *   set_uid reference into the listener. Split, this object references only
 *   __real_execve (always present) and is safe to pull anywhere.
 *
 * THE SESSION HANDOFF, SECOND HALF (vms-29b). ovmx_sshd_pre_drop_pw (the
 * permanently_set_uid wrap) created a $CREPRC(LOGINOUT) session in the still-
 * privileged pre-drop window and stashed its vterm master fd. It could NOT pump
 * there: for a non-PTY session OpenSSH wires the ssh channel onto fd0/1 only just
 * before THIS shell execve, after the credential drop. So the pump runs here,
 * where fd0/1 ARE the channel -- ovmx_sshd_run_pump_if_pending() relays and
 * _exits (the real login shell never runs). This RETIRES the old raw-execve-of-
 * DCL session shim (vms-16b): a SYSUAF login is a LOGINOUT/$CREPRC session, not a
 * DCL image execve'd here. Every other execve (a privsep re-exec of sshd-session
 * /sshd-auth, sftp-server, ...) has no handoff pending and passes straight
 * through.
 */

#include "includes.h"

#include <sys/types.h>
#include <unistd.h>

#include "sshd_session.h"   /* ovmx_sshd_run_pump_if_pending */

extern int __real_execve(const char *path, char *const argv[],
                         char *const envp[]);

int
__wrap_execve(const char *path, char *const argv[], char *const envp[])
{
	/* If a SYSUAF login's $CREPRC(LOGINOUT) session is pending, the ssh
	 * channel is now on fd0/1 -- relay it to/from the vterm master until the
	 * session ends, then _exit. This does NOT return when a handoff is
	 * pending. Fail-closed: a pre-drop creprc failure already _exited, so a
	 * pending handoff always names a live session; nothing-pending just
	 * returns (never hangs). */
	ovmx_sshd_run_pump_if_pending();

	/* No handoff pending -- a privsep re-exec, sftp-server, etc.: pass
	 * straight through to the real execve. */
	return __real_execve(path, argv, envp);
}
