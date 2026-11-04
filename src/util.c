/* SPDX-License-Identifier: MIT */
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <ftw.h>
#include <limits.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool opt_yes;

static const char *c_b = "", *c_g = "", *c_y = "", *c_r = "", *c_c = "", *c_0 = "";

void log_init(void)
{
	const char *nc = getenv("NO_COLOR");
	if (isatty(2) && !(nc && *nc)) {
		c_b = "\033[1m";
		c_g = "\033[32m";
		c_y = "\033[33m";
		c_r = "\033[31m";
		c_c = "\033[36m";
		c_0 = "\033[0m";
	}
}

/* one write per line, so parallel jobs never interleave inside a message */
static void vout(const char *pre, const char *post, const char *fmt, va_list ap)
{
	char *m = xvasprintf(fmt, ap);
	char *line = xasprintf("%s%s%s\n", pre, m, post);
	fflush(stdout);
	write_all(2, line, strlen(line));
	free(line);
	free(m);
}

void msg(const char *fmt, ...)
{
	char pre[64];
	va_list ap;
	snprintf(pre, sizeof pre, "%s==>%s %s", c_g, c_0, c_b);
	va_start(ap, fmt);
	vout(pre, c_0, fmt, ap);
	va_end(ap);
}

void msg2(const char *fmt, ...)
{
	char pre[64];
	va_list ap;
	snprintf(pre, sizeof pre, "  %s->%s ", c_c, c_0);
	va_start(ap, fmt);
	vout(pre, "", fmt, ap);
	va_end(ap);
}

void warn(const char *fmt, ...)
{
	char pre[64];
	va_list ap;
	snprintf(pre, sizeof pre, "%swarning:%s ", c_y, c_0);
	va_start(ap, fmt);
	vout(pre, "", fmt, ap);
	va_end(ap);
}

void report_error(const char *fmt, ...)
{
	char pre[64];
	va_list ap;
	snprintf(pre, sizeof pre, "%serror:%s ", c_r, c_0);
	va_start(ap, fmt);
	vout(pre, "", fmt, ap);
	va_end(ap);
}

void (*die_hook)(void);

void exit_status(int rc)
{
	void (*h)(void) = die_hook;
	die_hook = NULL; /* once, even when the hook dies itself */
	if (h)
		h();
	exit(rc);
}

void die(const char *fmt, ...)
{
	char pre[64];
	va_list ap;
	snprintf(pre, sizeof pre, "%serror:%s ", c_r, c_0);
	va_start(ap, fmt);
	vout(pre, "", fmt, ap);
	va_end(ap);
	exit_status(1);
}

void die_errno(const char *fmt, ...)
{
	int e = errno;
	va_list ap;
	va_start(ap, fmt);
	char *m = xvasprintf(fmt, ap);
	va_end(ap);
	die("%s: %s", m, strerror(e));
}

bool confirm(const char *question)
{
	if (opt_yes)
		return true;
	if (!isatty(0))
		die("confirmation needed but stdin is not a terminal (use --yes)");
	fprintf(stderr, "%s [y/N] ", question);
	fflush(stderr);
	char line[64];
	if (!fgets(line, sizeof line, stdin))
		return false;
	return line[0] == 'y' || line[0] == 'Y';
}

/* ---- memory ---- */

static NORETURN void oom(void)
{
	static const char m[] = "burn: out of memory\n";
	write_all(2, m, sizeof m - 1);
	abort();
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		oom();
	return p;
}

void *xcalloc(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size ? size : 1);
	if (!p)
		oom();
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p)
		oom();
	return p;
}

char *xstrdup(const char *s)
{
	return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);
	memcpy(p, s, n);
	p[n] = 0;
	return p;
}

char *xvasprintf(const char *fmt, va_list ap)
{
	if (!fmt)
		fmt = "";
	va_list aq;
	va_copy(aq, ap);
	int n = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	if (n < 0)
		oom();
	char *p = xmalloc((size_t)n + 1);
	vsnprintf(p, (size_t)n + 1, fmt, ap);
	return p;
}

char *xasprintf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *p = xvasprintf(fmt, ap);
	va_end(ap);
	return p;
}

/* ---- sbuf ---- */

void sb_grow(sbuf *b, size_t extra)
{
	if (b->len + extra + 1 <= b->cap)
		return;
	size_t cap = b->cap ? b->cap : 64;
	while (cap < b->len + extra + 1)
		cap *= 2;
	b->s = xrealloc(b->s, cap);
	b->cap = cap;
}

void sb_addn(sbuf *b, const char *s, size_t n)
{
	sb_grow(b, n);
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = 0;
}

void sb_adds(sbuf *b, const char *s)
{
	sb_addn(b, s, strlen(s));
}

void sb_addc(sbuf *b, char c)
{
	sb_grow(b, 1);
	b->s[b->len++] = c;
	b->s[b->len] = 0;
}

void sb_addf(sbuf *b, const char *fmt, ...)
{
	if (!fmt)
		fmt = "";
	va_list ap, aq;
	va_start(ap, fmt);
	va_copy(aq, ap);
	int n = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	if (n < 0)
		oom();
	sb_grow(b, (size_t)n);
	vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->len += (size_t)n;
}

const char *sb_str(sbuf *b)
{
	if (!b->s) {
		sb_grow(b, 0);
		b->s[0] = 0;
	}
	return b->s;
}

char *sb_steal(sbuf *b)
{
	sb_str(b);
	char *s = b->s;
	b->s = NULL;
	b->len = b->cap = 0;
	return s;
}

void sb_reset(sbuf *b)
{
	b->len = 0;
	if (b->s)
		b->s[0] = 0;
}

void sb_free(sbuf *b)
{
	free(b->s);
	b->s = NULL;
	b->len = b->cap = 0;
}

/* ---- svec ---- */

static void sv_grow(svec *v)
{
	if (v->n + 2 > v->cap) {
		v->cap = v->cap ? v->cap * 2 : 8;
		v->v = xrealloc(v->v, v->cap * sizeof *v->v);
	}
}

void sv_push_own(svec *v, char *s)
{
	sv_grow(v);
	v->v[v->n++] = s;
	v->v[v->n] = NULL;
}

void sv_push(svec *v, const char *s)
{
	sv_push_own(v, xstrdup(s));
}

void sv_pushf(svec *v, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	sv_push_own(v, xvasprintf(fmt, ap));
	va_end(ap);
}

void sv_extend(svec *v, const svec *w)
{
	for (size_t i = 0; i < w->n; i++)
		sv_push(v, w->v[i]);
}

ssize_t sv_index(const svec *v, const char *s)
{
	for (size_t i = 0; i < v->n; i++)
		if (!strcmp(v->v[i], s))
			return (ssize_t)i;
	return -1;
}

bool sv_has(const svec *v, const char *s)
{
	return sv_index(v, s) >= 0;
}

bool sv_bsearch(const svec *v, const char *s)
{
	size_t lo = 0, hi = v->n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = strcmp(v->v[mid], s);
		if (!c)
			return true;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return false;
}

void sv_remove(svec *v, size_t i)
{
	if (i >= v->n)
		return;
	free(v->v[i]);
	memmove(v->v + i, v->v + i + 1, (v->n - i) * sizeof *v->v); /* moves the NULL too */
	v->n--;
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

void sv_sort(svec *v)
{
	if (v->n > 1)
		qsort(v->v, v->n, sizeof *v->v, cmp_str);
}

void sv_sort_uniq(svec *v)
{
	sv_sort(v);
	size_t j = 0;
	for (size_t i = 0; i < v->n; i++) {
		if (j && !strcmp(v->v[j - 1], v->v[i]))
			free(v->v[i]);
		else
			v->v[j++] = v->v[i];
	}
	v->n = j;
	if (v->v)
		v->v[j] = NULL;
}

char *sv_join(const svec *v, const char *sep)
{
	sbuf b = SBUF_INIT;
	for (size_t i = 0; i < v->n; i++) {
		if (i)
			sb_adds(&b, sep);
		sb_adds(&b, v->v[i]);
	}
	return sb_steal(&b);
}

static bool is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\n';
}

void sv_split_ws(svec *v, const char *s)
{
	while (*s) {
		while (is_ws(*s))
			s++;
		if (!*s)
			break;
		const char *e = s;
		while (*e && !is_ws(*e))
			e++;
		sv_push_own(v, xstrndup(s, (size_t)(e - s)));
		s = e;
	}
}

char **sv_argv(svec *v)
{
	if (!v->v) {
		sv_grow(v);
		v->v[0] = NULL;
	}
	return v->v;
}

void sv_clear(svec *v)
{
	for (size_t i = 0; i < v->n; i++)
		free(v->v[i]);
	v->n = 0;
	if (v->v)
		v->v[0] = NULL;
}

void sv_free(svec *v)
{
	sv_clear(v);
	free(v->v);
	v->v = NULL;
	v->cap = 0;
}

/* ---- string map ---- */

static uint64_t str_hash(const char *s)
{
	uint64_t h = 1469598103934665603ull; /* FNV-1a */
	for (; *s; s++)
		h = (h ^ (unsigned char)*s) * 1099511628211ull;
	return h;
}

static size_t sm_slot(const strmap *m, const char *key)
{
	size_t i = (size_t)str_hash(key) & (m->cap - 1);
	while (m->keys[i] && strcmp(m->keys[i], key))
		i = (i + 1) & (m->cap - 1);
	return i;
}

void sm_put(strmap *m, const char *key, size_t val)
{
	if ((m->n + 1) * 10 > m->cap * 7) {
		strmap g = {NULL, NULL, 0, m->cap ? m->cap * 2 : 64};
		g.keys = xcalloc(g.cap, sizeof *g.keys);
		g.vals = xcalloc(g.cap, sizeof *g.vals);
		for (size_t i = 0; i < m->cap; i++) {
			if (m->keys[i]) {
				size_t j = sm_slot(&g, m->keys[i]);
				g.keys[j] = m->keys[i];
				g.vals[j] = m->vals[i];
				g.n++;
			}
		}
		free(m->keys);
		free(m->vals);
		*m = g;
	}
	size_t i = sm_slot(m, key);
	if (!m->keys[i]) {
		m->keys[i] = xstrdup(key);
		m->n++;
	}
	m->vals[i] = val;
}

bool sm_get(const strmap *m, const char *key, size_t *val)
{
	if (!m->cap)
		return false;
	size_t i = sm_slot(m, key);
	if (!m->keys[i])
		return false;
	if (val)
		*val = m->vals[i];
	return true;
}

bool sm_has(const strmap *m, const char *key)
{
	return sm_get(m, key, NULL);
}

void sm_free(strmap *m)
{
	for (size_t i = 0; i < m->cap; i++)
		free(m->keys[i]);
	free(m->keys);
	free(m->vals);
	*m = (strmap)STRMAP_INIT;
}

/* ---- strings ---- */

bool str_prefix(const char *s, const char *prefix)
{
	return !strncmp(s, prefix, strlen(prefix));
}

bool str_suffix(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);
	return n >= m && !memcmp(s + n - m, suffix, m);
}

bool str_eq(const char *a, const char *b)
{
	return !strcmp(a ? a : "", b ? b : "");
}

bool str_uint(const char *s)
{
	if (!*s)
		return false;
	for (; *s; s++)
		if (*s < '0' || *s > '9')
			return false;
	return true;
}

/* ---- paths ---- */

char *path_join(const char *a, const char *b)
{
	size_t n = strlen(a);
	while (n && a[n - 1] == '/')
		n--;
	while (*b == '/')
		b++;
	return xasprintf("%.*s/%s", (int)n, a, b);
}

char *path_abs(const char *p)
{
	sbuf out = SBUF_INIT; /* resolved prefix without a trailing slash; "" is the root */
	char *rest;
	if (*p == '/') {
		rest = xstrdup(p);
	} else {
		char cwd[PATH_MAX];
		if (!getcwd(cwd, sizeof cwd))
			die_errno("getcwd");
		rest = xasprintf("%s/%s", cwd, p);
	}
	const char *r = rest;
	int links = 0;
	sb_str(&out);
	while (*r) {
		while (*r == '/')
			r++;
		if (!*r)
			break;
		const char *e = strchr(r, '/');
		size_t n = e ? (size_t)(e - r) : strlen(r);
		if (n == 1 && r[0] == '.') {
			r += n;
			continue;
		}
		if (n == 2 && r[0] == '.' && r[1] == '.') {
			char *slash = strrchr(out.s, '/');
			out.len = slash ? (size_t)(slash - out.s) : 0;
			out.s[out.len] = 0;
			r += n;
			continue;
		}
		size_t keep = out.len;
		sb_addc(&out, '/');
		sb_addn(&out, r, n);
		r += n;
		struct stat st;
		if (links < 40 && lstat(out.s, &st) == 0 && S_ISLNK(st.st_mode)) {
			char buf[PATH_MAX];
			ssize_t l = readlink(out.s, buf, sizeof buf - 1);
			if (l < 0)
				continue;
			buf[l] = 0;
			links++;
			char *nr = xasprintf("%s/%s", buf, r);
			free(rest);
			rest = nr;
			r = rest;
			out.len = buf[0] == '/' ? 0 : keep;
			out.s[out.len] = 0;
		}
	}
	free(rest);
	if (!out.len) {
		sb_free(&out);
		return xstrdup("/");
	}
	return sb_steal(&out);
}

/* the PWD of bash's `cd DIR` once chdir(DIR) worked: DIR made absolute against the logical OLDPWD
 * and canonicalized without resolving symlinks (/a/./b//c/.. is /a/b), the physical getcwd() when
 * that is not the directory we are in. Programs then see the directory by the name the
 * configuration gave it: configure's in-tree test compares `pwd` with `cd $srcdir && pwd`. */
char *path_logical(const char *dir, const char *oldpwd)
{
	char *in = NULL;
	if (*dir == '/')
		in = xstrdup(dir);
	else if (oldpwd && *oldpwd == '/')
		in = xasprintf("%s/%s", oldpwd, dir);
	if (in) {
		sbuf out = SBUF_INIT;
		sb_str(&out);
		for (const char *r = in; *r;) {
			while (*r == '/')
				r++;
			if (!*r)
				break;
			const char *e = strchr(r, '/');
			size_t n = e ? (size_t)(e - r) : strlen(r);
			if (n == 2 && r[0] == '.' && r[1] == '.') {
				char *slash = strrchr(out.s, '/');
				out.len = slash ? (size_t)(slash - out.s) : 0;
				out.s[out.len] = 0;
			} else if (!(n == 1 && r[0] == '.')) {
				sb_addc(&out, '/');
				sb_addn(&out, r, n);
			}
			r += n;
		}
		free(in);
		if (!out.len)
			sb_addc(&out, '/');
		struct stat a, b;
		if (stat(out.s, &a) == 0 && stat(".", &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino)
			return sb_steal(&out);
		sb_free(&out);
	}
	char *cwd = getcwd(NULL, 0);
	if (!cwd)
		die_errno("getcwd");
	return cwd;
}

char *path_real(const char *p)
{
	char *r = realpath(p, NULL);
	if (!r && errno == ENOMEM)
		oom();
	return r;
}

const char *path_base(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

char *path_dir(const char *p)
{
	const char *s = strrchr(p, '/');
	if (!s)
		return xstrdup(".");
	if (s == p)
		return xstrdup("/");
	return xstrndup(p, (size_t)(s - p));
}

/* ---- files ---- */

bool is_file(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

bool is_dir(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_link(const char *p)
{
	struct stat st;
	return lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
}

bool exists(const char *p)
{
	struct stat st;
	return lstat(p, &st) == 0;
}

bool is_exec(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISREG(st.st_mode) && access(p, X_OK) == 0;
}

int mkdir_p(const char *path, mode_t mode)
{
	char *p = xstrdup(path);
	size_t n = strlen(p);
	while (n > 1 && p[n - 1] == '/')
		p[--n] = 0;
	for (char *s = p + 1;; s++) {
		if (*s && *s != '/')
			continue;
		char c = *s;
		*s = 0;
		if (mkdir(p, mode) < 0) {
			int e = errno;
			struct stat st;
			if (e != EEXIST || stat(p, &st) < 0 || !S_ISDIR(st.st_mode)) {
				free(p);
				errno = e == EEXIST ? ENOTDIR : e;
				return -1;
			}
		}
		*s = c;
		if (!c)
			break;
	}
	free(p);
	return 0;
}

static int rm_one(const char *p, const struct stat *st, int type, struct FTW *ftw)
{
	(void)st;
	(void)ftw;
	int r = type == FTW_DP ? rmdir(p) : unlink(p);
	if (r < 0 && errno != ENOENT)
		return -1;
	return 0;
}

void glob_paths(const char *dir, const char *pattern, svec *out)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	svec names = SVEC_INIT;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' && pattern[0] != '.')
			continue;
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (fnmatch(pattern, e->d_name, FNM_PERIOD) == 0)
			sv_push(&names, e->d_name);
	}
	closedir(d);
	sv_sort(&names);
	for (size_t i = 0; i < names.n; i++)
		sv_pushf(out, "%s/%s", dir, names.v[i]);
	sv_free(&names);
}

int rm_rf(const char *path)
{
	struct stat st;
	if (lstat(path, &st) < 0)
		return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	return nftw(path, rm_one, 32, FTW_DEPTH | FTW_PHYS);
}

char *file_read(const char *path, size_t *len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	sbuf b = SBUF_INIT;
	struct stat st;
	if (fstat(fd, &st) == 0 && st.st_size > 0)
		sb_grow(&b, (size_t)st.st_size);
	for (;;) {
		sb_grow(&b, 65536);
		ssize_t n = read(fd, b.s + b.len, b.cap - b.len - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			int e = errno;
			close(fd);
			sb_free(&b);
			errno = e;
			return NULL;
		}
		if (n == 0)
			break;
		b.len += (size_t)n;
	}
	close(fd);
	b.s[b.len] = 0;
	if (len)
		*len = b.len;
	return sb_steal(&b);
}

int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

int file_write(const char *path, const void *data, size_t len, mode_t mode)
{
	char *tmp = xasprintf("%s.burn-XXXXXX", path);
	int fd = mkostemp(tmp, O_CLOEXEC);
	if (fd < 0) {
		free(tmp);
		return -1;
	}
	int rc = write_all(fd, data, len);
	if (rc == 0)
		rc = fchmod(fd, mode);
	if (close(fd) < 0)
		rc = -1;
	if (rc == 0)
		rc = rename(tmp, path);
	if (rc < 0) {
		int e = errno;
		unlink(tmp);
		errno = e;
	}
	free(tmp);
	return rc;
}

int file_append(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	int rc = write_all(fd, s, strlen(s));
	if (close(fd) < 0)
		rc = -1;
	return rc;
}

int file_lines(const char *path, svec *out)
{
	size_t len;
	char *d = file_read(path, &len);
	if (!d)
		return errno == ENOENT ? 0 : -1;
	char *s = d;
	while (*s) {
		char *e = strchr(s, '\n');
		size_t n = e ? (size_t)(e - s) : strlen(s);
		sv_push_own(out, xstrndup(s, n));
		s += n + (e ? 1 : 0);
	}
	free(d);
	return 0;
}

char *find_program(const char *name)
{
	if (strchr(name, '/'))
		return is_exec(name) ? xstrdup(name) : NULL;
	const char *path = getenv("PATH");
	if (!path)
		path = "/usr/bin:/bin";
	while (*path) {
		const char *e = strchr(path, ':');
		size_t n = e ? (size_t)(e - path) : strlen(path);
		char *c = n ? xasprintf("%.*s/%s", (int)n, path, name) : xasprintf("./%s", name);
		if (is_exec(c))
			return c;
		free(c);
		path += n + (e ? 1 : 0);
	}
	return NULL;
}

bool have(const char *prog)
{
	char *p = find_program(prog);
	bool ok = p != NULL;
	free(p);
	return ok;
}

long nproc(void)
{
	cpu_set_t set;
	if (sched_getaffinity(0, sizeof set, &set) == 0) {
		int n = CPU_COUNT(&set);
		if (n > 0)
			return n;
	}
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return n > 0 ? n : 2;
}
