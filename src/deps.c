/* SPDX-License-Identifier: MIT */
/* Dependencies: what to build, depth first, dependencies before the packages that need them. */
#include "burn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* what the host has when it provides the build tools: the layers devel (compilers, meson, cmake,
 * perl, python and its modules) and core (openssl for the kernel's certificate tools, ...), which the
 * bootstrap checks for */
static bool from_host(const char *name)
{
	char *path = recipe_path(name);
	if (!path)
		return false; /* "no recipe provides" comes later, from visit() */
	free(path);
	recipe *r = recipe_load(name);
	const char *c = rget(r, "category");
	bool host = !strcmp(c, "devel") || !strcmp(c, "core");
	recipe_free(r);
	return host;
}

/* depends and makedepends. When the build tools come from the host (cross or bootstrap), the
 * makedepends of the layers devel and core are not built: the host has them; those of the other
 * layers (headers and data such as xorgproto, wayland-protocols, tllist) go into the root, the
 * sysroot of the build. A recipe with split packages never needs its own packages to build; a
 * split package needs what its recipe needs to build, and its own depends_KEY. */
static void recipe_deps(const char *name, svec *out)
{
	recipe *r = recipe_load(name);
	recipe_allowed(r);
	features_resolve(r);
	svec d = SVEC_INIT, act = SVEC_INIT, outputs = SVEC_INIT, words = SVEC_INIT;
	recipe_outputs(r, &outputs);
	rlist(r, "depends", &d);
	deps_active(r, &d, &act);
	for (size_t i = 0; i < act.n; i++)
		sv_split_ws(&words, act.v[i]); /* for d in $deps */
	sv_clear(&d);
	sv_clear(&act);
	rlist(r, "makedepends", &d);
	deps_active(r, &d, &act);
	bool host_tools = cfg_on("CROSS") || cfg_on("HOST_DEPS");
	svec mk = SVEC_INIT;
	for (size_t i = 0; i < act.n; i++)
		sv_split_ws(&mk, act.v[i]);
	for (size_t i = 0; i < mk.n; i++)
		if (!host_tools || !from_host(mk.v[i]))
			sv_push(&words, mk.v[i]);
	sv_free(&mk);
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

/* installed, or ASSUME_INSTALLED */
bool dep_satisfied(const char *d)
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

/* What the recipes NAMES (installed packages, about to be upgraded) need to build and run and is
 * not installed, with what that needs in turn, dependencies first; WHY gets, for each, the
 * package of NAMES that asks for it */
void deps_missing(const svec *names, svec *out, svec *why)
{
	strmap state = STRMAP_INIT;
	for (size_t i = 0; i < names->n; i++) {
		svec deps = SVEC_INIT;
		recipe_deps(names->v[i], &deps);
		for (size_t j = 0; j < deps.n; j++) {
			if (dep_satisfied(deps.v[j]))
				continue;
			size_t before = out->n;
			visit(deps.v[j], &state, out);
			for (size_t k = before; k < out->n; k++)
				sv_push(why, names->v[i]);
		}
		sv_free(&deps);
	}
	sm_free(&state);
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
