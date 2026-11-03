/* SPDX-License-Identifier: MIT */
/* .tar.zst reading: libzstd streaming decompression and a tar parser (ustar, GNU, pax). */
#include "tar.h"

#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zstd.h>

/* ---- zstd ---- */

struct zst_reader {
	int fd;
	char *path;
	ZSTD_DStream *ds;
	unsigned char *in, *out;
	size_t in_cap, out_cap;
	ZSTD_inBuffer ib;
	size_t out_pos, out_len;
	size_t frame; /* the last ZSTD_decompressStream() result: 0 right after a complete frame */
	bool eof;
};

static bool zst_magic(const unsigned char *b)
{
	uint32_t m = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
	return m == 0xFD2FB528u || (m & 0xFFFFFFF0u) == 0x184D2A50u; /* a frame or a skippable frame */
}

zst_reader *zst_open(const char *path, char **err)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		*err = xasprintf("cannot open %s: %s", path, strerror(errno));
		return NULL;
	}
	zst_reader *z = xcalloc(1, sizeof *z);
	z->fd = fd;
	z->path = xstrdup(path);
	z->in_cap = ZSTD_DStreamInSize();
	z->out_cap = ZSTD_DStreamOutSize();
	z->in = xmalloc(z->in_cap);
	z->out = xmalloc(z->out_cap);
	z->ib.src = z->in;
	ssize_t n;
	do
		n = read(fd, z->in, z->in_cap);
	while (n < 0 && errno == EINTR);
	if (n < 4 || !zst_magic(z->in)) {
		*err = n < 0 ? xasprintf("cannot read %s: %s", path, strerror(errno))
			     : xasprintf("%s is not zstd compressed", path);
		zst_close(z);
		return NULL;
	}
	z->ib.size = (size_t)n;
	z->ds = ZSTD_createDStream();
	if (!z->ds)
		die("out of memory");
	ZSTD_initDStream(z->ds);
	z->frame = 1;
	return z;
}

ssize_t zst_read(zst_reader *z, void *buf, size_t n, char **err)
{
	size_t got = 0;
	unsigned char *dst = buf;
	while (got < n) {
		if (z->out_pos < z->out_len) {
			size_t k = z->out_len - z->out_pos;
			if (k > n - got)
				k = n - got;
			memcpy(dst + got, z->out + z->out_pos, k);
			z->out_pos += k;
			got += k;
			continue;
		}
		/* a full output buffer may leave more data inside the decoder: drain it before reading */
		bool drained = z->out_len < z->out_cap;
		if (z->ib.pos == z->ib.size && drained) {
			if (z->eof)
				break;
			ssize_t r;
			do
				r = read(z->fd, z->in, z->in_cap);
			while (r < 0 && errno == EINTR);
			if (r < 0) {
				*err = xasprintf("cannot read %s: %s", z->path, strerror(errno));
				return -1;
			}
			if (r == 0) {
				z->eof = true;
				if (z->frame) {
					*err = xasprintf("%s: truncated zstd stream", z->path);
					return -1;
				}
				break;
			}
			z->ib.size = (size_t)r;
			z->ib.pos = 0;
		}
		ZSTD_outBuffer ob = {z->out, z->out_cap, 0};
		size_t ret = ZSTD_decompressStream(z->ds, &ob, &z->ib);
		if (ZSTD_isError(ret)) {
			*err = xasprintf("%s: %s", z->path, ZSTD_getErrorName(ret));
			return -1;
		}
		z->frame = ret;
		z->out_len = ob.pos;
		z->out_pos = 0;
	}
	return (ssize_t)got;
}

void zst_close(zst_reader *z)
{
	if (!z)
		return;
	if (z->ds)
		ZSTD_freeDStream(z->ds);
	close(z->fd);
	free(z->in);
	free(z->out);
	free(z->path);
	free(z);
}

/* ---- tar ---- */

typedef struct {
	char *path, *link;
	bool has_size, has_mtime, has_uid, has_gid;
	uint64_t size, uid, gid;
	int64_t mtime;
} pax_attrs;

struct tar_reader {
	zst_reader *z;
	char *file;
	tar_entry e;
	uint64_t left, pad; /* data of the current entry not read yet, and the padding after it */
	char *longname, *longlink;
	pax_attrs pax, global;
	bool done;
};

tar_reader *tar_open(const char *path, char **err)
{
	zst_reader *z = zst_open(path, err);
	if (!z)
		return NULL;
	tar_reader *t = xcalloc(1, sizeof *t);
	t->z = z;
	t->file = xstrdup(path);
	return t;
}

static void pax_clear(pax_attrs *p)
{
	free(p->path);
	free(p->link);
	memset(p, 0, sizeof *p);
}

static void entry_clear(tar_entry *e)
{
	free(e->path);
	free(e->link);
	memset(e, 0, sizeof *e);
}

void tar_close(tar_reader *t)
{
	if (!t)
		return;
	zst_close(t->z);
	entry_clear(&t->e);
	pax_clear(&t->pax);
	pax_clear(&t->global);
	free(t->longname);
	free(t->longlink);
	free(t->file);
	free(t);
}

static int read_exact(tar_reader *t, void *buf, size_t n, char **err)
{
	ssize_t r = zst_read(t->z, buf, n, err);
	if (r < 0)
		return -1;
	if ((size_t)r < n) {
		*err = xasprintf("%s: truncated archive", t->file);
		return -1;
	}
	return 0;
}

static int skip(tar_reader *t, uint64_t n, char **err)
{
	char buf[16384];
	while (n) {
		size_t k = n < sizeof buf ? (size_t)n : sizeof buf;
		if (read_exact(t, buf, k, err) < 0)
			return -1;
		n -= k;
	}
	return 0;
}

/* octal (with leading spaces, NUL or space terminated) or GNU base-256 */
static bool parse_num(const unsigned char *f, size_t n, uint64_t *out)
{
	uint64_t v = 0;
	if (f[0] & 0x80) {
		if (f[0] == 0xff) { /* negative: only a pre-1970 mtime, taken as 0 */
			*out = 0;
			return true;
		}
		v = f[0] & 0x7f;
		for (size_t i = 1; i < n; i++) {
			if (v >> 55)
				return false;
			v = v << 8 | f[i];
		}
		*out = v;
		return true;
	}
	size_t i = 0;
	while (i < n && f[i] == ' ')
		i++;
	for (; i < n && f[i] >= '0' && f[i] <= '7'; i++) {
		if (v >> 60)
			return false;
		v = v * 8 + (uint64_t)(f[i] - '0');
	}
	for (; i < n; i++)
		if (f[i] && f[i] != ' ')
			return false;
	*out = v;
	return true;
}

static char *field_str(const unsigned char *f, size_t n)
{
	size_t k = 0;
	while (k < n && f[k])
		k++;
	return xstrndup((const char *)f, k);
}

char *tar_normalize(const char *p)
{
	sbuf b = SBUF_INIT;
	sb_str(&b);
	const char *s = p;
	while (*s) {
		while (*s == '/')
			s++;
		const char *e = strchr(s, '/');
		size_t n = e ? (size_t)(e - s) : strlen(s);
		if (n == 2 && s[0] == '.' && s[1] == '.') {
			sb_free(&b);
			return NULL;
		}
		if (n && !(n == 1 && s[0] == '.')) {
			if (b.len)
				sb_addc(&b, '/');
			sb_addn(&b, s, n);
		}
		s += n;
	}
	return sb_steal(&b);
}

/* "LEN key=value\n" records */
static int pax_parse(tar_reader *t, char *d, size_t len, pax_attrs *p, char **err)
{
	size_t i = 0;
	while (i < len) {
		size_t rl = 0, j = i;
		while (j < len && d[j] >= '0' && d[j] <= '9')
			rl = rl * 10 + (size_t)(d[j++] - '0');
		if (j >= len || d[j] != ' ' || rl <= j - i || i + rl > len || d[i + rl - 1] != '\n') {
			*err = xasprintf("%s: bad pax header", t->file);
			return -1;
		}
		char *kv = d + j + 1, *end = d + i + rl - 1;
		char *eq = memchr(kv, '=', (size_t)(end - kv));
		if (!eq) {
			*err = xasprintf("%s: bad pax header", t->file);
			return -1;
		}
		size_t kl = (size_t)(eq - kv);
		char *val = xstrndup(eq + 1, (size_t)(end - eq - 1));
		if (kl == 4 && !memcmp(kv, "path", 4)) {
			free(p->path);
			p->path = val;
			val = NULL;
		} else if (kl == 8 && !memcmp(kv, "linkpath", 8)) {
			free(p->link);
			p->link = val;
			val = NULL;
		} else if (kl == 4 && !memcmp(kv, "size", 4)) {
			p->has_size = true;
			p->size = strtoull(val, NULL, 10);
		} else if (kl == 5 && !memcmp(kv, "mtime", 5)) {
			p->has_mtime = true;
			p->mtime = strtoll(val, NULL, 10);
		} else if (kl == 3 && !memcmp(kv, "uid", 3)) {
			p->has_uid = true;
			p->uid = strtoull(val, NULL, 10);
		} else if (kl == 3 && !memcmp(kv, "gid", 3)) {
			p->has_gid = true;
			p->gid = strtoull(val, NULL, 10);
		} else if (kl > 11 && !memcmp(kv, "GNU.sparse.", 11)) {
			free(val);
			*err = xasprintf("%s: sparse files are not supported", t->file);
			return -1;
		}
		free(val);
		i += rl;
	}
	return 0;
}

static char *read_data(tar_reader *t, uint64_t size, char **err)
{
	if (size > (64u << 20)) {
		*err = xasprintf("%s: header data too large", t->file);
		return NULL;
	}
	char *d = xmalloc((size_t)size + 1);
	if (read_exact(t, d, (size_t)size, err) < 0 || skip(t, (512 - size % 512) % 512, err) < 0) {
		free(d);
		return NULL;
	}
	d[size] = 0;
	return d;
}

int tar_next(tar_reader *t, const tar_entry **out, char **err)
{
	if (t->done)
		return 0;
	if (skip(t, t->left + t->pad, err) < 0)
		return -1;
	t->left = t->pad = 0;
	entry_clear(&t->e);
	for (;;) {
		unsigned char h[512];
		ssize_t r = zst_read(t->z, h, sizeof h, err);
		if (r < 0)
			return -1;
		if (r == 0) { /* no end-of-archive blocks: accepted, like GNU tar */
			t->done = true;
			return 0;
		}
		if (r < 512) {
			*err = xasprintf("%s: truncated archive", t->file);
			return -1;
		}
		bool zero = true;
		for (size_t i = 0; i < 512 && zero; i++)
			zero = !h[i];
		if (zero) {
			t->done = true;
			return 0;
		}
		unsigned sum = 0;
		int ssum = 0;
		for (size_t i = 0; i < 512; i++) {
			unsigned char c = i >= 148 && i < 156 ? ' ' : h[i];
			sum += c;
			ssum += (signed char)c;
		}
		uint64_t chk, mode, uid, gid, size, mtime, major, minor;
		if (!parse_num(h + 148, 8, &chk) || (chk != sum && (int64_t)chk != ssum)) {
			*err = xasprintf("%s: bad tar header checksum", t->file);
			return -1;
		}
		if (!parse_num(h + 100, 8, &mode) || !parse_num(h + 108, 8, &uid) || !parse_num(h + 116, 8, &gid) ||
		    !parse_num(h + 124, 12, &size) || !parse_num(h + 136, 12, &mtime) ||
		    !parse_num(h + 329, 8, &major) || !parse_num(h + 337, 8, &minor)) {
			*err = xasprintf("%s: bad number in a tar header", t->file);
			return -1;
		}
		int type = h[156] ? h[156] : TAR_FILE;
		if (type == 'L' || type == 'K' || type == 'x' || type == 'g') {
			char *d = read_data(t, size, err);
			if (!d)
				return -1;
			int rc = 0;
			if (type == 'L') {
				free(t->longname);
				t->longname = d;
			} else if (type == 'K') {
				free(t->longlink);
				t->longlink = d;
			} else {
				rc = pax_parse(t, d, (size_t)size, type == 'x' ? &t->pax : &t->global, err);
				free(d);
			}
			if (rc < 0)
				return -1;
			continue;
		}
		if (type == 'V') { /* volume label */
			if (skip(t, size + (512 - size % 512) % 512, err) < 0)
				return -1;
			continue;
		}
		if (type == '7')
			type = TAR_FILE;
		if (type == 'D') /* GNU dumpdir: a directory with a listing as data */
			type = TAR_DIR;
		if (!strchr("0123456", type)) {
			*err = xasprintf("%s: unsupported tar entry type '%c'", t->file, type);
			return -1;
		}
		/* the name: GNU long name, pax path, ustar prefix + name, name */
		char *name;
		if (t->longname)
			name = xstrdup(t->longname);
		else if (t->pax.path)
			name = xstrdup(t->pax.path);
		else if (t->global.path)
			name = xstrdup(t->global.path);
		else if (!memcmp(h + 257, "ustar\0", 6) && h[345]) {
			char *pre = field_str(h + 345, 155), *n = field_str(h, 100);
			name = xasprintf("%s/%s", pre, n);
			free(pre);
			free(n);
		} else {
			name = field_str(h, 100);
		}
		char *link = t->longlink ? xstrdup(t->longlink)
			     : t->pax.link ? xstrdup(t->pax.link)
					   : field_str(h + 157, 100);
		pax_attrs *px = &t->pax, *gl = &t->global;
		if (px->has_size || gl->has_size)
			size = px->has_size ? px->size : gl->size;
		if (px->has_mtime || gl->has_mtime)
			mtime = (uint64_t)(px->has_mtime ? px->mtime : gl->mtime);
		if (px->has_uid || gl->has_uid)
			uid = px->has_uid ? px->uid : gl->uid;
		if (px->has_gid || gl->has_gid)
			gid = px->has_gid ? px->gid : gl->gid;
		free(t->longname);
		free(t->longlink);
		t->longname = t->longlink = NULL;
		pax_clear(&t->pax);

		tar_entry *e = &t->e;
		e->path = tar_normalize(name);
		if (!e->path) {
			*err = xasprintf("%s: unsafe path in the archive: %s", t->file, name);
			free(name);
			free(link);
			return -1;
		}
		free(name);
		e->type = type;
		if (type == TAR_HARDLINK) {
			e->link = tar_normalize(link);
			if (!e->link || !*e->link) {
				*err = xasprintf("%s: unsafe hard link in the archive: %s", t->file, link);
				free(link);
				return -1;
			}
			free(link);
		} else {
			e->link = link;
		}
		e->mode = (mode_t)(mode & 07777);
		e->uid = (uid_t)uid;
		e->gid = (gid_t)gid;
		e->mtime = (int64_t)mtime;
		e->major = (unsigned)major;
		e->minor = (unsigned)minor;
		/* only regular files carry data; anything else stored with a size is skipped */
		e->size = type == TAR_FILE ? size : 0;
		t->left = size;
		t->pad = (512 - size % 512) % 512;
		*out = e;
		return 1;
	}
}

ssize_t tar_read(tar_reader *t, void *buf, size_t n, char **err)
{
	if (t->e.type != TAR_FILE || !t->left)
		return 0;
	if (n > t->left)
		n = (size_t)t->left;
	if (read_exact(t, buf, n, err) < 0)
		return -1;
	t->left -= n;
	return (ssize_t)n;
}
