/*
 * sysuaf.c - SYSUAF (System User Authorization File) shared library
 *
 * ATOMIC FLIP (vms-d92, epic vms-d0c). SYSUAF is now the genuine binary
 * $UAFDEF RMS Prolog-3 indexed file, and passwords are the real VMS Purdy
 * one-way hash (UAI$C_PURDY_S). The ASCII pipe-delimited + SHA-256 facade --
 * one text format with five hand-rolled parsers -- is retired. There is no
 * ASCII SYSUAF read/write and no SHA-256 on any path here.
 *
 * LAYERING SEAM. The binary engine (sysuaf_rms.c, rms_prolog3.c) and the
 * runtime read/enumerate/store entry points (sysuaf_live.c) live in LIBVMSRMS,
 * which links LIBVMS -- not the other way round. So this file reaches them the
 * same way rms_textfile.c reaches sys$open: through WEAK references. An image
 * that also links LIBVMSRMS (LOGINOUT, VMSSSHD, DCL, PROVISION, MAIL, the QEMU
 * facility tests) binds the real binary reader/writer; a bare LIBVMS unit test
 * that does not sees them NULL and fails honestly (Rule 9 / INV-6) -- never a
 * fabricated record, never a POSIX/ASCII fallback.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "str_util.h"
#include "sysuaf.h"
#include "vmsfs/filespec.h"
#include "vms/privs.h"
#include "purdy.h"
#include "ssdef.h"      /* $VMS_STATUS_SUCCESS */
#include "uaidef.h"
#include "prv_names.h"  /* VMS_PRIV_NAME_LIST X-macro */

/* ------------------------------------------------------------------ */
/* Weak references to the LIBVMSRMS binary SYSUAF entry points          */
/* (src/vmsrms/sysuaf_live.c). NULL when LIBVMSRMS is not in this image. */
/* ------------------------------------------------------------------ */
uint32_t ovmx_sysuaf_read_user(const char *username, sysuaf_rms_record_t *out);
uint32_t ovmx_sysuaf_read_uic(uint32_t uic, sysuaf_rms_record_t *out);
uint32_t ovmx_sysuaf_store_user(const sysuaf_rms_record_t *rec);
#pragma weak ovmx_sysuaf_read_user
#pragma weak ovmx_sysuaf_read_uic
#pragma weak ovmx_sysuaf_store_user

/* ------------------------------------------------------------------ */
/* Little-endian field accessors (substrate-agnostic, match p3_le*)     */
/* ------------------------------------------------------------------ */
static inline uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}
static inline void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

/* ------------------------------------------------------------------ */
/* FLAGS: names in the file, a longword in the API                     */
/* ------------------------------------------------------------------ */
static const struct { const char *name; uint32_t bit; } sysuaf_flag_names[] = {
    { "DISCTLY",      UAI$M_DISCTLY },
    { "DEFCLI",       UAI$M_DEFCLI },
    { "LOCKPWD",      UAI$M_LOCKPWD },
    { "DISMAIL",      UAI$M_DISMAIL },
    { "CAPTIVE",      UAI$M_CAPTIVE },
    { "DISREPORT",    UAI$M_DISREPORT },
    { "DISRECONNECT", UAI$M_DISRECONNECT },
    { "AUTOLOGIN",    UAI$M_AUTOLOGIN },
    { "DISLOCAL",     UAI$M_DISLOCAL },
    { "DISDIALUP",    UAI$M_DISDIALUP },
    { "DISNETWORK",   UAI$M_DISNETWORK },
    { "DISACNT",      UAI$M_DISACNT },
    { "DISBATCH",     UAI$M_DISBATCH },
    { "DISUSER",      UAI$M_DISUSER },
    { "DISWELCOME",   UAI$M_DISWELCOME },
    { "EXTAUTH",      UAI$M_EXTAUTH },
    { "PWDMIX",       UAI$M_PWDMIX },
    { "GENERATE_PWD", UAI$M_GENERATE_PWD },
};

uint32_t sysuaf_flags_to_mask(const char *flags)
{
    if (!flags || flags[0] == '\0')
        return 0;

    char buf[64];
    strncpy(buf, flags, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    uint32_t mask = 0;
    char *saveptr = NULL;
    for (char *tok = strtok_r(buf, ",", &saveptr); tok;
         tok = strtok_r(NULL, ",", &saveptr)) {
        while (*tok == ' ') tok++;
        for (size_t i = 0; i < sizeof(sysuaf_flag_names) /
                               sizeof(sysuaf_flag_names[0]); i++) {
            if (strcasecmp(tok, sysuaf_flag_names[i].name) == 0) {
                mask |= sysuaf_flag_names[i].bit;
                break;
            }
        }
    }
    return mask;
}

void sysuaf_mask_to_flags(uint32_t mask, char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return;
    out[0] = '\0';

    size_t used = 0;
    for (size_t i = 0; i < sizeof(sysuaf_flag_names) /
                           sizeof(sysuaf_flag_names[0]); i++) {
        if (!(mask & sysuaf_flag_names[i].bit))
            continue;
        size_t nlen = strlen(sysuaf_flag_names[i].name);
        size_t need = nlen + (used ? 1 : 0);
        if (used + need + 1 > outsz)
            break;
        if (used)
            out[used++] = ',';
        memcpy(out + used, sysuaf_flag_names[i].name, nlen);
        used += nlen;
        out[used] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* Privileges: names in the file, a quadword mask in the record         */
/* ------------------------------------------------------------------ */
static const struct { const char *name; uint64_t bit; } sysuaf_priv_names[] = {
#define PRIV_ROW(nm, mask, desc) { #nm, (uint64_t)(mask) },
    VMS_PRIV_NAME_LIST(PRIV_ROW)
#undef PRIV_ROW
};

uint64_t sysuaf_parse_privileges(const char *priv_string)
{
    return parse_privilege_string(priv_string);
}

void sysuaf_format_privileges(uint64_t mask, char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return;
    out[0] = '\0';

    size_t used = 0;
    for (size_t i = 0; i < sizeof(sysuaf_priv_names) /
                           sizeof(sysuaf_priv_names[0]); i++) {
        if (!(mask & sysuaf_priv_names[i].bit))
            continue;
        size_t nlen = strlen(sysuaf_priv_names[i].name);
        size_t need = nlen + (used ? 1 : 0);
        if (used + need + 1 > outsz)
            break;
        if (used)
            out[used++] = ',';
        memcpy(out + used, sysuaf_priv_names[i].name, nlen);
        used += nlen;
        out[used] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* Binary <-> view mapping                                             */
/* ------------------------------------------------------------------ */

/* Trim the 32-byte blank-padded UAF$T_USERNAME into a C string. */
static void raw_username(const sysuaf_rms_record_t *raw, char *out, size_t outsz)
{
    size_t n = SYSUAF_USERNAME_LEN;
    while (n > 0 && raw->uaf$t_username[n - 1] == ' ')
        n--;
    if (n >= outsz)
        n = outsz - 1;
    memcpy(out, raw->uaf$t_username, n);
    out[n] = '\0';
}

void sysuaf_record_set_username(sysuaf_record_t *rec, const char *username)
{
    size_t n = username ? strlen(username) : 0;
    for (size_t i = 0; i < SYSUAF_USERNAME_LEN; i++) {
        rec->raw.uaf$t_username[i] =
            (i < n) ? (char)toupper((unsigned char)username[i]) : ' ';
    }
    raw_username(&rec->raw, rec->username, sizeof(rec->username));
}

void sysuaf_raw_to_view(const sysuaf_rms_record_t *raw, sysuaf_record_t *rec)
{
    if (!raw || !rec)
        return;
    memset(rec, 0, sizeof(*rec));
    rec->raw = *raw;

    raw_username(raw, rec->username, sizeof(rec->username));

    uint32_t uic = le32(raw->uaf$l_uic);
    rec->uic_group  = (uic >> 16) & 0xffffu;
    rec->uic_member = uic & 0xffffu;

    /* [OVMX] string fields are NUL-terminated within their fixed width. */
    snprintf(rec->default_dir, sizeof(rec->default_dir), "%.*s",
             (int)sizeof(raw->uaf$t_defdir), raw->uaf$t_defdir);
    snprintf(rec->lgicmd, sizeof(rec->lgicmd), "%.*s",
             (int)sizeof(raw->uaf$t_lgicmd), raw->uaf$t_lgicmd);

    sysuaf_mask_to_flags(le32(raw->uaf$l_flags), rec->flags, sizeof(rec->flags));
    sysuaf_format_privileges(le64(raw->uaf$q_priv),
                             rec->privileges, sizeof(rec->privileges));
}

void sysuaf_view_to_raw(sysuaf_record_t *rec)
{
    if (!rec)
        return;
    sysuaf_rms_record_t *raw = &rec->raw;

    raw->uaf$b_rtype   = 1;
    raw->uaf$b_version = 1;

    sysuaf_record_set_username(rec, rec->username);

    put_le32(raw->uaf$l_uic,
             ((rec->uic_group & 0xffffu) << 16) | (rec->uic_member & 0xffffu));

    memset(raw->uaf$t_defdir, 0, sizeof(raw->uaf$t_defdir));
    strncpy(raw->uaf$t_defdir, rec->default_dir, sizeof(raw->uaf$t_defdir) - 1);
    memset(raw->uaf$t_lgicmd, 0, sizeof(raw->uaf$t_lgicmd));
    strncpy(raw->uaf$t_lgicmd, rec->lgicmd, sizeof(raw->uaf$t_lgicmd) - 1);
    if (raw->uaf$t_defcli[0] == '\0')
        strncpy(raw->uaf$t_defcli, "DCL", sizeof(raw->uaf$t_defcli) - 1);

    put_le32(raw->uaf$l_flags, sysuaf_flags_to_mask(rec->flags));
    uint64_t pmask = parse_privilege_string(rec->privileges);
    put_le64(raw->uaf$q_priv, pmask);
    put_le64(raw->uaf$q_def_priv, pmask);
    /* password area (uaf$q_pwd/salt/encrypt) is PRESERVED here -- see
     * sysuaf_set_password. */
}

/* ------------------------------------------------------------------ */
/* Password: Purdy set + verify                                        */
/* ------------------------------------------------------------------ */
static uint16_t fresh_salt(void)
{
    /* The salt is stored with the record and reused for verification, so any
     * value is functionally correct; VMS randomizes it per account. A private
     * LCG mixed with time()+getpid() (both already DECC$SHR universals -- NO
     * rand()/srand(), which the native-link C-RTL vector does not export)
     * varies the salt per call without a new cross-shareable dependency. */
    static uint32_t ctr = 0;
    uint32_t t = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16);
    ctr = ctr * 1103515245u + 12345u;        /* Numerical-Recipes LCG step */
    uint32_t x = t ^ ctr ^ (ctr >> 13) ^ (t << 7);
    return (uint16_t)(x & 0xffffu);
}

int sysuaf_set_password_salt(sysuaf_record_t *rec, const char *password,
                             uint16_t salt)
{
    if (!rec || !password)
        return -1;
    char user[SYSUAF_USERNAME_LEN + 1];
    raw_username(&rec->raw, user, sizeof(user));

    size_t pwlen = strlen(password);
    uint64_t quad = purdy_s_hash(password, pwlen, user, salt);

    put_le64(rec->raw.uaf$q_pwd, quad);
    put_le16(rec->raw.uaf$w_salt, salt);
    rec->raw.uaf$b_encrypt    = UAI$C_PURDY_S;
    rec->raw.uaf$b_pwd_length = (uint8_t)(pwlen > 255 ? 255 : pwlen);
    return 0;
}

int sysuaf_set_password(sysuaf_record_t *rec, const char *password)
{
    /* Interactive/runtime path: a fresh random salt per account, as VMS does. */
    return sysuaf_set_password_salt(rec, password, fresh_salt());
}

/*
 * Verify `password` against the stored Purdy quadword (constant-time compare).
 * A record whose UAF$B_ENCRYPT is not UAI$C_PURDY_S authenticates NOTHING --
 * the binary equivalent of the retired "empty hash => cannot authenticate"
 * rule (vms-08f). OpenVMS has no state where an absent password is a green
 * light (the one passwordless state, UAI$M_AUTOLOGIN, is an explicit per-
 * account flag OVMX does not implement), so this refuses every password.
 */
int sysuaf_authenticate(const sysuaf_record_t *rec, const char *password)
{
    if (!rec || !password)
        return 0;
    if (rec->raw.uaf$b_encrypt != UAI$C_PURDY_S)
        return 0;

    char user[SYSUAF_USERNAME_LEN + 1];
    raw_username(&rec->raw, user, sizeof(user));

    uint16_t salt = le16(rec->raw.uaf$w_salt);
    uint64_t attempt = purdy_s_hash(password, strlen(password), user, salt);
    uint64_t stored  = le64(rec->raw.uaf$q_pwd);

    uint64_t diff = attempt ^ stored;
    return (int)((diff | (0ull - diff)) >> 63) ^ 1;   /* 1 iff diff == 0 */
}

/* ------------------------------------------------------------------ */
/* Lookup (binary, via the weak LIBVMSRMS entry points)                */
/* ------------------------------------------------------------------ */

int sysuaf_lookup(const char *username, sysuaf_record_t *rec)
{
    if (!username || !rec)
        return -1;
    if (!ovmx_sysuaf_read_user)          /* no LIBVMSRMS in this image */
        return -1;

    sysuaf_rms_record_t raw;
    uint32_t st = ovmx_sysuaf_read_user(username, &raw);
    if (!$VMS_STATUS_SUCCESS(st))
        return -1;
    sysuaf_raw_to_view(&raw, rec);
    return 0;
}

int sysuaf_lookup_by_uic(uint32_t uic, sysuaf_record_t *rec)
{
    if (!rec)
        return -1;
    if (!ovmx_sysuaf_read_uic)
        return -1;

    sysuaf_rms_record_t raw;
    uint32_t st = ovmx_sysuaf_read_uic(uic, &raw);
    if (!$VMS_STATUS_SUCCESS(st))
        return -1;
    sysuaf_raw_to_view(&raw, rec);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Login-flag enforcement (vms-c8fa)                                   */
/* ------------------------------------------------------------------ */
int sysuaf_interactive_login_permitted(const sysuaf_record_t *rec)
{
    if (!rec)
        return 0;   /* fail closed */
    uint32_t mask = sysuaf_flags_to_mask(rec->flags);
    if (mask & (UAI$M_DISUSER | UAI$M_DISACNT))
        return 0;
    return 1;
}

int sysuaf_account_captive(const sysuaf_record_t *rec)
{
    if (!rec)
        return 0;
    return (sysuaf_flags_to_mask(rec->flags) & UAI$M_CAPTIVE) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* sysuaf_write_record (binary, via the weak store entry point)        */
/* ------------------------------------------------------------------ */
int sysuaf_write_record(const sysuaf_record_t *rec)
{
    if (!rec || rec->username[0] == '\0')
        return -1;
    if (!ovmx_sysuaf_store_user)
        return -1;
    uint32_t st = ovmx_sysuaf_store_user(&rec->raw);
    return $VMS_STATUS_SUCCESS(st) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* sysuaf_login_command_file (vms-e48)                                 */
/* ------------------------------------------------------------------ */
void sysuaf_login_command_file(const sysuaf_record_t *rec,
                               char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return;
    const char *spec = (rec && rec->lgicmd[0]) ? rec->lgicmd
                                               : SYSUAF_DEFAULT_LGICMD;
    snprintf(out, outsz, "%s", spec);
}
