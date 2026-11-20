#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Disposable, read-only qualification of the actual production BOOT application
# reading a real EFI-stub kernel and initramfs using Common V2 UFS2.so.
set -euo pipefail
export LC_ALL=C
boot=$(realpath "${1:?BOOTX64.EFI required}")
module=$(realpath "${2:?installed UFS2.so required}")
kernel=$(realpath "${3:?EFI stub required}")
fixture=$(realpath "${4:?native UFS2 fixture creator required}")
work=$(realpath -m "${5:?disposable output directory required}")
mode=${6:-clean}
case "$mode" in clean|bad-superblock) ;; *) exit 2 ;; esac
mkdir -p "$work/source" "$work/ram"/{bin,dev,proc}
esp="$work/esp.img"; media="$work/ufs2.img"; disk="$work/gpt.img"
cp "$kernel" "$work/source/native.img"
cp "$(command -v busybox)" "$work/ram/bin/busybox"
ln -sf busybox "$work/ram/bin/sh"
cat > "$work/ram/init" <<'GUEST'
#!/bin/sh
set -eu
b=/bin/busybox
$b mount -t devtmpfs devtmpfs /dev
exec </dev/ttyS0 >/dev/ttyS0 2>&1
trap '/bin/busybox poweroff -f' EXIT
$b mount -t proc proc /proc
$b grep -qw ufs2_so_trial /proc/cmdline
echo UFS2_COMMON_V2_LINUX_HANDOFF_PASS
GUEST
chmod +x "$work/ram/init"
(cd "$work/ram" && find . -print0 | cpio --null -o -H newc --owner=0:0 --quiet | gzip -1) > "$work/source/native.initrd"
# Independent on-media writer: 64 KiB blocks, 8 KiB fragments, real native
# BSD dirents, inode allocation and single-indirect large kernel block map.
# Sorted file names guarantee /file000=kernel and /file001=initramfs.
"$fixture" -t ffs -B little -s 128m -o version=2 "$media" "$work/source"
if [[ "$mode" == bad-superblock ]]; then
    printf '\000\000\000\000' |
       dd of="$media" bs=1 seek=66908 conv=notrunc status=none
fi
sha256sum "$media" > "$work/source.sha256"
truncate -s 64M "$esp"
mkfs.fat -F 32 "$esp"
mmd -i "$esp" ::/EFI ::/EFI/BOOT ::/EFI/BOOT/drivers_x64
mcopy -i "$esp" "$boot" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$esp" "$module" ::/EFI/BOOT/drivers_x64/UFS2.so
truncate -s 220M "$disk"
sgdisk --clear --new=1:2048:+64M --typecode=1:ef00 \
      --new=2:133120:+128M --typecode=2:8300 "$disk"
partuuid=$(sgdisk --info=2 "$disk" | awk '/Partition unique GUID: / { print $4 }')
[[ $partuuid =~ ^[[:xdigit:]]{8}(-[[:xdigit:]]{4}){3}-[[:xdigit:]]{12}$ ]]
cat > "$work/boot.conf" <<BOOT
timeout 1
textonly true
use_nvram false
driver_diagnostics true
scanfor manual
menuentry "InfiltratorOS kernel through UFS2.so" {
    volume $partuuid
    filesystem_module ufs2
    loader \file000
    initrd \file001
    linux_load memory
    ostype Linux
    options "console=ttyS0 rdinit=/init panic=1 ufs2_so_trial"
}
BOOT
mcopy -i "$esp" "$work/boot.conf" ::/EFI/BOOT/
dd if="$esp" of="$disk" bs=1M seek=1 conv=notrunc status=none
dd if="$media" of="$disk" bs=1M seek=65 conv=notrunc status=none
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$work/vars.fd"
qemu=(qemu-system-x86_64 -machine q35,accel=tcg -cpu max -m 768
  -display none -monitor none -net none -no-reboot
  -serial "file:$work/serial.log"
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd
  -drive "if=pflash,format=raw,file=$work/vars.fd"
  -drive "file=$disk,format=raw,if=none,id=bootdisk"
  -device nvme,drive=bootdisk,serial=ufs2-v2-qualification)
if [[ "$mode" == bad-superblock ]]; then
    result=0
    timeout 35 "${qemu[@]}" || result=$?
    [[ $result == 0 || $result == 124 ]]
    grep -Fq 'validated UFS2.so' "$work/serial.log"
    if grep -Fq UFS2_COMMON_V2_LINUX_HANDOFF_PASS "$work/serial.log"; then
        echo "BOOT executed a kernel from corrupted UFS2 superblock" >&2
        exit 1
    fi
    dd if="$disk" bs=1M skip=65 count=128 status=none | cmp - "$media"
    (cd "$work" && sha256sum -c source.sha256)
    echo UFS2_COMMON_V2_CORRUPT_SUPERBLOCK_REFUSED
    exit 0
fi
if ! timeout 180 "${qemu[@]}" ||
   ! grep -Fq UFS2_COMMON_V2_LINUX_HANDOFF_PASS "$work/serial.log"; then
    tail -n 100 "$work/serial.log" >&2
    exit 1
fi
grep -Fq 'validated UFS2.so' "$work/serial.log"
grep -Fq "close /file000: $(stat -c %s "$kernel") bytes read through module" \
    "$work/serial.log"
grep -Fq "close /file001: $(stat -c %s "$work/source/native.initrd") bytes read through module" \
    "$work/serial.log"
dd if="$disk" bs=1M skip=65 count=128 status=none | cmp - "$media"
(cd "$work" && sha256sum -c source.sha256)
echo UFS2_COMMON_V2_LINUX_HANDOFF_PASS
