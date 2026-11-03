/* SPDX-License-Identifier: MIT */
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(uint32_t h[8], const uint8_t *p)
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 |
		       (uint32_t)p[4 * i + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
		uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		k = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	h[5] += f;
	h[6] += g;
	h[7] += k;
}

void sha256_init(sha256_ctx *c)
{
	static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
				       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	memcpy(c->h, iv, sizeof iv);
	c->len = 0;
	c->n = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t len)
{
	const uint8_t *p = data;
	c->len += len;
	if (c->n) {
		size_t take = 64 - c->n < len ? 64 - c->n : len;
		memcpy(c->buf + c->n, p, take);
		c->n += take;
		p += take;
		len -= take;
		if (c->n < 64)
			return;
		block(c->h, c->buf);
		c->n = 0;
	}
	for (; len >= 64; p += 64, len -= 64)
		block(c->h, p);
	memcpy(c->buf, p, len);
	c->n = len;
}

void sha256_final(sha256_ctx *c, uint8_t out[32])
{
	uint64_t bits = c->len * 8;
	c->buf[c->n++] = 0x80;
	if (c->n > 56) {
		memset(c->buf + c->n, 0, 64 - c->n);
		block(c->h, c->buf);
		c->n = 0;
	}
	memset(c->buf + c->n, 0, 56 - c->n);
	for (int i = 0; i < 8; i++)
		c->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
	block(c->h, c->buf);
	for (int i = 0; i < 8; i++) {
		out[4 * i] = (uint8_t)(c->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)c->h[i];
	}
}

void sha256_final_hex(sha256_ctx *c, char out[65])
{
	static const char hex[] = "0123456789abcdef";
	uint8_t d[32];
	sha256_final(c, d);
	for (int i = 0; i < 32; i++) {
		out[2 * i] = hex[d[i] >> 4];
		out[2 * i + 1] = hex[d[i] & 15];
	}
	out[64] = 0;
}

int sha256_file(const char *path, char out[65])
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
	sha256_ctx c;
	sha256_init(&c);
	static uint8_t buf[1 << 17];
	for (;;) {
		ssize_t n = read(fd, buf, sizeof buf);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			int e = errno;
			close(fd);
			errno = e;
			return -1;
		}
		if (n == 0)
			break;
		sha256_update(&c, buf, (size_t)n);
	}
	close(fd);
	sha256_final_hex(&c, out);
	return 0;
}
