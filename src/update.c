/* SPDX-License-Identifier: MIT */
/* Rolling+++: sync (the recipe repositories), plan (what changed against the installed
 * packages), update (build everything first, then apply; ABI rebuilds; hooks). */
#include "burn.h"
#include "spawn.h"
#include "vercmp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* "${NAME[@]}": the elements of an array, a scalar as one element */
static void cfg_elems(const char *name, svec *out)
{
	const shvar *v = sh_find(&B.cfg, name);
	if (!v)
		return;
	if (v->array)
		sv_extend(out, &v->val);
	else
		sv_push(out, cfg(name));
}

/* git ARGS...: a failure stops burn with git's exit status, as errexit stopped the prototype */
static void git(const char *dir, ...) __attribute__((sentinel));
static void git(const char *dir, ...)
{
	svec argv = SVEC_INIT;
	sv_push(&argv, "git");
	if (dir) {
		sv_push(&argv, "-C");
		sv_push(&argv, dir);
	}
	va_list ap;
	va_start(ap, dir);
	for (const char *a; (a = va_arg(ap, const char *));)
		sv_push(&argv, a);
	va_end(ap);
	fflush(stdout);
	int rc = run(sv_argv(&argv), NULL);
	sv_free(&argv);
	if (rc)
		exit_status(rc);
}

/* basename URL .git */
static char *repo_name(const char *url)
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

/* the repositories under REPO_DIR (directories, byte order) */
static void repo_dirs(svec *out)
{
	svec all = SVEC_INIT;
	glob_paths(B.repo_dir, "*", &all);
	for (size_t i = 0; i < all.n; i++)
		if (is_dir(all.v[i]))
			sv_push(out, all.v[i]);
	sv_free(&all);
}

static bool has_git(const char *dir)
{
	char *g = path_join(dir, ".git");
	bool ok = is_dir(g);
	free(g);
	return ok;
}

static void sync_repos(void)
{
	if (!have("git"))
		die("git is required for sync");
	if (mkdir_p(B.repo_dir, 0755) < 0)
		die_errno("cannot create %s", B.repo_dir);
	const char *channel = cfg("CHANNEL");
	svec urls = SVEC_INIT;
	cfg_elems("RECIPE_REPOS", &urls);
	for (size_t i = 0; i < urls.n; i++) {
		char *base = repo_name(urls.v[i]);
		char *dir = xasprintf("%s/%s", B.repo_dir, base);
		if (has_git(dir)) {
			msg("Syncing %s (%s)", path_base(dir), channel);
			git(dir, "fetch", "-q", "origin", channel, NULL);
			git(dir, "checkout", "-q", channel, NULL);
			char *up = xasprintf("origin/%s", channel);
			git(dir, "merge", "-q", "--ff-only", up, NULL);
			free(up);
		} else {
			msg("Cloning %s (%s)", urls.v[i], channel);
			git(NULL, "clone", "-q", "--filter=blob:none", "--branch", channel, urls.v[i], dir, NULL);
		}
		char *const argv[] = {"git", "-C", dir, "log", "-1", "--format=%h %cs %s", NULL};
		char *line = run_output(argv, NULL);
		msg2("%s", line ? line : "");
		free(line);
		free(dir);
		free(base);
	}
	sv_free(&urls);
	spin_sync();
}

int cmd_sync(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	need_write();
	sync_repos();
	return 0;
}

/* ---- plan ---- */

typedef struct {
	char *name, *old, *avail, *reason, *risk; /* avail "-": will not be touched */
} plan_entry;

typedef struct {
	plan_entry *e;
	size_t n, cap;
} plan;

static void plan_add(plan *p, const char *name, const char *old, const char *avail, char *reason, const char *risk)
{
	if (p->n == p->cap) {
		p->cap = p->cap ? 2 * p->cap : 16;
		p->e = xrealloc(p->e, p->cap * sizeof *p->e);
	}
	p->e[p->n++] = (plan_entry){xstrdup(name), xstrdup(old), xstrdup(avail), reason, xstrdup(risk)};
}

static void plan_free(plan *p)
{
	for (size_t i = 0; i < p->n; i++) {
		free(p->e[i].name);
		free(p->e[i].old);
		free(p->e[i].avail);
		free(p->e[i].reason);
		free(p->e[i].risk);
	}
	free(p->e);
	*p = (plan){0};
}

/* "version-release" and the rid of the recipe NAME; false when it does not load */
static bool recipe_state(const char *name, char **vr, char **rid)
{
	char *err = NULL;
	recipe *r = recipe_try_load(name, &err);
	if (!r) {
		free(err);
		return false;
	}
	features_resolve(r);
	*vr = xasprintf("%s-%s", rget(r, "version"), rget(r, "release"));
	*rid = recipe_rid(r);
	recipe_free(r);
	return true;
}

static char *db_str(const char *name, const char *key)
{
	char *v = db_get(name, key);
	return v ? v : xstrdup("");
}

/* ${s%-*}: without the release */
static char *strip_release(const char *s)
{
	const char *d = strrchr(s, '-');
	return d ? xstrndup(s, (size_t)(d - s)) : xstrdup(s);
}

/* ${s%%.*} */
static char *major(const char *s)
{
	return xstrndup(s, strcspn(s, "."));
}

static void plan_compute(plan *p)
{
	svec pkgs = SVEC_INIT, core = SVEC_INIT;
	db_list(&pkgs);
	cfg_elems("CORE_PKGS", &core);
	for (size_t i = 0; i < pkgs.n; i++) {
		const char *n = pkgs.v[i];
		char *v = db_str(n, "version"), *rel = db_str(n, "release");
		char *old = xasprintf("%s-%s", v, rel);
		free(v);
		free(rel);
		char *path = recipe_path(n);
		char *nw = NULL, *rid = NULL, *reason = NULL;
		if (!path) {
			plan_add(p, n, old, "-", xstrdup("no recipe any more (kept, orphan)"), "");
			goto next;
		}
		if (!recipe_state(n, &nw, &rid)) {
			plan_add(p, n, old, "-", xstrdup("recipe failed to load (skipped)"), "");
			goto next;
		}
		if (strcmp(nw, old)) {
			char *a = strip_release(old), *b = strip_release(nw);
			reason = xstrdup(vercmp(a, b) > 0 ? "downgrade" : "upgrade");
			free(a);
			free(b);
		} else {
			char *drid = db_str(n, "rid");
			if (strcmp(rid, drid))
				reason = xstrdup("rebuild: recipe or features changed");
			free(drid);
			if (!reason)
				goto next;
		}
		if (is_held(n)) {
			plan_add(p, n, old, "-", xasprintf("held, recipe has %s (%s)", nw, reason), "");
			free(reason);
			goto next;
		}
		sbuf risk = SBUF_INIT;
		if (!strcmp(reason, "upgrade")) {
			char *a = major(old), *b = major(nw);
			if (strcmp(a, b))
				sb_adds(&risk, "major ");
			free(a);
			free(b);
		}
		if (sv_has(&core, n))
			sb_adds(&risk, "core ");
		if (risk.len)
			risk.s[--risk.len] = 0;
		plan_add(p, n, old, nw, reason, risk.s ? risk.s : "");
		sb_free(&risk);
	next:
		free(nw);
		free(rid);
		free(path);
		free(old);
	}
	/* what the spins ask for and is not installed yet (a new spin.conf after sync) */
	svec want = SVEC_INIT, why = SVEC_INIT;
	spin_missing(&want, &why);
	for (size_t i = 0; i < want.n; i++) {
		char *nw = NULL, *rid = NULL;
		if (!recipe_state(want.v[i], &nw, &rid)) {
			plan_add(p, want.v[i], "-", "-", xasprintf("spin %s: no recipe (skipped)", why.v[i]), "");
		} else {
			plan_add(p, want.v[i], "-", nw, xasprintf("new: spin %s", why.v[i]), "");
		}
		free(nw);
		free(rid);
	}
	sv_free(&want);
	sv_free(&why);
	sv_free(&core);
	sv_free(&pkgs);
}

/* the commits of each repository since the last update that touched the packages of the plan */
static void plan_changelog(const plan *p)
{
	svec dirs = SVEC_INIT;
	repo_dirs(&dirs);
	for (size_t i = 0; i < dirs.n; i++) {
		const char *dir = dirs.v[i];
		char *state = xasprintf("%s/%s.commit", B.state_dir, path_base(dir));
		size_t len;
		char *last = has_git(dir) && is_file(state) ? file_read(state, &len) : NULL;
		free(state);
		if (!last)
			continue;
		last[strcspn(last, "\n")] = 0; /* $(<file) */
		msg2("recipe changes in %s since the last update:", path_base(dir));
		svec argv = SVEC_INIT;
		sv_push(&argv, "git");
		sv_push(&argv, "-C");
		sv_push(&argv, dir);
		sv_push(&argv, "log");
		sv_push(&argv, "--oneline");
		sv_push(&argv, "--no-decorate");
		sv_pushf(&argv, "%s..HEAD", last);
		sv_push(&argv, "--");
		for (size_t j = 0; j < p->n; j++) {
			/* a split package changes with the directory of its recipe */
			char *rp = recipe_path(p->e[j].name), *rd = rp ? path_dir(rp) : NULL;
			sv_pushf(&argv, "packages/%s", rd ? path_base(rd) : p->e[j].name);
			free(rd);
			free(rp);
		}
		sbuf out = SBUF_INIT;
		run_opts o = {.out = &out, .quiet = true};
		run(sv_argv(&argv), &o);
		size_t lines = 0;
		for (size_t k = 0; k < out.len && lines < 30;) {
			const char *nl = memchr(out.s + k, '\n', out.len - k);
			size_t end = nl ? (size_t)(nl - out.s) : out.len;
			fprintf(stderr, "      %.*s\n", (int)(end - k), out.s + k);
			lines++;
			k = end + 1;
		}
		sb_free(&out);
		sv_free(&argv);
		free(last);
	}
	sv_free(&dirs);
}

static void plan_print(const plan *p)
{
	if (!p->n) {
		msg("Nothing to do: installed packages match the recipes");
		return;
	}
	msg("Update plan (%zu):", p->n);
	for (size_t i = 0; i < p->n; i++) {
		const plan_entry *e = &p->e[i];
		fprintf(stderr, "  %-22s %-14s -> %-14s %s%s%s%s\n", e->name, e->old, e->avail, e->reason,
			*e->risk ? "  [" : "", e->risk, *e->risk ? "]" : "");
	}
	plan_changelog(p);
}

int cmd_plan(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	plan p = {0};
	plan_compute(&p);
	plan_print(&p);
	plan_free(&p);
	return 0;
}

/* ---- update ---- */

/* the executables in /etc/burn/hooks/WHEN.d, in order; a failing pre-* hook stops everything */
static void run_hooks(const char *when)
{
	char *d = xasprintf("%s/etc/burn/hooks/%s.d", B.R, when);
	svec hooks = SVEC_INIT;
	glob_paths(d, "*", &hooks);
	svec env = SVEC_INIT;
	env_from_environ(&env);
	env_set(&env, "ROOT", B.root);
	for (size_t i = 0; i < hooks.n; i++) {
		const char *h = hooks.v[i];
		if (access(h, X_OK) != 0)
			continue;
		msg2("hook %s: %s", when, path_base(h));
		char *const argv[] = {(char *)h, NULL};
		run_opts o = {.envp = sv_argv(&env)};
		fflush(stdout);
		if (run(argv, &o) != 0) {
			if (str_prefix(when, "pre-"))
				die("hook %s failed; nothing was changed", h);
			warn("hook %s failed", h);
		}
	}
	sv_free(&env);
	sv_free(&hooks);
	free(d);
}

/* the commit of every repository and the time of this update */
static void state_record(void)
{
	if (mkdir_p(B.state_dir, 0755) < 0)
		die_errno("cannot create %s", B.state_dir);
	svec dirs = SVEC_INIT;
	repo_dirs(&dirs);
	for (size_t i = 0; i < dirs.n; i++) {
		if (!has_git(dirs.v[i]))
			continue;
		char *const argv[] = {"git", "-C", dirs.v[i], "rev-parse", "HEAD", NULL};
		char *head = run_output(argv, NULL);
		char *f = xasprintf("%s/%s.commit", B.state_dir, path_base(dirs.v[i]));
		char *line = xasprintf("%s\n", head ? head : "");
		if (file_write(f, line, strlen(line), 0644) < 0)
			die_errno("cannot write %s", f);
		free(line);
		free(f);
		free(head);
	}
	sv_free(&dirs);
	char now[32];
	time_t t = time(NULL);
	strftime(now, sizeof now, "%Y-%m-%dT%H:%M:%SZ\n", gmtime(&t));
	char *f = xasprintf("%s/last-update", B.state_dir);
	if (file_write(f, now, strlen(now), 0644) < 0)
		die_errno("cannot write %s", f);
	free(f);
}

int cmd_update(int argc, char **argv)
{
	need_write();
	if (!B.no_sync)
		sync_repos();
	plan p = {0};
	plan_compute(&p);
	plan_print(&p);
	svec todo = SVEC_INIT, fresh = SVEC_INIT;
	for (size_t i = 0; i < p.n; i++)
		if (strcmp(p.e[i].avail, "-"))
			sv_push(strcmp(p.e[i].old, "-") ? &todo : &fresh, p.e[i].name);
	plan_free(&p);
	if (!todo.n && !fresh.n) {
		sv_free(&todo);
		sv_free(&fresh);
		return 0;
	}
	char *q = xasprintf("Build %zu package(s) and apply them?", todo.n + fresh.n);
	if (!confirm(q))
		die("aborted, nothing changed");
	free(q);
	svec order = SVEC_INIT, files = SVEC_INIT, late = SVEC_INIT;
	deps_order_set(&todo, &order);
	/* 1) fetch and verify every source, then build everything: the running system is untouched
	 *    until every build succeeded. options=(late) waits for the applied update (step 5). */
	prefetch(&order);
	for (size_t i = 0; i < order.n; i++) {
		recipe *r = recipe_load(order.v[i]);
		bool is_late = rhas_option(r, "late");
		recipe_free(r);
		if (is_late)
			sv_push(&late, order.v[i]);
		else
			sv_push_own(&files, build_pkg(order.v[i]));
	}
	/* 2) the rollback point: a btrfs snapshot, then the pre-update hooks */
	snapshot_take(false);
	run_hooks("pre-update");
	/* 3) apply */
	for (size_t i = 0; i < files.n; i++)
		pkg_install_file(files.v[i], NULL);
	triggers_run();
	/* 4) rebuild what linked against sonames that disappeared */
	abi_followup();
	triggers_run();
	/* 5) what builds against the updated system: kernel modules for the new kernel. Should one
	 *    fail, the update stays applied and the snapshot of step 2 is the way back. */
	if (late.n) {
		char *list = sv_join(&late, " ");
		msg("Building against the updated system: %s", list);
		free(list);
		build_install(&late, NULL);
		triggers_run();
	}
	/* 6) the new packages of spins, with their dependencies, on top of the updated system */
	if (fresh.n) {
		svec before = SVEC_INIT;
		db_list(&before);
		install_packages(&fresh);
		spin_note_installed(&before);
		sv_free(&before);
	}
	run_hooks("post-update");
	state_record();
	cmd_doctor(argc, argv);
	sv_free(&files);
	sv_free(&late);
	sv_free(&order);
	sv_free(&todo);
	sv_free(&fresh);
	return 0;
}
