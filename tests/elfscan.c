/* SPDX-License-Identifier: MIT */
/* tests/elfscan FILE...: "FILE soname=... needed=... lto=yes|no", or "FILE not-elf lto=..." when
 * FILE does not start like ELF; damaged ELF says nothing, like readelf */
#include "../src/elf.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		const char *lto = elf_has_lto(argv[i]) ? "yes" : "no";
		if (!is_elf(argv[i])) {
			printf("%s not-elf lto=%s\n", argv[i], lto);
			continue;
		}
		svec so = SVEC_INIT, ne = SVEC_INIT;
		elf_dynamic(argv[i], &so, &ne);
		char *a = sv_join(&so, ","), *b = sv_join(&ne, ",");
		printf("%s soname=%s needed=%s lto=%s\n", argv[i], a, b, lto);
		free(a);
		free(b);
		sv_free(&so);
		sv_free(&ne);
	}
	return 0;
}
