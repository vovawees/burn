/* SPDX-License-Identifier: MIT */
#include "spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <net/if.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int write_proc(const char *file, const char *text)
{
	int fd = open(file, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t n = write(fd, text, strlen(text));
	int e = errno;
	close(fd);
	errno = e;
	return n == (ssize_t)strlen(text) ? 0 : -1;
}

/* the loopback interface of a new network namespace starts down: tests listen on 127.0.0.1 */
static int loopback_up(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	memcpy(ifr.ifr_name, "lo", 3);
	int rc = ioctl(fd, SIOCGIFFLAGS, &ifr);
	if (rc == 0) {
		ifr.ifr_flags |= IFF_UP;
		rc = ioctl(fd, SIOCSIFFLAGS, &ifr);
	}
	int e = errno;
	close(fd);
	errno = e;
	return rc;
}

int isolate(const isolation *s, const char **what)
{
	if (s->userns) {
		/* our ids must be written before anything else needs them */
		uid_t uid = geteuid();
		gid_t gid = getegid();
		char map[64];
		*what = "unshare(CLONE_NEWUSER|CLONE_NEWNET)";
		if (unshare(CLONE_NEWUSER | (s->net ? CLONE_NEWNET : 0)) < 0)
			return -1;
		*what = "/proc/self/uid_map";
		snprintf(map, sizeof map, "%u %u 1\n", (unsigned)uid, (unsigned)uid);
		if (write_proc("/proc/self/uid_map", map) < 0)
			return -1;
		*what = "/proc/self/setgroups";
		if (write_proc("/proc/self/setgroups", "deny\n") < 0 && errno != ENOENT)
			return -1;
		*what = "/proc/self/gid_map";
		snprintf(map, sizeof map, "%u %u 1\n", (unsigned)gid, (unsigned)gid);
		if (write_proc("/proc/self/gid_map", map) < 0)
			return -1;
	} else if (s->net) {
		*what = "unshare(CLONE_NEWNET)";
		if (unshare(CLONE_NEWNET) < 0)
			return -1;
	}
	if (s->net) {
		*what = "loopback up";
		if (loopback_up() < 0)
			return -1;
	}
	if (s->drop) {
		*what = "setgroups/setgid/setuid";
		if (setgroups(0, NULL) < 0 || setgid(s->gid) < 0 || setuid(s->uid) < 0)
			return -1;
	}
	/* no way back to privileges: setuid programs (su, sudo, fusermount3) do not raise them */
	*what = "PR_SET_NO_NEW_PRIVS";
	if ((s->drop || s->userns) && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
		return -1;
	return 0;
}

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
	const char *what;
	if (o->iso && isolate(o->iso, &what) < 0) {
		fprintf(stderr, "burn: build isolation: %s: %s (ISOLATE=0 in burn.conf turns it off)\n", what,
			strerror(errno));
		_exit(127);
	}
	if (o->envp)
		environ = o->envp; /* execvp searches the PATH of the new environment */
	if (o->cwd) { /* (cd DIR && PROG): PWD is the logical path */
		char *pwd = path_logical(o->cwd, getenv("PWD"));
		setenv("PWD", pwd, 1);
		free(pwd);
	}
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
