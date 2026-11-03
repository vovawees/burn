#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/elf.sh ELFSCAN: SONAME/NEEDED read in process against readelf -d, on what cc builds and on
# hand made files of every class and byte order (with section headers and with segments only);
# LTO bytecode in static libraries against readelf -S.
set -eu
elfscan=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
for t in cc ar readelf python3; do
	command -v $t >/dev/null || {
		echo "elf: $t not found, skipped"
		exit 0
	}
done
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"
printf 'int f(void) { return 1; }\n' >f.c
printf 'int g(void) { return 2; }\n' >g.c
printf 'int f(void);\nint main(void) { return f() - 1; }\n' >main.c
cc -shared -fPIC -Wl,-soname,libf.so.1 -o libf.so.1 f.c
cc -o prog main.c -L. -l:libf.so.1 -lm -Wl,--no-as-needed -lm
cc -c -o g.o g.c
ar rcs libplain.a g.o
cp g.o a-member-with-a-long-name.o
if cc -flto -c -o lto.o f.c 2>/dev/null && cc -flto -c -o a-long-named-lto-member.o g.c 2>/dev/null; then
	ar rcs liblto.a lto.o
	ar rcs libmixed.a a-member-with-a-long-name.o a-long-named-lto-member.o
fi
python3 - <<'PY'
import struct
strtab = b"\0libbe.so.2\0libc.so.6\0libm.so.6\0"
idx = lambda s: strtab.index(s)
shstr = b"\0.dynstr\0.dynamic\0.shstrtab\0"
VA = 0x10000
for bits in (32, 64):
    for be in (False, True):
        for sect in (True, False):
            e = ">" if be else "<"
            w = "Q" if bits == 64 else "I"
            sw = "q" if bits == 64 else "i"
            ehsize, phent, shent = (64, 56, 64) if bits == 64 else (52, 32, 40)
            off = ehsize + 2 * phent
            stroff = off
            off = (off + len(strtab) + 7) & ~7
            dyn = b"".join(struct.pack(e + sw + w, t, v) for t, v in
                           [(1, idx(b"libc.so.6")), (14, idx(b"libbe.so.2")), (1, idx(b"libm.so.6")),
                            (5, VA + stroff), (10, len(strtab)), (0, 0)])
            dynoff = off
            off += len(dyn)
            shstroff = off
            off = (off + len(shstr) + 7) & ~7
            shoff = off if sect else 0
            shnum = 4 if sect else 0
            total = off + (shnum * shent)
            ident = b"\x7fELF" + bytes([2 if bits == 64 else 1, 2 if be else 1, 1]) + bytes(9)
            if bits == 64:
                eh = ident + struct.pack(e + "HHIQQQIHHHHHH", 3, 62, 1, 0, ehsize, shoff, 0, ehsize, phent, 2, shent, shnum, 3 if sect else 0)
                ph = struct.pack(e + "IIQQQQQQ", 1, 4, 0, VA, VA, total, total, 0x1000)
                ph += struct.pack(e + "IIQQQQQQ", 2, 4, dynoff, VA + dynoff, VA + dynoff, len(dyn), len(dyn), 8)
            else:
                eh = ident + struct.pack(e + "HHIIIIIHHHHHH", 3, 3, 1, 0, ehsize, shoff, 0, ehsize, phent, 2, shent, shnum, 3 if sect else 0)
                ph = struct.pack(e + "IIIIIIII", 1, 0, VA, VA, total, total, 4, 0x1000)
                ph += struct.pack(e + "IIIIIIII", 2, dynoff, VA + dynoff, VA + dynoff, len(dyn), len(dyn), 4, 4)
            img = bytearray(total)
            img[0:len(eh)] = eh
            img[ehsize:ehsize + len(ph)] = ph
            img[stroff:stroff + len(strtab)] = strtab
            img[dynoff:dynoff + len(dyn)] = dyn
            img[shstroff:shstroff + len(shstr)] = shstr
            if sect:
                def sh(name, typ, o, size, link, entsize):
                    if bits == 64:
                        return struct.pack(e + "IIQQQQIIQQ", name, typ, 0, 0, o, size, link, 0, 1, entsize)
                    return struct.pack(e + "IIIIIIIIII", name, typ, 0, 0, o, size, link, 0, 1, entsize)
                hdrs = bytes(shent)
                hdrs += sh(shstr.index(b".dynstr"), 3, stroff, len(strtab), 0, 0)
                hdrs += sh(shstr.index(b".dynamic"), 6, dynoff, len(dyn), 1, len(dyn) // 6)
                hdrs += sh(shstr.index(b".shstrtab"), 3, shstroff, len(shstr), 0, 0)
                img[shoff:shoff + len(hdrs)] = hdrs
            name = "elf%d%s%s" % (bits, "be" if be else "le", "" if sect else "-nosections")
            open(name, "wb").write(img)
PY
head -c 64 /dev/urandom >noise
{ printf '\177ELF'; cat noise; } >garbage
head -c 3000 libf.so.1 >truncated
printf '#!/bin/sh\n' >script
: >empty
files="libf.so.1 prog g.o libplain.a $(ls elf* liblto.a libmixed.a 2>/dev/null) garbage truncated script empty"
for f in $files; do
	if [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" != '177ELF' ]; then
		line="$f not-elf"
	else
		dyn=$(LC_ALL=C readelf -d "$f" 2>/dev/null || true)
		so=$(printf '%s\n' "$dyn" | sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p' | paste -sd, -)
		ne=$(printf '%s\n' "$dyn" | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p' | paste -sd, -)
		line="$f soname=$so needed=$ne"
	fi
	lto=no
	if LC_ALL=C readelf -S "$f" 2>/dev/null | grep -q '\.gnu\.lto_'; then lto=yes; fi
	echo "$line lto=$lto"
done >want
# shellcheck disable=SC2086
"$elfscan" $files >got
if ! cmp -s want got; then
	echo "FAIL: elf (- readelf, + burn)"
	diff -u want got
	exit 1
fi
grep -q '^prog soname= needed=libf.so.1,libm.so.6,libc.so.6 ' got || {
	echo "FAIL: elf: prog must need libf.so.1, libm.so.6 and libc.so.6"
	exit 1
}
echo "elf: $(wc -l <got) files, OK"
