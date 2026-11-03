/* SPDX-License-Identifier: MIT */
#include "shparse.h"

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FNM_FLAGS FNM_EXTMATCH /* the prototype ran with shopt -s extglob */

static bool name_start(char ch)
{
	return ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

static bool name_char(char ch)
{
	return name_start(ch) || (ch >= '0' && ch <= '9');
}

static bool valid_name(const char *s)
{
	if (!name_start(*s))
		return false;
	while (*++s)
		if (!name_char(*s))
			return false;
	return true;
}

static bool is_ifs(char ch)
{
	return ch == ' ' || ch == '\t' || ch == '\n';
}

static bool is_meta(char ch)
{
	return ch == ' ' || ch == '\t' || ch == '\n' || ch == ';' || ch == '&' || ch == '|' || ch == '<' ||
	       ch == '>' || ch == '(' || ch == ')';
}

/* ======================================================================= variables */

void sh_init(shenv *e, const shenv *parent, bool use_environ)
{
	memset(e, 0, sizeof *e);
	e->parent = parent;
	e->use_environ = use_environ;
}

void sh_free(shenv *e)
{
	for (size_t i = 0; i < e->n; i++) {
		free(e->v[i].name);
		sv_free(&e->v[i].val);
	}
	free(e->v);
	memset(e, 0, sizeof *e);
}

/* the entry for name in e itself, tombstones included */
static shvar *entry(const shenv *e, const char *name)
{
	for (size_t i = 0; i < e->n; i++)
		if (!strcmp(e->v[i].name, name))
			return &e->v[i];
	return NULL;
}

static bool environ_fallback(const shenv *e)
{
	for (; e; e = e->parent)
		if (e->use_environ)
			return true;
	return false;
}

/* the visible variable from first on; *envval is the environment's value when no layer knows
 * the name and env allows the fallback */
static const shvar *lookup_chain(const shenv *first, bool env, const char *name, const char **envval)
{
	*envval = NULL;
	for (const shenv *p = first; p; p = p->parent) {
		const shvar *v = entry(p, name);
		if (v)
			return v->unset ? NULL : v;
	}
	if (env)
		*envval = getenv(name);
	return NULL;
}

static const shvar *lookup(const shenv *e, const char *name, const char **envval)
{
	return lookup_chain(e, environ_fallback(e), name, envval);
}

shvar *sh_find_local(shenv *e, const char *name)
{
	shvar *v = entry(e, name);
	return v && !v->unset ? v : NULL;
}

const shvar *sh_find(const shenv *e, const char *name)
{
	const char *envval;
	return lookup(e, name, &envval);
}

bool sh_isset(const shenv *e, const char *name)
{
	const char *envval;
	return lookup(e, name, &envval) || envval;
}

const char *sh_get(const shenv *e, const char *name)
{
	const char *envval;
	const shvar *v = lookup(e, name, &envval);
	if (v)
		return v->val.n ? v->val.v[0] : NULL;
	return envval;
}

bool sh_get_list(const shenv *e, const char *name, svec *out)
{
	const char *envval;
	const shvar *v = lookup(e, name, &envval);
	if (v) {
		sv_extend(out, &v->val);
		return true;
	}
	if (envval)
		sv_push(out, envval);
	return envval != NULL;
}

/* the variable in e itself, created as a copy of what the parents (or the environment) hold */
static shvar *local_var(shenv *e, const char *name)
{
	shvar *v = entry(e, name);
	if (v) {
		if (v->unset) { /* assigning revives a tombstone; `export name` before it counts */
			v->unset = false;
			v->array = false;
			sv_clear(&v->val);
		}
		return v;
	}
	const char *envval;
	const shvar *p = lookup_chain(e->parent, environ_fallback(e), name, &envval);
	if (e->n == e->cap) {
		e->cap = e->cap ? e->cap * 2 : 32;
		e->v = xrealloc(e->v, e->cap * sizeof *e->v);
	}
	v = &e->v[e->n++];
	memset(v, 0, sizeof *v);
	v->name = xstrdup(name);
	if (p) {
		v->array = p->array;
		v->exported = p->exported;
		sv_extend(&v->val, &p->val);
	} else if (envval) {
		v->exported = true;
		sv_push(&v->val, envval);
	}
	return v;
}

void sh_set(shenv *e, const char *name, const char *val)
{
	shvar *v = local_var(e, name);
	if (v->val.n) {
		free(v->val.v[0]);
		v->val.v[0] = xstrdup(val);
	} else {
		sv_push(&v->val, val);
	}
}

void sh_set_list(shenv *e, const char *name, const svec *vals)
{
	shvar *v = local_var(e, name);
	v->array = true;
	sv_clear(&v->val);
	sv_extend(&v->val, vals);
}

void sh_append(shenv *e, const char *name, const char *val)
{
	shvar *v = local_var(e, name);
	if (v->val.n) {
		char *s = xasprintf("%s%s", v->val.v[0], val);
		free(v->val.v[0]);
		v->val.v[0] = s;
	} else {
		sv_push(&v->val, val);
	}
}

void sh_append_list(shenv *e, const char *name, const svec *vals)
{
	shvar *v = local_var(e, name);
	v->array = true;
	sv_extend(&v->val, vals);
}

void sh_unset(shenv *e, const char *name)
{
	shvar *v = entry(e, name);
	if (!e->parent && !e->use_environ) {
		if (v) {
			free(v->name);
			sv_free(&v->val);
			size_t i = (size_t)(v - e->v);
			memmove(e->v + i, e->v + i + 1, (e->n - i - 1) * sizeof *e->v);
			e->n--;
		}
		return;
	}
	/* a tombstone hides what the parents and the environment hold */
	v = local_var(e, name);
	v->unset = true;
	v->array = false;
	v->exported = false;
	sv_clear(&v->val);
}

void sh_export(shenv *e, const char *name)
{
	shvar *v = entry(e, name);
	if (v && v->unset) {
		v->exported = true; /* bash: export x; x=1 exports x */
		return;
	}
	if (!sh_isset(e, name)) {
		v = local_var(e, name);
		v->unset = true;
		v->exported = true;
		return;
	}
	local_var(e, name)->exported = true;
}

static int name_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

void sh_local_names(const shenv *e, svec *out)
{
	size_t first = out->n;
	for (size_t i = 0; i < e->n; i++)
		if (!e->v[i].unset)
			sv_push(out, e->v[i].name);
	if (out->n - first > 1)
		qsort(out->v + first, out->n - first, sizeof *out->v, name_cmp);
}

void sh_quote(sbuf *b, const char *s)
{
	bool safe = *s != 0;
	for (const char *p = s; *p && safe; p++)
		safe = name_char(*p) || strchr("-./=:+,@%", *p);
	if (safe) {
		sb_adds(b, s);
		return;
	}
	sb_addc(b, '\'');
	for (; *s; s++) {
		if (*s == '\'')
			sb_adds(b, "'\\''");
		else
			sb_addc(b, *s);
	}
	sb_addc(b, '\'');
}

/* ======================================================================= errors */

void sh_ctx_init(shctx *c, shenv *env)
{
	memset(c, 0, sizeof *c);
	c->env = env;
}

void sh_ctx_free(shctx *c)
{
	sv_free(&c->funcs);
	free(c->err);
	c->err = NULL;
}

int sh_error(shctx *c, const char *fmt, ...)
{
	if (c->err)
		return -1;
	va_list ap;
	va_start(ap, fmt);
	char *m = xvasprintf(fmt, ap);
	va_end(ap);
	if (c->file) {
		c->err = xasprintf("%s:%d: %s", c->file, c->line, m);
		free(m);
	} else {
		c->err = m;
	}
	return -1;
}

/* ======================================================================= raw text scanning */

enum { E_SQUOTE = 1, E_DQUOTE, E_ANSI, E_BRACE, E_CMDSUB, E_ARITH, E_BACKTICK };

static const char *scan_error(int e)
{
	switch (e) {
	case E_SQUOTE:
		return "unterminated ' quote";
	case E_DQUOTE:
		return "unterminated \" quote";
	case E_ANSI:
		return "unterminated $'...' quote";
	case E_BRACE:
		return "unterminated ${...}";
	case E_CMDSUB:
		return "command substitution $(...) is not allowed at the top level: put it into a function";
	case E_ARITH:
		return "arithmetic expansion $((...)) is not supported at the top level: write the number";
	default:
		return "command substitution `...` is not allowed at the top level: put it into a function";
	}
}

static size_t skip_dollar(const char *s, size_t n, size_t i, int *nl, int *err);

static size_t skip_squote(const char *s, size_t n, size_t i, int *nl, int *err)
{
	for (i++; i < n; i++) {
		if (s[i] == '\n')
			(*nl)++;
		if (s[i] == '\'')
			return i + 1;
	}
	*err = E_SQUOTE;
	return n;
}

static size_t skip_dquote(const char *s, size_t n, size_t i, int *nl, int *err)
{
	for (i++; i < n && !*err;) {
		char ch = s[i];
		if (ch == '"')
			return i + 1;
		if (ch == '\\') {
			if (i + 1 < n && s[i + 1] == '\n')
				(*nl)++;
			i += 2;
			continue;
		}
		if (ch == '`') {
			*err = E_BACKTICK;
			return i;
		}
		if (ch == '$') {
			i = skip_dollar(s, n, i, nl, err);
			continue;
		}
		if (ch == '\n')
			(*nl)++;
		i++;
	}
	if (!*err)
		*err = E_DQUOTE;
	return n;
}

/* s[i] == '$' */
static size_t skip_dollar(const char *s, size_t n, size_t i, int *nl, int *err)
{
	char nx = i + 1 < n ? s[i + 1] : 0;
	if (nx == '\'') {
		for (i += 2; i < n; i++) {
			if (s[i] == '\\') {
				i++;
				continue;
			}
			if (s[i] == '\n')
				(*nl)++;
			if (s[i] == '\'')
				return i + 1;
		}
		*err = E_ANSI;
		return n;
	}
	if (nx == '(') {
		*err = i + 2 < n && s[i + 2] == '(' ? E_ARITH : E_CMDSUB;
		return i;
	}
	if (nx == '{') {
		for (i += 2; i < n && !*err;) {
			char ch = s[i];
			if (ch == '}')
				return i + 1;
			if (ch == '\\') {
				if (i + 1 < n && s[i + 1] == '\n')
					(*nl)++;
				i += 2;
				continue;
			}
			if (ch == '\'') {
				i = skip_squote(s, n, i, nl, err);
				continue;
			}
			if (ch == '"') {
				i = skip_dquote(s, n, i, nl, err);
				continue;
			}
			if (ch == '`') {
				*err = E_BACKTICK;
				return i;
			}
			if (ch == '$') {
				i = skip_dollar(s, n, i, nl, err);
				continue;
			}
			if (ch == '\n')
				(*nl)++;
			i++;
		}
		if (!*err)
			*err = E_BRACE;
		return n;
	}
	if (name_start(nx)) {
		for (i += 2; i < n && name_char(s[i]); i++)
			;
		return i;
	}
	if (nx && strchr("0123456789@*#?$!-", nx))
		return i + 2;
	return i + 1;
}

/* ======================================================================= expansion */

enum { X_SCALAR, X_FIELDS, X_PATTERN };

typedef struct {
	shctx *c;
	int mode;
	svec *out; /* X_FIELDS: the finished fields */
	sbuf cur;
	bool has;       /* the current field exists (a quoted "" counts) */
	bool empty_at;  /* the last expansion was "${a[@]}" without elements */
	char glob;      /* X_FIELDS: an unquoted pattern character (*, ? or [...]) was seen */
	bool bracket;   /* an unquoted [ was seen */
	int tilde;      /* 1: ~ at the start of words, 2: also after ':' (scalar assignments) */
	int split;      /* inside the word of an unquoted ${v:-word}: unquoted blanks split fields */
} X;

static void x_lit(X *x, const char *s, size_t n, bool quoted)
{
	if (x->mode == X_PATTERN && quoted) {
		for (size_t i = 0; i < n; i++) {
			if (strchr("*?[]\\+@!()|", s[i]))
				sb_addc(&x->cur, '\\');
			sb_addc(&x->cur, s[i]);
		}
	} else {
		sb_addn(&x->cur, s, n);
	}
	/* bash would do pathname expansion (with nullglob: drop the word when nothing matches) */
	if (x->mode == X_FIELDS && !quoted)
		for (size_t i = 0; i < n && !x->glob; i++) {
			if (s[i] == '*' || s[i] == '?' || (s[i] == ']' && x->bracket))
				x->glob = s[i] == ']' ? '[' : s[i];
			else if (s[i] == '[')
				x->bracket = true;
		}
	x->has = true;
}

static void x_field(X *x)
{
	if (x->mode == X_FIELDS) {
		sv_push(x->out, sb_str(&x->cur));
		sb_reset(&x->cur);
	}
	x->has = false;
	x->bracket = false;
}

static void x_emit(X *x, const svec *vals, bool at, bool quoted)
{
	if (x->mode == X_FIELDS && quoted && at) {
		if (!vals->n) {
			x->empty_at = true;
			return;
		}
		for (size_t k = 0; k < vals->n; k++) {
			if (k)
				x_field(x);
			x_lit(x, vals->v[k], strlen(vals->v[k]), true);
		}
		return;
	}
	char *j = sv_join(vals, " ");
	size_t len = strlen(j);
	if (x->mode != X_FIELDS || quoted) {
		x_lit(x, j, len, quoted);
		free(j);
		return;
	}
	/* unquoted in a field context: split on blanks */
	for (size_t a = 0; a < len;) {
		size_t b = a;
		while (b < len && is_ifs(j[b]))
			b++;
		if (b > a) {
			if (x->has)
				x_field(x);
			a = b;
			continue;
		}
		while (b < len && !is_ifs(j[b]))
			b++;
		x_lit(x, j + a, b - a, false);
		a = b;
	}
	free(j);
}

static void x_emit1(X *x, const char *s, bool quoted)
{
	svec one = SVEC_INIT;
	sv_push(&one, s);
	x_emit(x, &one, false, quoted);
	sv_free(&one);
}

static int x_walk(X *x, const char *w, size_t n);

/* expand w[0..n) on its own (X_SCALAR or X_PATTERN); tilde: see X */
static int expand_tilde(shctx *c, const char *w, size_t n, int mode, int tilde, char **out)
{
	X x = {.c = c, .mode = mode, .tilde = tilde};
	int rc = x_walk(&x, w, n);
	*out = sb_steal(&x.cur);
	return rc;
}

static int expand_sub(shctx *c, const char *w, size_t n, int mode, char **out)
{
	X x = {.c = c, .mode = mode};
	int rc = x_walk(&x, w, n);
	*out = sb_steal(&x.cur);
	return rc;
}

static void ansi_decode(sbuf *b, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (s[i] != '\\' || i + 1 >= n) {
			sb_addc(b, s[i]);
			continue;
		}
		char e = s[++i];
		switch (e) {
		case 'a': sb_addc(b, '\a'); break;
		case 'b': sb_addc(b, '\b'); break;
		case 'e':
		case 'E': sb_addc(b, '\033'); break;
		case 'f': sb_addc(b, '\f'); break;
		case 'n': sb_addc(b, '\n'); break;
		case 'r': sb_addc(b, '\r'); break;
		case 't': sb_addc(b, '\t'); break;
		case 'v': sb_addc(b, '\v'); break;
		case '\\':
		case '\'':
		case '"':
		case '?': sb_addc(b, e); break;
		case 'c':
			if (i + 1 < n) {
				char ch = s[++i];
				sb_addc(b, (char)((ch >= 'a' && ch <= 'z' ? ch - 32 : ch) & 0x1f));
			} else {
				sb_adds(b, "\\c");
			}
			break;
		case 'u':
		case 'U': {
			unsigned long cp = 0;
			int k = 0, max = e == 'u' ? 4 : 8;
			while (k < max && i + 1 < n && s[i + 1] && strchr("0123456789abcdefABCDEF", s[i + 1])) {
				char h = s[++i];
				cp = cp * 16 + (unsigned long)(h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
				k++;
			}
			if (!k) {
				sb_addc(b, '\\');
				sb_addc(b, e);
			} else if (cp < 0x80) {
				sb_addc(b, (char)cp);
			} else if (cp < 0x800) {
				sb_addc(b, (char)(0xc0 | cp >> 6));
				sb_addc(b, (char)(0x80 | (cp & 0x3f)));
			} else if (cp < 0x10000) {
				sb_addc(b, (char)(0xe0 | cp >> 12));
				sb_addc(b, (char)(0x80 | (cp >> 6 & 0x3f)));
				sb_addc(b, (char)(0x80 | (cp & 0x3f)));
			} else if (cp < 0x110000) {
				sb_addc(b, (char)(0xf0 | cp >> 18));
				sb_addc(b, (char)(0x80 | (cp >> 12 & 0x3f)));
				sb_addc(b, (char)(0x80 | (cp >> 6 & 0x3f)));
				sb_addc(b, (char)(0x80 | (cp & 0x3f)));
			}
			break;
		}
		case 'x': {
			int v = 0, k = 0;
			while (k < 2 && i + 1 < n && strchr("0123456789abcdefABCDEF", s[i + 1]) && s[i + 1]) {
				char h = s[++i];
				v = v * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
				k++;
			}
			if (k)
				sb_addc(b, (char)v);
			else
				sb_adds(b, "\\x");
			break;
		}
		default:
			if (e >= '0' && e <= '7') {
				int v = e - '0', k = 1;
				while (k < 3 && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '7') {
					v = v * 8 + (s[++i] - '0');
					k++;
				}
				sb_addc(b, (char)v);
			} else {
				sb_addc(b, '\\');
				sb_addc(b, e);
			}
		}
	}
}

static char *trim(const char *s, const char *pat, char op, bool longest)
{
	size_t n = strlen(s);
	char *res = NULL;
	if (op == '#') {
		char *buf = xmalloc(n + 1);
		for (size_t k0 = 0; k0 <= n && !res; k0++) {
			size_t k = longest ? n - k0 : k0;
			memcpy(buf, s, k);
			buf[k] = 0;
			if (!fnmatch(pat, buf, FNM_FLAGS))
				res = xstrdup(s + k);
		}
		free(buf);
	} else {
		for (size_t k0 = 0; k0 <= n && !res; k0++) {
			size_t k = longest ? k0 : n - k0;
			if (!fnmatch(pat, s + k, FNM_FLAGS))
				res = xstrndup(s, k);
		}
	}
	return res ? res : xstrdup(s);
}

static char *replace(const char *s, const char *pat, const char *rep, char mode)
{
	size_t n = strlen(s);
	sbuf o = SBUF_INIT;
	char *buf = xmalloc(n + 1);
	if (mode == '#') {
		size_t L = n + 1;
		while (L-- > 0) {
			memcpy(buf, s, L);
			buf[L] = 0;
			if (!fnmatch(pat, buf, FNM_FLAGS))
				break;
		}
		if (L != (size_t)-1) {
			sb_adds(&o, rep);
			sb_adds(&o, s + L);
		} else {
			sb_adds(&o, s);
		}
	} else if (mode == '%') {
		size_t k = 0;
		while (k <= n && fnmatch(pat, s + k, FNM_FLAGS))
			k++;
		sb_addn(&o, s, k <= n ? k : n);
		if (k <= n)
			sb_adds(&o, rep);
	} else {
		bool done = !*pat;
		for (size_t i = 0; i < n;) {
			size_t L = 0;
			if (!done)
				for (L = n - i; L > 0; L--) {
					memcpy(buf, s + i, L);
					buf[L] = 0;
					if (!fnmatch(pat, buf, FNM_FLAGS))
						break;
				}
			if (L > 0) {
				sb_adds(&o, rep);
				i += L;
				done = mode != '/';
			} else {
				sb_addc(&o, s[i++]);
			}
		}
	}
	free(buf);
	return sb_steal(&o);
}

/* "N", " -N", "(-N)" -> value; false when it is anything else (arithmetic is not supported) */
static bool parse_offset(const char *s, size_t n, long *v)
{
	size_t i = 0;
	while (i < n && s[i] == ' ')
		i++;
	bool paren = i < n && s[i] == '(';
	if (paren)
		i++;
	bool neg = i < n && s[i] == '-';
	if (neg)
		i++;
	if (i >= n || s[i] < '0' || s[i] > '9')
		return false;
	long r = 0;
	while (i < n && s[i] >= '0' && s[i] <= '9')
		r = r * 10 + (s[i++] - '0');
	if (paren) {
		if (i >= n || s[i] != ')')
			return false;
		i++;
	}
	while (i < n && s[i] == ' ')
		i++;
	*v = neg ? -r : r;
	return i == n;
}

/* bash substring semantics on [0, n): start and end indexes, false when empty */
static bool substr_range(long n, long off, bool has_len, long len, long *a, long *b)
{
	if (off < 0)
		off += n;
	if (off < 0 || off > n)
		return false;
	long end = n;
	if (has_len)
		end = len < 0 ? n + len : off + len;
	if (end > n)
		end = n;
	if (end <= off)
		return false;
	*a = off;
	*b = end;
	return true;
}

/* ${inner} */
static int x_param(X *x, const char *s, size_t n, bool quoted)
{
	shctx *c = x->c;
	size_t i = 0;
	bool length = false;
	if (n >= 2 && s[0] == '#' && name_start(s[1])) {
		length = true;
		i = 1;
	}
	if (i >= n || !name_start(s[i])) {
		if (n && s[0] == '!')
			return sh_error(c, "indirect expansion ${!...} is not supported");
		if (n && strchr("0123456789@*#?$-", s[0]))
			return sh_error(c, "${%.*s} is not supported at the top level", (int)n, s);
		return sh_error(c, "bad substitution: ${%.*s}", (int)n, s);
	}
	size_t ns = i;
	while (i < n && name_char(s[i]))
		i++;
	char *name = xstrndup(s + ns, i - ns);
	char idx = 0;
	long index = 0;
	int rc = 0;
	svec vals = SVEC_INIT;
	char *pat = NULL, *rep = NULL, *tmp = NULL;
	if (i < n && s[i] == '[') {
		const char *close = memchr(s + i, ']', n - i);
		if (!close) {
			rc = sh_error(c, "bad substitution: ${%.*s}", (int)n, s);
			goto out;
		}
		size_t sl = (size_t)(close - (s + i + 1));
		const char *sub = s + i + 1;
		if (sl == 1 && (*sub == '@' || *sub == '*')) {
			idx = *sub;
		} else {
			char *num = xstrndup(sub, sl);
			bool ok = str_uint(num);
			index = ok ? atol(num) : 0;
			free(num);
			if (!ok) {
				rc = sh_error(c, "array index must be a number, @ or *: ${%.*s}", (int)n, s);
				goto out;
			}
			idx = 'n';
		}
		i = (size_t)(close - s) + 1;
	}
	const char *envval;
	const shvar *v = lookup(c->env, name, &envval);
	bool set;
	bool list = idx == '@' || idx == '*', at = idx == '@';
	if (list) {
		set = v || envval;
		if (v)
			sv_extend(&vals, &v->val);
		else if (envval)
			sv_push(&vals, envval);
	} else {
		const char *one = NULL;
		if (v && idx == 'n')
			one = (size_t)index < v->val.n ? v->val.v[index] : NULL;
		else if (v)
			one = v->val.n ? v->val.v[0] : NULL;
		else if (envval && index == 0)
			one = envval;
		set = one != NULL;
		if (one)
			sv_push(&vals, one);
	}
	if (length) {
		if (i != n) {
			rc = sh_error(c, "bad substitution: ${%.*s}", (int)n, s);
			goto out;
		}
		if (!list && !set) {
			rc = sh_error(c, "%s: unbound variable", name);
			goto out;
		}
		tmp = xasprintf("%zu", list ? vals.n : strlen(vals.v[0]));
		x_emit1(x, tmp, quoted);
		goto out;
	}
	if (i == n) {
		if (!list && !set)
			rc = sh_error(c, "%s: unbound variable", name);
		else
			x_emit(x, &vals, at, quoted);
		goto out;
	}
	const char *op = s + i;
	size_t oplen = n - i;
	bool colon = op[0] == ':' && oplen >= 2 && strchr("-=?+", op[1]);
	if (colon || strchr("-=?+", op[0])) {
		char o = colon ? op[1] : op[0];
		const char *word = op + (colon ? 2 : 1);
		size_t wl = oplen - (size_t)(colon ? 2 : 1);
		if (o == '=' || o == '?') {
			rc = sh_error(c, "${%s%s%c...} is not supported: assign explicitly instead", name, colon ? ":" : "", o);
			goto out;
		}
		bool null = !set;
		if (!null && colon) {
			char *j = sv_join(&vals, "");
			null = !list ? !*vals.v[0] : !*j;
			free(j);
		}
		if (list && !vals.n)
			null = true;
		bool use_word = o == '-' ? null : !null;
		if (!use_word) {
			if (o == '-')
				x_emit(x, &vals, at, quoted);
			else if (quoted)
				x_lit(x, "", 0, true);
			goto out;
		}
		if (quoted || x->mode != X_FIELDS) {
			rc = expand_sub(c, word, wl, x->mode == X_PATTERN ? X_PATTERN : X_SCALAR, &tmp);
			if (rc == 0) {
				if (x->mode == X_PATTERN)
					sb_adds(&x->cur, tmp), x->has = true;
				else
					x_lit(x, tmp, strlen(tmp), quoted);
			}
		} else {
			x->split++;
			rc = x_walk(x, word, wl);
			x->split--;
		}
		goto out;
	}
	if (!list && !set) {
		rc = sh_error(c, "%s: unbound variable", name);
		goto out;
	}
	if (op[0] == '#' || op[0] == '%') {
		bool longest = oplen >= 2 && op[1] == op[0];
		size_t skip = longest ? 2 : 1;
		if ((rc = expand_sub(c, op + skip, oplen - skip, X_PATTERN, &pat)) < 0)
			goto out;
		for (size_t k = 0; k < vals.n; k++) {
			char *t = trim(vals.v[k], pat, op[0], longest);
			free(vals.v[k]);
			vals.v[k] = t;
		}
		x_emit(x, &vals, at, quoted);
	} else if (op[0] == '/') {
		char mode = 0;
		size_t skip = 1;
		if (oplen >= 2 && strchr("/#%", op[1])) {
			mode = op[1];
			skip = 2;
		}
		const char *ps = op + skip;
		size_t pn = oplen - skip, k = 0;
		int nl = 0, err = 0;
		while (k < pn && ps[k] != '/' && !err) {
			if (ps[k] == '\\')
				k += 2;
			else if (ps[k] == '\'')
				k = skip_squote(ps, pn, k, &nl, &err);
			else if (ps[k] == '"')
				k = skip_dquote(ps, pn, k, &nl, &err);
			else if (ps[k] == '$')
				k = skip_dollar(ps, pn, k, &nl, &err);
			else
				k++;
		}
		if (k > pn)
			k = pn;
		if ((rc = expand_sub(c, ps, k, X_PATTERN, &pat)) < 0)
			goto out;
		if (k < pn)
			rc = expand_sub(c, ps + k + 1, pn - k - 1, X_SCALAR, &rep);
		else
			rep = xstrdup("");
		if (rc < 0)
			goto out;
		for (size_t e = 0; e < vals.n; e++) {
			char *t = replace(vals.v[e], pat, rep, mode);
			free(vals.v[e]);
			vals.v[e] = t;
		}
		x_emit(x, &vals, at, quoted);
	} else if (op[0] == '^' || op[0] == ',') {
		bool all = oplen >= 2 && op[1] == op[0];
		if (oplen != (all ? 2u : 1u)) {
			rc = sh_error(c, "case conversion with a pattern is not supported: ${%.*s}", (int)n, s);
			goto out;
		}
		for (size_t e = 0; e < vals.n; e++)
			for (char *p = vals.v[e]; *p; p++) {
				if (op[0] == '^' && *p >= 'a' && *p <= 'z')
					*p = (char)(*p - 32);
				else if (op[0] == ',' && *p >= 'A' && *p <= 'Z')
					*p = (char)(*p + 32);
				if (!all)
					break;
			}
		x_emit(x, &vals, at, quoted);
	} else if (op[0] == ':') {
		const char *colon2 = memchr(op + 1, ':', oplen - 1);
		long off, len = 0;
		size_t offlen = colon2 ? (size_t)(colon2 - op - 1) : oplen - 1;
		bool ok = parse_offset(op + 1, offlen, &off);
		if (ok && colon2)
			ok = parse_offset(colon2 + 1, oplen - 1 - offlen - 1, &len);
		if (!ok) {
			rc = sh_error(c, "substring offsets must be plain numbers: ${%.*s}", (int)n, s);
			goto out;
		}
		long a, b;
		if (list) {
			svec part = SVEC_INIT;
			if (substr_range((long)vals.n, off, colon2 != NULL, len, &a, &b))
				for (long e = a; e < b; e++)
					sv_push(&part, vals.v[e]);
			x_emit(x, &part, at, quoted);
			sv_free(&part);
		} else {
			if (substr_range((long)strlen(vals.v[0]), off, colon2 != NULL, len, &a, &b))
				tmp = xstrndup(vals.v[0] + a, (size_t)(b - a));
			else
				tmp = xstrdup("");
			x_emit1(x, tmp, quoted);
		}
	} else {
		rc = sh_error(c, "bad substitution: ${%.*s}", (int)n, s);
	}
out:
	free(name);
	free(pat);
	free(rep);
	free(tmp);
	sv_free(&vals);
	return rc;
}

/* w[i] == '$'; *next is the index after the expansion */
static int x_dollar(X *x, const char *w, size_t n, size_t i, bool quoted, size_t *next)
{
	char nx = i + 1 < n ? w[i + 1] : 0;
	int nl = 0, err = 0;
	if (nx == '{') {
		size_t j = skip_dollar(w, n, i, &nl, &err);
		if (err)
			return sh_error(x->c, "%s", scan_error(err));
		*next = j;
		return x_param(x, w + i + 2, j - i - 3, quoted);
	}
	if (name_start(nx)) {
		size_t j = i + 1;
		while (j < n && name_char(w[j]))
			j++;
		char *name = xstrndup(w + i + 1, j - i - 1);
		const char *val = sh_get(x->c->env, name);
		int rc = 0;
		if (!val)
			rc = sh_error(x->c, "%s: unbound variable", name);
		else
			x_emit1(x, val, quoted);
		free(name);
		*next = j;
		return rc;
	}
	if (nx == '\'' && !quoted) {
		size_t j = skip_dollar(w, n, i, &nl, &err);
		if (err)
			return sh_error(x->c, "%s", scan_error(err));
		sbuf b = SBUF_INIT;
		ansi_decode(&b, w + i + 2, j - i - 3);
		x_lit(x, sb_str(&b), b.len, true);
		sb_free(&b);
		*next = j;
		return 0;
	}
	if (nx == '(')
		return sh_error(x->c, "%s", scan_error(i + 2 < n && w[i + 2] == '(' ? E_ARITH : E_CMDSUB));
	if (nx && strchr("0123456789@*#?$!-", nx))
		return sh_error(x->c, "$%c is not supported at the top level", nx);
	x_lit(x, "$", 1, quoted);
	*next = i + 1;
	return 0;
}

static int x_dquote(X *x, const char *w, size_t n, size_t *pi)
{
	size_t i = *pi + 1;
	bool had = x->has, all_empty_at = true;
	int parts = 0;
	x->has = true;
	while (i < n && w[i] != '"') {
		char ch = w[i];
		parts++;
		if (ch == '\\' && i + 1 < n && strchr("$`\"\\\n", w[i + 1])) {
			if (w[i + 1] != '\n')
				x_lit(x, &w[i + 1], 1, true);
			all_empty_at = false;
			i += 2;
			continue;
		}
		if (ch == '$') {
			x->empty_at = false;
			size_t j;
			if (x_dollar(x, w, n, i, true, &j) < 0)
				return -1;
			if (!x->empty_at)
				all_empty_at = false;
			i = j;
			continue;
		}
		all_empty_at = false;
		x_lit(x, &w[i], 1, true);
		i++;
	}
	*pi = i + 1;
	if (parts && all_empty_at)
		x->has = had;
	return 0;
}

/* ~ and ~/... at the start of a word or (in assignments) after ':' */
static bool x_tilde(X *x, const char *w, size_t n, size_t i)
{
	if (!x->tilde || w[i] != '~' || (i && !(x->tilde == 2 && w[i - 1] == ':')))
		return false;
	char nx = i + 1 < n ? w[i + 1] : 0;
	if (nx && nx != '/' && !(x->tilde == 2 && nx == ':'))
		return false;
	const char *home = sh_get(x->c->env, "HOME");
	if (!home)
		home = getenv("HOME");
	if (!home)
		return false;
	x_lit(x, home, strlen(home), true);
	return true;
}

static int x_walk(X *x, const char *w, size_t n)
{
	for (size_t i = 0; i < n;) {
		char ch = w[i];
		if (ch == '~' && x_tilde(x, w, n, i)) {
			i++;
			continue;
		}
		if (ch == '\\') {
			if (i + 1 < n) {
				if (w[i + 1] != '\n')
					x_lit(x, &w[i + 1], 1, true);
				i += 2;
			} else {
				x_lit(x, "\\", 1, true);
				i++;
			}
			continue;
		}
		if (ch == '\'') {
			int nl = 0, err = 0;
			size_t j = skip_squote(w, n, i, &nl, &err);
			if (err)
				return sh_error(x->c, "%s", scan_error(err));
			x_lit(x, w + i + 1, j - i - 2, true);
			i = j;
			continue;
		}
		if (ch == '"') {
			if (x_dquote(x, w, n, &i) < 0)
				return -1;
			continue;
		}
		if (ch == '$') {
			size_t j;
			if (x_dollar(x, w, n, i, false, &j) < 0)
				return -1;
			i = j;
			continue;
		}
		if (ch == '`')
			return sh_error(x->c, "%s", scan_error(E_BACKTICK));
		if (x->split && is_ifs(ch)) {
			if (x->has)
				x_field(x);
			i++;
			continue;
		}
		x_lit(x, &w[i], 1, false);
		i++;
	}
	return 0;
}

/* ---- brace expansion: {a,b} and {x..y[..step]} in unquoted parts of array elements ---- */

static bool brace_seq(const char *s, size_t n, svec *alts)
{
	char *t = xstrndup(s, n);
	char *dots = strstr(t, "..");
	bool ok = false;
	if (dots && dots != t) {
		*dots = 0;
		char *a = t, *b = dots + 2, *st = strstr(b, "..");
		long step = 1;
		if (st) {
			*st = 0;
			char *e;
			step = strtol(st + 2, &e, 10);
			if (*e || !st[2])
				goto out;
			if (step < 0)
				step = -step;
			if (!step)
				step = 1;
		}
		char *ea, *eb;
		long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
		if (*a && *b && !*ea && !*eb) {
			int width = 0;
			const char *da = a[0] == '-' ? a + 1 : a, *db = b[0] == '-' ? b + 1 : b;
			if ((da[0] == '0' && da[1]) || (db[0] == '0' && db[1])) {
				size_t la = strlen(a), lb = strlen(b);
				width = (int)(la > lb ? la : lb);
			}
			for (long k = x; x <= y ? k <= y : k >= y; k += x <= y ? step : -step)
				sv_pushf(alts, "%0*ld", width, k);
			ok = true;
		} else if (strlen(a) == 1 && strlen(b) == 1 && name_start(*a) && *a != '_' && name_start(*b) &&
			   *b != '_') {
			for (int k = *a; *a <= *b ? k <= *b : k >= *b; k += *a <= *b ? (int)step : -(int)step)
				sv_pushf(alts, "%c", k);
			ok = true;
		}
	}
out:
	free(t);
	return ok;
}

/* w[i] == '{': a valid group ends at *close and yields alts */
static bool brace_group(const char *w, size_t n, size_t i, size_t *close, svec *alts)
{
	int depth = 0, nl = 0, err = 0;
	size_t start = i + 1, seg = start;
	bool comma = false;
	svec parts = SVEC_INIT;
	for (size_t k = start; k < n;) {
		char ch = w[k];
		if (ch == '\\') {
			k += 2;
			continue;
		}
		if (ch == '\'') {
			k = skip_squote(w, n, k, &nl, &err);
			continue;
		}
		if (ch == '"') {
			k = skip_dquote(w, n, k, &nl, &err);
			continue;
		}
		if (ch == '$' && k + 1 < n && w[k + 1] == '{') {
			k = skip_dollar(w, n, k, &nl, &err);
			continue;
		}
		if (ch == '{')
			depth++;
		else if (ch == '}' && depth)
			depth--;
		else if (ch == ',' && !depth) {
			sv_push_own(&parts, xstrndup(w + seg, k - seg));
			seg = k + 1;
			comma = true;
		} else if (ch == '}') {
			bool ok = false;
			if (comma) {
				sv_push_own(&parts, xstrndup(w + seg, k - seg));
				sv_extend(alts, &parts);
				ok = true;
			} else {
				ok = brace_seq(w + start, k - start, alts);
			}
			sv_free(&parts);
			*close = k;
			return ok;
		}
		k++;
	}
	sv_free(&parts);
	return false;
}

static void brace_expand(const char *w, svec *out)
{
	size_t n = strlen(w);
	int nl = 0, err = 0;
	for (size_t i = 0; i < n;) {
		char ch = w[i];
		if (ch == '\\') {
			i += 2;
			continue;
		}
		if (ch == '\'') {
			i = skip_squote(w, n, i, &nl, &err);
			continue;
		}
		if (ch == '"') {
			i = skip_dquote(w, n, i, &nl, &err);
			continue;
		}
		if (ch == '$' && i + 1 < n && w[i + 1] == '{') {
			i = skip_dollar(w, n, i, &nl, &err);
			continue;
		}
		if (ch == '{') {
			size_t close;
			svec alts = SVEC_INIT;
			if (brace_group(w, n, i, &close, &alts) && out->n < 100000) {
				for (size_t k = 0; k < alts.n; k++) {
					char *s = xasprintf("%.*s%s%s", (int)i, w, alts.v[k], w + close + 1);
					brace_expand(s, out);
					free(s);
				}
				sv_free(&alts);
				return;
			}
			sv_free(&alts);
		}
		i++;
	}
	sv_push(out, w);
}

static int expand_fields(shctx *c, const char *raw, svec *out)
{
	svec words = SVEC_INIT;
	brace_expand(raw, &words);
	int rc = 0;
	for (size_t k = 0; k < words.n && rc == 0; k++) {
		X x = {.c = c, .mode = X_FIELDS, .out = out, .tilde = 1};
		rc = x_walk(&x, words.v[k], strlen(words.v[k]));
		if (rc == 0 && x.glob)
			rc = sh_error(c, "unquoted '%c' in %s: the recipe language does no pathname expansion, quote it",
				      x.glob, raw);
		if (rc == 0 && x.has)
			x_field(&x);
		sb_free(&x.cur);
	}
	sv_free(&words);
	return rc;
}

int sh_expand_word(shctx *c, const char *raw, char **out)
{
	return expand_sub(c, raw, strlen(raw), X_SCALAR, out);
}

/* ======================================================================= statements */

typedef struct {
	shctx *c;
	const char *s;
	size_t n, i;
	int line;
} P;

static int perr(P *p, const char *fmt, ...) PRINTF(2, 3);
static int perr(P *p, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *m = xvasprintf(fmt, ap);
	va_end(ap);
	p->c->line = p->line;
	sh_error(p->c, "%s", m);
	free(m);
	return -1;
}

static char peek(P *p, size_t k)
{
	return p->i + k < p->n ? p->s[p->i + k] : 0;
}

static void skip_blanks(P *p)
{
	for (;;) {
		char ch = peek(p, 0);
		if (ch == ' ' || ch == '\t' || ch == '\r') {
			p->i++;
		} else if (ch == '\\' && peek(p, 1) == '\n') {
			p->i += 2;
			p->line++;
		} else {
			return;
		}
	}
}

static void skip_comment(P *p)
{
	while (p->i < p->n && p->s[p->i] != '\n')
		p->i++;
}

static int read_word(P *p, char **out)
{
	size_t i = p->i;
	int nl = 0, err = 0;
	while (i < p->n && !err) {
		char ch = p->s[i];
		if (is_meta(ch))
			break;
		if (ch == '\\') {
			if (i + 1 < p->n && p->s[i + 1] == '\n')
				nl++;
			i += 2;
		} else if (ch == '\'') {
			i = skip_squote(p->s, p->n, i, &nl, &err);
		} else if (ch == '"') {
			i = skip_dquote(p->s, p->n, i, &nl, &err);
		} else if (ch == '`') {
			err = E_BACKTICK;
		} else if (ch == '$') {
			i = skip_dollar(p->s, p->n, i, &nl, &err);
		} else {
			i++;
		}
	}
	if (i > p->n)
		i = p->n;
	if (err)
		return perr(p, "%s", scan_error(err));
	*out = xstrndup(p->s + p->i, i - p->i);
	p->i = i;
	p->line += nl;
	return 0;
}

static int scalar_assignment(P *p, const char *name, bool append)
{
	char ch = peek(p, 0);
	char *raw = NULL, *val = NULL;
	if (!ch || is_meta(ch))
		raw = xstrdup("");
	else if (read_word(p, &raw) < 0)
		return -1;
	p->c->line = p->line;
	int rc = expand_tilde(p->c, raw, strlen(raw), X_SCALAR, 2, &val);
	if (rc == 0) {
		if (append)
			sh_append(p->c->env, name, val);
		else
			sh_set(p->c->env, name, val);
	}
	free(raw);
	free(val);
	return rc;
}

static int array_assignment(P *p, const char *name, bool append)
{
	int line0 = p->line, rc = 0;
	svec vals = SVEC_INIT;
	p->i++; /* ( */
	for (;;) {
		char ch = peek(p, 0);
		if (!ch) {
			p->line = line0;
			rc = perr(p, "unterminated array %s=(...): missing ')'", name);
			break;
		}
		if (ch == ' ' || ch == '\t' || ch == '\r') {
			p->i++;
		} else if (ch == '\n') {
			p->i++;
			p->line++;
		} else if (ch == '\\' && peek(p, 1) == '\n') {
			p->i += 2;
			p->line++;
		} else if (ch == '#') {
			skip_comment(p);
		} else if (ch == ')') {
			p->i++;
			break;
		} else if (is_meta(ch)) {
			rc = perr(p, "unexpected '%c' inside the array %s=(...)", ch, name);
			break;
		} else {
			char *w;
			if ((rc = read_word(p, &w)) < 0)
				break;
			p->c->line = p->line;
			if (w[0] == '[' && strstr(w, "]="))
				rc = perr(p, "indexed array elements ([n]=value) are not supported");
			else
				rc = expand_fields(p->c, w, &vals);
			free(w);
			if (rc < 0)
				break;
		}
	}
	if (rc == 0) {
		if (append)
			sh_append_list(p->c->env, name, &vals);
		else
			sh_set_list(p->c->env, name, &vals);
	}
	sv_free(&vals);
	return rc;
}

/* NAME=..., NAME+=..., NAME=(...): 1 when one was parsed, 0 when the word is something else */
static int try_assignment(P *p, bool export)
{
	size_t k = p->i;
	if (!name_start(peek(p, 0)))
		return 0;
	while (k < p->n && name_char(p->s[k]))
		k++;
	size_t name_end = k;
	bool append = false;
	if (k + 1 < p->n && p->s[k] == '+' && p->s[k + 1] == '=') {
		append = true;
		k++;
	}
	if (k >= p->n || p->s[k] != '=')
		return 0;
	char *name = xstrndup(p->s + p->i, name_end - p->i);
	p->i = k + 1;
	int rc = peek(p, 0) == '(' ? array_assignment(p, name, append) : scalar_assignment(p, name, append);
	if (rc == 0 && export)
		sh_export(p->c->env, name);
	free(name);
	return rc < 0 ? -1 : 1;
}

static int function_def(P *p, const char *name)
{
	int line0 = p->line;
	for (;;) {
		char ch = peek(p, 0);
		if (ch == ' ' || ch == '\t' || ch == '\r') {
			p->i++;
		} else if (ch == '\n') {
			p->i++;
			p->line++;
		} else {
			break;
		}
	}
	if (peek(p, 0) != '{')
		return perr(p, "function %s: the body must be { ... }", name);
	p->i++;
	if (sv_index(&p->c->funcs, name) < 0)
		sv_push(&p->c->funcs, name);
	/* one line: name() { ...; } */
	size_t eol = p->i;
	while (eol < p->n && p->s[eol] != '\n')
		eol++;
	size_t last = eol;
	while (last > p->i && strchr(" \t\r", p->s[last - 1]))
		last--;
	if (last > p->i && p->s[last - 1] == '}') {
		p->i = eol;
		return 0;
	}
	/* otherwise the body ends at a line that starts with } */
	p->i = eol;
	while (p->i < p->n) {
		p->i++; /* \n */
		p->line++;
		if (peek(p, 0) == '}') {
			p->i++;
			skip_blanks(p);
			char ch = peek(p, 0);
			if (ch && ch != '\n' && ch != '#' && ch != ';')
				return perr(p, "function %s: unexpected text after the closing '}'", name);
			skip_comment(p);
			return 0;
		}
		while (p->i < p->n && p->s[p->i] != '\n')
			p->i++;
	}
	p->line = line0;
	return perr(p, "function %s: no closing '}' at the start of a line", name);
}

static const char *const reserved[] = {"if",     "then", "else",   "elif", "fi", "for", "while", "until",
				       "do",     "done", "case",   "esac", "select", "time", "[[", "]]",
				       "{",      "}",    "!",      "function", "((", "declare", "local",
				       "readonly", "typeset", "source", ".", "eval", "exec", NULL};

static int command(P *p, svec *raw, int line)
{
	svec argv = SVEC_INIT;
	int rc = 0;
	p->c->line = line;
	for (size_t k = 0; k < raw->n && rc == 0; k++)
		rc = expand_fields(p->c, raw->v[k], &argv);
	if (rc < 0 || !argv.n)
		goto out;
	const char *cmd = argv.v[0];
	if (!strcmp(cmd, ":") || !strcmp(cmd, "true"))
		goto out;
	if (!strcmp(cmd, "unset")) {
		bool funcs = false;
		for (size_t k = 1; k < argv.n; k++) {
			if (!strcmp(argv.v[k], "-v"))
				continue;
			if (!strcmp(argv.v[k], "-f")) {
				funcs = true;
				continue;
			}
			if (!valid_name(argv.v[k])) {
				rc = perr(p, "unset: '%s' is not a variable name", argv.v[k]);
				goto out;
			}
			if (funcs) {
				ssize_t f = sv_index(&p->c->funcs, argv.v[k]);
				if (f >= 0)
					sv_remove(&p->c->funcs, (size_t)f);
			} else {
				sh_unset(p->c->env, argv.v[k]);
			}
		}
		goto out;
	}
	int r = p->c->command ? p->c->command(p->c, &argv) : 1;
	if (r < 0)
		rc = -1;
	else if (r > 0)
		rc = perr(p, "'%s' is not allowed at the top level: put commands into a function", cmd);
out:
	sv_free(&argv);
	return rc;
}

static int export_statement(P *p)
{
	for (;;) {
		skip_blanks(p);
		char ch = peek(p, 0);
		if (!ch || ch == '\n' || ch == ';' || ch == '#')
			return 0;
		if (is_meta(ch))
			return perr(p, "unexpected '%c' after export", ch);
		int a = try_assignment(p, true);
		if (a < 0)
			return -1;
		if (a > 0)
			continue;
		char *w, *name;
		if (read_word(p, &w) < 0)
			return -1;
		p->c->line = p->line;
		int rc = expand_sub(p->c, w, strlen(w), X_SCALAR, &name);
		if (rc == 0 && strcmp(name, "-n") && strcmp(name, "-p"))
			sh_export(p->c->env, name);
		free(w);
		free(name);
		if (rc < 0)
			return -1;
	}
}

static int statement(P *p)
{
	/* name() { ... } */
	if (name_start(peek(p, 0))) {
		size_t k = p->i;
		while (k < p->n && name_char(p->s[k]))
			k++;
		size_t m = k;
		while (m < p->n && (p->s[m] == ' ' || p->s[m] == '\t'))
			m++;
		if (m < p->n && p->s[m] == '(') {
			size_t q = m + 1;
			while (q < p->n && (p->s[q] == ' ' || p->s[q] == '\t'))
				q++;
			if (q < p->n && p->s[q] == ')') {
				char *name = xstrndup(p->s + p->i, k - p->i);
				p->i = q + 1;
				int rc = function_def(p, name);
				free(name);
				return rc;
			}
		}
	}
	svec words = SVEC_INIT;
	int nassign = 0, rc = 0, line = p->line;
	for (;;) {
		skip_blanks(p);
		char ch = peek(p, 0);
		if (!ch || ch == '\n' || ch == ';')
			break;
		if (ch == '#') {
			skip_comment(p);
			break;
		}
		if (is_meta(ch)) {
			rc = perr(p, "'%c' is not allowed at the top level: put logic into a function", ch);
			break;
		}
		if (!words.n) {
			int a = try_assignment(p, false);
			if (a < 0) {
				rc = -1;
				break;
			}
			if (a > 0) {
				nassign++;
				continue;
			}
		}
		char *w;
		if ((rc = read_word(p, &w)) < 0)
			break;
		if (!words.n) {
			for (const char *const *r = reserved; *r; r++)
				if (!strcmp(w, *r)) {
					rc = perr(p, "'%s' is not allowed at the top level: put logic into a function", w);
					free(w);
					goto out;
				}
			if (!strcmp(w, "export") && !nassign) {
				free(w);
				rc = export_statement(p);
				goto out;
			}
		}
		sv_push_own(&words, w);
	}
	if (rc == 0 && words.n) {
		if (nassign)
			rc = perr(p, "assignments in front of a command are not allowed at the top level");
		else
			rc = command(p, &words, line);
	}
out:
	sv_free(&words);
	return rc;
}

int sh_eval(shctx *c, const char *src, size_t len, const char *file)
{
	const char *ofile = c->file;
	int oline = c->line;
	P p = {c, src, len, 0, 1};
	int rc = 0;
	c->file = file;
	while (rc == 0 && p.i < p.n) {
		char ch = p.s[p.i];
		if (ch == ' ' || ch == '\t' || ch == '\r' || ch == ';') {
			p.i++;
		} else if (ch == '\n') {
			p.i++;
			p.line++;
		} else if (ch == '\\' && peek(&p, 1) == '\n') {
			p.i += 2;
			p.line++;
		} else if (ch == '#') {
			skip_comment(&p);
		} else {
			rc = statement(&p);
		}
	}
	c->file = ofile;
	c->line = oline;
	return rc;
}

int sh_eval_file(shctx *c, const char *path)
{
	size_t len;
	char *src = file_read(path, &len);
	if (!src) {
		c->line = 0;
		const char *ofile = c->file;
		c->file = NULL;
		sh_error(c, "%s: cannot read", path);
		c->file = ofile;
		return -1;
	}
	if (memchr(src, 0, len)) {
		free(src);
		const char *ofile = c->file;
		c->file = NULL;
		sh_error(c, "%s: binary file", path);
		c->file = ofile;
		return -1;
	}
	int rc = sh_eval(c, src, len, path);
	free(src);
	return rc;
}
