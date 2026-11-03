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
echo "cli: OK"
