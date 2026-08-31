/*
 * sshd_auth.h - SYSUAF password authentication + SYSUAF->passwd identity for
 * the WRAPPED OpenVMS OpenSSH sshd (rd vms-0cd, RUNG-3 step 3c).
 *
 * These are the OpenSSH-INDEPENDENT halves of the sshd auth seam, factored out
 * of the OpenSSH shim (third-party/openssh/ovmx/ovmx_sshd_auth.c) exactly like
 * ssh_ident.c / cred_drop.c are factored out of vmssshd.c: so the
 * security-critical decision (Purdy verify + login-flag enforcement, fail
 * closed) is unit-testable (tests/vmsssh/test_sshd_auth.c) WITHOUT the OpenSSH
 * tree, and the OpenSSH shim stays a thin adapter over the CUSTOM_SYS_AUTH_PASSWD
 * hook + a --wrap=getpwnam interposer.
 *
 * INV-6 / Rule 9: every entry point here fails CLOSED. An unknown user, an
 * unreadable SYSUAF (RMS engine absent / ACP refusing), or a DISUSER/DISACNT
 * account all REJECT -- never a fabricated accept, never a /etc/passwd fallback
 * standing in for the binary SYSUAF.
 *
 * This header is deliberately free of the OVMX SYSUAF headers (sysuaf.h &c.) so
 * the OpenSSH adapter can include it without dragging VMS macros into OpenSSH's
 * namespace; the .c includes them internally.
 */

#ifndef OVMX_VMSSSH_SSHD_AUTH_H
#define OVMX_VMSSSH_SSHD_AUTH_H

#include <sys/types.h>   /* uid_t, gid_t */
#include <stddef.h>

struct passwd;           /* <pwd.h> -- forward-declared; filled by the adapter */

/* Forward decl of the SYSUAF view record so the policy entry can take one
 * without this header pulling in sysuaf.h. The .c casts through the real type. */
struct sysuaf_record;

/*
 * THE LOGIN DECISION (unit-tested). Given an already-read binary SYSUAF view
 * record, return 1 to ACCEPT the interactive login, 0 to REJECT.
 *
 *   accept  <=>  the password Purdy-verifies (sysuaf_authenticate == 1)
 *                AND the account permits interactive login
 *                (sysuaf_interactive_login_permitted == 1: no DISUSER/DISACNT).
 *
 * A NULL record or NULL password rejects. This is the exact trio the console
 * login (tools/vms_login.c) enforces, minus the prompting.
 */
int ovmx_sshd_check_login(const struct sysuaf_record *rec, const char *password);

/*
 * THE PASSWORD AUTH HOOK BODY (drives sys_auth_passwd). Upcase `user` (VMS
 * usernames are uppercase), read the binary SYSUAF record via the RMS engine
 * (sysuaf_lookup), and apply ovmx_sshd_check_login. Returns 1 accept / 0 reject.
 *
 * FAIL CLOSED: if the user is unknown, or the SYSUAF cannot be read (the binary
 * engine is not linked / no /dev/vms ACP -- sysuaf_lookup returns -1), this
 * REJECTS. There is no path here that accepts without a genuine binary-SYSUAF
 * Purdy match (INV-6).
 */
int ovmx_sshd_sysuaf_auth(const char *user, const char *password);

/*
 * Caller-owned string storage backing an ovmx_sshd_fill_passwd() struct passwd
 * (the OpenSSH shim keeps one static instance per session process).
 */
struct ovmx_sshd_pwbufs {
    char name[64];
    char passwd[8];
    char gecos[64];
    char dir[256];
    char shell[256];
};

/*
 * SYSUAF -> struct passwd (drives the --wrap=getpwnam interposer, design §A).
 * Fill *pw (its char* fields pointing into *b) from the binary SYSUAF record
 * for `user`:
 *   pw_name  = upcased SYSUAF username
 *   pw_uid   = UIC member,  pw_gid = UIC group   (the cred_drop.c UIC mapping;
 *              OpenSSH's own permanently_set_uid then performs the ordered,
 *              permanent, fail-closed drop to these -- so no separate cred drop
 *              is needed in the sshd link)
 *   pw_dir   = "/"  (a VMS default-directory is not a Linux home; OpenSSH only
 *              chdir()s here best-effort)
 *   pw_shell = the DCL image path, so OpenSSH's do_child execve() targets DCL,
 *              which the session shim rewrites into LOGINOUT->DCL argv.
 *
 * Returns 0 if `user` is a SYSUAF account (found), -1 otherwise -- the adapter
 * MUST then fall through to the real getpwnam (so the privsep 'sshd'/'root'
 * accounts still resolve from /etc/passwd). Fail-honest: -1 on engine absent.
 */
int ovmx_sshd_fill_passwd(const char *user, struct passwd *pw,
                          struct ovmx_sshd_pwbufs *b);

/* The DCL image path used for pw_shell / the session exec (Linux path). */
const char *ovmx_sshd_dcl_image_path(void);

/* 1 if `path` names the DCL image (basename DCL.EXE) -- the session-shim test
 * for "OpenSSH is about to exec the login shell, which is DCL". */
int ovmx_sshd_is_dcl_path(const char *path);

/*
 * Build the LOGINOUT->DCL argv for `user` (design §B step 5), mirroring
 * tools/vms_login.c: resolve the account's LGICMD (or the SYS$LOGIN:LOGIN.COM
 * default) and whether it is CAPTIVE, and emit
 *   { "vmsdcl", "--login", "--lgicmd", <lgicmd>, NULL }   (or "--captive").
 * `lgicmd` is caller storage the argv points into; argv is caller storage of at
 * least 8 slots. Returns the DCL image path to execve, or NULL on failure
 * (unknown user / engine absent) so the caller fails honestly.
 */
const char *ovmx_sshd_dcl_login_argv(const char *user,
                                     char *lgicmd, size_t lgsz,
                                     const char **argv, size_t argvmax);

#endif /* OVMX_VMSSSH_SSHD_AUTH_H */
