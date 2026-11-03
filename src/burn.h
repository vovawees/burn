/* SPDX-License-Identifier: MIT */
/*
 * The state of one burn run and the modules that share it: configuration and profiles
 * (config.c), recipes and features (recipe.c), the installed-package database (db.c) and the
 * commands (cmd_*.c). Variable names follow the bash prototype, whose burn.conf, profiles,
 * recipes and database burn keeps using.
 */
#ifndef BURN_BURN_H
#define BURN_BURN_H

#include "shparse.h"
#include "util.h"

typedef struct {
	/* command line */
	bool verbose, force, rebuild, nodeps, no_sync, write;
	char *cli_features; /* every -f, like the prototype's CLI_FEATURES */
	char *config_file;  /* -c FILE or $BURN_CONFIG */
	/* paths */
	char *root;      /* ROOT, absolute */
	const char *R;   /* ROOT without the trailing slash: "" for / */
	char *db_dir;    /* $R/var/lib/burn/installed */
	char *state_dir; /* $R/var/lib/burn/state */
	char *repo_dir;  /* $BURN_REPO_DIR or $R/var/lib/burn/repos */
	char *hold_file; /* $R/etc/burn/hold */
	char *profile_file;
	/* every configuration variable: defaults, burn.conf, the profile, the command line and
	 * what flags_setup() derives (CHOST, CARCH, CROSS, ...). Recipes are evaluated on top. */
	shenv cfg;
} burn_state;

extern burn_state B;

/* ---- config.c ---- */
void config_defaults(void);
void config_load(void);
/* evaluate the profile PROFILE (with `inherit`) into env: CARCH, CPU_FLAGS, VIDEO_CARDS, ... */
void profile_load(shenv *env, const char *name, char **file_out);
char *profile_find(const char *name); /* NULL when there is none */
void profile_dirs(svec *out);
void flags_setup(void);
const char *cfg(const char *name); /* "" when unset */
bool cfg_on(const char *name);     /* set to a number other than 0 */
void cfg_set(const char *name, const char *val);
void cfg_words(const char *name, svec *out); /* an array, or the words of a scalar */
void detect_video_cards(svec *out);
const char *host_arch(void); /* uname -m */
int cmd_profile(int argc, char **argv);

/* ---- recipe.c ---- */
extern const char *const RECIPE_VARS[];
extern const char *const RECIPE_CATEGORIES[];
extern const char *const BUILD_TYPES[];
extern const char *const RECIPE_OPTIONS[];

typedef struct {
	char *name;
	bool on;
} feature;

typedef struct {
	char *file; /* RECIPE_FILE */
	char *dir;  /* RECIPE_DIR */
	shenv env;  /* the recipe's variables, on top of B.cfg */
	svec funcs; /* the functions it defines */
	feature *feat;
	size_t nfeat;
	bool resolved;
} recipe;

typedef struct {
	char *name; /* SRC_NAME: the file name in the cache or in the recipe directory */
	char *url;  /* SRC_URL: expanded URL, or the path relative to the recipe directory */
	bool local;
} source_spec;

void recipe_dirs(svec *out);
char *recipe_path(const char *what); /* NULL when not found */
void recipe_names(svec *out);        /* every recipe, the first of each name wins */
recipe *recipe_load(const char *what); /* dies on errors */
recipe *recipe_try_load(const char *what, char **err);
void recipe_free(recipe *r);
const char *rget(const recipe *r, const char *var); /* "" when unset */
void rlist(const recipe *r, const char *var, svec *out);
bool rhas_option(const recipe *r, const char *opt);
void features_resolve(recipe *r);
char *features_string(const recipe *r); /* "+a -b" sorted by name */
bool recipe_use(const recipe *r, const char *feat);
void deps_active(const recipe *r, const svec *deps, svec *out);
char *recipe_rid(const recipe *r);
/* regular files of the recipe directory (find -type f), sorted; with_recipe: the recipe too */
void recipe_files(const recipe *r, bool with_recipe, svec *out);
char *url_expand(const char *s);
void source_info(const char *spec, source_spec *out);
void source_spec_free(source_spec *s);

/* ---- db.c ---- */
bool db_has(const char *name);
void db_list(svec *out);                                   /* installed packages, byte order */
char *db_path(const char *name, const char *file);         /* $DB_DIR/name/file */
void db_lines(const char *name, const char *file, svec *out); /* appends; missing: none */
const char *meta_get(const svec *meta, const char *key);   /* "" when missing */
char *db_get(const char *name, const char *key);           /* NULL when missing */
bool is_held(const char *name);
void need_write(void); /* root for /, and the lock: dies when another burn works on ROOT */

/* ---- cmd_recipe.c ---- */
int cmd_search(int argc, char **argv);
int cmd_info(int argc, char **argv);
int cmd_query(int argc, char **argv);
int cmd_lint(int argc, char **argv);

/* ---- cmd_db.c ---- */
int cmd_list(int argc, char **argv);
int cmd_files(int argc, char **argv);
int cmd_owner(int argc, char **argv);
int cmd_hold(int argc, char **argv);
int cmd_unhold(int argc, char **argv);
int cmd_history(int argc, char **argv);
int cmd_doctor(int argc, char **argv);

#endif
