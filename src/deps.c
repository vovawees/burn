/* SPDX-License-Identifier: MIT */
/* Dependencies: what to build, depth first, dependencies before the packages that need them. */
#include "burn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* depends, and makedepends unless the build tools come from the host (cross or bootstrap).
 * A recipe with split packages never needs its own packages to build; a split package needs
 * what its recipe needs to build, and its own depends_KEY. */
static void recipe_deps(const char *name, svec *out)
{
	recipe *r = recipe_load(name);
	features_resolve(r);
	svec d = SVEC_INIT, act = SVEC_INIT, outputs = SVEC_INIT, words = SVEC_INIT;
	recipe_outputs(r, &outputs);
	rlist(r, "depends", &d);
	deps_active(r, &d, &act);
	if (!cfg_on("CROSS") && !cfg_on("HOST_DEPS")) {
		sv_clear(&d);
		rlist(r, "makedepends", &d);
		deps_active(r, &d, &act);
	}
	for (size_t i = 0; i < act.n; i++)
		sv_split_ws(&words, act.v[i]); /* for d in $deps */
	if (outputs.n == 1) {
		sv_extend(out, &words);
	} else {
		for (size_t i = 0; i < words.n; i++)
			if (!sv_has(&outputs, words.v[i]) && !sv_has(out, words.v[i]))
				sv_push(out, words.v[i]);
		/* to run: the package's own depends, its sibling packages too */
		sv_clear(&d);
		sv_clear(&act);
		sv_clear(&words);
		output_list(r, r->output, "depends", &d);
		deps_active(r, &d, &act);
		for (size_t i = 0; i < act.n; i++)
			sv_split_ws(&words, act.v[i]);
		for (size_t i = 0; i < words.n; i++)
			if (strcmp(words.v[i], r->output) && !sv_has(out, words.v[i]))
				sv_push(out, words.v[i]);
	}
	sv_free(&words);
	sv_free(&outputs);
	sv_free(&act);
	sv_free(&d);
	recipe_free(r);
}

static bool dep_satisfied(const char *d)
{
	if (db_has(d))
		return true;
	svec a = SVEC_INIT;
	cfg_words("ASSUME_INSTALLED", &a);
	bool in = sv_has(&a, d);
	sv_free(&a);
	return in;
}

enum { ACTIVE = 1, DONE = 2 };

static void visit(const char *p, strmap *state, svec *order)
{
	size_t st = 0;
	if (sm_get(state, p, &st)) {
		if (st == DONE)
			return;
		die("dependency cycle involving '%s'", p);
	}
	sm_put(state, p, ACTIVE);
	char *path = recipe_path(p);
	if (!path)
		die("no recipe provides '%s'", p);
	free(path);
	svec deps = SVEC_INIT;
	recipe_deps(p, &deps);
	for (size_t i = 0; i < deps.n; i++)
		if (!dep_satisfied(deps.v[i]))
			visit(deps.v[i], state, order);
	sv_free(&deps);
	sm_put(state, p, DONE);
	sv_push(order, p);
}

void deps_resolve(const svec *names, svec *order)
{
	strmap state = STRMAP_INIT;
	for (size_t i = 0; i < names->n; i++)
		visit(names->v[i], &state, order);
	sm_free(&state);
}

static void set_visit(const char *q, const strmap *in, strmap *seen, svec *order)
{
	if (sm_has(seen, q))
		return;
	sm_put(seen, q, 0);
	svec deps = SVEC_INIT;
	recipe_deps(q, &deps);
	for (size_t i = 0; i < deps.n; i++)
		if (sm_has(in, deps.v[i]))
			set_visit(deps.v[i], in, seen, order);
	sv_free(&deps);
	sv_push(order, q);
}

void deps_order_set(const svec *names, svec *order)
{
	strmap in = STRMAP_INIT, seen = STRMAP_INIT;
	for (size_t i = 0; i < names->n; i++)
		sm_put(&in, names->v[i], 0);
	for (size_t i = 0; i < names->n; i++)
		set_visit(names->v[i], &in, &seen, order);
	sm_free(&in);
	sm_free(&seen);
}

/* installed with the same version, recipe and features? (a broken recipe never is) */
bool pkg_uptodate(const char *name)
{
	if (!db_has(name))
		return false;
	char *err = NULL;
	recipe *r = recipe_try_load(name, &err);
	if (!r) {
		free(err);
		return false;
	}
	features_resolve(r);
	char *rid = recipe_rid(r);
	char *st = xasprintf("%s-%s %s", rget(r, "version"), rget(r, "release"), rid);
	char *v = db_get(name, "version"), *rel = db_get(name, "release"), *drid = db_get(name, "rid");
	char *db = xasprintf("%s-%s %s", v ? v : "", rel ? rel : "", drid ? drid : "");
	bool same = !strcmp(st, db);
	free(db);
	free(v);
	free(rel);
	free(drid);
	free(st);
	free(rid);
	recipe_free(r);
	return same;
}
