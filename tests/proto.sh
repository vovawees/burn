#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/proto.sh BURN PROTOTYPE RECIPES PROFILES: burn in C against the bash prototype.
#   BURN        ./burn
#   PROTOTYPE   burn/burn of CleanLinux at the tag burn-prototype
#   RECIPES     CleanLinux-Recipes/packages
#   PROFILES    profiles of the same CleanLinux
# Every command runs with both on the same staging root (each version keeps its own copy, moved
# into place for its run); stdout, stderr, the exit status and the root afterwards must be
# identical, and so must the package cache (packages byte for byte, build logs). The prototype
# is frozen; this keeps the C version from drifting where both should agree.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
abs() { (cd "$(dirname "$1")" && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
c=$(abs "$1") proto=$(abs "$2") recipes=$(cd "$3" && pwd) profiles=$(cd "$4" && pwd)
tmp=$(mktemp -d)
[ -n "${PROTO_KEEP-}" ] || trap 'rm -rf "$tmp"' EXIT
root=$tmp/root
mkdir -p "$root/etc/burn" "$root/var/lib/burn/installed/zstd" "$root/var/lib/burn/installed/good"
cat >"$root/etc/burn/burn.conf" <<'CONF'
FEATURES="-doc x11"
LOCALES="en"
FETCH_JOBS=1
CONF
# gnu:// sources come from a local mirror
mkdir -p "$tmp/mirror/fetchme"
printf 'fetchme 1.0\n' >"$tmp/mirror/fetchme/fetchme-1.0.txt"
printf 'GNU_MIRROR=file://%s\n' "$tmp/mirror" >>"$root/etc/burn/burn.conf"
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
sh "$here/mksources.sh" "$here/fixtures/src" "$tmp/fixtures"
# sync, plan and update: a recipe repository; their roots and hooks are set up by "@" lines
sh "$here/mkrepo.sh" "$tmp/uprepo.git"
mkdir -p "$tmp/up"
printf 'RECIPE_REPOS=(file://%s/uprepo.git)\nBURN_REPO_DIR=%s/cache/repos\nCORE_PKGS=(glibc up-lib)\nLOCALES=en\n' \
	"$tmp" "$tmp" >"$tmp/up/up.conf"
printf 'RECIPE_REPOS=(file:///nonexistent/burn-test/repo.git)\nBURN_REPO_DIR=%s/cache/repos-dead\n' "$tmp" >"$tmp/up/dead.conf"
# shellcheck disable=SC2016 # hooks get ROOT
{
	printf '#!/bin/sh\necho "pre-update in $ROOT"\nexit 1\n' >"$tmp/up/pre-fail"
	printf '#!/bin/sh\necho "snapshot of $ROOT"\n' >"$tmp/up/pre-ok"
	printf '#!/bin/sh\necho "post-update done"\n' >"$tmp/up/post-ok"
	printf '#!/bin/sh\necho "post-update failed" >&2\nexit 2\n' >"$tmp/up/post-fail"
	printf '#!/bin/sh\necho "not executable"\n' >"$tmp/up/not-executable"
}
chmod +x "$tmp/up/pre-fail" "$tmp/up/pre-ok" "$tmp/up/post-ok" "$tmp/up/post-fail"
# meson and cmake may come from pip (~/.local), and HOME changes below
PYTHONUSERBASE=${PYTHONUSERBASE:-$HOME/.local}
export PYTHONUSERBASE
printf 'binary\0data' >"$tmp/fixtures/broken-Name/blob.bin"
head -c 300000 /dev/zero | tr '\0' a >"$tmp/fixtures/broken-Name/big.txt"

# packages, packed like burn packs them, in the package cache (rollback finds them there)
pkgs=$tmp/cache/packages
mkdir -p "$pkgs"
mkpkg() { # NAME VERSION BUILDID: $tmp/dest becomes NAME-VERSION-1; DEPENDS BACKUP PROVIDES NEEDS
	d=$tmp/dest
	mkdir -p "$d/.BURN"
	{
		printf 'name=%s\nversion=%s\nrelease=1\narch=x86_64\nchost=x86_64-cleanlinux-linux-gnu\n' "$1" "$2"
		printf 'buildid=%s\nrid=%s\nprofile=native\nsummary=test\nlicense=MIT\n' "$3" "$3"
		printf 'depends=%s\nfeatures=\nbackup=%s\ncflags=-O2\nldflags=\n' "${DEPENDS-}" "${BACKUP-}"
		printf 'builddate=2023-11-14T22:13:20Z\n'
	} >"$d/.BURN/meta"
	(cd "$d" && find . -mindepth 1 \( -path ./.BURN -prune \) -o \( -type d -printf '%P/\n' \) -o -printf '%P\n') |
		LC_ALL=C sort >"$d/.BURN/files"
	printf '%s' "${PROVIDES-}" >"$d/.BURN/provides-so"
	printf '%s' "${NEEDS-}" >"$d/.BURN/needs-so"
	(cd "$d" && LC_ALL=C tar --sort=name --numeric-owner --owner=0 --group=0 --mtime=@1700000000 \
		--clamp-mtime -cf - .) | zstd -q >"$pkgs/$1-$2-1-x86_64-$3.pkg.tar.zst"
	rm -rf "$d"
}
long=$(printf 'long%.0s' $(seq 1 40))
hello_tree() { # VERSION
	d=$tmp/dest
	mkdir -p "$d/usr/bin" "$d/usr/lib" "$d/etc" "$d/usr/share/hello/empty" "$d/usr/share/hello/private"
	printf 'hello %s\n' "$1" >"$d/usr/bin/hello"
	chmod 755 "$d/usr/bin/hello"
	ln "$d/usr/bin/hello" "$d/usr/bin/hello-hard"
	ln -s hello "$d/usr/bin/hi"
	printf 'conf %s\n' "$1" >"$d/etc/hello.conf"
	printf 'lib %s\n' "$1" >"$d/usr/lib/libhello.so.1.$1"
	ln -s "libhello.so.1.$1" "$d/usr/lib/libhello.so.1"
	printf 'long\n' >"$d/usr/share/hello/$long.txt"
	printf 'read only\n' >"$d/usr/share/hello/ro"
	chmod 444 "$d/usr/share/hello/ro"
	chmod 700 "$d/usr/share/hello/private"
	printf 'suid\n' >"$d/usr/bin/hello-suid"
	chmod 4755 "$d/usr/bin/hello-suid"
	mkfifo "$d/usr/share/hello/fifo"
	printf 'secret\n' >"$d/usr/share/hello/private/key"
}
hello_tree 0
mkdir -p "$tmp/dest/usr/share/hello/old"
printf 'old\n' >"$tmp/dest/usr/share/hello/old/file"
printf 'old\n' >"$tmp/dest/usr/share/hello/old.txt"
BACKUP=etc/hello.conf PROVIDES='libhello.so.1
' NEEDS='libc.so.6
' mkpkg hello 1.0 b1
hello_tree 1
printf 'new\n' >"$tmp/dest/usr/share/hello/new.txt"
BACKUP=etc/hello.conf PROVIDES='libhello.so.1
' NEEDS='libc.so.6
' mkpkg hello 2.0 b2
hello_tree 0
BACKUP=etc/hello.conf PROVIDES='libhello.so.1
' mkpkg hello 1.0 b1x
mkdir -p "$tmp/dest/usr/bin"
printf 'user\n' >"$tmp/dest/usr/bin/user"
DEPENDS=hello NEEDS='libhello.so.1
' mkpkg user 1.0 u1
mkdir -p "$tmp/dest/usr/bin"
printf 'clash\n' >"$tmp/dest/usr/bin/hello"
printf 'clash\n' >"$tmp/dest/usr/bin/clash"
mkpkg clash 1.0 c1
mkdir -p "$tmp/dest/usr/bin"
printf 'bash\n' >"$tmp/dest/usr/bin/bash"
mkpkg bash 5.3 e1
mkdir -p "$tmp/dest/usr/lib/modules/6.1.0-test/kernel"
: >"$tmp/dest/usr/lib/modules/6.1.0-test/modules.order"
: >"$tmp/dest/usr/lib/modules/6.1.0-test/kernel/dummy.ko"
mkpkg kernel 6.1 k1
printf 'not a package' >"$pkgs/junk-1-1-x86_64-x.pkg.tar.zst"
# libfoo.so.1 moves to usr/lib/foo in 2.0: the old one stays preserved while app needs it
mkdir -p "$tmp/dest/usr/lib"
printf 'foo 1\n' >"$tmp/dest/usr/lib/libfoo.so.1.0"
ln -s libfoo.so.1.0 "$tmp/dest/usr/lib/libfoo.so.1"
PROVIDES='libfoo.so.1
' mkpkg foo 1.0 f1
mkdir -p "$tmp/dest/usr/lib/foo"
printf 'foo 2\n' >"$tmp/dest/usr/lib/foo/libfoo.so.1"
PROVIDES='libfoo.so.1
' mkpkg foo 2.0 f2
mkdir -p "$tmp/dest/usr/bin"
printf 'app\n' >"$tmp/dest/usr/bin/app"
DEPENDS=foo NEEDS='libfoo.so.1
' mkpkg app 1.0 a1

# one command per line: the arguments after "burn -r ROOT -R RECIPES -R FIXTURES";
# "@ SHELL" runs SHELL on both roots ($ROOT) instead
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
	# packages
	h1=$pkgs/hello-1.0-1-x86_64-b1.pkg.tar.zst
	h2=$pkgs/hello-2.0-1-x86_64-b2.pkg.tar.zst
	echo "install"
	echo "install /nonexistent/x.pkg.tar.zst"
	echo "install $h1"
	echo "list"
	echo "files hello"
	echo "owner /usr/bin/hi"
	echo "install $h1"
	echo "--force install $h1"
	echo "install $pkgs/hello-1.0-1-x86_64-b1x.pkg.tar.zst"
	echo "install $h1"
	echo "install $pkgs/user-1.0-1-x86_64-u1.pkg.tar.zst"
	echo "install $pkgs/clash-1.0-1-x86_64-c1.pkg.tar.zst"
	# shellcheck disable=SC2016 # expanded by the "@" runner
	printf '%s\n' '@ echo "my conf" >"$ROOT/etc/hello.conf" && touch -d @1600000000 "$ROOT/etc/hello.conf"'
	echo "install $h2"
	echo "history"
	echo "doctor"
	echo "rollback hello"
	echo "rollback user"
	echo "rollback nosuch"
	echo "hold"
	echo "unhold hello"
	echo "install $h2"
	echo "install $h1 $h2"
	echo "remove"
	echo "remove hello"
	echo "-y remove nosuch"
	echo "-y remove hello"
	echo "-y remove hello user"
	echo "list"
	echo "install $pkgs/bash-5.3-1-x86_64-e1.pkg.tar.zst"
	echo "-y remove bash"
	echo "-y --force remove bash"
	echo "install $pkgs/kernel-6.1-1-x86_64-k1.pkg.tar.zst"
	echo "-y remove kernel"
	echo "install $h2"
	echo "--force install $pkgs/clash-1.0-1-x86_64-c1.pkg.tar.zst"
	echo "-y uninstall clash"
	# preserved libraries
	f1=$pkgs/foo-1.0-1-x86_64-f1.pkg.tar.zst
	f2=$pkgs/foo-2.0-1-x86_64-f2.pkg.tar.zst
	echo "install $f1 $pkgs/app-1.0-1-x86_64-a1.pkg.tar.zst"
	echo "install $f2"
	echo "doctor"
	echo "rollback foo"
	echo "unhold foo"
	echo "install $f2"
	echo "-y --force remove foo"
	echo "doctor"
	echo "install $f1"
	echo "install $f2"
	echo "-y remove app"
	echo "doctor"
	echo "-y remove foo"
	echo "history"
	# sources: the mirror (file://), the recipe directory, the cache
	echo "fetch"
	echo "fetch nosuchrecipe"
	echo "fetch fetchme"
	echo "fetch fetchme fetchme"
	echo "fetch fetchbad"
	echo "fetch fetchdead"
	echo "fetch fetchnosum"
	echo "fetch fetchme fetchbad fetchnosum"
	echo "fetch fetchlocal"
	echo "checksum"
	echo "checksum fetchme fetchbad"
	echo "checksum fetchme"
	echo "checksum fetchbad"
	echo "checksum fetchlocal"
	echo "checksum fetchdead"
	echo "checksum nosuchrecipe"
	# shellcheck disable=SC2016 # expanded by the "@" runner
	printf '%s\n' '@ mkdir -p "$ROOT/r/fetchw" && cp '"$here"'/fixtures/recipes/fetchme/notes.txt "$ROOT/r/fetchw/" && sed "s/^name=fetchme/name=fetchw/; s/^\t9200.*/\tSKIP/" '"$here"'/fixtures/recipes/fetchme/recipe >"$ROOT/r/fetchw/recipe"'
	echo "-R $root/r checksum fetchw"
	echo "-R $root/r --write checksum fetchw"
	echo "-R $root/r query fetchw sha256"
	# shellcheck disable=SC2016
	printf '%s\n' '@ printf "name=fetchw\nversion=1\nsource=(notes.txt)\n" >"$ROOT/r/fetchw/recipe"'
	echo "-R $root/r --write checksum fetchw"
	# builds: every build type, finalize, the packages and logs in the cache
	echo "build"
	echo "build nosuchrecipe"
	echo "build bt-custom"
	echo "build bt-custom"
	echo "--rebuild build bt-custom"
	echo "-f -tls build bt-custom"
	echo "build bt-opts"
	echo "--check build bt-auto"
	echo "build bt-areconf"
	echo "build bt-noconf"
	echo "build bt-make"
	echo "build bt-cmake"
	echo "build bt-meson"
	echo "build bt-meta bt-opts"
	echo "build bt-fail"
	echo "-v build bt-fail"
	echo "build bt-badtype"
	echo "build bt-nopatch"
	echo "build bt-badpatch"
	echo "build bt-nosrcdir"
	echo "build bt-cargo"
	echo "build fetchme"
	echo "build fetchbad bt-opts"
	# sources ready for hacking
	echo "unpack"
	echo "unpack a b c"
	echo "unpack nosuchrecipe"
	echo "unpack bt-auto"
	echo "unpack bt-auto $root/src/bt-auto"
	echo "unpack bt-custom $root/src/custom"
	echo "unpack bt-nosrcdir"
	echo "unpack bt-badpatch $root/src/badpatch"
	# install from recipes: dependencies first, up to date, ABI rebuilds
	echo "install bt-app"
	echo "-y install bt-app"
	echo "-y install bt-app"
	echo "files bt-app"
	echo "-y install bt-meta"
	echo "-y install bt-missing"
	echo "--nodeps -y install bt-missing"
	echo "-y install bt-cyc1"
	echo "list"
	echo "@ sed -i 's/^version=1.0/version=2.0/; s/^sover=1/sover=2/' $tmp/fixtures/bt-lib/recipe && touch -d @1700000000 $tmp/fixtures/bt-lib/recipe"
	echo "-y install bt-lib"
	echo "files bt-lib"
	echo "doctor"
	echo "history"
	echo "-y remove bt-meta bt-app bt-lib bt-missing"
	# rolling+++ in a root of its own: sync, plan (upgrade, downgrade, rebuild, held, orphan,
	# broken recipe, risks, changelog), update (hooks, ABI rebuild, state)
	u="-r $root/up" hooks=$root/up/etc/burn/hooks
	echo "@ mkdir -p $root/up/etc/burn && cp $tmp/up/up.conf $root/up/etc/burn/burn.conf"
	echo "$u plan"
	echo "$u sync"
	echo "$u sync"
	echo "$u -y install up-lib up-app up-held up-feat up-gone up-broken up-down"
	echo "$u hold up-held"
	echo "$u plan"
	echo "$u --no-sync update"
	echo "@ git -C $tmp/uprepo.git update-ref refs/heads/main refs/heads/next"
	echo "$u plan"
	echo "$u sync"
	echo "$u plan"
	echo "$u update"
	echo "@ mkdir -p $hooks/pre-update.d && cp $tmp/up/pre-fail $hooks/pre-update.d/10-snapshot"
	echo "$u --no-sync -y update"
	echo "@ cp $tmp/up/pre-ok $hooks/pre-update.d/10-snapshot && mkdir -p $hooks/post-update.d && cp $tmp/up/post-ok $hooks/post-update.d/10-ok && cp $tmp/up/post-fail $hooks/post-update.d/20-fail && cp $tmp/up/not-executable $hooks/post-update.d/30-not-executable"
	echo "$u --no-sync -y update"
	echo "$u history"
	echo "$u list"
	echo "$u plan"
	echo "@ git -C $tmp/uprepo.git update-ref refs/heads/main refs/heads/next2"
	echo "$u sync"
	echo "$u plan"
	echo "$u -y upgrade"
	echo "$u doctor"
	echo "@ mkdir -p $root/up2/etc/burn && cp $tmp/up/dead.conf $root/up2/etc/burn/burn.conf"
	echo "-r $root/up2 sync"
	echo "-r $root/up2 update"
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
	mv "$out.cache" "$tmp/cache"
	set +e
	env BURN_PROFILE_DIR="$profiles" BURN_CACHE="$tmp/cache" HOME="$tmp" NO_COLOR=1 LC_ALL=C \
		"$tool" -r "$root" -R "$recipes" -R "$tmp/fixtures" "$@" >"$out.out" 2>"$out.err" </dev/null
	echo "exit $?" >>"$out.err"
	set -e
	mv "$root" "$out.root"
	mv "$tmp/cache" "$out.cache"
}

# The root as text: every path with its type and mode, and for anything but a directory the
# link count, the symlink target and the time (from the package, or "now"), then the content of
# regular files (history dates as DATE).
snapshot() {
	(
		cd "$1" || exit 1
		find . -path ./var/lib/burn/lock -prune -o -type d -printf '%p d %m\n' -o -printf '%p %y %m %n %T@ %l\n' |
			awk '$2 != "d" { split($5, t, "."); $5 = t[1] > 1700000000 ? "now" : t[1] } { print }' | LC_ALL=C sort
		find . -type f ! -path ./var/lib/burn/lock | LC_ALL=C sort | while IFS= read -r f; do
			echo "== $f"
			case $f in
				*/history) sed 's/^[0-9-]*T[0-9:]*Z|/DATE|/' "$f" ;;
				*/last-update) echo DATE ;;
				*) cat "$f" ;;
			esac
			echo
		done
	)
}

# A build log, or its tail in a failure message: the version and the date in its first line
# differ, and so do the times cmake reports.
normalize_log() {
	sed 's/^# burn [^ :]*: \(.*\), [^,]*$/# burn: \1, DATE/; s/^\(-- [A-Za-z]* done\) ([0-9.]*s)$/\1/' "$@"
}

# The cache as text: every path with its type (but the temporary work directory and the insides
# of git repositories), the contents of downloaded sources and of build logs, the checksum of
# packages.
snapshot_cache() {
	(
		cd "$1" || exit 1
		find . -path ./work -prune -o -name .git -prune -o -printf '%p %y\n' | LC_ALL=C sort
		find ./sources ./logs -type f 2>/dev/null | LC_ALL=C sort | while IFS= read -r f; do
			echo "== $f"
			normalize_log "$f"
		done
		find ./packages -type f 2>/dev/null | LC_ALL=C sort | while IFS= read -r f; do
			echo "== $f $(sha256sum <"$f")"
		done
	)
}

cp -a "$root" "$tmp/want.root"
mv "$root" "$tmp/got.root"
cp -a "$tmp/cache" "$tmp/want.cache"
mv "$tmp/cache" "$tmp/got.cache"
n=0 fail=0
while IFS= read -r line; do
	n=$((n + 1))
	case $line in
		"@ "*)
			for s in want got; do
				mv "$tmp/$s.root" "$root"
				mv "$tmp/$s.cache" "$tmp/cache"
				ROOT=$root CACHE=$tmp/cache sh -c "${line#@ }"
				mv "$root" "$tmp/$s.root"
				mv "$tmp/cache" "$tmp/$s.cache"
			done
			continue
			;;
	esac
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
	snapshot_cache "$tmp/want.cache" >"$tmp/want.ctree"
	snapshot_cache "$tmp/got.cache" >"$tmp/got.ctree"
	if ! cmp -s "$tmp/want.ctree" "$tmp/got.ctree"; then
		fail=$((fail + 1))
		echo "FAIL (cache): burn $line   (- prototype, + C)"
		diff -u "$tmp/want.ctree" "$tmp/got.ctree" | sed '1,2d; s/^/    /' | head -n 30
		rm -rf "$tmp/got.cache"
		cp -a "$tmp/want.cache" "$tmp/got.cache"
	fi
	# build logs in stderr: see normalize_log;
	# lint: the prototype reports file problems in directory order, burn in C sorts them;
	# history: the dates of today's transactions depend on when each version ran
	for f in want got; do normalize_log "$tmp/$f.err" >"$tmp/$f.tmp" && mv "$tmp/$f.tmp" "$tmp/$f.err"; done
	case $line in
		lint*) for f in want got; do sort "$tmp/$f.out" -o "$tmp/$f.out"; done ;;
		history* | *" history"*)
			today=$(date -u +%Y-%m-%d)
			for f in want got; do sed "s/^${today}T[0-9:]*Z /DATE /" "$tmp/$f.out" >"$tmp/$f.tmp" && mv "$tmp/$f.tmp" "$tmp/$f.out"; done
			;;
	esac
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
