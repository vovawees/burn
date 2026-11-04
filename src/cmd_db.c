/* SPDX-License-Identifier: MIT */
/* Commands on the installed-package database: list, files, owner, hold, unhold, history, doctor. */
#include "burn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int cmd_list(int argc, char **argv)
{
	svec pkgs = SVEC_INIT;
	db_list(&pkgs);
	for (size_t i = 0; i < pkgs.n; i++) {
		const char *p = pkgs.v[i];
		svec meta = SVEC_INIT;
		db_lines(p, "meta", &meta);
		char *ver = xasprintf("%s-%s", meta_get(&meta, "version"), meta_get(&meta, "release"));
		const char *held = is_held(p) ? " [held]" : "";
		if (B.verbose)
			printf("%-24s %-18s %-16s %s%s\n", p, ver, meta_get(&meta, "profile"),
			       meta_get(&meta, "features"), held);
		else
			printf("%-24s %s%s\n", p, ver, held);
		free(ver);
		sv_free(&meta);
	}
	sv_free(&pkgs);
	return 0;
}

int cmd_files(int argc, char **argv)
{
	if (argc != 1)
		die("usage: burn files <pkg>");
	if (!db_has(argv[0]))
		die("%s is not installed", argv[0]);
	svec files = SVEC_INIT;
	db_lines(argv[0], "files", &files);
	for (size_t i = 0; i < files.n; i++)
		if (!str_suffix(files.v[i], "/"))
			printf("/%s\n", files.v[i]);
	sv_free(&files);
	return 0;
}

/* the merged /usr spelling of a path: bin, sbin, usr/sbin -> usr/bin; lib, lib64, usr/lib64 -> usr/lib */
static char *usr_merged(const char *p)
{
	static const char *const bins[] = {"bin/", "sbin/", "usr/sbin/"};
	static const char *const libs[] = {"lib/", "lib64/", "usr/lib64/"};
	char *q = xstrdup(p);
	for (size_t i = 0; i < ARRAY_LEN(bins); i++)
		if (str_prefix(q, bins[i])) {
			char *n = xasprintf("usr/bin/%s", q + strlen(bins[i]));
			free(q);
			q = n;
			break;
		}
	for (size_t i = 0; i < ARRAY_LEN(libs); i++)
		if (str_prefix(q, libs[i])) {
			char *n = xasprintf("usr/lib/%s", q + strlen(libs[i]));
			free(q);
			q = n;
			break;
		}
	return q;
}

int cmd_owner(int argc, char **argv)
{
	if (argc != 1)
		die("usage: burn owner <path>");
	char *abs = path_abs(argv[0]);
	const char *p = abs;
	size_t rl = strlen(B.R);
	if (rl && !strncmp(p, B.R, rl) && p[rl] == '/')
		p += rl;
	while (*p == '/')
		p++;
	char *q = usr_merged(p);
	svec pkgs = SVEC_INIT;
	db_list(&pkgs);
	bool found = false;
	for (size_t i = 0; i < pkgs.n; i++) {
		svec files = SVEC_INIT;
		db_lines(pkgs.v[i], "files", &files);
		if (sv_has(&files, p) || sv_has(&files, q)) {
			svec meta = SVEC_INIT;
			db_lines(pkgs.v[i], "meta", &meta);
			printf("/%s is owned by %s %s-%s\n", q, pkgs.v[i], meta_get(&meta, "version"),
			       meta_get(&meta, "release"));
			sv_free(&meta);
			found = true;
		}
		sv_free(&files);
	}
	if (!found)
		die("no package owns /%s", p);
	sv_free(&pkgs);
	free(q);
	free(abs);
	return 0;
}

int cmd_hold(int argc, char **argv)
{
	if (!argc) {
		size_t len;
		char *d = file_read(B.hold_file, &len);
		if (d)
			fwrite(d, 1, len, stdout);
		free(d);
		return 0;
	}
	need_write();
	char *dir = path_dir(B.hold_file);
	if (mkdir_p(dir, 0755) < 0)
		die_errno("cannot create %s", dir);
	free(dir);
	size_t len = 0;
	char *old = file_read(B.hold_file, &len);
	sbuf add = SBUF_INIT;
	if (old && len && old[len - 1] != '\n')
		sb_addc(&add, '\n'); /* a hand-edited file without the last newline */
	svec held = SVEC_INIT;
	file_lines(B.hold_file, &held);
	for (int i = 0; i < argc; i++) {
		if (!sv_has(&held, argv[i])) {
			sb_addf(&add, "%s\n", argv[i]);
			sv_push(&held, argv[i]);
		}
	}
	if (file_append(B.hold_file, sb_str(&add)) < 0)
		die_errno("cannot write %s", B.hold_file);
	for (int i = 0; i < argc; i++)
		msg2("held %s (update will skip it)", argv[i]);
	sv_free(&held);
	sb_free(&add);
	free(old);
	return 0;
}

int cmd_unhold(int argc, char **argv)
{
	if (!argc)
		die("usage: burn unhold <pkg>...");
	need_write();
	if (!is_file(B.hold_file))
		return 0;
	svec held = SVEC_INIT;
	file_lines(B.hold_file, &held);
	sbuf out = SBUF_INIT;
	for (size_t i = 0; i < held.n; i++) {
		bool drop = false;
		for (int j = 0; j < argc && !drop; j++)
			drop = !strcmp(held.v[i], argv[j]);
		if (!drop)
			sb_addf(&out, "%s\n", held.v[i]);
	}
	if (file_write(B.hold_file, sb_str(&out), out.len, 0644) < 0)
		die_errno("cannot write %s", B.hold_file);
	for (int i = 0; i < argc; i++)
		msg2("released %s", argv[i]);
	sb_free(&out);
	sv_free(&held);
	return 0;
}

/* history: date|operation|name|old version|new version|previous package file */
int cmd_history(int argc, char **argv)
{
	char *path = xasprintf("%s/history", B.state_dir);
	if (!is_file(path)) {
		msg("no transactions yet");
		free(path);
		return 0;
	}
	svec lines = SVEC_INIT;
	if (file_lines(path, &lines) < 0)
		die_errno("cannot read %s", path);
	for (size_t i = 0; i < lines.n; i++) {
		const char *f[5] = {"", "", "", "", ""};
		char *s = lines.v[i];
		for (int k = 0; k < 5 && s; k++) {
			f[k] = s;
			s = strchr(s, '|');
			if (s)
				*s++ = 0;
		}
		printf("%s  %-9s %-22s %s -> %s\n", f[0], f[1], f[2], f[3], f[4]);
	}
	sv_free(&lines);
	free(path);
	return 0;
}

static bool size_nonzero(const char *name, const char *file)
{
	char *p = db_path(name, file);
	struct stat st;
	bool ok = stat(p, &st) == 0 && st.st_size > 0;
	free(p);
	return ok;
}

/* the library is in the root (or, with HOST_DEPS, on the host) without a package owning it */
static bool lib_exists(const char *so)
{
	const char *carch = cfg("CARCH");
	svec dirs = SVEC_INIT;
	sv_pushf(&dirs, "%s/usr/lib", B.R);
	sv_pushf(&dirs, "%s/usr/lib/%s-linux-gnu", B.R, carch);
	if (cfg_on("HOST_DEPS")) {
		static const char *const host[] = {"/usr/lib", "/usr/lib64", "/lib", "/lib64"};
		for (size_t i = 0; i < ARRAY_LEN(host); i++)
			sv_push(&dirs, host[i]);
		sv_pushf(&dirs, "/usr/lib/%s-linux-gnu", host_arch());
		sv_pushf(&dirs, "/lib/%s-linux-gnu", host_arch());
	}
	bool ok = false;
	for (size_t i = 0; i < dirs.n && !ok; i++) {
		char *p = xasprintf("%s/%s", dirs.v[i], so);
		struct stat st;
		ok = stat(p, &st) == 0;
		free(p);
	}
	sv_free(&dirs);
	return ok;
}

/* Every SONAME a package links against is provided by an installed package (or present in the
 * root), every package it depends on is installed; preserved old libraries are reported until the
 * packages using them are rebuilt. */
int cmd_doctor(int argc, char **argv)
{
	svec pkgs = SVEC_INIT, prov = SVEC_INIT;
	db_list(&pkgs);
	for (size_t i = 0; i < pkgs.n; i++)
		db_lines(pkgs.v[i], "provides-so", &prov);
	sv_sort_uniq(&prov);
	int problems = 0;
	for (size_t i = 0; i < pkgs.n; i++) {
		if (!size_nonzero(pkgs.v[i], "needs-so"))
			continue;
		svec needs = SVEC_INIT;
		db_lines(pkgs.v[i], "needs-so", &needs);
		for (size_t j = 0; j < needs.n; j++) {
			const char *so = needs.v[j];
			if (!*so || sv_bsearch(&prov, so) || lib_exists(so))
				continue;
			warn("%s needs %s, but no installed package provides it", pkgs.v[i], so);
			problems++;
		}
		sv_free(&needs);
	}
	/* what every package depends on is installed (update added no new dependencies before 0.9.1) */
	for (size_t i = 0; i < pkgs.n; i++) {
		char *d = db_get(pkgs.v[i], "depends");
		svec w = SVEC_INIT;
		sv_split_ws(&w, d ? d : "");
		for (size_t j = 0; j < w.n; j++) {
			if (dep_satisfied(w.v[j]))
				continue;
			warn("%s depends on %s, which is not installed (burn install %s)", pkgs.v[i], w.v[j], w.v[j]);
			problems++;
		}
		sv_free(&w);
		free(d);
	}
	for (size_t i = 0; i < pkgs.n; i++) {
		if (!size_nonzero(pkgs.v[i], "preserved"))
			continue;
		svec kept = SVEC_INIT;
		db_lines(pkgs.v[i], "preserved", &kept);
		char *list = sv_join(&kept, " ");
		warn("%s: preserved old libraries: %s", pkgs.v[i], list);
		free(list);
		sv_free(&kept);
		problems++;
	}
	int rc = 0;
	if (problems) {
		warn("doctor: %d problem(s)", problems);
		rc = 1;
	} else {
		msg("doctor: no problems found (%zu packages)", pkgs.n);
	}
	sv_free(&prov);
	sv_free(&pkgs);
	return rc;
}
