/* SPDX-License-Identifier: MIT */
/* The installed-package database: $ROOT/var/lib/burn/installed/<name>/{meta,files,...}. */
#include "burn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool db_has(const char *name)
{
	char *p = xasprintf("%s/%s/meta", B.db_dir, name);
	bool ok = is_file(p);
	free(p);
	return ok;
}

/* the first KEY=value line of the package's meta */
char *db_get(const char *name, const char *key)
{
	char *p = xasprintf("%s/%s/meta", B.db_dir, name);
	svec lines = SVEC_INIT;
	file_lines(p, &lines);
	char *val = NULL;
	size_t k = strlen(key);
	for (size_t i = 0; i < lines.n && !val; i++)
		if (!strncmp(lines.v[i], key, k) && lines.v[i][k] == '=')
			val = xstrdup(lines.v[i] + k + 1);
	sv_free(&lines);
	free(p);
	return val;
}

bool is_held(const char *name)
{
	svec lines = SVEC_INIT;
	file_lines(B.hold_file, &lines);
	bool held = sv_has(&lines, name);
	sv_free(&lines);
	return held;
}
