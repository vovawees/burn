/* SPDX-License-Identifier: MIT */
/* Commands that read recipes: search, info, query, lint. */
#include "burn.h"

#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sha256.h"
#include "spawn.h"

static bool in_list(const char *s, const char *const *list)
{
	for (; *list; list++)
		if (!strcmp(s, *list))
			return true;
	return false;
}

static char *lower(const char *s)
{
	char *l = xstrdup(s);
	for (char *p = l; *p; p++)
		*p = (char)tolower((unsigned char)*p);
	return l;
}

static char *join_or_dash(const recipe *r, const char *var)
{
	svec v = SVEC_INIT;
	rlist(r, var, &v);
	char *j = sv_join(&v, " ");
	sv_free(&v);
	if (!*j) {
		free(j);
		j = xstrdup("-");
	}
	return j;
}

int cmd_search(int argc, char **argv)
{
	char *re_s = lower(argc > 0 ? argv[0] : ".");
	regex_t re;
	if (regcomp(&re, re_s, REG_EXTENDED | REG_NOSUB))
		die("search: invalid regular expression: %s", argc > 0 ? argv[0] : ".");
	svec names = SVEC_INIT;
	recipe_names(&names);
	sv_sort(&names);
	for (size_t i = 0; i < names.n; i++) {
		char *err = NULL;
		recipe *r = recipe_try_load(names.v[i], &err);
		if (!r) {
			free(err);
			continue;
		}
		svec outputs = SVEC_INIT;
		recipe_outputs(r, &outputs);
		for (size_t k = 0; k < outputs.n; k++) {
			const char *o = outputs.v[k], *sum = output_get(r, o, "summary");
			char *head = xasprintf("%s %s-%s", o, rget(r, "version"), rget(r, "release"));
			char *line = xasprintf("%s|%s", head, sum);
			char *l = lower(line);
			if (regexec(&re, l, 0, NULL, 0) == 0) {
				char *left = xasprintf("%s%s", head, db_has(o) ? " [installed]" : "");
				printf("%-34s %s\n", left, sum);
				free(left);
			}
			free(l);
			free(line);
			free(head);
		}
		sv_free(&outputs);
		recipe_free(r);
	}
	sv_free(&names);
	regfree(&re);
	free(re_s);
	return 0;
}

static void kv(const char *k, const char *v)
{
	printf("%-12s %s\n", k, v);
}

int cmd_info(int argc, char **argv)
{
	if (argc != 1)
		die("usage: burn info <pkg>");
	recipe *r = recipe_load(argv[0]);
	features_resolve(r);
	const char *name = r->output;
	char *inst;
	if (db_has(name)) {
		char *v = db_get(name, "version"), *rel = db_get(name, "release"), *f = db_get(name, "features");
		inst = xasprintf("%s-%s [%s]", v ? v : "", rel ? rel : "", f ? f : "");
		free(v);
		free(rel);
		free(f);
	} else {
		inst = xstrdup("no");
	}
	if (is_held(name)) {
		char *t = xasprintf("%s (held)", inst);
		free(inst);
		inst = t;
	}
	char *ver = xasprintf("%s-%s", rget(r, "version"), rget(r, "release"));
	svec lic = SVEC_INIT, dl = SVEC_INIT, splits = SVEC_INIT;
	output_list(r, name, "license", &lic);
	char *license = sv_join(&lic, " ");
	output_list(r, name, "depends", &dl);
	char *deps = dl.n ? sv_join(&dl, " ") : xstrdup("-"), *mdeps = join_or_dash(r, "makedepends");
	char *opts = join_or_dash(r, "options"), *feats = features_string(r);
	kv("name", name);
	kv("version", ver);
	kv("summary", output_get(r, name, "summary"));
	kv("homepage", rget(r, "homepage"));
	kv("license", license);
	kv("category", *rget(r, "category") ? rget(r, "category") : "-");
	kv("build_type", rget(r, "build_type"));
	kv("depends", deps);
	kv("makedepends", mdeps);
	kv("features", feats);
	kv("options", opts);
	kv("installed", inst);
	rlist(r, "split", &splits);
	if (!recipe_is_main(r, name)) {
		kv("split of", rget(r, "name"));
	} else if (splits.n) {
		char *j = sv_join(&splits, " ");
		kv("split", j);
		free(j);
	}
	sv_free(&splits);
	sv_free(&dl);
	kv("recipe", r->file);
	svec src = SVEC_INIT;
	rlist(r, "source", &src);
	for (size_t i = 0; i < src.n; i++) {
		source_spec s;
		source_info(src.v[i], &s);
		kv("source", s.url);
		source_spec_free(&s);
	}
	sv_free(&src);
	sv_free(&lic);
	free(license);
	free(deps);
	free(mdeps);
	free(opts);
	free(feats);
	free(ver);
	free(inst);
	recipe_free(r);
	return 0;
}

int cmd_query(int argc, char **argv)
{
	if (argc != 2)
		die("usage: burn query <pkg> <variable>");
	recipe *r = recipe_load(argv[0]);
	features_resolve(r);
	const char *var = argv[1];
	if (!strcmp(var, "rid")) {
		char *rid = recipe_rid(r);
		puts(rid);
		free(rid);
	} else if (!strcmp(var, "features")) {
		char *f = features_string(r);
		puts(f);
		free(f);
	} else if (!strcmp(var, "dir")) {
		puts(r->dir);
	} else if (!strcmp(var, "urls")) {
		svec src = SVEC_INIT;
		rlist(r, "source", &src);
		for (size_t i = 0; i < src.n; i++) {
			source_spec s;
			source_info(src.v[i], &s);
			puts(s.url);
			source_spec_free(&s);
		}
		sv_free(&src);
	} else if (!recipe_is_main(r, r->output) &&
		   (!strcmp(var, "name") || !strcmp(var, "summary") || !strcmp(var, "depends") ||
		    !strcmp(var, "license") || !strcmp(var, "backup") || !strcmp(var, "files"))) {
		/* a split package: its own values */
		if (!strcmp(var, "name") || !strcmp(var, "summary")) {
			puts(!strcmp(var, "name") ? r->output : output_get(r, r->output, var));
		} else {
			svec v = SVEC_INIT;
			output_list(r, r->output, var, &v);
			for (size_t i = 0; i < v.n; i++)
				puts(v.v[i]);
			sv_free(&v);
		}
	} else {
		if (!in_list(var, RECIPE_VARS))
			die("unknown recipe variable: %s", var);
		const shvar *v = sh_find(&r->env, var);
		if (v && v->array) {
			for (size_t i = 0; i < v->val.n; i++)
				puts(v->val.v[i]);
		} else {
			puts(rget(r, var));
		}
	}
	recipe_free(r);
	return 0;
}

/* ======================================================================= lint */

static bool all_chars(const char *s, const char *extra, bool upper)
{
	for (; *s; s++) {
		unsigned char ch = (unsigned char)*s;
		if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || (upper && ch >= 'A' && ch <= 'Z') ||
		      strchr(extra, ch)))
			return false;
	}
	return true;
}

static bool good_name(const char *s) /* ^[a-z0-9][a-z0-9+._-]*$ */
{
	return ((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9')) && all_chars(s + 1, "+._-", false);
}

/* an advisory id: PREFIX-ID, the prefix in capitals (CVE-2026-1234, GHSA-abcd-efgh-ijkl, OSV-…) */
static bool good_advisory(const char *a)
{
	const char *p = a;
	if (!(*p >= 'A' && *p <= 'Z'))
		return false;
	while ((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))
		p++;
	if (*p++ != '-' || !*p)
		return false;
	for (; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
		      *p == '.' || *p == '_' || *p == ':'))
			return false;
	return true;
}

static bool good_feature(const char *s) /* ^[+-]?[a-z0-9][a-z0-9_-]*$ */
{
	if (*s == '+' || *s == '-')
		s++;
	return ((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9')) && all_chars(s + 1, "_-", false);
}

static bool good_sum(const char *s) /* ^[0-9a-f]{64}$ */
{
	return strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64;
}

/* GNU grep -I: a file with NUL bytes is binary */
static bool binary_file(const char *path)
{
	size_t len;
	char *s = file_read(path, &len);
	bool bin = s && memchr(s, 0, len);
	free(s);
	return bin;
}

/* lint_one: problems of one recipe, one line each; the number of problems */
static int lint_one(const char *target)
{
	char *path = recipe_path(target);
	if (!path) {
		printf("%s: recipe not found\n", target);
		return 1;
	}
	if (have("bash")) {
		char *argv[] = {"bash", "-n", path, NULL};
		run_opts o = {.quiet = true, .stdin_null = true};
		if (run(argv, &o) != 0) {
			printf("%s: syntax error\n", path);
			free(path);
			return 1;
		}
	}
	char *err = NULL;
	recipe *r = recipe_try_load(path, &err);
	free(path);
	if (!r) {
		printf("%s: recipe failed to load: %s\n", target, err);
		free(err);
		return 1;
	}
	svec e = SVEC_INIT;
	const char *name = rget(r, "name"), *version = rget(r, "version"), *release = rget(r, "release");
	const char *dirname = path_base(r->dir);
	if (strcmp(name, dirname))
		sv_pushf(&e, "name does not match the directory '%s'", dirname);
	if (!good_name(name))
		sv_push(&e, "bad name");
	if (!*version || !all_chars(version, "._+~", true))
		sv_pushf(&e, "bad version '%s' (letters, digits and ._+~ only)", version);
	if (!(*release >= '1' && *release <= '9' && str_uint(release)))
		sv_push(&e, "release must be a positive integer");
	if (!*rget(r, "summary"))
		sv_push(&e, "summary is empty");
	svec lic = SVEC_INIT, src = SVEC_INIT, sums = SVEC_INIT, srcnames = SVEC_INIT;
	rlist(r, "license", &lic);
	if (!lic.n)
		sv_push(&e, "license is empty");
	const char *home = rget(r, "homepage");
	if (!str_prefix(home, "https://") && !str_prefix(home, "http://"))
		sv_push(&e, "homepage must be a URL");
	const char *cat = rget(r, "category");
	if (!in_list(cat, RECIPE_CATEGORIES)) {
		svec all = SVEC_INIT;
		for (const char *const *c = RECIPE_CATEGORIES; *c; c++)
			sv_push(&all, *c);
		char *j = sv_join(&all, " ");
		sv_pushf(&e, "category must be one of: %s (got '%s')", j, cat);
		free(j);
		sv_free(&all);
	}
	const char *bt = rget(r, "build_type");
	if (!in_list(bt, BUILD_TYPES))
		sv_pushf(&e, "unknown build_type '%s'", bt);
	rlist(r, "source", &src);
	rlist(r, "sha256", &sums);
	if (strcmp(bt, "meta") && !src.n)
		sv_push(&e, "no sources");
	if (src.n != sums.n)
		sv_pushf(&e, "%zu sources but %zu sha256 sums", src.n, sums.n);
	for (size_t i = 0; i < src.n; i++) {
		source_spec s;
		source_info(src.v[i], &s);
		const char *sum = i < sums.n ? sums.v[i] : "";
		if (s.local) {
			char *p = xasprintf("%s/%s", r->dir, s.url);
			if (!is_file(p))
				sv_pushf(&e, "local source '%s' is missing", s.url);
			free(p);
		} else {
			const char *u = strstr(src.v[i], "::") ? strstr(src.v[i], "::") + 2 : src.v[i];
			if (!str_prefix(u, "https://") && !str_prefix(u, "gnu://") && !str_prefix(u, "kernel://"))
				sv_pushf(&e, "source must use https:// (or gnu:// kernel://): %s", src.v[i]);
			if (!strcmp(sum, "SKIP"))
				sv_pushf(&e, "remote source %s needs a real sha256", s.name);
		}
		if (!good_sum(sum) && strcmp(sum, "SKIP"))
			sv_pushf(&e, "bad sha256 for %s", s.name);
		sv_push(&srcnames, s.name);
		source_spec_free(&s);
	}
	svec patches = SVEC_INIT;
	rlist(r, "patches", &patches);
	for (size_t i = 0; i < patches.n; i++) {
		char *a = xasprintf("%s/%s", r->dir, patches.v[i]);
		char *b = xasprintf("%s/patches/%s", r->dir, patches.v[i]);
		if (!is_file(a) && !is_file(b) && !sv_has(&srcnames, patches.v[i]))
			sv_pushf(&e, "patch '%s' is missing", patches.v[i]);
		free(a);
		free(b);
	}
	svec feats = SVEC_INIT, known = SVEC_INIT, deps = SVEC_INIT, splits = SVEC_INIT;
	rlist(r, "features", &feats);
	for (size_t i = 0; i < feats.n; i++)
		sv_push(&known, feats.v[i] + (feats.v[i][0] == '+' || feats.v[i][0] == '-'));
	rlist(r, "depends", &deps);
	rlist(r, "makedepends", &deps);
	/* split packages: own names, files, summary and dependencies */
	rlist(r, "split", &splits);
	for (size_t i = 0; i < splits.n; i++) {
		const char *sp = splits.v[i];
		char *key = split_key(sp);
		if (!good_name(sp))
			sv_pushf(&e, "bad split name '%s'", sp);
		else if (!strcmp(sp, name))
			sv_pushf(&e, "split '%s' is the name of the recipe", sp);
		for (size_t k = 0; k < i; k++)
			if (!strcmp(splits.v[k], sp))
				sv_pushf(&e, "split '%s' twice", sp);
		svec dirs = SVEC_INIT;
		recipe_dirs(&dirs);
		for (size_t k = 0; k < dirs.n; k++) {
			char *other = xasprintf("%s/%s/recipe", dirs.v[k], sp);
			if (is_file(other))
				sv_pushf(&e, "split '%s' is also a recipe: %s", sp, other);
			free(other);
		}
		sv_free(&dirs);
		svec files = SVEC_INIT;
		output_list(r, sp, "files", &files);
		if (!files.n)
			sv_pushf(&e, "split '%s' has no files_%s", sp, key);
		sv_free(&files);
		if (!*output_get(r, sp, "summary"))
			sv_pushf(&e, "split '%s' has no summary_%s", sp, key);
		output_list(r, sp, "depends", &deps);
		free(key);
	}
	for (size_t i = 0; i < deps.n; i++) {
		const char *d = deps.v[i], *colon = strchr(d, ':');
		char *pkg = colon ? xstrndup(d, (size_t)(colon - d)) : xstrdup(d);
		char *rp = recipe_path(pkg);
		if (!rp)
			sv_pushf(&e, "unknown dependency '%s'", pkg);
		free(rp);
		if (colon) {
			const char *f = colon + 1;
			if (*f == '!')
				f++;
			if (!sv_has(&known, f))
				sv_pushf(&e, "dependency '%s' uses undeclared feature '%s'", d, f);
		}
		free(pkg);
	}
	for (size_t i = 0; i < feats.n; i++)
		if (!good_feature(feats.v[i]))
			sv_pushf(&e, "bad feature '%s'", feats.v[i]);
	svec opts = SVEC_INIT;
	rlist(r, "options", &opts);
	for (size_t i = 0; i < opts.n; i++)
		if (!in_list(opts.v[i], RECIPE_OPTIONS))
			sv_pushf(&e, "unknown option '%s'", opts.v[i]);
	/* security=(CVE-2026-1234 GHSA-…): the advisories this version-release fixes */
	svec adv = SVEC_INIT;
	rlist(r, "security", &adv);
	for (size_t i = 0; i < adv.n; i++)
		if (!good_advisory(adv.v[i]))
			sv_pushf(&e, "bad advisory '%s' in security (like CVE-2026-1234 or GHSA-xxxx-xxxx-xxxx)", adv.v[i]);
	sv_free(&adv);
	/* the recipe repository holds instructions only: no binaries, no vendored tarballs */
	svec files = SVEC_INIT;
	recipe_files(r, true, &files);
	for (size_t i = 0; i < files.n; i++) {
		struct stat st;
		if (stat(files.v[i], &st) < 0)
			continue;
		const char *rel = files.v[i] + strlen(r->dir) + 1;
		if (st.st_size > 262144)
			sv_pushf(&e, "file too large for a recipe: %s", rel);
		else if (st.st_size > 0 && binary_file(files.v[i]))
			sv_pushf(&e, "binary file not allowed: %s", rel);
	}
	for (size_t i = 0; i < e.n; i++)
		printf("%s: %s\n", name, e.v[i]);
	int n = (int)e.n;
	sv_free(&files);
	sv_free(&opts);
	sv_free(&deps);
	sv_free(&splits);
	sv_free(&known);
	sv_free(&feats);
	sv_free(&patches);
	sv_free(&srcnames);
	sv_free(&sums);
	sv_free(&src);
	sv_free(&lic);
	sv_free(&e);
	recipe_free(r);
	return n;
}

int cmd_lint(int argc, char **argv)
{
	svec targets = SVEC_INIT;
	for (int i = 0; i < argc; i++)
		sv_push(&targets, argv[i]);
	if (!argc) {
		recipe_names(&targets);
		sv_sort(&targets);
	}
	if (!targets.n)
		die("no recipes to check (use -R DIR)");
	int total = 0;
	for (size_t i = 0; i < targets.n; i++) {
		fflush(stdout);
		total += lint_one(targets.v[i]);
	}
	fflush(stdout);
	if (total)
		die("lint: %d problem(s) in %zu recipe(s)", total, targets.n);
	msg("lint: %zu recipe(s) OK", targets.n);
	sv_free(&targets);
	return 0;
}
