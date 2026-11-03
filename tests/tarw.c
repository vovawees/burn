/* SPDX-License-Identifier: MIT */
/* tests/tarw DIR OUT EPOCH: DIR packed like burn packs packages */
#include "../src/tar.h"
#include "../src/util.h"

#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc != 4)
		die("usage: tarw DIR OUT EPOCH");
	char *err = NULL;
	if (tar_write_dir(argv[1], argv[2], atoll(argv[3]), 10, 0, &err) < 0)
		die("%s", err);
	return 0;
}
