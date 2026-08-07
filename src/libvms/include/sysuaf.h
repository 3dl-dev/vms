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
#include <stdio.h>
#include "ovmx_layout.h"
#include "rms/xab.h"

#define SYSUAF_PATH VMS_SYSUAF_PATH

/* =========================================================================
 * THE SYSUAF.DAT TEXT FORMAT — ONE DEFINITION, ONE READER, ONE WRITER
 * (vms-9b7)
 *
 * WHAT THIS REPLACES, so it is never put back: five independent hand-rolled
 * parsers of one file format, with THREE different line-buffer sizes and TWO
 * different writer format strings.
 *
 *   src/ovmx_init/ovmx_init.c  sysuaf_split()   char line[512]   DELETED
 *   src/ovmx_init/ovmx_init.c  sysuaf_field()   char line[512]   DELETED
 *   src/libvms/rtl/sysuaf.c    sysuaf_scan()    char line[1024]  -> this
 *   src/libvms/syssvc/sys_uai.c parse_uaf_line  512              -> this
 *   tools/vms_authorize.c      load_sysuaf()    1024             -> this
 *
 * MEASURED, on a real QEMU boot of the unfixed tree (three boots, each with
 * a control): a SYSTEM row long enough that its SIXTH '|' falls past byte
 * 511 is read by the 512-byte readers as a record with only five fields.
 * PID 1's establish_system_identity() then reported
 *
 *     %OVMX-F-EXECINIT, no SYSTEM record in SYS$SYSTEM:SYSUAF.DAT
 *
 * and powered the machine off, while the 1024-byte readers accepted the same
 * row without complaint. That is a writer and a reader disagreeing about one
 * file, and the disagreement is fatal by design (Rule 10) -- so the format
 * gets exactly one definition and every accessor is derived from it.
 *
 * SYSUAF_LINE_MAX is the ONE limit. It bounds the writer (an over-length
 * record is REFUSED, loudly, by sysuaf_format_record() -- never silently
 * clipped) and it bounds the reader (an over-length line is REPORTED by
 * sysuaf_read_line() -- never silently truncated into a short record).
 * ========================================================================= */

/* The record separator. Chosen over ':' because VMS device names contain
   colons and DEFAULT_DIR is a filespec. */
#define SYSUAF_SEP_CHAR      '|'
#define SYSUAF_SEP_STR       "|"

/* Field order and count. Rows carrying fewer than SYSUAF_MIN_FIELDS are
   malformed; FLAGS and PRIVILEGES may be absent on a legacy row. */
#define SYSUAF_FIELD_COUNT   7
#define SYSUAF_MIN_FIELDS    5

enum {
    SYSUAF_F_USERNAME   = 0,
    SYSUAF_F_PWHASH     = 1,
    SYSUAF_F_UIC_GROUP  = 2,
    SYSUAF_F_UIC_MEMBER = 3,
    SYSUAF_F_DEFDIR     = 4,
    SYSUAF_F_FLAGS      = 5,
    SYSUAF_F_PRIVILEGES = 6
};

/*
 * THE UIC FIELDS ARE OCTAL (vms-e60). Derivation -- from the oracle, not
 * chosen -- is in the block comment on sysuaf_parse_line() in
 * src/libvms/rtl/sysuaf.c. Every read, write, display and /UIC=[g,m] parse
 * in the tree goes through this constant so they cannot drift apart again.
 */
#define SYSUAF_UIC_RADIX     8

/* The one line limit, shared by the writer's refusal and the reader's
   truncation report. Includes the terminating newline. */
#define SYSUAF_LINE_MAX      1024

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

/*
 * Parse one SYSUAF line into a record. 'line' is MODIFIED IN PLACE.
 * Returns  1 if a record was parsed,
 *          0 if the line is a comment or blank (not an error),
 *         -1 if the line is malformed (fewer than SYSUAF_MIN_FIELDS fields).
 */
int sysuaf_parse_line(char *line, sysuaf_record_t *rec);

/*
 * Render one record as the SYSUAF line that represents it, WITHOUT the
 * trailing newline.
 *
 * Returns the length written on success, or -1 if the record does not fit in
 * SYSUAF_LINE_MAX (counting the newline a writer will append). AN OVER-LENGTH
 * RECORD IS REFUSED, NEVER CLIPPED: a reader that silently truncates is how
 * the SYSTEM record went missing and the boot halted, and clipping here would
 * simply move that same silent data loss one process earlier.
 */
int sysuaf_format_record(const sysuaf_record_t *rec, char *out, size_t outsz);

/*
 * Read one line, reporting truncation instead of hiding it.
 *
 * Returns 1 when a line was read, 0 at end of file. When the line did not fit
 * in 'bufsz', *too_long is set to 1, the REST OF THE LINE IS CONSUMED (so the
 * caller's next read starts at a real record boundary and never at a
 * fragment), and the caller decides what to do -- which is never "carry on
 * with the prefix".
 */
int sysuaf_read_line(FILE *fp, char *buf, size_t bufsz, int *too_long);

/* FLAGS is stored in the file as a comma-separated list of UAI flag NAMES
   (what a system manager types at AUTHORIZE's /FLAGS=), and exposed through
   $GETUAI/$SETUAI as the UAI$M_* longword. These two functions are the only
   conversion between the two, so the file format has one answer and the API
   has one answer and neither invents the other's. An unrecognized name is
   ignored; an empty string is mask 0 and round-trips back to empty. */
uint32_t sysuaf_flags_to_mask(const char *flags);
void     sysuaf_mask_to_flags(uint32_t mask, char *out, size_t outsz);


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

/* Look up the account holding UIC 'uic' ((group << 16) | member).
   Returns 0 on success, -1 if no account holds it (vms-2f8: the rights
   database derives UIC identifiers from SYSUAF rather than keeping a second
   copy of every account's UIC that is free to disagree with this one). */
int sysuaf_lookup_by_uic(uint32_t uic, sysuaf_record_t *rec);

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
