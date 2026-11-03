/* SPDX-License-Identifier: MIT */
/* burn - the CleanLinux package manager */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "burn.h"
#include "vercmp.h"

static void usage(FILE *f)
{
	fputs("burn " BURN_VERSION " - the CleanLinux package manager (source based, rolling+++)\n"
	      "\n"
	      "Usage: burn [options] <command> [arguments]\n"
	      "\n"
	      "Commands:\n"
	      "  sync                        update the recipe repositories (git)\n"
	      "  search [regex]              find recipes\n"
	      "  info <pkg>                  show a recipe with its resolved features\n"
	      "  query <pkg> <var>           print one recipe variable (for scripts)\n"
	      "  fetch <pkg>...              download (in parallel) and verify sources\n"
	      "  unpack <pkg> [dir]          unpack + patch + prepare sources, print the source dir\n"
	      "  build <pkg>...              build into the package cache (no deps, no install)\n"
	      "  install <pkg|file>...       resolve dependencies, build and install\n"
	      "  remove <pkg>...             remove packages\n"
	      "  list                        list installed packages\n"
	      "  files <pkg>                 list the files of an installed package\n"
	      "  owner <path>                show which package owns a file\n"
	      "  plan                        show what 'update' would do (changes nothing)\n"
	      "  update                      sync, build every update, snapshot, apply, fix ABI breaks\n"
	      "  hold|unhold [pkg]...        pin packages (update skips them); no args: list holds\n"
	      "  history                     show the transaction log\n"
	      "  rollback <pkg>              reinstall the previous build of a package and hold it\n"
	      "  doctor                      check the system (missing and preserved libraries)\n"
	      "  lint [pkg|dir]...           check recipes\n"
	      "  checksum <pkg>              print the sha256 of the sources (--write: into the recipe)\n"
	      "  profile [show|list|detect]  hardware profiles\n"
	      "  vercmp <a> <b>              compare two versions: prints -1, 0 or 1\n"
	      "\n"
	      "Options:\n"
	      "  -r, --root DIR              operate on another root (default: /)\n"
	      "  -p, --profile NAME          hardware profile (default: native)\n"
	      "  -R, --recipes DIR           extra recipe directory, searched first (repeatable)\n"
	      "  -c, --config FILE           extra configuration file\n"
	      "  -j, --jobs N                parallel build jobs\n"
	      "  -f, --features \"a -b\"       enable/disable features for this run\n"
	      "      --target TRIPLE         cross-compile, e.g. aarch64-cleanlinux-linux-gnu\n"
	      "      --host-deps             take build tools from the host (ignore makedepends)\n"
	      "      --check                 run test suites while building\n"
	      "      --nodeps                do not resolve dependencies\n"
	      "      --rebuild               ignore cached builds\n"
	      "      --force                 overwrite conflicting files, allow removing essentials\n"
	      "      --no-sync               'update' without syncing recipes first\n"
	      "      --write                 'checksum': write the sums into the recipe\n"
	      "  -y, --yes                   do not ask for confirmation\n"
	      "  -v, --verbose               show build output\n"
	      "  -h, --help                  show this help\n"
	      "  -V, --version               show the version\n",
	      f);
}

typedef struct {
	const char *name;
	int (*fn)(int argc, char **argv);
} command;

static const command commands[] = {
	{"search", cmd_search}, {"info", cmd_info},       {"query", cmd_query},   {"lint", cmd_lint},
	{"profile", cmd_profile}, {"list", cmd_list},     {"files", cmd_files},   {"owner", cmd_owner},
	{"hold", cmd_hold},     {"unhold", cmd_unhold},   {"history", cmd_history}, {"doctor", cmd_doctor},
	{"install", cmd_install}, {"remove", cmd_remove}, {"uninstall", cmd_remove}, {"rollback", cmd_rollback},
};

/* commands of the bash prototype that are not ported yet */
static const char *const pending[] = {"sync",   "fetch",  "unpack", "build",
				      "plan",   "update", "upgrade", "checksum", NULL};

static bool takes_value(const char *a)
{
	static const char *const opts[] = {"-r", "--root",   "-p", "--profile", "-R",       "--recipes", "-c",
					   "--config", "-j", "--jobs", "-f",      "--features", "--target", NULL};
	for (const char *const *o = opts; *o; o++)
		if (!strcmp(a, *o))
			return true;
	return false;
}

static bool is_eq(const char *a, const char *s, const char *l)
{
	return !strcmp(a, s) || !strcmp(a, l);
}

int main(int argc, char **argv)
{
	log_init();
	/* vercmp needs no configuration: fast and usable from any script */
	if (argc == 4 && !strcmp(argv[1], "vercmp")) {
		printf("%d\n", vercmp(argv[2], argv[3]));
		return 0;
	}
	config_defaults();
	const char *bc = getenv("BURN_CONFIG");
	B.config_file = bc && *bc ? xstrdup(bc) : NULL;
	const char *o_root = NULL, *o_profile = NULL, *o_jobs = NULL, *o_target = NULL;
	bool o_host_deps = false, o_check = false;
	/* static: still reachable when -h or -V returns early */
	static svec o_recipes = SVEC_INIT, args = SVEC_INIT;
	static sbuf feats = SBUF_INIT;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *eq = str_prefix(a, "--") ? strchr(a, '=') : NULL;
		char *opt = eq ? xstrndup(a, (size_t)(eq - a)) : xstrdup(a);
		const char *val = eq ? eq + 1 : NULL;
		if (takes_value(opt)) {
			if (!val) {
				if (i + 1 >= argc)
					die("option %s needs a value", opt);
				val = argv[++i];
			}
			if (is_eq(opt, "-r", "--root")) {
				o_root = val;
			} else if (is_eq(opt, "-p", "--profile")) {
				o_profile = val;
			} else if (is_eq(opt, "-R", "--recipes")) {
				if (!is_dir(val))
					die("recipe directory not found: %s", val);
				sv_push_own(&o_recipes, path_real(val));
			} else if (is_eq(opt, "-c", "--config")) {
				if (!is_file(val))
					die("config file not found: %s", val);
				free(B.config_file);
				B.config_file = path_real(val);
			} else if (is_eq(opt, "-j", "--jobs")) {
				if (!(*val >= '1' && *val <= '9' && str_uint(val)))
					die("--jobs needs a positive number");
				o_jobs = val;
			} else if (is_eq(opt, "-f", "--features")) {
				sb_addf(&feats, " %s", val);
			} else {
				o_target = val;
			}
		} else if (eq) {
			die("unknown option: %s (see: burn --help)", opt);
		} else if (!strcmp(a, "--host-deps")) {
			o_host_deps = true;
		} else if (!strcmp(a, "--check")) {
			o_check = true;
		} else if (!strcmp(a, "--nodeps")) {
			B.nodeps = true;
		} else if (!strcmp(a, "--rebuild")) {
			B.rebuild = true;
		} else if (!strcmp(a, "--force")) {
			B.force = true;
		} else if (!strcmp(a, "--no-sync")) {
			B.no_sync = true;
		} else if (!strcmp(a, "--write")) {
			B.write = true;
		} else if (is_eq(a, "-y", "--yes")) {
			opt_yes = true;
		} else if (is_eq(a, "-v", "--verbose")) {
			B.verbose = true;
		} else if (is_eq(a, "-h", "--help")) {
			free(opt);
			usage(stdout);
			return 0;
		} else if (is_eq(a, "-V", "--version")) {
			free(opt);
			puts("burn " BURN_VERSION);
			return 0;
		} else if (!strcmp(a, "--")) {
			for (i++; i < argc; i++)
				sv_push(&args, argv[i]);
		} else if (a[0] == '-' && a[1]) {
			die("unknown option: %s (see: burn --help)", a);
		} else {
			sv_push(&args, a);
		}
		free(opt);
	}
	if (!args.n) {
		usage(stderr);
		return 2;
	}
	B.cli_features = sb_steal(&feats);

	/* root -> config -> profile -> config again (user settings beat the profile) -> command line */
	if (o_root)
		cfg_set("ROOT", o_root);
	B.root = path_abs(cfg("ROOT"));
	char *R = xstrdup(B.root);
	if (!strcmp(R, "/"))
		R[0] = 0;
	B.R = R;
	cfg_set("ROOT", B.root);
	cfg_set("R", B.R);
	config_load();
	if (o_profile)
		cfg_set("PROFILE", o_profile);
	if (o_target && !o_profile && !strcmp(cfg("PROFILE"), "native")) {
		char *arch = xstrndup(o_target, strcspn(o_target, "-"));
		if (strcmp(arch, host_arch())) {
			char *p = xasprintf("generic-%s", arch);
			cfg_set("PROFILE", p);
			free(p);
		}
		free(arch);
	}
	char *loaded = xstrdup(cfg("PROFILE"));
	profile_load(&B.cfg, loaded, &B.profile_file);
	cfg_set("PROFILE_FILE", B.profile_file);
	config_load();
	cfg_set("ROOT", B.root);
	cfg_set("R", B.R);
	cfg_set("PROFILE", loaded);
	free(loaded);
	if (o_jobs)
		cfg_set("JOBS", o_jobs);
	if (o_target)
		cfg_set("CHOST", o_target);
	if (o_host_deps)
		cfg_set("HOST_DEPS", "1");
	if (o_check)
		cfg_set("CHECK", "1");
	if (o_recipes.n) {
		cfg_words("RECIPE_PATHS", &o_recipes);
		sh_set_list(&B.cfg, "RECIPE_PATHS", &o_recipes);
	}
	B.db_dir = xasprintf("%s/var/lib/burn/installed", B.R);
	B.state_dir = xasprintf("%s/var/lib/burn/state", B.R);
	B.repo_dir = *cfg("BURN_REPO_DIR") ? xstrdup(cfg("BURN_REPO_DIR")) : xasprintf("%s/var/lib/burn/repos", B.R);
	B.hold_file = xasprintf("%s/etc/burn/hold", B.R);
	cfg_set("DB_DIR", B.db_dir);
	cfg_set("STATE_DIR", B.state_dir);
	cfg_set("REPO_DIR", B.repo_dir);
	cfg_set("HOLD_FILE", B.hold_file);
	flags_setup();

	const char *cmd = args.v[0];
	int rc = -1;
	if (!strcmp(cmd, "help")) {
		usage(stdout);
		rc = 0;
	} else if (!strcmp(cmd, "vercmp")) {
		if (args.n != 3)
			die("usage: burn vercmp <a> <b>");
		printf("%d\n", vercmp(args.v[1], args.v[2]));
		rc = 0;
	}
	for (size_t i = 0; rc < 0 && i < ARRAY_LEN(commands); i++)
		if (!strcmp(cmd, commands[i].name))
			rc = commands[i].fn((int)args.n - 1, args.v + 1);
	for (const char *const *p = pending; rc < 0 && *p; p++)
		if (!strcmp(cmd, *p))
			die("'%s' is not in burn %s yet: use the bash prototype (CleanLinux/burn/burn) meanwhile", cmd,
			    BURN_VERSION);
	if (rc < 0)
		die("unknown command: %s (see: burn --help)", cmd);
	fflush(stdout);
	sv_free(&args);
	sv_free(&o_recipes);
	return rc;
}
