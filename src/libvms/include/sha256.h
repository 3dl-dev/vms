/*
 * sha256.h - SHA-256 hash function
 *
 * Public domain implementation for OVMX project.
 * Based on the FIPS 180-4 specification.
 */

#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_BLOCK_SIZE  32  /* SHA-256 outputs 32 bytes */

typedef struct {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} SHA256_CTX;

void sha256_init(SHA256_CTX *ctx);
void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len);
void sha256_final(SHA256_CTX *ctx, uint8_t hash[SHA256_BLOCK_SIZE]);

/* Convenience: hash data and produce hex string (65 bytes including NUL) */
void sha256_hex(const uint8_t *data, size_t len, char hex[65]);

#endif /* SHA256_H */
