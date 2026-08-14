/* hello-crypto.c — the P-C acceptance probe (bead vms-2e8).
 *
 * Proves the vendored static libcrypto.a is actually LINKABLE and CORRECT: it
 * calls two libcrypto primitives and checks them against known answers, so a
 * pass means the OpenSSH port (vms-22a / vms-0cd) can link crypto against this
 * lib for the musl-static bootable image.
 *
 *   1. SHA-256 of "abc"  (FIPS 180-2 test vector) must equal
 *      ba7816bf 8f01cfea 414140de 5dae2223 b00361a3 96177a9c b410ff61 f20015ad
 *   2. A bignum op (7 * 6 = 42) via BN_mul, to exercise the crypto/bn path too.
 *
 * Built + statically linked + run by run_hello_crypto.sh. No OpenSSH here —
 * this is a standalone linkability proof only.
 */
#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#include <openssl/bn.h>

static const char *EXPECT_SHA256_ABC =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

static int check_sha256(void)
{
    unsigned char md[SHA256_DIGEST_LENGTH];
    char hex[2 * SHA256_DIGEST_LENGTH + 1];
    const char *msg = "abc";

    SHA256((const unsigned char *)msg, strlen(msg), md);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        snprintf(hex + 2 * i, 3, "%02x", md[i]);

    printf("SHA256(\"abc\") = %s\n", hex);
    if (strcmp(hex, EXPECT_SHA256_ABC) != 0) {
        printf("FAIL: SHA-256 known-answer mismatch\n  expected %s\n",
               EXPECT_SHA256_ABC);
        return 1;
    }
    printf("  OK: SHA-256 known-answer matches\n");
    return 0;
}

static int check_bignum(void)
{
    BN_CTX *ctx = BN_CTX_new();
    BIGNUM *a = BN_new(), *b = BN_new(), *r = BN_new();
    int rc = 1;

    if (!ctx || !a || !b || !r)
        goto out;
    BN_set_word(a, 7);
    BN_set_word(b, 6);
    if (!BN_mul(r, a, b, ctx))
        goto out;
    printf("BN_mul(7, 6) = %s\n", BN_bn2dec(r));
    if (!BN_is_word(r, 42)) {
        printf("FAIL: bignum multiply mismatch (expected 42)\n");
        goto out;
    }
    printf("  OK: bignum multiply matches\n");
    rc = 0;
out:
    BN_free(r); BN_free(b); BN_free(a); BN_CTX_free(ctx);
    return rc;
}

int main(void)
{
    printf("== hello-crypto: linking the vendored static libcrypto ==\n");
    if (check_sha256())
        return 1;
    if (check_bignum())
        return 1;
    printf("PASS: static libcrypto is linkable and correct\n");
    return 0;
}
