/* SPDX-License-Identifier: MIT */
/* Recipes: discovery, loading, features ("USE flags"), dependencies and identity. */
#include "burn.h"

#include <dirent.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sha256.h"

const char *const RECIPE_VARS[] = {
	"name",        "version",    "release",        "summary",     "homepage",     "license",
	"category",    "source",     "sha256",         "build_type",  "srcdir",       "depends",
	"makedepends", "features",   "patches",        "patch_strip", "options",      "backup",
	"noextract",   "configure_args", "meson_args", "cmake_args",  "make_args",    "make_install_args",
	"cargo_args",  "out_of_tree", "meson_subdir",  "cmake_subdir", "cflags_append", NULL};
/* category = layer of the base; 'extra' is for spin and user overlays */
const char *const RECIPE_CATEGORIES[] = {"core",     "devel",   "meta",     "kernel", "firmware",
					 "drivers",  "platform", "extra",   NULL};
const char *const BUILD_TYPES[] = {"autotools", "meson", "cmake", "make", "cargo", "python", "custom", "meta", NULL};
const char *const RECIPE_OPTIONS[] = {"!lto",       "!strip",    "!hardening", "!prune",
				      "!linker",    "staticlibs", "emptydirs", "o3", NULL};

static const char *const ARRAY_VARS[] = {"license",   "source",         "sha256",     "depends",    "makedepends",
					 "features",  "patches",        "options",    "backup",     "noextract",
					 "configure_args", "meson_args", "cmake_args", "make_args", "make_install_args",
					 "cargo_args", NULL};
static const char *const SCALAR_DEFAULTS[][2] = {
	{"name", ""},         {"version", ""},     {"release", "1"},     {"summary", ""},
	{"homepage", ""},     {"category", ""},    {"build_type", "autotools"}, {"srcdir", ""},
	{"patch_strip", "1"}, {"out_of_tree", "0"}, {"meson_subdir", ""}, {"cmake_subdir", ""},
	{"cflags_append", ""},
};

/* ======================================================================= discovery */

void recipe_dirs(svec *out)
{
	svec paths = SVEC_INIT;
	cfg_words("RECIPE_PATHS", &paths);
	for (size_t i = 0; i < paths.n; i++)
		if (is_dir(paths.v[i]))
			sv_push(out, paths.v[i]);
	sv_free(&paths);
	char *pat = xasprintf("%s/*/packages", B.repo_dir ? B.repo_dir : "");
	glob_t g;
	if (glob(pat, 0, NULL, &g) == 0) {
		for (size_t i = 0; i < g.gl_pathc; i++)
			if (is_dir(g.gl_pathv[i]))
				sv_push(out, g.gl_pathv[i]);
		globfree(&g);
	}
	free(pat);
}

char *recipe_path(const char *what)
{
	if (strchr(what, '/')) {
		char *in = xasprintf("%s/recipe", what);
		char *r = NULL;
		if (is_file(in))
			r = path_real(in);
		else if (is_file(what))
			r = path_real(what);
		free(in);
		return r;
	}
	svec dirs = SVEC_INIT;
	char *found = NULL;
	recipe_dirs(&dirs);
	for (size_t i = 0; i < dirs.n && !found; i++) {
		char *p = xasprintf("%s/%s/recipe", dirs.v[i], what);
		if (is_file(p))
			found = p;
		else
			free(p);
	}
	sv_free(&dirs);
	return found;
}

void recipe_names(svec *out)
{
	svec dirs = SVEC_INIT;
	recipe_dirs(&dirs);
	for (size_t i = 0; i < dirs.n; i++) {
		char *pat = xasprintf("%s/*/recipe", dirs.v[i]);
		glob_t g;
		if (glob(pat, 0, NULL, &g) == 0) {
			for (size_t k = 0; k < g.gl_pathc; k++) {
				char *d = path_dir(g.gl_pathv[k]);
				const char *n = path_base(d);
				if (!sv_has(out, n))
					sv_push(out, n);
				free(d);
			}
			globfree(&g);
		}
		free(pat);
	}
	sv_free(&dirs);
}

/* ======================================================================= loading */

static const char *const RECIPE_FUNCS[] = {"do_prepare",   "do_configure",    "do_build",    "do_check",
					   "do_install",   "post_install",    "configure_extra", "meson_extra",
					   "cmake_extra",  NULL};

/* bash's `unset -v` of every recipe variable, then the defaults: fresh variables, never exported */
static void recipe_reset(recipe *r)
{
	for (const char *const *v = RECIPE_VARS; *v; v++)
		sh_unset(&r->env, *v);
	for (size_t i = 0; i < ARRAY_LEN(SCALAR_DEFAULTS); i++)
		sh_set(&r->env, SCALAR_DEFAULTS[i][0], SCALAR_DEFAULTS[i][1]);
	svec none = SVEC_INIT;
	for (const char *const *a = ARRAY_VARS; *a; a++)
		sh_set_list(&r->env, *a, &none);
	sh_set(&r->env, "RECIPE_FILE", r->file);
	sh_set(&r->env, "RECIPE_DIR", r->dir);
}

void recipe_reset_sh(const recipe *r, sbuf *s)
{
	sb_adds(s, "unset -v");
	for (const char *const *v = RECIPE_VARS; *v; v++)
		sb_addf(s, " %s", *v);
	sb_adds(s, "\nunset -f");
	for (const char *const *f = RECIPE_FUNCS; *f; f++)
		sb_addf(s, " %s", *f);
	sb_addc(s, '\n');
	for (size_t i = 0; i < ARRAY_LEN(SCALAR_DEFAULTS); i++) {
		sb_addf(s, "%s=", SCALAR_DEFAULTS[i][0]);
		sh_quote(s, SCALAR_DEFAULTS[i][1]);
		sb_addc(s, '\n');
	}
	for (const char *const *a = ARRAY_VARS; *a; a++)
		sb_addf(s, "%s=()\n", *a);
	sb_adds(s, "RECIPE_FILE=");
	sh_quote(s, r->file);
	sb_adds(s, "\nRECIPE_DIR=");
	sh_quote(s, r->dir);
	sb_adds(s, "\n");
}

recipe *recipe_try_load(const char *what, char **err)
{
	char *path = recipe_path(what);
	if (!path) {
		*err = xasprintf("recipe not found: %s", what);
		return NULL;
	}
	recipe *r = xcalloc(1, sizeof *r);
	r->file = path;
	r->dir = path_dir(path);
	sh_init(&r->env, &B.cfg, false);
	recipe_reset(r);
	shctx c;
	sh_ctx_init(&c, &r->env);
	if (sh_eval_file(&c, path) < 0) {
		*err = c.err;
		c.err = NULL;
		sh_ctx_free(&c);
		recipe_free(r);
		return NULL;
	}
	r->funcs = c.funcs;
	c.funcs = (svec)SVEC_INIT;
	sh_ctx_free(&c);
	if (!*rget(r, "name") || !*rget(r, "version")) {
		*err = xasprintf("%s: 'name' and 'version' are required", path);
		recipe_free(r);
		return NULL;
	}
	return r;
}

recipe *recipe_load(const char *what)
{
	char *err = NULL;
	recipe *r = recipe_try_load(what, &err);
	if (!r)
		die("%s", err);
	return r;
}

void recipe_free(recipe *r)
{
	if (!r)
		return;
	for (size_t i = 0; i < r->nfeat; i++)
		free(r->feat[i].name);
	free(r->feat);
	sh_free(&r->env);
	sv_free(&r->funcs);
	free(r->file);
	free(r->dir);
	free(r);
}

const char *rget(const recipe *r, const char *var)
{
	const char *v = sh_get(&r->env, var);
	return v ? v : "";
}

void rlist(const recipe *r, const char *var, svec *out)
{
	sh_get_list(&r->env, var, out);
}

bool rhas_option(const recipe *r, const char *opt)
{
	svec o = SVEC_INIT;
	rlist(r, "options", &o);
	bool has = sv_has(&o, opt);
	sv_free(&o);
	return has;
}

/* ======================================================================= features */

static feature *feat_find(const recipe *r, const char *name)
{
	for (size_t i = 0; i < r->nfeat; i++)
		if (!strcmp(r->feat[i].name, name))
			return &r->feat[i];
	return NULL;
}

static void feat_set(recipe *r, const char *name, bool on)
{
	feature *f = feat_find(r, name);
	if (!f) {
		r->feat = xrealloc(r->feat, (r->nfeat + 1) * sizeof *r->feat);
		f = &r->feat[r->nfeat++];
		f->name = xstrdup(name);
	}
	f->on = on;
}

/* the words after NAME on its lines of $ROOT/etc/burn/package.features */
static void package_features(const char *name, svec *out)
{
	char *path = xasprintf("%s/etc/burn/package.features", B.R);
	svec lines = SVEC_INIT;
	file_lines(path, &lines);
	for (size_t i = 0; i < lines.n; i++) {
		char *hash = strchr(lines.v[i], '#');
		if (hash)
			*hash = 0;
		svec w = SVEC_INIT;
		sv_split_ws(&w, lines.v[i]);
		if (w.n && !strcmp(w.v[0], name))
			for (size_t k = 1; k < w.n; k++)
				sv_push(out, w.v[k]);
		sv_free(&w);
	}
	sv_free(&lines);
	free(path);
}

static void words_of(const recipe *r, const char *var, svec *out)
{
	const shvar *v = sh_find(&r->env, var);
	if (v && v->array) {
		for (size_t i = 0; i < v->val.n; i++)
			sv_split_ws(out, v->val.v[i]);
	} else {
		sv_split_ws(out, rget(r, var));
	}
}

/* features=(+on -off off) in the recipe; overrides: the profile, burn.conf, package.features
 * and --features, in this order. Only the features a recipe declares can be switched. */
void features_resolve(recipe *r)
{
	for (size_t i = 0; i < r->nfeat; i++)
		free(r->feat[i].name);
	r->nfeat = 0;
	svec f = SVEC_INIT;
	rlist(r, "features", &f);
	for (size_t i = 0; i < f.n; i++) {
		const char *s = f.v[i];
		if (*s == '+')
			feat_set(r, s + 1, true);
		else if (*s == '-')
			feat_set(r, s + 1, false);
		else
			feat_set(r, s, false);
	}
	sv_free(&f);
	svec tok = SVEC_INIT;
	words_of(r, "PROFILE_FEATURES", &tok);
	words_of(r, "FEATURES", &tok);
	package_features(rget(r, "name"), &tok);
	sv_split_ws(&tok, B.cli_features ? B.cli_features : "");
	for (size_t i = 0; i < tok.n; i++) {
		const char *t = tok.v[i];
		bool on = *t != '-';
		if (*t == '-' || *t == '+')
			t++;
		feature *ft = feat_find(r, t);
		if (ft)
			ft->on = on;
	}
	sv_free(&tok);
	r->resolved = true;
}

static int feat_cmp(const void *a, const void *b)
{
	return strcmp(((const feature *)a)->name, ((const feature *)b)->name);
}

char *features_string(const recipe *r)
{
	feature *sorted = xmalloc((r->nfeat + 1) * sizeof *sorted);
	if (r->nfeat)
		memcpy(sorted, r->feat, r->nfeat * sizeof *sorted);
	if (r->nfeat > 1)
		qsort(sorted, r->nfeat, sizeof *sorted, feat_cmp);
	sbuf b = SBUF_INIT;
	for (size_t i = 0; i < r->nfeat; i++)
		sb_addf(&b, "%s%c%s", i ? " " : "", sorted[i].on ? '+' : '-', sorted[i].name);
	free(sorted);
	return sb_steal(&b);
}

bool recipe_use(const recipe *r, const char *feat)
{
	const feature *f = feat_find(r, feat);
	return f && f->on;
}

/* "pkg" always, "pkg:feat" only with feat on, "pkg:!feat" only with feat off */
void deps_active(const recipe *r, const svec *deps, svec *out)
{
	for (size_t i = 0; i < deps->n; i++) {
		const char *d = deps->v[i];
		const char *colon = strchr(d, ':'), *neg = strstr(d, ":!");
		bool take = true;
		if (neg)
			take = !recipe_use(r, neg + 2);
		else if (colon)
			take = recipe_use(r, colon + 1);
		if (take)
			sv_push_own(out, colon ? xstrndup(d, (size_t)(colon - d)) : xstrdup(d));
	}
}

/* ======================================================================= identity */

/* regular files under dir (not following links, like find -type f) */
static void walk_files(const char *dir, bool with_recipe, svec *out)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char *p = xasprintf("%s/%s", dir, e->d_name);
		struct stat st;
		if (lstat(p, &st) == 0) {
			if (S_ISDIR(st.st_mode))
				walk_files(p, with_recipe, out);
			else if (S_ISREG(st.st_mode) && (with_recipe || strcmp(e->d_name, "recipe")))
				sv_push(out, p);
		}
		free(p);
	}
	closedir(d);
}

void recipe_files(const recipe *r, bool with_recipe, svec *out)
{
	struct stat st;
	size_t first = out->n;
	if (lstat(r->dir, &st) == 0 && S_ISDIR(st.st_mode))
		walk_files(r->dir, with_recipe, out);
	svec part = SVEC_INIT;
	for (size_t i = first; i < out->n; i++)
		sv_push(&part, out->v[i]);
	while (out->n > first)
		sv_remove(out, out->n - 1);
	sv_sort(&part);
	sv_extend(out, &part);
	sv_free(&part);
}

static void hash_file(sha256_ctx *c, const char *path)
{
	size_t len;
	char *s = file_read(path, &len);
	if (s)
		sha256_update(c, s, len);
	free(s);
}

/* the identity of the recipe itself (its files + features): a change means a rebuild */
char *recipe_rid(const recipe *r)
{
	sha256_ctx c;
	sha256_init(&c);
	hash_file(&c, r->file);
	svec files = SVEC_INIT;
	recipe_files(r, false, &files);
	for (size_t i = 0; i < files.n; i++)
		hash_file(&c, files.v[i]);
	sv_free(&files);
	if (!r->resolved)
		features_resolve((recipe *)r);
	char *fs = features_string(r);
	sha256_update(&c, fs, strlen(fs));
	sha256_update(&c, "\n", 1);
	free(fs);
	char hex[65];
	sha256_final_hex(&c, hex);
	return xstrndup(hex, 16);
}

/* ======================================================================= sources */

char *url_expand(const char *s)
{
	if (str_prefix(s, "gnu://"))
		return xasprintf("%s/%s", cfg("GNU_MIRROR"), s + 6);
	if (str_prefix(s, "kernel://"))
		return xasprintf("%s/%s", cfg("KERNEL_MIRROR"), s + 9);
	return xstrdup(s);
}

/* SPEC: [name::]url | local-file */
void source_info(const char *spec, source_spec *out)
{
	const char *s = spec, *sep = strstr(spec, "::");
	char *name = NULL;
	if (sep) {
		name = xstrndup(spec, (size_t)(sep - spec));
		s = sep + 2;
	}
	out->local = !strstr(s, "://");
	out->url = out->local ? xstrdup(s) : url_expand(s);
	if (!name || !*name) {
		free(name);
		const char *slash = strrchr(out->url, '/');
		name = xstrdup(slash ? slash + 1 : out->url);
	}
	out->name = name;
}

void source_spec_free(source_spec *s)
{
	free(s->name);
	free(s->url);
	s->name = s->url = NULL;
}
