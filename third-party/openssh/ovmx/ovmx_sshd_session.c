/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ovmx_sshd_session.c - OVMX OpenSSH sshd SESSION seam (rd vms-0cd, RUNG-3 step
 * 3c, design §B). A thin adapter that turns UNMODIFIED OpenSSH's "drop to the
 * user and exec the login shell" tail into the VMS LOGINOUT->DCL sequence, via
 * two linker --wrap interposers (no OpenSSH source edit):
 *
 *   __wrap_permanently_set_uid(pw): while STILL ROOT, run the LOGINOUT pre-drop
 *       setup -- establish the executive identity (fail-closed), banner,
 *       accounting -- then call __real_permanently_set_uid, OpenSSH's own
 *       ordered/permanent/fail-closed setgid-before-setuid drop to the SYSUAF
 *       UIC. (permanently_set_uid is OpenSSH's own symbol; --wrap redirects
 *       every caller to us and renames the original __real_permanently_set_uid.)
 *
 *   __wrap_execve(path, argv, envp): when do_child() execs the login shell and
 *       it is DCL (pw_shell set by __wrap_getpwnam), rewrite argv into the
 *       LOGINOUT->DCL form { vmsdcl --login [--captive] --lgicmd <f> } for the
 *       user in the environment. Every other execve (sshd re-exec of
 *       sshd-session, sftp-server, ...) passes straight through.
 *
 * All VMS logic lives in src/vmsssh/{sshd_session,sshd_auth}.c; this file only
 * bridges OpenSSH's call sites to it.
 */

#include "includes.h"

#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>
#include <string.h>

#include "uidswap.h"       /* permanently_set_uid prototype */

#include "sshd_session.h"  /* ovmx_sshd_pre_drop_pw (macro-clean helper header) */
#include "sshd_auth.h"     /* ovmx_sshd_is_dcl_path / ovmx_sshd_dcl_login_argv  */

extern void __real_permanently_set_uid(struct passwd *pw);
extern int  __real_execve(const char *path, char *const argv[],
                          char *const envp[]);

/*
 * The credential drop. OpenSSH's do_setusercontext() calls permanently_set_uid
 * to drop to pw_uid/pw_gid (the SYSUAF UIC). We interpose the LOGINOUT pre-drop
 * work FIRST, while still privileged, then perform the real drop. A refusal in
 * the pre-drop (executive denies the identity) _exit(1)s inside
 * ovmx_sshd_pre_drop_pw -- the session is never dropped-into or exec'd.
 */
void
__wrap_permanently_set_uid(struct passwd *pw)
{
	ovmx_sshd_pre_drop_pw(pw);
	__real_permanently_set_uid(pw);
}

/*
 * The session exec. do_child() has (by now) dropped to the UIC and calls
 * execve(pw_shell=DCL, argv, env). We replace OpenSSH's shell argv with the
 * LOGINOUT->DCL argv for the login user, honouring the account's LGICMD /
 * captive flag. The username comes from the session environment (do_setup_env
 * sets USER/LOGNAME to pw_name = the upcased SYSUAF name).
 */
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
