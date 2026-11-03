#!/bin/sh
# Command line tests: sh tests/cli.sh ./burn
set -eu
BURN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
fixtures=$(cd "$(dirname "$0")/fixtures/recipes" && pwd)
fail() {
	echo "FAIL: $*" >&2
	exit 1
}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/profiles" "$tmp/root/etc/burn"
# shellcheck disable=SC2016 # the profile expands $HOST_ARCH itself
printf 'PROFILE_DESC="Test machine"\nCARCH=$HOST_ARCH\nPROFILE_FEATURES="nls -doc"\n' >"$tmp/profiles/native.profile"
printf 'inherit native\nPROFILE_DESC=Handheld\nVIDEO_CARDS=amdgpu\n' >"$tmp/profiles/handheld.profile"
b() { env BURN_PROFILE_DIR="$tmp/profiles" HOST_VIDEO_CARDS= NO_COLOR=1 "$BURN" -r "$tmp/root" -R "$fixtures" "$@"; }

"$BURN" --version | grep -q '^burn ' || fail "--version"
[ "$("$BURN" vercmp 1.10 1.9)" = 1 ] || fail "vercmp"
[ "$("$BURN" vercmp 2.0~rc1 2.0)" = -1 ] || fail "vercmp ~"
if "$BURN" no-such-command 2>/dev/null; then fail "unknown command must fail"; fi

# recipes
[ "$(b query good version)" = 2.1 ] || fail "query version"
[ "$(b query good features)" = "-docs +tls" ] || fail "query features"
[ "$(b -f -tls -f +docs query good features)" = "+docs -tls" ] || fail "-f"
b search 'without PROBLEMS' | grep -q '^good 2\.1-3 ' || fail "search"
b info good | grep -q '^installed  *no$' || fail "info"
[ "$(b lint broken-Name 2>/dev/null | wc -l)" -eq 23 ] || fail "lint: every rule"
if b lint good >/dev/null 2>&1; then fail "lint: unknown dependency must fail"; fi
if b query nosuchrecipe version 2>/dev/null; then fail "query: unknown recipe must fail"; fi

# profiles
b profile show | grep -q '^description  *Test machine$' || fail "profile show"
b -p handheld profile show | grep -q '^video_cards  *amdgpu$' || fail "profile inherit"
[ "$(b profile list | grep -c .)" -ge 2 ] || fail "profile list"
if b -p nosuchprofile profile show 2>/dev/null; then fail "unknown profile must fail"; fi
# the database
db=$tmp/root/var/lib/burn/installed
mkdir -p "$db/zstd" "$tmp/root/var/lib/burn/state"
printf 'name=zstd\nversion=1.5.7\nrelease=2\n' >"$db/zstd/meta"
printf 'usr/\nusr/bin/\nusr/bin/zstd\nusr/lib/\nusr/lib/libzstd.so.1\n' >"$db/zstd/files"
printf 'libzstd.so.1\n' >"$db/zstd/provides-so"
printf 'libzstd.so.1\n' >"$db/zstd/needs-so"
[ "$(b list)" = "zstd                     1.5.7-2" ] || fail "list"
[ "$(b files zstd)" = "$(printf '/usr/bin/zstd\n/usr/lib/libzstd.so.1')" ] || fail "files"
[ "$(b owner "$tmp/root/usr/bin/zstd")" = "/usr/bin/zstd is owned by zstd 1.5.7-2" ] || fail "owner in the root"
[ "$(b owner /usr/sbin/zstd)" = "/usr/bin/zstd is owned by zstd 1.5.7-2" ] || fail "owner: merged /usr"
if b owner /usr/bin/nothing 2>/dev/null; then fail "owner: nobody owns it"; fi
b doctor 2>&1 | grep -q 'no problems found (1 packages)' || fail "doctor"
b hold zstd 2>/dev/null
b list | grep -q '\[held\]$' || fail "hold"
b unhold zstd 2>/dev/null
[ -z "$(b hold)" ] || fail "unhold"
b history 2>&1 | grep -q 'no transactions yet' || fail "history: empty"
echo '2026-01-02T03:04:05Z|install|zstd|-|1.5.7-2|' >"$tmp/root/var/lib/burn/state/history"
[ "$(b history)" = "2026-01-02T03:04:05Z  install   zstd                   - -> 1.5.7-2" ] || fail "history"
if command -v flock >/dev/null; then
	if flock "$tmp/root/var/lib/burn/lock" env BURN_PROFILE_DIR="$tmp/profiles" "$BURN" -r "$tmp/root" hold x 2>"$tmp/err"; then
		fail "the lock must keep a second burn out"
	fi
	grep -q 'another burn process is working on' "$tmp/err" || fail "lock message"
fi
# packages (needs GNU tar and zstd to make them)
if command -v zstd >/dev/null && tar --version 2>/dev/null | grep -q GNU; then
	pkgs=$tmp/pkgs
	mkdir -p "$pkgs"
	mkpkg() { # NAME VERSION PROVIDES NEEDS DEPENDS: the tree in $tmp/dest
		d=$tmp/dest
		mkdir -p "$d/.BURN"
		printf 'name=%s\nversion=%s\nrelease=1\narch=x86_64\nbuildid=b%s\ndepends=%s\n' "$1" "$2" "$2" "$5" >"$d/.BURN/meta"
		(cd "$d" && find . -mindepth 1 \( -path ./.BURN -prune \) -o \( -type d -printf '%P/\n' \) -o -printf '%P\n') |
			LC_ALL=C sort >"$d/.BURN/files"
		printf '%s' "$3" >"$d/.BURN/provides-so"
		printf '%s' "$4" >"$d/.BURN/needs-so"
		(cd "$d" && LC_ALL=C tar --sort=name --numeric-owner --owner=0 --group=0 -cf - .) | zstd -q >"$pkgs/$1-$2.pkg.tar.zst"
		rm -rf "$d"
	}
	mkdir -p "$tmp/dest/usr/lib" && echo 1 >"$tmp/dest/usr/lib/libfoo.so.1"
	mkpkg libfoo 1 'libfoo.so.1
' '' ''
	mkdir -p "$tmp/dest/usr/lib" && echo 2 >"$tmp/dest/usr/lib/libfoo.so.2"
	mkpkg libfoo 2 'libfoo.so.2
' '' ''
	mkdir -p "$tmp/dest/usr/bin" "$tmp/dest/etc" && echo app >"$tmp/dest/usr/bin/app" && echo conf >"$tmp/dest/etc/app.conf"
	mkpkg app 1 '' 'libfoo.so.1
' libfoo
	b install "$pkgs/libfoo-1.pkg.tar.zst" "$pkgs/app-1.pkg.tar.zst" 2>/dev/null || fail "install files"
	[ "$(cat "$tmp/root/usr/lib/libfoo.so.1")" = 1 ] || fail "install: contents"
	# the upgrade drops a soname app still links against: the library stays until app is rebuilt
	if b install "$pkgs/libfoo-2.pkg.tar.zst" 2>"$tmp/err"; then fail "ABI change must ask for a rebuild"; fi
	grep -q 'preserving /usr/lib/libfoo.so.1: still needed' "$tmp/err" || fail "preserved library message"
	grep -q 'ABI change (libfoo.so.1): rebuilding app' "$tmp/err" || fail "ABI change message"
	[ -f "$tmp/root/usr/lib/libfoo.so.1" ] && [ -f "$tmp/root/usr/lib/libfoo.so.2" ] || fail "preserved library"
	grep -qx 'usr/lib/libfoo.so.1' "$db/libfoo/preserved" || fail "preserved list"
	b list | grep -q '^libfoo  *2-1$' || fail "upgraded"
	if b remove libfoo -y 2>"$tmp/err"; then fail "remove: still required"; fi
	grep -q 'libfoo is required by: app' "$tmp/err" || fail "required by message"
	if b remove app 2>"$tmp/err" </dev/null; then fail "remove must ask"; fi
	grep -q 'use --yes' "$tmp/err" || fail "confirmation message"
	b -y remove libfoo app 2>/dev/null || fail "remove both"
	[ ! -e "$tmp/root/usr/lib/libfoo.so.1" ] && [ ! -e "$tmp/root/usr/bin/app" ] || fail "remove: files"
	b history | grep -q 'remove  *app' || fail "history: remove"
	b -y rollback libfoo 2>/dev/null && fail "rollback: not installed"
	printf 'not a package\n' | zstd -q >"$pkgs/junk.pkg.tar.zst"
	if b install "$pkgs/junk.pkg.tar.zst" 2>"$tmp/err"; then fail "junk must fail"; fi
	grep -q 'junk.pkg.tar.zst: truncated archive' "$tmp/err" || fail "junk message: $(cat "$tmp/err")"
	if b install "$pkgs/missing.pkg.tar.zst" 2>/dev/null; then fail "missing file must fail"; fi
fi
# sources: parallel downloads from a mirror, sha256 inside burn
if command -v curl >/dev/null || command -v wget >/dev/null; then
	mkdir -p "$tmp/mirror/fetchme"
	printf 'fetchme 1.0\n' >"$tmp/mirror/fetchme/fetchme-1.0.txt"
	printf 'GNU_MIRROR=file://%s/mirror\nFETCH_JOBS=4\n' "$tmp" >"$tmp/fetch.conf"
	f() { BURN_CACHE="$tmp/cache" b -c "$tmp/fetch.conf" "$@"; }
	f fetch fetchme 2>"$tmp/err" || fail "fetch: $(cat "$tmp/err")"
	grep -q 'sources: 2 downloaded and verified' "$tmp/err" || fail "fetch message"
	[ "$(cat "$tmp/cache/sources/renamed.txt")" = "fetchme 1.0" ] || fail "fetch: name::url"
	if f fetch fetchme fetchbad fetchnosum 2>"$tmp/err"; then fail "bad sources must fail"; fi
	grep -q 'fetchnosum: missing sha256 for nosum.txt' "$tmp/err" || fail "missing sha256 message"
	grep -q '2 source(s) could not be fetched or verified' "$tmp/err" || fail "failure count"
	[ -f "$tmp/cache/sources/bad.txt.bad" ] || fail "bad download moved aside"
	[ "$(f checksum fetchme | sed -n 2p)" = "	920036cf7afe3227e028562af93cd4c8be055794da11373062bcb8c2706650cd" ] || fail "checksum"
fi
# a build outlives the file of its burn (burn installed a new burn, then builds more): the
# helpers of recipe functions still reach the running burn
mkdir -p "$tmp/self/selfdel"
cp "$BURN" "$tmp/burn-copy"
# shellcheck disable=SC2016 # the recipe expands these
printf '%s\n' 'name=selfdel' 'version=1' 'release=1' 'summary="burn replaced during a build"' \
	'homepage=https://example.org/selfdel' 'license=(MIT)' 'category=extra' 'build_type=custom' \
	'do_install() {' "	rm -f '$tmp/burn-copy'" '	[ "$(vercmp 2 1)" = 1 ]' \
	'	mkdir -p "$DESTDIR/usr/share/selfdel"' '}' >"$tmp/self/selfdel/recipe"
env BURN_PROFILE_DIR="$tmp/profiles" BURN_CACHE="$tmp/cache" NO_COLOR=1 "$tmp/burn-copy" -r "$tmp/root" \
	-R "$tmp/self" build selfdel >/dev/null 2>"$tmp/err" || fail "build without the burn file: $(cat "$tmp/err")"
if [ "$(id -u)" != 0 ]; then
	if env BURN_PROFILE_DIR="$tmp/profiles" "$BURN" -r / hold x 2>"$tmp/err"; then fail "/ needs root"; fi
	grep -q 'run it as root' "$tmp/err" || fail "root message"
fi
echo "cli: OK"
