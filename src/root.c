/* SPDX-License-Identifier: MIT */
/* Root-confined file operations: openat2(RESOLVE_IN_ROOT) or a userspace walk with the same rules. */
#include "root.h"

#include "burn.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int rootfd = -1;
static bool use_openat2;

static int sys_openat2(int dfd, const char *path, int flags, mode_t mode)
{
	struct open_how how;
	memset(&how, 0, sizeof how);
	how.flags = (uint64_t)(flags | O_CLOEXEC);
	how.mode = flags & (O_CREAT | O_TMPFILE) ? (uint64_t)mode : 0;
	how.resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS;
	int fd;
	do
		fd = (int)syscall(SYS_openat2, dfd, path, &how, sizeof how);
	while (fd < 0 && errno == EAGAIN);
	return fd;
}

void root_init(void)
{
	if (rootfd >= 0)
		return;
	if (mkdir_p(B.root, 0755) < 0)
		die_errno("cannot create %s", B.root);
	rootfd = open(B.root, O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (rootfd < 0)
		die_errno("cannot open %s", B.root);
	int fd = sys_openat2(rootfd, ".", O_PATH | O_DIRECTORY, 0);
	use_openat2 = fd >= 0;
	if (fd >= 0)
		close(fd);
}

/* The userspace walk: the resolved prefix never holds a symlink; ".." stops at the root; a
 * symlink restarts the walk with its target (an absolute one from the root). */
static int walk(const char *path, int flags, mode_t mode)
{
	sbuf res = SBUF_INIT, todo = SBUF_INIT;
	sb_adds(&todo, path);
	int links = 0, fd = -1;
	size_t pos = 0;
	sb_str(&res);
	for (;;) {
		const char *s = sb_str(&todo) + pos;
		while (*s == '/')
			s++;
		if (!*s)
			break;
		const char *e = strchr(s, '/');
		size_t n = e ? (size_t)(e - s) : strlen(s);
		pos = (size_t)(s - todo.s) + n;
		if (n == 1 && s[0] == '.')
			continue;
		if (n == 2 && s[0] == '.' && s[1] == '.') {
			char *slash = strrchr(res.s, '/');
			res.len = slash ? (size_t)(slash - res.s) : 0;
			res.s[res.len] = 0;
			continue;
		}
		size_t keep = res.len;
		if (res.len)
			sb_addc(&res, '/');
		sb_addn(&res, s, n);
		bool last = !todo.s[pos] || !todo.s[pos + strspn(todo.s + pos, "/")];
		if (last && (flags & O_NOFOLLOW))
			break;
		struct stat st;
		if (fstatat(rootfd, res.s, &st, AT_SYMLINK_NOFOLLOW) < 0 || !S_ISLNK(st.st_mode))
			continue;
		if (++links > 40) {
			errno = ELOOP;
			goto out;
		}
		char target[4096];
		ssize_t tl = readlinkat(rootfd, res.s, target, sizeof target - 1);
		if (tl < 0)
			goto out;
		target[tl] = 0;
		sbuf next = SBUF_INIT;
		sb_adds(&next, target);
		sb_addc(&next, '/');
		sb_adds(&next, todo.s + pos);
		sb_free(&todo);
		todo = next;
		pos = 0;
		if (target[0] == '/')
			keep = 0;
		res.len = keep;
		res.s[res.len] = 0;
	}
	/* every symlink is resolved by now: one at the end means the root changed under us */
	fd = openat(rootfd, res.len ? res.s : ".", flags | O_CLOEXEC | (res.len ? O_NOFOLLOW : 0), mode);
out:
	sb_free(&res);
	sb_free(&todo);
	return fd;
}

static int resolve(const char *path, int flags, mode_t mode)
{
	if (rootfd < 0)
		root_init();
	const char *p = *path ? path : ".";
	if (use_openat2)
		return sys_openat2(rootfd, p, flags, mode);
	return walk(p, flags, mode);
}

/* the directories used last; LRU, so the two of one operation (rename) never evict each other */
#define NCACHE 64
static struct {
	char *dir;
	int fd;
	unsigned long used;
} cache[NCACHE];
static unsigned long cache_clock;

void root_flush(void)
{
	for (size_t i = 0; i < NCACHE; i++) {
		if (cache[i].dir) {
			close(cache[i].fd);
			free(cache[i].dir);
			cache[i].dir = NULL;
		}
	}
}

int root_dir(const char *dir)
{
	size_t old = 0;
	for (size_t i = 0; i < NCACHE; i++) {
		if (cache[i].dir && !strcmp(cache[i].dir, dir)) {
			cache[i].used = ++cache_clock;
			return cache[i].fd;
		}
		if (!cache[i].dir || (cache[old].dir && cache[i].used < cache[old].used))
			old = i;
	}
	int fd = resolve(dir, O_PATH | O_DIRECTORY, 0);
	if (fd < 0)
		return -1;
	if (cache[old].dir) {
		close(cache[old].fd);
		free(cache[old].dir);
	}
	cache[old].dir = xstrdup(dir);
	cache[old].fd = fd;
	cache[old].used = ++cache_clock;
	return fd;
}

int root_parent(const char *path, const char **name)
{
	const char *slash = strrchr(path, '/');
	if (!slash) {
		*name = path;
		return root_dir("");
	}
	*name = slash + 1;
	char *dir = xstrndup(path, (size_t)(slash - path));
	int fd = root_dir(dir);
	free(dir);
	return fd;
}

int root_mkdirs(const char *dir, mode_t mode)
{
	int fd = root_dir(dir);
	if (fd >= 0 || errno != ENOENT || !*dir)
		return fd;
	const char *name;
	char *parent = xstrndup(dir, strrchr(dir, '/') ? (size_t)(strrchr(dir, '/') - dir) : 0);
	int pfd = root_mkdirs(parent, mode);
	free(parent);
	if (pfd < 0)
		return -1;
	root_parent(dir, &name);
	if (mkdirat(pfd, name, mode) < 0 && errno != EEXIST)
		return -1;
	return root_dir(dir);
}

int root_open(const char *path, int flags, mode_t mode)
{
	return resolve(path, flags, mode);
}

bool root_lstat(const char *path, struct stat *st)
{
	const char *name;
	int dfd = root_parent(path, &name);
	return dfd >= 0 && fstatat(dfd, *name ? name : ".", st, AT_SYMLINK_NOFOLLOW) == 0;
}

bool root_stat(const char *path, struct stat *st)
{
	int fd = resolve(path, O_PATH, 0);
	if (fd < 0)
		return false;
	bool ok = fstat(fd, st) == 0;
	close(fd);
	return ok;
}

char *root_readlink(const char *path)
{
	const char *name;
	int dfd = root_parent(path, &name);
	if (dfd < 0)
		return NULL;
	char buf[4096];
	ssize_t n = readlinkat(dfd, name, buf, sizeof buf - 1);
	if (n < 0)
		return NULL;
	return xstrndup(buf, (size_t)n);
}

int root_unlink(const char *path)
{
	const char *name;
	int dfd = root_parent(path, &name);
	return dfd < 0 ? -1 : unlinkat(dfd, name, 0);
}

int root_rmdir(const char *path)
{
	const char *name;
	int dfd = root_parent(path, &name);
	if (dfd < 0)
		return -1;
	int rc = unlinkat(dfd, name, AT_REMOVEDIR);
	if (rc == 0)
		root_flush();
	return rc;
}

int root_rename(const char *from, const char *to)
{
	const char *fn, *tn;
	int ffd = root_parent(from, &fn);
	if (ffd < 0)
		return -1;
	int tfd = root_parent(to, &tn);
	if (tfd < 0)
		return -1;
	return renameat(ffd, fn, tfd, tn);
}

static size_t read_full(int fd, char *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, buf + got, n - got);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		got += (size_t)r;
	}
	return got;
}

bool root_same_content(const char *a_path, int fd_b)
{
	int a = root_open(a_path, O_RDONLY, 0);
	if (a < 0)
		return false;
	bool same = lseek(fd_b, 0, SEEK_SET) == 0;
	static char ba[65536], bb[65536];
	while (same) {
		size_t na = read_full(a, ba, sizeof ba), nb = read_full(fd_b, bb, sizeof bb);
		same = na == nb && !memcmp(ba, bb, na);
		if (!na)
			break;
	}
	close(a);
	return same;
}
