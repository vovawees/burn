/* SPDX-License-Identifier: MIT */
/* Sources: the source cache, downloads (curl or wget; built with CURL=1: libcurl inside the process,
 * HTTP/2 multiplexed), sha256 checks inside the process and the parallel prefetch that verifies
 * everything before anything is built. */
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

#ifdef BURN_CURL
#include <curl/curl.h>
#include <time.h>
#endif

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

#ifdef BURN_CURL
/* ---- libcurl: every transfer of a fetch in one process, over shared connections (HTTP/2 streams
 * of one connection per server when it speaks HTTP/2), with the retries of curl --retry 3 ---- */

typedef struct {
	const char *url, *alt; /* alt: the second address (gnu:// sources), NULL */
	const char *file;
	bool quiet;
	char *tmp;
	FILE *f;
	CURL *h;
	int tries;
	enum { X_WAIT, X_RUN, X_RETRY, X_DONE } state;
	double not_before;
	int rc;
	char err[CURL_ERROR_SIZE];
} xfer;

static double mono_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* the errors curl --retry retries, and a broken stream */
static bool transient(CURLcode c, long http)
{
	switch (c) {
	case CURLE_OPERATION_TIMEDOUT:
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_PARTIAL_FILE:
	case CURLE_GOT_NOTHING:
	case CURLE_SEND_ERROR:
	case CURLE_RECV_ERROR:
	case CURLE_HTTP2:
	case CURLE_HTTP2_STREAM:
		return true;
	case CURLE_HTTP_RETURNED_ERROR:
		return http == 408 || http == 429 || http == 500 || http == 502 || http == 503 || http == 504;
	default:
		return false;
	}
}

static bool xfer_start(CURLM *m, xfer *x)
{
	x->err[0] = 0;
	if (!(x->f = fopen(x->tmp, "wbe"))) {
		if (!x->quiet)
			report_error("cannot write %s: %s", x->tmp, strerror(errno));
		x->state = X_DONE;
		x->rc = 1;
		return false;
	}
	CURL *h = x->h = curl_easy_init();
	if (!h)
		die("libcurl: cannot start a transfer");
	curl_easy_setopt(h, CURLOPT_URL, x->url);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, x->f);
	curl_easy_setopt(h, CURLOPT_PRIVATE, x);
	curl_easy_setopt(h, CURLOPT_ERRORBUFFER, x->err);
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(h, CURLOPT_MAXREDIRS, 20L);
	curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, x->quiet ? 10L : 20L);
	curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
	curl_easy_setopt(h, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
	curl_easy_setopt(h, CURLOPT_PIPEWAIT, 1L); /* rather a stream of a connection than a new one */
	curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(h, CURLOPT_USERAGENT, "burn/" BURN_VERSION);
	curl_multi_add_handle(m, h);
	x->state = X_RUN;
	return true;
}

/* XS[0..N): JOBS transfers at a time; each ends with rc 0 (the file in place) or 1 (said why,
 * unless quiet) */
static void curl_fetch(xfer *xs, size_t n, long jobs)
{
	static bool init;
	if (!init && curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
		die("libcurl: cannot initialize");
	init = true;
	CURLM *m = curl_multi_init();
	if (!m)
		die("libcurl: cannot initialize");
	curl_multi_setopt(m, CURLMOPT_PIPELINING, (long)CURLPIPE_MULTIPLEX);
	curl_multi_setopt(m, CURLMOPT_MAX_HOST_CONNECTIONS, jobs);
	size_t next = 0, active = 0, done = 0;
	for (size_t i = 0; i < n; i++) {
		xs[i].tmp = xasprintf("%s.part", xs[i].file);
		xs[i].state = X_WAIT;
	}
	while (done < n) {
		double t = mono_now();
		for (size_t i = 0; i < next && active < (size_t)jobs; i++)
			if (xs[i].state == X_RETRY && xs[i].not_before <= t)
				active += xfer_start(m, &xs[i]);
		while (active < (size_t)jobs && next < n)
			active += xfer_start(m, &xs[next++]);
		done = 0;
		for (size_t i = 0; i < n; i++)
			done += xs[i].state == X_DONE;
		if (done == n)
			break;
		int running;
		curl_multi_perform(m, &running);
		CURLMsg *cm;
		int left;
		while ((cm = curl_multi_info_read(m, &left))) {
			if (cm->msg != CURLMSG_DONE)
				continue;
			xfer *x;
			curl_easy_getinfo(cm->easy_handle, CURLINFO_PRIVATE, (char **)&x);
			CURLcode c = cm->data.result;
			long http = 0;
			curl_easy_getinfo(x->h, CURLINFO_RESPONSE_CODE, &http);
			curl_multi_remove_handle(m, x->h);
			curl_easy_cleanup(x->h);
			x->h = NULL;
			bool ok = fclose(x->f) == 0 && c == CURLE_OK;
			x->f = NULL;
			active--;
			if (ok && rename(x->tmp, x->file) == 0) {
				x->state = X_DONE;
				x->rc = 0;
				continue;
			}
			unlink(x->tmp);
			if (c != CURLE_OK && transient(c, http) && ++x->tries < 4) {
				x->state = X_RETRY;
				x->not_before = t + 2;
			} else if (x->alt) {
				x->url = x->alt;
				x->alt = NULL;
				x->tries = 0;
				x->state = X_RETRY;
				x->not_before = t;
			} else {
				if (!x->quiet)
					report_error("download %s: %s", x->url, *x->err ? x->err : curl_easy_strerror(c));
				x->state = X_DONE;
				x->rc = 1;
			}
		}
		curl_multi_poll(m, NULL, 0, 200, NULL);
	}
	curl_multi_cleanup(m);
	for (size_t i = 0; i < n; i++) {
		free(xs[i].tmp);
		xs[i].tmp = NULL;
	}
}

static int curl_one(const char *url, const char *alt, const char *file, bool quiet)
{
	xfer x = {.url = url, .alt = alt, .file = file, .quiet = quiet};
	curl_fetch(&x, 1, 1);
	return x.rc;
}
#endif

#ifndef BURN_CURL
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
#endif

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
#ifdef BURN_CURL
	} else if (true) {
		free(tmp);
		return curl_one(url, NULL, file, true);
#endif
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
#ifdef BURN_CURL
		const char *gnu = strstr(spec, "gnu://");
		char *alt = gnu ? xasprintf("https://ftp.gnu.org/gnu/%s", gnu + 6) : NULL;
		int rc = curl_one(s.url, alt, file, false);
		free(alt);
		if (rc != 0)
			die("%s: download failed: %s", pkg, s.url);
#else
		if (download(s.url, file) != 0) {
			const char *gnu = strstr(spec, "gnu://");
			if (!gnu)
				die("%s: download failed: %s", pkg, s.url);
			char *alt = xasprintf("https://ftp.gnu.org/gnu/%s", gnu + 6);
			if (download(alt, file) != 0)
				die("%s: download failed: %s", pkg, s.url);
			free(alt);
		}
#endif
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

#ifdef BURN_CURL
/* the files missing from the source cache in one go (FILES with their SPECS, SUMS and PKGS, the
 * same file may come twice), then every check: how many failed (each said why) */
static long prefetch_curl(const svec *files, const svec *specs, const svec *sums, const svec *pkgs, long jobs,
			  size_t *unique)
{
	strmap seen = STRMAP_INIT;
	size_t *idx = xcalloc(files->n + 1, sizeof *idx), nidx = 0, nx = 0;
	for (size_t i = 0; i < files->n; i++)
		if (!sm_has(&seen, files->v[i])) {
			sm_put(&seen, files->v[i], 0);
			idx[nidx++] = i;
		}
	sm_free(&seen);
	*unique = nidx;
	xfer *xs = xcalloc(nidx + 1, sizeof *xs);
	source_spec *ss = xcalloc(nidx + 1, sizeof *ss);
	char **alts = xcalloc(nidx + 1, sizeof *alts);
	for (size_t k = 0; k < nidx; k++) {
		size_t i = idx[k];
		source_info(specs->v[i], &ss[k]);
		if (is_file(files->v[i]))
			continue;
		const char *gnu = strstr(specs->v[i], "gnu://");
		alts[k] = gnu ? xasprintf("https://ftp.gnu.org/gnu/%s", gnu + 6) : NULL;
		msg2("download %s", ss[k].name);
		xs[nx++] = (xfer){.url = ss[k].url, .alt = alts[k], .file = files->v[i]};
	}
	if (nx)
		curl_fetch(xs, nx, jobs);
	long failed = 0;
	for (size_t k = 0; k < nidx; k++) {
		size_t i = idx[k];
		const char *pkg = pkgs->v[i], *name = ss[k].name;
		if (!is_file(files->v[i])) {
			report_error("%s: download failed: %s", pkg, ss[k].url);
			failed++;
		} else if (!*sums->v[i]) {
			report_error("%s: missing sha256 for %s", pkg, name);
			failed++;
		} else if (!sum_ok(files->v[i], sums->v[i])) {
			char *bad = xasprintf("%s.bad", files->v[i]);
			rename(files->v[i], bad);
			free(bad);
			report_error("%s: sha256 mismatch for %s (moved aside as .bad)", pkg, name);
			failed++;
		}
		source_spec_free(&ss[k]);
		free(alts[k]);
	}
	free(ss);
	free(alts);
	free(xs);
	free(idx);
	return failed;
}
#endif

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
#ifdef BURN_CURL
	failed = prefetch_curl(&files, &specs, &sums, &pkgs, jobs, &unique);
	(void)running;
#else
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
#endif
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
