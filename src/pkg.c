/* SPDX-License-Identifier: MIT */
/*
 * Packages in the root: installing a .pkg.tar.zst, removing, the history and the triggers. The
 * steps and messages are the prototype's (lib/db.sh); the file operations are new. Installing
 * happens in two phases: every file of the package is first written next to its place under a
 * temporary name, then all of them are renamed into place. A damaged package or a full disk in
 * the first phase changes nothing, and nobody ever sees a half-written file. Paths are resolved
 * inside the root (root.c), so a package can never write outside it.
 */
#include "burn.h"
#include "root.h"
#include "spawn.h"
#include "tar.h"
#include "vercmp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

/* ---- the package file ---- */

typedef struct {
	char *name; /* inside .BURN */
	char *data;
	size_t len;
	int64_t mtime;
} burn_file;

typedef struct {
	char *path;
	burn_file *burn;
	size_t nburn;
	svec meta, files, provides;
} pkg_info;

static const burn_file *burn_get(const pkg_info *p, const char *name)
{
	for (size_t i = 0; i < p->nburn; i++)
		if (!strcmp(p->burn[i].name, name))
			return &p->burn[i];
	return NULL;
}

static void lines_of(const burn_file *f, svec *out)
{
	if (!f)
		return;
	const char *s = f->data, *end = f->data + f->len;
	while (s < end) {
		const char *e = memchr(s, '\n', (size_t)(end - s));
		size_t n = e ? (size_t)(e - s) : (size_t)(end - s);
		sv_push_own(out, xstrndup(s, n));
		s += n + (e ? 1 : 0);
	}
}

static bool is_burn_path(const char *p)
{
	return !strcmp(p, ".BURN") || str_prefix(p, ".BURN/");
}

/* .BURN comes first in a package (tar --sort=name): reading stops after it */
static void pkg_read(const char *file, pkg_info *p)
{
	memset(p, 0, sizeof *p);
	p->path = xstrdup(file);
	char *err = NULL;
	tar_reader *t = tar_open(file, &err);
	if (!t)
		die("%s", err);
	const tar_entry *e;
	int r;
	bool seen = false;
	while ((r = tar_next(t, &e, &err)) > 0) {
		if (!is_burn_path(e->path)) {
			if (seen)
				break;
			continue;
		}
		seen = true;
		if (e->type != TAR_FILE || strchr(e->path + 6, '/'))
			continue;
		if (e->size > (256u << 20))
			die("%s: .BURN/%s is too large", file, e->path + 6);
		burn_file f = {xstrdup(e->path + 6), xmalloc((size_t)e->size + 1), (size_t)e->size, e->mtime};
		size_t got = 0;
		ssize_t n;
		while (got < f.len && (n = tar_read(t, f.data + got, f.len - got, &err)) > 0)
			got += (size_t)n;
		if (got < f.len)
			die("%s", err ? err : "short read");
		f.data[f.len] = 0;
		p->burn = xrealloc(p->burn, (p->nburn + 1) * sizeof *p->burn);
		p->burn[p->nburn++] = f;
	}
	if (r < 0)
		die("%s", err);
	tar_close(t);
	lines_of(burn_get(p, "meta"), &p->meta);
	lines_of(burn_get(p, "files"), &p->files);
	lines_of(burn_get(p, "provides-so"), &p->provides);
}

static void pkg_free(pkg_info *p)
{
	for (size_t i = 0; i < p->nburn; i++) {
		free(p->burn[i].name);
		free(p->burn[i].data);
	}
	free(p->burn);
	free(p->path);
	sv_free(&p->meta);
	sv_free(&p->files);
	sv_free(&p->provides);
}

/* ---- the transaction: lost sonames and triggers, for the whole command ---- */

svec txn_lost = SVEC_INIT;
static bool trig_ldconfig, trig_modules;

static void triggers_note(const svec *files, bool libs)
{
	die_hook = triggers_run; /* what is installed gets its triggers even when burn stops later */
	for (size_t i = 0; i < files->n; i++) {
		const char *f = files->v[i];
		if (str_prefix(f, "usr/lib/modules/") && f[16])
			trig_modules = true;
		if (libs && str_prefix(f, "usr/lib/")) { /* ^usr/lib/[^/]*\.so */
			const char *seg = f + 8, *slash = strchr(seg, '/'), *so = strstr(seg, ".so");
			if (so && (!slash || so + 3 <= slash))
				trig_ldconfig = true;
		}
	}
}

static char *find_sbin(const char *prog)
{
	char *p = find_program(prog);
	static const char *const dirs[] = {"/usr/sbin", "/sbin"};
	for (size_t i = 0; !p && i < ARRAY_LEN(dirs); i++) {
		char *c = xasprintf("%s/%s", dirs[i], prog);
		if (is_exec(c))
			p = c;
		else
			free(c);
	}
	return p;
}

/* PROG of the root, run here: through the root's own loader when it has one (its programs may
 * need a newer glibc than the host's) */
int root_run(const char *prog, char *const args[], bool quiet)
{
	char *link = xasprintf("%s/usr/bin/%s", B.R, prog);
	char *bin = path_real(link);
	int rc = 127;
	if (!bin || !is_exec(bin))
		goto out;
	svec lds = SVEC_INIT, argv = SVEC_INIT;
	char *libdir = xasprintf("%s/usr/lib", B.R);
	glob_paths(libdir, "ld-linux*.so.[0-9]", &lds);
	run_opts o = {.quiet = quiet, .stdin_null = true};
	if (lds.n) {
		sv_push(&argv, lds.v[0]);
		sv_push(&argv, "--library-path");
		sv_push(&argv, libdir);
		sv_push(&argv, "--argv0");
		sv_push(&argv, prog);
		sv_push(&argv, bin);
	} else {
		/* through the link: multi-call programs (kmod) dispatch on their argv[0] */
		sv_push(&argv, link);
	}
	for (char *const *a = args; *a; a++)
		sv_push(&argv, *a);
	svec env = SVEC_INIT;
	if (!lds.n) {
		env_from_environ(&env);
		const char *old = getenv("LD_LIBRARY_PATH");
		char *lp = old && *old ? xasprintf("%s:%s", libdir, old) : xstrdup(libdir);
		env_set(&env, "LD_LIBRARY_PATH", lp);
		free(lp);
		o.envp = sv_argv(&env);
	}
	rc = run(sv_argv(&argv), &o);
	sv_free(&env);
	sv_free(&argv);
	sv_free(&lds);
	free(libdir);
out:
	free(bin);
	free(link);
	return rc;
}

/* depmod KVER with the root's own kmod when it can run here, otherwise the host's (the index
 * files are portable); without either, the boot service generates them at the next boot */
static void depmod_run(const char *kver)
{
	char *host = find_sbin("depmod");
	bool ok = false;
	if (!*B.R) {
		ok = host && runl(host, kver, (char *)NULL) == 0;
	} else {
		char *own = xasprintf("%s/usr/bin/depmod", B.R);
		char *const args[] = {"-b", (char *)B.R, (char *)kver, NULL};
		ok = exists(own) && root_run("depmod", args, true) == 0;
		if (!ok && host) {
			/* quiet, like the prototype's 2>/dev/null: depmod writes nothing else */
			char *const argv[] = {host, "-b", (char *)B.R, (char *)kver, NULL};
			run_opts o = {.quiet = true, .stdin_null = true};
			ok = run(argv, &o) == 0;
		}
		free(own);
	}
	if (!ok)
		warn("depmod could not run: modules.dep for kernel %s is generated at the next boot", kver);
	free(host);
}

/* kernel modules changed: depmod for every installed kernel; the depmod output of kernels that
 * are gone is removed */
static void modules_trigger(void)
{
	static const char *const generated[] = {
		"modules.alias",   "modules.alias.bin", "modules.builtin.alias.bin", "modules.builtin.bin",
		"modules.dep",     "modules.dep.bin",   "modules.devname",           "modules.softdep",
		"modules.symbols", "modules.symbols.bin", "modules.weakdep"};
	char *base = xasprintf("%s/usr/lib/modules", B.R);
	svec dirs = SVEC_INIT;
	glob_paths(base, "*", &dirs);
	for (size_t i = 0; i < dirs.n; i++) {
		if (!is_dir(dirs.v[i]))
			continue;
		char *order = xasprintf("%s/modules.order", dirs.v[i]);
		if (is_file(order)) {
			depmod_run(path_base(dirs.v[i]));
		} else {
			for (size_t g = 0; g < ARRAY_LEN(generated); g++) {
				char *f = xasprintf("%s/%s", dirs.v[i], generated[g]);
				unlink(f);
				free(f);
			}
			rmdir(dirs.v[i]);
		}
		free(order);
	}
	sv_free(&dirs);
	free(base);
}

void triggers_run(void)
{
	if (trig_ldconfig && geteuid() == 0) {
		char *ld = find_sbin("ldconfig");
		if (ld && !*B.R) {
			if (runl(ld, (char *)NULL) != 0)
				warn("ldconfig failed");
		} else if (ld) {
			char *conf = xasprintf("%s/etc/ld.so.conf", B.R);
			if (is_file(conf)) {
				char *const argv[] = {ld, "-r", (char *)B.R, NULL};
				run_opts o = {.quiet = true, .stdin_null = true};
				run(argv, &o);
			}
			free(conf);
		}
		free(ld);
	}
	bool modules = trig_modules;
	trig_ldconfig = trig_modules = false;
	if (modules)
		modules_trigger();
}

/* ---- history ---- */

/* date|operation|name|old version|new version|previous package file */
void history_add(const char *op, const char *name, const char *old, const char *new, const char *file)
{
	if (mkdir_p(B.state_dir, 0755) < 0)
		die_errno("cannot create %s", B.state_dir);
	char date[32];
	time_t now = time(NULL);
	struct tm tm;
	gmtime_r(&now, &tm);
	strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", &tm);
	char *line = xasprintf("%s|%s|%s|%s|%s|%s\n", date, op, name, old, new, file);
	char *path = xasprintf("%s/history", B.state_dir);
	if (file_append(path, line) < 0)
		die_errno("cannot write %s", path);
	free(path);
	free(line);
}

/* ---- unpacking: phase 1 stages, phase 2 renames ---- */

typedef struct {
	char *path; /* in the root */
	char *tmp;  /* the temporary name next to it */
	int type;
} staged;

typedef struct {
	char *path;
	mode_t mode;
	uid_t uid;
	gid_t gid;
	int64_t mtime;
} new_dir;

typedef struct {
	const char *pkg;
	staged *st;
	size_t nst;
	new_dir *dirs;
	size_t ndirs;
	strmap index; /* path -> staged entry, for hard links */
	bool as_root;
} unpack;

static unsigned long tmp_seq;

static char *tmp_name(void)
{
	return xasprintf(".burn.%ld.%lu", (long)getpid(), ++tmp_seq);
}

/* phase 1 failed: the temporary files and the new directories go, nothing else was touched */
static NORETURN PRINTF(2, 3) void unpack_fail(unpack *u, const char *fmt, ...)
{
	int saved = errno;
	for (size_t i = u->nst; i-- > 0;) {
		const char *name;
		int dfd = root_parent(u->st[i].path, &name);
		if (dfd >= 0)
			unlinkat(dfd, u->st[i].tmp, 0);
	}
	for (size_t i = u->ndirs; i-- > 0;)
		root_rmdir(u->dirs[i].path);
	errno = saved;
	va_list ap;
	va_start(ap, fmt);
	char *m = xvasprintf(fmt, ap);
	va_end(ap);
	die("%s: %s; nothing was changed", u->pkg, m);
}

static int parent_dir(unpack *u, const char *path, const char **name)
{
	int dfd = root_parent(path, name);
	if (dfd < 0 && errno == ENOENT) { /* no directory entry in the archive: made like tar does */
		char *dir = path_dir(path);
		dfd = root_mkdirs(strcmp(dir, ".") ? dir : "", 0755);
		free(dir);
		root_parent(path, name);
	}
	if (dfd < 0)
		unpack_fail(u, "cannot open the directory of /%s: %s", path, strerror(errno));
	return dfd;
}

static staged *stage_push(unpack *u, const char *path, char *tmp, int type)
{
	u->st = xrealloc(u->st, (u->nst + 1) * sizeof *u->st);
	staged *s = &u->st[u->nst];
	s->path = xstrdup(path);
	s->tmp = tmp;
	s->type = type;
	sm_put(&u->index, path, u->nst);
	u->nst++;
	return s;
}

static void set_times(int dfd, const char *name, int64_t mtime)
{
	struct timespec ts[2] = {{(time_t)mtime, 0}, {(time_t)mtime, 0}};
	utimensat(dfd, name, ts, AT_SYMLINK_NOFOLLOW);
}

static void stage_dir(unpack *u, const tar_entry *e)
{
	struct stat st;
	if (root_lstat(e->path, &st)) {
		if (S_ISDIR(st.st_mode))
			return; /* an existing directory keeps its owner, mode and times */
		if (S_ISLNK(st.st_mode) && root_stat(e->path, &st) && S_ISDIR(st.st_mode))
			return; /* a symlink to a directory stays (merged /usr in the root) */
		if (root_unlink(e->path) < 0)
			unpack_fail(u, "cannot replace /%s with a directory: %s", e->path, strerror(errno));
	}
	const char *name;
	int dfd = parent_dir(u, e->path, &name);
	/* owner rwx until the end: the files of the directory go in first */
	if (mkdirat(dfd, name, 0700) < 0)
		unpack_fail(u, "cannot create /%s: %s", e->path, strerror(errno));
	u->dirs = xrealloc(u->dirs, (u->ndirs + 1) * sizeof *u->dirs);
	u->dirs[u->ndirs++] = (new_dir){xstrdup(e->path), e->mode, e->uid, e->gid, e->mtime};
}

static void stage_file(unpack *u, tar_reader *t, const tar_entry *e)
{
	const char *name;
	int dfd = parent_dir(u, e->path, &name);
	char *tmp = tmp_name();
	int fd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		free(tmp);
		unpack_fail(u, "cannot write /%s: %s", e->path, strerror(errno));
	}
	stage_push(u, e->path, tmp, TAR_FILE);
	static char buf[1 << 17];
	char *err = NULL;
	ssize_t n;
	while ((n = tar_read(t, buf, sizeof buf, &err)) > 0)
		if (write_all(fd, buf, (size_t)n) < 0)
			unpack_fail(u, "cannot write /%s: %s", e->path, strerror(errno));
	if (n < 0)
		unpack_fail(u, "%s", err);
	if (u->as_root && fchown(fd, e->uid, e->gid) < 0)
		unpack_fail(u, "cannot change the owner of /%s: %s", e->path, strerror(errno));
	if (fchmod(fd, e->mode) < 0)
		unpack_fail(u, "cannot change the mode of /%s: %s", e->path, strerror(errno));
	struct timespec ts[2] = {{(time_t)e->mtime, 0}, {(time_t)e->mtime, 0}};
	futimens(fd, ts);
	if (close(fd) < 0)
		unpack_fail(u, "cannot write /%s: %s", e->path, strerror(errno));
}

static void stage_symlink(unpack *u, const tar_entry *e)
{
	const char *name;
	int dfd = parent_dir(u, e->path, &name);
	char *tmp = tmp_name();
	if (symlinkat(e->link, dfd, tmp) < 0) {
		free(tmp);
		unpack_fail(u, "cannot create /%s: %s", e->path, strerror(errno));
	}
	stage_push(u, e->path, tmp, TAR_SYMLINK);
	if (u->as_root && fchownat(dfd, tmp, e->uid, e->gid, AT_SYMLINK_NOFOLLOW) < 0)
		unpack_fail(u, "cannot change the owner of /%s: %s", e->path, strerror(errno));
	set_times(dfd, tmp, e->mtime);
}

static void stage_hardlink(unpack *u, const tar_entry *e)
{
	size_t i;
	if (!sm_get(&u->index, e->link, &i) || u->st[i].type == TAR_SYMLINK)
		unpack_fail(u, "/%s is a hard link to /%s, which is no file of the package", e->path, e->link);
	const char *tname, *name;
	int tfd = root_parent(u->st[i].path, &tname);
	int dfd = parent_dir(u, e->path, &name);
	tfd = root_parent(u->st[i].path, &tname); /* still cached: the LRU keeps both */
	char *tmp = tmp_name();
	if (tfd < 0 || linkat(tfd, u->st[i].tmp, dfd, tmp, 0) < 0) {
		free(tmp);
		unpack_fail(u, "cannot link /%s: %s", e->path, strerror(errno));
	}
	stage_push(u, e->path, tmp, TAR_HARDLINK);
}

static void stage_node(unpack *u, const tar_entry *e)
{
	const char *name;
	int dfd = parent_dir(u, e->path, &name);
	char *tmp = tmp_name();
	mode_t kind = e->type == TAR_FIFO ? S_IFIFO : e->type == TAR_CHAR ? S_IFCHR : S_IFBLK;
	if (mknodat(dfd, tmp, kind | 0600, makedev(e->major, e->minor)) < 0) {
		free(tmp);
		unpack_fail(u, "cannot create /%s: %s", e->path, strerror(errno));
	}
	stage_push(u, e->path, tmp, e->type);
	if (u->as_root && fchownat(dfd, tmp, e->uid, e->gid, AT_SYMLINK_NOFOLLOW) < 0)
		unpack_fail(u, "cannot change the owner of /%s: %s", e->path, strerror(errno));
	if (fchmodat(dfd, tmp, e->mode, 0) < 0)
		unpack_fail(u, "cannot change the mode of /%s: %s", e->path, strerror(errno));
	set_times(dfd, tmp, e->mtime);
}

/* phase 2: one rename per file. backup=() files that exist and differ stay; the new version
 * goes to NAME.burnnew next to them. */
static void commit(unpack *u, const strmap *backup, strmap *kept)
{
	for (size_t i = 0; i < u->nst; i++) {
		staged *s = &u->st[i];
		const char *name;
		int dfd = root_parent(s->path, &name);
		if (dfd < 0)
			die_errno("%s: cannot open the directory of /%s", u->pkg, s->path);
		struct stat st;
		if (s->type == TAR_FILE && sm_has(backup, s->path) && root_stat(s->path, &st)) {
			int fd = openat(dfd, s->tmp, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
			bool same = fd >= 0 && root_same_content(s->path, fd);
			if (fd >= 0)
				close(fd);
			dfd = root_parent(s->path, &name);
			if (!same) {
				char *nn = xasprintf("%s.burnnew", name);
				if (renameat(dfd, s->tmp, dfd, nn) < 0)
					die_errno("%s: cannot write /%s.burnnew", u->pkg, s->path);
				free(nn);
				sm_put(kept, s->path, 1);
				continue;
			}
		}
		if (renameat(dfd, s->tmp, dfd, name) == 0)
			continue;
		/* a directory where the package has a file: an empty one gives way, like with tar */
		if ((errno == EISDIR || errno == ENOTEMPTY || errno == EEXIST) && root_rmdir(s->path) == 0) {
			dfd = root_parent(s->path, &name);
			if (dfd >= 0 && renameat(dfd, s->tmp, dfd, name) == 0)
				continue;
		}
		die_errno("%s: cannot put /%s in place", u->pkg, s->path);
	}
	/* new directories get their mode and times last, the deepest first */
	for (size_t i = u->ndirs; i-- > 0;) {
		new_dir *d = &u->dirs[i];
		const char *name;
		int dfd = root_parent(d->path, &name);
		int fd = dfd < 0 ? -1 : openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (u->as_root && fchown(fd, d->uid, d->gid) < 0)
			warn("cannot change the owner of /%s: %s", d->path, strerror(errno));
		if (fchmod(fd, d->mode) < 0)
			warn("cannot change the mode of /%s: %s", d->path, strerror(errno));
		struct timespec ts[2] = {{(time_t)d->mtime, 0}, {(time_t)d->mtime, 0}};
		futimens(fd, ts);
		close(fd);
	}
}

static void unpack_free(unpack *u)
{
	for (size_t i = 0; i < u->nst; i++) {
		free(u->st[i].path);
		free(u->st[i].tmp);
	}
	for (size_t i = 0; i < u->ndirs; i++)
		free(u->dirs[i].path);
	free(u->st);
	free(u->dirs);
	sm_free(&u->index);
}

static void unpack_package(const char *name, const pkg_info *p, const svec *backups)
{
	root_init();
	unpack u = {.pkg = name, .as_root = geteuid() == 0};
	char *err = NULL;
	tar_reader *t = tar_open(p->path, &err);
	if (!t)
		die("%s", err);
	const tar_entry *e;
	int r;
	while ((r = tar_next(t, &e, &err)) > 0) {
		if (!*e->path || is_burn_path(e->path))
			continue;
		switch (e->type) {
		case TAR_DIR:
			stage_dir(&u, e);
			break;
		case TAR_FILE:
			stage_file(&u, t, e);
			break;
		case TAR_SYMLINK:
			stage_symlink(&u, e);
			break;
		case TAR_HARDLINK:
			stage_hardlink(&u, e);
			break;
		default:
			stage_node(&u, e);
			break;
		}
	}
	if (r < 0)
		unpack_fail(&u, "%s", err);
	tar_close(t);
	strmap bset = STRMAP_INIT, kept = STRMAP_INIT;
	for (size_t i = 0; i < backups->n; i++) {
		char *b = tar_normalize(backups->v[i]);
		if (b)
			sm_put(&bset, b, i);
		free(b);
	}
	commit(&u, &bset, &kept);
	for (size_t i = 0; i < backups->n; i++) {
		char *b = tar_normalize(backups->v[i]);
		if (b && sm_has(&kept, b))
			warn("kept your /%s; the new default is /%s.burnnew", backups->v[i], backups->v[i]);
		free(b);
	}
	sm_free(&bset);
	sm_free(&kept);
	unpack_free(&u);
}

/* ---- upgrade: files the new version dropped ---- */

/* every line of FILE over all database entries ($DB_DIR/<any>/FILE) */
static void db_all_lines(const char *file, svec *out)
{
	svec names = SVEC_INIT;
	glob_paths(B.db_dir, "*", &names);
	for (size_t i = 0; i < names.n; i++) {
		char *f = xasprintf("%s/%s", names.v[i], file);
		file_lines(f, out);
		free(f);
	}
	sv_free(&names);
}

/* Files of the old version that the new one dropped are deleted, except shared libraries that
 * other installed packages still link against ("preserved libs", rebuilt later). */
static void remove_obsolete(const svec *oldf, const svec *newf, svec *preserved)
{
	strmap now = STRMAP_INIT, needed = STRMAP_INIT;
	for (size_t i = 0; i < newf->n; i++)
		sm_put(&now, newf->v[i], 0);
	svec old = SVEC_INIT, obsolete = SVEC_INIT, keep = SVEC_INIT, dirs = SVEC_INIT, needs = SVEC_INIT;
	sv_extend(&old, oldf);
	sv_sort(&old);
	for (size_t i = 0; i < old.n; i++)
		if (!sm_has(&now, old.v[i]))
			sv_push(&obsolete, old.v[i]);
	db_all_lines("needs-so", &needs);
	for (size_t i = 0; i < needs.n; i++)
		sm_put(&needed, needs.v[i], 0);
	for (size_t i = 0; i < obsolete.n; i++) {
		const char *f = obsolete.v[i];
		if (!*f || str_suffix(f, "/") || !strstr(path_base(f), ".so.") || !sm_has(&needed, path_base(f)))
			continue;
		sv_push(&keep, f);
		char *t = root_readlink(f);
		if (t) {
			const char *slash = strrchr(f, '/');
			if (t[0] == '/')
				sv_push(&keep, t + 1);
			else
				sv_pushf(&keep, "%.*s/%s", slash ? (int)(slash - f) : (int)strlen(f), f, t);
			free(t);
		}
	}
	for (size_t i = 0; i < obsolete.n; i++) {
		const char *f = obsolete.v[i];
		if (!*f)
			continue;
		if (str_suffix(f, "/")) {
			sv_push(&dirs, f);
			continue;
		}
		if (sv_has(&keep, f)) {
			warn("preserving /%s: still needed by installed packages", f);
			sv_push(preserved, f);
			continue;
		}
		root_unlink(f);
	}
	sv_sort(&dirs);
	for (size_t i = dirs.n; i-- > 0;) {
		dirs.v[i][strlen(dirs.v[i]) - 1] = 0;
		root_rmdir(dirs.v[i]);
	}
	sv_free(&old);
	sv_free(&obsolete);
	sv_free(&keep);
	sv_free(&dirs);
	sv_free(&needs);
	sm_free(&now);
	sm_free(&needed);
}

/* ---- the database entry ---- */

static void db_replace(const char *name, const pkg_info *p, const sbuf *preserved, bool with_preserved)
{
	if (mkdir_p(B.db_dir, 0755) < 0)
		die_errno("cannot create %s", B.db_dir);
	char *tmp = xasprintf("%s/.%s.burn-new", B.db_dir, name);
	char *dst = db_path(name, "");
	dst[strlen(dst) - 1] = 0;
	rm_rf(tmp);
	if (mkdir(tmp, 0755) < 0)
		die_errno("cannot create %s", tmp);
	for (size_t i = 0; i < p->nburn; i++) {
		if (!strcmp(p->burn[i].name, "preserved"))
			continue;
		char *f = xasprintf("%s/%s", tmp, p->burn[i].name);
		if (file_write(f, p->burn[i].data, p->burn[i].len, 0644) < 0)
			die_errno("cannot write %s", f);
		struct timespec ts[2] = {{(time_t)p->burn[i].mtime, 0}, {(time_t)p->burn[i].mtime, 0}};
		utimensat(AT_FDCWD, f, ts, 0); /* the times of the package, like cp -a */
		free(f);
	}
	if (with_preserved) {
		char *f = xasprintf("%s/preserved", tmp);
		if (file_write(f, preserved->s ? preserved->s : "", preserved->len, 0644) < 0)
			die_errno("cannot write %s", f);
		free(f);
	}
	if (exists(dst)) {
		if (renameat2(AT_FDCWD, tmp, AT_FDCWD, dst, RENAME_EXCHANGE) < 0) {
			char *old = xasprintf("%s/.%s.burn-old", B.db_dir, name);
			rm_rf(old);
			if (rename(dst, old) < 0 || rename(tmp, dst) < 0)
				die_errno("cannot replace %s", dst);
			rm_rf(old);
			free(old);
		} else {
			rm_rf(tmp); /* the old entry, after the exchange */
		}
	} else if (rename(tmp, dst) < 0) {
		die_errno("cannot create %s", dst);
	}
	free(tmp);
	free(dst);
}

/* ---- install ---- */

static void check_conflicts(const char *name, const svec *files)
{
	strmap want = STRMAP_INIT;
	for (size_t i = 0; i < files->n; i++)
		if (!str_suffix(files->v[i], "/"))
			sm_put(&want, files->v[i], 0);
	svec pkgs = SVEC_INIT;
	db_list(&pkgs);
	sbuf out = SBUF_INIT;
	size_t found = 0;
	for (size_t i = 0; i < pkgs.n; i++) {
		if (!strcmp(pkgs.v[i], name))
			continue;
		svec theirs = SVEC_INIT;
		db_lines(pkgs.v[i], "files", &theirs);
		for (size_t j = 0; j < theirs.n; j++)
			if (sm_has(&want, theirs.v[j]) && ++found <= 20)
				sb_addf(&out, "  /%s (owned by %s)\n", theirs.v[j], pkgs.v[i]);
		sv_free(&theirs);
	}
	if (found) {
		fputs(sb_str(&out), stderr);
		if (!B.force)
			die("%s: file conflicts with installed packages (use --force to overwrite)", name);
	}
	sb_free(&out);
	sv_free(&pkgs);
	sm_free(&want);
}

void pkg_install_file(const char *file, const char *op)
{
	pkg_info p;
	pkg_read(file, &p);
	const char *name = meta_get(&p.meta, "name");
	if (!*name)
		die("%s is not a burn package", file);
	char *ver = xasprintf("%s-%s", meta_get(&p.meta, "version"), meta_get(&p.meta, "release"));
	svec om = SVEC_INIT;
	char *old = NULL, *old_file = NULL;
	if (db_has(name)) {
		db_lines(name, "meta", &om);
		old = xasprintf("%s-%s", meta_get(&om, "version"), meta_get(&om, "release"));
		old_file = xasprintf("%s/packages/%s-%s-%s-%s.pkg.tar.zst", cfg("CACHE_DIR"), name, old,
				     meta_get(&om, "arch"), meta_get(&om, "buildid"));
		if (!strcmp(meta_get(&om, "buildid"), meta_get(&p.meta, "buildid")) && !B.force && !B.rebuild) {
			msg2("%s %s is already installed", name, ver);
			goto out;
		}
	}
	check_conflicts(name, &p.files);
	if (!old)
		msg("Installing %s %s", name, ver);
	else if (!strcmp(old, ver))
		msg("Rebuilding %s %s", name, ver);
	else if (vercmp(old, ver) == 1)
		msg("Downgrading %s %s -> %s", name, old, ver);
	else
		msg("Upgrading %s %s -> %s", name, old, ver);
	svec backups = SVEC_INIT;
	sv_split_ws(&backups, meta_get(&p.meta, "backup"));
	unpack_package(name, &p, &backups);
	sv_free(&backups);

	sbuf preserved = SBUF_INIT;
	bool with_preserved = false;
	const burn_file *own = burn_get(&p, "preserved");
	if (own) {
		sb_addn(&preserved, own->data, own->len);
		with_preserved = true;
	}
	if (old) {
		svec oldf = SVEC_INIT, kept = SVEC_INIT, oldprov = SVEC_INIT;
		db_lines(name, "files", &oldf);
		remove_obsolete(&oldf, &p.files, &kept);
		for (size_t i = 0; i < kept.n; i++)
			sb_addf(&preserved, "%s\n", kept.v[i]);
		with_preserved |= kept.n > 0;
		/* earlier preserved files stay preserved, unless this version ships them again */
		char *op_path = db_path(name, "preserved");
		if (is_file(op_path)) {
			svec was = SVEC_INIT;
			strmap now = STRMAP_INIT;
			file_lines(op_path, &was);
			for (size_t i = 0; i < p.files.n; i++)
				sm_put(&now, p.files.v[i], 0);
			for (size_t i = 0; i < was.n; i++)
				if (!sm_has(&now, was.v[i]))
					sb_addf(&preserved, "%s\n", was.v[i]);
			with_preserved = true;
			sm_free(&now);
			sv_free(&was);
		}
		free(op_path);
		/* sonames the old version provided and the new one does not (comm -23) */
		db_lines(name, "provides-so", &oldprov);
		strmap prov = STRMAP_INIT;
		for (size_t i = 0; i < p.provides.n; i++)
			sm_put(&prov, p.provides.v[i], 0);
		for (size_t i = 0; i < oldprov.n; i++)
			if (!sm_has(&prov, oldprov.v[i]))
				sv_push(&txn_lost, oldprov.v[i]);
		sm_free(&prov);
		sv_free(&oldf);
		sv_free(&kept);
		sv_free(&oldprov);
	}
	/* the files are on disk before the database says so */
	int rfd = root_dir("");
	if (rfd >= 0) {
		int fd = openat(rfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (fd >= 0) {
			syncfs(fd);
			close(fd);
		}
	}
	db_replace(name, &p, &preserved, with_preserved);
	sb_free(&preserved);
	if (old)
		history_add(op ? op : "upgrade", name, old, ver, old_file);
	else
		history_add(op ? op : "install", name, "-", ver, "");
	triggers_note(&p.files, true);
out:
	sv_free(&om);
	free(old);
	free(old_file);
	free(ver);
	pkg_free(&p);
}

/* ---- remove ---- */

/* installed packages that depend on NAME */
static char *db_rdeps(const char *name)
{
	svec pkgs = SVEC_INIT, out = SVEC_INIT;
	db_list(&pkgs);
	for (size_t i = 0; i < pkgs.n; i++) {
		if (!strcmp(pkgs.v[i], name))
			continue;
		char *deps = db_get(pkgs.v[i], "depends");
		svec d = SVEC_INIT;
		sv_split_ws(&d, deps ? deps : "");
		if (sv_has(&d, name))
			sv_push(&out, pkgs.v[i]);
		sv_free(&d);
		free(deps);
	}
	char *s = sv_join(&out, " ");
	sv_free(&pkgs);
	sv_free(&out);
	return s;
}

void pkg_remove(const char *name)
{
	if (!db_has(name))
		die("%s is not installed", name);
	svec essential = SVEC_INIT;
	cfg_words("ESSENTIAL", &essential);
	if (sv_has(&essential, name) && !B.force)
		die("%s is essential (use --force)", name);
	sv_free(&essential);
	char *rdeps = db_rdeps(name);
	if (*rdeps && !B.force)
		die("%s is required by: %s", name, rdeps);
	free(rdeps);
	root_init();
	svec meta = SVEC_INIT, files = SVEC_INIT, backups = SVEC_INIT, dirs = SVEC_INIT;
	db_lines(name, "meta", &meta);
	db_lines(name, "files", &files);
	char *ver = xasprintf("%s-%s", meta_get(&meta, "version"), meta_get(&meta, "release"));
	msg("Removing %s %s", name, ver);
	sv_split_ws(&backups, meta_get(&meta, "backup"));
	for (size_t i = 0; i < files.n; i++) {
		const char *f = files.v[i];
		struct stat st;
		if (!*f)
			continue;
		if (str_suffix(f, "/")) {
			sv_push(&dirs, f);
		} else if (sv_has(&backups, f) && root_stat(f, &st)) {
			char *save = xasprintf("%s.burnsave", f);
			if (root_rename(f, save) < 0)
				warn("cannot save /%s: %s", f, strerror(errno));
			else
				warn("saved /%s as /%s", f, save);
			free(save);
		} else {
			root_unlink(f);
		}
	}
	/* the old libraries kept for others go with the package */
	svec pres = SVEC_INIT;
	db_lines(name, "preserved", &pres);
	for (size_t i = 0; i < pres.n; i++)
		if (*pres.v[i])
			root_unlink(pres.v[i]);
	sv_free(&pres);
	sv_sort(&dirs);
	for (size_t i = dirs.n; i-- > 0;) {
		dirs.v[i][strlen(dirs.v[i]) - 1] = 0;
		root_rmdir(dirs.v[i]);
	}
	history_add("remove", name, ver, "-", "");
	char *entry = db_path(name, "");
	if (rm_rf(entry) < 0)
		die_errno("cannot remove %s", entry);
	free(entry);
	triggers_note(&files, false);
	free(ver);
	sv_free(&meta);
	sv_free(&files);
	sv_free(&backups);
	sv_free(&dirs);
}

/* ---- preserved libraries ---- */

/* add F, and what it links to, to KEEP */
static void keep_with_target(strmap *keep, const char *f)
{
	sm_put(keep, f, 0);
	char *t = root_readlink(f);
	if (!t)
		return;
	if (t[0] == '/') {
		sm_put(keep, t + 1, 0);
	} else {
		const char *slash = strrchr(f, '/');
		char *k = xasprintf("%.*s/%s", slash ? (int)(slash - f) : (int)strlen(f), f, t);
		sm_put(keep, k, 0);
		free(k);
	}
	free(t);
}

/* Preserved libraries that no installed package links against any more are deleted. A needed
 * soname keeps the file its symlink points to as well. */
void preserved_cleanup(void)
{
	svec pkgs = SVEC_INIT, needs = SVEC_INIT;
	strmap needed = STRMAP_INIT, keep = STRMAP_INIT;
	db_list(&pkgs);
	db_all_lines("needs-so", &needs);
	for (size_t i = 0; i < needs.n; i++)
		sm_put(&needed, needs.v[i], 0);
	bool any = false;
	for (size_t i = 0; i < pkgs.n; i++) {
		svec pres = SVEC_INIT;
		db_lines(pkgs.v[i], "preserved", &pres);
		for (size_t j = 0; j < pres.n; j++) {
			any = true;
			if (*pres.v[j] && sm_has(&needed, path_base(pres.v[j])))
				keep_with_target(&keep, pres.v[j]);
		}
		sv_free(&pres);
	}
	for (size_t i = 0; any && i < pkgs.n; i++) {
		svec pres = SVEC_INIT;
		db_lines(pkgs.v[i], "preserved", &pres);
		if (!pres.n) {
			sv_free(&pres);
			continue;
		}
		sbuf out = SBUF_INIT;
		bool changed = false;
		for (size_t j = 0; j < pres.n; j++) {
			const char *f = pres.v[j];
			if (!*f) {
				changed = true;
			} else if (sm_has(&keep, f)) {
				sb_addf(&out, "%s\n", f);
			} else {
				if (root_unlink(f) < 0 && errno != ENOENT)
					warn("cannot remove /%s: %s", f, strerror(errno));
				msg2("removed preserved /%s", f);
				changed = true;
			}
		}
		if (changed) {
			char *path = db_path(pkgs.v[i], "preserved");
			if (file_write(path, out.s ? out.s : "", out.len, 0644) < 0)
				die_errno("cannot write %s", path);
			free(path);
		}
		sb_free(&out);
		sv_free(&pres);
	}
	sv_free(&pkgs);
	sv_free(&needs);
	sm_free(&needed);
	sm_free(&keep);
}
