/*
 * purdy.c - VMS Purdy password one-way hash (UAI$C_PURDY_S). See purdy.h for
 * the algorithm, provenance (public / clean-room, Rule 8), and the oracle bar.
 *
 * Verified byte-exact against the 7 real OpenVMS vectors (4 VAX V7.3 + 3 Alpha
 * V8.4) in docs/oracle/purdy-hash-vectors.md and against the 200 published
 * DecHpwd PURDY_S calibration vectors -- see tests/libvms/test_purdy.c.
 *
 * Arithmetic is over GF(P), P = 2^64 - 59 ("cmod P" = conditional reduce: the
 * intermediate ops only fix up on a 2^64 overflow; a final real mod P is done
 * once at the end of the polynomial). Every value is a uint64_t; 64x64->128 is
 * never needed because the modular multiply decomposes the product into four
 * 32x32->64 partials scaled by 2^32 (2^64 == 59*... cmod P), exactly the
 * classic LGI$HPWD decomposition. That keeps the code correct on a 32-bit
 * substrate (VAX ILP32) where __int128 does not exist.
 */
#include "purdy.h"

/* P = 2^64 - A, A = 59 (largest quadword prime). */
#define PURDY_A          59ull

/* Polynomial coefficients C1..C5 (public LGI$HPWD table; == P minus a small
 * constant each, pinned by the oracle vectors). */
#define PURDY_C1  0xFFFFFFFFFFFFFFADull   /* P - 83  */
#define PURDY_C2  0xFFFFFFFFFFFFFF4Dull   /* P - 179 */
#define PURDY_C3  0xFFFFFFFFFFFFFEFFull   /* P - 257 */
#define PURDY_C4  0xFFFFFFFFFFFFFEBDull   /* P - 323 */
#define PURDY_C5  0xFFFFFFFFFFFFFE95ull   /* P - 363 */

/* Exponents. N0 = 2^24-3, N1 = 2^24-63. N1-1 factors as Na*Nb (chosen for a
 * minimal-Hamming-weight power ladder). */
#define PURDY_N0  0xFFFFFDul
#define PURDY_N1  0xFFFFC1ul
#define PURDY_NA  448ul
#define PURDY_NB  37449ul

/* VMS field-width clamps (SYS$HASH_PASSWORD: password <=32, username <=31). */
#define PURDY_PW_MAX    32u
#define PURDY_USER_MAX  31u

static inline uint32_t lo32(uint64_t x) { return (uint32_t)x; }
static inline uint32_t hi32(uint64_t x) { return (uint32_t)(x >> 32); }

/* (U + Y) cmod P: add, and if the sum overflowed 2^64 fold the deficit A back
 * in (twice if the fold itself overflows -- both operands may exceed P). */
static uint64_t pqadd(uint64_t u, uint64_t y)
{
    uint64_t r = u + y;
    if (r < u) {                 /* 2^64 overflow */
        r += PURDY_A;
        while (r < PURDY_A)      /* the fold itself overflowed */
            r += PURDY_A;
    }
    return r;
}

/* 2^32 * U cmod P. Uses 2^64 == A (cmod P), so the high half v contributes
 * A*v: result = 2^32*u + A*v. */
static uint64_t pqlsh(uint64_t u)
{
    uint64_t av = (uint64_t)hi32(u) * PURDY_A;   /* A*v (fits in 64 bits)   */
    uint64_t x  = (uint64_t)lo32(u) << 32;       /* 2^32*u                  */
    return pqadd(x, av);
}

/* 32x32 -> 64 exact product (safe on ILP32: uint64_t is compiler-provided). */
static inline uint64_t emulq(uint32_t a, uint32_t b)
{
    return (uint64_t)a * (uint64_t)b;
}

/* U * Y cmod P, via the four 32x32 partials scaled by powers of 2^32. */
static uint64_t pqmul(uint64_t u, uint64_t y)
{
    uint64_t part1 = pqlsh(emulq(hi32(u), hi32(y)));            /* 2^32*(v*z)   */
    uint64_t vy    = emulq(hi32(u), lo32(y));                   /* v*y          */
    uint64_t uz    = emulq(lo32(u), hi32(y));                   /* u*z          */
    uint64_t mid   = pqadd(part1, pqadd(vy, uz));               /* 2^32vz+vy+uz */
    part1          = pqlsh(mid);                                /* *2^32        */
    return pqadd(part1, emulq(lo32(u), lo32(y)));               /* + u*y        */
}

/* U^n cmod P (right-to-left binary exponentiation; Knuth 4.6.3). */
static uint64_t pqexp(uint64_t u, unsigned long n)
{
    uint64_t acc = 0, sq = u;
    int have = 0;
    while (n != 0) {
        if (n & 1u) {
            acc  = have ? pqmul(acc, sq) : sq;
            have = 1;
            if (n == 1)
                return acc;
        }
        n >>= 1;
        sq = pqmul(sq, sq);
    }
    return 1ull;   /* U^0 */
}

/* f(U) mod P, evaluated as
 *   ((U^(N0-N1) + C1)*U^(N1-1) + (U*C2 + C3)*U + C4)*U + C5
 * with U^(N1-1) = (U^Na)^Nb. */
static uint64_t purdy_poly(uint64_t u)
{
    uint64_t un1m1 = pqexp(pqexp(u, PURDY_NA), PURDY_NB);       /* U^(N1-1)     */
    uint64_t part1 = pqmul(un1m1,
                           pqadd(pqexp(u, PURDY_N0 - PURDY_N1),
                                 PURDY_C1));                    /* first half   */
    uint64_t part2 = pqadd(pqmul(u,
                              pqadd(pqmul(u, PURDY_C2), PURDY_C3)),
                           PURDY_C4);                           /* second half  */
    uint64_t r = pqadd(pqmul(u, pqadd(part1, part2)), PURDY_C5);

    /* One real mod P (all the above were cmod P). Only 58 residues exceed P. */
    if (hi32(r) == 0xFFFFFFFFul && lo32(r) >= (uint32_t)(0ul - (uint32_t)PURDY_A))
        r = (uint64_t)(lo32(r) + (uint32_t)PURDY_A);   /* r - P */
    return r;
}

/* --- byte-level pre-hash (operates on the 8-byte little-endian seed) ------- */

/* Add a 16-bit value to the unaligned little-endian word at o[0..1]. */
static void add_word(uint8_t *o, uint16_t x)
{
    uint16_t w = (uint16_t)(o[0] | ((uint16_t)o[1] << 8));
    w = (uint16_t)(w + x);
    o[0] = (uint8_t)(w & 0xff);
    o[1] = (uint8_t)(w >> 8);
}

static inline uint32_t rd_le32(const uint8_t *o)
{
    return (uint32_t)o[0] | ((uint32_t)o[1] << 8) |
           ((uint32_t)o[2] << 16) | ((uint32_t)o[3] << 24);
}
static inline void wr_le32(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)v;        o[1] = (uint8_t)(v >> 8);
    o[2] = (uint8_t)(v >> 16); o[3] = (uint8_t)(v >> 24);
}

/* Rotate each 32-bit half of the little-endian seed left by one bit (the
 * PURDY_S "extra rotation"). The two halves rotate independently. */
static void qrol1(uint8_t *o)
{
    uint32_t l = rd_le32(o), h = rd_le32(o + 4);
    l = (l >> 31) | (l << 1);
    h = (h >> 31) | (h << 1);
    wr_le32(o, l);
    wr_le32(o + 4, h);
}

/* Collapse a byte string into the running 8-byte seed: cycle through the seed
 * bytes (index = position & 7) adding each input byte; when the top byte
 * (index 7) is touched, rotate (PURDY_S). Input position counts DOWN from len,
 * matching LGI$HPWD. */
static void collapse(const uint8_t *s, size_t len, uint8_t *o)
{
    for (size_t r0 = len; r0 != 0; r0--) {
        unsigned r1 = (unsigned)(r0 & 7u);
        o[r1] = (uint8_t)(o[r1] + *s++);
        if (r1 == 7u)
            qrol1(o);
    }
}

static inline uint8_t upcase(uint8_t c)
{
    return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 'a' + 'A') : c;
}

uint64_t purdy_s_hash(const char *password, size_t pwlen,
                      const char *username, uint16_t salt)
{
    uint8_t pw[PURDY_PW_MAX];
    uint8_t un[PURDY_USER_MAX];
    size_t  ulen = 0;
    uint8_t seed[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

    /* Upcase + clamp the password to the VMS field width. */
    if (pwlen > PURDY_PW_MAX)
        pwlen = PURDY_PW_MAX;
    for (size_t i = 0; i < pwlen; i++)
        pw[i] = upcase((uint8_t)password[i]);

    /* Upcase the username, dropping trailing blanks (the record stores it
     * blank-padded to 32; VMS hashes the trimmed variable-length name). */
    if (username) {
        size_t raw = 0;
        while (username[raw] != '\0' && raw < PURDY_USER_MAX)
            raw++;
        while (raw > 0 && username[raw - 1] == ' ')
            raw--;
        for (size_t i = 0; i < raw; i++)
            un[i] = upcase((uint8_t)username[i]);
        ulen = raw;
    }

    /* PURDY_S pre-hash: fold the password length in, collapse the password,
     * mix the salt at the unaligned middle, collapse the username. */
    add_word(seed, (uint16_t)pwlen);
    collapse(pw, pwlen, seed);
    add_word(seed + 3, salt);          /* salt spans bytes 3..4 (unaligned) */
    collapse(un, ulen, seed);

    /* Evaluate the polynomial; the seed is a little-endian quadword value. */
    return purdy_poly(rd_le32(seed) | ((uint64_t)rd_le32(seed + 4) << 32));
}
