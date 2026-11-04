/* SPDX-License-Identifier: MIT */
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__x86_64__) && defined(__GNUC__)
#include <cpuid.h>
#include <immintrin.h>
#define HAVE_SHANI 1
#endif

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

static void block_c(uint32_t h[8], const uint8_t *p)
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

static void blocks_c(uint32_t h[8], const uint8_t *p, size_t n)
{
	for (; n; n--, p += 64)
		block_c(h, p);
}

#ifdef HAVE_SHANI
/* the SHA extensions of x86 (Intel since Goldmont and Ice Lake, AMD since Zen): 4 rounds per
 * sha256rnds2 pair, the message schedule in sha256msg1/msg2. State as ABEF and CDGH. */
__attribute__((target("sha,sse4.1"))) static void blocks_shani(uint32_t h[8], const uint8_t *p, size_t n)
{
	const __m128i bswap = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
	__m128i t = _mm_loadu_si128((const __m128i *)&h[0]);  /* DCBA */
	__m128i s1 = _mm_loadu_si128((const __m128i *)&h[4]); /* HGFE */
	t = _mm_shuffle_epi32(t, 0xb1);                      /* CDAB */
	s1 = _mm_shuffle_epi32(s1, 0x1b);                    /* EFGH */
	__m128i s0 = _mm_alignr_epi8(t, s1, 8);              /* ABEF */
	s1 = _mm_blend_epi16(s1, t, 0xf0);                   /* CDGH */
	for (; n; n--, p += 64) {
		__m128i abef = s0, cdgh = s1, w[4];
		for (int i = 0; i < 4; i++)
			w[i] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 16 * i)), bswap);
		for (int g = 0; g < 16; g++) { /* rounds 4g..4g+3 with the words w[g & 3] */
			__m128i m = _mm_add_epi32(w[g & 3], _mm_loadu_si128((const __m128i *)&K[4 * g]));
			s1 = _mm_sha256rnds2_epu32(s1, s0, m);
			m = _mm_shuffle_epi32(m, 0x0e);
			s0 = _mm_sha256rnds2_epu32(s0, s1, m);
			if (g < 12) { /* the words of group g + 4 replace those of group g */
				__m128i x = _mm_sha256msg1_epu32(w[g & 3], w[(g + 1) & 3]);
				x = _mm_add_epi32(x, _mm_alignr_epi8(w[(g + 3) & 3], w[(g + 2) & 3], 4));
				w[g & 3] = _mm_sha256msg2_epu32(x, w[(g + 3) & 3]);
			}
		}
		s0 = _mm_add_epi32(s0, abef);
		s1 = _mm_add_epi32(s1, cdgh);
	}
	t = _mm_shuffle_epi32(s0, 0x1b);    /* FEBA */
	s1 = _mm_shuffle_epi32(s1, 0xb1);   /* DCHG */
	s0 = _mm_blend_epi16(t, s1, 0xf0);  /* DCBA */
	s1 = _mm_alignr_epi8(s1, t, 8);     /* HGFE */
	_mm_storeu_si128((__m128i *)&h[0], s0);
	_mm_storeu_si128((__m128i *)&h[4], s1);
}

static bool cpu_shani(void)
{
	unsigned a, b, c, d;
	if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & bit_SSSE3) || !(c & bit_SSE4_1))
		return false;
	return __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & bit_SHA);
}
#endif

/* the implementation for this CPU, chosen at the first use; BURN_SHA256=c forces the portable one
 * (the tests compare both) */
static void (*blocks_fn)(uint32_t h[8], const uint8_t *p, size_t n);

static void pick(void)
{
	if (blocks_fn)
		return;
	blocks_fn = blocks_c;
#ifdef HAVE_SHANI
	const char *force = getenv("BURN_SHA256");
	if (!(force && !strcmp(force, "c")) && cpu_shani())
		blocks_fn = blocks_shani;
#endif
}

static void blocks(uint32_t h[8], const uint8_t *p, size_t n)
{
	pick();
	blocks_fn(h, p, n);
}

const char *sha256_impl(void)
{
	pick();
#ifdef HAVE_SHANI
	if (blocks_fn == blocks_shani)
		return "sha-ni";
#endif
	return "c";
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
		blocks(c->h, c->buf, 1);
		c->n = 0;
	}
	if (len >= 64) {
		blocks(c->h, p, len / 64);
		p += len & ~(size_t)63;
		len &= 63;
	}
	memcpy(c->buf, p, len);
	c->n = len;
}

void sha256_final(sha256_ctx *c, uint8_t out[32])
{
	uint64_t bits = c->len * 8;
	c->buf[c->n++] = 0x80;
	if (c->n > 56) {
		memset(c->buf + c->n, 0, 64 - c->n);
		blocks(c->h, c->buf, 1);
		c->n = 0;
	}
	memset(c->buf + c->n, 0, 56 - c->n);
	for (int i = 0; i < 8; i++)
		c->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
	blocks(c->h, c->buf, 1);
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
