/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ovmx_sshd_exec.c - OVMX OpenSSH sshd SESSION exec seam (rd vms-0cd, RUNG-3
 * step 3c, design §B). The `--wrap=execve` half of the session shim, kept in its
 * OWN translation unit (separate from the permanently_set_uid wrap in
 * ovmx_sshd_session.c) ON PURPOSE:
 *
 *   Every OpenSSH server binary references execve (libc), so --wrap=execve pulls
 *   THIS object into each of them -- including the sshd LISTENER, which does NOT
 *   link uidswap.o. If __wrap_execve and __wrap_permanently_set_uid shared one
 *   object, pulling it for execve would drag an unresolved __real_permanently_
 *   set_uid reference into the listener. Split, this object references only
 *   __real_execve (always present) and is safe to pull anywhere.
 *
 * When do_child() execs the login shell and it is DCL (pw_shell set by
 * __wrap_getpwnam), rewrite OpenSSH's shell argv into the LOGINOUT->DCL argv for
 * the login user, honouring the account's LGICMD / captive flag. Every other
 * execve (sshd re-exec of sshd-session, sftp-server, ...) passes straight
 * through.
 */

#include "includes.h"

#include <sys/types.h>
#include <unistd.h>
#include <string.h>

#include "sshd_auth.h"   /* ovmx_sshd_is_dcl_path / ovmx_sshd_dcl_login_argv */

extern int __real_execve(const char *path, char *const argv[],
                         char *const envp[]);

int
__wrap_execve(const char *path, char *const argv[], char *const envp[])
{
	const char *user = NULL;
	const char *nargv[8];
	char lgicmd[256];
	const char *dcl;
	size_t i;

	if (!ovmx_sshd_is_dcl_path(path))
		return __real_execve(path, argv, envp);

	/* Find the login user in the environment (USER, then LOGNAME). */
	if (envp != NULL) {
		for (i = 0; envp[i] != NULL; i++) {
			if (strncmp(envp[i], "USER=", 5) == 0) {
				user = envp[i] + 5;
				break;
			}
			if (user == NULL && strncmp(envp[i], "LOGNAME=", 8) == 0)
				user = envp[i] + 8;
		}
	}

	if (user != NULL) {
		dcl = ovmx_sshd_dcl_login_argv(user, lgicmd, sizeof(lgicmd),
		                               nargv, sizeof(nargv) /
		                               sizeof(nargv[0]));
		if (dcl != NULL)
			return __real_execve(dcl, (char *const *)nargv, envp);
	}

	/* Could not resolve the user/record: fail honestly rather than exec a
	 * shell we cannot turn into a DCL session. execve DCL with OpenSSH's own
	 * argv still lands in DCL (it just ignores the unknown args). */
	return __real_execve(path, argv, envp);
}
