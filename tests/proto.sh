#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/proto.sh BURN PROTOTYPE RECIPES PROFILES: burn in C against the bash prototype.
#   BURN        ./burn
#   PROTOTYPE   CleanLinux/burn/burn
#   RECIPES     CleanLinux-Recipes/packages
#   PROFILES    CleanLinux/profiles
# Every command runs with both on the same staging root (each version keeps its own copy, moved
# into place for its run); stdout, stderr, the exit status and the root afterwards must be
# identical. Used while the C version replaces the prototype.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
abs() { (cd "$(dirname "$1")" && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
c=$(abs "$1") proto=$(abs "$2") recipes=$(cd "$3" && pwd) profiles=$(cd "$4" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
root=$tmp/root
mkdir -p "$root/etc/burn" "$root/var/lib/burn/installed/zstd" "$root/var/lib/burn/installed/good"
cat >"$root/etc/burn/burn.conf" <<'CONF'
FEATURES="-doc x11"
LOCALES="en"
CONF
printf '# per package\nbash -nls\ngood -tls +docs # comment\n' >"$root/etc/burn/package.features"
db=$root/var/lib/burn/installed
printf 'name=zstd\nversion=1.5.7\nrelease=1\nprofile=native\nfeatures=+lz4\n' >"$db/zstd/meta"
printf 'usr/\nusr/bin/\nusr/bin/zstd\nusr/lib/\nusr/lib/libzstd.so.1\nusr/lib/libzstd.so.1.5.7\n' >"$db/zstd/files"
printf 'libzstd.so.1\n' >"$db/zstd/provides-so"
printf 'libc.so.6\nliblz4.so.1\n' >"$db/zstd/needs-so"
printf 'name=good\nversion=2.0\nrelease=1\nfeatures=\ndepends=zstd\n' >"$db/good/meta"
printf 'etc/\netc/good.conf\nusr/\nusr/bin/\nusr/bin/good\nusr/bin/good-tool\n' >"$db/good/files"
: >"$db/good/provides-so"
printf 'libc.so.6\nlibzstd.so.1\n' >"$db/good/needs-so"
printf 'usr/lib/libgood.so.1\nusr/lib/libgood.so.1.0\n' >"$db/good/preserved"
mkdir -p "$db/.hidden" "$db/nometa" "$root/usr/lib" "$root/var/lib/burn/state" "$root/empty"
printf 'name=hidden\n' >"$db/.hidden/meta"
printf 'usr/bin/zstd\n' >"$db/nometa/files"
: >"$root/usr/lib/libc.so.6"
printf 'zstd\ngood\n' >"$root/etc/burn/hold"
{
	echo '2026-01-02T03:04:05Z|install|zstd|-|1.5.7-1|'
	echo '2026-01-03T03:04:05Z|upgrade|good|1.0-1|2.0-1|/var/cache/burn/packages/good-1.0-1-x86_64-abc.pkg.tar.zst'
	echo ''
	echo 'short|line'
	printf '2026-01-04T03:04:05Z|remove|lz4|1.10.0-2|-|'
} >"$root/var/lib/burn/state/history"
cp -r "$here/fixtures/recipes" "$tmp/fixtures"
printf 'binary\0data' >"$tmp/fixtures/broken-Name/blob.bin"
head -c 300000 /dev/zero | tr '\0' a >"$tmp/fixtures/broken-Name/big.txt"

# one command per line: the arguments after "burn -r ROOT -R RECIPES -R FIXTURES"
list=$tmp/commands
{
	echo "search"
	echo "search ^z"
	echo "search GNU.*SHELL"
	echo "search nothing-matches-this"
	echo "lint"
	echo "lint good broken"
	echo "lint $tmp/fixtures/good"
	echo "lint nosuchrecipe"
	echo "profile show"
	echo "profile list"
	echo "profile detect"
	echo "-p steam-deck profile show"
	echo "-p generic-aarch64 profile show"
	echo "--target aarch64-cleanlinux-linux-gnu profile show"
	echo "-p nosuchprofile profile show"
	for f in "$profiles"/*.profile "$profiles"/devices/*.profile; do
		f=${f##*/}
		echo "-p ${f%.profile} profile show"
	done
	echo "-f -tls -f +docs info good"
	echo "--features=+tls query good features"
	echo "info nosuchrecipe"
	echo "query bash nosuchvar"
	echo "query bash"
	echo "-j 0 info bash"
	echo "--bogus info bash"
	echo "frobnicate"
	echo "info good"
	echo "info broken"
	# the database; hold and unhold change the root
	echo "list"
	echo "-v list"
	echo "list ignored-argument"
	echo "files zstd"
	echo "files good"
	echo "files nometa"
	echo "files"
	echo "files zstd good"
	for f in /usr/bin/zstd "$root/usr/bin/zstd" /bin/zstd /sbin/good /usr/sbin/good-tool /lib64/libzstd.so.1 \
		/usr/lib64/libzstd.so.1 /usr/bin /etc/good.conf /nonexistent/file / "$root" relative/path; do
		echo "owner $f"
	done
	echo "owner"
	echo "owner a b"
	echo "history"
	echo "doctor"
	echo "--host-deps doctor"
	echo "-r $root/empty list"
	echo "-r $root/empty history"
	echo "-r $root/empty doctor"
	echo "-r $root/empty hold"
	echo "-r $root/empty unhold zstd"
	echo "hold"
	echo "hold newpkg zstd newpkg"
	echo "hold"
	echo "list"
	echo "unhold zstd"
	echo "unhold zstd nosuch"
	echo "unhold"
	echo "hold"
	echo "-r $root/empty hold one"
	echo "-r $root/empty hold"
	for v in name version release summary homepage license category source sha256 build_type srcdir \
		depends makedepends features patches patch_strip options backup noextract configure_args \
		meson_args cmake_args make_args make_install_args cargo_args out_of_tree meson_subdir \
		cmake_subdir cflags_append rid features dir urls; do
		echo "query good $v"
		echo "query linux $v"
	done
	for r in "$recipes"/*/recipe; do
		p=${r%/recipe}
		p=${p##*/}
		for v in info "query $p rid" "query $p features" "query $p urls" "query $p version"; do
			case $v in info) echo "info $p" ;; *) echo "$v" ;; esac
		done
	done
} >"$list"

run_one() { # TOOL want|got ARGS...
	tool=$1 out=$tmp/$2
	shift 2
	mv "$out.root" "$root"
	set +e
	env BURN_PROFILE_DIR="$profiles" BURN_CACHE="$tmp/cache" HOME="$tmp" NO_COLOR=1 LC_ALL=C \
		"$tool" -r "$root" -R "$recipes" -R "$tmp/fixtures" "$@" >"$out.out" 2>"$out.err" </dev/null
	echo "exit $?" >>"$out.err"
	set -e
	mv "$root" "$out.root"
}

# the root as text: every path with its type and mode, and the content of regular files
snapshot() {
	(
		cd "$1" || exit 1
		find . -path ./var/lib/burn/lock -prune -o -printf '%p %y %m\n' | LC_ALL=C sort
		find . -type f ! -path ./var/lib/burn/lock | LC_ALL=C sort | while IFS= read -r f; do
			echo "== $f"
			cat "$f"
			echo
		done
	)
}

cp -a "$root" "$tmp/want.root"
mv "$root" "$tmp/got.root"
n=0 fail=0
while IFS= read -r line; do
	n=$((n + 1))
	# shellcheck disable=SC2086 # the command line is split into words on purpose
	run_one "$proto" want $line
	# shellcheck disable=SC2086
	run_one "$c" got $line
	if ! snapshot "$tmp/want.root" >"$tmp/want.tree" || ! snapshot "$tmp/got.root" >"$tmp/got.tree"; then
		echo "proto: cannot read the staging roots" >&2
		exit 1
	fi
	if ! cmp -s "$tmp/want.tree" "$tmp/got.tree"; then
		fail=$((fail + 1))
		echo "FAIL (root): burn $line   (- prototype, + C)"
		diff -u "$tmp/want.tree" "$tmp/got.tree" | sed '1,2d; s/^/    /' | head -n 30
		rm -rf "$tmp/got.root"
		cp -a "$tmp/want.root" "$tmp/got.root"
	fi
	# lint: the prototype reports file problems in directory order, burn in C sorts them
	case $line in lint*) for f in want got; do sort "$tmp/$f.out" -o "$tmp/$f.out"; done ;; esac
	for s in out err; do
		if ! cmp -s "$tmp/want.$s" "$tmp/got.$s"; then
			fail=$((fail + 1))
			echo "FAIL ($s): burn $line   (- prototype, + C)"
			diff -u "$tmp/want.$s" "$tmp/got.$s" | sed '1,2d; s/^/    /' | head -n 30
		fi
	done
done <"$list"
echo "proto: $n commands, $fail differences"
[ "$fail" -eq 0 ]
