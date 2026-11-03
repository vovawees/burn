/* SPDX-License-Identifier: MIT */
/* burn - the CleanLinux package manager */
#include <stdio.h>
#include <string.h>

#include "util.h"
#include "vercmp.h"

static void usage(FILE *f)
{
	fputs("burn " BURN_VERSION " - the CleanLinux package manager (source based, rolling+++)\n"
	      "\n"
	      "Usage: burn [options] <command> [arguments]\n"
	      "\n"
	      "Commands:\n"
	      "  vercmp <a> <b>              compare two versions: prints -1, 0 or 1\n"
	      "\n"
	      "Options:\n"
	      "  -h, --help                  show this help\n"
	      "  -V, --version               show the version\n",
	      f);
}

int main(int argc, char **argv)
{
	log_init();
	int i = 1;
	for (; i < argc && argv[i][0] == '-'; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(stdout);
			return 0;
		}
		if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
			puts("burn " BURN_VERSION);
			return 0;
		}
		if (!strcmp(a, "--")) {
			i++;
			break;
		}
		die("unknown option: %s (see: burn --help)", a);
	}
	if (i >= argc) {
		usage(stderr);
		return 2;
	}
	const char *cmd = argv[i++];
	if (!strcmp(cmd, "vercmp")) {
		if (argc - i != 2)
			die("usage: burn vercmp <a> <b>");
		printf("%d\n", vercmp(argv[i], argv[i + 1]));
		return 0;
	}
	if (!strcmp(cmd, "help")) {
		usage(stdout);
		return 0;
	}
	die("unknown command: %s (see: burn --help)", cmd);
}
