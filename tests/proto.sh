#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/proto.sh BURN PROTOTYPE RECIPES PROFILES: burn in C against the bash prototype.
#   BURN        ./burn
#   PROTOTYPE   CleanLinux/burn/burn
#   RECIPES     CleanLinux-Recipes/packages
#   PROFILES    CleanLinux/profiles
# Every command runs with both on the same staging root; stdout, stderr and the exit status
# must be identical. Used while the C version replaces the prototype.
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
printf 'name=zstd\nversion=1.5.7\nrelease=1\nfeatures=+lz4\n' >"$root/var/lib/burn/installed/zstd/meta"
printf 'name=good\nversion=2.0\nrelease=1\nfeatures=\n' >"$root/var/lib/burn/installed/good/meta"
printf 'zstd\ngood\n' >"$root/etc/burn/hold"
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

run_one() { # TOOL OUTPREFIX ARGS...
	tool=$1 out=$2
	shift 2
	set +e
	# shellcheck disable=SC2086 # the command line is split into words on purpose
	env BURN_PROFILE_DIR="$profiles" BURN_CACHE="$tmp/cache" HOME="$tmp" NO_COLOR=1 \
		"$tool" -r "$root" -R "$recipes" -R "$tmp/fixtures" "$@" >"$out.out" 2>"$out.err" </dev/null
	echo "exit $?" >>"$out.err"
	set -e
}

n=0 fail=0
while IFS= read -r line; do
	n=$((n + 1))
	# shellcheck disable=SC2086
	run_one "$proto" "$tmp/want" $line
	# shellcheck disable=SC2086
	run_one "$c" "$tmp/got" $line
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
