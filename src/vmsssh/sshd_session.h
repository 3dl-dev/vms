/*
 * sshd_session.h - the SSH session handoff for the wrapped OpenVMS OpenSSH sshd
 * seam (rd vms-843a / vms-29b / vms-65b, design docs/design-ssh-loginout-
 * handoff.md). See sshd_session.c.
 *
 * OpenSSH's do_child() drops to the login user (do_setusercontext ->
 * permanently_set_uid) and then execve()s the login shell. OVMX interposes at
 * permanently_set_uid via linker --wrap (no OpenSSH source edit) and, for a
 * SYSUAF login, HANDS THE SESSION OFF to $CREPRC(LOGINOUT) instead of letting
 * OpenSSH drop-and-exec a shell:
 *
 *   __wrap_permanently_set_uid -> ovmx_sshd_pre_drop_pw(pw)   [still root]
 *       SYSUAF login  -> mint a vterm, vouch the pre-authenticated user onto it
 *                        (VMS_IOCTL_TERM_SETLOGIN, vms-65b), $CREPRC(LOGINOUT,
 *                        vterm, PRC$M_INTER|PRC$M_LOGINOUT), pump, _exit.
 *                        NEVER returns -- OpenSSH's own drop + execve do not run.
 *       privsep 'sshd' -> no-op; return so __real_permanently_set_uid drops.
 *
 * This runs while still root ON PURPOSE: the created LOGINOUT establishes SYSTEM
 * to read the World-denied SYS$SYSTEM:SYSUAF.DAT, which needs the creator to
 * still hold CAP_SYS_ADMIN -- true only before OpenSSH's drop, and the same
 * reason VMS_IOCTL_TERM_SETLOGIN (which vouches the pre-auth) is gated on it.
 *
 * This unifies console login (JOB_CONTROL -> LOGINOUT), DECnet SET HOST and SSH
 * on the ONE $CREPRC(LOGINOUT) primitive, retiring the SSH C-reimpl session
 * shim's raw-execve DCL activation (vms-16b). This header stays free of the OVMX
 * SYSUAF headers so the OpenSSH adapter can include it cleanly.
 */

#ifndef OVMX_VMSSSH_SSHD_SESSION_H
#define OVMX_VMSSSH_SSHD_SESSION_H

#include <stddef.h>

struct passwd;   /* <pwd.h> */

/*
 * Hand off the login described by *pw to a $CREPRC(LOGINOUT) session, while the
 * process is still privileged (called from __wrap_permanently_set_uid before
 * the real drop). For a SYSUAF account this NEVER returns: on success it becomes
 * the byte relay between the SSH channel and the LOGINOUT/DCL session and
 * _exit()s when the session ends; on any failure to create the session it
 * _exit(1)s fail-closed (INV-6 -- no session is ever admitted without one).
 *
 * If pw->pw_name is NOT a SYSUAF account (the privsep 'sshd' user, whose
 * permanently_set_uid also routes through the wrap), this is a NO-OP and the
 * real drop proceeds unchanged.
 */
void ovmx_sshd_pre_drop_pw(const struct passwd *pw);

#endif /* OVMX_VMSSSH_SSHD_SESSION_H */
