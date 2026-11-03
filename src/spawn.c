/* SPDX-License-Identifier: MIT */
#include "spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* the child's side of run_opts: working directory, redirections, environment */
static void child_setup(const run_opts *o, int pipe_w)
{
	die_hook = NULL;
	if (o->cwd && chdir(o->cwd) < 0) {
		fprintf(stderr, "burn: cd %s: %s\n", o->cwd, strerror(errno));
		_exit(127);
	}
	int null = -1;
	if (o->stdin_null || o->quiet) {
		null = open("/dev/null", O_RDWR);
		if (null < 0)
			_exit(127);
	}
	if (o->stdin_null)
		dup2(null, 0);
	if (o->out) {
		dup2(pipe_w, 1);
		if (o->err_to_out)
			dup2(pipe_w, 2);
		else if (o->log_fd > 0)
			dup2(o->log_fd, 2);
		else if (o->quiet)
			dup2(null, 2);
	} else if (o->log_fd > 0) {
		int fd = o->tee ? pipe_w : o->log_fd;
		dup2(fd, 1);
		dup2(fd, 2);
	} else if (o->quiet) {
		dup2(null, 1);
		dup2(null, 2);
	}
	if (null > 2)
		close(null);
	if (o->envp)
		environ = o->envp; /* execvp searches the PATH of the new environment */
}

static NORETURN void child(char *const argv[], int (*fn)(void *), void *arg, const run_opts *o, int pipe_w)
{
	child_setup(o, pipe_w);
	if (fn) {
		if (pipe_w > 2)
			close(pipe_w); /* dup2()ed already; the readers see the end when the child exits */
		int rc = fn(arg);
		fflush(stdout);
		fflush(stderr);
		_exit(rc & 255);
	}
	execvp(argv[0], argv);
	int e = errno;
	if (!o->quiet)
		fprintf(stderr, "burn: %s: %s\n", argv[0], strerror(e));
	_exit(127);
}

static int run_common(char *const argv[], int (*fn)(void *), void *arg, const run_opts *o)
{
	static const run_opts none = {0};
	if (!o)
		o = &none;
	int p[2] = {-1, -1};
	bool piped = o->out || (o->log_fd > 0 && o->tee);
	if (piped && pipe2(p, O_CLOEXEC) < 0)
		die_errno("pipe");
	fflush(stdout);
	fflush(stderr);
	pid_t pid = fork();
	if (pid < 0)
		die_errno("fork");
	if (pid == 0)
		child(argv, fn, arg, o, p[1]);
	if (piped) {
		close(p[1]);
		char buf[65536];
		for (;;) {
			ssize_t n = read(p[0], buf, sizeof buf);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			if (o->out) {
				sb_addn(o->out, buf, (size_t)n);
			} else {
				write_all(o->log_fd, buf, (size_t)n);
				write_all(2, buf, (size_t)n);
			}
		}
		close(p[0]);
	}
	int st;
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			die_errno("waitpid");
	if (WIFEXITED(st))
		return WEXITSTATUS(st);
	if (WIFSIGNALED(st))
		return 128 + WTERMSIG(st);
	return 1;
}

int run(char *const argv[], const run_opts *o)
{
	return run_common(argv, NULL, NULL, o);
}

int run_func(int (*fn)(void *), void *arg, const run_opts *o)
{
	return run_common(NULL, fn, arg, o);
}

int runl(const char *prog, ...)
{
	svec argv = SVEC_INIT;
	va_list ap;
	va_start(ap, prog);
	for (const char *a = prog; a; a = va_arg(ap, const char *))
		sv_push(&argv, a);
	va_end(ap);
	int rc = run(sv_argv(&argv), NULL);
	sv_free(&argv);
	return rc;
}

char *run_output(char *const argv[], const run_opts *o)
{
	run_opts c = o ? *o : (run_opts){0};
	sbuf out = SBUF_INIT;
	c.out = &out;
	if (run(argv, &c) != 0) {
		sb_free(&out);
		return NULL;
	}
	while (out.len && out.s[out.len - 1] == '\n')
		out.s[--out.len] = 0;
	return sb_steal(&out);
}

void env_from_environ(svec *env)
{
	for (char **e = environ; *e; e++)
		sv_push(env, *e);
}

static ssize_t env_find(const svec *env, const char *key)
{
	size_t k = strlen(key);
	for (size_t i = 0; i < env->n; i++)
		if (!strncmp(env->v[i], key, k) && env->v[i][k] == '=')
			return (ssize_t)i;
	return -1;
}

void env_set(svec *env, const char *key, const char *val)
{
	ssize_t i = env_find(env, key);
	char *kv = xasprintf("%s=%s", key, val);
	if (i >= 0) {
		free(env->v[i]);
		env->v[i] = kv;
	} else {
		sv_push_own(env, kv);
	}
}

void env_unset(svec *env, const char *key)
{
	ssize_t i = env_find(env, key);
	if (i >= 0)
		sv_remove(env, (size_t)i);
}

const char *env_get(const svec *env, const char *key)
{
	ssize_t i = env_find(env, key);
	return i >= 0 ? env->v[i] + strlen(key) + 1 : NULL;
}
