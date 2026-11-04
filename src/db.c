/* SPDX-License-Identifier: MIT */
/*
 * The installed-package database: $ROOT/var/lib/burn/installed/<name>/ with
 *   meta         KEY=value lines (name version release arch buildid depends features backup ...)
 *   files        every path of the package relative to the root, directories end with '/'
 *   provides-so  the SONAMEs it ships, needs-so the ones it links against, preserved: old
 *                libraries kept for other packages.
 *   backup-sums  "SHA256 PATH": what each backup=() file held in this version (untouched or changed)
 * Plus $ROOT/etc/burn/hold and $ROOT/var/lib/burn/state/history.
 */
#include "burn.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

bool db_has(const char *name)
{
	char *p = xasprintf("%s/%s/meta", B.db_dir, name);
	bool ok = is_file(p);
	free(p);
	return ok;
}

/* every installed package in byte order (the prototype globs $DB_DIR/<name>/meta) */
void db_list(svec *out)
{
	DIR *d = opendir(B.db_dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char *m = xasprintf("%s/%s/meta", B.db_dir, e->d_name);
		if (exists(m))
			sv_push(out, e->d_name);
		free(m);
	}
	closedir(d);
	sv_sort(out);
}

char *db_path(const char *name, const char *file)
{
	return xasprintf("%s/%s/%s", B.db_dir, name, file);
}

/* the lines of one database file of the package; a missing file has none */
void db_lines(const char *name, const char *file, svec *out)
{
	char *p = db_path(name, file);
	if (file_lines(p, out) < 0)
		warn("cannot read %s: %s", p, strerror(errno));
	free(p);
}

const char *meta_get(const svec *meta, const char *key)
{
	size_t k = strlen(key);
	for (size_t i = 0; i < meta->n; i++)
		if (!strncmp(meta->v[i], key, k) && meta->v[i][k] == '=')
			return meta->v[i] + k + 1;
	return "";
}

/* the first KEY=value line of the package's meta */
char *db_get(const char *name, const char *key)
{
	svec meta = SVEC_INIT;
	db_lines(name, "meta", &meta);
	size_t k = strlen(key);
	char *val = NULL;
	for (size_t i = 0; i < meta.n && !val; i++)
		if (!strncmp(meta.v[i], key, k) && meta.v[i][k] == '=')
			val = xstrdup(meta.v[i] + k + 1);
	sv_free(&meta);
	return val;
}

bool is_held(const char *name)
{
	svec lines = SVEC_INIT;
	file_lines(B.hold_file, &lines);
	bool held = sv_has(&lines, name);
	sv_free(&lines);
	return held;
}

/* Commands that change ROOT: root privileges for / and one burn at a time. The lock is the
 * prototype's ($R/var/lib/burn/lock, flock), so the two exclude each other as well. */
void need_write(void)
{
	static int lock_fd = -1;
	if (lock_fd >= 0)
		return;
	if (!*B.R && geteuid() != 0)
		die("this command changes the system: run it as root");
	char *dir = xasprintf("%s/var/lib/burn", B.R);
	if (mkdir_p(dir, 0755) < 0)
		die_errno("cannot create %s", dir);
	char *lock = xasprintf("%s/lock", dir);
	lock_fd = open(lock, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	if (lock_fd < 0)
		die_errno("cannot open %s", lock);
	if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0)
		die("another burn process is working on %s", B.root);
	free(lock);
	free(dir);
}
