/* SPDX-License-Identifier: MIT */
/* Configuration: defaults, burn.conf, hardware profiles and the target (CHOST, CPU flags). */
#include "burn.h"

#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>
#ifdef __x86_64__
#include <cpuid.h>
#endif

#include "spawn.h"

burn_state B;

const char *cfg(const char *name)
{
	const char *v = sh_get(&B.cfg, name);
	return v ? v : "";
}

bool cfg_on(const char *name)
{
	return atol(cfg(name)) != 0;
}

void cfg_set(const char *name, const char *val)
{
	sh_set(&B.cfg, name, val);
}

void cfg_words(const char *name, svec *out)
{
	const shvar *v = sh_find(&B.cfg, name);
	if (v && v->array)
		sv_extend(out, &v->val);
	else
		sv_split_ws(out, cfg(name));
}

static void cfg_list(const char *name, int n, const char *const *vals)
{
	svec l = SVEC_INIT;
	for (int i = 0; i < n; i++)
		sv_push(&l, vals[i]);
	sh_set_list(&B.cfg, name, &l);
	sv_free(&l);
}

const char *host_arch(void)
{
	static char arch[65];
	if (!arch[0]) {
		struct utsname u;
		if (uname(&u) < 0)
			die_errno("uname");
		snprintf(arch, sizeof arch, "%s", u.machine);
	}
	return arch;
}

static const char *env_or(const char *name, const char *def)
{
	const char *v = getenv(name);
	return v && *v ? v : def;
}

void config_defaults(void)
{
	sh_init(&B.cfg, NULL, true);
	cfg_set("ROOT", env_or("BURN_ROOT", "/"));
	if (geteuid() == 0) {
		cfg_set("CACHE_DIR", env_or("BURN_CACHE", "/var/cache/burn"));
		cfg_set("WORK_DIR", env_or("BURN_WORK", "/var/tmp/burn"));
	} else {
		const char *xdg = env_or("XDG_CACHE_HOME", NULL);
		char *cache = xdg ? xasprintf("%s/burn", xdg) : xasprintf("%s/.cache/burn", env_or("HOME", ""));
		cfg_set("CACHE_DIR", env_or("BURN_CACHE", cache));
		char *work = xasprintf("%s/work", cfg("CACHE_DIR"));
		cfg_set("WORK_DIR", env_or("BURN_WORK", work));
		free(work);
		free(cache);
	}
	static const char *const repos[] = {"https://github.com/vovawees/CleanLinux-Recipes.git"};
	cfg_list("RECIPE_REPOS", 1, repos);
	cfg_list("RECIPE_PATHS", 0, NULL);
	cfg_set("CHANNEL", "main");
	cfg_set("PROFILE", env_or("BURN_PROFILE", "native"));
	char jobs[32];
	snprintf(jobs, sizeof jobs, "%ld", nproc());
	cfg_set("JOBS", jobs);
	static const char *const scalars[][2] = {
		{"FETCH_JOBS", "4"},
		{"FEATURES", ""},
		{"LOCALES", "en ru"},
		{"KEEP_DOCS", "0"},
		{"KEEP_INFO", "0"},
		{"KEEP_MAN", "1"},
		{"STRIP_BINARIES", "1"},
		{"LTO", "1"},
		{"CHECK", "0"},
		{"HOST_DEPS", "0"},
		{"CCACHE", "0"},
		{"CCACHE_SIZE", "5G"},
		{"LINKER", ""},
		{"MESON_AUTO_FEATURES", "disabled"},
		{"OPT_FLAGS", "-O2 -pipe"},
		{"HARDENING_FLAGS", "-fstack-clash-protection -Wp,-D_FORTIFY_SOURCE=3"},
		{"LDFLAGS_BASE", "-Wl,-O1 -Wl,--as-needed -Wl,--sort-common -Wl,-z,relro -Wl,-z,now "
				 "-Wl,-z,pack-relative-relocs"},
		{"GNU_MIRROR", "https://ftpmirror.gnu.org/gnu"},
		{"KERNEL_MIRROR", "https://cdn.kernel.org/pub/linux"},
		{"PKG_COMPRESS", "zstd -q -T0 -10"},
		{"CHOST", ""},
	};
	for (size_t i = 0; i < ARRAY_LEN(scalars); i++)
		cfg_set(scalars[i][0], scalars[i][1]);
	cfg_list("ASSUME_INSTALLED", 0, NULL);
	static const char *const essential[] = {"filesystem", "glibc", "bash", "coreutils", "burn"};
	cfg_list("ESSENTIAL", ARRAY_LEN(essential), essential);
	static const char *const core[] = {"glibc", "gcc", "binutils", "linux", "mesa", "llvm"};
	cfg_list("CORE_PKGS", ARRAY_LEN(core), core);
}

static void eval_or_die(shctx *c, const char *path)
{
	if (sh_eval_file(c, path) < 0)
		die("%s", c->err);
}

/* $ROOT/etc/burn/burn.conf, then -c FILE ($BURN_CONFIG) */
void config_load(void)
{
	char *sys = xasprintf("%s/etc/burn/burn.conf", B.R ? B.R : "");
	const char *files[] = {sys, B.config_file};
	for (size_t i = 0; i < ARRAY_LEN(files); i++) {
		if (!files[i] || !*files[i] || !is_file(files[i]))
			continue;
		shctx c;
		sh_ctx_init(&c, &B.cfg);
		eval_or_die(&c, files[i]);
		sh_ctx_free(&c);
	}
	free(sys);
}

/* ======================================================================= profiles */

static char *exe_dir(void)
{
	char buf[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
	if (n <= 0)
		return NULL;
	buf[n] = 0;
	return path_dir(buf);
}

void profile_dirs(svec *out)
{
	svec cand = SVEC_INIT;
	const char *env = cfg("BURN_PROFILE_DIR");
	if (*env)
		sv_push(&cand, env);
	sv_pushf(&cand, "%s/etc/burn/profiles", B.R ? B.R : "");
	char *exe = exe_dir();
	if (exe) {
		/* a source tree (profiles/ next to the binary) or an installation under a prefix */
		sv_pushf(&cand, "%s/profiles", exe);
		sv_pushf(&cand, "%s/../share/burn/profiles", exe);
		free(exe);
	}
	sv_push(&cand, "/usr/share/burn/profiles");
	for (size_t i = 0; i < cand.n; i++) {
		if (!is_dir(cand.v[i]))
			continue;
		char *r = path_real(cand.v[i]);
		if (r && !sv_has(out, r))
			sv_push(out, r);
		free(r);
	}
	sv_free(&cand);
}

char *profile_find(const char *name)
{
	svec dirs = SVEC_INIT;
	char *found = NULL;
	profile_dirs(&dirs);
	for (size_t i = 0; i < dirs.n && !found; i++) {
		char *a = xasprintf("%s/%s.profile", dirs.v[i], name);
		char *b = xasprintf("%s/devices/%s.profile", dirs.v[i], name);
		if (is_file(a))
			found = path_real(a);
		else if (is_file(b))
			found = path_real(b);
		free(a);
		free(b);
	}
	sv_free(&dirs);
	return found;
}

/* GPU drivers of this machine, in VIDEO_CARDS words (amd intel nouveau freedreno panfrost ...) */
void detect_video_cards(svec *out)
{
	static const char *const map[][2] = {
		{"amdgpu", "amd"},      {"radeon", "amd"},      {"i915", "intel"},  {"xe", "intel"},
		{"nouveau", "nouveau"}, {"msm*", "freedreno"},  {"panfrost", "panfrost"},
		{"panthor", "panfrost"}, {"v3d", "v3d"},         {"vc4", "v3d"},     {"lima", "lima"},
		{"etnaviv", "etnaviv"}, {"virtio*", "virgl"},
	};
	glob_t g;
	svec cards = SVEC_INIT;
	if (glob("/sys/class/drm/card[0-9]*/device", 0, NULL, &g) == 0) {
		for (size_t i = 0; i < g.gl_pathc; i++) {
			char *drv = xasprintf("%s/driver", g.gl_pathv[i]);
			char *real = exists(drv) ? path_real(drv) : NULL;
			if (real)
				for (size_t k = 0; k < ARRAY_LEN(map); k++)
					if (!fnmatch(map[k][0], path_base(real), 0)) {
						sv_push(&cards, map[k][1]);
						break;
					}
			free(real);
			free(drv);
		}
		globfree(&g);
	}
	sv_sort_uniq(&cards);
	sv_extend(out, &cards);
	sv_free(&cards);
}

typedef struct {
	int depth;
	char *err;
} inherit_state;

static int profile_command(shctx *c, svec *argv)
{
	if (strcmp(argv->v[0], "inherit"))
		return 1;
	inherit_state *st = c->user;
	const char *name = argv->n > 1 ? argv->v[1] : "";
	char *p = profile_find(name);
	if (!p)
		return sh_error(c, "inherited profile '%s' not found", name);
	if (st->depth >= 16) {
		free(p);
		return sh_error(c, "inherit %s: profiles inherit too deep (a cycle?)", name);
	}
	st->depth++;
	char *saved = xstrdup(sh_get(c->env, "PROFILE_DIR") ? sh_get(c->env, "PROFILE_DIR") : "");
	char *dir = path_dir(p);
	sh_set(c->env, "PROFILE_DIR", dir);
	int rc = sh_eval_file(c, p);
	sh_set(c->env, "PROFILE_DIR", saved);
	free(dir);
	free(saved);
	st->depth--;
	free(p);
	return rc < 0 ? -1 : 0;
}

/* profile NAME evaluated into env; 0, or -1 with *err */
static int profile_eval(shenv *env, const char *name, char **file_out, char **err)
{
	static const char *const reset[] = {"VIDEO_CARDS",   "PROFILE_FEATURES", "PROFILE_DESC", "EXTRA_CFLAGS",
					    "EXTRA_LDFLAGS", "KERNEL_CMDLINE",   "KERNEL_CONFIG", "KERNEL_FRAGMENT",
					    "FIRMWARE",      "MODULES",          "QUIRKS"};
	sh_set(env, "CARCH", host_arch());
	sh_set(env, "CPU_FLAGS", "native");
	for (size_t i = 0; i < ARRAY_LEN(reset); i++)
		sh_set(env, reset[i], "");
	/* what native.profile describes: this machine */
	static char *host_cards;
	if (!host_cards && getenv("HOST_VIDEO_CARDS")) {
		host_cards = xstrdup(getenv("HOST_VIDEO_CARDS"));
	} else if (!host_cards) {
		svec cards = SVEC_INIT;
		detect_video_cards(&cards);
		host_cards = sv_join(&cards, " ");
		sv_free(&cards);
	}
	sh_set(env, "HOST_ARCH", host_arch());
	sh_set(env, "HOST_VIDEO_CARDS", host_cards);
	char *file = profile_find(name);
	if (!file) {
		*err = xasprintf("profile '%s' not found (see: burn profile list)", name);
		return -1;
	}
	inherit_state st = {0};
	shctx c;
	sh_ctx_init(&c, env);
	c.command = profile_command;
	c.user = &st;
	/* PROFILE_DIR: the directory of the profile file being read, for the files that come with it
	 * (KERNEL_FRAGMENT="$PROFILE_DIR/steam-deck.config", QUIRKS="$PROFILE_DIR/steam-deck.d") */
	char *pdir = path_dir(file);
	sh_set(env, "PROFILE_DIR", pdir);
	free(pdir);
	int rc = sh_eval_file(&c, file);
	sh_unset(env, "PROFILE_DIR");
	if (rc < 0) {
		*err = c.err;
		c.err = NULL;
		free(file);
	} else {
		sh_set(env, "PROFILE_ARCH", sh_get(env, "CARCH") ? sh_get(env, "CARCH") : "");
		if (file_out)
			*file_out = file;
		else
			free(file);
	}
	sh_ctx_free(&c);
	return rc;
}

void profile_load(shenv *env, const char *name, char **file_out)
{
	char *err = NULL;
	if (profile_eval(env, name, file_out, &err) < 0)
		die("%s", err);
}

/* ======================================================================= the target */

void flags_setup(void)
{
	const char *host = host_arch();
	char *cbuild = NULL;
	static const char *const ccs[] = {"cc", "gcc"};
	for (size_t i = 0; i < ARRAY_LEN(ccs) && !cbuild; i++) {
		char *argv[] = {(char *)ccs[i], "-dumpmachine", NULL};
		run_opts o = {.quiet = true, .stdin_null = true};
		cbuild = run_output(argv, &o);
		if (cbuild && !*cbuild) {
			free(cbuild);
			cbuild = NULL;
		}
	}
	if (!cbuild)
		cbuild = xasprintf("%s-pc-linux-gnu", host);
	cfg_set("CBUILD", cbuild);
	const char *parch = cfg("PROFILE_ARCH");
	if (!*cfg("CHOST")) {
		if (!strcmp(parch, host)) {
			cfg_set("CHOST", cbuild);
		} else {
			char *t = xasprintf("%s-cleanlinux-linux-gnu", parch);
			cfg_set("CHOST", t);
			free(t);
		}
	}
	char *carch = xstrdup(cfg("CHOST"));
	char *dash = strchr(carch, '-');
	if (dash)
		*dash = 0;
	cfg_set("CARCH", carch);
	if (strcmp(carch, parch))
		die("profile '%s' is for %s, but the target is %s", cfg("PROFILE"), parch, cfg("CHOST"));
	cfg_set("CROSS", strcmp(cfg("CHOST"), cbuild) ? "1" : "0");
	cfg_set("KARCH", !strcmp(carch, "x86_64") ? "x86" : !strcmp(carch, "aarch64") ? "arm64" : carch);
	cfg_set("SYSROOT", B.root);
	const char *cpu = cfg("CPU_FLAGS");
	if (!strcmp(cpu, "native")) {
		if (strcmp(carch, host))
			die("CPU_FLAGS=native cannot target %s from %s; use a device profile", carch, host);
		cpu = !strcmp(carch, "aarch64") ? "-mcpu=native" : "-march=native";
	}
	char *c = xstrdup(cpu);
	cfg_set("CPU_CFLAGS", c);
	free(c);
	free(carch);
	free(cbuild);
}

/* ======================================================================= burn profile */

static void kv(const char *k, const char *v)
{
	printf("%-12s %s\n", k, v);
}

static const char *or_dash(const char *s)
{
	return *s ? s : "-";
}

static int profile_show(void)
{
	svec w = SVEC_INIT;
	char *both = xasprintf("%s %s", cfg("PROFILE_FEATURES"), cfg("FEATURES"));
	sv_split_ws(&w, both);
	char *features = sv_join(&w, " ");
	char *cflags = xasprintf("%s %s %s%s%s", cfg("OPT_FLAGS"), cfg("CPU_CFLAGS"), cfg("HARDENING_FLAGS"),
				 *cfg("EXTRA_CFLAGS") ? " " : "", cfg("EXTRA_CFLAGS"));
	char *ldflags = xasprintf("%s%s%s", cfg("LDFLAGS_BASE"), *cfg("EXTRA_LDFLAGS") ? " " : "", cfg("EXTRA_LDFLAGS"));
	kv("profile", cfg("PROFILE"));
	kv("file", B.profile_file ? B.profile_file : "");
	kv("description", or_dash(cfg("PROFILE_DESC")));
	kv("chost", cfg("CHOST"));
	kv("cross", cfg("CROSS"));
	kv("cpu", cfg("CPU_CFLAGS"));
	kv("video_cards", or_dash(cfg("VIDEO_CARDS")));
	kv("kernel_config", or_dash(cfg("KERNEL_CONFIG")));
	kv("kernel_fragment", or_dash(cfg("KERNEL_FRAGMENT")));
	kv("kernel_cmdline", or_dash(cfg("KERNEL_CMDLINE")));
	kv("firmware", or_dash(cfg("FIRMWARE")));
	kv("modules", or_dash(cfg("MODULES")));
	kv("quirks", or_dash(cfg("QUIRKS")));
	kv("features", features);
	kv("cflags", cflags);
	kv("ldflags", ldflags);
	kv("lto", cfg("LTO"));
	free(both);
	free(features);
	free(cflags);
	free(ldflags);
	sv_free(&w);
	return 0;
}

static int profile_list(void)
{
	svec dirs = SVEC_INIT;
	profile_dirs(&dirs);
	for (size_t i = 0; i < dirs.n; i++) {
		static const char *const sub[] = {"", "devices/"};
		for (size_t k = 0; k < ARRAY_LEN(sub); k++) {
			char *pat = xasprintf("%s/%s*.profile", dirs.v[i], sub[k]);
			glob_t g;
			if (glob(pat, 0, NULL, &g) == 0) {
				for (size_t f = 0; f < g.gl_pathc; f++) {
					char *n = xstrdup(path_base(g.gl_pathv[f]));
					n[strlen(n) - strlen(".profile")] = 0;
					shenv env;
					sh_init(&env, &B.cfg, false);
					char *err = NULL;
					if (profile_eval(&env, n, NULL, &err) == 0) {
						const char *a = sh_get(&env, "CARCH"), *d = sh_get(&env, "PROFILE_DESC");
						printf("%-18s %-8s %s\n", n, a ? a : "", d ? d : "");
					}
					free(err);
					sh_free(&env);
					free(n);
				}
				globfree(&g);
			}
			free(pat);
		}
	}
	sv_free(&dirs);
	return 0;
}

/* the first line of a sysfs file, NULs removed (device tree strings end with one) */
static char *sysfs_line(const char *path)
{
	size_t len;
	char *s = file_read(path, &len);
	if (!s)
		return NULL;
	size_t o = 0;
	for (size_t i = 0; i < len; i++)
		if (s[i])
			s[o++] = s[i];
	s[o] = 0;
	while (o && s[o - 1] == '\n')
		s[--o] = 0;
	return s;
}

#ifdef __x86_64__
/* the x86-64 microarchitecture level of this CPU (x86-64, x86-64-v2, -v3, -v4), from CPUID */
static const char *x86_level(void)
{
	unsigned a, b, c, d, c1, b7 = 0, ce = 0;
	if (!__get_cpuid(1, &a, &b, &c1, &d))
		return "x86-64";
	if (__get_cpuid_max(0, NULL) >= 7)
		__cpuid_count(7, 0, a, b7, c, d);
	if (__get_cpuid(0x80000001, &a, &b, &ce, &d) == 0)
		ce = 0;
	unsigned long long xcr0 = 0;
	if (c1 & bit_OSXSAVE) {
		unsigned lo, hi;
		__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
		xcr0 = (unsigned long long)hi << 32 | lo;
	}
	bool v2 = (c1 & bit_SSE3) && (c1 & bit_SSSE3) && (c1 & bit_SSE4_1) && (c1 & bit_SSE4_2) &&
		  (c1 & bit_POPCNT) && (c1 & bit_CMPXCHG16B) && (ce & bit_LAHF_LM);
	bool v3 = v2 && (c1 & bit_AVX) && (c1 & bit_FMA) && (c1 & bit_F16C) && (c1 & bit_MOVBE) &&
		  (b7 & bit_AVX2) && (b7 & bit_BMI) && (b7 & bit_BMI2) && (ce & bit_ABM) && (xcr0 & 6) == 6;
	bool v4 = v3 && (b7 & bit_AVX512F) && (b7 & bit_AVX512DQ) && (b7 & bit_AVX512CD) && (b7 & bit_AVX512BW) &&
		  (b7 & bit_AVX512VL) && (xcr0 & 0xe6) == 0xe6;
	return v4 ? "x86-64-v4" : v3 ? "x86-64-v3" : v2 ? "x86-64-v2" : "x86-64";
}
#endif

/* what -march=native / -mcpu=native means here, as GCC itself resolves it */
static char *detect_cpu(const char *arch)
{
	bool arm = !strcmp(arch, "aarch64");
	const char *flag = arm ? "-mcpu=native" : "-march=native";
	const char *want = arm ? "-mcpu=" : "-march=";
	char *argv[] = {"gcc", (char *)flag, "-###", "-E", "-", NULL};
	run_opts o = {.stdin_null = true, .err_to_out = true};
	sbuf out = SBUF_INIT;
	o.out = &out;
	char *found = NULL;
	if (have("gcc") && run(argv, &o) == 0) {
		for (char *line = out.s; line && *line && !found;) {
			char *nl = strchr(line, '\n');
			if (nl)
				*nl = 0;
			if (strstr(line, "cc1")) {
				char *m = strstr(line, want);
				if (m) {
					size_t n = strcspn(m, " \"'");
					found = xstrndup(m, n);
				}
			}
			line = nl ? nl + 1 : NULL;
		}
	}
	sb_free(&out);
#ifdef __x86_64__
	if (!found && !arm)
		found = xasprintf("-march=%s", x86_level()); /* no GCC: at least the microarchitecture level */
#endif
	return found;
}

static int profile_detect(void)
{
	static const char *const devices[][2] = {
		{"*Jupiter*", "steam-deck"},
		{"*Galileo*", "steam-deck"},
		{"*RC73XA*", "rog-xbox-ally-x"},
		{"*ROG Ally*", "rog-ally"},
		{"*RC71L*", "rog-ally"},
		{"*RC72L*", "rog-ally"},
		{"*Legion Go S*", "legion-go-s"},
		{"*Legion Go*", "legion-go"},
		{"*Claw*AI+*", "msi-claw-ai-plus"},
		{"*Claw A1M*", "msi-claw"},
		{"*X540YA*", "asus-x540ya"},
		{"*Raspberry Pi 5*", "raspberry-pi-5"},
		{"*Retroid Pocket 5*", "retroid-pocket-5"},
		{"*Retroid Pocket Mini*", "retroid-pocket-5"},
		{"*Odin 2*", "ayn-odin2"},
		{"*Odin2*", "ayn-odin2"},
		{"*[Mm]angmi*", "mangmi-air-x"},
		{"*RG353*", "rk3566-handheld"},
		{"*RG503*", "rk3566-handheld"},
		{"*RK3566*", "rk3566-handheld"},
		{"*rk3566*", "rk3566-handheld"},
	};
	const char *arch = host_arch();
	char *cpu = detect_cpu(arch);
	svec cards = SVEC_INIT;
	detect_video_cards(&cards);
	char *gpu = sv_join(&cards, " ");
	char *model = sysfs_line("/sys/firmware/devicetree/base/model");
	if (!model)
		model = sysfs_line("/sys/class/dmi/id/product_name");
	/* Lenovo keeps the model name in product_version, ASUS in board_name */
	svec idv = SVEC_INIT;
	static const char *const idf[] = {"/sys/class/dmi/id/product_version", "/sys/class/dmi/id/board_name"};
	for (size_t i = 0; i < ARRAY_LEN(idf); i++)
		file_lines(idf[i], &idv);
	char *ids = sv_join(&idv, " ");
	char *hay = xasprintf("%s %s", model ? model : "", ids);
	const char *suggestion = "native";
	for (size_t i = 0; i < ARRAY_LEN(devices); i++)
		if (!fnmatch(devices[i][0], hay, 0)) {
			suggestion = devices[i][1];
			break;
		}
	printf("%-10s %s\n", "arch", arch);
	printf("%-10s %s\n", "model", model && *model ? model : "unknown");
	printf("%-10s %s\n", "cpu", cpu && *cpu ? cpu : "unknown");
	printf("%-10s %s\n", "gpu", *gpu ? gpu : "unknown");
	printf("%-10s %s\n", "profile", suggestion);
	if (!strcmp(suggestion, "native")) {
		printf("\nNo device profile matched. A starting point for /etc/burn/profiles/my-device.profile:\n\n");
		printf("PROFILE_DESC=\"My device\"\nCARCH=%s\nCPU_FLAGS=\"%s\"\nVIDEO_CARDS=\"%s\"\n", arch,
		       cpu && *cpu ? cpu : "native", gpu);
	}
	free(cpu);
	free(gpu);
	free(model);
	free(ids);
	free(hay);
	sv_free(&idv);
	sv_free(&cards);
	return 0;
}

int cmd_profile(int argc, char **argv)
{
	const char *sub = argc > 0 ? argv[0] : "show";
	if (!strcmp(sub, "show"))
		return profile_show();
	if (!strcmp(sub, "list"))
		return profile_list();
	if (!strcmp(sub, "detect"))
		return profile_detect();
	die("usage: burn profile [show|list|detect]");
}
