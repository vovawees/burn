/* SPDX-License-Identifier: MIT */
/*
 * Signed packages and the binary cache.
 *
 * A signature, FILE.sig next to a package, is text: the file name, its sha256 and the name of the
 * key, then the Ed25519 signature of those three lines (hex). openssl makes and checks the
 * signature: every CleanLinux system has it (the TLS of curl), and burn computes the sha256 itself,
 * so only a short statement is signed, never the whole package. Trusted keys are
 * $ROOT/etc/burn/keys/NAME.pub; SIGNING_KEY (burn.conf) is the private key NAME.key that signs
 * what this machine builds.
 *
 * The binary cache: BINARY_CACHES lists the package caches of your other machines (https://,
 * http://, file:// or a directory). A build missing here is looked up there by its file name,
 * which holds the BUILDID (recipe, features, flags and toolchain): the same name is the same
 * build. It is taken only with a valid signature of a trusted key, else burn builds it.
 */
#include "burn.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sha256.h"
#include "spawn.h"

#define SIG_MAGIC "burn-signature 1"

char *keys_dir(void) { return xasprintf("%s/etc/burn/keys", B.R ? B.R : ""); }

static bool key_name_ok(const char *s)
{
	if (!*s || *s == '.' || strlen(s) > 64)
		return false;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
		      *s == '-' || *s == '_' || *s == '.'))
			return false;
	return true;
}

static char *statement(const char *file, const char *sum, const char *key)
{
	return xasprintf(SIG_MAGIC "\nfile %s\nsha256 %s\nkey %s\n", file, sum, key);
}

/* a private directory for the statement and the raw signature that openssl reads */
static char *tmp_dir(void)
{
	const char *t = getenv("TMPDIR");
	char *d = xasprintf("%s/burn-sign.XXXXXX", t && *t ? t : "/tmp");
	if (!mkdtemp(d))
		die_errno("mkdtemp %s", d);
	return d;
}

static void need_openssl(void)
{
	if (!have("openssl"))
		die("signatures need the openssl tool (package openssl)");
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* the key name of a private key file: NAME.key */
static char *signing_key_name(const char *path)
{
	const char *b = path_base(path);
	size_t n = strlen(b);
	char *name = str_suffix(b, ".key") ? xstrndup(b, n - 4) : xstrdup(b);
	if (!key_name_ok(name))
		die("SIGNING_KEY %s: the file name must be NAME.key (letters, digits, - _ .)", path);
	return name;
}

/* FILE.sig for FILE with the private key KEY */
void sign_file(const char *file, const char *key)
{
	need_openssl();
	if (!is_file(key))
		die("signing key %s: no such file", key);
	char sum[65];
	if (sha256_file(file, sum) < 0)
		die_errno("cannot read %s", file);
	char *name = signing_key_name(key), *st = statement(path_base(file), sum, name);
	char *dir = tmp_dir(), *in = path_join(dir, "statement"), *sig = path_join(dir, "sig");
	if (file_write(in, st, strlen(st), 0600) < 0)
		die_errno("cannot write %s", in);
	char *argv[] = {"openssl", "pkeyutl", "-sign", "-inkey", (char *)key, "-rawin", "-in", in, "-out", sig, NULL};
	run_opts o = {.stdin_null = true};
	if (run(argv, &o) != 0)
		die("openssl could not sign %s with %s (an Ed25519 key: burn key new)", file, key);
	size_t len = 0;
	char *raw = file_read(sig, &len);
	if (!raw || len != 64)
		die("openssl gave no Ed25519 signature for %s (is %s an Ed25519 key?)", file, key);
	sbuf out = SBUF_INIT;
	sb_adds(&out, st);
	sb_adds(&out, "signature ");
	for (size_t i = 0; i < len; i++)
		sb_addf(&out, "%02x", (unsigned char)raw[i]);
	sb_addc(&out, '\n');
	char *dst = xasprintf("%s.sig", file);
	if (file_write(dst, sb_str(&out), out.len, 0644) < 0)
		die_errno("cannot write %s", dst);
	free(dst);
	sb_free(&out);
	free(raw);
	rm_rf(dir);
	free(sig);
	free(in);
	free(dir);
	free(st);
	free(name);
}

/* sign what this machine builds: SIGNING_KEY of burn.conf */
void sign_built(const char *file)
{
	const char *key = cfg("SIGNING_KEY");
	if (*key)
		sign_file(file, key);
}

/* NULL when SIGFILE is a valid signature of FILE, by a trusted key, for the name NAME (the file
 * may sit under a temporary name); else why not */
char *verify_file(const char *file, const char *name, const char *sigfile)
{
	size_t len = 0;
	char *text = file_read(sigfile, &len);
	if (!text)
		return xasprintf("no signature (%s)", path_base(sigfile));
	char *why = NULL, *dir = NULL, *in = NULL, *sig = NULL, *pub = NULL, *st = NULL;
	char f_file[256] = "", f_sum[65] = "", f_key[65] = "", f_sig[129] = "";
	char *l[5] = {0};
	char *save = NULL, *p = strtok_r(text, "\n", &save);
	for (int i = 0; i < 5 && p; i++, p = strtok_r(NULL, "\n", &save))
		l[i] = p;
	if (!l[4] || strcmp(l[0], SIG_MAGIC) || sscanf(l[1], "file %255s", f_file) != 1 ||
	    sscanf(l[2], "sha256 %64s", f_sum) != 1 || sscanf(l[3], "key %64s", f_key) != 1 ||
	    sscanf(l[4], "signature %128s", f_sig) != 1 || strlen(f_sig) != 128) {
		why = xstrdup("not a burn signature");
		goto out;
	}
	if (strcmp(f_file, name)) {
		why = xasprintf("the signature is for %s", f_file);
		goto out;
	}
	if (!key_name_ok(f_key)) {
		why = xasprintf("bad key name '%s'", f_key);
		goto out;
	}
	char *kd = keys_dir();
	pub = xasprintf("%s/%s.pub", kd, f_key);
	free(kd);
	if (!is_file(pub)) {
		why = xasprintf("signed by %s, which is not a trusted key (no %s)", f_key, pub);
		goto out;
	}
	char sum[65];
	if (sha256_file(file, sum) < 0) {
		why = xasprintf("cannot read %s: %s", file, strerror(errno));
		goto out;
	}
	if (strcmp(sum, f_sum)) {
		why = xstrdup("sha256 mismatch: the file is not the one that was signed");
		goto out;
	}
	unsigned char raw[64];
	for (int i = 0; i < 64; i++) {
		int hi = hexval(f_sig[2 * i]), lo = hexval(f_sig[2 * i + 1]);
		if (hi < 0 || lo < 0) {
			why = xstrdup("the signature is not hex");
			goto out;
		}
		raw[i] = (unsigned char)(hi << 4 | lo);
	}
	need_openssl();
	st = statement(f_file, f_sum, f_key);
	dir = tmp_dir();
	in = path_join(dir, "statement");
	sig = path_join(dir, "sig");
	if (file_write(in, st, strlen(st), 0600) < 0 || file_write(sig, raw, sizeof raw, 0600) < 0)
		die_errno("cannot write into %s", dir);
	char *argv[] = {"openssl", "pkeyutl", "-verify", "-pubin", "-inkey", pub, "-rawin", "-in", in, "-sigfile", sig, NULL};
	run_opts o = {.stdin_null = true, .quiet = true};
	if (run(argv, &o) != 0)
		why = xasprintf("the signature of %s does not verify", f_key);
out:
	if (dir)
		rm_rf(dir);
	free(dir);
	free(in);
	free(sig);
	free(pub);
	free(st);
	free(text);
	return why;
}

/* ---- the binary cache ---- */

static bool trusted_keys(void)
{
	char *kd = keys_dir();
	svec k = SVEC_INIT;
	glob_paths(kd, "*.pub", &k);
	bool any = k.n > 0;
	sv_free(&k);
	free(kd);
	return any;
}

/* URL of FILE in the cache C: a directory is a file:// URL */
static char *cache_url(const char *c, const char *file)
{
	size_t n = strlen(c);
	while (n > 1 && c[n - 1] == '/')
		n--;
	return c[0] == '/' ? xasprintf("file://%.*s/%s", (int)n, c, file) : xasprintf("%.*s/%s", (int)n, c, file);
}

/* every file of OUTS (paths in the package cache) from one of BINARY_CACHES, signed by a trusted
 * key; true when they are all in the package cache now */
bool bincache_fetch(const svec *outs)
{
	svec caches = SVEC_INIT;
	cfg_words("BINARY_CACHES", &caches);
	if (!caches.n || !outs->n)
		return false;
	static bool warned;
	if (!trusted_keys()) {
		if (!warned)
			warn("BINARY_CACHES: no trusted key in %s/etc/burn/keys, the caches are not used", B.R ? B.R : "");
		warned = true;
		sv_free(&caches);
		return false;
	}
	char *pdir = path_dir(outs->v[0]);
	if (mkdir_p(pdir, 0755) < 0)
		die_errno("cannot create %s", pdir);
	free(pdir);
	bool ok = false;
	for (size_t c = 0; c < caches.n && !ok; c++) {
		ok = true;
		svec got = SVEC_INIT;
		for (size_t i = 0; i < outs->n && ok; i++) {
			const char *base = path_base(outs->v[i]);
			char *tmp = xasprintf("%s.bincache", outs->v[i]), *tsig = xasprintf("%s.bincache.sig", outs->v[i]);
			char *u = cache_url(caches.v[c], base), *us = xasprintf("%s.sig", u);
			sv_push(&got, tmp);
			sv_push(&got, tsig);
			if (download_quiet(us, tsig) != 0 || download_quiet(u, tmp) != 0) {
				ok = false;
			} else {
				char *why = verify_file(tmp, base, tsig);
				if (why) {
					warn("%s from %s: %s", base, caches.v[c], why);
					ok = false;
					free(why);
				}
			}
			free(u);
			free(us);
			free(tmp);
			free(tsig);
		}
		for (size_t i = 0; ok && i < outs->n; i++) {
			char *sig = xasprintf("%s.sig", outs->v[i]);
			if (rename(got.v[2 * i + 1], sig) < 0 || rename(got.v[2 * i], outs->v[i]) < 0)
				die_errno("cannot move %s into the package cache", got.v[2 * i]);
			free(sig);
		}
		if (ok)
			msg2("from the binary cache %s (signed)", caches.v[c]);
		for (size_t i = 0; i < got.n; i++)
			unlink(got.v[i]);
		sv_free(&got);
	}
	sv_free(&caches);
	return ok;
}

/* ---- commands ---- */

/* sign FILE...: with SIGNING_KEY, or --key FILE */
int cmd_sign(int argc, char **argv)
{
	const char *key = cfg("SIGNING_KEY");
	int i = 0;
	if (argc >= 2 && !strcmp(argv[0], "--key")) {
		key = argv[1];
		i = 2;
	}
	if (i >= argc)
		die("usage: burn sign [--key NAME.key] <file>...");
	if (!*key)
		die("no signing key: SIGNING_KEY in burn.conf, or --key NAME.key (burn key new)");
	for (; i < argc; i++) {
		sign_file(argv[i], key);
		msg("signed %s", argv[i]);
	}
	return 0;
}

/* verify FILE...: FILE.sig, by a trusted key */
int cmd_verify(int argc, char **argv)
{
	if (!argc)
		die("usage: burn verify <file>...");
	int bad = 0;
	for (int i = 0; i < argc; i++) {
		char *sig = xasprintf("%s.sig", argv[i]);
		char *why = verify_file(argv[i], path_base(argv[i]), sig);
		if (why) {
			warn("%s: %s", argv[i], why);
			bad++;
		} else {
			size_t len = 0;
			char *t = file_read(sig, &len), *k = t ? strstr(t, "\nkey ") : NULL;
			char key[65] = "?";
			if (k)
				sscanf(k + 5, "%64s", key);
			msg("%s: good signature by %s", argv[i], key);
			free(t);
		}
		free(why);
		free(sig);
	}
	return bad ? 1 : 0;
}

static void key_print(const char *pub)
{
	char sum[65];
	if (sha256_file(pub, sum) < 0)
		return;
	const char *b = path_base(pub);
	printf("%-20.*s %.16s\n", (int)(strlen(b) - 4), b, sum);
}

/* key new PATH/NAME | list | trust FILE.pub */
int cmd_key(int argc, char **argv)
{
	const char *sub = argc ? argv[0] : "list";
	if (!strcmp(sub, "new") && argc == 2) {
		need_openssl();
		char *base = str_suffix(argv[1], ".key") ? xstrndup(argv[1], strlen(argv[1]) - 4) : xstrdup(argv[1]);
		char *key = xasprintf("%s.key", base), *pub = xasprintf("%s.pub", base);
		if (!key_name_ok(path_base(base)))
			die("key name '%s': letters, digits, - _ .", path_base(base));
		if (exists(key))
			die("%s exists already", key);
		mode_t old = umask(077);
		char *gen[] = {"openssl", "genpkey", "-algorithm", "ed25519", "-out", key, NULL};
		int rc = run(gen, &(run_opts){.stdin_null = true});
		umask(old);
		char *exp[] = {"openssl", "pkey", "-in", key, "-pubout", "-out", pub, NULL};
		if (rc != 0 || run(exp, &(run_opts){.stdin_null = true}) != 0)
			die("openssl could not make an Ed25519 key");
		chmod(pub, 0644);
		msg("private key %s (SIGNING_KEY=%s in burn.conf signs what this machine builds)", key, key);
		msg("public key %s (burn key trust %s on the machines that take its builds)", pub, pub);
		free(key);
		free(pub);
		free(base);
		return 0;
	}
	if (!strcmp(sub, "trust") && argc == 2) {
		const char *b = path_base(argv[1]);
		if (!str_suffix(b, ".pub") || !key_name_ok(b))
			die("usage: burn key trust NAME.pub");
		need_openssl();
		char *chk[] = {"openssl", "pkey", "-pubin", "-in", argv[1], "-noout", NULL};
		if (run(chk, &(run_opts){.stdin_null = true, .quiet = true}) != 0)
			die("%s is not a public key", argv[1]);
		size_t len = 0;
		char *t = file_read(argv[1], &len);
		if (!t)
			die_errno("cannot read %s", argv[1]);
		char *kd = keys_dir(), *dst = path_join(kd, b);
		if (mkdir_p(kd, 0755) < 0 || file_write(dst, t, len, 0644) < 0)
			die_errno("cannot write %s", dst);
		msg("trusted: %s", dst);
		free(dst);
		free(kd);
		free(t);
		return 0;
	}
	if (!strcmp(sub, "list") && argc <= 1) {
		char *kd = keys_dir();
		svec k = SVEC_INIT;
		glob_paths(kd, "*.pub", &k);
		sv_sort(&k);
		for (size_t i = 0; i < k.n; i++)
			key_print(k.v[i]);
		if (!k.n)
			msg("no trusted keys in %s", kd);
		sv_free(&k);
		free(kd);
		return 0;
	}
	die("usage: burn key new PATH/NAME | trust NAME.pub | list");
}
