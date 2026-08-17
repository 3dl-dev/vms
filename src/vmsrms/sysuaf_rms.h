/*
 * sysuaf_rms.h - SYSUAF as a genuine binary RMS Prolog-3 INDEXED file
 * (vms-f88, epic vms-d0c). The real $UAFDEF record (src/libvms/include/
 * sysuaf.h, 644 bytes, oracle-pinned offsets) authored and read through the
 * Files-11 Prolog-3 engine (rms_prolog3.{c,h}) over the executive ACP window
 * -- primary key = 32-byte USERNAME (key of reference 0), secondary key = the
 * UIC longword (key of reference 1, resolved via SIDR). This SUPERSEDES the
 * ASCII+SHA-256 SYSUAF.DAT facade; it stores/reads the binary record only.
 *
 * SUBSTRATE-AGNOSTIC (vms-5f0 thin-seam). The API takes an already-opened
 * rms_file_t handle (rms_io.h): an ACP channel window when /dev/vms is present
 * (Rule 9 / INV-6), or the rms_io POSIX backend (rms_io_posix_wrap) on the
 * executive-absent host defer / netbsd-vax cross. There is NO substrate #ifdef
 * here and no /vms path -- the handle decides the substrate, the record bytes
 * are identical either way. The caller owns the handle; sysuaf_rms_close frees
 * only the bound Prolog-3 context.
 *
 * ⚠ NO PASSWORD HASHING HERE. sysuaf_put_record stores the record's password
 * quadword + salt + encrypt byte verbatim; computing the Purdy hash from
 * (password, username, salt) is the next rung (vms-631e). Consumers of the
 * live auth path (sysuaf.c / sys_uai.c) are flipped onto this engine by C1
 * (vms-d92) -- this rung adds the real binary path ALONGSIDE the ASCII path.
 *
 * Oracle grounding: docs/oracle/vax73-alpha84-uafdef.md (record layout),
 * docs/oracle/vax73-alpha84-rms-prolog3.md (the indexed envelope).
 */
#ifndef SYSUAF_RMS_H
#define SYSUAF_RMS_H

#include <stdint.h>
#include <string.h>

#include "sysuaf.h"        /* sysuaf_rms_record_t (644-byte $UAFDEF) + offsets */
#include "rms_prolog3.h"   /* the Prolog-3 engine + p3_le/p3_put_le accessors  */

/* Key of reference in the SYSUAF indexed file. */
#define SYSUAF_KRF_USERNAME  0u
#define SYSUAF_KRF_UIC       1u

/* Bound SYSUAF indexed file: a borrowed rms_file_t handle plus the Prolog-3
 * context authored/bound over it. */
typedef struct sysuaf_rms_file {
    rms_file_t *f;     /* borrowed handle (ACP window or POSIX wrap) -- NOT owned */
    p3_ctx_t   *ctx;   /* bound/created Prolog-3 context (owned; freed on close)  */
} sysuaf_rms_file_t;

/* ---- record field accessors (binary, fixed-width little-endian) ---------- */

/* UIC longword: (group << 16) | member, stored LE at [PIN] offset 0x24. */
static inline void sysuaf_rec_set_uic(sysuaf_rms_record_t *r,
                                      uint16_t group, uint16_t member)
{
    p3_put_le32(r->uaf$l_uic, ((uint32_t)group << 16) | (uint32_t)member);
}
static inline uint32_t sysuaf_rec_uic(const sysuaf_rms_record_t *r)
{
    return p3_le32(r->uaf$l_uic);
}

/* USERNAME primary key: upcased, blank-padded to 32 at [PIN] offset 0x04. */
void sysuaf_rec_set_username(sysuaf_rms_record_t *r, const char *username);

/* Password FIELD (bytes only -- NOT hashed here; see vms-631e seam):
 * store the quadword, salt word and algorithm byte at their [PIN] offsets. */
static inline void sysuaf_rec_set_password(sysuaf_rms_record_t *r,
                                           uint64_t pwd_quad,
                                           uint16_t salt,
                                           uint8_t  encrypt,
                                           uint8_t  pwd_length)
{
    p3_put_le64(r->uaf$q_pwd, pwd_quad);
    p3_put_le16(r->uaf$w_salt, salt);
    r->uaf$b_encrypt    = encrypt;
    r->uaf$b_pwd_length = pwd_length;
}
static inline uint64_t sysuaf_rec_pwd(const sysuaf_rms_record_t *r)
{
    return p3_le64(r->uaf$q_pwd);
}
static inline uint16_t sysuaf_rec_salt(const sysuaf_rms_record_t *r)
{
    return p3_le16(r->uaf$w_salt);
}

/* ---- password HASHING seam (vms-631e, epic vms-d0c) ---------------------- *
 * The real Purdy hash. sysuaf_rec_set_password (above) stores an
 * already-computed quadword AS BYTES; these two functions are where a
 * PLAINTEXT password meets the record -- computing UAF$Q_PWD with the genuine
 * UAI$C_PURDY_S one-way hash (src/libvms/rtl/purdy.c, byte-exact vs real VMS
 * AUTHORIZE) and verifying a login attempt against the stored quadword. This
 * REPLACES the SHA-256 facade the legacy sysuaf.c auth path used. The live
 * auth flip (sysuaf.c / sys_uai.c) is C1 (vms-d92); this rung provides the
 * functions and fills the record seam. Implemented in sysuaf_rms.c. */

/* Compute the PURDY_S hash of `password` for this record's identity and store
 * it, the `salt`, the algorithm byte (UAI$C_PURDY_S) and `pwd_length` at their
 * [PIN] offsets. The username folded into the hash is taken from the record's
 * UAF$T_USERNAME field, so set the username (sysuaf_rec_set_username) FIRST. */
void sysuaf_rec_set_password_plaintext(sysuaf_rms_record_t *r,
                                       const char *password, size_t pwlen,
                                       uint16_t salt, uint8_t pwd_length);

/* Verify a login attempt: hash `password` with the record's stored salt and
 * username, compare byte-exact to the stored UAF$Q_PWD quadword. Returns 1 on
 * match, 0 otherwise (including a record whose UAF$B_ENCRYPT is not
 * UAI$C_PURDY_S -- OVMX only computes that variant, so any other algorithm
 * byte fails honestly rather than faking a match). */
int sysuaf_rec_verify_password(const sysuaf_rms_record_t *r,
                               const char *password, size_t pwlen);

/* ---- indexed-file API ---------------------------------------------------- */

/* Author a fresh, EMPTY SYSUAF indexed file over the (writable) handle `f`:
 * a Prolog-3 image with the primary USERNAME key (unique) and the secondary
 * UIC key (duplicates allowed). On success *sf is a bound context ready for
 * sysuaf_put_record. Returns RMS$_CREATED, or an RMS$_ error (RMS$_FAB for a
 * NULL argument; the engine's RMS$_WPL/RMS$_KEY/RMS$_PLG/RMS$_DME otherwise).*/
uint32_t sysuaf_rms_create(rms_file_t *f, sysuaf_rms_file_t *sf);

/* Bind an EXISTING SYSUAF indexed file on the handle `f` (parses the prologue;
 * requires the 2-key USERNAME+UIC image sysuaf_rms_create authors). Returns
 * RMS$_NORMAL, RMS$_FAB (NULL arg), or RMS$_PLG on a bad/foreign prologue. */
uint32_t sysuaf_rms_open(rms_file_t *f, sysuaf_rms_file_t *sf);

/* Write one $UAFDEF record (644 bytes): $PUT into the primary key; the engine
 * maintains the UIC SIDR automatically from the record's embedded UIC field.
 * Returns RMS$_NORMAL, RMS$_DUP (username already present), RMS$_FAB, or an
 * engine I/O status. */
uint32_t sysuaf_put_record(sysuaf_rms_file_t *sf,
                           const sysuaf_rms_record_t *rec);

/* Read the record for `username` by the PRIMARY key (upcased/blank-padded to
 * 32 internally). Returns RMS$_NORMAL, RMS$_RNF (no such user), RMS$_FAB. */
uint32_t sysuaf_get_by_username(sysuaf_rms_file_t *sf, const char *username,
                                sysuaf_rms_record_t *out);

/* Read a record holding UIC `uic` ((group<<16)|member) by the SECONDARY key:
 * descends the UIC index to the SIDR, takes the first primary RFA, resolves
 * the primary record. Returns RMS$_NORMAL, RMS$_RNF (no account with that
 * UIC), RMS$_FAB. (Duplicate UICs: the first is returned; use the engine's
 * rms_p3_sidr_lookup for the full pointer array.) */
uint32_t sysuaf_get_by_uic(sysuaf_rms_file_t *sf, uint32_t uic,
                           sysuaf_rms_record_t *out);

/* Free the bound Prolog-3 context. Does NOT close/unwrap the handle `f`
 * (the caller owns it). Safe on a zeroed / partially-created sf. */
void sysuaf_rms_close(sysuaf_rms_file_t *sf);

#endif /* SYSUAF_RMS_H */
