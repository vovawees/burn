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
	"cargo_args",  "out_of_tree", "meson_subdir",  "cmake_subdir", "cflags_append", "split", "security", NULL};
/* category = layer of the base; 'extra' is for spin and user overlays; 'nonfree' is software
 * under a proprietary license (the NVIDIA driver), built only with ALLOW_NONFREE=1 */
const char *const RECIPE_CATEGORIES[] = {"core",     "devel",    "meta",  "kernel",  "firmware",
					 "drivers",  "platform", "extra", "nonfree", NULL};
const char *const BUILD_TYPES[] = {"autotools", "meson", "cmake", "make", "cargo", "python", "custom", "meta", NULL};
/* late: built after an update is applied, against the updated system (out-of-tree kernel modules
 * need the new kernel) */
/* pgo: with PGO=1, an instrumented build, the training of do_pgo, then the build with the profile;
 * !clang: built with GCC also when COMPILER=clang (glibc, the kernel, GCC itself) */
const char *const RECIPE_OPTIONS[] = {"!lto",       "!strip",    "!hardening", "!prune",
				      "!linker",    "staticlibs", "emptydirs", "o3", "late", "pgo", "!clang", NULL};

static const char *const ARRAY_VARS[] = {"license",   "source",         "sha256",     "depends",    "makedepends",
					 "features",  "patches",        "options",    "backup",     "noextract",
					 "configure_args", "meson_args", "cmake_args", "make_args", "make_install_args",
					 "cargo_args", "split", "security", NULL};
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
	/* the recipes of the spins come before the base's: a spin may carry its own build */
	svec spins = SVEC_INIT;
	spin_dirs(&spins);
	for (size_t i = 0; i < spins.n; i++) {
		char *p = path_join(spins.v[i], "packages");
		if (is_dir(p))
			sv_push_own(out, p);
		else
			free(p);
	}
	sv_free(&spins);
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

static char *split_owner(const char *what);

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
	return found ? found : split_owner(what);
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
					   "cmake_extra",  "do_pgo",          NULL};

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
	/* a split package loads the recipe that builds it */
	bool split = !strchr(what, '/') && strcmp(what, rget(r, "name")) && recipe_has_split(r, what);
	r->output = xstrdup(split ? what : rget(r, "name"));
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
	free(r->output);
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

/* proprietary software is built and installed only when the owner of the system asked for it */
void recipe_allowed(const recipe *r)
{
	if (strcmp(rget(r, "category"), "nonfree") || cfg_on("ALLOW_NONFREE"))
		return;
	svec lic = SVEC_INIT;
	rlist(r, "license", &lic);
	char *l = sv_join(&lic, " ");
	die("%s is not free software (license: %s); to build and install it, set ALLOW_NONFREE=1 in "
	    "/etc/burn/burn.conf", rget(r, "name"), l);
}

bool rhas_option(const recipe *r, const char *opt)
{
	svec o = SVEC_INIT;
	rlist(r, "options", &o);
	bool has = sv_has(&o, opt);
	sv_free(&o);
	return has;
}

/* ======================================================================= split packages */

/* split=(NAME...): one build, more packages. Each split takes the files of DESTDIR its
 * files_KEY patterns match and has its own summary_KEY, depends_KEY, backup_KEY and
 * license_KEY (the recipe's when unset); KEY is NAME with '_' for every character outside
 * [A-Za-z0-9_]: gcc-libs -> files_gcc_libs. */
char *split_key(const char *name)
{
	char *k = xstrdup(name);
	for (char *p = k; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'))
			*p = '_';
	return k;
}

bool recipe_has_split(const recipe *r, const char *name)
{
	svec s = SVEC_INIT;
	rlist(r, "split", &s);
	bool has = sv_has(&s, name);
	sv_free(&s);
	return has;
}

void recipe_outputs(const recipe *r, svec *out)
{
	sv_push(out, rget(r, "name"));
	rlist(r, "split", out);
}

bool recipe_is_main(const recipe *r, const char *output)
{
	return !strcmp(output, rget(r, "name"));
}

static char *split_var(const char *field, const char *split)
{
	char *k = split_key(split);
	char *v = xasprintf("%s_%s", field, k);
	free(k);
	return v;
}

bool split_isset(const recipe *r, const char *split, const char *field)
{
	char *v = split_var(field, split);
	const shvar *sv = sh_find(&r->env, v);
	free(v);
	return sv != NULL;
}

/* FIELD of the package OUTPUT: the recipe's own for the main package, FIELD_KEY for a split
 * (license falls back to the recipe's) */
const char *output_get(const recipe *r, const char *output, const char *field)
{
	if (recipe_is_main(r, output))
		return rget(r, field);
	char *v = split_var(field, output);
	const char *s = sh_get(&r->env, v);
	free(v);
	return s ? s : "";
}

void output_list(const recipe *r, const char *output, const char *field, svec *out)
{
	if (recipe_is_main(r, output) || (!strcmp(field, "license") && !split_isset(r, output, field))) {
		rlist(r, field, out);
		return;
	}
	char *v = split_var(field, output);
	rlist(r, v, out);
	free(v);
}

/* the recipe that builds WHAT as a split package (the first recipe of each name counts, like
 * recipe_path); found ones are remembered */
static char *split_owner(const char *what)
{
	static strmap known = STRMAP_INIT;
	static svec paths = SVEC_INIT;
	size_t at;
	if (sm_get(&known, what, &at) && is_file(paths.v[at]))
		return xstrdup(paths.v[at]);
	svec dirs = SVEC_INIT;
	strmap seen = STRMAP_INIT;
	char *found = NULL;
	recipe_dirs(&dirs);
	for (size_t i = 0; i < dirs.n && !found; i++) {
		char *pat = xasprintf("%s/*/recipe", dirs.v[i]);
		glob_t g;
		if (glob(pat, 0, NULL, &g) == 0) {
			for (size_t k = 0; k < g.gl_pathc && !found; k++) {
				char *d = path_dir(g.gl_pathv[k]);
				bool first = !sm_has(&seen, path_base(d));
				sm_put(&seen, path_base(d), 0);
				free(d);
				size_t len;
				char *text = first ? file_read(g.gl_pathv[k], &len) : NULL;
				if (text && strstr(text, "split") && strstr(text, what)) {
					char *err = NULL;
					recipe *r = recipe_try_load(g.gl_pathv[k], &err);
					if (r && recipe_has_split(r, what))
						found = xstrdup(r->file);
					recipe_free(r);
					free(err);
				}
				free(text);
			}
			globfree(&g);
		}
		free(pat);
	}
	sm_free(&seen);
	sv_free(&dirs);
	if (found) {
		sm_put(&known, what, paths.n);
		sv_push(&paths, found);
	}
	return found;
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

/* the words after NAME on its lines of the spins' package.features, then of
 * $ROOT/etc/burn/package.features */
static void package_features(const char *name, svec *out)
{
	svec files = SVEC_INIT;
	spin_package_features_files(&files);
	sv_push_own(&files, xasprintf("%s/etc/burn/package.features", B.R));
	svec lines = SVEC_INIT;
	for (size_t i = 0; i < files.n; i++)
		file_lines(files.v[i], &lines);
	sv_free(&files);
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

/* features=(+on -off off) in the recipe; overrides: VIDEO_CARDS (video_cards_CARD), the profile,
 * the spins, burn.conf, package.features (the spins', then /etc/burn's) and --features, in this
 * order. Only the features a recipe declares can be switched. */
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
	/* the GPU drivers of the profile switch the declared video_cards_CARD features (as USE_EXPAND
	 * in Gentoo): on for the cards in VIDEO_CARDS, off for the others; the overrides still apply */
	svec cards = SVEC_INIT;
	words_of(r, "VIDEO_CARDS", &cards);
	for (size_t i = 0; i < r->nfeat; i++)
		if (!strncmp(r->feat[i].name, "video_cards_", 12))
			r->feat[i].on = sv_has(&cards, r->feat[i].name + 12);
	sv_free(&cards);
	svec tok = SVEC_INIT;
	words_of(r, "PROFILE_FEATURES", &tok);
	words_of(r, "SPINS_FEATURES", &tok);
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

void files_under(const char *dir, svec *out)
{
	svec part = SVEC_INIT;
	walk_files(dir, true, &part);
	sv_sort(&part);
	sv_extend(out, &part);
	sv_free(&part);
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
