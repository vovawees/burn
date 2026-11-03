/* SPDX-License-Identifier: MIT */
/* Running programs directly (fork + exec, never through a shell). */
#ifndef BURN_SPAWN_H
#define BURN_SPAWN_H

#include "util.h"

typedef struct {
	const char *cwd;   /* chdir first */
	char **envp;       /* NULL: our environment; also used for the PATH lookup */
	bool stdin_null;   /* stdin from /dev/null */
	bool quiet;        /* stdout and stderr to /dev/null */
	int log_fd;        /* > 0: stdout and stderr appended there */
	bool tee;          /* with log_fd: also copied to our stderr */
	sbuf *out;         /* capture stdout (stderr goes to log_fd, /dev/null when quiet, else ours) */
} run_opts;

/* Exit status of the program, 128 + signal, 127 when it cannot be started. */
int run(char *const argv[], const run_opts *o);
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
