/* SPDX-License-Identifier: MIT */
/* ELF inside the process: what a binary provides and needs (SONAME, NEEDED) and whether a static
 * library holds LTO-only objects. 32 and 64 bit, either byte order; damaged files are no ELF. */
#ifndef BURN_ELF_H
#define BURN_ELF_H

#include "util.h"

bool is_elf(const char *path); /* starts with \x7fELF */
/* appends the SONAME and NEEDED entries of the dynamic section; -1 when PATH is no readable ELF */
int elf_dynamic(const char *path, svec *soname, svec *needed);
/* an ar archive (or an ELF object) with .gnu.lto_ sections: LTO bytecode without real code */
bool elf_has_lto(const char *path);

#endif
