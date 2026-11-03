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
if [ "$(id -u)" != 0 ]; then
	if env BURN_PROFILE_DIR="$tmp/profiles" "$BURN" -r / hold x 2>"$tmp/err"; then fail "/ needs root"; fi
	grep -q 'run it as root' "$tmp/err" || fail "root message"
fi
echo "cli: OK"
