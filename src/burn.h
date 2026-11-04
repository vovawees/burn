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
	bool verbose, force, rebuild, nodeps, no_sync, write, security;
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
	char *output; /* the package asked for: name, or one of split=() */
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
void recipe_allowed(const recipe *r); /* dies on category nonfree without ALLOW_NONFREE=1 */
bool rhas_option(const recipe *r, const char *opt);
void features_resolve(recipe *r);
char *features_string(const recipe *r); /* "+a -b" sorted by name */
bool recipe_use(const recipe *r, const char *feat);
void deps_active(const recipe *r, const svec *deps, svec *out);
void files_under(const char *dir, svec *out); /* regular files below dir, recursively, sorted */
char *recipe_rid(const recipe *r);
/* split packages: split=(NAME...) with files_KEY, summary_KEY, depends_KEY, ... */
char *split_key(const char *name); /* gcc-libs -> gcc_libs */
bool recipe_has_split(const recipe *r, const char *name);
void recipe_outputs(const recipe *r, svec *out); /* name, then the splits */
bool recipe_is_main(const recipe *r, const char *output);
bool split_isset(const recipe *r, const char *split, const char *field);
const char *output_get(const recipe *r, const char *output, const char *field);
void output_list(const recipe *r, const char *output, const char *field, svec *out);
/* the prototype's recipe_reset as shell code, with RECIPE_FILE and RECIPE_DIR (for env.sh) */
void recipe_reset_sh(const recipe *r, sbuf *s);
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

/* ---- pkg.c ---- */
extern svec txn_lost; /* sonames that upgrades of this command took away (ABI follow-up) */
void pkg_install_file(const char *file, const char *op); /* op: NULL (install/upgrade), "rollback" */
void pkg_remove(const char *name);
void triggers_run(void); /* ldconfig and depmod: once for the whole command */
void preserved_cleanup(void); /* delete preserved libraries nothing needs any more */
void history_add(const char *op, const char *name, const char *old, const char *new, const char *file);
int root_run(const char *prog, char *const args[], bool quiet); /* $R/usr/bin/PROG, run here */

/* ---- source.c ---- */
char *source_path(const recipe *r, const char *spec); /* the recipe directory or the source cache */
void fetch_sources(recipe *r, bool verify);          /* dies on a missing source or a bad sum */
void prefetch(const svec *names);                    /* every remote source, FETCH_JOBS at a time */
int download_quiet(const char *url, const char *file); /* also file://; 0 or not, no noise */
int cmd_fetch(int argc, char **argv);
int cmd_checksum(int argc, char **argv);

/* ---- flags.c: the build environment of a recipe ---- */
typedef struct {
	recipe *r;
	shenv v;       /* the build's variables (toolchain, flags, paths), on top of the recipe's */
	char *envfile; /* WORK/env.sh: the shell state of recipe functions (shrun.c) */
	int log_fd;    /* the build log, appended to; -1 before it is open */
} build;
void build_init(build *b, recipe *r); /* takes r */
void build_free(build *b);
const char *bget(const build *b, const char *name); /* "" when unset */
bool bon(const build *b, const char *name);         /* ((name)) */
void bset(build *b, const char *name, const char *val);
void bexport(build *b, const char *name, const char *val);
/* the environment of programs the build runs: ours and every exported variable */
void build_environ(const build *b, svec *env);
long long recipe_epoch(const recipe *r);
/* the prototype's recipe_env: toolchain, CFLAGS/LDFLAGS, BUILDID, ccache */
void recipe_env(build *b);

/* ---- buildtype.c: build_type= in C ---- */
typedef struct bt_ctx {
	const shenv *v; /* the variables: SRC, BUILD, CFLAGS, configure_args, ... */
	svec *env;      /* the environment of the programs */
	/* the lines a recipe function (configure_extra, ...) prints: its extra arguments */
	void (*extra)(struct bt_ctx *c, const char *fn, svec *out);
	void *user;
} bt_ctx;
bool bt_known(const char *type);
bool bt_has(const char *type, const char *phase);
int bt_run(const char *type, const char *phase, bt_ctx *c); /* exit status */
int bt_main(int argc, char **argv);                         /* burn __bt TYPE PHASE ... */

/* ---- shrun.c: recipe functions, run by bash ---- */
void shrun_write_env(build *b, const char *type, const char *file); /* type NULL: no bt_* (unpack) */
/* argv for bash running FN (cd "$SRC" first when in_src); free with sv_free */
void shrun_argv(const build *b, const char *fn, bool in_src, svec *argv);
/* the lines FN prints, run here (mapfile -t), when the recipe defines FN */
void shrun_lines(const build *b, char **envp, const char *fn, svec *out);

/* ---- build.c ---- */
char *build_pkg(const char *name); /* the package file in the cache, built unless cached */
int cmd_build(int argc, char **argv);
int cmd_unpack(int argc, char **argv);

/* ---- deps.c ---- */
void deps_resolve(const svec *names, svec *order); /* what to build, dependencies first */
void deps_order_set(const svec *names, svec *order); /* NAMES, dependencies inside the set first */
bool pkg_uptodate(const char *name);               /* installed from this recipe and features */

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

/* ---- cmd_pkg.c ---- */
int cmd_install(int argc, char **argv);
int cmd_remove(int argc, char **argv);
/* install NAMES with their dependencies (what is up to date is skipped), like burn install */
void install_packages(const svec *names);
/* remove NAMES, dependents first (no question asked), like burn remove */
void remove_packages(const svec *names);
int cmd_rollback(int argc, char **argv);
int cmd_clone(int argc, char **argv);
void abi_followup(void); /* rebuild the packages linked against sonames this command took away */
/* build NAMES in this order, installing each right away (op: the history word, NULL: install) */
void build_install(const svec *names, const char *op);

/* ---- update.c ---- */
int cmd_sync(int argc, char **argv);
int cmd_plan(int argc, char **argv);
int cmd_update(int argc, char **argv);

/* ---- spin.c ---- */
char *spin_root(void);          /* $R/var/lib/burn/spins */
void spin_dirs(svec *out);      /* the added spins, by name */
void spin_setup(void);          /* SPINS_FEATURES: the features of every spin */
void spin_package_features_files(svec *out);
void spin_missing(svec *names, svec *why); /* packages spins want that are not installed */
void spin_sync(void);           /* pull the git spins */
void spin_note_installed(const svec *before); /* burn update installed what spins wanted */
int cmd_spin(int argc, char **argv);

/* ---- snapshot.c ---- */
void snapshot_take(bool required); /* the btrfs snapshot of ROOT before an update */
int cmd_snapshot(int argc, char **argv);

/* ---- sign.c ---- */
char *keys_dir(void);                                  /* $ROOT/etc/burn/keys: the trusted NAME.pub */
void sign_file(const char *file, const char *key);     /* FILE.sig with the private key KEY */
void sign_built(const char *file);                     /* with SIGNING_KEY, when it is set */
/* NULL: SIGFILE is a good signature of FILE (named NAME) by a trusted key; else why not */
char *verify_file(const char *file, const char *name, const char *sigfile);
bool bincache_fetch(const svec *outs);                 /* the builds OUTS from BINARY_CACHES, signed */
int cmd_sign(int argc, char **argv);
int cmd_verify(int argc, char **argv);
int cmd_key(int argc, char **argv);

#endif
