/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cluster_sha256.h - SHA-256 for the host cluster fixture loader (FC-P0.6).
 *
 * Host-test support only; never linked into the executive. It exists so the
 * fixture loader can verify a specimen's chain of custody with no external
 * dependency (OpenSSL is not a build dependency of this repo and a test that
 * needs one is a test that gets skipped -- and a skipped test is a failing
 * test). The algorithm is FIPS 180-4, a public standard; nothing here is VMS
 * reverse engineering.
 */
#ifndef OVMX_CLUSTER_SHA256_H
#define OVMX_CLUSTER_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* Writes 64 lowercase hex chars + NUL into `hex` (>= 65 bytes). */
void cluster_sha256_hex(const uint8_t *data, size_t len, char *hex);

#endif /* OVMX_CLUSTER_SHA256_H */
