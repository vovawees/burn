/* SPDX-License-Identifier: MIT */
#include "elf.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
	const uint8_t *p;
	size_t size;
	bool is64, be;
} elf;

static uint64_t rd(const elf *e, size_t off, size_t n)
{
	uint64_t v = 0;
	for (size_t i = 0; i < n; i++)
		v |= (uint64_t)e->p[off + i] << (8 * (e->be ? n - 1 - i : i));
	return v;
}

static bool in(const elf *e, uint64_t off, uint64_t len)
{
	return off <= e->size && len <= e->size - off;
}

static bool elf_open(elf *e, const uint8_t *p, size_t size)
{
	if (size < 52 || memcmp(p, "\x7f" "ELF", 4) != 0)
		return false;
	if ((p[4] != 1 && p[4] != 2) || (p[5] != 1 && p[5] != 2))
		return false;
	*e = (elf){p, size, p[4] == 2, p[5] == 2};
	return !e->is64 || size >= 64;
}

/* ---- the section headers ---- */

typedef struct {
	uint64_t name, type, offset, size, link, entsize;
} shdr;

static bool sections(const elf *e, uint64_t *off, uint64_t *num, uint64_t *entsz, uint64_t *strndx)
{
	*off = e->is64 ? rd(e, 40, 8) : rd(e, 32, 4);
	*entsz = rd(e, e->is64 ? 58 : 46, 2);
	*num = rd(e, e->is64 ? 60 : 48, 2);
	*strndx = rd(e, e->is64 ? 62 : 50, 2);
	if (!*off || *entsz < (e->is64 ? 64u : 40u) || !in(e, *off, *entsz))
		return false;
	if (*num == 0) /* more than 0xff00 sections: the count is in section 0 */
		*num = e->is64 ? rd(e, *off + 32, 8) : rd(e, *off + 20, 4);
	if (*strndx == 0xffff)
		*strndx = rd(e, *off + (e->is64 ? 40 : 24), 4);
	return *num < (1u << 24) && in(e, *off, *num * *entsz);
}

static shdr section(const elf *e, uint64_t off, uint64_t entsz, uint64_t i)
{
	uint64_t b = off + i * entsz;
	shdr s;
	s.name = rd(e, b, 4);
	s.type = rd(e, b + 4, 4);
	if (e->is64) {
		s.offset = rd(e, b + 24, 8);
		s.size = rd(e, b + 32, 8);
		s.link = rd(e, b + 40, 4);
		s.entsize = rd(e, b + 56, 8);
	} else {
		s.offset = rd(e, b + 16, 4);
		s.size = rd(e, b + 20, 4);
		s.link = rd(e, b + 24, 4);
		s.entsize = rd(e, b + 36, 4);
	}
	return s;
}

static const char *str_at(const elf *e, uint64_t tab, uint64_t tabsize, uint64_t i)
{
	if (i >= tabsize || !in(e, tab, tabsize))
		return NULL;
	const char *s = (const char *)e->p + tab + i;
	return memchr(s, 0, tabsize - i) ? s : NULL;
}

/* ---- the dynamic section ---- */

enum { SHT_DYNAMIC_ = 6, PT_LOAD_ = 1, PT_DYNAMIC_ = 2, DT_NEEDED_ = 1, DT_STRTAB_ = 5, DT_STRSZ_ = 10, DT_SONAME_ = 14 };

/* file offset of the virtual address VA through the PT_LOAD segments */
static bool va_to_off(const elf *e, uint64_t va, uint64_t *off)
{
	uint64_t phoff = e->is64 ? rd(e, 32, 8) : rd(e, 28, 4);
	uint64_t entsz = rd(e, e->is64 ? 54 : 42, 2), num = rd(e, e->is64 ? 56 : 44, 2);
	if (!phoff || !in(e, phoff, num * entsz) || entsz < (e->is64 ? 56u : 32u))
		return false;
	for (uint64_t i = 0; i < num; i++) {
		uint64_t b = phoff + i * entsz;
		if (rd(e, b, 4) != PT_LOAD_)
			continue;
		uint64_t o = e->is64 ? rd(e, b + 8, 8) : rd(e, b + 4, 4);
		uint64_t v = e->is64 ? rd(e, b + 16, 8) : rd(e, b + 8, 4);
		uint64_t fs = e->is64 ? rd(e, b + 32, 8) : rd(e, b + 16, 4);
		if (va >= v && va - v < fs) {
			*off = o + (va - v);
			return true;
		}
	}
	return false;
}

static bool dynamic_from_phdrs(const elf *e, uint64_t *off, uint64_t *size)
{
	uint64_t phoff = e->is64 ? rd(e, 32, 8) : rd(e, 28, 4);
	uint64_t entsz = rd(e, e->is64 ? 54 : 42, 2), num = rd(e, e->is64 ? 56 : 44, 2);
	if (!phoff || entsz < (e->is64 ? 56u : 32u) || !in(e, phoff, num * entsz))
		return false;
	for (uint64_t i = 0; i < num; i++) {
		uint64_t b = phoff + i * entsz;
		if (rd(e, b, 4) == PT_DYNAMIC_) {
			*off = e->is64 ? rd(e, b + 8, 8) : rd(e, b + 4, 4);
			*size = e->is64 ? rd(e, b + 32, 8) : rd(e, b + 16, 4);
			return in(e, *off, *size);
		}
	}
	return false;
}

static int dynamic(const elf *e, svec *soname, svec *needed)
{
	uint64_t dyn = 0, dynsz = 0, str = 0, strsz = 0, shoff, shnum, shent, shstr;
	bool have = false;
	/* the section headers name the string table; without them, DT_STRTAB through the segments */
	if (sections(e, &shoff, &shnum, &shent, &shstr)) {
		for (uint64_t i = 0; i < shnum; i++) {
			shdr s = section(e, shoff, shent, i);
			if (s.type != SHT_DYNAMIC_ || !in(e, s.offset, s.size) || s.link >= shnum)
				continue;
			shdr t = section(e, shoff, shent, s.link);
			dyn = s.offset, dynsz = s.size, str = t.offset, strsz = t.size;
			have = in(e, str, strsz);
			break;
		}
	}
	if (!have) {
		if (!dynamic_from_phdrs(e, &dyn, &dynsz))
			return 0; /* static, or an object file: nothing to say */
		size_t ent = e->is64 ? 16 : 8;
		for (uint64_t o = dyn; o + ent <= dyn + dynsz; o += ent) {
			uint64_t tag = rd(e, o, ent / 2), val = rd(e, o + ent / 2, ent / 2);
			if (tag == 0)
				break;
			if (tag == DT_STRTAB_ && !va_to_off(e, val, &str))
				return -1;
			if (tag == DT_STRSZ_)
				strsz = val;
		}
		if (!str || !in(e, str, strsz))
			return -1;
	}
	size_t ent = e->is64 ? 16 : 8;
	for (uint64_t o = dyn; o + ent <= dyn + dynsz; o += ent) {
		uint64_t tag = rd(e, o, ent / 2), val = rd(e, o + ent / 2, ent / 2);
		if (tag == 0)
			break;
		if (tag != DT_NEEDED_ && tag != DT_SONAME_)
			continue;
		const char *s = str_at(e, str, strsz, val);
		if (!s)
			return -1;
		sv_push(tag == DT_SONAME_ ? soname : needed, s);
	}
	return 0;
}

/* ---- files ---- */

static const uint8_t *map(const char *path, size_t *size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return NULL;
	struct stat st;
	void *p = MAP_FAILED;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
		p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (p == MAP_FAILED)
		return NULL;
	*size = (size_t)st.st_size;
	return p;
}

bool is_elf(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return false;
	char b[4];
	bool r = read(fd, b, 4) == 4 && !memcmp(b, "\x7f" "ELF", 4);
	close(fd);
	return r;
}

int elf_dynamic(const char *path, svec *soname, svec *needed)
{
	size_t size;
	const uint8_t *p = map(path, &size);
	if (!p)
		return -1;
	elf e;
	svec so = SVEC_INIT, ne = SVEC_INIT;
	int r = elf_open(&e, p, size) ? dynamic(&e, &so, &ne) : -1;
	if (r == 0) {
		sv_extend(soname, &so);
		sv_extend(needed, &ne);
	}
	sv_free(&so);
	sv_free(&ne);
	munmap((void *)p, size);
	return r;
}

static bool object_has_lto(const uint8_t *p, size_t size)
{
	elf e;
	uint64_t shoff, shnum, shent, shstr;
	if (!elf_open(&e, p, size) || !sections(&e, &shoff, &shnum, &shent, &shstr) || shstr >= shnum)
		return false;
	shdr names = section(&e, shoff, shent, shstr);
	for (uint64_t i = 0; i < shnum; i++) {
		const char *n = str_at(&e, names.offset, names.size, section(&e, shoff, shent, i).name);
		if (n && strstr(n, ".gnu.lto_"))
			return true;
	}
	return false;
}

bool elf_has_lto(const char *path)
{
	size_t size;
	const uint8_t *p = map(path, &size);
	if (!p)
		return false;
	bool lto = false;
	if (size >= 8 && !memcmp(p, "!<arch>\n", 8)) {
		size_t off = 8;
		while (!lto && off + 60 <= size) {
			const char *h = (const char *)p + off;
			char num[11];
			memcpy(num, h + 48, 10);
			num[10] = 0;
			char *end;
			unsigned long long len = strtoull(num, &end, 10);
			if (end == num || memcmp(h + 58, "`\n", 2) != 0 || len > size - off - 60)
				break;
			size_t data = off + 60, skip = 0;
			if (!memcmp(h, "#1/", 3)) /* BSD: the name is in front of the data */
				skip = (size_t)strtoul(h + 3, NULL, 10);
			if (h[0] != '/' || (h[1] != ' ' && h[1] != '/' && h[1] != 'S'))
				if (skip <= len)
					lto = object_has_lto(p + data + skip, (size_t)len - skip);
			off = data + (size_t)len + (len & 1);
		}
	} else {
		lto = object_has_lto(p, size);
	}
	munmap((void *)p, size);
	return lto;
}
