/* SPDX-License-Identifier: MIT */
/* The rollback point of an update: a read-only btrfs snapshot of the root in
 * $ROOT/.snapshots/burn-<UTC time>, made with the btrfs ioctls (no btrfs-progs). The newest
 * SNAPSHOTS (burn.conf; default 5, 0: no snapshots before updates) burn-* snapshots stay. */
#include "burn.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/btrfs.h>
#include <linux/magic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

#define SUBVOL_INO 256 /* BTRFS_FIRST_FREE_OBJECTID: the inode of every subvolume root */
#define SNAP_DIR ".snapshots"

static long snapshots_keep(void)
{
	const char *v = cfg("SNAPSHOTS");
	if (!*v)
		return 5;
	if (!str_uint(v))
		die("SNAPSHOTS=%s: expected how many snapshots to keep (0: none)", v);
	return atol(v);
}

/* an open ROOT when it is a btrfs subvolume; -1 and the reason otherwise */
static int root_subvol(const char **why)
{
	int fd = open(B.root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		die_errno("cannot open %s", B.root);
	struct statfs sf;
	struct stat st;
	if (fstatfs(fd, &sf) < 0 || fstat(fd, &st) < 0)
		die_errno("cannot stat %s", B.root);
	*why = (uint32_t)sf.f_type != (uint32_t)BTRFS_SUPER_MAGIC ? "not on btrfs"
	       : st.st_ino != SUBVOL_INO			   ? "not a btrfs subvolume"
								   : NULL;
	if (*why) {
		close(fd);
		return -1;
	}
	return fd;
}

static int snap_create(int dir, int src, const char *name)
{
	struct btrfs_ioctl_vol_args_v2 a;
	memset(&a, 0, sizeof a);
	a.fd = src;
	a.flags = BTRFS_SUBVOL_RDONLY;
	snprintf(a.name, sizeof a.name, "%s", name);
	return ioctl(dir, BTRFS_IOC_SNAP_CREATE_V2, &a);
}

static int snap_delete(int dir, const char *name)
{
#ifdef BTRFS_IOC_SNAP_DESTROY_V2
	struct btrfs_ioctl_vol_args_v2 a2;
	memset(&a2, 0, sizeof a2);
	snprintf(a2.name, sizeof a2.name, "%s", name);
	if (ioctl(dir, BTRFS_IOC_SNAP_DESTROY_V2, &a2) == 0)
		return 0;
	if (errno != ENOTTY && errno != EOPNOTSUPP)
		return -1;
#endif
	/* kernels before 5.7 */
	struct btrfs_ioctl_vol_args a;
	memset(&a, 0, sizeof a);
	snprintf(a.name, sizeof a.name, "%s", name);
	return ioctl(dir, BTRFS_IOC_SNAP_DESTROY, &a);
}

static int by_version(const void *a, const void *b)
{
	return strverscmp(*(char *const *)a, *(char *const *)b);
}

/* the burn-* subvolumes in DIR, oldest first (burn-<time>, then burn-<time>.2, ...) */
static void snap_list(int dir, svec *out)
{
	/* opened again: btrfs readdir does not show what was added after the directory was opened */
	int fd = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	DIR *d = fd < 0 ? NULL : fdopendir(fd);
	if (!d)
		die_errno("cannot read %s/" SNAP_DIR, B.R);
	struct dirent *e;
	struct stat st;
	while ((e = readdir(d)))
		if (str_prefix(e->d_name, "burn-") && fstatat(dir, e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
		    S_ISDIR(st.st_mode) && st.st_ino == SUBVOL_INO)
			sv_push(out, e->d_name);
	closedir(d);
	qsort(out->v, out->n, sizeof *out->v, by_version);
}

/* update: the snapshot before anything changes, when ROOT is a btrfs subvolume (it dies when
 * that fails: nothing was changed yet). burn snapshot: required, whatever SNAPSHOTS says. */
void snapshot_take(bool required)
{
	long keep = snapshots_keep();
	if (!keep && !required)
		return;
	const char *why;
	int src = root_subvol(&why);
	if (src < 0) {
		if (required)
			die("no snapshot: %s is %s", B.root, why);
		if (strcmp(why, "not on btrfs"))
			msg2("no snapshot: %s is %s", B.root, why);
		return;
	}
	if (mkdirat(src, SNAP_DIR, 0755) < 0 && errno != EEXIST)
		die_errno("cannot create %s/" SNAP_DIR, B.R);
	int dir = openat(src, SNAP_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dir < 0)
		die_errno("cannot open %s/" SNAP_DIR, B.R);
	char stamp[32], name[48];
	time_t t = time(NULL);
	struct tm tm;
	strftime(stamp, sizeof stamp, "burn-%Y%m%dT%H%M%SZ", gmtime_r(&t, &tm));
	snprintf(name, sizeof name, "%s", stamp);
	int rc;
	/* several snapshots within a second: burn-<time>.2, .3, ... */
	for (int i = 2; (rc = snap_create(dir, src, name)) < 0 && errno == EEXIST && i < 100; i++)
		snprintf(name, sizeof name, "%s.%d", stamp, i);
	if (rc < 0)
		die_errno("cannot snapshot %s into %s/" SNAP_DIR "/%s; nothing was changed", B.root, B.R, name);
	msg2("snapshot: %s/" SNAP_DIR "/%s", B.R, name);
	if (keep) {
		svec all = SVEC_INIT;
		snap_list(dir, &all);
		for (size_t i = 0; i + (size_t)keep < all.n; i++) {
			if (snap_delete(dir, all.v[i]) < 0)
				warn("cannot delete the old snapshot %s/" SNAP_DIR "/%s: %s", B.R, all.v[i], strerror(errno));
			else
				msg2("old snapshot deleted: %s/" SNAP_DIR "/%s", B.R, all.v[i]);
		}
		sv_free(&all);
	}
	close(dir);
	close(src);
}

int cmd_snapshot(int argc, char **argv)
{
	(void)argv;
	if (argc)
		die("usage: burn snapshot");
	need_write();
	snapshot_take(true);
	return 0;
}
