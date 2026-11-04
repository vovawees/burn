/* SPDX-License-Identifier: MIT */
/* Sources: the source cache, downloads (curl or wget), sha256 checks inside the process and the
 * parallel prefetch that verifies everything before anything is built. */
#include "burn.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sha256.h"
#include "spawn.h"

char *source_path(const recipe *r, const char *spec)
{
	source_spec s;
	source_info(spec, &s);
	char *p = s.local ? xasprintf("%s/%s", r->dir, s.url) : xasprintf("%s/sources/%s", cfg("CACHE_DIR"), s.name);
	source_spec_free(&s);
	return p;
}

/* SUM is the sha256 of FILE, or SKIP */
static bool sum_ok(const char *file, const char *sum)
{
	if (!strcmp(sum, "SKIP"))
		return true;
	char hex[65];
	return sha256_file(file, hex) == 0 && !strcmp(hex, sum);
}

/* URL into FILE, through FILE.part */
static int download(const char *url, const char *file)
{
	char *tmp = xasprintf("%s.part", file);
	int rc;
	if (have("curl")) {
		char *argv[] = {"curl", "-fL", "--retry", "3", "--retry-delay", "2", "--connect-timeout", "20",
				"-sS", "-o", tmp, (char *)url, NULL};
		rc = run(argv, NULL);
	} else if (have("wget")) {
		char *argv[] = {"wget", "-q", "-O", tmp, (char *)url, NULL};
		rc = run(argv, NULL);
	} else {
		die("curl or wget is required to download sources");
	}
	if (rc == 0 && rename(tmp, file) < 0)
		rc = 1;
	free(tmp);
	return rc;
}

/* cp FROM TO: 0, or -1 (TO is removed) */
static int copy_quiet(const char *from, const char *to)
{
	int in = open(from, O_RDONLY | O_CLOEXEC), out = -1, rc = -1;
	if (in < 0)
		return -1;
	out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	char buf[65536];
	while (out >= 0) {
		ssize_t n = read(in, buf, sizeof buf);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			rc = n == 0 ? 0 : -1;
			break;
		}
		if (write_all(out, buf, (size_t)n) < 0)
			break;
	}
	if (out >= 0 && close(out) < 0)
		rc = -1;
	close(in);
	if (rc < 0)
		unlink(to);
	return rc;
}

/* URL (also file://) into FILE through FILE.part, without noise: a file the server does not
 * have is no error here (the binary cache asks for builds it may not have). 0 or not. */
int download_quiet(const char *url, const char *file)
{
	char *tmp = xasprintf("%s.part", file);
	int rc;
	if (str_prefix(url, "file://")) {
		rc = copy_quiet(url + 7, tmp);
	} else if (have("curl")) {
		char *argv[] = {"curl", "-fsL", "--connect-timeout", "10", "-o", tmp, (char *)url, NULL};
		run_opts o = {.stdin_null = true, .quiet = true};
		rc = run(argv, &o);
	} else if (have("wget")) {
		char *argv[] = {"wget", "-q", "-O", tmp, (char *)url, NULL};
		run_opts o = {.stdin_null = true, .quiet = true};
		rc = run(argv, &o);
	} else {
		die("curl or wget is required to download from %s", url);
	}
	if (rc == 0 && rename(tmp, file) < 0)
		rc = 1;
	if (rc != 0)
		unlink(tmp);
	free(tmp);
	return rc;
}

/* FILE (in the source cache) is downloaded and matches SUM; dies otherwise */
static void fetch_one(const char *spec, const char *file, const char *sum, const char *pkg)
{
	source_spec s;
	source_info(spec, &s);
	if (!is_file(file)) {
		msg2("download %s", s.name);
		if (download(s.url, file) != 0) {
			const char *gnu = strstr(spec, "gnu://");
			if (!gnu)
				die("%s: download failed: %s", pkg, s.url);
			char *alt = xasprintf("https://ftp.gnu.org/gnu/%s", gnu + 6);
			if (download(alt, file) != 0)
				die("%s: download failed: %s", pkg, s.url);
			free(alt);
		}
	}
	if (!*sum)
		die("%s: missing sha256 for %s", pkg, s.name);
	if (!sum_ok(file, sum)) {
		char *bad = xasprintf("%s.bad", file);
		rename(file, bad);
		free(bad);
		die("%s: sha256 mismatch for %s (moved aside as .bad)", pkg, s.name);
	}
	source_spec_free(&s);
}

static void sources_dir(void)
{
	char *d = xasprintf("%s/sources", cfg("CACHE_DIR"));
	if (mkdir_p(d, 0755) < 0)
		die_errno("cannot create %s", d);
	free(d);
}

void fetch_sources(recipe *r, bool verify)
{
	const char *name = rget(r, "name");
	svec src = SVEC_INIT, sums = SVEC_INIT;
	rlist(r, "source", &src);
	rlist(r, "sha256", &sums);
	sources_dir();
	for (size_t i = 0; i < src.n; i++) {
		char *file = source_path(r, src.v[i]);
		source_spec s;
		source_info(src.v[i], &s);
		const char *sum = !verify ? "SKIP" : i < sums.n ? sums.v[i] : "";
		if (s.local) {
			if (!is_file(file))
				die("%s: local source '%s' is missing", name, s.url);
			if (!*sum)
				die("%s: missing sha256 for %s", name, s.name);
			if (!sum_ok(file, sum))
				die("%s: sha256 mismatch for %s", name, s.name);
		} else {
			fetch_one(src.v[i], file, sum, name);
		}
		source_spec_free(&s);
		free(file);
	}
	sv_free(&src);
	sv_free(&sums);
}

/* Download (FETCH_JOBS at a time) and verify every source of NAMES before anything is built,
 * so a long update never stops halfway on a dead link or a bad checksum. */
void prefetch(const svec *names)
{
	svec files = SVEC_INIT, specs = SVEC_INIT, sums = SVEC_INIT, pkgs = SVEC_INIT;
	for (size_t i = 0; i < names->n; i++) {
		recipe *r = recipe_load(names->v[i]);
		features_resolve(r);
		svec src = SVEC_INIT, sha = SVEC_INIT;
		rlist(r, "source", &src);
		rlist(r, "sha256", &sha);
		for (size_t j = 0; j < src.n; j++) {
			source_spec s;
			source_info(src.v[j], &s);
			if (!s.local) {
				sv_pushf(&files, "%s/sources/%s", cfg("CACHE_DIR"), s.name);
				sv_push(&specs, src.v[j]);
				sv_push(&sums, j < sha.n ? sha.v[j] : "");
				sv_push(&pkgs, rget(r, "name"));
			}
			source_spec_free(&s);
		}
		sv_free(&src);
		sv_free(&sha);
		recipe_free(r);
	}
	sources_dir();
	long jobs = atol(cfg("FETCH_JOBS"));
	if (jobs < 1)
		jobs = 1;
	strmap seen = STRMAP_INIT;
	long running = 0, failed = 0;
	size_t unique = 0;
	for (size_t i = 0; i <= files.n; i++) {
		bool more = i < files.n;
		if (more && sm_has(&seen, files.v[i]))
			continue;
		/* wait for a free slot, or for everything at the end */
		while (running > 0 && (!more || running >= jobs)) {
			int st;
			if (wait(&st) < 0) {
				if (errno == EINTR)
					continue;
				die_errno("wait");
			}
			if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
				failed++;
			running--;
		}
		if (!more)
			break;
		sm_put(&seen, files.v[i], 0);
		unique++;
		fflush(stdout);
		pid_t pid = fork();
		if (pid < 0)
			die_errno("fork");
		if (pid == 0) {
			die_hook = NULL;
			fetch_one(specs.v[i], files.v[i], sums.v[i], pkgs.v[i]);
			_exit(0);
		}
		running++;
	}
	if (failed)
		die("%ld source(s) could not be fetched or verified: nothing was built", failed);
	msg2("sources: %zu downloaded and verified", unique);
	sm_free(&seen);
	sv_free(&files);
	sv_free(&specs);
	sv_free(&sums);
	sv_free(&pkgs);
}

int cmd_fetch(int argc, char **argv)
{
	if (!argc)
		die("usage: burn fetch <pkg>...");
	svec names = SVEC_INIT;
	for (int i = 0; i < argc; i++)
		sv_push(&names, argv[i]);
	char *all = sv_join(&names, " ");
	msg("Fetching the sources of: %s", all);
	prefetch(&names);
	free(all);
	sv_free(&names);
	return 0;
}

/* checksum [--write] PKG: the sha256=() block of the downloaded sources, or written into the recipe */
int cmd_checksum(int argc, char **argv)
{
	if (argc != 1)
		die("usage: burn checksum [--write] <pkg>");
	recipe *r = recipe_load(argv[0]);
	features_resolve(r);
	fetch_sources(r, false);
	svec src = SVEC_INIT;
	rlist(r, "source", &src);
	sbuf block = SBUF_INIT;
	sb_adds(&block, "sha256=(");
	char hex[65];
	for (size_t i = 0; i < src.n; i++) {
		char *f = source_path(r, src.v[i]);
		if (sha256_file(f, hex) < 0)
			die_errno("cannot read %s", f);
		if (src.n == 1)
			sb_adds(&block, hex);
		else
			sb_addf(&block, "\n\t%s", hex);
		free(f);
	}
	sb_adds(&block, src.n == 1 ? ")" : "\n)");
	if (!B.write) {
		printf("%s\n", sb_str(&block));
		goto out;
	}
	svec lines = SVEC_INIT;
	file_lines(r->file, &lines);
	sbuf out = SBUF_INIT;
	bool found = false, skip = false;
	for (size_t i = 0; i < lines.n; i++) {
		const char *l = lines.v[i];
		if (str_prefix(l, "sha256=(")) {
			sb_addf(&out, "%s\n", sb_str(&block));
			skip = !strchr(l, ')');
			found = true;
		} else if (skip) {
			skip = !strchr(l, ')');
		} else {
			sb_addf(&out, "%s\n", l);
		}
	}
	if (!found)
		die("%s has no 'sha256=(' line to replace", r->file);
	if (file_write(r->file, out.s, out.len, 0644) < 0)
		die_errno("cannot write %s", r->file);
	msg("sha256 updated in %s", r->file);
	sb_free(&out);
	sv_free(&lines);
out:
	sb_free(&block);
	sv_free(&src);
	recipe_free(r);
	return 0;
}
