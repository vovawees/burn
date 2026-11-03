/* SPDX-License-Identifier: MIT */
/*
 * Reading .tar.zst archives inside the process: zstd decompression and the tar formats GNU tar
 * writes (ustar, GNU long names, pax headers). Paths come out normalized and safe: relative,
 * without "." and ".." components.
 */
#ifndef BURN_TAR_H
#define BURN_TAR_H

#include <stdint.h>
#include <sys/types.h>

typedef struct zst_reader zst_reader;
zst_reader *zst_open(const char *path, char **err);               /* NULL and *err */
ssize_t zst_read(zst_reader *z, void *buf, size_t n, char **err); /* 0 at the end, -1 and *err */
void zst_close(zst_reader *z);

enum {
	TAR_FILE = '0',
	TAR_HARDLINK = '1',
	TAR_SYMLINK = '2',
	TAR_CHAR = '3',
	TAR_BLOCK = '4',
	TAR_DIR = '5',
	TAR_FIFO = '6',
};

typedef struct {
	char *path; /* "usr/bin/x"; "" is the top directory itself */
	char *link; /* symlink: the target as stored; hard link: the normalized path it repeats */
	int type;   /* TAR_* */
	mode_t mode; /* permission bits with setuid, setgid and sticky */
	uid_t uid;
	gid_t gid;
	int64_t mtime;
	uint64_t size; /* data of a regular file */
	unsigned major, minor;
} tar_entry;

typedef struct tar_reader tar_reader;
tar_reader *tar_open(const char *path, char **err); /* a zstd compressed tar */
/* the next entry: 1; the end of the archive: 0; an error: -1 and *err. *e stays valid until the next call. */
int tar_next(tar_reader *t, const tar_entry **e, char **err);
/* data of the current entry: bytes read, 0 at its end, -1 and *err */
ssize_t tar_read(tar_reader *t, void *buf, size_t n, char **err);
void tar_close(tar_reader *t);

/* "./a//b/" -> "a/b"; NULL for ".." components */
char *tar_normalize(const char *p);

#endif
