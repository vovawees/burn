/* SPDX-License-Identifier: MIT */
/*
 * tests/shdump FILE...: evaluate recipe-language files with burn's parser and print the
 * variables and functions they define, in the format of tests/shdump.bash (bash does the same).
 * tests/shcompat.sh compares the two.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/shparse.h"

static void put(const char *s)
{
	fputs("  <", stdout);
	for (; *s; s++) {
		if (*s == '\\')
			fputs("\\\\", stdout);
		else if (*s == '\n')
			fputs("\\n", stdout);
		else
			putchar(*s);
	}
	fputs(">\n", stdout);
}

static int cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(int argc, char **argv)
{
	shenv env;
	shctx c;
	sh_init(&env, NULL, true);
	sh_ctx_init(&c, &env);
	for (int i = 1; i < argc; i++) {
		if (sh_eval_file(&c, argv[i]) < 0) {
			printf("error: %s\n", c.err);
			sh_ctx_free(&c);
			sh_free(&env);
			return 1;
		}
	}
	svec names = SVEC_INIT, gone = SVEC_INIT;
	sh_local_names(&env, &names);
	for (size_t i = 0; i < names.n; i++) {
		const shvar *v = sh_find(&env, names.v[i]);
		const char *ev = getenv(v->name);
		if (!strncmp(v->name, "__", 2))
			continue;
		/* bash only reports what changed: exporting an environment variable is no change */
		if (!v->array && v->exported && ev && v->val.n && !strcmp(ev, v->val.v[0]))
			continue;
		if (v->array) {
			printf("%s array%s %zu\n", v->name, v->exported ? " export" : "", v->val.n);
			for (size_t k = 0; k < v->val.n; k++)
				put(v->val.v[k]);
		} else if (v->val.n) {
			printf("%s scalar%s\n", v->name, v->exported ? " export" : "");
			put(v->val.v[0]);
		}
	}
	for (size_t i = 0; i < env.n; i++)
		if (env.v[i].unset && getenv(env.v[i].name))
			sv_push(&gone, env.v[i].name);
	if (gone.n > 1)
		qsort(gone.v, gone.n, sizeof *gone.v, cmp);
	for (size_t i = 0; i < gone.n; i++)
		printf("unset %s\n", gone.v[i]);
	if (c.funcs.n > 1)
		qsort(c.funcs.v, c.funcs.n, sizeof *c.funcs.v, cmp);
	for (size_t i = 0; i < c.funcs.n; i++)
		printf("function %s\n", c.funcs.v[i]);
	sv_free(&names);
	sv_free(&gone);
	sh_ctx_free(&c);
	sh_free(&env);
	return 0;
}
