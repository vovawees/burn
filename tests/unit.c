/* SPDX-License-Identifier: MIT */
/* Unit tests: make check */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/sha256.h"
#include "../src/shparse.h"
#include "../src/util.h"
#include "../src/vercmp.h"

static int checks, fails;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		checks++;                                                                          \
		if (!(cond)) {                                                                     \
			fails++;                                                                   \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);   \
		}                                                                                  \
	} while (0)

#define CHECK_STR(got, want)                                                                       \
	do {                                                                                       \
		const char *g_ = (got), *w_ = (want);                                              \
		checks++;                                                                          \
		if (!g_ || strcmp(g_, w_)) {                                                       \
			fails++;                                                                   \
			fprintf(stderr, "%s:%d: got '%s', want '%s'\n", __FILE__, __LINE__,        \
				g_ ? g_ : "(null)", w_);                                           \
		}                                                                                  \
	} while (0)

static void sha_str(const char *s, char out[65])
{
	sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, s, strlen(s));
	sha256_final_hex(&c, out);
}

static void test_sha256(void)
{
	char h[65];
	sha_str("", h);
	CHECK_STR(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	sha_str("abc", h);
	CHECK_STR(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	sha_str("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", h);
	CHECK_STR(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	/* one million 'a', fed in odd pieces to cross every block boundary */
	sha256_ctx c;
	sha256_init(&c);
	char a[997];
	memset(a, 'a', sizeof a);
	size_t left = 1000000;
	for (size_t step = 1; left; step = step % 990 + 7) {
		size_t n = step < left ? step : left;
		sha256_update(&c, a, n);
		left -= n;
	}
	sha256_final_hex(&c, h);
	CHECK_STR(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void test_vercmp(void)
{
	static const struct {
		const char *a, *b;
		int r;
	} t[] = {
		{"1.0", "1.0", 0},          {"1.0", "1.1", -1},          {"1.10", "1.9", 1},
		{"2.0~rc1", "2.0", -1},     {"1.0-1", "1.0-2", -1},      {"1.0-10", "1.0-9", 1},
		{"5.3.20-1", "5.3.3-1", 1}, {"2026.1-1", "2026.1-3", -1}, {"7.3.1-3", "7.10-1", -1},
		{"1.0a", "1.0", 1},         {"1.0", "1.00", -1},         {"0.22.1", "0.22.1.1", -1},
		{"a", "b", -1},             {"1.2.3", "1.2.3a", -1},     {"10", "9", 1},
	};
	for (size_t i = 0; i < ARRAY_LEN(t); i++) {
		int r = vercmp(t[i].a, t[i].b);
		if (r != t[i].r)
			fprintf(stderr, "vercmp(%s, %s) = %d, want %d\n", t[i].a, t[i].b, r, t[i].r);
		CHECK(r == t[i].r);
		CHECK(vercmp(t[i].b, t[i].a) == -t[i].r);
	}
}

static void test_strings(void)
{
	svec v = SVEC_INIT;
	sv_split_ws(&v, "  b a\tc \n a ");
	CHECK(v.n == 4);
	sv_sort_uniq(&v);
	char *j = sv_join(&v, ",");
	CHECK_STR(j, "a,b,c");
	free(j);
	CHECK(v.v[v.n] == NULL);
	CHECK(sv_bsearch(&v, "a") && sv_bsearch(&v, "b") && sv_bsearch(&v, "c"));
	CHECK(!sv_bsearch(&v, "") && !sv_bsearch(&v, "bb") && !sv_bsearch(&v, "d"));
	sv_remove(&v, 1);
	j = sv_join(&v, " ");
	CHECK_STR(j, "a c");
	free(j);
	sv_free(&v);

	char *p = path_join("", "usr/bin");
	CHECK_STR(p, "/usr/bin");
	free(p);
	p = path_join("/r/", "/usr");
	CHECK_STR(p, "/r/usr");
	free(p);
	CHECK(str_prefix("gnu://x", "gnu://"));
	CHECK(str_suffix("a.pkg.tar.zst", ".pkg.tar.zst"));
	CHECK(!str_uint("") && str_uint("042") && !str_uint("4a"));
}

static void test_paths(void)
{
	char tmpl[] = "/tmp/burn-unit.XXXXXX";
	char *d = mkdtemp(tmpl);
	CHECK(d != NULL);
	if (!d)
		return;
	char *sub = xasprintf("%s/a/b/c", d);
	CHECK(mkdir_p(sub, 0755) == 0);
	CHECK(is_dir(sub));
	char *link = xasprintf("%s/l", d);
	CHECK(symlink("a/b", link) == 0);
	char *q = xasprintf("%s/l/c/../x/./y", d);
	char *abs = path_abs(q);
	char *want = xasprintf("%s/a/b/x/y", d);
	char *real = path_real(d);
	char *want_real = xasprintf("%s/a/b/x/y", real ? real : d);
	CHECK(!strcmp(abs, want) || !strcmp(abs, want_real));
	char *f = xasprintf("%s/a/file", d);
	CHECK(file_write(f, "one\ntwo\n", 8, 0644) == 0);
	svec lines = SVEC_INIT;
	CHECK(file_lines(f, &lines) == 0 && lines.n == 2 && !strcmp(lines.v[1], "two"));
	sv_free(&lines);
	CHECK(rm_rf(d) == 0);
	CHECK(!exists(d));
	free(sub);
	free(link);
	free(q);
	free(abs);
	free(want);
	free(real);
	free(want_real);
	free(f);
}

static int eval(shctx *c, const char *src)
{
	return sh_eval(c, src, strlen(src), "t");
}

static int inherit_cb(shctx *c, svec *argv)
{
	if (strcmp(argv->v[0], "inherit"))
		return 1;
	if (argv->n != 2)
		return sh_error(c, "usage: inherit PROFILE");
	sv_push((svec *)c->user, argv->v[1]);
	return 0;
}

static void test_shparse(void)
{
	/* layers: a recipe is evaluated on top of the configuration */
	shenv base, env;
	sh_init(&base, NULL, false);
	sh_set(&base, "CARCH", "x86_64");
	svec l = SVEC_INIT;
	sv_push(&l, "a");
	sv_push(&l, "b");
	sh_set_list(&base, "PROFILE_FEATURES", &l);
	sv_free(&l);
	sh_init(&env, &base, false);
	shctx c;
	sh_ctx_init(&c, &env);
	CHECK(eval(&c, "x=$CARCH-${PROFILE_FEATURES[1]}\nPROFILE_FEATURES+=(c)\nunset CARCH\n") == 0);
	CHECK_STR(sh_get(&env, "x"), "x86_64-b");
	CHECK(sh_get(&env, "CARCH") == NULL && !sh_isset(&env, "CARCH"));
	CHECK_STR(sh_get(&base, "CARCH"), "x86_64");
	svec got = SVEC_INIT;
	CHECK(sh_get_list(&env, "PROFILE_FEATURES", &got));
	char *j = sv_join(&got, " ");
	CHECK_STR(j, "a b c");
	free(j);
	sv_clear(&got);
	sh_get_list(&base, "PROFILE_FEATURES", &got);
	CHECK(got.n == 2);
	sv_clear(&got);
	CHECK(eval(&c, "CARCH=aarch64; export LATER; LATER=1; unset -v x") == 0);
	CHECK_STR(sh_get(&env, "CARCH"), "aarch64");
	CHECK(sh_find(&env, "LATER") && sh_find(&env, "LATER")->exported);
	CHECK(!sh_isset(&env, "x"));
	sh_local_names(&env, &got);
	j = sv_join(&got, " ");
	CHECK_STR(j, "CARCH LATER PROFILE_FEATURES");
	free(j);
	sv_clear(&got);

	/* functions and the command hook */
	svec inherited = SVEC_INIT;
	c.command = inherit_cb;
	c.user = &inherited;
	CHECK(eval(&c, "inherit generic\nf() { :; }\ng() {\n\tf\n}\nf() { true; }\nunset -f g\n") == 0);
	CHECK(inherited.n == 1 && !strcmp(inherited.v[0], "generic"));
	CHECK(c.funcs.n == 1 && !strcmp(c.funcs.v[0], "f"));
	CHECK(eval(&c, "inherit") < 0);
	CHECK_STR(c.err, "t:1: usage: inherit PROFILE");
	sv_free(&inherited);
	sh_ctx_free(&c);

	/* errors carry file:line, the first one wins */
	sh_ctx_init(&c, &env);
	CHECK(eval(&c, "a=1\n\nb=(x\n y $(date))\nc=`x`") < 0);
	CHECK_STR(c.err, "t:4: command substitution $(...) is not allowed at the top level: put it into a function");
	CHECK(eval(&c, "c=`x`") < 0);
	CHECK(strstr(c.err, "t:4:") != NULL);
	sh_ctx_free(&c);
	sh_ctx_init(&c, &env);
	CHECK(sh_eval_file(&c, "/nonexistent/recipe") < 0);
	CHECK_STR(c.err, "/nonexistent/recipe: cannot read");
	sh_ctx_free(&c);

	/* single words, $'\u...' */
	sh_ctx_init(&c, &env);
	char *w = NULL;
	CHECK(eval(&c, "version=1.2.3 u=$'\\u00e9\\U0001F600\\x41'") == 0);
	CHECK(sh_expand_word(&c, "${version%.*}-\"$CARCH\"", &w) == 0);
	CHECK_STR(w, "1.2-aarch64");
	free(w);
	CHECK_STR(sh_get(&env, "u"), "\xc3\xa9\xf0\x9f\x98\x80" "A");
	CHECK(sh_expand_word(&c, "$nope", &w) < 0);
	free(w);
	sh_ctx_free(&c);

	sbuf b = SBUF_INIT;
	sh_quote(&b, "it's");
	sb_addc(&b, ' ');
	sh_quote(&b, "plain-1.0/x");
	sb_addc(&b, ' ');
	sh_quote(&b, "");
	CHECK_STR(sb_str(&b), "'it'\\''s' plain-1.0/x ''");
	sb_free(&b);
	sv_free(&got);
	sh_free(&env);
	sh_free(&base);
}

int main(void)
{
	test_sha256();
	test_vercmp();
	test_strings();
	test_paths();
	test_shparse();
	printf("unit: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
