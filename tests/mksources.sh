#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/mksources.sh SRC RECIPES: the sources of the build fixtures (tarballs and a zip), packed
# reproducibly from SRC (tests/fixtures/src) into the recipe directories under RECIPES.
set -eu
src=$1 fx=$2
mktgz() { # DIR OUT
	(cd "$src" && LC_ALL=C tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@1700000000 \
		-cf - "$1") | gzip -n >"$2"
}
for p in bt-auto bt-areconf bt-noconf bt-cmake bt-meson; do mktgz "$p-1.0" "$fx/$p/$p-1.0.tar.gz"; done
mktgz extras-1.0 "$fx/bt-auto/extras-1.0.tar.gz"
mktgz extras-1.0 "$fx/bt-custom/data.tar.gz"
python3 - "$src" bt-make-1.0 "$fx/bt-make/bt-make-1.0.zip" <<'PY'
import os, sys, zipfile
src, top, out = sys.argv[1:]
with zipfile.ZipFile(out, "w") as z:
    for d, dirs, files in sorted(os.walk(os.path.join(src, top))):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(d, f)
            i = zipfile.ZipInfo(os.path.relpath(p, src), (2023, 11, 14, 22, 13, 20))
            i.external_attr = (os.stat(p).st_mode & 0o777 | 0o100000) << 16
            i.compress_type = zipfile.ZIP_DEFLATED
            with open(p, "rb") as fh:
                z.writestr(i, fh.read())
PY
