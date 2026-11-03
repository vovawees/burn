/* SPDX-License-Identifier: MIT */
/* Version comparison: the same order as `sort -V` (GNU filevercmp, Debian rules). */
#ifndef BURN_VERCMP_H
#define BURN_VERCMP_H

int filevercmp(const char *a, const char *b);
/* -1, 0 or 1; different strings never compare equal (byte order breaks ties, like sort). */
int vercmp(const char *a, const char *b);

#endif
