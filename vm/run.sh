#!/bin/sh
# Build the module + static init, assemble an initramfs, and boot the exact
# host kernel in QEMU to run the n_tty_info test matrix. Prints PASS/FAIL.
#
# Needs: linux-headers for the running kernel, gcc, qemu-system-x86_64, cpio.
# Uses /dev/kvm if writable, else falls back to TCG (slower).
set -e
cd "$(dirname "$0")"
KREL=$(uname -r)

echo ">> building module"
mkdir -p root/dev root/proc root/sys	# initramfs mountpoints (not tracked in git)
make -C .. module >/dev/null
cp ../n_tty_info.ko root/n_tty_info.ko

echo ">> building static init"
gcc -O2 -static -o init init.c
cp init root/init

if [ ! -f vmlinuz ]; then
	echo ">> fetching matching kernel image ($KREL)"
	tmp=$(mktemp -d)
	( cd "$tmp" && apt-get download "linux-image-unsigned-$KREL" )
	dpkg-deb -x "$tmp"/linux-image-unsigned-*.deb "$tmp/x"
	cp "$tmp/x/boot/vmlinuz-$KREL" vmlinuz
	rm -rf "$tmp"
fi

echo ">> packing initramfs"
( cd root && find . | cpio -o -H newc 2>/dev/null | gzip -9 > ../initramfs.cpio.gz )

KVM=""
[ -w /dev/kvm ] && KVM="-enable-kvm -cpu host"

echo ">> booting (timeout 120s)"
timeout 120 qemu-system-x86_64 $KVM -m 512 \
	-kernel vmlinuz -initrd initramfs.cpio.gz \
	-append "console=ttyS0 panic=-1 rdinit=/init ignore_loglevel" \
	-nographic -no-reboot 2>&1 | tee boot.log | \
	grep -aE '\[PASS\]|\[FAIL\]|wchan line|RESULT' || true

echo ">> done (full console in vm/boot.log)"
