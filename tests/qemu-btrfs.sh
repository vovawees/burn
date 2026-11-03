#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/qemu-btrfs.sh KERNEL BURN: tests/btrfs.sh inside a small virtual machine, for hosts whose
# kernel has no btrfs (containers, sandboxes). KERNEL is a bzImage built from tinyconfig plus
# tests/qemu-kernel.config. The initramfs takes bash, a few coreutils, mount and burn from the
# host with the libraries ldd lists; the disk is a fresh mkfs.btrfs image. No root needed.
set -eu
kernel=$1 burn=$2
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/root/bin" "$tmp/root/proc" "$tmp/root/sys" "$tmp/root/mnt" "$tmp/root/tmp" "$tmp/root/tests"
for p in bash cat grep ls mkdir mount rm sed touch; do
	f=$(command -v "$p")
	if [ "$(head -c 2 "$f")" = '#!' ]; then
		# coreutils-single: "#!/usr/bin/coreutils --coreutils-prog-shebang=rm"
		interp=$(sed -n '1s/^#![[:space:]]*\([^[:space:]]*\).*/\1/p' "$f")
		[ "${interp##*/}" = coreutils ] || { echo "qemu-btrfs: cannot take $f"; exit 1; }
		cp -L "$interp" "$tmp/root/bin/coreutils"
		ln -sf coreutils "$tmp/root/bin/$p"
	else
		cp -L "$f" "$tmp/root/bin/$p"
	fi
done
cp "$burn" "$tmp/root/bin/burn"
cp "$here/btrfs.sh" "$tmp/root/tests/"
ln -s bash "$tmp/root/bin/sh"
for f in "$tmp"/root/bin/*; do
	ldd "$f" 2>/dev/null | sed -n 's|.*=> \(/[^ ]*\) .*|\1|p; s|^[[:space:]]*\(/[^ ]*\) .*|\1|p'
done | sort -u | while read -r lib; do
	mkdir -p "$tmp/root${lib%/*}"
	cp -L "$lib" "$tmp/root$lib"
done
cat >"$tmp/root/init" <<'INIT'
#!/bin/bash
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
if mount -t btrfs /dev/vda /mnt; then
	TMPDIR=/tmp sh /tests/btrfs.sh /bin/burn /mnt
fi
echo "qemu-btrfs: exit $?"
INIT
chmod 755 "$tmp/root/init"
(cd "$tmp/root" && find . | cpio -o -H newc --quiet) >"$tmp/initramfs.cpio"
truncate -s 256M "$tmp/disk.img"
mkfs.btrfs -q -f "$tmp/disk.img" >/dev/null
# no reboot: when init exits, the kernel panics and qemu stops
timeout 900 qemu-system-x86_64 -cpu max -m 512 -nographic -no-reboot -kernel "$kernel" \
	-initrd "$tmp/initramfs.cpio" -drive "file=$tmp/disk.img,format=raw,if=virtio" \
	-append "console=ttyS0 panic=-1 quiet" >"$tmp/console.log" 2>&1 || true
if grep -q 'qemu-btrfs: exit 0' "$tmp/console.log"; then
	echo "qemu-btrfs: OK"
else
	cat "$tmp/console.log"
	echo "qemu-btrfs: FAIL"
	exit 1
fi
