/* SPDX-License-Identifier: MIT */
/* Running programs directly (fork + exec, never through a shell). */
#ifndef BURN_SPAWN_H
#define BURN_SPAWN_H

#include <sys/types.h>

#include "util.h"

/* the isolation of the build phases (burn.conf ISOLATE): an own network namespace with only lo
 * (up), so a build cannot reach the network; as root also an unprivileged user (BUILD_USER). */
typedef struct {
	bool net;    /* a network namespace: unshare(CLONE_NEWNET) */
	bool userns; /* not root: in an own user namespace (our uid and gid map to themselves) */
	bool drop;   /* root: then setgid/setuid to uid and gid, without supplementary groups */
	uid_t uid;
	gid_t gid;
} isolation;

/* sets up S in this (child) process before it execs: 0, or -1 with errno and *WHAT, the step
 * that failed */
int isolate(const isolation *s, const char **what);

typedef struct {
	const char *cwd;   /* chdir first */
	char **envp;       /* NULL: our environment; also used for the PATH lookup */
	bool stdin_null;   /* stdin from /dev/null */
	bool quiet;        /* stdout and stderr to /dev/null */
	int log_fd;        /* > 0: stdout and stderr appended there */
	bool tee;          /* with log_fd: also copied to our stderr */
	sbuf *out;         /* capture stdout (stderr goes to log_fd, /dev/null when quiet, else ours) */
	bool err_to_out;   /* with out: capture stderr too (2>&1) */
	const isolation *iso; /* isolate the child (a build phase); a failure is its exit 127 */
} run_opts;

/* Exit status of the program, 128 + signal, 127 when it cannot be started. */
int run(char *const argv[], const run_opts *o);
/* FN(ARG) in a child process with the options of run() (redirections, environment, cwd): its
 * return value is the exit status. For C code whose output belongs in a log. */
int run_func(int (*fn)(void *), void *arg, const run_opts *o);
/* run() with a NULL terminated argument list and default options. */
int runl(const char *prog, ...) __attribute__((sentinel));
/* Captured stdout without the trailing newline, NULL when the program failed. */
char *run_output(char *const argv[], const run_opts *o);

/* environment lists ("KEY=value" entries) for child processes */
void env_from_environ(svec *env);
void env_set(svec *env, const char *key, const char *val);
void env_unset(svec *env, const char *key);
const char *env_get(const svec *env, const char *key);

#endif
