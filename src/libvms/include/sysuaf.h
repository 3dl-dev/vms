/*
 * sysuaf.h - SYSUAF (System User Authorization File) library
 *
 * Shared interface for looking up users and authenticating passwords
 * against SYS$SYSTEM:SYSUAF.DAT. Used by vms_login, vms_ssh_auth,
 * and vmssshd.
 *
 * The library is usable without any VMS runtime being initialized —
 * it is pure file parsing and SHA256 hashing.
 */

#ifndef SYSUAF_H
#define SYSUAF_H

#include <stdint.h>
#include <stddef.h>
#include "ovmx_layout.h"
#include "rms/xab.h"

#define SYSUAF_PATH VMS_SYSUAF_PATH

/* -------------------------------------------------------------------------
 * In-memory (parsed) SYSUAF record.
 *
 * Legacy representation produced by sysuaf_lookup() from the current
 * pipe-delimited text SYSUAF.DAT. Fields are text/string-oriented. This is
 * the interface consumed today by vms_login, vms_ssh_auth, and vmssshd.
 * ------------------------------------------------------------------------- */
typedef struct {
    char     username[64];
    char     password_hash[128];
    uint32_t uic_group;
    uint32_t uic_member;
    char     default_dir[256];
    char     flags[64];
    char     privileges[256];
} sysuaf_record_t;

/* =========================================================================
 * Binary on-disk SYSUAF record (RMS indexed file) — vms-846 / vms-846.1
 *
 * Real OpenVMS stores users in SYS$SYSTEM:SYSUAF.DAT as an RMS indexed
 * (ISAM) file of fixed-length binary records keyed by username, NOT the
 * flat pipe-delimited text file OVMX currently ships. sysuaf_rms_record_t
 * is the fixed-length record written to that RMS FAB$C_IDX file.
 *
 * CLEAN-ROOM / PURITY (CLAUDE.md invariant #8): field *names* follow the
 * public UAF$ symbol convention (UAF$T_USERNAME, UAF$L_UIC, UAF$Q_PRIV, …)
 * and the *shape* mirrors real VMS (indexed RMS file, fixed binary records,
 * primary key = 32-byte space-padded username). The exact byte offsets and
 * the compact field set below are an **OVMX design choice** — this is NOT a
 * byte-authentic reproduction of VSI's SYSUAF record ($UAFDEF), which is far
 * larger. Do not present these offsets as VMS-authentic.
 *
 * Encodings that DO match documented VMS semantics:
 *   - uaf$l_uic  : longword UIC, group in high 16 bits, member in low 16
 *                  (uic = (group << 16) | member).
 *   - uaf$q_priv : 64-bit privilege mask (PRV$ bit positions).
 *
 * Layout is hand-aligned so the struct has NO implicit padding, giving a
 * deterministic 368-byte on-disk record across compilers. The _Static_assert
 * block below locks the layout; adding/reordering fields is a design change
 * (run the vms-846 cascade) and must keep the key segment at offset 0.
 * ========================================================================= */

#define SYSUAF_USERNAME_LEN  32     /* space-padded, primary key */
#define SYSUAF_PWHASH_LEN    64     /* SHA-256 = 32 bytes used, remainder reserved */
#define SYSUAF_DEFDIR_LEN   256     /* default directory filespec */

typedef struct {
    char     uaf$t_username[SYSUAF_USERNAME_LEN]; /* @0   primary key, space-padded */
    uint8_t  uaf$b_pwd[SYSUAF_PWHASH_LEN];        /* @32  raw hash bytes (SHA-256) */
    uint32_t uaf$l_uic;                           /* @96  (group << 16) | member */
    uint32_t uaf$l_flags;                         /* @100 UAF$M_* flag bitmask */
    uint64_t uaf$q_priv;                          /* @104 64-bit privilege mask */
    char     uaf$t_defdir[SYSUAF_DEFDIR_LEN];     /* @112 default directory */
} sysuaf_rms_record_t;                            /* total: 368 bytes */

/* Compile-time layout lock — deterministic on-disk record (no padding). */
_Static_assert(sizeof(sysuaf_rms_record_t) == 368,
               "sysuaf_rms_record_t must be exactly 368 bytes on disk");
_Static_assert(offsetof(sysuaf_rms_record_t, uaf$t_username) == 0,
               "username (primary key) must be at record offset 0");
_Static_assert(offsetof(sysuaf_rms_record_t, uaf$l_uic) == 96,
               "uaf$l_uic layout drift");
_Static_assert(offsetof(sysuaf_rms_record_t, uaf$q_priv) == 104,
               "uaf$q_priv layout drift");

/* Primary key geometry (segment 0). */
#define SYSUAF_KEY_USERNAME_POS  0
#define SYSUAF_KEY_USERNAME_SIZ  SYSUAF_USERNAME_LEN

/*
 * Build the RMS XABKEY describing the SYSUAF primary key (key of reference 0):
 * a single string segment over the 32-byte username at record offset 0.
 * Usernames are unique, so no duplicates and no change-in-place are allowed.
 * Chain the returned XAB off the FAB (fab$l_xab) when creating/opening
 * SYSUAF.DAT as an indexed file.
 */
static inline struct XABKEY sysuaf_rms_primary_key(void)
{
    struct XABKEY k = cc$rms_xabkey;    /* cod=KEY, bln, dtp=STG, nseg=1 */
    k.xab$b_ref  = 0;                   /* primary key */
    k.xab$w_flg  = 0;                   /* unique: no DUP, no CHG */
    k.xab$w_pos0 = SYSUAF_KEY_USERNAME_POS;
    k.xab$b_siz0 = SYSUAF_KEY_USERNAME_SIZ;
    k.xab$w_tks  = SYSUAF_KEY_USERNAME_SIZ;
    k.xab$l_knm  = "USERNAME";
    return k;
}

/* Look up a user in sysuaf.dat. Returns 0 on success, -1 if not found. */
int sysuaf_lookup(const char *username, sysuaf_record_t *rec);

/*
 * Look up a user by UIC. Returns 0 on success, -1 if no account in
 * sysuaf.dat carries that [group,member], or the file cannot be read.
 *
 * WHY THIS EXISTS (vms-cb5 / vms-f39). F$IDENTIFIER's NUMBER_TO_NAME
 * direction needs the UIC -> name mapping. It used to get it from
 * getpwuid(), i.e. from the HOST's /etc/passwd, so DCL answered
 * F$IDENTIFIER(1000,"NUMBER_TO_NAME") with the Linux login name of
 * whoever built the image, upcased. On OpenVMS the mapping comes from
 * the rights database, which carries a UIC identifier per SYSUAF
 * account; OVMX's authorization data is this file, and it is the same
 * file LOGINOUT authenticates against, so it is the only source of a
 * VMS user name OVMX has that a process cannot write.
 *
 * If two rows share a UIC the FIRST in file order wins. That is a
 * property of this function, not a claim about SYSUAF.DAT: nothing
 * enforces UIC uniqueness in the flat file OVMX ships today.
 */
int sysuaf_lookup_by_uic(uint32_t uic_group, uint32_t uic_member,
                         sysuaf_record_t *rec);

/* Authenticate: returns 1 if password matches, 0 if not.
   An empty/unset hash NEVER authenticates -- returns 0 for every password,
   including the empty string (vms-08f; see the Rule 10 disposition in
   sysuaf.c). OpenVMS has no state where an absent password field is a
   green light; the one documented passwordless state (UAI$M_AUTOLOGIN)
   is an explicit per-account flag OVMX does not implement, so an unset
   hash must mean "cannot authenticate", not "no password required". */
int sysuaf_authenticate(const sysuaf_record_t *rec, const char *password);

/* Parse VMS privilege string (e.g. "TMPMBX,NETMBX,OPER") into bitmask */
uint64_t sysuaf_parse_privileges(const char *priv_string);

#endif /* SYSUAF_H */
