/* SPDX-License-Identifier: MIT */
/*
 * File operations inside the root ($ROOT). Every path is relative to the root, and every symlink
 * met on the way is resolved inside it: openat2(RESOLVE_IN_ROOT), or the same walk in userspace
 * when the kernel has no openat2. A package or a broken root can never touch files outside.
 */
#ifndef BURN_ROOT_H
#define BURN_ROOT_H

#include <stdbool.h>
#include <sys/stat.h>

void root_init(void); /* opens $ROOT, creating it when missing; dies on errors */
/* a directory as an O_PATH descriptor, cached: never close it. -1 and errno on errors. */
int root_dir(const char *dir);
int root_mkdirs(const char *dir, mode_t mode); /* mkdir -p inside the root; the fd like root_dir */
void root_flush(void);                        /* forget cached directories (after removing some) */
/* the parent directory of PATH (root_dir) and its last component */
int root_parent(const char *path, const char **name);
int root_open(const char *path, int flags, mode_t mode); /* follows symlinks inside the root */
bool root_lstat(const char *path, struct stat *st);
bool root_stat(const char *path, struct stat *st); /* following symlinks inside the root */
char *root_readlink(const char *path);             /* NULL when it is no symlink */
int root_unlink(const char *path);                 /* files and symlinks */
int root_rmdir(const char *path);
int root_rename(const char *from, const char *to);
bool root_same_content(const char *a_path, int fd_b); /* cmp -s */

#endif
