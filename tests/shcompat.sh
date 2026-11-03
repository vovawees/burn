#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/shcompat.sh SHDUMP [RECIPES_DIR]: burn's recipe language parser against bash.
#   tests/sh/*.sh        bash and the parser must report the same variables and functions
#   tests/sh/err/*.sh    the parser must fail with the message of the "# error:" line
#   RECIPES_DIR          also every RECIPES_DIR/*/recipe (e.g. ../CleanLinux-Recipes/packages)
set -eu
if ! command -v bash >/dev/null 2>&1; then
	echo "shcompat: no bash, skipped"
	exit 0
fi
here=$(cd "$(dirname "$0")" && pwd)
dump=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
recipes=${2:-}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/cwd"
# both sides get the same environment and an empty working directory
run() {
	(cd "$tmp/cwd" && env -i PATH="$PATH" HOME=/home/test LC_ALL=C TESTENV=from-env \
		TESTSPACE="a  b	c" TESTENV2=gone CARCH=x86_64 CHOST=x86_64-cleanlinux-linux-gnu "$@")
}
n=0 fail=0
compare() { # NAME FILE
	n=$((n + 1))
	run bash "$here/shdump.bash" "$2" >"$tmp/want" 2>&1 || true
	run "$dump" "$2" >"$tmp/got" 2>&1 || true
	if ! cmp -s "$tmp/want" "$tmp/got"; then
		fail=$((fail + 1))
		echo "FAIL: $1 (- bash, + burn)"
		diff -u "$tmp/want" "$tmp/got" | sed '1,2d; s/^/    /' | head -n 40
	fi
}
for f in "$here"/sh/*.sh; do
	compare "sh/${f##*/}" "$f"
done
for f in "$here"/sh/err/*.sh; do
	n=$((n + 1))
	want=$(sed -n '1s/^# error: //p' "$f")
	if run "$dump" "$f" >"$tmp/got" 2>&1; then
		fail=$((fail + 1))
		echo "FAIL: sh/err/${f##*/}: no error, want: $want"
	elif ! grep -qF -- "$want" "$tmp/got"; then
		fail=$((fail + 1))
		echo "FAIL: sh/err/${f##*/}: want: $want"
		sed 's/^/    got: /' "$tmp/got"
	fi
done
if [ -n "$recipes" ]; then
	for f in "$recipes"/*/recipe; do
		[ -f "$f" ] || continue
		d=${f%/recipe}
		compare "recipe ${d##*/}" "$(cd "$d" && pwd)/recipe"
	done
fi
echo "shcompat: $n cases, $fail failed"
[ "$fail" -eq 0 ]
