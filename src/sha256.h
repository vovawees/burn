/* SPDX-License-Identifier: MIT */
/* SHA-256 (FIPS 180-4): source checksums, recipe and build identities. */
#ifndef BURN_SHA256_H
#define BURN_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t h[8];
	uint64_t len; /* bytes */
	uint8_t buf[64];
	size_t n;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256_final_hex(sha256_ctx *c, char out[65]);
int sha256_file(const char *path, char out[65]); /* 0, or -1 with errno */
/* "sha-ni" or "c": the block function this CPU uses */
const char *sha256_impl(void);

#endif
