/* SPDX-License-Identifier: MIT */
/*
 * The recipe language: the static subset of shell that recipes, burn.conf and profiles use at
 * their top level (assignments, arrays, quoting, parameter expansion, brace expansion, function
 * definitions). Function bodies are not parsed: bash runs them at build time. See docs/DESIGN.md.
 */
#ifndef BURN_SHPARSE_H
#define BURN_SHPARSE_H

#include "util.h"

typedef struct {
	char *name;
	bool array;
	bool exported;
	bool unset; /* a tombstone: hides the parents' (or the environment's) variable of this name */
	svec val;   /* a scalar has exactly one element */
} shvar;

typedef struct shenv {
	const struct shenv *parent; /* read-only fallback for lookups */
	bool use_environ;           /* names unknown here and in the parents come from getenv() */
	shvar *v;
	size_t n, cap;
} shenv;

void sh_init(shenv *e, const shenv *parent, bool use_environ);
void sh_free(shenv *e);
shvar *sh_find_local(shenv *e, const char *name);      /* NULL for tombstones too */
const shvar *sh_find(const shenv *e, const char *name); /* this environment, then the parents */
bool sh_isset(const shenv *e, const char *name);         /* a variable or an environment variable */
const char *sh_get(const shenv *e, const char *name);   /* "$name"; NULL when unset */
bool sh_get_list(const shenv *e, const char *name, svec *out); /* appends "${name[@]}" */
void sh_set(shenv *e, const char *name, const char *val);      /* name=val (element 0 of an array) */
void sh_set_list(shenv *e, const char *name, const svec *vals); /* name=(vals...) */
void sh_append(shenv *e, const char *name, const char *val);   /* name+=val */
void sh_append_list(shenv *e, const char *name, const svec *vals); /* name+=(vals...) */
void sh_unset(shenv *e, const char *name);
void sh_export(shenv *e, const char *name); /* also before the assignment, like bash */
/* the variables set in e itself (not the parents'), sorted by name */
void sh_local_names(const shenv *e, svec *out);

typedef struct shctx shctx;
struct shctx {
	shenv *env;
	svec funcs; /* functions defined by the evaluated files, in order */
	/* commands other than unset and export (e.g. `inherit` in profiles):
	 * return 0 when handled, 1 when unknown, -1 after sh_error() */
	int (*command)(shctx *c, svec *argv);
	void *user;
	char *err; /* the first error: "file:line: message" */
	const char *file;
	int line;
};

void sh_ctx_init(shctx *c, shenv *env);
void sh_ctx_free(shctx *c);
int sh_error(shctx *c, const char *fmt, ...) PRINTF(2, 3); /* records the error, returns -1 */
int sh_eval(shctx *c, const char *src, size_t len, const char *file);
int sh_eval_file(shctx *c, const char *path);
/* expand one word like the value of a scalar assignment, e.g. "${version%.*}" */
int sh_expand_word(shctx *c, const char *raw, char **out);
/* quote s for bash: 'it'\''s' */
void sh_quote(sbuf *b, const char *s);

#endif
