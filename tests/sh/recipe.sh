# a realistic recipe
name=example
version=2.4.1
_major=${version%%.*}
_minor=${version#*.}
release=3
summary="An example: \"quotes\", \$dollars and 'apostrophes'"
homepage=https://example.org/$name/
license=(GPL-2.0-or-later 'LGPL-2.1-or-later')
category=extra
depends=(glibc zlib-ng 'openssl:+tls' 'gtk:!headless')
makedepends=(meson pkgconf)
features=(+nls -docs tls)
source=(
	https://example.org/releases/$name-$version.tar.xz
	"$name-fix.patch"
	renamed.tar.gz::https://example.org/archive/v$version.tar.gz
)
sha256=(
	0000000000000000000000000000000000000000000000000000000000000000
	SKIP
)
patches=("$name-fix.patch")
build_type=meson
meson_args=(-Ddocs=disabled -Dexamples=false "-Dname=$name $version")
srcdir=$name-${version}
backup=(etc/$name.conf)
options=(!lto o3)
cflags_append='-fno-strict-aliasing'

prepare_extra() {
	sed -i 's/a/b/' meson.build
}

do_check() { meson test -C build; }
