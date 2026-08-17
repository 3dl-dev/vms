/*
 * test_sysuaf_record.c - the REAL binary $UAFDEF SYSUAF record over the
 * genuine Files-11 Prolog-3 indexed engine (vms-f88, epic vms-d0c).
 *
 * Supersedes the ASCII+SHA-256 / 368-byte facade the operator caught. Two
 * halves:
 *
 *  1. LAYOUT -- the on-disk $UAFDEF record is 644 bytes (both VAX V7.3 and
 *     Alpha V8.4, per docs/oracle/vax73-alpha84-uafdef.md) with every
 *     oracle-[PIN] field at its exact byte offset: USERNAME@0x04, UIC@0x24,
 *     UAF$Q_PWD@0x154, UAF$W_SALT@0x166, UAF$B_ENCRYPT@0x168, PWD_LENGTH@0x16A.
 *
 *  2. INDEXED FILE -- author a binary SYSUAF indexed file with sysuaf_rms_create
 *     (primary key = 32-byte USERNAME, secondary key = UIC longword), $PUT a
 *     few accounts, then read them back BOTH by username (primary) AND by UIC
 *     (secondary via SIDR), asserting the record bytes land at the exact oracle
 *     offsets and round-trip byte-exact. This drives the genuine Prolog-3
 *     engine (rms_prolog3.c) through sysuaf_rms.c -- no flat file, no /vms
 *     passthrough, no SHA-256. Host ctest wraps a plain fd (rms_io_posix_wrap);
 *     the /dev/vms ACP end-to-end is the engine's paired positive
 *     tests/qemu/test_syssvc_rms_p3_acp.c.
 *
 * ⚠ NO PASSWORD IS HASHED HERE. The password quadword is stored AS BYTES (the
 * record field); computing the Purdy hash is the next rung (vms-631e). Where a
 * hash value is needed it is a LABELLED oracle test-vector fixture
 * (docs/oracle/purdy-hash-vectors.md V1), never a computed or fake hash.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#include "sysuaf.h"
#include "sysuaf_rms.h"
#include "rmsdef.h"
#include "ssdef.h"

static int failures = 0;

static void check(int cond, const char *name)
{
    if (cond) { printf("  OK: %s\n", name); }
    else      { printf("  FAIL: %s\n", name); failures++; }
}

/* Oracle test-vector FIXTURE (docs/oracle/purdy-hash-vectors.md V1): A1ORA /
 * KNOWNPW12 / salt 0x4D63 -> UAF$Q_PWD = 0x716CBDC03C071C59, ENCRYPT = PURDY_S.
 * Used here ONLY as a stored byte pattern to prove the password FIELD lands at
 * the pinned offsets and round-trips; it is NOT computed and NOT tied to the
 * SYSTEM username. The Purdy computation is vms-631e. */
#define FIX_PWD_QUAD  0x716CBDC03C071C59ull
#define FIX_SALT      0x4D63u
#define FIX_PWD_LEN   6u

static void build_record(sysuaf_rms_record_t *r, const char *user,
                         uint16_t grp, uint16_t mem, int with_pw_fixture)
{
    memset(r, 0, sizeof(*r));
    r->uaf$b_rtype   = 1;
    r->uaf$b_version = 1;
    sysuaf_rec_set_username(r, user);
    sysuaf_rec_set_uic(r, grp, mem);
    if (with_pw_fixture)
        sysuaf_rec_set_password(r, FIX_PWD_QUAD, FIX_SALT,
                                UAI$C_PURDY_S, FIX_PWD_LEN);
}

int main(void)
{
    printf("test_sysuaf_record (vms-f88): binary $UAFDEF over Prolog-3 index\n");

    /* ===== 1. LAYOUT: 644-byte record, oracle [PIN] offsets ============== */
    check(sizeof(sysuaf_rms_record_t) == 644, "record is 644 bytes ($UAFDEF)");
    check(offsetof(sysuaf_rms_record_t, uaf$t_username) == 0x04, "USERNAME @0x04");
    check(offsetof(sysuaf_rms_record_t, uaf$l_uic)      == 0x24, "UIC @0x24");
    check(offsetof(sysuaf_rms_record_t, uaf$q_owner_id) == 0x2C, "owner id @0x2C");
    check(offsetof(sysuaf_rms_record_t, uaf$q_pwd)      == 0x154, "UAF$Q_PWD @0x154");
    check(offsetof(sysuaf_rms_record_t, uaf$w_salt)     == 0x166, "UAF$W_SALT @0x166");
    check(offsetof(sysuaf_rms_record_t, uaf$b_encrypt)  == 0x168, "UAF$B_ENCRYPT @0x168");
    check(offsetof(sysuaf_rms_record_t, uaf$b_pwd_length) == 0x16A, "PWD_LENGTH @0x16A");
    check(offsetof(sysuaf_rms_record_t, uaf$q_pwd2)     == 0x16C, "UAF$Q_PWD2 @0x16C");
    check(sizeof(((sysuaf_rms_record_t *)0)->uaf$t_username) == 32, "username width 32");
    check(sizeof(((sysuaf_rms_record_t *)0)->uaf$q_pwd) == 8, "pwd is an 8-byte quadword");
    check(sizeof(((sysuaf_rms_record_t *)0)->uaf$w_salt) == 2, "salt is a 2-byte word");

    /* ===== 1b. PASSWORD HASHING SEAM (vms-631e): the real PURDY_S hash ===
     * Fill the record's password from PLAINTEXT via the seam helper and prove
     * it lands byte-exact on the real OpenVMS oracle quadword (V1: A1ORA /
     * KNOWNPW12 / salt 0x4D63 -> 0x716CBDC03C071C59), then verify a login. */
    {
        sysuaf_rms_record_t r;
        memset(&r, 0, sizeof(r));
        r.uaf$b_rtype = 1; r.uaf$b_version = 1;
        sysuaf_rec_set_username(&r, "A1ORA");        /* username FIRST */
        sysuaf_rec_set_password_plaintext(&r, "KNOWNPW12", 9,
                                          0x4D63u, FIX_PWD_LEN);
        check(sysuaf_rec_pwd(&r) == 0x716CBDC03C071C59ull,
              "set_password_plaintext computes the real oracle V1 UAF$Q_PWD");
        check(sysuaf_rec_salt(&r) == 0x4D63u, "salt stored");
        check(r.uaf$b_encrypt == UAI$C_PURDY_S, "encrypt byte = PURDY_S");
        check(sysuaf_rec_verify_password(&r, "KNOWNPW12", 9) == 1,
              "verify_password accepts the correct password");
        check(sysuaf_rec_verify_password(&r, "WRONGPW00", 9) == 0,
              "verify_password rejects a wrong password");
        check(sysuaf_rec_verify_password(&r, "knownpw12", 9) == 1,
              "verify_password is case-insensitive (VMS upcases)");
    }

    /* ===== 2. INDEXED FILE: create, put, read by username AND by UIC ===== */
    char path[] = "/tmp/ovmx_sysuaf_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("mkstemp failed\n"); return 1; }

    rms_file_t *f = rms_io_posix_wrap(fd);
    check(f != NULL, "wrap fd (host rms_io POSIX backend, no /dev/vms)");

    sysuaf_rms_file_t sf;
    uint32_t st = sysuaf_rms_create(f, &sf);
    check(st == RMS$_CREATED && sf.ctx != NULL,
          "sysuaf_rms_create authors the binary indexed SYSUAF (2 keys)");
    if (!sf.ctx) { rms_io_posix_unwrap(f); unlink(path); return 1; }

    /* the created prologue is a real 2-key Prolog-3 image (username + UIC) */
    check(sf.ctx->num_keys == 2 &&
          sf.ctx->keys[0].key_size == 32 && sf.ctx->keys[0].seg0_pos == 0x04 &&
          sf.ctx->keys[1].key_size == 4  && sf.ctx->keys[1].seg0_pos == 0x24,
          "prologue: USERNAME key @0x04/32 + UIC secondary key @0x24/4");

    /* three accounts, distinct UICs; SYSTEM carries the pwd-field fixture */
    struct { const char *user; uint16_t grp, mem; int fix; } acct[] = {
        { "SYSTEM", 1,   4,   1 },
        { "JULIE",  1,   100, 0 },
        { "GUEST",  200, 1,   0 },
    };
    const int NACCT = (int)(sizeof(acct) / sizeof(acct[0]));

    int put_ok = 1;
    for (int i = 0; i < NACCT; i++) {
        sysuaf_rms_record_t r;
        build_record(&r, acct[i].user, acct[i].grp, acct[i].mem, acct[i].fix);
        uint32_t ps = sysuaf_put_record(&sf, &r);
        if (!$VMS_STATUS_SUCCESS(ps)) {
            printf("    put %s -> 0x%X\n", acct[i].user, ps); put_ok = 0;
        }
    }
    check(put_ok, "sysuaf_put_record wrote every account into the index");

    /* ---- read back BY USERNAME (primary key) + byte-exact round-trip ---- */
    int by_user_ok = 1, roundtrip_ok = 1;
    for (int i = 0; i < NACCT; i++) {
        sysuaf_rms_record_t want, got;
        build_record(&want, acct[i].user, acct[i].grp, acct[i].mem, acct[i].fix);
        memset(&got, 0xAA, sizeof(got));
        uint32_t gs = sysuaf_get_by_username(&sf, acct[i].user, &got);
        if (!$VMS_STATUS_SUCCESS(gs)) { by_user_ok = 0; continue; }
        if (memcmp(&want, &got, sizeof(want)) != 0) roundtrip_ok = 0;
    }
    check(by_user_ok, "every account resolves BY USERNAME (primary key)");
    check(roundtrip_ok, "primary read-back is byte-exact over all 644 bytes");

    /* ---- read back BY UIC (secondary key, via SIDR) -> right username --- */
    int by_uic_ok = 1;
    for (int i = 0; i < NACCT; i++) {
        sysuaf_rms_record_t got, want;
        uint32_t uic = ((uint32_t)acct[i].grp << 16) | acct[i].mem;
        memset(&got, 0, sizeof(got));
        uint32_t gs = sysuaf_get_by_uic(&sf, uic, &got);
        /* the resolved primary record must be the account holding that UIC */
        build_record(&want, acct[i].user, acct[i].grp, acct[i].mem, acct[i].fix);
        if (!$VMS_STATUS_SUCCESS(gs) ||
            memcmp(&want, &got, sizeof(want)) != 0) {
            printf("    uic-lookup miss %s uic=%08x -> 0x%X\n",
                   acct[i].user, uic, gs);
            by_uic_ok = 0;
        }
    }
    check(by_uic_ok,
          "every account resolves BY UIC (secondary index descent + SIDR + RFA)");

    /* ---- dump the SYSTEM record bytes; assert the pinned oracle offsets --- */
    {
        sysuaf_rms_record_t sys;
        memset(&sys, 0, sizeof(sys));
        uint32_t gs = sysuaf_get_by_username(&sf, "SYSTEM", &sys);
        check($VMS_STATUS_SUCCESS(gs), "re-read SYSTEM for byte-offset dump");
        const uint8_t *b = (const uint8_t *)&sys;

        /* USERNAME @0x04: "SYSTEM" upcased + blank padded to 32 */
        uint8_t exp_user[32];
        for (int i = 0; i < 32; i++) exp_user[i] = (i < 6) ? "SYSTEM"[i] : ' ';
        check(memcmp(b + 0x04, exp_user, 32) == 0,
              "bytes @0x04 == \"SYSTEM\" blank-padded (primary key)");

        /* UIC @0x24: [1,4] = 0x00010004, little-endian longword */
        uint8_t exp_uic[4] = { 0x04, 0x00, 0x01, 0x00 };
        check(memcmp(b + 0x24, exp_uic, 4) == 0,
              "bytes @0x24 == UIC [1,4] LE longword 0x00010004");
        check(p3_le32(b + 0x24) == 0x00010004u, "UIC decodes to (group<<16)|member");

        /* UAF$Q_PWD @0x154: the fixture quadword, little-endian */
        uint8_t exp_pwd[8];
        p3_put_le64(exp_pwd, FIX_PWD_QUAD);
        check(memcmp(b + 0x154, exp_pwd, 8) == 0,
              "bytes @0x154 == UAF$Q_PWD fixture quadword (V1, NOT a hash we compute)");

        /* UAF$W_SALT @0x166, UAF$B_ENCRYPT @0x168 */
        check(p3_le16(b + 0x166) == FIX_SALT, "word @0x166 == UAF$W_SALT fixture");
        check(b[0x168] == UAI$C_PURDY_S, "byte @0x168 == UAF$B_ENCRYPT = 0x03 (PURDY_S)");
        check(b[0x16A] == FIX_PWD_LEN, "byte @0x16A == UAF$B_PWD_LENGTH");
    }

    /* ---- fail-honest: a bind of the same file re-parses the 2-key image --- */
    {
        sysuaf_rms_file_t reopened;
        /* rewind + rebind through a fresh handle on the same fd contents */
        int fd2 = open(path, O_RDWR);
        check(fd2 >= 0, "reopen SYSUAF file for a fresh bind");
        rms_file_t *f2 = rms_io_posix_wrap(fd2);
        uint32_t os = sysuaf_rms_open(f2, &reopened);
        check($VMS_STATUS_SUCCESS(os) && reopened.ctx &&
              reopened.ctx->num_keys == 2,
              "sysuaf_rms_open re-binds the on-disk 2-key Prolog-3 image");
        if ($VMS_STATUS_SUCCESS(os)) {
            sysuaf_rms_record_t got;
            uint32_t gs = sysuaf_get_by_username(&reopened, "GUEST", &got);
            check($VMS_STATUS_SUCCESS(gs) &&
                  sysuaf_rec_uic(&got) == ((200u << 16) | 1u),
                  "reopened index still resolves GUEST by username, UIC intact");
        }
        sysuaf_rms_close(&reopened);
        rms_io_posix_unwrap(f2);
    }

    sysuaf_rms_close(&sf);
    rms_io_posix_unwrap(f);   /* closes fd */
    unlink(path);

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
