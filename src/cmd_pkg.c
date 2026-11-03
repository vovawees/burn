/* SPDX-License-Identifier: MIT */
/* Commands that change the packages of the root: install (package files), remove, rollback. */
#include "burn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Packages linked against sonames that upgrades took away are rebuilt. Building is not in the
 * C version yet: the preserved old libraries keep them working until the prototype does it. */
static void abi_followup(void)
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
	if (!rebuild.n) {
		free(lost);
		return;
	}
	char *list = sv_join(&rebuild, " ");
	msg("ABI change (%s): rebuilding %s", lost, list);
	die("building is not in burn %s yet: run 'burn --rebuild --nodeps install %s' with the bash prototype "
	    "(CleanLinux/burn/burn); the old libraries are preserved until then",
	    BURN_VERSION, list);
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
	if (names.n)
		die("'install <recipe>' is not in burn %s yet: use the bash prototype (CleanLinux/burn/burn) "
		    "meanwhile, or give package files",
		    BURN_VERSION);
	for (size_t i = 0; i < files.n; i++)
		pkg_install_file(files.v[i], NULL);
	preserved_cleanup();
	triggers_run();
	abi_followup();
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
