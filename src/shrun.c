/* SPDX-License-Identifier: MIT */
/*
 * Recipe functions (do_*, post_install, *_extra) are shell code that burn does not interpret:
 * bash runs them. Every build writes WORK/env.sh, the shell state the prototype had when it
 * called them: the configuration, the helpers of src/prelude.sh, the recipe (sourced again, so
 * bash defines its functions), the build type's bt_* and the variables of the build. A phase is
 * one bash process that sources env.sh and calls the function.
 */
#include "burn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "spawn.h"

static const char PRELUDE[] =
#include "prelude.inc"
	;

/* variables bash owns: assigning them fails or means something else */
static bool bash_owned(const char *n)
{
	static const char *const names[] = {"DIRSTACK", "EPOCHREALTIME", "EPOCHSECONDS", "EUID", "FUNCNAME", "GROUPS",
					    "HISTCMD",  "LINENO",        "OLDPWD",       "PIPESTATUS", "PPID", "PWD",
					    "RANDOM",   "SECONDS",       "SHELLOPTS",    "SRANDOM",  "UID",  "_"};
	if (str_prefix(n, "BASH"))
		return true;
	for (size_t i = 0; i < ARRAY_LEN(names); i++)
		if (!strcmp(n, names[i]))
			return true;
	return false;
}

static void put_var(sbuf *s, const shvar *v)
{
	if (bash_owned(v->name))
		return;
	if (v->unset) {
		sb_addf(s, "unset -v %s\n", v->name);
	} else if (v->array) {
		sb_addf(s, "%s=(", v->name);
		for (size_t i = 0; i < v->val.n; i++) {
			if (i)
				sb_addc(s, ' ');
			sh_quote(s, v->val.v[i]);
		}
		sb_adds(s, ")\n");
	} else {
		sb_addf(s, "%s=", v->name);
		sh_quote(s, v->val.n ? v->val.v[0] : "");
		sb_addc(s, '\n');
	}
	if (v->exported)
		sb_addf(s, "export %s\n", v->name);
}

static void put_layer(sbuf *s, const shenv *e)
{
	for (size_t i = 0; i < e->n; i++)
		put_var(s, &e->v[i]);
}

static void put_scalar(sbuf *s, const char *name, const char *val)
{
	sb_addf(s, "%s=", name);
	sh_quote(s, val);
	sb_addc(s, '\n');
}

void shrun_write_env(build *b, const char *type, const char *file)
{
	const recipe *r = b->r;
	sbuf s = SBUF_INIT;
	sb_addf(&s, "# burn %s: the shell of the recipe functions of %s, sourced by every phase\n", BURN_VERSION,
		rget(r, "name"));
	sb_adds(&s, "set --\nset -o errexit -o nounset -o pipefail\nshopt -s nullglob extglob inherit_errexit\n");
	sb_adds(&s, "# the configuration\n");
	put_layer(&s, &B.cfg);
	char self[4096];
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
	self[n > 0 ? n : 0] = 0;
	put_scalar(&s, "BURN_SELF", n > 0 ? self : "burn");
	put_scalar(&s, "BURN_VERSION", BURN_VERSION);
	put_scalar(&s, "CLI_FEATURES", B.cli_features ? B.cli_features : "");
	static const struct {
		const char *name;
		const bool *on;
	} flags[] = {{"VERBOSE", &B.verbose}, {"FORCE", &B.force},   {"REBUILD", &B.rebuild},
		     {"NODEPS", &B.nodeps},   {"NO_SYNC", &B.no_sync}, {"ASSUME_YES", &opt_yes}};
	for (size_t i = 0; i < ARRAY_LEN(flags); i++)
		put_scalar(&s, flags[i].name, *flags[i].on ? "1" : "0");
	sb_adds(&s, "# helpers (src/prelude.sh)\n");
	sb_adds(&s, PRELUDE);
	sb_adds(&s, "# the recipe\n");
	recipe_reset_sh(r, &s);
	sb_adds(&s, ". \"$RECIPE_FILE\"\n");
	if (type)
		sb_addf(&s, "__burn_bt_define %s\n", type);
	sb_adds(&s, "declare -gA FEAT=(");
	for (size_t i = 0; i < r->nfeat; i++) {
		sb_adds(&s, i ? " [" : "[");
		sh_quote(&s, r->feat[i].name);
		sb_addf(&s, "]=%d", r->feat[i].on);
	}
	sb_adds(&s, ")\n# the build\n");
	put_layer(&s, &b->v);
	free(b->envfile);
	b->envfile = xstrdup(file);
	if (file_write(b->envfile, s.s, s.len, 0644) < 0)
		die_errno("cannot write %s", b->envfile);
	sb_free(&s);
}

void shrun_argv(const build *b, const char *fn, bool in_src, svec *argv)
{
	sv_push(argv, "bash");
	sv_push(argv, "-c");
	sv_pushf(argv, ". \"$1\"\n%s%s", in_src ? "cd \"$SRC\"\n" : "", fn);
	sv_push(argv, "burn");
	sv_push(argv, b->envfile);
}

void shrun_lines(const build *b, char **envp, const char *fn, svec *out)
{
	if (!sv_has(&b->r->funcs, fn))
		return;
	svec argv = SVEC_INIT;
	char *call = xasprintf("run_fn %s", fn);
	shrun_argv(b, call, false, &argv);
	sbuf o = SBUF_INIT;
	run_opts opts = {.envp = envp, .out = &o};
	run(sv_argv(&argv), &opts); /* like mapfile < <(run_fn FN): the status does not count */
	for (size_t i = 0; i < o.len;) {
		const char *nl = memchr(o.s + i, '\n', o.len - i);
		size_t end = nl ? (size_t)(nl - o.s) : o.len;
		sv_push_own(out, xstrndup(o.s + i, end - i));
		i = end + 1;
	}
	sb_free(&o);
	free(call);
	sv_free(&argv);
}
