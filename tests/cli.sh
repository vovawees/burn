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
# hardware variables: files next to the profile ($PROFILE_DIR), reset between profiles, part of
# the build id of the recipes that use them (and only of those)
mkdir -p "$tmp/profiles/deck.d" "$tmp/hwroot"
echo 'one' >"$tmp/profiles/deck.d/a.rules"
# shellcheck disable=SC2016 # the profile expands $PROFILE_DIR itself
printf 'inherit native\nVIDEO_CARDS=amd\nFIRMWARE="amdgpu rtw88"\nQUIRKS="$PROFILE_DIR/deck.d"\n' >"$tmp/profiles/deck.profile"
b -p deck profile show | grep -q "^quirks  *$tmp/profiles/deck.d$" || fail "PROFILE_DIR"
b -p deck profile show | grep -q '^firmware  *amdgpu rtw88$' || fail "profile: firmware"
b profile show | grep -q '^firmware  *-$' || fail "profile: hardware variables are reset"
hw() {
	env BURN_PROFILE_DIR="$tmp/profiles" BURN_CACHE="$tmp/hwcache" NO_COLOR=1 "$BURN" -r "$tmp/hwroot" \
		-R "$fixtures" --host-deps -y "$@" build bt-hw >"$tmp/hw.log" 2>&1 || fail "build bt-hw $*: $(cat "$tmp/hw.log")"
	find "$tmp/hwcache/packages" -name 'bt-hw-*' | wc -l
}
[ "$(hw -p deck)" -eq 1 ] || fail "bt-hw: first build"
[ "$(hw -p deck)" -eq 1 ] || fail "bt-hw: same profile, cached build"
echo 'two' >"$tmp/profiles/deck.d/a.rules"
[ "$(hw -p deck)" -eq 2 ] || fail "bt-hw: the contents of QUIRKS are part of the build id"
[ "$(hw -p native)" -eq 3 ] || fail "bt-hw: VIDEO_CARDS is part of the build id"
sed -i 's/rtw88/rtw89/' "$tmp/profiles/deck.profile"
[ "$(hw -p deck)" -eq 3 ] || fail "bt-hw: FIRMWARE is not used, the build is cached"
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
	# file triggers: a package's usr/share/burn/triggers/*.trigger, run after the transaction
	mkdir -p "$tmp/dest/usr/share/burn/triggers" "$tmp/dest/usr/bin"
	printf '# test\npath=usr/lib/modules/*/vmlinuz\npath=usr/lib/other/*\nrun=/usr/bin/trig --root $ROOT\n' \
		>"$tmp/dest/usr/share/burn/triggers/kernel.trigger"
	printf '#!/bin/sh\necho "$*" >>"$2/triggered"\n' >"$tmp/dest/usr/bin/trig" && chmod 755 "$tmp/dest/usr/bin/trig"
	mkpkg trig 1 '' '' ''
	mkdir -p "$tmp/dest/usr/lib/modules/1.0" && echo k >"$tmp/dest/usr/lib/modules/1.0/vmlinuz"
	mkpkg kern 1 '' '' ''
	mkdir -p "$tmp/dest/usr/share/doc" && echo x >"$tmp/dest/usr/share/doc/x"
	mkpkg other 1 '' '' ''
	b install "$pkgs/trig-1.pkg.tar.zst" "$pkgs/kern-1.pkg.tar.zst" 2>"$tmp/err" || fail "install with a trigger"
	[ "$(cat "$tmp/root/triggered")" = "--root $tmp/root" ] || fail "trigger: run once, \$ROOT: $(cat "$tmp/err")"
	grep -q 'trigger kernel' "$tmp/err" || fail "trigger message"
	b install "$pkgs/other-1.pkg.tar.zst" 2>/dev/null || fail "install other"
	[ "$(wc -l <"$tmp/root/triggered")" -eq 1 ] || fail "trigger: unrelated paths do not run it"
	b -y remove kern 2>/dev/null || fail "remove kern"
	[ "$(wc -l <"$tmp/root/triggered")" -eq 2 ] || fail "trigger: removals run it"
	# clone: the builds another root has installed, from the package cache, nothing built
	mkdir -p "$tmp/ccache/packages" "$tmp/clone" "$tmp/clone1"
	mkdir -p "$tmp/dest/usr/bin" && echo dep >"$tmp/dest/usr/bin/dep"
	mkpkg dep 1 '' '' ''
	mkdir -p "$tmp/dest/usr/bin" && echo top >"$tmp/dest/usr/bin/top"
	mkpkg top 1 '' '' dep
	b install "$pkgs/dep-1.pkg.tar.zst" "$pkgs/top-1.pkg.tar.zst" 2>/dev/null || fail "install dep top"
	rm -r "$db/zstd" # hand-written above: no cache file for it
	for p in trig other dep top; do cp "$pkgs/$p-1.pkg.tar.zst" "$tmp/ccache/packages/$p-1-1-x86_64-b1.pkg.tar.zst"; done
	c() { env BURN_PROFILE_DIR="$tmp/profiles" BURN_CACHE="$tmp/ccache" NO_COLOR=1 "$BURN" "$@"; }
	c -r "$tmp/clone" -y clone "$tmp/root" 2>"$tmp/err" || fail "clone: $(cat "$tmp/err")"
	[ "$(c -r "$tmp/clone" list | awk '{print $1}' | tr '\n' ' ')" = "dep other top trig " ] || fail "clone: all packages"
	[ "$(cat "$tmp/clone/usr/bin/top")" = top ] || fail "clone: contents"
	c -r "$tmp/clone1" -y clone "$tmp/root" top 2>"$tmp/err" || fail "clone top: $(cat "$tmp/err")"
	[ "$(c -r "$tmp/clone1" list | awk '{print $1}' | tr '\n' ' ')" = "dep top " ] || fail "clone: a package and its dependencies"
	grep -q 'Packages (2): dep top' "$tmp/err" || fail "clone: dependency order: $(cat "$tmp/err")"
	if c -r "$tmp/root" -y clone "$tmp/root" 2>"$tmp/err"; then fail "clone: into itself"; fi
	grep -q 'is the target root itself' "$tmp/err" || fail "clone: same root message"
	if c -r "$tmp/clone1" -y clone "$tmp/root" nosuch 2>"$tmp/err"; then fail "clone: unknown package"; fi
	grep -q 'nosuch is not installed in' "$tmp/err" || fail "clone: unknown package message"
	rm "$tmp/ccache/packages/dep-1-1-x86_64-b1.pkg.tar.zst"
	if c -r "$tmp/clone2" -y clone "$tmp/root" top 2>"$tmp/err"; then fail "clone: missing package file"; fi
	grep -q '1 package file(s) missing' "$tmp/err" || fail "clone: missing message: $(cat "$tmp/err")"
	[ ! -e "$tmp/clone2/usr/bin/top" ] || fail "clone: nothing installed when a file is missing"
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
