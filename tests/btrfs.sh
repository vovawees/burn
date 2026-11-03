#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/btrfs.sh BURN [DIR]: snapshots on a real btrfs, as root. DIR is the top of a fresh,
# mounted btrfs; without it the test makes one on a loop device (root or sudo, btrfs-progs).
# tests/qemu-btrfs.sh runs this inside a small virtual machine when the kernel has no btrfs.
set -eu
if [ $# = 1 ]; then
	s=
	[ "$(id -u)" = 0 ] || s=sudo
	img=$(mktemp) mnt=$(mktemp -d)
	truncate -s 256M "$img"
	mkfs.btrfs -q -f "$img" >/dev/null
	if ! $s mount -o loop "$img" "$mnt"; then
		rm -f "$img"
		rmdir "$mnt"
		echo "btrfs: no btrfs here; in qemu: make check-btrfs KERNEL=bzImage (tests/qemu-btrfs.sh)"
		exit 1
	fi
	rc=0
	$s sh "$0" "$(realpath "$1")" "$mnt" || rc=$?
	$s umount "$mnt"
	rm -f "$img"
	rmdir "$mnt"
	exit "$rc"
fi
burn=$1 dir=$2
tmp=${TMPDIR:-/tmp}/burn-btrfs.$$
fail() {
	echo "btrfs: FAIL: $*"
	exit 1
}
rm -rf "$tmp"
mkdir -p "$tmp/profiles" "$tmp/r1/snaptest" "$tmp/r2/snaptest"
: >"$tmp/profiles/native.profile"
BURN_PROFILE_DIR=$tmp/profiles BURN_CACHE=$tmp/cache BURN_WORK=$tmp/work
export BURN_PROFILE_DIR BURN_CACHE BURN_WORK
echo SNAPSHOTS=3 >"$tmp/keep3.conf"
echo SNAPSHOTS=0 >"$tmp/keep0.conf"
b() { "$burn" -r "$dir" "$@"; }
count() {
	n=0
	for s in "$dir"/.snapshots/burn-*; do
		[ -e "$s" ] && n=$((n + 1))
	done
	echo "$n"
}

# four snapshots, three stay: the newest ones, read-only, each with the root of its time
for i in 1 2 3 4; do
	echo "$i" >"$dir/marker"
	b -c "$tmp/keep3.conf" snapshot >"$tmp/out" 2>&1 || fail "snapshot $i: $(cat "$tmp/out")"
done
grep -q 'old snapshot deleted' "$tmp/out" || fail "nothing pruned: $(cat "$tmp/out")"
[ "$(count)" = 3 ] || fail "$(count) snapshots, expected 3"
seen=
for s in "$dir"/.snapshots/burn-*; do
	seen="$seen$(cat "$s/marker")"
done
[ "$seen" = 234 ] || fail "snapshots hold markers '$seen', expected 234"
if touch "$s/written" 2>/dev/null; then fail "snapshot $s is writable"; fi
# older snapshots inside a snapshot are empty directories, not copies
for e in "$s"/.snapshots/*; do
	[ -z "$(ls -A "$e")" ] || fail "snapshot $s holds a copy of $e"
done

# SNAPSHOTS=0: burn snapshot still makes one and deletes none
b -c "$tmp/keep0.conf" snapshot >/dev/null 2>&1 || fail "snapshot with SNAPSHOTS=0"
[ "$(count)" = 4 ] || fail "$(count) snapshots after SNAPSHOTS=0, expected 4"

# not a subvolume: burn snapshot refuses, update goes on without one
mkdir -p "$dir/plain"
if "$burn" -r "$dir/plain" snapshot >"$tmp/out" 2>&1; then fail "snapshot of a plain directory"; fi
grep -q 'not a btrfs subvolume' "$tmp/out" || fail "plain directory: $(cat "$tmp/out")"

# update: the snapshot is taken after the builds, before anything is installed
for v in 1 2; do
	printf 'name=snaptest\nversion=%s\nrelease=1\nsummary="btrfs test"\nhomepage=https://example.org\nlicense=(MIT)\ncategory=extra\nbuild_type=meta\n' \
		"$v" >"$tmp/r$v/snaptest/recipe"
done
b -R "$tmp/r1" -y install snaptest >"$tmp/out" 2>&1 || fail "install: $(cat "$tmp/out")"
b -R "$tmp/r2" -c "$tmp/keep3.conf" --no-sync -y update >"$tmp/out" 2>&1 || fail "update: $(cat "$tmp/out")"
snap=$(sed -n 's|.*snapshot: \(.*/\.snapshots/burn-[^ ]*\).*|\1|p' "$tmp/out")
[ -n "$snap" ] || fail "update made no snapshot: $(cat "$tmp/out")"
grep -q '^version=1$' "$snap/var/lib/burn/installed/snaptest/meta" || fail "the snapshot does not hold snaptest 1"
grep -q '^version=2$' "$dir/var/lib/burn/installed/snaptest/meta" || fail "snaptest 2 is not installed"
[ "$(count)" = 3 ] || fail "$(count) snapshots after update, expected 3"
rm -rf "$tmp"
echo "btrfs: OK"
