/* SPDX-License-Identifier: MIT */
/* Small foundation shared by every module: output, memory, strings, vectors, files. */
#ifndef BURN_UTIL_H
#define BURN_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define BURN_VERSION "0.5.0"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define PRINTF(a, b) __attribute__((format(printf, a, b)))
#define NORETURN __attribute__((noreturn))

/* ---- output (stderr; colors when it is a terminal and NO_COLOR is unset) ---- */
extern bool opt_yes; /* --yes: confirm() never asks */
void log_init(void);
void msg(const char *fmt, ...) PRINTF(1, 2);  /* ==> step */
void msg2(const char *fmt, ...) PRINTF(1, 2); /*   -> detail */
void warn(const char *fmt, ...) PRINTF(1, 2);
NORETURN void die(const char *fmt, ...) PRINTF(1, 2);
NORETURN void die_errno(const char *fmt, ...) PRINTF(1, 2); /* appends ": strerror(errno)" */
bool confirm(const char *question);
/* run once when burn exits on an error (die, a failed command): the triggers of what is already
 * installed. Forked children that do not exec clear it. */
extern void (*die_hook)(void);
NORETURN void exit_status(int rc); /* die_hook, then exit(rc) */

/* ---- memory (out of memory is fatal) ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...) PRINTF(1, 2);
char *xvasprintf(const char *fmt, va_list ap);

/* ---- growable string ---- */
typedef struct {
	char *s;
	size_t len, cap;
} sbuf;
#define SBUF_INIT {NULL, 0, 0}
void sb_grow(sbuf *b, size_t extra);
void sb_addn(sbuf *b, const char *s, size_t n);
void sb_adds(sbuf *b, const char *s);
void sb_addc(sbuf *b, char c);
void sb_addf(sbuf *b, const char *fmt, ...) PRINTF(2, 3);
const char *sb_str(sbuf *b); /* never NULL */
char *sb_steal(sbuf *b);     /* caller owns the string, the buffer is empty again */
void sb_reset(sbuf *b);
void sb_free(sbuf *b);

/* ---- vector of owned strings, always NULL terminated (usable as argv) ---- */
typedef struct {
	char **v;
	size_t n, cap;
} svec;
#define SVEC_INIT {NULL, 0, 0}
void sv_push(svec *v, const char *s);
void sv_push_own(svec *v, char *s);
void sv_pushf(svec *v, const char *fmt, ...) PRINTF(2, 3);
void sv_extend(svec *v, const svec *w);
bool sv_has(const svec *v, const char *s);
bool sv_bsearch(const svec *v, const char *s); /* v sorted by sv_sort */
ssize_t sv_index(const svec *v, const char *s);
void sv_remove(svec *v, size_t i);
void sv_sort(svec *v);      /* byte order (LC_ALL=C sort) */
void sv_sort_uniq(svec *v); /* sort -u */
char *sv_join(const svec *v, const char *sep);
void sv_split_ws(svec *v, const char *s); /* appends the whitespace separated words of s */
char **sv_argv(svec *v);
void sv_clear(svec *v);
void sv_free(svec *v);

/* ---- string map: open addressing, keys are copied (a set when the values are unused) ---- */
typedef struct {
	char **keys;
	size_t *vals;
	size_t n, cap;
} strmap;
#define STRMAP_INIT {NULL, NULL, 0, 0}
void sm_put(strmap *m, const char *key, size_t val); /* inserts or replaces */
bool sm_get(const strmap *m, const char *key, size_t *val);
bool sm_has(const strmap *m, const char *key);
void sm_free(strmap *m);

/* ---- strings ---- */
bool str_prefix(const char *s, const char *prefix);
bool str_suffix(const char *s, const char *suffix);
bool str_eq(const char *a, const char *b); /* NULL equals "" */
bool str_uint(const char *s);              /* one or more digits */

/* ---- paths ---- */
char *path_join(const char *a, const char *b); /* "$a/$b" with single slashes; a may be "" */
char *path_abs(const char *p);                 /* readlink -m */
char *path_real(const char *p);                /* readlink -f; NULL if it does not exist */
char *path_logical(const char *dir, const char *oldpwd); /* $PWD after cd DIR (bash) */
const char *path_base(const char *p);
char *path_dir(const char *p);

/* ---- files ---- */
bool is_file(const char *p);
bool is_dir(const char *p);
bool is_link(const char *p);
bool exists(const char *p); /* lstat: dangling links exist */
bool is_exec(const char *p);
int mkdir_p(const char *path, mode_t mode);
int rm_rf(const char *path);
/* entries of DIR matching the fnmatch PATTERN as DIR/NAME, sorted; dot files only for ".*" patterns */
void glob_paths(const char *dir, const char *pattern, svec *out);
char *file_read(const char *path, size_t *len); /* NUL terminated; NULL + errno on error */
int file_write(const char *path, const void *data, size_t len, mode_t mode); /* atomic */
int file_append(const char *path, const char *s);
int file_lines(const char *path, svec *out); /* appends lines; a missing file has none */
int write_all(int fd, const void *buf, size_t len);
char *find_program(const char *name); /* PATH lookup; NULL if not found */
bool have(const char *prog);
long nproc(void);

#endif
