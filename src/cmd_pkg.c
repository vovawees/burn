/* SPDX-License-Identifier: MIT */
/* Commands that change the packages of the root: install (recipes and package files), remove,
 * rollback. */
#include "burn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* build NAMES (in this order) and install each package right away: what comes later may need it */
static void build_install(const svec *names, const char *op)
{
	prefetch(names);
	for (size_t i = 0; i < names->n; i++) {
		triggers_run(); /* ldconfig for what is installed already, before anything links against it */
		char *f = build_pkg(names->v[i]);
		pkg_install_file(f, op);
		free(f);
	}
}

/* Packages linked against sonames that upgrades took away are rebuilt (the preserved old
 * libraries kept them working meanwhile). */
void abi_followup(void)
{
	if (!txn_lost.n)
		return;
	sv_sort_uniq(&txn_lost);
	svec pkgs = SVEC_INIT, rebuild = SVEC_INIT;
	db_list(&pkgs);
	for (size_t i = 0; i < pkgs.n; i++) {
		svec needs = SVEC_INIT;
		db_lines(pkgs.v[i], "needs-so", &needs);
		for (size_t j = 0; j < txn_lost.n; j++) {
			if (sv_has(&needs, txn_lost.v[j])) {
				sv_push(&rebuild, pkgs.v[i]);
				break;
			}
		}
		sv_free(&needs);
	}
	char *lost = sv_join(&txn_lost, " ");
	sv_clear(&txn_lost);
	sv_free(&pkgs);
	if (rebuild.n) {
		char *list = sv_join(&rebuild, " ");
		msg("ABI change (%s): rebuilding %s", lost, list);
		free(list);
		svec order = SVEC_INIT;
		deps_order_set(&rebuild, &order);
		bool saved = B.rebuild;
		B.rebuild = true;
		build_install(&order, "rebuild");
		B.rebuild = saved;
		sv_free(&order);
		preserved_cleanup();
	}
	free(lost);
	sv_free(&rebuild);
}

static void install_names(const svec *names)
{
	svec order = SVEC_INIT, todo = SVEC_INIT;
	if (B.nodeps)
		sv_extend(&order, names);
	else
		deps_resolve(names, &order);
	for (size_t i = 0; i < order.n; i++) {
		if (!B.force && !B.rebuild && pkg_uptodate(order.v[i])) {
			msg2("%s is up to date", order.v[i]);
			continue;
		}
		sv_push(&todo, order.v[i]);
	}
	if (todo.n) {
		char *all = sv_join(&todo, " ");
		msg("Packages (%zu): %s", todo.n, all);
		free(all);
		if (!confirm("Build and install them?"))
			die("aborted, nothing changed");
		build_install(&todo, NULL);
	}
	sv_free(&order);
	sv_free(&todo);
}

int cmd_install(int argc, char **argv)
{
	if (!argc)
		die("usage: burn install <pkg|file.pkg.tar.zst>...");
	need_write();
	svec files = SVEC_INIT, names = SVEC_INIT;
	for (int i = 0; i < argc; i++) {
		if (str_suffix(argv[i], ".pkg.tar.zst")) {
			if (!is_file(argv[i]))
				die("no such package file: %s", argv[i]);
			sv_push_own(&files, path_real(argv[i]));
		} else {
			sv_push(&names, argv[i]);
		}
	}
	for (size_t i = 0; i < files.n; i++)
		pkg_install_file(files.v[i], NULL);
	triggers_run();
	if (names.n) {
		install_names(&names);
		triggers_run();
	}
	preserved_cleanup();
	abi_followup();
	triggers_run();
	sv_free(&files);
	sv_free(&names);
	return 0;
}

/* NAMES ordered so that dependencies come first (installed metadata only) */
static void db_order(const svec *names, size_t at, strmap *seen, svec *order)
{
	const char *q = names->v[at];
	if (sm_has(seen, q))
		return;
	sm_put(seen, q, 0);
	char *deps = db_get(q, "depends");
	svec d = SVEC_INIT;
	sv_split_ws(&d, deps ? deps : "");
	for (size_t i = 0; i < d.n; i++) {
		ssize_t j = sv_index(names, d.v[i]);
		if (j >= 0)
			db_order(names, (size_t)j, seen, order);
	}
	sv_push(order, q);
	sv_free(&d);
	free(deps);
}

int cmd_remove(int argc, char **argv)
{
	if (!argc)
		die("usage: burn remove <pkg>...");
	need_write();
	svec names = SVEC_INIT, order = SVEC_INIT;
	for (int i = 0; i < argc; i++) {
		if (!db_has(argv[i]))
			die("%s is not installed", argv[i]);
		sv_push(&names, argv[i]);
	}
	char *all = sv_join(&names, " ");
	char *q = xasprintf("Remove %s?", all);
	if (!confirm(q))
		die("aborted, nothing changed");
	free(q);
	free(all);
	strmap seen = STRMAP_INIT;
	for (size_t i = 0; i < names.n; i++)
		db_order(&names, i, &seen, &order);
	for (size_t i = order.n; i-- > 0;)
		pkg_remove(order.v[i]);
	preserved_cleanup();
	triggers_run();
	sm_free(&seen);
	sv_free(&order);
	sv_free(&names);
	return 0;
}

int cmd_rollback(int argc, char **argv)
{
	if (argc != 1)
		die("usage: burn rollback <pkg>");
	need_write();
	const char *p = argv[0];
	if (!db_has(p))
		die("%s is not installed", p);
	char *hist = xasprintf("%s/history", B.state_dir);
	if (!is_file(hist))
		die("no history yet");
	svec lines = SVEC_INIT;
	file_lines(hist, &lines);
	const char *line = NULL;
	for (size_t i = 0; i < lines.n; i++) {
		svec f = SVEC_INIT;
		char *s = lines.v[i];
		for (char *c = s;; c++) {
			if (*c == '|' || !*c) {
				sv_push_own(&f, xstrndup(s, (size_t)(c - s)));
				if (!*c)
					break;
				s = c + 1;
			}
		}
		if (f.n >= 3 && !strcmp(f.v[2], p) && !strcmp(f.v[1], "upgrade"))
			line = lines.v[i];
		sv_free(&f);
	}
	if (!line)
		die("no previous build of %s in the history", p);
	const char *file = strrchr(line, '|') ? strrchr(line, '|') + 1 : line;
	if (!is_file(file))
		die("the previous build is not in the package cache any more: %s", file);
	pkg_install_file(file, "rollback");
	char *hold[] = {(char *)p, NULL};
	cmd_hold(1, hold);
	preserved_cleanup();
	triggers_run();
	abi_followup();
	msg("%s rolled back and held; 'burn unhold %s' lets updates touch it again", p, p);
	sv_free(&lines);
	free(hist);
	return 0;
}

/* clone [/SOURCE] [pkg...]: install into the root the very builds that SOURCE (a path; default /)
 * has installed, straight from the package cache and without building anything: an installer
 * copies the running system this way. With names, only those packages and what they depend on. */
int cmd_clone(int argc, char **argv)
{
	const char *from = "/";
	svec want = SVEC_INIT;
	for (int i = 0; i < argc; i++) {
		if (strchr(argv[i], '/'))
			from = argv[i];
		else
			sv_push(&want, argv[i]);
	}
	need_write();
	char *src_root = path_real(from);
	if (!src_root)
		die("no such root: %s", from);
	char *src_db = path_join(strcmp(src_root, "/") ? src_root : "", "var/lib/burn/installed");
	char *a = path_real(src_db), *b = path_real(B.db_dir);
	if (!a)
		die("%s has no burn database (%s)", from, src_db);
	if (b && !strcmp(a, b))
		die("clone: %s is the target root itself", from);
	free(a);
	free(b);

	/* read the source database: which packages, in dependency order, and their cache files */
	char *saved = B.db_dir;
	B.db_dir = src_db;
	svec names = SVEC_INIT;
	if (want.n) {
		/* the closure of WANT over the installed dependencies */
		for (size_t i = 0; i < want.n; i++) {
			if (!db_has(want.v[i]))
				die("%s is not installed in %s", want.v[i], from);
			sv_push(&names, want.v[i]);
		}
		for (size_t i = 0; i < names.n; i++) {
			char *deps = db_get(names.v[i], "depends");
			svec d = SVEC_INIT;
			sv_split_ws(&d, deps ? deps : "");
			for (size_t j = 0; j < d.n; j++)
				if (db_has(d.v[j]) && sv_index(&names, d.v[j]) < 0)
					sv_push(&names, d.v[j]);
			sv_free(&d);
			free(deps);
		}
	} else {
		db_list(&names);
	}
	if (!names.n)
		die("nothing installed in %s", from);
	svec order = SVEC_INIT, files = SVEC_INIT, missing = SVEC_INIT;
	strmap seen = STRMAP_INIT;
	for (size_t i = 0; i < names.n; i++)
		db_order(&names, i, &seen, &order);
	for (size_t i = 0; i < order.n; i++) {
		svec m = SVEC_INIT;
		db_lines(order.v[i], "meta", &m);
		char *f = xasprintf("%s/packages/%s-%s-%s-%s-%s.pkg.tar.zst", cfg("CACHE_DIR"), order.v[i],
				    meta_get(&m, "version"), meta_get(&m, "release"), meta_get(&m, "arch"),
				    meta_get(&m, "buildid"));
		if (is_file(f))
			sv_push_own(&files, f);
		else {
			sv_push_own(&missing, f);
		}
		sv_free(&m);
	}
	B.db_dir = saved;
	if (missing.n) {
		for (size_t i = 0; i < missing.n; i++)
			warn("not in the package cache: %s", missing.v[i]);
		die("clone: %zu package file(s) missing (BURN_CACHE=%s)", missing.n, cfg("CACHE_DIR"));
	}
	char *all = sv_join(&order, " ");
	msg("Packages (%zu): %s", order.n, all);
	free(all);
	if (!confirm("Install these builds?"))
		die("aborted, nothing changed");
	for (size_t i = 0; i < files.n; i++)
		pkg_install_file(files.v[i], NULL);
	preserved_cleanup();
	triggers_run();
	sm_free(&seen);
	sv_free(&order);
	sv_free(&files);
	sv_free(&missing);
	sv_free(&names);
	sv_free(&want);
	free(src_db);
	free(src_root);
	return 0;
}
