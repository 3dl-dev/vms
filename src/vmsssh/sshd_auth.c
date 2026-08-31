/*
 * sshd_auth.c - SYSUAF password auth + SYSUAF->passwd identity for the wrapped
 * OpenVMS OpenSSH sshd (rd vms-0cd, RUNG-3 step 3c). See sshd_auth.h.
 *
 * Nothing here is new auth/session POLICY: sysuaf_lookup / sysuaf_authenticate
 * (Purdy) / sysuaf_interactive_login_permitted / sysuaf_login_command_file are
 * the SAME landed, tested primitives the console login (tools/vms_login.c) and
 * the old libssh vmssshd.c run. This file only sequences them behind the two
 * OpenSSH porting seams so the OpenSSH source stays unmodified.
 */

#include <string.h>
#include <pwd.h>

#include "sshd_auth.h"
#include "sysuaf.h"
#include "str_util.h"
#include "ovmx_layout.h"

/* The DCL image, as a Linux path OpenSSH's execve() can run (mirrors
 * vmssshd.c's DCL_SHELL_PATH). */
#define OVMX_SSHD_DCL_PATH   VMS_SYSTEM_DIR "/DCL.EXE"

/* ---- the login decision (unit-tested, no SYSUAF file needed) ------------- */

int ovmx_sshd_check_login(const struct sysuaf_record *rec_opaque,
                          const char *password)
{
    const sysuaf_record_t *rec = (const sysuaf_record_t *)rec_opaque;

    if (rec == NULL || password == NULL)
        return 0;

    /* Purdy verify against the binary record's stored quadword. A record with
     * no UAI$C_PURDY_S credential authenticates NOTHING (sysuaf_authenticate). */
    if (!sysuaf_authenticate(rec, password))
        return 0;

    /* Login-flag enforcement, SEPARATE from the password check: a DISUSER /
     * DISACNT account is refused even with the correct password (fail closed on
     * a NULL record too). Mirrors tools/vms_login.c. */
    if (!sysuaf_interactive_login_permitted(rec))
        return 0;

    return 1;
}

/* ---- the sys_auth_passwd body: read binary SYSUAF, then decide ----------- */

int ovmx_sshd_sysuaf_auth(const char *user, const char *password)
{
    char uname[64];
    sysuaf_record_t rec;

    if (user == NULL || password == NULL)
        return 0;

    /* VMS usernames are uppercase; the client sends whatever the user typed. */
    strncpy(uname, user, sizeof(uname) - 1);
    uname[sizeof(uname) - 1] = '\0';
    str_upcase(uname);

    /* Read the BINARY SYSUAF ($UAFDEF via the RMS engine over the ACP). -1 =
     * unknown user OR engine/ACP unreadable -> REJECT (fail closed, INV-6). */
    memset(&rec, 0, sizeof(rec));
    if (sysuaf_lookup(uname, &rec) != 0)
        return 0;

    return ovmx_sshd_check_login((const struct sysuaf_record *)&rec, password);
}

/* ---- SYSUAF -> struct passwd (the --wrap=getpwnam body) ------------------ */

int ovmx_sshd_fill_passwd(const char *user, struct passwd *pw,
                          struct ovmx_sshd_pwbufs *b)
{
    char uname[64];
    sysuaf_record_t rec;

    if (user == NULL || pw == NULL || b == NULL)
        return -1;

    strncpy(uname, user, sizeof(uname) - 1);
    uname[sizeof(uname) - 1] = '\0';
    str_upcase(uname);

    memset(&rec, 0, sizeof(rec));
    if (sysuaf_lookup(uname, &rec) != 0)
        return -1;   /* not a SYSUAF account -> adapter falls through to real  */

    memset(b, 0, sizeof(*b));
    strncpy(b->name, rec.username, sizeof(b->name) - 1);
    strncpy(b->passwd, "x", sizeof(b->passwd) - 1);   /* unused: CUSTOM auth   */
    strncpy(b->gecos, rec.username, sizeof(b->gecos) - 1);
    strncpy(b->dir, "/", sizeof(b->dir) - 1);
    strncpy(b->shell, OVMX_SSHD_DCL_PATH, sizeof(b->shell) - 1);

    memset(pw, 0, sizeof(*pw));
    pw->pw_name   = b->name;
    pw->pw_passwd = b->passwd;
    pw->pw_gecos  = b->gecos;
    pw->pw_dir    = b->dir;
    pw->pw_shell  = b->shell;
    /* UIC [group,member] -> (gid=group, uid=member): the exact mapping
     * cred_drop.c derives and the executive/RMS enforce (see cred_drop.h). */
    pw->pw_uid    = (uid_t)rec.uic_member;
    pw->pw_gid    = (gid_t)rec.uic_group;
    return 0;
}

const char *ovmx_sshd_dcl_image_path(void)
{
    return OVMX_SSHD_DCL_PATH;
}

int ovmx_sshd_is_dcl_path(const char *path)
{
    const char *base;
    if (path == NULL)
        return 0;
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strcmp(base, "DCL.EXE") == 0;
}

const char *ovmx_sshd_dcl_login_argv(const char *user,
                                     char *lgicmd, size_t lgsz,
                                     const char **argv, size_t argvmax)
{
    char uname[64];
    sysuaf_record_t rec;
    size_t i = 0;

    if (user == NULL || lgicmd == NULL || argv == NULL || argvmax < 6)
        return NULL;

    strncpy(uname, user, sizeof(uname) - 1);
    uname[sizeof(uname) - 1] = '\0';
    str_upcase(uname);

    memset(&rec, 0, sizeof(rec));
    if (sysuaf_lookup(uname, &rec) != 0)
        return NULL;   /* fail honest: no DCL session without the real record  */

    /* Hand over the account's LGICMD (or SYS$LOGIN:LOGIN.COM default), exactly
     * as LOGINOUT does (tools/vms_login.c / vmssshd.c). */
    sysuaf_login_command_file(&rec, lgicmd, lgsz);

    argv[i++] = "vmsdcl";
    argv[i++] = "--login";
    if (sysuaf_account_captive(&rec))
        argv[i++] = "--captive";
    argv[i++] = "--lgicmd";
    argv[i++] = lgicmd;
    argv[i]   = NULL;   /* argvmax >= 6 guarantees room */
    return OVMX_SSHD_DCL_PATH;
}
