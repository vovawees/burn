/* SPDX-License-Identifier: MIT */
#include "vercmp.h"

#include <stdbool.h>
#include <string.h>
#include <sys/types.h>

static bool c_digit(int c)
{
	return c >= '0' && c <= '9';
}

static bool c_alpha(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* length of s without its file suffix: (\.[A-Za-z~][A-Za-z0-9~]*)*$ */
static size_t prefix_len(const char *s, size_t n)
{
	size_t prefix = 0;
	for (size_t i = 0;;) {
		if (i == n)
			return prefix;
		i++;
		prefix = i;
		while (i + 1 < n && s[i] == '.' && (c_alpha(s[i + 1]) || s[i + 1] == '~'))
			for (i += 2; i < n && (c_alpha(s[i]) || c_digit(s[i]) || s[i] == '~'); i++)
				continue;
	}
}

static int order(const char *s, size_t pos, size_t len)
{
	if (pos == len)
		return -1;
	unsigned char c = (unsigned char)s[pos];
	if (c_digit(c))
		return 0;
	if (c_alpha(c))
		return c;
	if (c == '~')
		return -2;
	return c + 256;
}

static int verrevcmp(const char *a, size_t alen, const char *b, size_t blen)
{
	size_t i = 0, j = 0;
	while (i < alen || j < blen) {
		int first_diff = 0;
		while ((i < alen && !c_digit(a[i])) || (j < blen && !c_digit(b[j]))) {
			int ac = order(a, i, alen), bc = order(b, j, blen);
			if (ac != bc)
				return ac - bc;
			i++;
			j++;
		}
		while (i < alen && a[i] == '0')
			i++;
		while (j < blen && b[j] == '0')
			j++;
		while (i < alen && j < blen && c_digit(a[i]) && c_digit(b[j])) {
			if (!first_diff)
				first_diff = a[i] - b[j];
			i++;
			j++;
		}
		if (i < alen && c_digit(a[i]))
			return 1;
		if (j < blen && c_digit(b[j]))
			return -1;
		if (first_diff)
			return first_diff;
	}
	return 0;
}

int filevercmp(const char *a, const char *b)
{
	size_t alen = strlen(a), blen = strlen(b);
	if (!alen)
		return blen ? -1 : 0;
	if (!blen)
		return 1;
	/* "." first, then "..", then other names with a leading dot, then the rest */
	if (a[0] == '.') {
		if (b[0] != '.')
			return -1;
		bool adot = alen == 1, bdot = blen == 1;
		if (adot)
			return bdot ? 0 : -1;
		if (bdot)
			return 1;
		bool adotdot = a[1] == '.' && alen == 2, bdotdot = b[1] == '.' && blen == 2;
		if (adotdot)
			return bdotdot ? 0 : -1;
		if (bdotdot)
			return 1;
	} else if (b[0] == '.') {
		return 1;
	}
	size_t ap = prefix_len(a, alen), bp = prefix_len(b, blen);
	int r = verrevcmp(a, ap, b, bp);
	if (r || (ap == alen && bp == blen))
		return r;
	return verrevcmp(a, alen, b, blen);
}

int vercmp(const char *a, const char *b)
{
	int r = strcmp(a, b);
	if (!r)
		return 0;
	int v = filevercmp(a, b);
	if (v)
		r = v;
	return r < 0 ? -1 : 1;
}
