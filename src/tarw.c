/* SPDX-License-Identifier: MIT */
/* Writing packages: a tar stream (GNU format, like `tar --sort=name --numeric-owner --owner=0
 * --group=0 --mtime=@EPOCH --clamp-mtime -cf - .`) compressed with libzstd as it is produced. */
#include "tar.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <zstd.h>

#include "util.h"

typedef struct {
	int fd;
	ZSTD_CCtx *z;
	char *zbuf;
	size_t zcap;
	char blk[512];
	strmap inodes; /* "dev:ino" of files with more than one link -> index into first */
	svec first;    /* the archived name of their first occurrence */
	int64_t epoch;
	uint64_t total; /* bytes of tar so far */
	char *err;
} tw;

static int tw_fail(tw *t, const char *fmt, ...) PRINTF(2, 3);
static int tw_fail(tw *t, const char *fmt, ...)
{
	if (!t->err) {
		va_list ap;
		va_start(ap, fmt);
		t->err = xvasprintf(fmt, ap);
		va_end(ap);
	}
	return -1;
}

static int z_push(tw *t, const void *data, size_t len, ZSTD_EndDirective end)
{
	ZSTD_inBuffer in = {data, len, 0};
	t->total += len;
	for (;;) {
		ZSTD_outBuffer out = {t->zbuf, t->zcap, 0};
		size_t r = ZSTD_compressStream2(t->z, &out, &in, end);
		if (ZSTD_isError(r))
			return tw_fail(t, "zstd: %s", ZSTD_getErrorName(r));
		if (out.pos && write_all(t->fd, t->zbuf, out.pos) < 0)
			return tw_fail(t, "write: %s", strerror(errno));
		if (end == ZSTD_e_end ? r == 0 : in.pos == in.size)
			return 0;
	}
}

static void octal(char *f, size_t w, uint64_t v)
{
	/* w - 1 octal digits and a NUL; too big: GNU base-256 */
	if (w - 1 >= 22 || v < (1ULL << (3 * (w - 1)))) {
		snprintf(f, w, "%0*llo", (int)(w - 1), (unsigned long long)v);
		return;
	}
	memset(f, 0, w);
	f[0] = (char)0x80;
	for (size_t i = w - 1; i > 0 && v; i--, v >>= 8)
		f[i] = (char)(v & 0xff);
}

static int header(tw *t, const char *name, const char *link, int type, mode_t mode, uint64_t size,
		  int64_t mtime, unsigned major, unsigned minor)
{
	char *h = t->blk;
	memset(h, 0, 512);
	memcpy(h, name, strnlen(name, 100));
	octal(h + 100, 8, mode & 07777);
	octal(h + 108, 8, 0);
	octal(h + 116, 8, 0);
	octal(h + 124, 12, size);
	octal(h + 136, 12, mtime > 0 ? (uint64_t)mtime : 0);
	h[156] = (char)type;
	if (link)
		memcpy(h + 157, link, strnlen(link, 100));
	memcpy(h + 257, "ustar  ", 8); /* GNU magic and version, with the NUL */
	if (type == '3' || type == '4') {
		octal(h + 329, 8, major);
		octal(h + 337, 8, minor);
	}
	memset(h + 148, ' ', 8);
	unsigned sum = 0;
	for (int i = 0; i < 512; i++)
		sum += (unsigned char)h[i];
	snprintf(h + 148, 8, "%06o", sum);
	h[155] = ' ';
	return z_push(t, h, 512, ZSTD_e_continue);
}

static int padded(tw *t, const char *data, size_t len)
{
	static const char zero[512];
	if (z_push(t, data, len, ZSTD_e_continue) < 0)
		return -1;
	return len % 512 ? z_push(t, zero, 512 - len % 512, ZSTD_e_continue) : 0;
}

/* a GNU ././@LongLink entry ('L' name, 'K' link target) for strings that do not fit, with the
 * header GNU tar gives it (mode 0644, time 0) */
static int long_name(tw *t, int type, const char *s)
{
	size_t n = strlen(s) + 1;
	if (header(t, "././@LongLink", NULL, type, 0644, n, 0, 0, 0) < 0)
		return -1;
	return padded(t, s, n);
}

static int entry(tw *t, const char *name, const char *link, int type, const struct stat *st, uint64_t size)
{
	if (strlen(name) > 100 && long_name(t, 'L', name) < 0)
		return -1;
	if (link && strlen(link) > 100 && long_name(t, 'K', link) < 0)
		return -1;
	int64_t mtime = st->st_mtime < t->epoch ? st->st_mtime : t->epoch;
	return header(t, name, link, type, st->st_mode, size, mtime, major(st->st_rdev), minor(st->st_rdev));
}

static int file_data(tw *t, int dfd, const char *base, const char *name, const struct stat *st)
{
	int fd = openat(dfd, base, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return tw_fail(t, "%s: %s", name, strerror(errno));
	char buf[1 << 16];
	uint64_t left = (uint64_t)st->st_size;
	while (left) {
		ssize_t n = read(fd, buf, left < sizeof buf ? (size_t)left : sizeof buf);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			close(fd);
			return tw_fail(t, "%s: %s", name, n < 0 ? strerror(errno) : "file shrank while it was read");
		}
		if (z_push(t, buf, (size_t)n, ZSTD_e_continue) < 0) {
			close(fd);
			return -1;
		}
		left -= (uint64_t)n;
	}
	close(fd);
	static const char zero[512];
	size_t r = (size_t)(st->st_size % 512);
	return r ? z_push(t, zero, 512 - r, ZSTD_e_continue) : 0;
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* the entries of the directory DFD (archived as PREFIX, e.g. "./usr/"), sorted, recursively */
static int walk(tw *t, int dfd, const char *prefix)
{
	int fd = dup(dfd);
	DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
	if (!d) {
		if (fd >= 0)
			close(fd);
		return tw_fail(t, "%s: %s", prefix, strerror(errno));
	}
	svec names = SVEC_INIT;
	struct dirent *de;
	while ((de = readdir(d)))
		if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
			sv_push(&names, de->d_name);
	closedir(d);
	if (names.n)
		qsort(names.v, names.n, sizeof *names.v, cmp_str);
	int rc = 0;
	for (size_t i = 0; rc == 0 && i < names.n; i++) {
		const char *base = names.v[i];
		struct stat st;
		if (fstatat(dfd, base, &st, AT_SYMLINK_NOFOLLOW) < 0) {
			rc = tw_fail(t, "%s%s: %s", prefix, base, strerror(errno));
			break;
		}
		char *name = xasprintf("%s%s%s", prefix, base, S_ISDIR(st.st_mode) ? "/" : "");
		if (S_ISDIR(st.st_mode)) {
			rc = entry(t, name, NULL, '5', &st, 0);
			int sub = rc == 0 ? openat(dfd, base, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW) : -1;
			if (rc == 0 && sub < 0)
				rc = tw_fail(t, "%s: %s", name, strerror(errno));
			if (sub >= 0) {
				rc = walk(t, sub, name);
				close(sub);
			}
		} else if (S_ISLNK(st.st_mode)) {
			char target[4097];
			ssize_t n = readlinkat(dfd, base, target, sizeof target - 1);
			if (n < 0) {
				rc = tw_fail(t, "%s: %s", name, strerror(errno));
			} else {
				target[n] = 0;
				rc = entry(t, name, target, '2', &st, 0);
			}
		} else if (S_ISREG(st.st_mode)) {
			size_t idx;
			char key[64];
			snprintf(key, sizeof key, "%llx:%llx", (unsigned long long)st.st_dev, (unsigned long long)st.st_ino);
			if (st.st_nlink > 1 && sm_get(&t->inodes, key, &idx)) {
				rc = entry(t, name, t->first.v[idx], '1', &st, 0);
			} else {
				if (st.st_nlink > 1) {
					sm_put(&t->inodes, key, t->first.n);
					sv_push(&t->first, name);
				}
				rc = entry(t, name, NULL, '0', &st, (uint64_t)st.st_size);
				if (rc == 0)
					rc = file_data(t, dfd, base, name, &st);
			}
		} else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) || S_ISFIFO(st.st_mode)) {
			rc = entry(t, name, NULL, S_ISCHR(st.st_mode) ? '3' : S_ISBLK(st.st_mode) ? '4' : '6', &st, 0);
		} /* sockets are not archived, like GNU tar */
		free(name);
	}
	sv_free(&names);
	return rc;
}

int tar_write_dir(const char *dir, const char *out, int64_t epoch, int level, int threads, char **err)
{
	tw t = {.fd = -1, .epoch = epoch};
	char *part = xasprintf("%s.part", out);
	int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	struct stat st;
	int rc = -1;
	if (dfd < 0 || fstat(dfd, &st) < 0) {
		tw_fail(&t, "%s: %s", dir, strerror(errno));
		goto out;
	}
	t.fd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (t.fd < 0) {
		tw_fail(&t, "%s: %s", part, strerror(errno));
		goto out;
	}
	t.z = ZSTD_createCCtx();
	t.zcap = ZSTD_CStreamOutSize();
	t.zbuf = xmalloc(t.zcap);
	ZSTD_CCtx_setParameter(t.z, ZSTD_c_compressionLevel, level);
	ZSTD_CCtx_setParameter(t.z, ZSTD_c_checksumFlag, 1);
	ZSTD_bounds b = ZSTD_cParam_getBounds(ZSTD_c_nbWorkers);
	if (!ZSTD_isError(b.error) && b.upperBound > 0) {
		int n = threads > 0 ? threads : (int)nproc();
		ZSTD_CCtx_setParameter(t.z, ZSTD_c_nbWorkers, n < b.upperBound ? n : b.upperBound);
	}
	/* two empty blocks end the archive, padded to whole records of 10240 bytes like GNU tar */
	static const char zero[10240];
	if (entry(&t, "./", NULL, '5', &st, 0) < 0 || walk(&t, dfd, "./") < 0 || z_push(&t, zero, 1024, ZSTD_e_continue) < 0)
		goto out;
	if (t.total % 10240 && z_push(&t, zero, 10240 - t.total % 10240, ZSTD_e_continue) < 0)
		goto out;
	if (z_push(&t, NULL, 0, ZSTD_e_end) < 0)
		goto out;
	if (fsync(t.fd) < 0 || close(t.fd) < 0) {
		t.fd = -1;
		tw_fail(&t, "%s: %s", part, strerror(errno));
		goto out;
	}
	t.fd = -1;
	if (rename(part, out) < 0) {
		tw_fail(&t, "%s: %s", out, strerror(errno));
		goto out;
	}
	rc = 0;
out:
	if (t.fd >= 0)
		close(t.fd);
	if (rc < 0)
		unlink(part);
	if (dfd >= 0)
		close(dfd);
	ZSTD_freeCCtx(t.z);
	free(t.zbuf);
	sm_free(&t.inodes);
	sv_free(&t.first);
	free(part);
	if (rc < 0)
		*err = t.err ? t.err : xstrdup("cannot write the package");
	else
		free(t.err);
	return rc;
}
