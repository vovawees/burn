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
# VIDEO_CARDS switches the video_cards_CARD features of a recipe; the overrides still apply
[ "$(b -p deck query bt-hw features)" = "+video_cards_amd -video_cards_intel" ] || fail "video_cards features"
[ "$(b query bt-hw features)" = "-video_cards_amd -video_cards_intel" ] || fail "video_cards: no cards"
[ "$(b -p deck -f +video_cards_intel query bt-hw features)" = "+video_cards_amd +video_cards_intel" ] ||
	fail "video_cards: -f"
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
# isolation of the phases: an own network namespace with only lo (up); ISOLATE=0 is the host's
iso() {
	env BURN_PROFILE_DIR="$tmp/profiles" BURN_CACHE="$tmp/isocache$1" BURN_CONFIG="$tmp/iso$1.conf" NO_COLOR=1 \
		"$BURN" -r "$tmp/isoroot" -R "$fixtures" --host-deps -y build bt-iso >"$tmp/iso.log" 2>&1 ||
		fail "build bt-iso: $(cat "$tmp/iso.log")"
	sed -n 's/^\(netns\|ifaces\|lo\|net\)=//p' "$tmp/isocache$1/logs/bt-iso-1.0-1.log"
}
: >"$tmp/iso1.conf"
echo ISOLATE=0 >"$tmp/iso0.conf"
host_ns=$(readlink /proc/self/ns/net)
if [ "$(iso 0 | head -n 1)" != "$host_ns" ]; then fail "ISOLATE=0: the host's network"; fi
if grep -q 'build isolation unavailable' "$tmp/iso.log"; then
	echo "cli: no user and network namespaces here, isolation not tested" >&2
else
	iso 1 >"$tmp/iso.out"
	[ "$(sed -n 1p "$tmp/iso.out")" != "$host_ns" ] || fail "isolation: own network namespace"
	[ "$(sed -n 2p "$tmp/iso.out")" = "lo " ] || fail "isolation: only lo"
	sed -n 3p "$tmp/iso.out" | grep -q 'Connection refused' || fail "isolation: lo is up"
	sed -n 4p "$tmp/iso.out" | grep -q 'Network is unreachable' || fail "isolation: no route out"
fi
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
# spins: a directory (or git repository) with spin.conf and its own recipes; `spin add` brings
# its features, packages and services, `update` what a newer spin.conf asks for, `remove` takes
# all of it back
sp() { env BURN_PROFILE_DIR="$tmp/profiles" BURN_CACHE="$tmp/spcache" HOST_VIDEO_CARDS= NO_COLOR=1 "$BURN" \
	-r "$tmp/sproot" -R "$fixtures" --host-deps "$@"; }
spin_recipe() { # NAME [DEPENDS]
	mkdir -p "$tmp/myspin/packages/$1"
	# shellcheck disable=SC2016 # the recipe expands these
	printf '%s\n' "name=$1" 'version=1' 'release=1' "summary=\"$1 of the test spin\"" \
		"homepage=https://example.org/$1" 'license=(MIT)' 'category=extra' 'build_type=custom' \
		"depends=(${2:-})" 'do_install() {' "	mkdir -p \"\$DESTDIR/usr/share/$1\"" \
		"	echo $1 >\"\$DESTDIR/usr/share/$1/file\"" '}' >"$tmp/myspin/packages/$1/recipe"
}
spin_recipe spinlib
spin_recipe spinapp spinlib
mkdir -p "$tmp/sproot/usr/lib/dinit.d/user"
printf 'type = process\ncommand = /usr/bin/true\n' >"$tmp/sproot/usr/lib/dinit.d/spind"
printf 'type = process\ncommand = /usr/bin/true\n' >"$tmp/sproot/usr/lib/dinit.d/user/spinu"
printf '%s\n' 'SPIN_NAME=myspin' 'SPIN_DESC="Test spin"' 'SPIN_PACKAGES=(spinapp)' 'SPIN_FEATURES="+docs"' \
	'SPIN_SERVICES=(spind nosuchsvc)' 'SPIN_USER_SERVICES=(spinu)' >"$tmp/myspin/spin.conf"
[ -z "$(sp spin list 2>/dev/null)" ] || fail "spin list: none yet"
sp -y spin add "$tmp/myspin" 2>"$tmp/err" || fail "spin add: $(cat "$tmp/err")"
[ "$(sp spin list | awk '{print $1}')" = myspin ] || fail "spin list"
[ "$(cat "$tmp/sproot/usr/share/spinapp/file")" = spinapp ] || fail "spin add: the package"
[ -f "$tmp/sproot/usr/share/spinlib/file" ] || fail "spin add: its dependency"
[ "$(readlink "$tmp/sproot/etc/dinit.d/boot.d/spind")" = /usr/lib/dinit.d/spind ] || fail "spin add: service"
[ -L "$tmp/sproot/etc/dinit.d/user/boot.d/spinu" ] || fail "spin add: user service"
grep -q 'no service nosuchsvc in the root' "$tmp/err" || fail "spin add: missing service warning"
[ "$(sp query good features)" = "+docs +tls" ] || fail "spin features"
sp profile show | grep -q '^features .* +docs' || fail "profile show: the features of spins"
sp spin show myspin | grep -q '^brought  *spinapp spinlib$' || fail "spin show: $(sp spin show myspin)"
if sp -y spin add "$tmp/myspin" 2>"$tmp/err"; then fail "spin add twice"; fi
grep -q 'spin myspin is added already' "$tmp/err" || fail "spin add twice: message"
printf 'SPIN_PACKAGES=(nosuchpkg)\n' >"$tmp/badspin.conf"
mkdir -p "$tmp/badspin" && mv "$tmp/badspin.conf" "$tmp/badspin/spin.conf"
if sp -y spin add "$tmp/badspin" 2>"$tmp/err"; then fail "spin add: unknown package"; fi
grep -q 'spin badspin: no recipe for nosuchpkg' "$tmp/err" || fail "unknown package message: $(cat "$tmp/err")"
# SPIN_BASE: the lowest level of the base (CLEANLINUX_BASE of os-release) the spin works on
mkdir -p "$tmp/newspin" "$tmp/sproot/usr/lib"
printf 'SPIN_BASE=2\nSPIN_PACKAGES=(spinlib)\n' >"$tmp/newspin/spin.conf"
printf 'ID=cleanlinux\nCLEANLINUX_BASE=1\n' >"$tmp/sproot/usr/lib/os-release"
if sp -y spin add "$tmp/newspin" 2>"$tmp/err"; then fail "spin add: SPIN_BASE above the base"; fi
grep -q 'spin newspin needs CleanLinux base 2, this root has 1' "$tmp/err" || fail "SPIN_BASE message: $(cat "$tmp/err")"
[ ! -e "$tmp/sproot/var/lib/burn/spins/newspin" ] || fail "SPIN_BASE: no spin left behind"
[ ! -e "$tmp/sproot/var/lib/burn/spins/badspin" ] || fail "a failed add leaves no spin behind"
# a newer spin.conf: plan shows the new package, update installs it
spin_recipe spinextra
sed -i 's/^SPIN_PACKAGES=.*/SPIN_PACKAGES=(spinapp spinextra)/' "$tmp/myspin/spin.conf"
sp plan 2>&1 | grep -q 'spinextra .*new: spin myspin' || fail "plan: the new package of a spin"
sp -y --no-sync update >/dev/null 2>"$tmp/err" || fail "update: $(cat "$tmp/err")"
[ -f "$tmp/sproot/usr/share/spinextra/file" ] || fail "update: the new package of a spin"
# a package of the user's own that needs spinlib keeps it
spin_recipe mine spinlib
mv "$tmp/myspin/packages/mine" "$tmp/minepkg"
mkdir -p "$tmp/mine" && mv "$tmp/minepkg" "$tmp/mine/mine"
sp -R "$tmp/mine" -y install mine >/dev/null 2>&1 || fail "install mine"
sp -y spin remove myspin 2>"$tmp/err" || fail "spin remove: $(cat "$tmp/err")"
grep -q 'keeping spinlib: mine needs it' "$tmp/err" || fail "spin remove: keeps what others need"
[ ! -e "$tmp/sproot/usr/share/spinapp" ] && [ ! -e "$tmp/sproot/usr/share/spinextra" ] || fail "spin remove: packages"
[ -f "$tmp/sproot/usr/share/spinlib/file" ] || fail "spin remove: spinlib stays"
[ ! -e "$tmp/sproot/etc/dinit.d/boot.d/spind" ] && [ ! -e "$tmp/sproot/etc/dinit.d/user/boot.d/spinu" ] ||
	fail "spin remove: services"
[ "$(sp query good features)" = "-docs +tls" ] || fail "spin remove: features"
[ -z "$(sp spin list 2>/dev/null)" ] || fail "spin remove: list"
if sp -y spin remove myspin 2>/dev/null; then fail "spin remove: unknown spin"; fi
# a git spin: cloned under its repository name, pulled by sync; what was installed before stays
if command -v git >/dev/null; then
	g=$tmp/gitspin.git
	mkdir -p "$g/packages" && cp -r "$tmp/myspin/packages/spinlib" "$g/packages/"
	printf 'SPIN_DESC="Git spin"\nSPIN_PACKAGES=(spinlib)\n' >"$g/spin.conf"
	gc() { git -C "$g" -c user.name=t -c user.email=t@example.org "$@"; }
	gc init -q && gc add -A && gc commit -qm one
	sp -y spin add "file://$g" 2>"$tmp/err" || fail "spin add git: $(cat "$tmp/err")"
	sp spin list | grep -q '^gitspin  *[0-9a-f]' || fail "git spin list: $(sp spin list)"
	printf 'RECIPE_REPOS=()\n' >"$tmp/norepos.conf"
	echo '# two' >>"$g/spin.conf" && gc commit -qam two
	sp -c "$tmp/norepos.conf" sync 2>"$tmp/err" || fail "sync git spin: $(cat "$tmp/err")"
	grep -q 'Syncing spin gitspin' "$tmp/err" || fail "sync: spins"
	grep -q '# two' "$tmp/sproot/var/lib/burn/spins/gitspin/spin.conf" || fail "sync: pulled"
	sp -y spin remove gitspin 2>/dev/null || fail "spin remove git"
	[ ! -e "$tmp/sproot/var/lib/burn/spins/gitspin" ] || fail "spin remove: the clone"
	[ -f "$tmp/sproot/usr/share/spinlib/file" ] || fail "spin remove: what was installed before stays"
fi
# split packages: one build makes every package of the recipe; each installs on its own, the
# files of a split belong to it, and files move between the packages of one recipe on update
spl() { env BURN_PROFILE_DIR="$tmp/profiles" BURN_CACHE="$tmp/splcache" HOST_VIDEO_CARDS= NO_COLOR=1 "$BURN" \
	-r "$tmp/splroot" -R "$tmp/splrec" --host-deps "$@"; }
sdb=$tmp/splroot/var/lib/burn/installed
mkdir -p "$tmp/splrec" && cp -r "$fixtures/bt-split" "$tmp/splrec/"
spl lint bt-split >/dev/null 2>&1 || fail "split: lint"
spl info bt-split-data | grep -q '^split of  *bt-split$' || fail "split: info of a split"
spl info bt-split | grep -q '^split  *bt-split-libs bt-split-data$' || fail "split: info of the recipe"
spl search 'data files' | grep -q '^bt-split-data 1\.0-1 ' || fail "split: search"
[ "$(spl query bt-split-data license)" = CC0-1.0 ] || fail "split: own license"
[ "$(spl query bt-split-libs license)" = MIT ] || fail "split: the license of the recipe"
spl -y install bt-split-libs >"$tmp/spl.log" 2>&1 || fail "split: install bt-split-libs: $(cat "$tmp/spl.log")"
[ "$(find "$tmp/splcache/packages" -name 'bt-split*' | wc -l)" -eq 3 ] || fail "split: one build, three packages"
grep -q 'split bt-split-libs: nothing matches usr/lib/nothing-here.so.\*' "$tmp/spl.log" || fail "split: no-match warning"
[ "$(spl list | awk '{print $1}' | tr '\n' ' ')" = "bt-split-libs " ] || fail "split: only the library"
[ -f "$tmp/splroot/usr/lib/libbtsplit.so.1" ] && [ ! -e "$tmp/splroot/usr/bin/btsplit" ] || fail "split: files"
grep -qx 'libbtsplit.so.1' "$sdb/bt-split-libs/provides-so" || fail "split: provides-so"
grep -qx 'base=bt-split' "$sdb/bt-split-libs/meta" || fail "split: base= in the metadata"
spl -y install bt-split-data >"$tmp/spl.log" 2>&1 || fail "split: install bt-split-data"
[ "$(grep -c '^==> Building' "$tmp/spl.log")" -eq 0 ] || fail "split: the build is reused: $(cat "$tmp/spl.log")"
[ "$(spl list | awk '{print $1}' | tr '\n' ' ')" = "bt-split bt-split-data bt-split-libs " ] || fail "split: dependencies"
grep -qx 'libbtsplit.so.1' "$sdb/bt-split/needs-so" || fail "split: needs-so of the main package"
if grep -q 'libbtsplit.so.1$' "$sdb/bt-split/files"; then fail "split: the library left the main package"; fi
grep -qx 'usr/share/bt-split/levels/1.txt' "$sdb/bt-split-data/files" || fail "split: a directory"
[ ! -e "$tmp/splroot/usr/share/bt-split" ] || [ -f "$tmp/splroot/usr/share/bt-split/levels/1.txt" ] || fail "split: data"
spl doctor >/dev/null 2>&1 || fail "split: doctor"
if spl -y remove bt-split-libs 2>"$tmp/err"; then fail "split: remove a split others need"; fi
grep -q 'bt-split-libs is required by: bt-split' "$tmp/err" || fail "split: required by"
# a new release: plan lists every installed package of the recipe, update builds it once; the
# development link moves from bt-split to bt-split-libs and stays on disk
sed -i -e 's/^release=1$/release=2/' -e "s|^files_bt_split_libs=(|files_bt_split_libs=(usr/lib/libbtsplit.so |" \
	"$tmp/splrec/bt-split/recipe"
[ "$(spl plan 2>&1 | grep -c ' upgrade')" -eq 3 ] || fail "split: plan: $(spl plan 2>&1)"
spl -y --no-sync update >"$tmp/spl.log" 2>&1 || fail "split: update: $(cat "$tmp/spl.log")"
[ "$(grep -c '^==> Building bt-split ' "$tmp/spl.log")" -eq 1 ] || fail "split: update builds once: $(cat "$tmp/spl.log")"
grep -q 'bt-split-libs takes over 1 file(s) from bt-split' "$tmp/spl.log" || fail "split: takeover: $(cat "$tmp/spl.log")"
[ -L "$tmp/splroot/usr/lib/libbtsplit.so" ] || fail "split: the moved file stays"
spl owner /usr/lib/libbtsplit.so | grep -q 'owned by bt-split-libs 1.0-2' || fail "split: new owner"
[ "$(spl list | grep -c '1.0-2')" -eq 3 ] || fail "split: all updated"
# a split no file matches, a split without files or summary
mkdir -p "$tmp/splbad/sb"
# shellcheck disable=SC2016 # the recipe expands these
printf '%s\n' 'name=sb' 'version=1' 'release=1' 'summary="bad splits"' 'homepage=https://example.org/sb' \
	'license=(MIT)' 'category=extra' 'build_type=custom' 'split=(sb-none sb-empty sb)' \
	'summary_sb_none="matches nothing"' "files_sb_none=(usr/nothing)" \
	'do_install() {' '	mkdir -p "$DESTDIR/usr/share/sb"' '	echo x >"$DESTDIR/usr/share/sb/x"' '}' \
	>"$tmp/splbad/sb/recipe"
spl -R "$tmp/splbad" lint sb >"$tmp/out" 2>/dev/null && fail "split: lint must fail"
grep -q "split 'sb' is the name of the recipe" "$tmp/out" || fail "split lint: own name"
grep -q "split 'sb-empty' has no files_sb_empty" "$tmp/out" || fail "split lint: files"
grep -q "split 'sb-empty' has no summary_sb_empty" "$tmp/out" || fail "split lint: summary"
sed -i 's/^split=.*/split=(sb-none)/' "$tmp/splbad/sb/recipe"
if spl -R "$tmp/splbad" build sb >/dev/null 2>"$tmp/err"; then fail "split: a split without files must fail"; fi
grep -q 'split sb-none: no file of the build matches files_sb_none' "$tmp/err" || fail "split: message: $(cat "$tmp/err")"
# a build outlives the file of its burn (burn installed a new burn, then builds more): the
# build tools from the host (--host-deps, cross): makedepends of the layers devel and core are not
# built, those of other layers (headers, data) go into the root, which is the sysroot of the build
mkdir -p "$tmp/hdrec"
hd_recipe() { # NAME CATEGORY [MAKEDEPENDS]
	mkdir -p "$tmp/hdrec/$1"
	# shellcheck disable=SC2016 # the recipe expands these
	printf '%s\n' "name=$1" 'version=1' 'release=1' "summary=\"$1\"" "homepage=https://example.org/$1" \
		'license=(MIT)' "category=$2" 'build_type=custom' "makedepends=(${3:-})" 'do_install() {' \
		"	mkdir -p \"\$DESTDIR/usr/share/$1\"" '}' >"$tmp/hdrec/$1/recipe"
}
hd_recipe hd-tool devel
hd_recipe hd-corelib core
hd_recipe hd-proto platform
hd_recipe hd-app extra 'hd-tool hd-corelib hd-proto'
hd() { env BURN_PROFILE_DIR="$tmp/profiles" BURN_CACHE="$tmp/hdcache" HOST_VIDEO_CARDS= NO_COLOR=1 "$BURN" \
	-r "$tmp/hdroot" -R "$tmp/hdrec" "$@"; }
hd --host-deps -y install hd-app >"$tmp/hd.log" 2>&1 || fail "host-deps: install: $(cat "$tmp/hd.log")"
[ "$(hd list | awk '{print $1}' | tr '\n' ' ')" = "hd-app hd-proto " ] || fail "host-deps: built: $(hd list)"
# a native build needs every makedepend in the root (they stay installed, as in Gentoo)
hd -y install hd-app >"$tmp/hd.log" 2>&1 || fail "native: install: $(cat "$tmp/hd.log")"
[ "$(hd list | awk '{print $1}' | tr '\n' ' ')" = "hd-app hd-corelib hd-proto hd-tool " ] || fail "native: every makedepend: $(hd list)"
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
