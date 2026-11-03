/* SPDX-License-Identifier: MIT */
/* tests/tarls FILE.tar.zst: every entry as "type mode uid gid size mtime sha256 path -> link" */
#include "../src/sha256.h"
#include "../src/tar.h"
#include "../src/util.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc != 2)
		die("usage: tarls FILE");
	char *err = NULL;
	tar_reader *t = tar_open(argv[1], &err);
	if (!t)
		die("%s", err);
	const tar_entry *e;
	int r;
	while ((r = tar_next(t, &e, &err)) > 0) {
		char hex[65] = "-";
		if (e->type == TAR_FILE) {
			sha256_ctx c;
			sha256_init(&c);
			char buf[4096];
			ssize_t n;
			while ((n = tar_read(t, buf, sizeof buf, &err)) > 0)
				sha256_update(&c, buf, (size_t)n);
			if (n < 0)
				die("%s", err);
			sha256_final_hex(&c, hex);
		}
		printf("%c %04o %u %u %llu %lld %s %s -> %s\n", e->type, (unsigned)e->mode, (unsigned)e->uid,
		       (unsigned)e->gid, (unsigned long long)e->size, (long long)e->mtime, hex, e->path, e->link);
	}
	if (r < 0)
		die("%s", err);
	tar_close(t);
	return 0;
}
