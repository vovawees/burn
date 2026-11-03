#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/tar.sh TARLS [TARW]: the C reader against Python's tarfile on archives GNU tar writes in
# every format it has (gnu, ustar, pax), with long names, long link targets, hard links and odd
# modes; the C writer against GNU tar, byte for byte.
set -eu
tarls=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
tarw=${2:+$(cd "$(dirname "$2")" && pwd)/$(basename "$2")}
command -v python3 >/dev/null || {
	echo "tar: python3 not found, skipped"
	exit 0
}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
d=$tmp/tree
long=$(printf 'n%.0s' $(seq 1 120))
deep=$(printf 'dir%s/' $(seq 1 30))
mkdir -p "$d/usr/bin" "$d/usr/lib" "$d/empty" "$d/$deep" "$d/ro"
printf '#!/bin/sh\necho hi\n' >"$d/usr/bin/hello"
chmod 4755 "$d/usr/bin/hello"
ln "$d/usr/bin/hello" "$d/usr/bin/hello-hard"
ln -s hello "$d/usr/bin/hi"
ln -s "../../$long/$long" "$d/usr/lib/longlink"
head -c 100000 /dev/urandom >"$d/usr/lib/libx.so.1.0"
ln -s libx.so.1.0 "$d/usr/lib/libx.so.1"
: >"$d/usr/lib/empty-file"
printf 'deep\n' >"$d/${deep}file"
mkdir -p "$d/$long"
printf 'long\n' >"$d/$long/$long"
printf 'x' >"$d/ro/file"
chmod 0444 "$d/ro/file"
chmod 0555 "$d/ro"
mkfifo "$d/fifo"
n=0
for fmt in gnu ustar posix; do
	f=$tmp/t-$fmt.tar.zst
	if [ $fmt = ustar ]; then
		# ustar has no long names: only what fits
		(cd "$d" && tar --format=ustar --sort=name --numeric-owner --owner=0 --group=0 --mtime=@1700000000 \
			--exclude=./usr/lib/longlink -cf - ./usr ./empty ./ro ./fifo) | zstd -q >"$f"
	else
		(cd "$d" && tar --format=$fmt --sort=name --numeric-owner --owner=0 --group=0 --mtime=@1700000000 \
			-cf - .) | zstd -q >"$f"
	fi
	python3 - "$f" >"$tmp/want" <<'PY'
import hashlib, subprocess, sys, tarfile, io
data = subprocess.run(["zstd", "-dcq", sys.argv[1]], check=True, stdout=subprocess.PIPE).stdout
types = {tarfile.REGTYPE: "0", tarfile.AREGTYPE: "0", tarfile.LNKTYPE: "1", tarfile.SYMTYPE: "2",
         tarfile.CHRTYPE: "3", tarfile.BLKTYPE: "4", tarfile.DIRTYPE: "5", tarfile.FIFOTYPE: "6"}
def norm(p):
    return "/".join(c for c in p.split("/") if c not in ("", "."))
with tarfile.open(fileobj=io.BytesIO(data)) as t:
    for m in t:
        h = "-"
        if m.isreg():
            h = hashlib.sha256(t.extractfile(m).read()).hexdigest()
        link = norm(m.linkname) if m.islnk() else m.linkname
        size = m.size if m.isreg() else 0
        print(f"{types[m.type]} {m.mode:04o} {m.uid} {m.gid} {size} {int(m.mtime)} {h} {norm(m.name)} -> {link}")
PY
	"$tarls" "$f" >"$tmp/got"
	if ! cmp -s "$tmp/want" "$tmp/got"; then
		echo "FAIL: format $fmt (- tarfile, + burn)"
		diff -u "$tmp/want" "$tmp/got" | head -n 20
		exit 1
	fi
	n=$((n + $(wc -l <"$tmp/got")))
done
# the writer: the tar stream is what GNU tar writes for the same tree, byte for byte (sockets
# skipped, mtimes newer than the epoch clamped, older ones kept); zstd checks the frame checksum
if [ -n "$tarw" ]; then
	touch -d @1600000000 "$d/usr/lib/libx.so.1.0"
	touch -h -d @1600000000 "$d/usr/bin/hi"
	python3 -c 'import socket, sys; socket.socket(socket.AF_UNIX).bind(sys.argv[1])' "$d/sock"
	(cd "$d" && LC_ALL=C tar --format=gnu --sort=name --numeric-owner --owner=0 --group=0 \
		--mtime=@1700000000 --clamp-mtime -cf - . 2>/dev/null) >"$tmp/gnu.tar"
	"$tarw" "$d" "$tmp/w.tar.zst" 1700000000
	zstd -dcq "$tmp/w.tar.zst" >"$tmp/w.tar"
	if ! cmp -s "$tmp/gnu.tar" "$tmp/w.tar"; then
		echo "FAIL: the writer differs from GNU tar"
		cmp "$tmp/gnu.tar" "$tmp/w.tar" || true
		exit 1
	fi
	"$tarls" "$tmp/w.tar.zst" >"$tmp/got"
	if ! grep -q ' 1600000000 - usr/bin/hi -> hello$' "$tmp/got" || grep -q sock "$tmp/got" || [ -e "$tmp/w.tar.zst.part" ]; then
		echo "FAIL: writer: old mtimes, sockets or the .part file"
		exit 1
	fi
	if "$tarw" "$tmp/missing" "$tmp/m.tar.zst" 0 2>/dev/null || [ -e "$tmp/m.tar.zst" ] || [ -e "$tmp/m.tar.zst.part" ]; then
		echo "FAIL: writer: a failure must leave nothing behind"
		exit 1
	fi
	n=$((n + $(wc -l <"$tmp/got")))
fi
# damaged archives are errors, never a partial success
f=$tmp/t-gnu.tar.zst
head -c 2000 "$f" >"$tmp/cut.tar.zst"
if "$tarls" "$tmp/cut.tar.zst" >/dev/null 2>&1; then
	echo "FAIL: a truncated archive must fail"
	exit 1
fi
printf 'not zstd' >"$tmp/plain.tar.zst"
if "$tarls" "$tmp/plain.tar.zst" >/dev/null 2>&1; then
	echo "FAIL: a file that is not zstd must fail"
	exit 1
fi
for evil in ../evil usr/../../evil; do
	python3 - "$tmp/x.tar" "$evil" <<'PY'
import io, sys, tarfile
with tarfile.open(sys.argv[1], "w", format=tarfile.GNU_FORMAT) as t:
    i = tarfile.TarInfo(sys.argv[2])
    i.size = 4
    t.addfile(i, io.BytesIO(b"evil"))
PY
	zstd -qf <"$tmp/x.tar" >"$tmp/evil.tar.zst"
	if "$tarls" "$tmp/evil.tar.zst" >/dev/null 2>"$tmp/err" || ! grep -q 'unsafe path' "$tmp/err"; then
		echo "FAIL: $evil must be refused"
		exit 1
	fi
done
echo "tar: $n entries in 3 formats${tarw:+ and written}, OK"
