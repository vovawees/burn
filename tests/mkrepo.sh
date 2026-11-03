#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/mkrepo.sh REPO: a bare recipe repository for sync, plan and update, with fixed commit
# dates (the same hashes every time). main holds the first versions; next and next2 hold what
# comes later (tests move main there).
set -eu
repo=$1 work=$1.work
rm -rf "$repo" "$work"
mkdir -p "$work/packages"
g() {
	GIT_AUTHOR_DATE='1700000000 +0000' GIT_COMMITTER_DATE='1700000000 +0000' \
		git -C "$work" -c user.name=burn -c user.email=burn@example.org -c init.defaultBranch=main "$@"
}
# recipe NAME VERSION [BODY]: a custom recipe that installs a file with its version
recipe() {
	mkdir -p "$work/packages/$1"
	{
		printf 'name=%s\nversion=%s\nrelease=1\nsummary="update test: %s"\n' "$1" "$2" "$1"
		printf 'homepage=https://example.org/%s\nlicense=(MIT)\ncategory=extra\nbuild_type=custom\n' "$1"
		if [ -n "${3-}" ]; then
			printf '%s\n' "$3"
		else
			# shellcheck disable=SC2016 # recipe code
			printf '\ndo_install() {\n\techo "$name $version" | install -Dm644 /dev/stdin "$DESTDIR/usr/share/$name/version"\n}\n'
		fi
	} >"$work/packages/$1/recipe"
}
lib() { # SOVER
	cat <<LIB
sover=$1

do_build() {
	printf 'int uplib(void) { return %s; }\n' "\$sover" >uplib.c
	\$CC \$CFLAGS -fPIC -shared -Wl,-soname,libuplib.so.\$sover \$LDFLAGS -o libuplib.so.\$sover uplib.c
}

do_install() {
	install -Dm755 libuplib.so.\$sover "\$DESTDIR/usr/lib/libuplib.so.\$sover"
	ln -s libuplib.so.\$sover "\$DESTDIR/usr/lib/libuplib.so"
}
LIB
}
# shellcheck disable=SC2016 # recipe code
app='depends=(up-lib)

do_build() {
	printf "int uplib(void);\nint main(void) { return uplib() == 0; }\n" >app.c
	$CC $CFLAGS $LDFLAGS -o up-app app.c -luplib
}

do_install() {
	install -Dm755 up-app "$DESTDIR/usr/bin/up-app"
}'
g init -q
recipe up-lib 1.0 "$(lib 1)"
recipe up-app 1.0 "$app"
for p in up-held up-feat up-gone up-broken; do recipe "$p" 1.0; done
recipe up-down 2.0
g add -A
g commit -q -m "first versions"
g checkout -q -b next
recipe up-lib 2.0 "$(lib 2)"
recipe up-held 1.1
printf '# the recipe changed, the version did not\n' >>"$work/packages/up-feat/recipe"
g rm -q -r packages/up-gone
sed -i 's/^version=.*/version=/' "$work/packages/up-broken/recipe"
recipe up-down 1.5
g add -A
GIT_COMMITTER_DATE='1700086400 +0000' g commit -q -m "up-lib 2.0, up-held 1.1, up-down 1.5"
g checkout -q -b next2
recipe up-held 1.2
g add -A
GIT_COMMITTER_DATE='1700172800 +0000' g commit -q -m "up-held 1.2"
g checkout -q main
git clone -q --bare "$work" "$repo"
rm -rf "$work"
