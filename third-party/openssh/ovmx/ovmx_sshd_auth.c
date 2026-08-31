/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ovmx_sshd_auth.c - OVMX OpenSSH sshd AUTH seam (rd vms-0cd, RUNG-3 step 3c,
 * design §A). A thin adapter that routes UNMODIFIED OpenSSH's two account hooks
 * to the binary SYSUAF, keeping the OpenSSH source untouched:
 *
 *   - sys_auth_passwd()  (selected by -DCUSTOM_SYS_AUTH_PASSWD): password auth
 *     against SYSUAF (Purdy) + login-flag enforcement -- NOT /etc/shadow, NOT
 *     PAM. Contract: return 1 = accept, 0 = reject.
 *   - __wrap_getpwnam()  (selected by -Wl,--wrap=getpwnam): resolve the login
 *     account from SYSUAF so the whole session runs as the VMS user (UIC ->
 *     uid/gid, pw_shell = DCL). Non-SYSUAF names (the privsep 'sshd'/'root'
 *     accounts) fall through to the real getpwnam.
 *
 * All decision logic lives in src/vmsssh/sshd_auth.c (unit-tested, OpenSSH-free,
 * fail-closed INV-6); this file only bridges OpenSSH's types to it.
 */

#include "includes.h"

#include <sys/types.h>
#include <pwd.h>
#include <string.h>

/* The prerequisite prefix auth.h needs (mirrors auth-passwd.c's include order:
 * auth.h references HostStatus/struct sshkey, so hostfile.h + sshkey.h precede
 * it, and packet.h defines struct ssh which carries ->authctxt). */
#include "packet.h"
#include "sshbuf.h"
#include "ssherr.h"
#include "log.h"
#include "misc.h"
#include "servconf.h"
#include "sshkey.h"
#include "hostfile.h"
#include "auth.h"       /* Authctxt: ->user, ->pw, ->valid */

#include "sshd_auth.h"  /* OVMX helper (macro-clean; no SYSUAF headers leak in) */

/* --wrap gives us the genuine libc implementations under these names. */
extern struct passwd *__real_getpwnam(const char *name);
extern struct passwd *__real_getpwuid(uid_t uid);

/*
 * OpenSSH's password-auth hook. auth_password() calls this and accepts iff it
 * returns non-zero AND authctxt->valid (getpwnam found the account). We verify
 * the client's password against the binary SYSUAF record for authctxt->user.
 */
int
sys_auth_passwd(struct ssh *ssh, const char *password)
{
	Authctxt *authctxt = ssh->authctxt;

	if (authctxt == NULL || authctxt->user == NULL || password == NULL)
		return 0;
	return ovmx_sshd_sysuaf_auth(authctxt->user, password);
}

/*
 * getpwnam -> SYSUAF. One static struct passwd + its string storage per session
 * process (OpenSSH pwcopy()s the result immediately, so a single reusable slot
 * is safe). A SYSUAF account resolves here; anything else (privsep users) falls
 * through to the real getpwnam so sshd's own machinery still works.
 */
struct passwd *
__wrap_getpwnam(const char *name)
{
	static struct passwd ovmx_pw;
	static struct ovmx_sshd_pwbufs ovmx_b;

	if (name != NULL &&
	    ovmx_sshd_fill_passwd(name, &ovmx_pw, &ovmx_b) == 0)
		return &ovmx_pw;
	return __real_getpwnam(name);
}

/*
 * getpwuid: OpenSSH looks accounts up by name for the login path; the uid path
 * is used for incidental lookups (logging, the privsep uid). A UIC member alone
 * does not carry the group, so a faithful SYSUAF reverse-map is deferred -- fall
 * through to the real getpwuid rather than fabricate a half-record. The login
 * identity is fully established via __wrap_getpwnam above.
 */
struct passwd *
__wrap_getpwuid(uid_t uid)
{
	return __real_getpwuid(uid);
}
