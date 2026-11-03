/* SPDX-License-Identifier: MIT */
/*
 * Building a recipe into a package, the prototype's build_pkg: fetch, unpack, patch, the phases
 * (the build type in C, recipe functions in bash), finalize (merged /usr, pruning, strip), the
 * metadata and the package itself, packed in the process. Same steps, messages, log and bytes.
 */
#include "burn.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "elf.h"
#include "spawn.h"
#include "tar.h"

static const char *const PHASES[] = {"prepare", "configure", "build", "check", "install"};

/* ---- the tree of DESTDIR ---- */

/* the entries of DIR, sorted (LC_ALL=C ls -A) */
static void ls_A(const char *dir, svec *out)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			sv_push(out, e->d_name);
	closedir(d);
	sv_sort(out);
}

typedef struct {
	char *path; /* absolute */
	mode_t mode; /* lstat */
} node;

typedef struct {
	node *v;
	size_t n, cap;
} nodes;

/* everything below DIR, pre-order with sorted entries; skip: a name at the top to leave out */
static void tree(const char *dir, const char *skip, nodes *out)
{
	svec names = SVEC_INIT;
	ls_A(dir, &names);
	for (size_t i = 0; i < names.n; i++) {
		if (skip && !strcmp(names.v[i], skip))
			continue;
		char *p = path_join(dir, names.v[i]);
		struct stat st;
		if (lstat(p, &st) < 0) {
			free(p);
			continue;
		}
		if (out->n == out->cap) {
			out->cap = out->cap ? out->cap * 2 : 256;
			out->v = xrealloc(out->v, out->cap * sizeof *out->v);
		}
		out->v[out->n++] = (node){p, st.st_mode};
		if (S_ISDIR(st.st_mode))
			tree(p, NULL, out);
	}
	sv_free(&names);
}

static void nodes_free(nodes *t)
{
	for (size_t i = 0; i < t->n; i++)
		free(t->v[i].path);
	free(t->v);
	memset(t, 0, sizeof *t);
}

/* ---- the log ---- */

static void log_open(build *b, bool truncate)
{
	const char *log = bget(b, "LOG");
	if (b->log_fd >= 0)
		close(b->log_fd);
	b->log_fd = open(log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | (truncate ? O_TRUNC : 0), 0644);
	if (b->log_fd < 0)
		die_errno("cannot write %s", log);
}

/* tail -n LINES FILE >&2 */
static void log_tail(const char *file, int lines)
{
	size_t len;
	char *d = file_read(file, &len);
	if (!d)
		return;
	size_t start = len && d[len - 1] == '\n' ? len - 1 : len;
	int seen = 0;
	while (start > 0 && !(d[start - 1] == '\n' && ++seen == lines))
		start--;
	write_all(2, d + start, len - start);
	free(d);
}

/* ---- phases ---- */

typedef struct {
	build *b;
	const char *type, *step;
	svec *env;
} step_job;

static void extra_lines(bt_ctx *c, const char *fn, svec *out)
{
	shrun_lines(c->user, sv_argv(c->env), fn, out);
}

static int step_child(void *arg)
{
	step_job *j = arg;
	bt_ctx c = {.v = &j->b->v, .env = j->env, .extra = extra_lines, .user = j->b};
	return bt_run(j->type, j->step, &c);
}

static char *phase_cleanup; /* a file to remove when a phase fails */

/* phase LABEL: the recipe function FN (bash, in SRC) or the STEP of the build type (C), with the
 * output in the log (and on stderr with -v); a failure shows the end of the log and stops */
static void phase(build *b, const char *label, const char *fn, const char *type, const char *step)
{
	msg2("%s", label);
	svec env = SVEC_INIT;
	build_environ(b, &env);
	run_opts o = {.envp = sv_argv(&env), .log_fd = b->log_fd, .tee = B.verbose};
	int rc;
	if (fn) {
		svec argv = SVEC_INIT;
		shrun_argv(b, fn, true, &argv);
		rc = run(sv_argv(&argv), &o);
		sv_free(&argv);
	} else {
		step_job j = {b, type, step, &env};
		rc = run_func(step_child, &j, &o);
	}
	sv_free(&env);
	if (rc) {
		if (phase_cleanup)
			unlink(phase_cleanup);
		log_tail(bget(b, "LOG"), 40);
		die("%s: %s failed (exit %d, full log: %s)", rget(b->r, "name"), label, rc, bget(b, "LOG"));
	}
}

static bool has_fn(const build *b, const char *fn)
{
	return sv_has(&b->r->funcs, fn);
}

/* ---- sources ---- */

static bool is_archive(const char *n)
{
	static const char *const ext[] = {".tar",    ".tar.gz", ".tgz",     ".tar.xz", ".txz",
					  ".tar.bz2", ".tbz2",  ".tar.zst", ".tar.lz", ".zip"};
	for (size_t i = 0; i < ARRAY_LEN(ext); i++)
		if (str_suffix(n, ext[i]))
			return true;
	return false;
}

/* a command whose failure ends burn with its status, like errexit did */
static void must_run(char *const argv[])
{
	int rc = run(argv, NULL);
	if (rc)
		exit_status(rc);
}

static void mkdirs(const char *dir)
{
	if (mkdir_p(dir, 0755) < 0)
		die_errno("cannot create %s", dir);
}

/* cp -f FROM TO */
static void copy_file(const char *from, const char *to)
{
	int in = open(from, O_RDONLY | O_CLOEXEC);
	struct stat st;
	if (in < 0 || fstat(in, &st) < 0)
		die_errno("cannot read %s", from);
	unlink(to);
	int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 0777);
	if (out < 0)
		die_errno("cannot write %s", to);
	char buf[65536];
	for (;;) {
		ssize_t n = read(in, buf, sizeof buf);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			die_errno("cannot read %s", from);
		if (n == 0)
			break;
		if (write_all(out, buf, (size_t)n) < 0)
			die_errno("cannot write %s", to);
	}
	if (close(out) < 0)
		die_errno("cannot write %s", to);
	close(in);
}

/* archives into WORK/src (SRC: srcdir=, or the first new top directory), other files into
 * WORK/files (FILES) */
static void unpack_sources(build *b)
{
	recipe *r = b->r;
	const char *work = bget(b, "WORK");
	char *src = path_join(work, "src"), *files = path_join(work, "files");
	rm_rf(src);
	rm_rf(files);
	mkdirs(src);
	mkdirs(files);
	bset(b, "FILES", files);
	svec specs = SVEC_INIT, noextract = SVEC_INIT;
	rlist(r, "source", &specs);
	rlist(r, "noextract", &noextract);
	char *first = NULL;
	for (size_t i = 0; i < specs.n; i++) {
		char *file = source_path(r, specs.v[i]);
		source_spec s;
		source_info(specs.v[i], &s);
		if (is_archive(s.name) && !sv_has(&noextract, s.name)) {
			svec before = SVEC_INIT;
			ls_A(src, &before);
			if (str_suffix(s.name, ".zip")) {
				char *argv[] = {"unzip", "-q", file, "-d", src, NULL};
				must_run(argv);
			} else {
				char *argv[] = {"tar", "-xf", file, "-C", src, "--no-same-owner", NULL};
				must_run(argv);
			}
			if (!first) {
				svec after = SVEC_INIT;
				ls_A(src, &after);
				for (size_t k = 0; k < after.n && !first; k++)
					if (!sv_bsearch(&before, after.v[k]))
						first = xstrdup(after.v[k]);
				sv_free(&after);
			}
			sv_free(&before);
		} else {
			char *to = path_join(files, s.name);
			copy_file(file, to);
			free(to);
		}
		source_spec_free(&s);
		free(file);
	}
	const char *srcdir = rget(r, "srcdir");
	char *dir;
	if (*srcdir) {
		dir = path_join(src, srcdir);
	} else {
		char *f = first ? path_join(src, first) : NULL;
		if (f && is_dir(f)) {
			dir = f;
			f = NULL;
		} else {
			dir = xstrdup(src);
		}
		free(f);
	}
	bset(b, "SRC", dir);
	if (!is_dir(dir))
		die("%s: source directory %s does not exist (check srcdir=)", rget(r, "name"), dir);
	free(dir);
	free(first);
	sv_free(&specs);
	sv_free(&noextract);
	free(src);
	free(files);
}

static void apply_patches(build *b)
{
	recipe *r = b->r;
	const char *name = rget(r, "name");
	svec patches = SVEC_INIT;
	rlist(r, "patches", &patches);
	for (size_t i = 0; i < patches.n; i++) {
		const char *p = patches.v[i];
		char *c[3] = {path_join(r->dir, p), xasprintf("%s/patches/%s", r->dir, p), path_join(bget(b, "FILES"), p)};
		const char *f = NULL;
		for (int k = 0; k < 3 && !f; k++)
			if (is_file(c[k]))
				f = c[k];
		if (!f)
			die("%s: patch '%s' not found", name, p);
		msg2("patch %s", p);
		char *strip = xasprintf("-Np%s", rget(r, "patch_strip"));
		char *argv[] = {"patch", "-d", (char *)bget(b, "SRC"), strip, "-i", (char *)f, NULL};
		run_opts o = {.log_fd = b->log_fd};
		if (run(argv, &o) != 0)
			die("%s: patch %s failed (log: %s)", name, p, bget(b, "LOG"));
		free(strip);
		for (int k = 0; k < 3; k++)
			free(c[k]);
	}
	sv_free(&patches);
}

/* ---- finalize: keep the tree clean ---- */

/* cp -a FROM/. TO/ && rm -rf FROM, by renaming: what exists in TO is replaced, directories
 * merge, TO gets the mode and times of FROM */
static void merge_into(const char *from, const char *to)
{
	struct stat fs;
	if (lstat(from, &fs) < 0)
		die_errno("%s", from);
	svec names = SVEC_INIT;
	ls_A(from, &names);
	for (size_t i = 0; i < names.n; i++) {
		char *s = path_join(from, names.v[i]), *t = path_join(to, names.v[i]);
		struct stat ss, ts;
		if (lstat(s, &ss) < 0)
			die_errno("%s", s);
		bool there = lstat(t, &ts) == 0;
		if (there && S_ISDIR(ss.st_mode) && S_ISDIR(ts.st_mode))
			merge_into(s, t);
		else if (there && S_ISDIR(ss.st_mode) != S_ISDIR(ts.st_mode))
			die("cannot merge %s into %s: one is a directory, the other is not", s, t);
		else if (rename(s, t) < 0)
			die_errno("cannot move %s to %s", s, t);
		free(s);
		free(t);
	}
	sv_free(&names);
	chmod(to, fs.st_mode & 07777);
	struct timespec ts[2] = {fs.st_atim, fs.st_mtim};
	utimensat(AT_FDCWD, to, ts, AT_SYMLINK_NOFOLLOW);
}

static void merge_usr(const char *d)
{
	static const char *const pairs[][2] = {{"bin", "usr/bin"}, {"sbin", "usr/bin"}, {"usr/sbin", "usr/bin"},
					       {"lib", "usr/lib"}, {"lib64", "usr/lib"}, {"usr/lib64", "usr/lib"}};
	for (size_t i = 0; i < ARRAY_LEN(pairs); i++) {
		char *from = path_join(d, pairs[i][0]), *to = path_join(d, pairs[i][1]);
		if (is_dir(from) && !is_link(from)) {
			mkdirs(to);
			merge_into(from, to);
			rm_rf(from);
		}
		free(from);
		free(to);
	}
}

/* keep only the LOCALES translations in DIR; man pages keep their sections (man1, ...) */
static void prune_langs(const build *b, const char *dir, bool keep_sections)
{
	const char *locales = bget(b, "LOCALES");
	if (!is_dir(dir) || !*locales || !strcmp(locales, "all"))
		return;
	svec keep = SVEC_INIT, all = SVEC_INIT;
	sv_split_ws(&keep, locales);
	glob_paths(dir, "*", &all);
	for (size_t i = 0; i < all.n; i++) {
		const char *l = path_base(all.v[i]);
		if (!is_dir(all.v[i]) || (keep_sections && str_prefix(l, "man")))
			continue;
		bool ok = false;
		for (size_t k = 0; k < keep.n && !ok; k++) {
			size_t n = strlen(keep.v[k]);
			ok = !strncmp(l, keep.v[k], n) && (!l[n] || strchr("_@.", l[n]));
		}
		if (!ok)
			rm_rf(all.v[i]);
	}
	sv_free(&keep);
	sv_free(&all);
}

static void strip_batch(const build *b, const char *how, svec *files, char **envp)
{
	for (size_t at = 0; at < files->n;) {
		svec argv = SVEC_INIT;
		sv_push(&argv, bget(b, "STRIP"));
		sv_push(&argv, how);
		size_t len = 0;
		for (; at < files->n && argv.n < 512 && len < 65536; at++) {
			sv_push(&argv, files->v[at]);
			len += strlen(files->v[at]) + 1;
		}
		run_opts o = {.envp = envp, .quiet = true}; /* 2>/dev/null || true */
		run(sv_argv(&argv), &o);
		sv_free(&argv);
	}
}

static void strip_tree(const build *b, const nodes *t)
{
	static const char *const skip[] = {"*.ko", "*.ko.*", "*.o", "*/usr/lib/debug/*"};
	svec so = SVEC_INIT, other = SVEC_INIT;
	for (size_t i = 0; i < t->n; i++) {
		const char *f = t->v[i].path;
		if (!S_ISREG(t->v[i].mode) || str_suffix(path_base(f), ".a"))
			continue;
		bool skipped = false;
		for (size_t k = 0; k < ARRAY_LEN(skip) && !skipped; k++)
			skipped = fnmatch(skip[k], f, 0) == 0;
		if (skipped || !is_elf(f))
			continue;
		sv_push(fnmatch("*.so", f, 0) == 0 || fnmatch("*.so.*", f, 0) == 0 ? &so : &other, f);
	}
	svec env = SVEC_INIT;
	build_environ(b, &env);
	strip_batch(b, "--strip-unneeded", &so, sv_argv(&env));
	strip_batch(b, "--strip-all", &other, sv_argv(&env));
	sv_free(&env);
	sv_free(&so);
	sv_free(&other);
}

/* find -mindepth 1 -type d -empty -delete: bottom up, so emptied parents go too */
static bool prune_empty(const char *dir, bool top)
{
	svec names = SVEC_INIT;
	ls_A(dir, &names);
	size_t left = names.n;
	for (size_t i = 0; i < names.n; i++) {
		char *p = path_join(dir, names.v[i]);
		struct stat st;
		if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode) && prune_empty(p, false))
			left--;
		free(p);
	}
	sv_free(&names);
	return !top && left == 0 && rmdir(dir) == 0;
}

static void finalize(build *b)
{
	recipe *r = b->r;
	const char *d = bget(b, "DESTDIR");
	merge_usr(d);
	nodes t = {0};
	tree(d, NULL, &t);
	for (size_t i = 0; i < t.n; i++)
		if (S_ISREG(t.v[i].mode) && str_suffix(path_base(t.v[i].path), ".la"))
			unlink(t.v[i].path);
	if (!rhas_option(r, "staticlibs")) {
		for (size_t i = 0; i < t.n; i++) {
			const char *a = t.v[i].path;
			if (!S_ISREG(t.v[i].mode) || !str_suffix(path_base(a), ".a"))
				continue;
			char *so = xasprintf("%.*s.so", (int)(strlen(a) - 2), a);
			struct stat st;
			if (stat(so, &st) == 0)
				unlink(a);
			free(so);
		}
	}
	nodes_free(&t);
	if (!rhas_option(r, "!prune")) {
		static const struct {
			const char *keep;
			const char *dirs[2];
		} docs[] = {{"KEEP_DOCS", {"usr/share/doc", "usr/share/gtk-doc"}},
			    {"KEEP_INFO", {"usr/share/info", NULL}},
			    {"KEEP_MAN", {"usr/share/man", NULL}}};
		for (size_t i = 0; i < ARRAY_LEN(docs); i++) {
			if (bon(b, docs[i].keep))
				continue;
			for (size_t k = 0; k < 2 && docs[i].dirs[k]; k++) {
				char *p = path_join(d, docs[i].dirs[k]);
				rm_rf(p);
				free(p);
			}
		}
		char *loc = path_join(d, "usr/share/locale"), *man = path_join(d, "usr/share/man");
		prune_langs(b, loc, false);
		prune_langs(b, man, true);
		free(loc);
		free(man);
	}
	tree(d, NULL, &t);
	if (bon(b, "STRIP_BINARIES") && !rhas_option(r, "!strip"))
		strip_tree(b, &t);
	if (bon(b, "LTO_ON") && !rhas_option(r, "staticlibs")) {
		for (size_t i = 0; i < t.n; i++) {
			const char *a = t.v[i].path;
			if (S_ISREG(t.v[i].mode) && str_suffix(path_base(a), ".a") && elf_has_lto(a))
				warn("%s: %s holds LTO-only objects; add options=(staticlibs) or options=(!lto)",
				     rget(r, "name"), a + strlen(d));
		}
	}
	nodes_free(&t);
	if (!rhas_option(r, "emptydirs"))
		prune_empty(d, true);
}

/* ---- the metadata: .BURN/meta, files, provides-so, needs-so ---- */

static void write_lines(const char *file, const svec *lines)
{
	sbuf s = SBUF_INIT;
	for (size_t i = 0; i < lines->n; i++)
		sb_addf(&s, "%s\n", lines->v[i]);
	if (file_write(file, s.s ? s.s : "", s.len, 0644) < 0)
		die_errno("cannot write %s", file);
	sb_free(&s);
}

/* date -u -d @T +%Y-%m-%dT%H:%M:%SZ (iso), or date -u */
static char *utc(long long t, bool iso)
{
	time_t tt = (time_t)t;
	struct tm tm;
	char buf[64];
	gmtime_r(&tt, &tm);
	if (iso)
		strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
	else
		strftime(buf, sizeof buf, "%a %b %e %H:%M:%S UTC %Y", &tm);
	return xstrdup(buf);
}

/* returns the number of files (not directories) */
static size_t write_meta(build *b)
{
	recipe *r = b->r;
	const char *d = bget(b, "DESTDIR");
	char *m = path_join(d, ".BURN");
	mkdirs(m);
	svec lic = SVEC_INIT, deps = SVEC_INIT, act = SVEC_INIT, backup = SVEC_INIT;
	rlist(r, "license", &lic);
	rlist(r, "depends", &deps);
	deps_active(r, &deps, &act);
	rlist(r, "backup", &backup);
	char *rid = recipe_rid(r), *feat = features_string(r);
	char *l = sv_join(&lic, " "), *a = sv_join(&act, " "), *bk = sv_join(&backup, " ");
	char *date = utc(strtoll(bget(b, "SOURCE_DATE_EPOCH"), NULL, 10), true);
	const char *cflags = *bget(b, "META_CFLAGS") ? bget(b, "META_CFLAGS") : bget(b, "CFLAGS");
	sbuf s = SBUF_INIT;
	sb_addf(&s, "name=%s\nversion=%s\nrelease=%s\narch=%s\nchost=%s\n", rget(r, "name"), rget(r, "version"),
		rget(r, "release"), bget(b, "CARCH"), bget(b, "CHOST"));
	sb_addf(&s, "buildid=%s\nrid=%s\nprofile=%s\n", bget(b, "BUILDID"), rid, bget(b, "PROFILE"));
	sb_addf(&s, "summary=%s\nlicense=%s\ndepends=%s\n", rget(r, "summary"), l, a);
	sb_addf(&s, "features=%s\nbackup=%s\n", feat, bk);
	sb_addf(&s, "cflags=%s\nldflags=%s\nbuilddate=%s\n", cflags, bget(b, "LDFLAGS"), date);
	char *f = path_join(m, "meta");
	if (file_write(f, s.s, s.len, 0644) < 0)
		die_errno("cannot write %s", f);
	free(f);
	sb_free(&s);
	free(date);
	free(l);
	free(a);
	free(bk);
	free(rid);
	free(feat);
	sv_free(&lic);
	sv_free(&deps);
	sv_free(&act);
	sv_free(&backup);

	nodes t = {0};
	tree(d, ".BURN", &t);
	svec files = SVEC_INIT, provides = SVEC_INIT, needs = SVEC_INIT, needed = SVEC_INIT;
	size_t nfiles = 0, base = strlen(d) + 1;
	for (size_t i = 0; i < t.n; i++) {
		bool dir = S_ISDIR(t.v[i].mode);
		sv_pushf(&files, "%s%s", t.v[i].path + base, dir ? "/" : "");
		nfiles += !dir;
		if (S_ISREG(t.v[i].mode) && is_elf(t.v[i].path))
			elf_dynamic(t.v[i].path, &provides, &needed);
	}
	nodes_free(&t);
	sv_sort(&files);
	sv_sort_uniq(&provides);
	sv_sort_uniq(&needed);
	for (size_t i = 0; i < needed.n; i++)
		if (!sv_bsearch(&provides, needed.v[i]))
			sv_push(&needs, needed.v[i]);
	static const char *const names[] = {"files", "provides-so", "needs-so"};
	const svec *lists[] = {&files, &provides, &needs};
	for (size_t i = 0; i < ARRAY_LEN(names); i++) {
		char *p = path_join(m, names[i]);
		write_lines(p, lists[i]);
		free(p);
	}
	sv_free(&files);
	sv_free(&provides);
	sv_free(&needs);
	sv_free(&needed);
	free(m);
	return nfiles;
}

/* ---- the package ---- */

/* PKG_COMPRESS: burn compresses with zstd itself and takes its level and threads */
static void pkg_compress(const build *b, int *level, int *threads)
{
	const char *pc = bget(b, "PKG_COMPRESS");
	svec w = SVEC_INIT;
	sv_split_ws(&w, pc);
	*level = 3;
	*threads = 1;
	bool ok = w.n && !strcmp(path_base(w.v[0]), "zstd");
	for (size_t i = 1; ok && i < w.n; i++) {
		const char *a = w.v[i];
		if (!strcmp(a, "-q") || !strcmp(a, "-c") || !strcmp(a, "--ultra") || !strcmp(a, "--quiet"))
			continue;
		if (a[0] == '-' && str_uint(a + 1))
			*level = atoi(a + 1);
		else if (str_prefix(a, "-T") && str_uint(a + 2))
			*threads = atoi(a + 2);
		else if (str_prefix(a, "--threads=") && str_uint(a + 10))
			*threads = atoi(a + 10);
		else
			ok = false;
	}
	sv_free(&w);
	if (!ok)
		die("PKG_COMPRESS=\"%s\": burn packs with zstd itself and understands zstd -LEVEL, -T THREADS, "
		    "--ultra and -q",
		    pc);
}

/* du -h: the disk usage, rounded up like coreutils (gnulib's human_readable, base 1024) */
static void du_h(const char *file, char *buf, size_t size)
{
	struct stat st;
	uint64_t amt = stat(file, &st) == 0 ? (uint64_t)st.st_blocks * 512 : 0;
	static const char letters[] = "KMGTPEZYRQ";
	const unsigned base = 1024;
	unsigned tenths = 0, rounding = 0;
	int exponent = 0;
	char frac[4] = "";
	if (amt >= base) {
		do {
			unsigned r10 = (unsigned)(amt % base) * 10 + tenths;
			unsigned r2 = (r10 % base) * 2 + (rounding >> 1);
			amt /= base;
			tenths = r10 / base;
			rounding = r2 < base ? (r2 + rounding) != 0 : 2 + (base < r2 + rounding);
			exponent++;
		} while (amt >= base && exponent < 10);
		if (amt < 10) {
			if (rounding > 0) {
				tenths++;
				rounding = 0;
				if (tenths == 10) {
					amt++;
					tenths = 0;
				}
			}
			if (amt < 10) {
				snprintf(frac, sizeof frac, ".%u", tenths % 10);
				tenths = rounding = 0;
			}
		}
	}
	if (tenths + rounding > 0) {
		amt++;
		if (amt == base && exponent < 10) {
			exponent++;
			snprintf(frac, sizeof frac, ".0");
			amt = 1;
		}
	}
	if (exponent)
		snprintf(buf, size, "%" PRIu64 "%s%c", amt, frac, letters[exponent - 1]);
	else
		snprintf(buf, size, "%" PRIu64 "%s", amt, frac);
}

static void package(build *b, const char *out, size_t nfiles)
{
	int level, threads;
	pkg_compress(b, &level, &threads);
	char *dir = path_dir(out), *part = xasprintf("%s.part", out), *err = NULL;
	mkdirs(dir);
	if (tar_write_dir(bget(b, "DESTDIR"), part, strtoll(bget(b, "SOURCE_DATE_EPOCH"), NULL, 10), level, threads,
			  &err) < 0)
		die("%s", err);
	if (rename(part, out) < 0)
		die_errno("cannot rename %s", part);
	char size[32];
	du_h(out, size, sizeof size);
	msg2("package %s (%s, %zu files)", path_base(out), size, nfiles);
	free(part);
	free(dir);
}

/* ---- build ---- */

char *build_pkg(const char *pkgname)
{
	recipe *r = recipe_load(pkgname);
	features_resolve(r);
	build b;
	build_init(&b, r);
	recipe_env(&b);
	const char *name = rget(r, "name"), *ver = rget(r, "version"), *rel = rget(r, "release");
	char *out = xasprintf("%s/packages/%s-%s-%s-%s-%s.pkg.tar.zst", bget(&b, "CACHE_DIR"), name, ver, rel,
			      bget(&b, "CARCH"), bget(&b, "BUILDID"));
	if (is_file(out) && !B.rebuild) {
		msg("%s %s-%s: using cached build %s", name, ver, rel, bget(&b, "BUILDID"));
		build_free(&b);
		return out;
	}
	const char *type = rget(r, "build_type");
	if (!bt_known(type))
		die("%s: unknown build_type '%s'", name, type);
	msg("Building %s %s-%s (%s, %s, %s)", name, ver, rel, bget(&b, "CHOST"), bon(&b, "CROSS") ? "cross" : "native",
	    type);
	if (!*bget(&b, "WORK_DIR"))
		die("internal: empty work dir");
	char *work = path_join(bget(&b, "WORK_DIR"), name);
	bset(&b, "WORK", work);
	char *p = path_join(work, "build");
	bset(&b, "BUILD", p);
	free(p);
	p = path_join(work, "dest");
	bset(&b, "DESTDIR", p);
	free(p);
	p = xasprintf("%s/logs/%s-%s-%s.log", bget(&b, "CACHE_DIR"), name, ver, rel);
	bset(&b, "LOG", p);
	free(p);
	rm_rf(work);
	mkdirs(bget(&b, "BUILD"));
	mkdirs(bget(&b, "DESTDIR"));
	p = path_dir(bget(&b, "LOG"));
	mkdirs(p);
	free(p);
	/* reproducible packages: the same recipe, flags and toolchain give the same bytes; times come
	 * from the recipe, build paths never end up in the binaries */
	char epoch[32];
	snprintf(epoch, sizeof epoch, "%lld", recipe_epoch(r));
	bexport(&b, "SOURCE_DATE_EPOCH", epoch);
	bset(&b, "META_CFLAGS", bget(&b, "CFLAGS"));
	char *map = xasprintf(" -ffile-prefix-map=%s=/usr/src/%s", work, name);
	static const char *const fl[] = {"CFLAGS", "CXXFLAGS"};
	for (size_t i = 0; i < ARRAY_LEN(fl); i++) {
		char *v = xasprintf("%s%s", bget(&b, fl[i]), map);
		bexport(&b, fl[i], v);
		free(v);
	}
	free(map);
	log_open(&b, true);
	char *now = utc(time(NULL), false), *feat = features_string(r);
	char *head = xasprintf("# burn %s: %s %s-%s, %s\n# CHOST=%s CC=%s SOURCE_DATE_EPOCH=%s\n# CFLAGS=%s\n"
			       "# LDFLAGS=%s\n# features: %s\n",
			       BURN_VERSION, name, ver, rel, now, bget(&b, "CHOST"), bget(&b, "CC"), epoch,
			       bget(&b, "CFLAGS"), bget(&b, "LDFLAGS"), feat);
	write_all(b.log_fd, head, strlen(head));
	free(head);
	free(now);
	free(feat);
	if (!strcmp(type, "meta")) {
		bset(&b, "SRC", work);
		bset(&b, "FILES", work);
	} else {
		fetch_sources(r, true);
		msg2("unpack");
		unpack_sources(&b);
		apply_patches(&b);
	}
	char *mf = xasprintf("-j%s", bget(&b, "JOBS"));
	bexport(&b, "MAKEFLAGS", mf);
	free(mf);
	char *envfile = path_join(work, "env.sh");
	shrun_write_env(&b, type, envfile);
	free(envfile);
	for (size_t i = 0; i < ARRAY_LEN(PHASES); i++) {
		const char *ph = PHASES[i];
		if (!strcmp(ph, "check") && strcmp(bget(&b, "CHECK"), "1"))
			continue;
		char *fn = xasprintf("do_%s", ph);
		if (has_fn(&b, fn))
			phase(&b, ph, fn, type, ph);
		else if (bt_has(type, ph))
			phase(&b, ph, NULL, type, ph);
		free(fn);
	}
	if (has_fn(&b, "post_install"))
		phase(&b, "post_install", "post_install", type, NULL);
	msg2("finalize");
	finalize(&b);
	size_t nfiles = write_meta(&b);
	package(&b, out, nfiles);
	if (strcmp(bget(&b, "KEEP_WORK"), "1"))
		rm_rf(work);
	free(work);
	build_free(&b);
	return out;
}

int cmd_build(int argc, char **argv)
{
	if (!argc)
		die("usage: burn build <pkg>...");
	for (int i = 0; i < argc; i++) {
		char *f = build_pkg(argv[i]);
		printf("%s\n", f);
		fflush(stdout);
		free(f);
	}
	return 0;
}

/* unpack PKG [DIR]: sources ready for hacking (or for bootstrap/), prints the source directory */
int cmd_unpack(int argc, char **argv)
{
	if (argc != 1 && argc != 2)
		die("usage: burn unpack <pkg> [dir]");
	recipe *r = recipe_load(argv[0]);
	features_resolve(r);
	build b;
	build_init(&b, r);
	const char *name = rget(r, "name");
	char *w = argc == 2 && *argv[1] ? xstrdup(argv[1]) : path_join(cfg("WORK_DIR"), name);
	char *work = path_abs(w);
	free(w);
	/* the layout of a real build, so do_prepare may use $BUILD and $DESTDIR */
	bset(&b, "WORK", work);
	char *p = path_join(work, "build");
	bset(&b, "BUILD", p);
	mkdirs(work);
	mkdirs(p);
	free(p);
	p = path_join(work, "dest");
	bset(&b, "DESTDIR", p);
	free(p);
	p = path_join(work, "unpack.log");
	bset(&b, "LOG", p);
	free(p);
	log_open(&b, true);
	msg("Unpacking %s %s into %s", name, rget(r, "version"), work);
	fetch_sources(r, true);
	unpack_sources(&b);
	apply_patches(&b);
	if (has_fn(&b, "do_prepare")) {
		/* not in WORK: the directory holds only what the prototype left there */
		const char *t = getenv("TMPDIR");
		char *envfile = xasprintf("%s/burn-env.XXXXXX", t && *t ? t : "/tmp");
		int fd = mkstemp(envfile);
		if (fd < 0)
			die_errno("mkstemp %s", envfile);
		close(fd);
		shrun_write_env(&b, NULL, envfile);
		phase_cleanup = envfile;
		phase(&b, "prepare", "do_prepare", NULL, NULL);
		phase_cleanup = NULL;
		unlink(envfile);
		free(envfile);
	}
	printf("%s\n", bget(&b, "SRC"));
	free(work);
	build_free(&b);
	return 0;
}
