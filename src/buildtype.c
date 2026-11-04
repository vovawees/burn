/* SPDX-License-Identifier: MIT */
/*
 * The build systems of build_type=, done by burn itself: the arguments ./configure, cmake, meson,
 * make, cargo and python get, exactly as the prototype's buildtypes/NAME.sh gave them. A build calls
 * them directly; recipe functions reach them as bt_configure, bt_build, ... through
 * `burn __bt TYPE PHASE VAR=VALUE... @ARRAY N ELEMENT...` (src/prelude.sh).
 */
#include "burn.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "spawn.h"

static const struct {
	const char *type;
	const char *const phases[5];
} TYPES[] = {
	{"autotools", {"configure", "build", "check", "install", NULL}},
	{"cargo", {"prepare", "build", "check", "install", NULL}},
	{"cmake", {"configure", "build", "check", "install", NULL}},
	{"custom", {"install", NULL}},
	{"make", {"build", "check", "install", NULL}},
	{"meson", {"configure", "build", "check", "install", NULL}},
	{"meta", {"install", NULL}},
	{"python", {"build", "check", "install", NULL}},
};

bool bt_known(const char *type)
{
	for (size_t i = 0; i < ARRAY_LEN(TYPES); i++)
		if (!strcmp(TYPES[i].type, type))
			return true;
	return false;
}

bool bt_has(const char *type, const char *phase)
{
	for (size_t i = 0; i < ARRAY_LEN(TYPES); i++)
		if (!strcmp(TYPES[i].type, type))
			for (const char *const *p = TYPES[i].phases; *p; p++)
				if (!strcmp(*p, phase))
					return true;
	return false;
}

/* ---- the context ---- */

static const char *V(const bt_ctx *b, const char *name)
{
	const char *s = sh_get(b->v, name);
	return s ? s : "";
}

static bool on(const bt_ctx *b, const char *name) /* ((name)) */
{
	return strtoll(V(b, name), NULL, 10) != 0;
}

static void L(const bt_ctx *b, const char *name, svec *out)
{
	sh_get_list(b->v, name, out);
}

static int exec_in(const bt_ctx *b, svec *argv, svec *env)
{
	run_opts o = {.envp = sv_argv(env ? env : b->env)};
	return run(sv_argv(argv), &o);
}

/* cd DIR like the shell: PWD of the programs becomes the logical path; a message and status 1
 * when it fails */
static bool cd(const bt_ctx *b, const char *dir)
{
	if (chdir(dir) != 0) {
		fprintf(stderr, "burn: cd: %s: %s\n", dir, strerror(errno));
		return false;
	}
	char *pwd = path_logical(dir, env_get(b->env, "PWD"));
	env_set(b->env, "PWD", pwd);
	free(pwd);
	return true;
}

static int cmdv(const bt_ctx *b, svec *env, const char *first, ...) __attribute__((sentinel));
static int cmdv(const bt_ctx *b, svec *env, const char *first, ...)
{
	svec argv = SVEC_INIT;
	va_list ap;
	va_start(ap, first);
	for (const char *a = first; a; a = va_arg(ap, const char *))
		sv_push(&argv, a);
	va_end(ap);
	int rc = exec_in(b, &argv, env);
	sv_free(&argv);
	return rc;
}

static void push_list(const bt_ctx *b, svec *argv, const char *name)
{
	L(b, name, argv);
}

/* ---- autotools: ./configure && make && make install ---- */

static const char *bt_dir(const bt_ctx *b)
{
	return on(b, "out_of_tree") ? V(b, "BUILD") : V(b, "SRC");
}

static int autotools(bt_ctx *b, const char *phase)
{
	const char *src = V(b, "SRC");
	svec argv = SVEC_INIT;
	int rc;
	if (!strcmp(phase, "configure")) {
		char *conf = xasprintf("%s/configure", src);
		if (access(conf, X_OK) != 0) {
			char *ac = xasprintf("%s/configure.ac", src), *in = xasprintf("%s/configure.in", src);
			if (!is_file(ac) && !is_file(in))
				die("%s: no configure script", V(b, "name"));
			free(ac);
			free(in);
			run_opts o = {.cwd = src, .envp = sv_argv(b->env)};
			char *ar[] = {"autoreconf", "-fi", NULL};
			if ((rc = run(ar, &o)) != 0) {
				free(conf);
				return rc;
			}
		}
		if (!cd(b, bt_dir(b))) {
			free(conf);
			return 1;
		}
		sv_push_own(&argv, conf);
		static const char *const base[] = {"--prefix=/usr", "--sysconfdir=/etc", "--localstatedir=/var",
						   "--bindir=/usr/bin", "--sbindir=/usr/bin", "--libdir=/usr/lib",
						   "--libexecdir=/usr/lib", "--mandir=/usr/share/man",
						   "--infodir=/usr/share/info"};
		for (size_t i = 0; i < ARRAY_LEN(base); i++)
			sv_push(&argv, base[i]);
		sv_pushf(&argv, "--build=%s", V(b, "CBUILD"));
		sv_pushf(&argv, "--host=%s", V(b, "CHOST"));
		sv_push(&argv, "--disable-dependency-tracking");
		if (!*V(b, "LOCALES"))
			sv_push(&argv, "--disable-nls");
		push_list(b, &argv, "configure_args");
		b->extra(b, "configure_extra", &argv);
	} else {
		if (!cd(b, bt_dir(b)))
			return 1;
		sv_push(&argv, "make");
		if (!strcmp(phase, "install")) {
			sv_pushf(&argv, "DESTDIR=%s", V(b, "DESTDIR"));
			push_list(b, &argv, "make_args");
			push_list(b, &argv, "make_install_args");
			sv_push(&argv, "install");
		} else {
			push_list(b, &argv, "make_args");
			if (!strcmp(phase, "check"))
				sv_push(&argv, "check");
		}
	}
	rc = exec_in(b, &argv, NULL);
	sv_free(&argv);
	return rc;
}

/* ---- make: plain Makefiles, PREFIX=/usr ---- */

static int make(bt_ctx *b, const char *phase)
{
	if (!cd(b, V(b, "SRC")))
		return 1;
	svec argv = SVEC_INIT;
	sv_push(&argv, "make");
	sv_push(&argv, "PREFIX=/usr");
	if (!strcmp(phase, "install"))
		sv_pushf(&argv, "DESTDIR=%s", V(b, "DESTDIR"));
	push_list(b, &argv, "make_args");
	if (!strcmp(phase, "install"))
		push_list(b, &argv, "make_install_args");
	if (strcmp(phase, "build"))
		sv_push(&argv, phase);
	int rc = exec_in(b, &argv, NULL);
	sv_free(&argv);
	return rc;
}

/* ---- cmake (Ninja when available), CMAKE_BUILD_TYPE=None ---- */

static int cmake(bt_ctx *b, const char *phase)
{
	const char *bdir = V(b, "BUILD"), *jobs = V(b, "JOBS");
	if (!strcmp(phase, "build"))
		return cmdv(b, NULL, "cmake", "--build", bdir, "-j", jobs, (char *)NULL);
	if (!strcmp(phase, "check"))
		return cmdv(b, NULL, "ctest", "--test-dir", bdir, "--output-on-failure", "-j", jobs, (char *)NULL);
	if (!strcmp(phase, "install")) {
		svec env = SVEC_INIT;
		sv_extend(&env, b->env);
		env_set(&env, "DESTDIR", V(b, "DESTDIR"));
		int rc = cmdv(b, &env, "cmake", "--install", bdir, (char *)NULL);
		sv_free(&env);
		return rc;
	}
	char *ar = find_program(V(b, "AR")), *ranlib = find_program(V(b, "RANLIB"));
	if (!ranlib) { /* the shell: the failed $(command -v "$RANLIB") fails the assignment */
		free(ar);
		return 1;
	}
	svec argv = SVEC_INIT;
	sv_push(&argv, "cmake");
	sv_push(&argv, "-S");
	const char *sub = V(b, "cmake_subdir");
	sv_pushf(&argv, "%s%s%s", V(b, "SRC"), *sub ? "/" : "", sub);
	sv_push(&argv, "-B");
	sv_push(&argv, bdir);
	sv_push(&argv, "-G");
	sv_push(&argv, have("ninja") ? "Ninja" : "Unix Makefiles");
	static const char *const base[] = {
		"-DCMAKE_INSTALL_PREFIX=/usr",      "-DCMAKE_INSTALL_LIBDIR=lib", "-DCMAKE_INSTALL_LIBEXECDIR=lib",
		"-DCMAKE_INSTALL_BINDIR=bin",       "-DCMAKE_INSTALL_SBINDIR=bin", "-DCMAKE_INSTALL_SYSCONFDIR=/etc",
		"-DCMAKE_INSTALL_LOCALSTATEDIR=/var", "-DCMAKE_BUILD_TYPE=None"};
	for (size_t i = 0; i < ARRAY_LEN(base); i++)
		sv_push(&argv, base[i]);
	sv_pushf(&argv, "-DCMAKE_AR=%s", ar ? ar : "");
	sv_pushf(&argv, "-DCMAKE_RANLIB=%s", ranlib);
	sv_pushf(&argv, "-DCMAKE_C_COMPILER=%s", V(b, "CC"));
	sv_pushf(&argv, "-DCMAKE_CXX_COMPILER=%s", V(b, "CXX"));
	sv_push(&argv, "-Wno-dev");
	free(ar);
	free(ranlib);
	if (on(b, "CROSS")) {
		const char *sys = V(b, "SYSROOT");
		sv_push(&argv, "-DCMAKE_SYSTEM_NAME=Linux");
		sv_pushf(&argv, "-DCMAKE_SYSTEM_PROCESSOR=%s", V(b, "CARCH"));
		sv_pushf(&argv, "-DCMAKE_SYSROOT=%s", sys);
		sv_pushf(&argv, "-DCMAKE_FIND_ROOT_PATH=%s", sys);
		sv_push(&argv, "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER");
		sv_push(&argv, "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY");
		sv_push(&argv, "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY");
		sv_push(&argv, "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY");
	} else if (*V(b, "R")) {
		sv_pushf(&argv, "-DCMAKE_PREFIX_PATH=%s/usr", V(b, "R"));
	}
	push_list(b, &argv, "cmake_args");
	b->extra(b, "cmake_extra", &argv);
	int rc = exec_in(b, &argv, NULL);
	sv_free(&argv);
	return rc;
}

/* ---- meson + ninja, auto features off unless the recipe enables them ---- */

static void meson_list(sbuf *s, const char *words)
{
	svec w = SVEC_INIT;
	sv_split_ws(&w, words);
	sb_addc(s, '[');
	for (size_t i = 0; i < w.n; i++)
		sb_addf(s, "%s'%s'", i ? ", " : "", w.v[i]);
	sb_addc(s, ']');
	sv_free(&w);
}

static char *meson_cross_file(const bt_ctx *b)
{
	char *f = xasprintf("%s/cross.ini", V(b, "WORK"));
	const char *carch = V(b, "CARCH");
	const char *pc = *V(b, "PKG_CONFIG") ? V(b, "PKG_CONFIG") : "pkg-config";
	sbuf s = SBUF_INIT;
	sb_addf(&s, "[binaries]\nc = '%s'\ncpp = '%s'\nar = '%s'\nnm = '%s'\nstrip = '%s'\nobjcopy = '%s'\n",
		V(b, "CC"), V(b, "CXX"), V(b, "AR"), V(b, "NM"), V(b, "STRIP"), V(b, "OBJCOPY"));
	sb_addf(&s, "pkg-config = '%s'\npkgconfig = '%s'\n\n[built-in options]\n", pc, pc);
	static const char *const opts[][2] = {
		{"c_args", "CFLAGS"}, {"cpp_args", "CXXFLAGS"}, {"c_link_args", "LDFLAGS"}, {"cpp_link_args", "LDFLAGS"}};
	for (size_t i = 0; i < ARRAY_LEN(opts); i++) {
		sb_addf(&s, "%s = ", opts[i][0]);
		meson_list(&s, V(b, opts[i][1]));
		sb_addc(&s, '\n');
	}
	/* needs_exe_wrapper: what the cross compiler makes is not run here, even for the same
	 * architecture (it links against the sysroot's glibc, newer than the host's: DT_RELR), so
	 * the checks of meson.build take their cross answers instead of failing to run */
	sb_addf(&s, "\n[properties]\nsys_root = '%s'\npkg_config_libdir = '%s'\nneeds_exe_wrapper = true\n\n",
		V(b, "SYSROOT"), V(b, "PKG_CONFIG_LIBDIR"));
	sb_addf(&s, "[host_machine]\nsystem = 'linux'\ncpu_family = '%s'\ncpu = '%s'\nendian = 'little'\n",
		str_prefix(carch, "arm") ? "arm" : carch, carch);
	if (file_write(f, s.s, s.len, 0644) < 0)
		die_errno("%s", f);
	sb_free(&s);
	return f;
}

static int meson(bt_ctx *b, const char *phase)
{
	const char *bdir = V(b, "BUILD");
	if (!strcmp(phase, "build"))
		return cmdv(b, NULL, "meson", "compile", "-C", bdir, "-j", V(b, "JOBS"), (char *)NULL);
	if (!strcmp(phase, "check"))
		return cmdv(b, NULL, "meson", "test", "-C", bdir, "--print-errorlogs", (char *)NULL);
	if (!strcmp(phase, "install"))
		return cmdv(b, NULL, "meson", "install", "-C", bdir, "--no-rebuild", "--destdir", V(b, "DESTDIR"),
			    (char *)NULL);
	svec argv = SVEC_INIT, extra = SVEC_INIT, env = SVEC_INIT;
	sv_push(&argv, "meson");
	sv_push(&argv, "setup");
	static const char *const base[] = {"--prefix=/usr",  "--sysconfdir=/etc", "--localstatedir=/var",
					   "--libdir=lib",   "--libexecdir=lib",  "--bindir=bin",
					   "--sbindir=bin",  "--buildtype=plain", "--wrap-mode=nodownload"};
	for (size_t i = 0; i < ARRAY_LEN(base); i++)
		sv_push(&argv, base[i]);
	sv_pushf(&argv, "--auto-features=%s", V(b, "MESON_AUTO_FEATURES"));
	sv_push(&argv, "-Db_pie=true");
	sv_push(&argv, "-Db_staticpic=true");
	b->extra(b, "meson_extra", &extra);
	sv_extend(&env, b->env);
	if (on(b, "CROSS")) {
		sv_push(&argv, "--cross-file");
		sv_push_own(&argv, meson_cross_file(b));
		env_unset(&env, "CFLAGS");
		env_unset(&env, "CXXFLAGS");
		env_unset(&env, "LDFLAGS");
		/* the cross file has the sysroot and the pkg-config dirs of the host machine; in the
		 * environment they would be those of the build machine too, whose lookups (native: true,
		 * wayland-scanner, say) take its own dirs and PKG_CONFIG_PATH_FOR_BUILD */
		env_unset(&env, "PKG_CONFIG_SYSROOT_DIR");
		env_unset(&env, "PKG_CONFIG_LIBDIR");
		env_unset(&env, "PKG_CONFIG_PATH");
	}
	push_list(b, &argv, "meson_args");
	sv_extend(&argv, &extra);
	sv_push(&argv, bdir);
	const char *sub = V(b, "meson_subdir");
	sv_pushf(&argv, "%s%s%s", V(b, "SRC"), *sub ? "/" : "", sub);
	int rc = exec_in(b, &argv, &env);
	sv_free(&argv);
	sv_free(&extra);
	sv_free(&env);
	return rc;
}

/* ---- cargo: crates fetched in prepare (Cargo.lock required), the build itself offline ---- */

static const char *rust_target(const bt_ctx *b)
{
	const char *carch = V(b, "CARCH");
	if (!strcmp(carch, "x86_64"))
		return "x86_64-unknown-linux-gnu";
	if (!strcmp(carch, "aarch64"))
		return "aarch64-unknown-linux-gnu";
	die("%s: no Rust target for %s", V(b, "name"), carch);
}

/* -march=znver2 / -mcpu=cortex-a73.cortex-a53 / native -> rustc's target-cpu */
static char *rust_cpu(const bt_ctx *b)
{
	const char *c = V(b, "CPU_CFLAGS");
	if (strstr(c, "=native"))
		return xstrdup("native");
	if (!str_prefix(c, "-mcpu=") && !str_prefix(c, "-march="))
		return xstrdup("generic");
	char *w = xstrndup(c, strcspn(c, " "));
	char *v = xstrdup(strchr(w, '=') + 1);
	free(w);
	v[strcspn(v, ".")] = 0;
	v[strcspn(v, "+")] = 0;
	if (str_prefix(v, "armv")) {
		free(v);
		return xstrdup("generic");
	}
	return v;
}

/* cargo_env: what the cargo steps export */
static void cargo_env(const bt_ctx *b, svec *vars)
{
	const char *t = rust_target(b);
	sv_pushf(vars, "CARGO_HOME=%s/cargo", V(b, "CACHE_DIR"));
	sv_pushf(vars, "CARGO_TARGET_DIR=%s", V(b, "BUILD"));
	sv_pushf(vars, "CARGO_BUILD_TARGET=%s", t);
	char *cpu = rust_cpu(b);
	sv_pushf(vars, "RUSTFLAGS=-C opt-level=3 -C target-cpu=%s -C link-arg=-Wl,-z,relro,-z,now %s", cpu,
		 V(b, "RUSTFLAGS"));
	free(cpu);
	if (on(b, "CROSS")) {
		char *v = xstrdup(t);
		for (char *p = v; *p; p++)
			*p = *p == '-' ? '_' : (char)toupper((unsigned char)*p);
		sv_pushf(vars, "CARGO_TARGET_%s_LINKER=%s", v, V(b, "CC"));
		sv_pushf(vars, "CARGO_TARGET_%s_RUSTFLAGS=-C link-arg=--sysroot=%s", v, V(b, "SYSROOT"));
		free(v);
	}
	if (on(b, "LTO_ON"))
		sv_push(vars, "CARGO_PROFILE_RELEASE_LTO=thin");
}

static int cargo(bt_ctx *b, const char *phase)
{
	svec vars = SVEC_INIT, env = SVEC_INIT, argv = SVEC_INIT;
	cargo_env(b, &vars);
	if (!strcmp(phase, "env")) { /* for the shell: eval "$(burn __bt cargo env ...)" */
		for (size_t i = 0; i < vars.n; i++) {
			char *eq = strchr(vars.v[i], '=');
			sbuf q = SBUF_INIT;
			sh_quote(&q, eq + 1);
			printf("export %.*s=%s\n", (int)(eq - vars.v[i]), vars.v[i], sb_str(&q));
			sb_free(&q);
		}
		sv_free(&vars);
		return 0;
	}
	if (!cd(b, V(b, "SRC"))) {
		sv_free(&vars);
		return 1;
	}
	sv_extend(&env, b->env); /* after the cd: its PWD */
	for (size_t i = 0; i < vars.n; i++) {
		char *k = xstrndup(vars.v[i], strcspn(vars.v[i], "="));
		env_set(&env, k, vars.v[i] + strlen(k) + 1);
		free(k);
	}
	sv_push(&argv, "cargo");
	if (!strcmp(phase, "prepare")) {
		if (!is_file("Cargo.lock"))
			die("%s: Cargo.lock missing (reproducible builds need it)", V(b, "name"));
		sv_push(&argv, "fetch");
		sv_push(&argv, "--locked");
		sv_push(&argv, "--target");
		sv_push(&argv, env_get(&env, "CARGO_BUILD_TARGET"));
	} else if (!strcmp(phase, "install")) {
		static const char *const a[] = {"install", "--frozen", "--offline", "--no-track", "--path", ".", "--root"};
		for (size_t i = 0; i < ARRAY_LEN(a); i++)
			sv_push(&argv, a[i]);
		sv_pushf(&argv, "%s/usr", V(b, "DESTDIR"));
		push_list(b, &argv, "cargo_args");
	} else {
		sv_push(&argv, !strcmp(phase, "build") ? "build" : "test");
		sv_push(&argv, "--release");
		sv_push(&argv, "--frozen");
		push_list(b, &argv, "cargo_args");
	}
	int rc = exec_in(b, &argv, &env);
	sv_free(&argv);
	sv_free(&env);
	sv_free(&vars);
	return rc;
}

/* ---- python (PEP 517): build + installer when present, pip otherwise ---- */

static bool py_has(const bt_ctx *b, const char *code)
{
	char *argv[] = {"python3", "-c", (char *)code, NULL};
	run_opts o = {.envp = sv_argv(b->env), .quiet = true};
	return run(argv, &o) == 0;
}

static int python(bt_ctx *b, const char *phase)
{
	const char *bdir = V(b, "BUILD"), *dest = V(b, "DESTDIR");
	if (!strcmp(phase, "build")) {
		if (on(b, "CROSS"))
			die("%s: cross builds of Python packages are not supported yet", V(b, "name"));
		if (!cd(b, V(b, "SRC")))
			return 1;
		if (py_has(b, "import build, installer"))
			return cmdv(b, NULL, "python3", "-m", "build", "--wheel", "--no-isolation", "--outdir", bdir,
				    (char *)NULL);
		return cmdv(b, NULL, "python3", "-m", "pip", "wheel", "--no-deps", "--no-build-isolation", "--wheel-dir",
			    bdir, ".", (char *)NULL);
	}
	if (!strcmp(phase, "check")) {
		if (!cd(b, V(b, "SRC")))
			return 1;
		return cmdv(b, NULL, "python3", "-m", "pytest", (char *)NULL);
	}
	svec w = SVEC_INIT, argv = SVEC_INIT;
	glob_paths(bdir, "*.whl", &w);
	if (!w.n)
		die("%s: no wheel was built", V(b, "name"));
	sv_push(&argv, "python3");
	sv_push(&argv, "-m");
	if (py_has(b, "import installer")) {
		sv_push(&argv, "installer");
		sv_pushf(&argv, "--destdir=%s", dest);
		sv_push(&argv, "--prefix=/usr");
	} else {
		sv_push(&argv, "pip");
		sv_push(&argv, "install");
		sv_push(&argv, "--no-deps");
		sv_push(&argv, "--no-index");
		sv_pushf(&argv, "--root=%s", dest);
		sv_push(&argv, "--prefix=/usr");
	}
	sv_extend(&argv, &w);
	int rc = exec_in(b, &argv, NULL);
	sv_free(&argv);
	sv_free(&w);
	return rc;
}

int bt_run(const char *type, const char *phase, bt_ctx *b)
{
	fflush(stdout);
	if (!strcmp(type, "autotools"))
		return autotools(b, phase);
	if (!strcmp(type, "make"))
		return make(b, phase);
	if (!strcmp(type, "cmake"))
		return cmake(b, phase);
	if (!strcmp(type, "meson"))
		return meson(b, phase);
	if (!strcmp(type, "cargo"))
		return cargo(b, phase);
	if (!strcmp(type, "python"))
		return python(b, phase);
	if (!strcmp(type, "custom"))
		die("%s: build_type=custom needs a do_install() function", V(b, "name"));
	return 0; /* meta: nothing to do */
}

/* ---- burn __bt: a recipe function's bt_* ---- */

static void extra_from_args(bt_ctx *b, const char *fn, svec *out)
{
	sh_get_list(b->v, "extra", out);
}

int bt_main(int argc, char **argv)
{
	if (argc < 2 || !bt_known(argv[0]))
		die("usage: burn __bt TYPE PHASE [VAR=VALUE]... [@ARRAY N ELEMENT...]... (for recipe functions)");
	const char *type = argv[0], *phase = argv[1];
	if (!bt_has(type, phase) && !(strcmp(type, "cargo") == 0 && strcmp(phase, "env") == 0))
		die("build_type=%s has no %s step", type, phase);
	shenv v;
	sh_init(&v, NULL, true);
	for (int i = 2; i < argc; i++) {
		const char *a = argv[i];
		if (a[0] == '@') {
			if (i + 1 >= argc || !str_uint(argv[i + 1]))
				die("__bt: %s needs a count", a);
			size_t n = strtoull(argv[++i], NULL, 10);
			if (n > (size_t)(argc - i - 1))
				die("__bt: %s: too few elements", a);
			svec list = SVEC_INIT;
			for (size_t j = 0; j < n; j++)
				sv_push(&list, argv[++i]);
			sh_set_list(&v, a + 1, &list);
			sv_free(&list);
		} else {
			const char *eq = strchr(a, '=');
			if (!eq || eq == a)
				die("__bt: not VAR=VALUE: %s", a);
			char *k = xstrndup(a, (size_t)(eq - a));
			sh_set(&v, k, eq + 1);
			free(k);
		}
	}
	svec env = SVEC_INIT;
	env_from_environ(&env);
	bt_ctx b = {.v = &v, .env = &env, .extra = extra_from_args};
	int rc = bt_run(type, phase, &b);
	fflush(stdout);
	sv_free(&env);
	sh_free(&v);
	return rc;
}
