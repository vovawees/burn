/* SPDX-License-Identifier: MIT */
/*
 * Spins: what goes on top of the base (a desktop, a window manager, a kiosk...), each in its own
 * repository. A spin is a directory with spin.conf (its packages, features and services) and,
 * optionally, packages/ (its own recipes, searched before the base's) and package.features.
 * `burn spin add` clones it (a git URL) or links it (a local directory) as
 * $R/var/lib/burn/spins/NAME. From then on its recipes and features take part in every build,
 * plan and update, `burn sync` pulls it and `burn update` installs what a newer spin.conf asks
 * for. `burn spin remove` takes back what `add` brought: packages, services, features.
 */
#include "burn.h"
#include "spawn.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
	char *name, *dir, *desc;
	svec packages, features, services, user_services;
} spin;

/* what `add` changed, for `remove`: $STATE_DIR/spins/NAME */
typedef struct {
	char *source, *added;
	svec installed, services, user_services;
} spin_st;
static void st_read(const char *name, spin_st *t);
static void st_write(const char *name, const spin_st *t);
static void st_free(spin_st *t);

static void spin_free(spin *s)
{
	free(s->name);
	free(s->dir);
	free(s->desc);
	sv_free(&s->packages);
	sv_free(&s->features);
	sv_free(&s->services);
	sv_free(&s->user_services);
	*s = (spin){0};
}

char *spin_root(void)
{
	return xasprintf("%s/var/lib/burn/spins", B.R);
}

static char *spin_state(const char *name)
{
	return xasprintf("%s/spins/%s", B.state_dir, name);
}

/* the added spins: $R/var/lib/burn/spins/NAME (clones, or links to directories), by name */
void spin_dirs(svec *out)
{
	char *root = spin_root();
	svec all = SVEC_INIT;
	glob_paths(root, "*", &all);
	for (size_t i = 0; i < all.n; i++)
		if (is_dir(all.v[i]))
			sv_push(out, all.v[i]);
	sv_free(&all);
	free(root);
}

/* the name of a repository: basename URL .git */
static char *repo_base(const char *url)
{
	char *u = xstrdup(url);
	size_t n = strlen(u);
	while (n > 1 && u[n - 1] == '/')
		u[--n] = 0;
	char *b = xstrdup(path_base(u));
	free(u);
	size_t bl = strlen(b);
	if (bl > 4 && str_suffix(b, ".git"))
		b[bl - 4] = 0;
	return b;
}

static bool valid_name(const char *s)
{
	if (!*s || !((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9')))
		return false;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || strchr("+._-", *s)))
			return false;
	return true;
}

static void words(const shenv *e, const char *var, svec *out)
{
	svec v = SVEC_INIT;
	sh_get_list(e, var, &v);
	for (size_t i = 0; i < v.n; i++)
		sv_split_ws(out, v.v[i]);
	sv_free(&v);
}

/* DIR/spin.conf. NAME: the name it is added under; NULL: SPIN_NAME, else DEF */
static bool spin_read(const char *dir, const char *name, const char *def, spin *s, char **err)
{
	*s = (spin){0};
	char *conf = path_join(dir, "spin.conf");
	if (!is_file(conf)) {
		*err = xasprintf("%s: no spin.conf (not a spin)", dir);
		free(conf);
		return false;
	}
	shenv e;
	sh_init(&e, NULL, false);
	shctx c;
	sh_ctx_init(&c, &e);
	bool ok = sh_eval_file(&c, conf) == 0;
	if (!ok)
		*err = xstrdup(c.err ? c.err : conf);
	sh_ctx_free(&c);
	free(conf);
	if (ok) {
		const char *n = sh_get(&e, "SPIN_NAME");
		s->name = xstrdup(name ? name : n && *n ? n : def);
		s->dir = xstrdup(dir);
		const char *d = sh_get(&e, "SPIN_DESC");
		s->desc = xstrdup(d ? d : "");
		words(&e, "SPIN_PACKAGES", &s->packages);
		words(&e, "SPIN_FEATURES", &s->features);
		words(&e, "SPIN_SERVICES", &s->services);
		words(&e, "SPIN_USER_SERVICES", &s->user_services);
		if (!valid_name(s->name)) {
			*err = xasprintf("%s: bad spin name '%s' (a-z, 0-9, +._-)", dir, s->name);
			ok = false;
		}
		const svec *lists[] = {&s->packages, &s->services, &s->user_services};
		for (size_t l = 0; ok && l < ARRAY_LEN(lists); l++)
			for (size_t i = 0; ok && i < lists[l]->n; i++)
				if (!valid_name(lists[l]->v[i])) {
					*err = xasprintf("%s/spin.conf: bad name '%s'", dir, lists[l]->v[i]);
					ok = false;
				}
		for (size_t i = 0; ok && i < s->features.n; i++) {
			const char *f = s->features.v[i];
			if (!valid_name(f + (*f == '+' || *f == '-'))) {
				*err = xasprintf("%s/spin.conf: bad feature '%s'", dir, f);
				ok = false;
			}
		}
	}
	sh_free(&e);
	if (!ok)
		spin_free(s);
	return ok;
}

/* every added spin that reads; the broken ones are reported once */
static void spins_all(spin **out, size_t *n)
{
	static bool warned;
	svec dirs = SVEC_INIT;
	spin_dirs(&dirs);
	*out = xcalloc(dirs.n + 1, sizeof **out);
	*n = 0;
	for (size_t i = 0; i < dirs.n; i++) {
		char *err = NULL;
		if (spin_read(dirs.v[i], path_base(dirs.v[i]), NULL, &(*out)[*n], &err))
			(*n)++;
		else if (!warned)
			warn("spin skipped: %s", err);
		free(err);
	}
	warned = true;
	sv_free(&dirs);
}

static void spins_free(spin *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		spin_free(&s[i]);
	free(s);
}

/* SPINS_FEATURES: the features of every spin, between the profile's and burn.conf's */
void spin_setup(void)
{
	spin *s;
	size_t n;
	spins_all(&s, &n);
	svec all = SVEC_INIT;
	for (size_t i = 0; i < n; i++)
		sv_extend(&all, &s[i].features);
	char *j = sv_join(&all, " ");
	cfg_set("SPINS_FEATURES", j);
	free(j);
	sv_free(&all);
	spins_free(s, n);
}

/* the package.features files of the spins, before $R/etc/burn/package.features */
void spin_package_features_files(svec *out)
{
	svec dirs = SVEC_INIT;
	spin_dirs(&dirs);
	for (size_t i = 0; i < dirs.n; i++) {
		char *f = path_join(dirs.v[i], "package.features");
		if (is_file(f))
			sv_push_own(out, f);
		else
			free(f);
	}
	sv_free(&dirs);
}

/* the packages the spins want that are not installed: NAME and the spin asking for it */
void spin_missing(svec *names, svec *why)
{
	spin *s;
	size_t n;
	spins_all(&s, &n);
	for (size_t i = 0; i < n; i++)
		for (size_t k = 0; k < s[i].packages.n; k++) {
			const char *p = s[i].packages.v[k];
			if (db_has(p) || sv_has(names, p))
				continue;
			sv_push(names, p);
			sv_push(why, s[i].name);
		}
	spins_free(s, n);
}

/* after burn update installed what spins asked for: the packages new since BEFORE belong to
 * the spins that wanted any of them (so that `spin remove` takes them back) */
void spin_note_installed(const svec *before)
{
	svec now = SVEC_INIT, fresh = SVEC_INIT;
	db_list(&now);
	for (size_t i = 0; i < now.n; i++)
		if (!sv_has(before, now.v[i]))
			sv_push(&fresh, now.v[i]);
	spin *s;
	size_t n;
	spins_all(&s, &n);
	for (size_t i = 0; i < n && fresh.n; i++) {
		bool mine = false;
		for (size_t k = 0; k < s[i].packages.n && !mine; k++)
			mine = sv_has(&fresh, s[i].packages.v[k]);
		if (!mine)
			continue;
		spin_st t;
		st_read(s[i].name, &t);
		for (size_t k = 0; k < fresh.n; k++)
			if (!sv_has(&t.installed, fresh.v[k]))
				sv_push(&t.installed, fresh.v[k]);
		st_write(s[i].name, &t);
		st_free(&t);
	}
	spins_free(s, n);
	sv_free(&fresh);
	sv_free(&now);
}

/* git spins: pull them like the recipe repositories (local directories are the user's) */
void spin_sync(void)
{
	svec dirs = SVEC_INIT;
	spin_dirs(&dirs);
	for (size_t i = 0; i < dirs.n; i++) {
		char *g = path_join(dirs.v[i], ".git");
		bool git = !is_link(dirs.v[i]) && is_dir(g);
		free(g);
		if (!git)
			continue;
		msg("Syncing spin %s", path_base(dirs.v[i]));
		char *const pull[] = {"git", "-C", dirs.v[i], "pull", "-q", "--ff-only", NULL};
		fflush(stdout);
		int rc = run(pull, NULL);
		if (rc)
			exit_status(rc);
		char *const log[] = {"git", "-C", dirs.v[i], "log", "-1", "--format=%h %cs %s", NULL};
		char *line = run_output(log, NULL);
		msg2("%s", line ? line : "");
		free(line);
	}
	sv_free(&dirs);
}

/* ---- services ---- */

/* SVC enabled at boot: a link in $R/etc/dinit.d/boot.d (user services: etc/dinit.d/user/boot.d,
 * which turnstile's dinit --user waits for, for every user) */
static char *svc_link(const char *svc, bool user)
{
	return xasprintf("%s/etc/dinit.d/%sboot.d/%s", B.R, user ? "user/" : "", svc);
}

/* the service file as the running system sees it, NULL when the root has none */
static char *svc_file(const char *svc, bool user)
{
	const char *sub = user ? "/user" : "";
	const char *dirs[] = {"/etc/dinit.d", "/usr/lib/dinit.d"};
	for (size_t i = 0; i < ARRAY_LEN(dirs); i++) {
		char *in_root = xasprintf("%s%s%s/%s", B.R, dirs[i], sub, svc);
		bool ok = is_file(in_root);
		free(in_root);
		if (ok)
			return xasprintf("%s%s/%s", dirs[i], sub, svc);
	}
	return NULL;
}

/* true when this call enabled it */
static bool svc_enable(const char *svc, bool user)
{
	char *l = svc_link(svc, user);
	bool did = false;
	if (exists(l)) {
		msg2("%s%s: enabled already", svc, user ? " (user)" : "");
	} else {
		char *target = svc_file(svc, user);
		if (!target) {
			warn("no %sservice %s in the root: not enabled", user ? "user " : "", svc);
		} else {
			char *d = path_dir(l);
			if (mkdir_p(d, 0755) < 0)
				die_errno("cannot create %s", d);
			free(d);
			if (symlink(target, l) < 0)
				die_errno("cannot link %s", l);
			msg2("enabled %s%s", svc, user ? " (user, every user)" : "");
			did = true;
		}
		free(target);
	}
	free(l);
	return did;
}

static void svc_disable(const char *svc, bool user)
{
	char *l = svc_link(svc, user);
	if (is_link(l)) {
		if (unlink(l) < 0)
			die_errno("cannot remove %s", l);
		msg2("disabled %s%s", svc, user ? " (user)" : "");
	}
	free(l);
}

/* ---- state ---- */



static void st_free(spin_st *t)
{
	free(t->source);
	free(t->added);
	sv_free(&t->installed);
	sv_free(&t->services);
	sv_free(&t->user_services);
	*t = (spin_st){0};
}

static void st_read(const char *name, spin_st *t)
{
	*t = (spin_st){0};
	char *f = spin_state(name);
	svec lines = SVEC_INIT;
	file_lines(f, &lines);
	for (size_t i = 0; i < lines.n; i++) {
		char *l = lines.v[i], *eq = strchr(l, '=');
		if (!eq)
			continue;
		*eq++ = 0;
		if (!strcmp(l, "source"))
			t->source = xstrdup(eq);
		else if (!strcmp(l, "added"))
			t->added = xstrdup(eq);
		else if (!strcmp(l, "installed"))
			sv_split_ws(&t->installed, eq);
		else if (!strcmp(l, "services"))
			sv_split_ws(&t->services, eq);
		else if (!strcmp(l, "user_services"))
			sv_split_ws(&t->user_services, eq);
	}
	sv_free(&lines);
	free(f);
}

static void st_write(const char *name, const spin_st *t)
{
	char *f = spin_state(name), *d = path_dir(f);
	if (mkdir_p(d, 0755) < 0)
		die_errno("cannot create %s", d);
	char *a = sv_join(&t->installed, " "), *b = sv_join(&t->services, " "),
	     *c = sv_join(&t->user_services, " ");
	char *s = xasprintf("source=%s\nadded=%s\ninstalled=%s\nservices=%s\nuser_services=%s\n",
			    t->source ? t->source : "", t->added ? t->added : "", a, b, c);
	if (file_write(f, s, strlen(s), 0644) < 0)
		die_errno("cannot write %s", f);
	free(s);
	free(a);
	free(b);
	free(c);
	free(d);
	free(f);
}

/* ---- commands ---- */

static bool find_spin(const char *name, spin *s)
{
	char *root = spin_root(), *dir = path_join(root, name);
	char *err = NULL;
	bool ok = is_dir(dir) && spin_read(dir, name, NULL, s, &err);
	if (!ok && err)
		die("%s", err);
	free(err);
	free(dir);
	free(root);
	return ok;
}

/* installed packages whose recipe or features no longer match (spin features changed) */
static void stale_packages(svec *out)
{
	svec pkgs = SVEC_INIT;
	db_list(&pkgs);
	for (size_t i = 0; i < pkgs.n; i++) {
		char *p = recipe_path(pkgs.v[i]);
		if (p && !pkg_uptodate(pkgs.v[i]))
			sv_push(out, pkgs.v[i]);
		free(p);
	}
	sv_free(&pkgs);
}

/* the installed packages that went stale since BEFORE was taken: the features of the spin
 * changed them (what was stale already is left to burn update) */
static void rebuild_stale(const svec *before)
{
	svec now = SVEC_INIT, todo = SVEC_INIT;
	stale_packages(&now);
	for (size_t i = 0; i < now.n; i++)
		if (!sv_has(before, now.v[i]) && db_has(now.v[i]))
			sv_push(&todo, now.v[i]);
	if (todo.n) {
		char *l = sv_join(&todo, " ");
		msg("Features changed: rebuilding %s", l);
		free(l);
		install_packages(&todo);
	}
	sv_free(&todo);
	sv_free(&now);
}

static int spin_add(int argc, char **argv)
{
	if (argc != 1)
		die("usage: burn spin add <git-url|directory>");
	need_write();
	const char *src = argv[0];
	char *root = spin_root();
	if (mkdir_p(root, 0755) < 0)
		die_errno("cannot create %s", root);
	bool local = is_dir(src);
	char *tmp = NULL, *source = NULL;
	spin s;
	char *err = NULL;
	if (local) {
		source = path_real(src);
		if (!spin_read(source, NULL, path_base(source), &s, &err))
			die("%s", err);
	} else {
		if (!have("git"))
			die("git is required to add a spin from %s", src);
		tmp = xasprintf("%s/.new-%ld", root, (long)getpid());
		rm_rf(tmp);
		msg("Cloning %s", src);
		char *const clone[] = {"git", "clone", "-q", "--filter=blob:none", (char *)src, tmp, NULL};
		fflush(stdout);
		int rc = run(clone, NULL);
		if (rc) {
			rm_rf(tmp);
			die("cannot clone %s", src);
		}
		source = xstrdup(src);
		char *def = repo_base(src);
		bool ok = spin_read(tmp, NULL, def, &s, &err);
		free(def);
		if (!ok) {
			rm_rf(tmp);
			die("%s", err);
		}
	}
	char *dir = path_join(root, s.name);
	if (exists(dir)) {
		if (tmp)
			rm_rf(tmp);
		die("spin %s is added already (burn spin remove %s first)", s.name, s.name);
	}
	svec stale = SVEC_INIT;
	stale_packages(&stale);
	if (local ? symlink(source, dir) < 0 : rename(tmp, dir) < 0) {
		int e = errno;
		if (tmp)
			rm_rf(tmp);
		errno = e;
		die_errno("cannot add %s as %s", src, dir);
	}
	/* registered: its recipes and features count from here on */
	spin_setup();
	for (size_t i = 0; i < s.packages.n; i++) {
		char *p = recipe_path(s.packages.v[i]);
		if (!p) {
			if (local)
				unlink(dir);
			else
				rm_rf(dir);
			die("spin %s: no recipe for %s (neither in the spin nor in the recipe repositories)", s.name,
			    s.packages.v[i]);
		}
		free(p);
	}
	msg("Spin %s%s%s", s.name, *s.desc ? ": " : "", s.desc);
	if (s.features.n) {
		char *f = sv_join(&s.features, " ");
		msg2("features: %s", f);
		free(f);
	}
	svec before = SVEC_INIT, after = SVEC_INIT;
	db_list(&before);
	spin_st t = {0};
	t.source = source;
	char now[32];
	time_t tt = time(NULL);
	strftime(now, sizeof now, "%Y-%m-%dT%H:%M:%SZ", gmtime(&tt));
	t.added = xstrdup(now);
	st_write(s.name, &t); /* a failed install leaves a spin that `remove` can still undo */
	if (s.packages.n)
		install_packages(&s.packages);
	rebuild_stale(&stale);
	db_list(&after);
	for (size_t i = 0; i < after.n; i++)
		if (!sv_has(&before, after.v[i]))
			sv_push(&t.installed, after.v[i]);
	for (size_t i = 0; i < s.services.n; i++)
		if (svc_enable(s.services.v[i], false))
			sv_push(&t.services, s.services.v[i]);
	for (size_t i = 0; i < s.user_services.n; i++)
		if (svc_enable(s.user_services.v[i], true))
			sv_push(&t.user_services, s.user_services.v[i]);
	st_write(s.name, &t);
	msg("Spin %s added: %zu new package(s), %zu service(s) enabled", s.name, t.installed.n,
	    t.services.n + t.user_services.n);
	st_free(&t);
	sv_free(&stale);
	sv_free(&before);
	sv_free(&after);
	spin_free(&s);
	free(dir);
	free(tmp);
	free(root);
	return 0;
}

/* NAMES minus what something outside of them still needs (the user's own packages, other spins) */
static void removable(svec *names, const svec *keep_extra)
{
	for (bool changed = true; changed;) {
		changed = false;
		svec pkgs = SVEC_INIT;
		db_list(&pkgs);
		for (size_t i = 0; i < pkgs.n && !changed; i++) {
			if (sv_has(names, pkgs.v[i]))
				continue;
			char *deps = db_get(pkgs.v[i], "depends");
			svec d = SVEC_INIT;
			sv_split_ws(&d, deps ? deps : "");
			for (size_t k = 0; k < d.n; k++) {
				ssize_t j = sv_index(names, d.v[k]);
				if (j >= 0) {
					msg2("keeping %s: %s needs it", d.v[k], pkgs.v[i]);
					sv_remove(names, (size_t)j);
					changed = true;
				}
			}
			sv_free(&d);
			free(deps);
		}
		for (size_t i = 0; i < names->n; i++)
			if (sv_has(keep_extra, names->v[i])) {
				msg2("keeping %s: another spin wants it", names->v[i]);
				sv_remove(names, i--);
			}
		sv_free(&pkgs);
	}
}

static int spin_remove(int argc, char **argv)
{
	if (argc != 1)
		die("usage: burn spin remove <name>");
	need_write();
	const char *name = argv[0];
	spin s;
	char *root = spin_root(), *dir = path_join(root, name), *stf = spin_state(name);
	bool have_spin = find_spin(name, &s);
	if (!have_spin && !exists(dir) && !is_file(stf))
		die("no spin %s (see: burn spin list)", name);
	spin_st t;
	st_read(name, &t);
	/* what the other spins want stays */
	spin *all;
	size_t n;
	spins_all(&all, &n);
	svec others = SVEC_INIT, other_svc = SVEC_INIT, other_usvc = SVEC_INIT;
	for (size_t i = 0; i < n; i++) {
		if (!strcmp(all[i].name, name))
			continue;
		sv_extend(&others, &all[i].packages);
		sv_extend(&other_svc, &all[i].services);
		sv_extend(&other_usvc, &all[i].user_services);
		spin_st o;
		st_read(all[i].name, &o);
		sv_extend(&others, &o.installed);
		st_free(&o);
	}
	spins_free(all, n);
	svec gone = SVEC_INIT;
	for (size_t i = 0; i < t.installed.n; i++)
		if (db_has(t.installed.v[i]))
			sv_push(&gone, t.installed.v[i]);
	removable(&gone, &others);
	svec stale = SVEC_INIT;
	stale_packages(&stale);
	char *list = gone.n ? sv_join(&gone, " ") : xstrdup("none");
	char *q = xasprintf("Remove spin %s and its packages (%s)?", name, list);
	if (!confirm(q))
		die("aborted, nothing changed");
	free(q);
	free(list);
	for (size_t i = 0; i < t.services.n; i++)
		if (!sv_has(&other_svc, t.services.v[i]))
			svc_disable(t.services.v[i], false);
	for (size_t i = 0; i < t.user_services.n; i++)
		if (!sv_has(&other_usvc, t.user_services.v[i]))
			svc_disable(t.user_services.v[i], true);
	/* unregistered first: the features and recipes of the spin are gone for what follows */
	if (is_link(dir) ? unlink(dir) < 0 : rm_rf(dir) < 0)
		die_errno("cannot remove %s", dir);
	spin_setup();
	if (gone.n)
		remove_packages(&gone);
	unlink(stf);
	rebuild_stale(&stale);
	msg("Spin %s removed", name);
	sv_free(&stale);
	sv_free(&gone);
	sv_free(&others);
	sv_free(&other_svc);
	sv_free(&other_usvc);
	st_free(&t);
	if (have_spin)
		spin_free(&s);
	free(stf);
	free(dir);
	free(root);
	return 0;
}

static char *spin_commit(const char *dir)
{
	char *g = path_join(dir, ".git");
	char *c = NULL;
	if (is_dir(g)) {
		char *const argv[] = {"git", "-C", (char *)dir, "log", "-1", "--format=%h %cs", NULL};
		run_opts o = {.quiet = false};
		c = run_output(argv, &o);
	}
	free(g);
	return c;
}

static int spin_list(void)
{
	spin *s;
	size_t n;
	spins_all(&s, &n);
	if (!n)
		msg("No spins (burn spin add <git-url|directory>)");
	for (size_t i = 0; i < n; i++) {
		char *c = spin_commit(s[i].dir);
		char *link = is_link(s[i].dir) ? path_real(s[i].dir) : NULL;
		printf("%-16s %-24s %s\n", s[i].name, c ? c : link ? link : "-", s[i].desc);
		free(link);
		free(c);
	}
	spins_free(s, n);
	return 0;
}

static void kv(const char *k, const char *v)
{
	printf("%-14s %s\n", k, *v ? v : "-");
}

static int spin_show(int argc, char **argv)
{
	if (argc != 1)
		die("usage: burn spin show <name>");
	spin s;
	if (!find_spin(argv[0], &s))
		die("no spin %s (see: burn spin list)", argv[0]);
	spin_st t;
	st_read(s.name, &t);
	char *c = spin_commit(s.dir);
	char *real = path_real(s.dir);
	kv("name", s.name);
	kv("description", s.desc);
	kv("source", t.source ? t.source : real ? real : "");
	kv("directory", real ? real : s.dir);
	kv("commit", c ? c : "");
	kv("added", t.added ? t.added : "");
	sbuf b = SBUF_INIT;
	for (size_t i = 0; i < s.packages.n; i++)
		sb_addf(&b, "%s%s%s", i ? " " : "", s.packages.v[i], db_has(s.packages.v[i]) ? "" : " (not installed)");
	kv("packages", sb_str(&b));
	sb_reset(&b);
	char *pd = path_join(s.dir, "packages");
	svec recipes = SVEC_INIT;
	glob_paths(pd, "*", &recipes);
	for (size_t i = 0, k = 0; i < recipes.n; i++) {
		char *r = path_join(recipes.v[i], "recipe");
		if (is_file(r))
			sb_addf(&b, "%s%s", k++ ? " " : "", path_base(recipes.v[i]));
		free(r);
	}
	kv("recipes", sb_str(&b));
	sb_reset(&b);
	char *f = sv_join(&s.features, " ");
	kv("features", f);
	free(f);
	const svec *lists[] = {&s.services, &s.user_services};
	const char *keys[] = {"services", "user_services"};
	for (size_t l = 0; l < 2; l++) {
		for (size_t i = 0; i < lists[l]->n; i++) {
			char *ln = svc_link(lists[l]->v[i], l == 1);
			sb_addf(&b, "%s%s%s", i ? " " : "", lists[l]->v[i], exists(ln) ? "" : " (disabled)");
			free(ln);
		}
		kv(keys[l], sb_str(&b));
		sb_reset(&b);
	}
	char *inst = sv_join(&t.installed, " ");
	kv("brought", inst);
	free(inst);
	sb_free(&b);
	sv_free(&recipes);
	free(pd);
	free(real);
	free(c);
	st_free(&t);
	spin_free(&s);
	return 0;
}

int cmd_spin(int argc, char **argv)
{
	const char *sub = argc ? argv[0] : "list";
	if (!strcmp(sub, "add"))
		return spin_add(argc - 1, argv + 1);
	if (!strcmp(sub, "remove"))
		return spin_remove(argc - 1, argv + 1);
	if (!strcmp(sub, "list"))
		return spin_list();
	if (!strcmp(sub, "show"))
		return spin_show(argc - 1, argv + 1);
	die("usage: burn spin [add <git-url|directory> | remove <name> | list | show <name>]");
}
