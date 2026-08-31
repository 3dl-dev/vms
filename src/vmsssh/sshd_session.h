/*
 * sshd_session.h - the LOGINOUT pre-drop setup for the wrapped OpenVMS OpenSSH
 * sshd session seam (rd vms-0cd, RUNG-3 step 3c, design §B). See sshd_session.c.
 *
 * OpenSSH's do_child() drops to the login user (do_setusercontext ->
 * permanently_set_uid) and then execve()s the login shell. OVMX interposes at
 * both points via linker --wrap (no OpenSSH source edit):
 *
 *   __wrap_permanently_set_uid -> ovmx_sshd_pre_drop_pw(pw)  [still root]
 *       establish the executive identity (fail-closed), banner, accounting;
 *   __real_permanently_set_uid                               [OpenSSH's own
 *       ordered, permanent, fail-closed setgid-before-setuid drop to the UIC];
 *   __wrap_execve -> rewrite the DCL exec into LOGINOUT->DCL argv (sshd_auth.c).
 *
 * The pre-drop half MUST run as root (setident needs SETPRV from the root
 * registration; accounting writes the protected store) -- exactly the ordering
 * tools/vms_login.c and vmssshd.c use. This header stays free of the OVMX SYSUAF
 * headers so the OpenSSH adapter can include it cleanly.
 */

#ifndef OVMX_VMSSSH_SSHD_SESSION_H
#define OVMX_VMSSSH_SSHD_SESSION_H

struct passwd;   /* <pwd.h> */

/*
 * Run the LOGINOUT pre-drop sequence for the login described by *pw, while the
 * process is still privileged (called from __wrap_permanently_set_uid before
 * the real drop):
 *
 *   1. re-read the binary SYSUAF for pw->pw_name (privileges + exact UIC);
 *   2. establish the authenticated identity in the executive
 *      (ovmx_ssh_establish_identity -> vms_kif_setident) -- FAIL CLOSED: on
 *      refusal (or /dev/vms absent) print %OVMX-F-NOIDENT and _exit(1), so NO
 *      session is ever handed an identity the executive denied (INV-6, the
 *      audit-§3.7 decision, mirrored from tools/vms_login.c);
 *   3. print the SYS$WELCOME banner + last-interactive-login line;
 *   4. record this login in the accounting store.
 *
 * If pw->pw_name is NOT a SYSUAF account (e.g. the privsep 'sshd' user whose
 * permanently_set_uid also routes through the wrap), this is a NO-OP and the
 * real drop proceeds unchanged.
 */
void ovmx_sshd_pre_drop_pw(const struct passwd *pw);

#endif /* OVMX_VMSSSH_SSHD_SESSION_H */
