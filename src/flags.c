/* SPDX-License-Identifier: MIT */
/*
 * The build environment of a recipe, as the prototype's recipe_env made it: the toolchain (native
 * or cross), CFLAGS and LDFLAGS from the profile and the recipe's options (with probes of what the
 * toolchain and the libc can do), ccache, the identity of the exact build (BUILDID) and its time
 * (SOURCE_DATE_EPOCH). The values live in the build's own layer of variables on top of the recipe,
 * so recipe functions and build types see them like the prototype's globals.
 */
#include "burn.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sha256.h"
#include "spawn.h"

void build_init(build *b, recipe *r)
{
	memset(b, 0, sizeof *b);
	b->r = r;
	b->log_fd = -1;
	sh_init(&b->v, &r->env, false);
}

void build_free(build *b)
{
	if (b->log_fd >= 0)
		close(b->log_fd);
	sh_free(&b->v);
	recipe_free(b->r);
	free(b->envfile);
	memset(b, 0, sizeof *b);
}

const char *bget(const build *b, const char *name)
{
	const char *v = sh_get(&b->v, name);
	return v ? v : "";
}

bool bon(const build *b, const char *name)
{
	return strtoll(bget(b, name), NULL, 10) != 0;
}

void bset(build *b, const char *name, const char *val)
{
	sh_set(&b->v, name, val);
}

void bexport(build *b, const char *name, const char *val)
{
	sh_set(&b->v, name, val);
	sh_export(&b->v, name);
}

static void bappend(build *b, const char *name, const char *s)
{
	char *v = xasprintf("%s%s", bget(b, name), s);
	bset(b, name, v);
	free(v);
}

/* ${NAME/old/new}: the first occurrence */
static void breplace(build *b, const char *name, const char *old, const char *new)
{
	const char *v = bget(b, name), *at = strstr(v, old);
	if (!at)
		return;
	char *r = xasprintf("%.*s%s%s", (int)(at - v), v, new, at + strlen(old));
	bset(b, name, r);
	free(r);
}

/* the environment of programs the build runs: ours with every exported variable of the
 * configuration, the recipe and the build applied (bash's rules: arrays are not exported, unset
 * removes) */
void build_environ(const build *b, svec *env)
{
	env_from_environ(env);
	const shenv *layers[] = {&B.cfg, &b->r->env, &b->v};
	for (size_t l = 0; l < ARRAY_LEN(layers); l++) {
		const shenv *e = layers[l];
		for (size_t i = 0; i < e->n; i++) {
			const shvar *v = &e->v[i];
			if (v->unset || (v->exported && v->array))
				env_unset(env, v->name);
			else if (v->exported)
				env_set(env, v->name, v->val.n ? v->val.v[0] : "");
		}
	}
}

/* ---- the toolchain ---- */

static void toolchain_env(build *b)
{
	static const char *const tools[][3] = {
		{"CC", "gcc", "cc"},         {"CXX", "g++", "c++"},       {"LD", "ld", "ld"},
		{"AS", "as", "as"},          {"AR", "ar", "ar"},          {"NM", "nm", "nm"},
		{"RANLIB", "ranlib", "ranlib"}, {"STRIP", "strip", "strip"}, {"OBJCOPY", "objcopy", "objcopy"},
		{"OBJDUMP", "objdump", "objdump"}, {"READELF", "readelf", "readelf"}};
	bool cross = bon(b, "CROSS");
	for (size_t i = 0; i < ARRAY_LEN(tools); i++) {
		char *v = cross ? xasprintf("%s-%s", bget(b, "CHOST"), tools[i][1])
				: xstrdup(*bget(b, tools[i][0]) ? bget(b, tools[i][0]) : tools[i][2]);
		bexport(b, tools[i][0], v);
		free(v);
	}
	if (cross) {
		const char *sys = bget(b, "SYSROOT");
		char *libdir = xasprintf("%s/usr/lib/pkgconfig:%s/usr/share/pkgconfig", sys, sys);
		bexport(b, "PKG_CONFIG_SYSROOT_DIR", sys);
		bexport(b, "PKG_CONFIG_PATH", "");
		bexport(b, "PKG_CONFIG_LIBDIR", libdir);
		free(libdir);
		if (!have(bget(b, "CC")))
			die("cross compiler %s not found in PATH (build the toolchain first: bootstrap/bootstrap.sh)",
			    bget(b, "CC"));
	}
}

/* a scratch directory for the probes */
static char *probe_dir(void)
{
	const char *t = getenv("TMPDIR");
	char *d = xasprintf("%s/burn-probe.XXXXXX", t && *t ? t : "/tmp");
	if (!mkdtemp(d))
		die_errno("mkdtemp %s", d);
	return d;
}

static bool quiet_ok(char *const argv[])
{
	run_opts o = {.quiet = true, .stdin_null = true};
	return run(argv, &o) == 0;
}

/* DT_RELR (pack-relative-relocs) needs glibc >= 2.36: an older loader silently ignores it and
 * binaries crash. The answer is remembered for the run (the prototype asked for every recipe). */
static char *ldflags_probe(build *b)
{
	const char *base = bget(b, "LDFLAGS_BASE");
	if (!strstr(base, "pack-relative-relocs"))
		return xstrdup(base);
	static char *key;
	static bool ok;
	bool cross = bon(b, "CROSS");
	char *k = xasprintf("%d\n%s\n%s\n%s", cross, bget(b, "CC"), bget(b, "READELF"), bget(b, "SYSROOT"));
	if (!key || strcmp(key, k)) {
		free(key);
		key = k;
		k = NULL;
		if (cross) {
			char *lib = xasprintf("%s/usr/lib/libc.so.6", bget(b, "SYSROOT"));
			char *argv[] = {(char *)bget(b, "READELF"), "-V", lib, NULL};
			run_opts o = {.quiet = true, .stdin_null = true};
			char *out = run_output(argv, &o);
			ok = out && strstr(out, "GLIBC_ABI_DT_RELR");
			free(out);
			free(lib);
		} else {
			char *d = probe_dir(), *src = xasprintf("%s/t.c", d), *exe = xasprintf("%s/t", d);
			static const char prog[] = "static int a = 1, b = 2;\nstatic int *p[] = { &a, &b };\n"
						   "int main(void) { return *p[0] + *p[1] == 3 ? 0 : 1; }\n";
			file_write(src, prog, sizeof prog - 1, 0644);
			char *cc[] = {(char *)bget(b, "CC"), "-fPIE", "-pie", "-Wl,-z,pack-relative-relocs", src, "-o", exe, NULL};
			char *t[] = {exe, NULL};
			ok = quiet_ok(cc) && quiet_ok(t);
			rm_rf(d);
			free(src);
			free(exe);
			free(d);
		}
	}
	free(k);
	char *out = xstrdup(base);
	const char *flag = " -Wl,-z,pack-relative-relocs";
	char *at = ok ? NULL : strstr(out, flag);
	if (at)
		memmove(at, at + strlen(flag), strlen(at + strlen(flag)) + 1);
	return out;
}

/* LINKER=mold|lld|bfd: " -fuse-ld=..." when the compiler can use it. GCC < 12 does not know mold:
 * its libexec/mold/ld wrapper (-B) does the same. */
static char *linker_flags(build *b)
{
	const char *linker = bget(b, "LINKER");
	if (!*linker || rhas_option(b->r, "!linker"))
		return xstrdup("");
	static char *key, *flag;
	bool cross = bon(b, "CROSS");
	char *k = xasprintf("%s\n%s\n%d\n%s", linker, bget(b, "CC"), cross, bget(b, "SYSROOT"));
	if (!key || strcmp(key, k)) {
		free(key);
		free(flag);
		key = k;
		k = NULL;
		flag = xstrdup("");
		char *d = probe_dir(), *src = xasprintf("%s/t.c", d), *exe = xasprintf("%s/t", d);
		static const char prog[] = "int main(void) { return 0; }\n";
		file_write(src, prog, sizeof prog - 1, 0644);
		char *sys = xasprintf("--sysroot=%s", bget(b, "SYSROOT"));
		for (int i = 0; i < 2; i++) {
			char *f;
			if (i == 0) {
				f = xasprintf("-fuse-ld=%s", linker);
			} else {
				char *mold = !strcmp(linker, "mold") ? find_program("mold") : NULL;
				char *bin = mold ? path_real(mold) : NULL;
				free(mold);
				if (!bin)
					break;
				char *dir = path_dir(bin), *lx = xasprintf("%s/../libexec/mold", dir);
				char *m = path_abs(lx), *ld = xasprintf("%s/ld", m);
				bool usable = is_exec(ld);
				f = xasprintf("-B%s", m);
				free(dir);
				free(lx);
				free(m);
				free(ld);
				free(bin);
				if (!usable) {
					free(f);
					break;
				}
			}
			svec argv = SVEC_INIT;
			sv_push(&argv, bget(b, "CC"));
			if (cross)
				sv_push(&argv, sys);
			sv_push(&argv, f);
			sv_push(&argv, src);
			sv_push(&argv, "-o");
			sv_push(&argv, exe);
			bool works = quiet_ok(sv_argv(&argv));
			sv_free(&argv);
			if (works) {
				free(flag);
				flag = xasprintf(" %s", f);
				free(f);
				break;
			}
			free(f);
		}
		rm_rf(d);
		free(sys);
		free(src);
		free(exe);
		free(d);
	}
	free(k);
	if (!*flag)
		warn("LINKER=%s does not work with %s: using its default linker", linker, bget(b, "CC"));
	return xstrdup(flag);
}

/* the major version of the compiler (0 when it does not say) */
static int cc_major(const char *cc)
{
	static char *key;
	static int major;
	if (key && !strcmp(key, cc))
		return major;
	free(key);
	key = xstrdup(cc);
	char *argv[] = {(char *)cc, "-dumpversion", NULL};
	run_opts o = {.quiet = true, .stdin_null = true};
	char *v = run_output(argv, &o);
	major = v ? atoi(v) : 0;
	free(v);
	return major;
}

/* per recipe CFLAGS, CXXFLAGS and LDFLAGS (after toolchain_env) */
static void flags_for_recipe(build *b)
{
	recipe *r = b->r;
	char *c = xasprintf("%s %s", bget(b, "OPT_FLAGS"), bget(b, "CPU_CFLAGS"));
	bset(b, "CFLAGS", c);
	free(c);
	/* options=(o3): hot loops (compressors, codecs) that upstream tunes for -O3 */
	if (rhas_option(r, "o3"))
		breplace(b, "CFLAGS", "-O2", "-O3");
	const char *carch = bget(b, "CARCH");
	if (!rhas_option(r, "!hardening")) {
		char *h = xasprintf(" %s", bget(b, "HARDENING_FLAGS"));
		bappend(b, "CFLAGS", h);
		free(h);
		if (!strcmp(carch, "x86_64"))
			bappend(b, "CFLAGS", " -fcf-protection");
	}
	if (!strcmp(carch, "aarch64"))
		bappend(b, "CFLAGS", " -mbranch-protection=standard");
	/* GCC < 12 cannot do _FORTIFY_SOURCE=3 and warns on every file */
	if (strstr(bget(b, "CFLAGS"), "_FORTIFY_SOURCE=3") && cc_major(bget(b, "CC")) < 12)
		breplace(b, "CFLAGS", "_FORTIFY_SOURCE=3", "_FORTIFY_SOURCE=2");
	if (*bget(b, "EXTRA_CFLAGS")) {
		char *e = xasprintf(" %s", bget(b, "EXTRA_CFLAGS"));
		bappend(b, "CFLAGS", e);
		free(e);
	}
	char *probe = ldflags_probe(b), *lf = linker_flags(b);
	const char *extra = bget(b, "EXTRA_LDFLAGS");
	char *ld = xasprintf("%s%s%s%s", probe, lf, *extra ? " " : "", extra);
	bset(b, "LDFLAGS", ld);
	free(ld);
	free(probe);
	free(lf);
	bset(b, "LTO_ON", "0");
	if (bon(b, "LTO") && !rhas_option(r, "!lto")) {
		bset(b, "LTO_ON", "1");
		bappend(b, "CFLAGS", " -flto=auto");
		bappend(b, "LDFLAGS", " -flto=auto");
		if (rhas_option(r, "staticlibs"))
			bappend(b, "CFLAGS", " -ffat-lto-objects");
		const char *pre = bon(b, "CROSS") ? bget(b, "CHOST") : "";
		const char *dash = *pre ? "-" : "";
		char *gar = xasprintf("%s%sgcc-ar", pre, dash);
		if (have(gar)) {
			static const char *const t[][2] = {{"AR", "gcc-ar"}, {"NM", "gcc-nm"}, {"RANLIB", "gcc-ranlib"}};
			for (size_t i = 0; i < ARRAY_LEN(t); i++) {
				char *v = xasprintf("%s%s%s", pre, dash, t[i][1]);
				bset(b, t[i][0], v);
				free(v);
			}
		}
		free(gar);
	}
	const char *R = bget(b, "R");
	if (bon(b, "CROSS")) {
		char *s = xasprintf(" --sysroot=%s", bget(b, "SYSROOT"));
		bappend(b, "CFLAGS", s);
		bappend(b, "LDFLAGS", s);
		free(s);
	} else if (*R) {
		/* a staging root on a foreign host: builds find what is already installed in ROOT */
		char *ci = xasprintf(" -isystem %s/usr/include", R);
		char *li = xasprintf(" -L%s/usr/lib -Wl,-rpath-link,%s/usr/lib", R, R);
		const char *old = bget(b, "PKG_CONFIG_PATH");
		char *pc = xasprintf("%s/usr/lib/pkgconfig:%s/usr/share/pkgconfig%s%s", R, R, *old ? ":" : "", old);
		bappend(b, "CFLAGS", ci);
		bappend(b, "LDFLAGS", li);
		bexport(b, "PKG_CONFIG_PATH", pc);
		free(ci);
		free(li);
		free(pc);
	}
	if (*rget(r, "cflags_append")) {
		char *a = xasprintf(" %s", rget(r, "cflags_append"));
		bappend(b, "CFLAGS", a);
		free(a);
	}
	bset(b, "CXXFLAGS", bget(b, "CFLAGS"));
	static const char *const exp[] = {"CFLAGS", "CXXFLAGS", "LDFLAGS", "AR", "NM", "RANLIB"};
	for (size_t i = 0; i < ARRAY_LEN(exp); i++)
		sh_export(&b->v, exp[i]);
}

/* CCACHE=1: the compiler names resolve to ccache through a masquerade directory first in PATH */
static void ccache_setup(build *b)
{
	char *cc = find_program("ccache");
	if (!cc) {
		warn("CCACHE=1, but ccache is not installed");
		return;
	}
	char *bin = xasprintf("%s/ccache/bin", bget(b, "CACHE_DIR"));
	if (mkdir_p(bin, 0755) < 0)
		die_errno("cannot create %s", bin);
	const char *names[] = {bget(b, "CC"), bget(b, "CXX"), "cc", "c++", "gcc", "g++"};
	for (size_t i = 0; i < ARRAY_LEN(names); i++) {
		if (strchr(names[i], '/'))
			continue;
		char *l = xasprintf("%s/%s", bin, names[i]);
		if (!is_link(l)) {
			unlink(l);
			if (symlink(cc, l) < 0)
				die_errno("ln -s %s %s", cc, l);
		}
		free(l);
	}
	char *dir = *bget(b, "CCACHE_DIR") ? xstrdup(bget(b, "CCACHE_DIR")) : xasprintf("%s/ccache", bget(b, "CACHE_DIR"));
	bexport(b, "CCACHE_DIR", dir);
	bexport(b, "CCACHE_BASEDIR", bget(b, "WORK_DIR"));
	bexport(b, "CCACHE_MAXSIZE", bget(b, "CCACHE_SIZE"));
	char *path = xasprintf("%s:%s", bin, bget(b, "PATH"));
	bexport(b, "PATH", path);
	free(path);
	free(dir);
	free(bin);
	free(cc);
}

/* Identity of the exact build: recipe + toolchain + flags + the ABI (sonames) of the installed
 * dependencies. It names the cached package, so a soname bump of a dependency never reuses a
 * stale build. */
static char *recipe_buildid(build *b)
{
	recipe *r = b->r;
	sha256_ctx c;
	sha256_init(&c);
	char *rid = recipe_rid(r);
	sbuf s = SBUF_INIT;
	sb_addf(&s, "%s\n%s\n%s\n%s\n%s\n", rid, bget(b, "CHOST"), bget(b, "CFLAGS"), bget(b, "CXXFLAGS"),
		bget(b, "LDFLAGS"));
	sb_addf(&s, "%s%s%s%s %s\n", bget(b, "KEEP_DOCS"), bget(b, "KEEP_INFO"), bget(b, "KEEP_MAN"),
		bget(b, "STRIP_BINARIES"), bget(b, "LOCALES"));
	svec deps = SVEC_INIT, act = SVEC_INIT;
	rlist(r, "depends", &deps);
	deps_active(r, &deps, &act);
	for (size_t i = 0; i < act.n; i++) {
		svec words = SVEC_INIT;
		sv_split_ws(&words, act.v[i]);
		for (size_t w = 0; w < words.n; w++) {
			char *f = db_path(words.v[w], "provides-so");
			if (is_file(f)) {
				svec so = SVEC_INIT;
				file_lines(f, &so);
				char *j = sv_join(&so, " ");
				sb_addf(&s, "%s: %s\n", words.v[w], j);
				free(j);
				sv_free(&so);
			}
			free(f);
		}
		sv_free(&words);
	}
	sv_free(&deps);
	sv_free(&act);
	sha256_update(&c, s.s, s.len);
	char hex[65];
	sha256_final_hex(&c, hex);
	sb_free(&s);
	free(rid);
	return xstrndup(hex, 12);
}

static void newest(const char *dir, long long *t)
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
				newest(p, t);
			else if (S_ISREG(st.st_mode) && st.st_mtime > *t)
				*t = st.st_mtime;
		}
		free(p);
	}
	closedir(d);
}

/* SOURCE_DATE_EPOCH of a build: the last commit that touched the recipe, or its newest file */
long long recipe_epoch(const recipe *r)
{
	if (have("git")) {
		char *argv[] = {"git", "-C", r->dir, "log", "-1", "--format=%ct", "--", ".", NULL};
		run_opts o = {.quiet = true, .stdin_null = true};
		char *t = run_output(argv, &o);
		if (t && *t) {
			long long v = strtoll(t, NULL, 10);
			free(t);
			return v;
		}
		free(t);
	}
	long long t = 0;
	newest(r->dir, &t);
	return t;
}

/* the prototype's recipe_env after recipe_load and features_resolve */
void recipe_env(build *b)
{
	toolchain_env(b);
	flags_for_recipe(b);
	char *id = recipe_buildid(b);
	bset(b, "BUILDID", id);
	free(id);
	if (bon(b, "CCACHE"))
		ccache_setup(b);
}
