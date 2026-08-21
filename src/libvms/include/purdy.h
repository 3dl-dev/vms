/*
 * purdy.h - VMS Purdy password one-way hash (UAI$C_PURDY_S).
 *
 * This is the REAL OpenVMS password hash the SYSUAF record stores in
 * UAF$Q_PWD (an 8-byte quadword) -- it REPLACES the SHA-256 facade that the
 * legacy sysuaf.c auth path used (vms-631e, epic vms-d0c). The output is
 * byte-exact against real OpenVMS AUTHORIZE, verified against captured VAX
 * V7.3 + Alpha V8.4 (password, username, salt) -> UAF$Q_PWD vectors
 * (docs/oracle/purdy-hash-vectors.md).
 *
 * ALGORITHM (public, clean-room -- CLAUDE.md Rule 8). Derived from the public
 * description in the OpenVMS Guide to System Security ("password encryption":
 * a Purdy polynomial one-way hash over GF(2^64 - 59), salted, folding in the
 * username) together with the widely published, independently reimplemented
 * LGI$HPWD algorithm (Shawn Clifford's C, the phrack VAX-assembler listing,
 * the JtR/VMSCRACK lineage -- all public, none of it VSI/HPE source). The
 * numeric coefficients below come from those public references and are pinned
 * by the oracle vectors; NO VMS binary was disassembled or decompiled.
 *
 * Field: GF(P), P = 2^64 - 59 (the largest quadword prime).
 * Polynomial: f(X) = X^N0 + C1*X^N1 + C2*X^3 + C3*X^2 + C4*X + C5  (mod P)
 *   N0 = 2^24 - 3 (0xFFFFFD), N1 = 2^24 - 63 (0xFFFFC1),
 *   C1..C5 = P-83, P-179, P-257, P-323, P-363 (see purdy.c).
 * PURDY_S pre-hash: fold the password LENGTH into the seed, COLLAPSE the
 * password bytes into the running quadword (rotating each 32-bit half left by
 * one bit as the top byte is touched -- the "S" bit rotation), mix the 16-bit
 * salt in at an unaligned offset, COLLAPSE the username bytes, then evaluate
 * the polynomial. The result is the little-endian on-disk UAF$Q_PWD quadword.
 *
 * FIXED WIDTH, NO SUBSTRATE #ifdef: all arithmetic is uint64_t/uint32_t. On a
 * 32-bit substrate (VAX ILP32) uint64_t is the compiler-provided 64-bit type;
 * there is no __int128 and no 64-bit-only assumption, so the same source is
 * byte-identical on x86_64/aarch64 LP64 and elf32-vax ILP32.
 */
#ifndef OVMX_PURDY_H
#define OVMX_PURDY_H

#include <stdint.h>
#include <stddef.h>

/* UAF$B_ENCRYPT algorithm byte for the hash this module computes. */
#define PURDY_ALG_PURDY_S  3   /* UAI$C_PURDY_S -- the modern VMS default */

/* Compute the UAI$C_PURDY_S (salted Purdy, "Hickory") one-way hash.
 *
 *   password  - plaintext password bytes (NOT NUL-terminated-dependent).
 *   pwlen      - password length in bytes (VMS max 32; clamped to 32).
 *   username  - account name, NUL-terminated; trailing blanks are trimmed and
 *               the name is upcased internally (VMS max 31; clamped to 31).
 *   salt       - the per-account UAF$W_SALT word.
 *
 * The password is upcased internally as well (VMS treats password case as
 * insignificant). Returns the 64-bit hash as a host value; store it
 * little-endian at UAF$Q_PWD (sysuaf_rec_set_password / p3_put_le64).
 *
 * The caller is responsible for validating the character set (A-Z 0-9 $ _)
 * exactly as SYS$HASH_PASSWORD requires; this routine does not reject
 * out-of-set bytes, it just hashes what it is given.
 */
uint64_t purdy_s_hash(const char *password, size_t pwlen,
                      const char *username, uint16_t salt);

#endif /* OVMX_PURDY_H */
